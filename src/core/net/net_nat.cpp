// NetBackend: NAT table management, IP rewriting, TCP/UDP data relay.

#include "core/net/net_compat.h"
#include "core/net/net_backend.h"
#include "core/net/net_packet.h"
#include "core/net/frame_builder.h"

extern "C" {
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/ip4.h"
}

#include <cstring>
#include <algorithm>


static uint64_t GetMonotonicMs() {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 +
           static_cast<uint64_t>(ts.tv_nsec) / 1000000;
#endif
}

// ============================================================
// NAT table lookup / creation
// ============================================================

NetBackend::NatEntry* NetBackend::FindNatEntry(
    uint32_t guest_port, uint32_t dst_ip, uint16_t dst_port, uint8_t proto) {
    uint64_t now = GetMonotonicMs();
    for (auto& e : nat_entries_) {
        if (e->state == NatState::Closed) continue;
        // TCP entry in HalfClosed with all resources released:
        // still findable during TIME_WAIT so close handshake completes,
        // then treated as dead so new connections can be created.
        if (e->proto == IPPROTO_TCP &&
            e->state == NatState::HalfClosed &&
            e->host_socket == static_cast<uintptr_t>(SOCK_INVALID) &&
            !e->conn_pcb && !e->listen_pcb &&
            (now - e->last_active_ms) > 15000) {
            continue;
        }
        if (e->proto == proto &&
            e->guest_port == guest_port &&
            e->real_dst_ip == dst_ip &&
            e->real_dst_port == dst_port)
            return e.get();
    }
    return nullptr;
}

NetBackend::NatEntry* NetBackend::CreateNatEntry(
    uint32_t guest_ip, uint16_t guest_port,
    uint32_t dst_ip, uint16_t dst_port, uint8_t proto) {

    auto entry = std::make_unique<NatEntry>();
    entry->backend = this;
    entry->proto = proto;
    entry->guest_ip = guest_ip;
    entry->guest_port = guest_port;
    entry->real_dst_ip = dst_ip;
    entry->real_dst_port = dst_port;
    entry->proxy_port = AllocProxyPort();
    entry->last_active_ms = GetMonotonicMs();

    if (proto == IPPROTO_TCP) {
        entry->state = NatState::Listening;

        struct tcp_pcb* pcb = tcp_new();
        ip_addr_t bind_ip;
        IP4_ADDR(ip_2_ip4(&bind_ip), 10, 0, 2, 2);
        tcp_bind(pcb, &bind_ip, entry->proxy_port);

        struct tcp_pcb* listen_pcb = tcp_listen(pcb);
        if (!listen_pcb) {
            tcp_close(pcb);
            return nullptr;
        }
        entry->listen_pcb = listen_pcb;
        tcp_arg(listen_pcb, entry.get());
        tcp_accept(listen_pcb, [](void* arg, struct tcp_pcb* new_pcb, err_t err) -> err_t {
            if (err != ERR_OK || !new_pcb) return ERR_VAL;
            auto* self_entry = static_cast<NatEntry*>(arg);
            self_entry->backend->OnTcpAccepted(self_entry, new_pcb);
            return ERR_OK;
        });
    } else {
        entry->state = NatState::Established;
        SocketHandle s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s == SOCK_INVALID) return nullptr;
        SOCK_SETNONBLOCK(s);
        entry->host_socket = static_cast<uintptr_t>(s);
    }

    nat_entries_.push_back(std::move(entry));
    auto* ptr = nat_entries_.back().get();
    UpdateNatPoll(ptr);
    return ptr;
}

uint16_t NetBackend::AllocProxyPort() {
    const uint16_t kMinPort = 10000;
    const uint16_t kMaxPort = 60000;
    uint16_t attempts = 0;
    while (attempts < (kMaxPort - kMinPort)) {
        uint16_t port = next_proxy_port_++;
        if (next_proxy_port_ > kMaxPort) next_proxy_port_ = kMinPort;
        if (!IsProxyPortInUse(port))
            return port;
        attempts++;
    }
    return next_proxy_port_++;
}

bool NetBackend::IsProxyPortInUse(uint16_t port) const {
    for (auto& e : nat_entries_) {
        if (e->proxy_port == port && e->state != NatState::Closed)
            return true;
    }
    return false;
}

void NetBackend::RemoveNatEntry(NatEntry* entry) {
    CloseHostSocket(entry);
    if (entry->listen_pcb) {
        tcp_close(static_cast<struct tcp_pcb*>(entry->listen_pcb));
        entry->listen_pcb = nullptr;
    }
    if (entry->conn_pcb) {
        if (entry->proto == IPPROTO_TCP) {
            auto* pcb = static_cast<struct tcp_pcb*>(entry->conn_pcb);
            tcp_arg(pcb, nullptr);
            tcp_recv(pcb, nullptr);
            tcp_err(pcb, nullptr);
            tcp_abort(pcb);
        } else {
            udp_remove(static_cast<struct udp_pcb*>(entry->conn_pcb));
        }
        entry->conn_pcb = nullptr;
    }

    entry->state = NatState::Closed;
    if (entry->poll.inited() && !entry->poll.closing()) {
        entry->poll.Close();
    } else if (!entry->poll.inited() || entry->poll.closed()) {
        nat_entries_.erase(
            std::remove_if(nat_entries_.begin(), nat_entries_.end(),
                            [entry](auto& e) { return e.get() == entry; }),
            nat_entries_.end());
    }
}

void NetBackend::CleanupStaleEntries() {
    uint64_t now = GetMonotonicMs();
    constexpr uint64_t kClosedEntryTimeoutMs  = 5000;
    constexpr uint64_t kUdpIdleTimeoutMs      = 120000;
    constexpr uint64_t kTcpDeadTimeoutMs      = 30000;
    constexpr uint64_t kTcpIdleTimeoutMs      = 300000;

    // Use index-based iteration because tcp_abort / tcp_close may
    // synchronously fire lwIP callbacks that could re-enter and mutate
    // nat_entries_ (e.g. via RemoveNatEntry), invalidating iterators.
    size_t i = 0;
    while (i < nat_entries_.size()) {
        auto& e = nat_entries_[i];
        bool remove = false;
        uint64_t age = now - e->last_active_ms;

        if (e->state == NatState::Closed) {
            remove = age > kClosedEntryTimeoutMs;
        } else if (e->proto == IPPROTO_UDP) {
            remove = age > kUdpIdleTimeoutMs;
        } else if (e->proto == IPPROTO_TCP) {
            bool all_released = (e->host_socket == static_cast<uintptr_t>(SOCK_INVALID) &&
                                 !e->conn_pcb && !e->listen_pcb);
            if (all_released)
                remove = age > kTcpDeadTimeoutMs;
            else
                remove = age > kTcpIdleTimeoutMs;
        }

        if (remove) {
            CloseHostSocket(e.get());
            if (e->listen_pcb) {
                tcp_close(static_cast<struct tcp_pcb*>(e->listen_pcb));
                e->listen_pcb = nullptr;
            }
            if (e->conn_pcb) {
                void* pcb = e->conn_pcb;
                if (e->proto == IPPROTO_TCP) {
                    // Detach callbacks before aborting so the synchronous
                    // tcp_err firing inside tcp_abort does not re-enter us.
                    auto* tcp_pcb_p = static_cast<struct tcp_pcb*>(pcb);
                    tcp_arg(tcp_pcb_p, nullptr);
                    tcp_recv(tcp_pcb_p, nullptr);
                    tcp_err(tcp_pcb_p, nullptr);
                    tcp_abort(tcp_pcb_p);
                }
                e->conn_pcb = nullptr;
            }
            if (e->poll.inited() && !e->poll.closing()) {
                e->poll.Close();
                e->state = NatState::Closed;
                ++i;
            } else if (e->poll.closing() && !e->poll.closed()) {
                e->state = NatState::Closed;
                ++i;
            } else {
                nat_entries_.erase(nat_entries_.begin() + static_cast<ptrdiff_t>(i));
            }
        } else {
            ++i;
        }
    }
}

// ============================================================
// NAT IP rewriting
// ============================================================

void NetBackend::RewriteAndFeed(uint8_t* frame, uint32_t len, NatEntry* entry) {
    auto* ip = reinterpret_cast<IpHdr*>(frame + sizeof(EthHdr));
    uint32_t ip_hdr_len = (ip->ver_ihl & 0xF) * 4;
    uint32_t old_dst_ip = ip->dst_ip;

    if (entry->proto == IPPROTO_TCP) {
        auto* tcp = reinterpret_cast<TcpHdr*>(frame + sizeof(EthHdr) + ip_hdr_len);
        uint16_t old_port = tcp->dst_port;
        uint16_t new_port = htons(entry->proxy_port);
        IncrementalCksumUpdate(&tcp->checksum,
                               old_dst_ip, htonl(kGatewayIp),
                               old_port, new_port);
        tcp->dst_port = new_port;
    } else if (entry->proto == IPPROTO_UDP) {
        auto* udp = reinterpret_cast<UdpHdr*>(frame + sizeof(EthHdr) + ip_hdr_len);
        uint16_t old_port = udp->dst_port;
        uint16_t new_port = htons(entry->proxy_port);
        if (udp->checksum != 0) {
            IncrementalCksumUpdate(&udp->checksum,
                                   old_dst_ip, htonl(kGatewayIp),
                                   old_port, new_port);
        }
        udp->dst_port = new_port;
    }

    ip->dst_ip = htonl(kGatewayIp);
    RecalcIpChecksum(ip);

    struct pbuf* p = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(len), PBUF_RAM);
    if (p) {
        pbuf_take(p, frame, static_cast<u16_t>(len));
        auto* nif = static_cast<struct netif*>(netif_);
        if (nif->input(p, nif) != ERR_OK) pbuf_free(p);
    }
}

void NetBackend::ReverseRewrite(uint8_t* frame, uint32_t len) {
    if (len < sizeof(EthHdr) + sizeof(IpHdr)) return;
    auto* eth = reinterpret_cast<EthHdr*>(frame);
    if (ntohs(eth->type) != 0x0800) return;

    auto* ip = reinterpret_cast<IpHdr*>(frame + sizeof(EthHdr));
    uint32_t ip_hdr_len = (ip->ver_ihl & 0xF) * 4;
    uint32_t src_ip_host = ntohl(ip->src_ip);

    if (src_ip_host != kGatewayIp) return;

    uint16_t src_port = 0;
    if (ip->proto == IPPROTO_TCP && len >= sizeof(EthHdr) + ip_hdr_len + sizeof(TcpHdr)) {
        src_port = ntohs(reinterpret_cast<TcpHdr*>(frame + sizeof(EthHdr) + ip_hdr_len)->src_port);
    } else if (ip->proto == IPPROTO_UDP && len >= sizeof(EthHdr) + ip_hdr_len + sizeof(UdpHdr)) {
        src_port = ntohs(reinterpret_cast<UdpHdr*>(frame + sizeof(EthHdr) + ip_hdr_len)->src_port);
    } else {
        return;
    }

    NatEntry* entry = nullptr;
    for (auto& e : nat_entries_) {
        if (e->state == NatState::Closed) continue;
        if (e->proxy_port == src_port && e->proto == ip->proto) {
            entry = e.get();
            break;
        }
    }
    if (!entry) return;

    // gateway_local: keep src_ip as 10.0.2.2, only rewrite port back
    uint32_t new_src_ip = entry->gateway_local ? ip->src_ip : htonl(entry->real_dst_ip);
    uint16_t new_src_port = htons(entry->real_dst_port);

    if (ip->proto == IPPROTO_TCP) {
        auto* tcp = reinterpret_cast<TcpHdr*>(frame + sizeof(EthHdr) + ip_hdr_len);
        IncrementalCksumUpdate(&tcp->checksum,
                               ip->src_ip, new_src_ip,
                               tcp->src_port, new_src_port);
        tcp->src_port = new_src_port;
    } else if (ip->proto == IPPROTO_UDP) {
        auto* udp = reinterpret_cast<UdpHdr*>(frame + sizeof(EthHdr) + ip_hdr_len);
        if (udp->checksum != 0) {
            IncrementalCksumUpdate(&udp->checksum,
                                   ip->src_ip, new_src_ip,
                                   udp->src_port, new_src_port);
        }
        udp->src_port = new_src_port;
    }

    if (!entry->gateway_local)
        ip->src_ip = new_src_ip;
    RecalcIpChecksum(ip);
}

// ============================================================
// TCP NAT callbacks
// ============================================================

void NetBackend::OnTcpAccepted(NatEntry* entry, void* new_pcb_v) {
    auto* new_pcb = static_cast<struct tcp_pcb*>(new_pcb_v);
    entry->conn_pcb = new_pcb;

    // Defer closing the listener — lwIP accesses pcb->listener AFTER
    // the accept callback returns (tcp_backlog_accepted).
    if (entry->listen_pcb) {
        deferred_listen_close_.push_back(entry->listen_pcb);
        entry->listen_pcb = nullptr;
    }

    tcp_arg(new_pcb, entry);
    tcp_recv(new_pcb, [](void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err) -> err_t {
        auto* e = static_cast<NatEntry*>(arg);
        e->backend->OnTcpRecv(e, pcb, p);
        return ERR_OK;
    });
    tcp_err(new_pcb, [](void* arg, err_t err) {
        auto* e = static_cast<NatEntry*>(arg);
        e->backend->OnTcpErr(e);
    });

    // Create host socket and connect to real destination.
    // Do NOT call RemoveNatEntry / tcp_abort here — we are inside the
    // accept callback and lwIP continues using the PCB afterward.
    SocketHandle s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_INVALID) {
        DetachAndCloseLwipPcb(entry);
        entry->state = NatState::Closed;
        return;
    }
    SOCK_SETNONBLOCK(s);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(entry->real_dst_ip);
    addr.sin_port = htons(entry->real_dst_port);

    int ret = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    int connect_err = SOCK_ERRNO;
    if (ret == SOCK_ERR && connect_err != SOCK_WOULDBLOCK && connect_err != SOCK_INPROGRESS) {
        SOCK_CLOSE(s);
        DetachAndCloseLwipPcb(entry);
        entry->state = NatState::Closed;
        return;
    }

    entry->host_socket = static_cast<uintptr_t>(s);
    entry->state = (ret == SOCK_ERR) ? NatState::Connecting : NatState::Established;
    UpdateNatPoll(entry);
}

void NetBackend::OnTcpRecv(NatEntry* entry, void* pcb_v, void* p_v) {
    auto* pcb = static_cast<struct tcp_pcb*>(pcb_v);
    auto* p = static_cast<struct pbuf*>(p_v);

    if (!p) {
        // Guest sent FIN
        entry->poll.Stop();
        CloseHostSocket(entry);
        tcp_arg(pcb, nullptr);
        tcp_recv(pcb, nullptr);
        tcp_err(pcb, nullptr);
        tcp_close(pcb);
        entry->conn_pcb = nullptr;
        entry->state = NatState::HalfClosed;
        entry->last_active_ms = GetMonotonicMs();
        return;
    }

    std::vector<uint8_t> data(p->tot_len);
    pbuf_copy_partial(p, data.data(), p->tot_len, 0);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    entry->last_active_ms = GetMonotonicMs();

    if (entry->state == NatState::Connecting) {
        entry->pending_data.insert(entry->pending_data.end(),
                                    data.begin(), data.end());
        return;
    }

    entry->pending_to_host.insert(entry->pending_to_host.end(),
                                   data.begin(), data.end());
    DrainTcpToHost(entry);
    UpdateNatPoll(entry);
}

void NetBackend::OnTcpErr(NatEntry* entry) {
    entry->conn_pcb = nullptr; // lwIP already freed the PCB
    entry->poll.Stop();
    CloseHostSocket(entry);
    entry->state = NatState::Closed;
}

// ============================================================
// UDP NAT callback
// ============================================================

void NetBackend::OnUdpRecv(NatEntry* entry, void* pcb_v, void* p_v) {
    auto* p = static_cast<struct pbuf*>(p_v);
    if (!p) return;

    std::vector<uint8_t> data(p->tot_len);
    pbuf_copy_partial(p, data.data(), p->tot_len, 0);
    pbuf_free(p);

    if (entry->host_socket != static_cast<uintptr_t>(SOCK_INVALID)) {
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(entry->real_dst_ip);
        addr.sin_port = htons(entry->real_dst_port);
        sendto(static_cast<SocketHandle>(entry->host_socket),
               SOCK_CCAST(data.data()),
               static_cast<int>(data.size()), 0,
               reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    }
}

// ============================================================
// Socket I/O event handlers
// ============================================================

void NetBackend::OnNatPollEvent(NatEntry* entry, int status, int events) {
    if (status < 0) {
        TeardownNatEntry(entry);
        return;
    }

    SocketHandle s = static_cast<SocketHandle>(entry->host_socket);
    if (s == SOCK_INVALID) return;

    if (entry->state == NatState::Connecting && (events & UV_WRITABLE)) {
        int sock_err = 0;
        socklen_t optlen = sizeof(sock_err);
        getsockopt(s, SOL_SOCKET, SO_ERROR,
                   SOCK_CAST(&sock_err), &optlen);
        if (sock_err != 0) {
            SOCK_CLOSE(s);
            entry->host_socket = static_cast<uintptr_t>(SOCK_INVALID);
            entry->state = NatState::Closed;
            entry->poll.Stop();
            return;
        }
        entry->state = NatState::Established;
        entry->last_active_ms = GetMonotonicMs();
        if (!entry->pending_data.empty()) {
            entry->pending_to_host.insert(entry->pending_to_host.end(),
                                          entry->pending_data.begin(),
                                          entry->pending_data.end());
            entry->pending_data.clear();
            DrainTcpToHost(entry);
        }
        UpdateNatPoll(entry);
        return;
    }

    if (entry->state != NatState::Connecting && entry->proto == IPPROTO_TCP &&
        !entry->pending_to_host.empty() && (events & UV_WRITABLE)) {
        DrainTcpToHost(entry);
    }

    if (entry->state != NatState::Connecting && (events & UV_READABLE)) {
        if (entry->proto == IPPROTO_TCP) {
            DrainTcpToGuest(entry);
            if (entry->pending_to_guest.empty())
                HandleTcpReadable(entry);
        } else {
            HandleUdpReadable(entry);
        }
    }

    if (entry->host_socket != static_cast<uintptr_t>(SOCK_INVALID))
        UpdateNatPoll(entry);
    else
        entry->poll.Stop();

    RescheduleLwipTimer();
}

// ============================================================
// TCP data relay
// ============================================================

void NetBackend::DrainTcpToGuest(NatEntry* entry) {
    if (entry->pending_to_guest.empty() || !entry->conn_pcb) return;

    auto* pcb = static_cast<struct tcp_pcb*>(entry->conn_pcb);
    uint16_t avail = tcp_sndbuf(pcb);
    uint16_t to_write = static_cast<uint16_t>(
        std::min<size_t>(avail, entry->pending_to_guest.size()));
    if (to_write > 0) {
        tcp_write(pcb, entry->pending_to_guest.data(), to_write, TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
        entry->pending_to_guest.erase(
            entry->pending_to_guest.begin(),
            entry->pending_to_guest.begin() + to_write);
    }
}

void NetBackend::DrainTcpToHost(NatEntry* entry) {
    if (entry->pending_to_host.empty()) return;
    if (entry->host_socket == static_cast<uintptr_t>(SOCK_INVALID)) {
        entry->pending_to_host.clear();
        return;
    }

    SocketHandle s = static_cast<SocketHandle>(entry->host_socket);
    int sent = send(s, SOCK_CCAST(entry->pending_to_host.data()),
                    static_cast<int>(entry->pending_to_host.size()), 0);

    if (sent > 0) {
        entry->pending_to_host.erase(
            entry->pending_to_host.begin(),
            entry->pending_to_host.begin() + sent);
    } else if (sent == SOCK_ERR) {
        int err = SOCK_ERRNO;
        if (err != SOCK_WOULDBLOCK) {
            TeardownNatEntry(entry);
        }
    }
}

void NetBackend::HandleTcpReadable(NatEntry* entry) {
    char buf[32768];
    SocketHandle s = static_cast<SocketHandle>(entry->host_socket);
    int n = recv(s, buf, sizeof(buf), 0);

    if (n <= 0) {
        DrainTcpToGuest(entry);
        DetachAndCloseLwipPcb(entry);
        entry->poll.Stop();
        SOCK_CLOSE(s);
        entry->host_socket = static_cast<uintptr_t>(SOCK_INVALID);
        entry->state = NatState::HalfClosed;
        entry->last_active_ms = GetMonotonicMs();
        return;
    }

    entry->last_active_ms = GetMonotonicMs();
    entry->pending_to_guest.insert(entry->pending_to_guest.end(), buf, buf + n);
    DrainTcpToGuest(entry);
}

void NetBackend::HandleUdpReadable(NatEntry* entry) {
    char buf[2048];
    SocketHandle s = static_cast<SocketHandle>(entry->host_socket);
    struct sockaddr_in from{};
    socklen_t fromlen = sizeof(from);
    int n = recvfrom(s, buf, sizeof(buf), 0,
                     reinterpret_cast<sockaddr*>(&from), &fromlen);
    if (n <= 0) return;

    entry->last_active_ms = GetMonotonicMs();

    uint32_t reply_src_ip = entry->gateway_local ? kGatewayIp : entry->real_dst_ip;
    auto frame = frame::BuildUdpFrame(
        kGuestMac, kGatewayMac,
        reply_src_ip, entry->guest_ip,
        entry->real_dst_port, entry->guest_port,
        buf, static_cast<uint32_t>(n));

    InjectFrame(frame.data(), static_cast<uint32_t>(frame.size()));
}
