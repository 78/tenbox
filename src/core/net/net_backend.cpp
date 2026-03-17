// NetBackend: lifecycle, lwIP init, libuv event loop, frame dispatch.

#include "core/net/net_compat.h"
#include "core/net/net_backend.h"
#include "core/net/net_packet.h"
#include "core/device/virtio/virtio_net.h"
#include "core/vmm/types.h"

extern "C" {
#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
}

#include <uv.h>
#include <cstring>
#include <memory>
#include <algorithm>

#ifdef _WIN32
#include <icmpapi.h>
#endif


// ============================================================
// Monotonic time (milliseconds)
// ============================================================

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

extern "C" u32_t sys_now(void) {
    return static_cast<u32_t>(GetMonotonicMs());
}

// ============================================================
// lwIP netif callbacks
// ============================================================

static err_t LwipNetifInit(struct netif* nif) {
    nif->name[0] = 't';
    nif->name[1] = 'c';
    nif->mtu = 1500;
    nif->hwaddr_len = 6;
    memcpy(nif->hwaddr, NetBackend::kGatewayMac, 6);
    nif->output = etharp_output;
    nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                 NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    return ERR_OK;
}

static err_t LwipLinkOutput(struct netif* nif, struct pbuf* p) {
    auto* backend = static_cast<NetBackend*>(nif->state);
    std::vector<uint8_t> buf(p->tot_len);
    pbuf_copy_partial(p, buf.data(), p->tot_len, 0);
    backend->ReverseRewrite(buf.data(), static_cast<uint32_t>(buf.size()));
    backend->InjectFrame(buf.data(), static_cast<uint32_t>(buf.size()));
    return ERR_OK;
}

// ============================================================
// Lifecycle
// ============================================================

NetBackend::NetBackend() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

NetBackend::~NetBackend() {
    Stop();
#ifdef _WIN32
    WSACleanup();
#endif
}

bool NetBackend::Start(VirtioNetDevice* dev,
                       std::function<void()> irq_cb,
                       const std::vector<PortForward>& forwards,
                       const std::vector<GuestForward>& guest_forwards) {
    virtio_net_ = dev;
    irq_callback_ = std::move(irq_cb);
    for (const auto& f : forwards) {
        port_forwards_.emplace_back();
        auto& pf = port_forwards_.back();
        pf.backend = this;
        pf.host_port = f.host_port;
        pf.guest_port = f.guest_port;
        pf.host_ip = f.host_ip;
    }
    ApplyGuestForwards(guest_forwards);
    if (!guest_forwards_.empty()) {
        LOG_INFO("Guest forwards initialized (%zu entries)", guest_forwards_.size());
        for (const auto& gf : guest_forwards_) {
            LOG_INFO("  guestfwd: %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u",
                     (gf.guest_ip >> 24) & 0xFF, (gf.guest_ip >> 16) & 0xFF,
                     (gf.guest_ip >> 8) & 0xFF, gf.guest_ip & 0xFF, gf.guest_port,
                     (gf.host_ip >> 24) & 0xFF, (gf.host_ip >> 16) & 0xFF,
                     (gf.host_ip >> 8) & 0xFF, gf.host_ip & 0xFF, gf.host_port);
        }
    }

    net_thread_ = std::thread(&NetBackend::NetworkThread, this);

    std::unique_lock<std::mutex> lock(loop_ready_mutex_);
    loop_ready_cv_.wait(lock, [this] { return loop_ready_; });
    running_ = true;
    return true;
}

void NetBackend::Stop() {
    if (!running_) return;
    running_ = false;
#ifdef _WIN32
    if (icmp_running_) {
        icmp_running_ = false;
        icmp_cv_.notify_all();
        if (icmp_thread_.joinable())
            icmp_thread_.join();
    }
#endif
    uv_async_send(&stop_wakeup_);
    if (net_thread_.joinable()) net_thread_.join();
#ifdef _WIN32
    if (icmp_handle_) {
        IcmpCloseHandle(icmp_handle_);
        icmp_handle_ = nullptr;
    }
#endif
}

void NetBackend::SetLinkUp(bool up) {
    link_up_ = up;
}

void NetBackend::UpdatePortForwards(const std::vector<PortForward>& forwards,
                                     PortForwardCallback cb) {
    {
        std::lock_guard<std::mutex> lock(pf_update_mutex_);
        pending_pf_update_ = forwards;
        pf_update_cb_ = std::move(cb);
    }
    if (running_) uv_async_send(&pf_update_wakeup_);
}

void NetBackend::UpdateGuestForwards(const std::vector<GuestForward>& guest_forwards) {
    {
        std::lock_guard<std::mutex> lock(gf_update_mutex_);
        pending_gf_update_ = guest_forwards;
    }
    if (running_) uv_async_send(&gf_update_wakeup_);
}

void NetBackend::EnqueueTx(const uint8_t* frame, uint32_t len) {
    if (!link_up_) return;
    {
        std::lock_guard<std::mutex> lock(tx_mutex_);
        tx_queue_.emplace_back(frame, frame + len);
    }
    if (running_) uv_async_send(&tx_wakeup_);
}

void NetBackend::InjectFrame(const uint8_t* frame, uint32_t len) {
    if (!link_up_) return;
    virtio_net_->InjectRx(frame, len);
}

// ============================================================
// Consolidated teardown helpers
// ============================================================

void NetBackend::CloseHostSocket(NatEntry* e) {
    if (e->host_socket != static_cast<uintptr_t>(SOCK_INVALID)) {
        SOCK_CLOSE(static_cast<SocketHandle>(e->host_socket));
        e->host_socket = static_cast<uintptr_t>(SOCK_INVALID);
    }
}

void NetBackend::DetachAndCloseLwipPcb(NatEntry* e) {
    if (e->conn_pcb) {
        auto* pcb = static_cast<struct tcp_pcb*>(e->conn_pcb);
        tcp_arg(pcb, nullptr);
        tcp_recv(pcb, nullptr);
        tcp_err(pcb, nullptr);
        tcp_close(pcb);
        e->conn_pcb = nullptr;
    }
}

void NetBackend::TeardownNatEntry(NatEntry* e) {
    e->poll.Close();
    CloseHostSocket(e);
    DetachAndCloseLwipPcb(e);
    e->pending_to_host.clear();
    e->state = NatState::Closed;
}

void NetBackend::TeardownPfConn(PfEntry::Conn& c) {
    c.poll.Close();
    if (c.host_sock != ~(uintptr_t)0) {
        SOCK_CLOSE(static_cast<SocketHandle>(c.host_sock));
        c.host_sock = ~(uintptr_t)0;
    }
    if (c.guest_pcb) {
        auto* pcb = static_cast<struct tcp_pcb*>(c.guest_pcb);
        tcp_arg(pcb, nullptr);
        tcp_recv(pcb, nullptr);
        tcp_err(pcb, nullptr);
        tcp_close(pcb);
        c.guest_pcb = nullptr;
    }
}

// ============================================================
// libuv callbacks
// ============================================================

void NetBackend::RescheduleLwipTimer() {
    u32_t sleep_ms = sys_timeouts_sleeptime();
    if (sleep_ms == SYS_TIMEOUTS_SLEEPTIME_INFINITE) sleep_ms = 1000;
    if (sleep_ms == 0) sleep_ms = 1;
    uv_timer_start(&lwip_timer_, OnLwipTimer, sleep_ms, 0);
}

void NetBackend::OnLwipTimer(uv_timer_t* handle) {
    auto* self = static_cast<NetBackend*>(handle->data);
    sys_check_timeouts();
    self->RescheduleLwipTimer();
}

void NetBackend::OnCleanupTimer(uv_timer_t* handle) {
    auto* self = static_cast<NetBackend*>(handle->data);
    self->CleanupStaleEntries();
    for (auto& pf : self->port_forwards_) {
        pf.conns.remove_if([](const PfEntry::Conn& c) {
            return c.host_sock == ~(uintptr_t)0 && c.guest_pcb == nullptr
                && c.poll.closed();
        });
    }
}

void NetBackend::OnTxReady(uv_async_t* handle) {
    auto* self = static_cast<NetBackend*>(handle->data);
    self->ProcessPendingTx();
    for (auto* pcb : self->deferred_listen_close_)
        tcp_close(static_cast<struct tcp_pcb*>(pcb));
    self->deferred_listen_close_.clear();
    self->RescheduleLwipTimer();
}

void NetBackend::OnPfUpdateReady(uv_async_t* handle) {
    auto* self = static_cast<NetBackend*>(handle->data);
    self->CheckPendingUpdates();
}

void NetBackend::OnGfUpdateReady(uv_async_t* handle) {
    auto* self = static_cast<NetBackend*>(handle->data);
    self->CheckPendingGfUpdates();
}

void NetBackend::CheckPendingGfUpdates() {
    std::optional<std::vector<GuestForward>> update;
    {
        std::lock_guard<std::mutex> lock(gf_update_mutex_);
        if (pending_gf_update_) {
            update = std::move(pending_gf_update_);
            pending_gf_update_.reset();
        }
    }
    if (update) {
        ApplyGuestForwards(*update);
        LOG_INFO("Guest forwards updated (%zu entries)", guest_forwards_.size());
    }
}

void NetBackend::ApplyGuestForwards(const std::vector<GuestForward>& gfs) {
    guest_forwards_.clear();
    for (const auto& gf : gfs) {
        GfEntry entry{};
        entry.guest_ip = gf.guest_ip;
        entry.guest_port = gf.guest_port;
        entry.host_port = gf.host_port;
        // Resolve host_addr to IP
        std::string addr = gf.EffectiveHostAddr();
        unsigned o[4] = {};
        if (sscanf(addr.c_str(), "%u.%u.%u.%u", &o[0], &o[1], &o[2], &o[3]) == 4) {
            entry.host_ip = (o[0] << 24) | (o[1] << 16) | (o[2] << 8) | o[3];
        } else {
            entry.host_ip = 0x7F000001; // fallback 127.0.0.1
        }
        guest_forwards_.push_back(entry);
    }
}

bool NetBackend::IsGuestFwdIp(uint32_t ip) const {
    for (const auto& gf : guest_forwards_) {
        if (gf.guest_ip == ip) return true;
    }
    return false;
}

const NetBackend::GfEntry* NetBackend::FindGuestFwd(uint32_t guest_ip, uint16_t guest_port) const {
    for (const auto& gf : guest_forwards_) {
        if (gf.guest_ip == guest_ip && gf.guest_port == guest_port)
            return &gf;
    }
    return nullptr;
}

bool NetBackend::HandleGuestFwdArp(const uint8_t* frame, uint32_t len) {
    if (len < sizeof(EthHdr) + 28) return false;
    auto* eth = reinterpret_cast<const EthHdr*>(frame);
    if (ntohs(eth->type) != 0x0806) return false;

    const uint8_t* arp = frame + sizeof(EthHdr);
    uint16_t op = (arp[6] << 8) | arp[7];
    if (op != 1) return false; // only handle ARP request

    uint32_t target_ip = (arp[24] << 24) | (arp[25] << 16) | (arp[26] << 8) | arp[27];
    if (!IsGuestFwdIp(target_ip)) return false;

    // Build ARP reply using gateway MAC for the guestfwd IP
    uint8_t reply[sizeof(EthHdr) + 28]{};
    auto* reth = reinterpret_cast<EthHdr*>(reply);
    memcpy(reth->dst, eth->src, 6);
    memcpy(reth->src, kGatewayMac, 6);
    reth->type = htons(0x0806);

    uint8_t* rarp = reply + sizeof(EthHdr);
    rarp[0] = 0; rarp[1] = 1;   // hardware type: Ethernet
    rarp[2] = 0x08; rarp[3] = 0; // protocol type: IPv4
    rarp[4] = 6;                  // hardware size
    rarp[5] = 4;                  // protocol size
    rarp[6] = 0; rarp[7] = 2;    // op: reply
    memcpy(rarp + 8, kGatewayMac, 6);  // sender MAC
    rarp[14] = (target_ip >> 24) & 0xFF;
    rarp[15] = (target_ip >> 16) & 0xFF;
    rarp[16] = (target_ip >> 8) & 0xFF;
    rarp[17] = target_ip & 0xFF;
    memcpy(rarp + 18, arp + 8, 6);     // target MAC = requester MAC
    memcpy(rarp + 24, arp + 14, 4);    // target IP = requester IP

    InjectFrame(reply, sizeof(reply));
    return true;
}

static void CloseWalkCb(uv_handle_t* handle, void*) {
    if (!uv_is_closing(handle)) uv_close(handle, nullptr);
}

void NetBackend::OnStopSignal(uv_async_t* handle) {
    auto* self = static_cast<NetBackend*>(handle->data);

    for (auto& e : self->nat_entries_) {
        e->poll.Close();
        self->CloseHostSocket(e.get());
        if (e->listen_pcb) { tcp_close(static_cast<struct tcp_pcb*>(e->listen_pcb)); e->listen_pcb = nullptr; }
        if (e->conn_pcb) {
            if (e->proto == IPPROTO_TCP) {
                auto* pcb = static_cast<struct tcp_pcb*>(e->conn_pcb);
                tcp_arg(pcb, nullptr);
                tcp_recv(pcb, nullptr);
                tcp_err(pcb, nullptr);
                tcp_abort(pcb);
            }
            e->conn_pcb = nullptr;
        }
    }

#ifndef _WIN32
    if (self->icmp_poll_active_) {
        uv_poll_stop(&self->icmp_poll_);
        self->icmp_poll_active_ = false;
    }
    if (self->icmp_socket_ != ~(uintptr_t)0) {
        SOCK_CLOSE(static_cast<SocketHandle>(self->icmp_socket_));
        self->icmp_socket_ = ~(uintptr_t)0;
    }
#endif

    for (auto& pf : self->port_forwards_) {
        pf.listener_poll.Close();
        if (pf.listener != ~(uintptr_t)0) {
            SOCK_CLOSE(static_cast<SocketHandle>(pf.listener));
            pf.listener = ~(uintptr_t)0;
        }
        for (auto& c : pf.conns) {
            c.poll.Close();
            if (c.host_sock != ~(uintptr_t)0) {
                SOCK_CLOSE(static_cast<SocketHandle>(c.host_sock));
                c.host_sock = ~(uintptr_t)0;
            }
        }
    }

    if (self->netif_) {
        netif_remove(static_cast<struct netif*>(self->netif_));
        delete static_cast<struct netif*>(self->netif_);
        self->netif_ = nullptr;
    }

    uv_walk(&self->loop_, CloseWalkCb, nullptr);
}

// ============================================================
// NAT poll management
// ============================================================

void NetBackend::UpdateNatPoll(NatEntry* entry) {
    if (entry->poll.closing()) return;
    SocketHandle s = static_cast<SocketHandle>(entry->host_socket);
    if (s == SOCK_INVALID) {
        entry->poll.Stop();
        return;
    }
    int events = UV_READABLE;
    if (entry->state == NatState::Connecting || !entry->pending_to_host.empty())
        events |= UV_WRITABLE;

    if (!entry->poll.active()) {
        entry->poll.Init(&loop_, s);
    }
    entry->poll.Start(events, [](uv_poll_t* h, int status, int events) {
        auto* e = static_cast<NatEntry*>(h->data);
        e->backend->OnNatPollEvent(e, status, events);
    }, entry);
}

// ============================================================
// PfConn poll management
// ============================================================

void NetBackend::UpdatePfConnPoll(PfEntry::Conn& conn) {
    if (conn.poll.closing()) return;
    SocketHandle s = static_cast<SocketHandle>(conn.host_sock);
    if (s == SOCK_INVALID || static_cast<uintptr_t>(s) == ~(uintptr_t)0) {
        conn.poll.Stop();
        return;
    }
    int events = UV_READABLE;
    if (!conn.pending_to_host.empty())
        events |= UV_WRITABLE;
    if (!conn.pending_to_guest.empty())
        events &= ~UV_READABLE;  // back-pressure

    if (!conn.poll.active()) {
        conn.poll.Init(&loop_, s);
    }
    conn.poll.Start(events, [](uv_poll_t* h, int status, int events) {
        auto* c = static_cast<PfEntry::Conn*>(h->data);
        c->backend->OnPfConnPollEvent(c, status, events);
    }, &conn);
}

// ============================================================
// Network thread
// ============================================================

void NetBackend::NetworkThread() {
    lwip_init();

    auto* nif = new netif();
    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip,   10, 0, 2, 2);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw,   10, 0, 2, 2);

    netif_add(nif, &ip, &mask, &gw, this, LwipNetifInit, ethernet_input);
    netif_set_default(nif);
    nif->linkoutput = LwipLinkOutput;
    netif_ = nif;

    ip4_addr_t guest_addr;
    IP4_ADDR(&guest_addr, 10, 0, 2, 15);
    struct eth_addr guest_eth;
    memcpy(guest_eth.addr, kGuestMac, 6);
    etharp_add_static_entry(&guest_addr, &guest_eth);

    uv_loop_init(&loop_);

    lwip_timer_.data = this;
    uv_timer_init(&loop_, &lwip_timer_);
    RescheduleLwipTimer();

    cleanup_timer_.data = this;
    uv_timer_init(&loop_, &cleanup_timer_);
    uv_timer_start(&cleanup_timer_, OnCleanupTimer, 5000, 5000);

    tx_wakeup_.data = this;
    uv_async_init(&loop_, &tx_wakeup_, OnTxReady);

    pf_update_wakeup_.data = this;
    uv_async_init(&loop_, &pf_update_wakeup_, OnPfUpdateReady);

    gf_update_wakeup_.data = this;
    uv_async_init(&loop_, &gf_update_wakeup_, OnGfUpdateReady);

    stop_wakeup_.data = this;
    uv_async_init(&loop_, &stop_wakeup_, OnStopSignal);

    {
        std::lock_guard<std::mutex> lock(loop_ready_mutex_);
        loop_ready_ = true;
    }
    loop_ready_cv_.notify_one();

    SetupPortForwards();

    LOG_INFO("Network backend started (gateway 10.0.2.2, guest 10.0.2.15)");

    uv_run(&loop_, UV_RUN_DEFAULT);

    // Drain any remaining close callbacks so every handle is fully
    // detached from the loop before we destroy the owning containers.
    while (uv_run(&loop_, UV_RUN_NOWAIT) != 0)
        ;

    nat_entries_.clear();
    for (auto& pf : port_forwards_)
        pf.conns.clear();

    uv_loop_close(&loop_);
}

// ============================================================
// TX frame dispatch
// ============================================================

void NetBackend::ProcessPendingTx() {
    std::vector<std::vector<uint8_t>> frames;
    {
        std::lock_guard<std::mutex> lock(tx_mutex_);
        frames.swap(tx_queue_);
    }

    for (auto& frame : frames) {
        if (frame.size() < sizeof(EthHdr)) continue;

        if (HandleArpOrDhcp(frame.data(), static_cast<uint32_t>(frame.size())))
            continue;

        auto* eth = reinterpret_cast<EthHdr*>(frame.data());
        uint16_t ethertype = ntohs(eth->type);

        // Intercept ARP requests for guestfwd IPs before lwIP sees them
        if (ethertype == 0x0806) {
            if (HandleGuestFwdArp(frame.data(), static_cast<uint32_t>(frame.size())))
                continue;
        }

        // Feed ARP frames to lwIP
        if (ethertype == 0x0806) {
            struct pbuf* p = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(frame.size()), PBUF_RAM);
            if (p) {
                pbuf_take(p, frame.data(), static_cast<u16_t>(frame.size()));
                auto* nif = static_cast<struct netif*>(netif_);
                if (nif->input(p, nif) != ERR_OK) pbuf_free(p);
            }
            continue;
        }

        if (ethertype != 0x0800) continue;
        if (frame.size() < sizeof(EthHdr) + sizeof(IpHdr)) continue;

        auto* ip = reinterpret_cast<IpHdr*>(frame.data() + sizeof(EthHdr));
        uint32_t dst = ntohl(ip->dst_ip);

        // Check for guestfwd match (exact IP+port). This must be tested
        // before the gateway-lwIP path so rules on kGatewayIp also work.
        bool handled_by_gf = false;
        if (IsGuestFwdIp(dst)) {
            uint32_t ip_hdr_len_gf = (ip->ver_ihl & 0xF) * 4;

            if (ip->proto == IPPROTO_TCP &&
                frame.size() >= sizeof(EthHdr) + ip_hdr_len_gf + sizeof(TcpHdr)) {
                auto* tcp = reinterpret_cast<TcpHdr*>(
                    frame.data() + sizeof(EthHdr) + ip_hdr_len_gf);
                uint16_t dport = ntohs(tcp->dst_port);

                const GfEntry* gf = FindGuestFwd(dst, dport);
                if (gf || dst != kGatewayIp) {
                    uint32_t fwd_ip = gf ? gf->host_ip : 0x7F000001;
                    uint16_t fwd_port = gf ? gf->host_port : dport;

                    auto* entry = FindNatEntry(
                        ntohs(tcp->src_port), fwd_ip, fwd_port, IPPROTO_TCP);
                    if (!entry) {
                        entry = CreateNatEntry(
                            ntohl(ip->src_ip), ntohs(tcp->src_port),
                            fwd_ip, fwd_port, IPPROTO_TCP);
                        if (!entry) continue;
                        entry->gateway_local = true;
                        entry->guestfwd_ip = dst;
                        entry->guestfwd_port = dport;
                    }
                    RewriteAndFeed(frame.data(), static_cast<uint32_t>(frame.size()), entry);
                    handled_by_gf = true;
                }
            } else if (ip->proto == IPPROTO_UDP &&
                       frame.size() >= sizeof(EthHdr) + ip_hdr_len_gf + sizeof(UdpHdr)) {
                auto* udp = reinterpret_cast<UdpHdr*>(
                    frame.data() + sizeof(EthHdr) + ip_hdr_len_gf);
                uint16_t g_sport = ntohs(udp->src_port);
                uint16_t g_dport = ntohs(udp->dst_port);

                const GfEntry* gf = FindGuestFwd(dst, g_dport);
                if (gf || dst != kGatewayIp) {
                    uint32_t fwd_ip = gf ? gf->host_ip : 0x7F000001;
                    uint16_t fwd_port = gf ? gf->host_port : g_dport;
                    uint32_t udp_off = sizeof(EthHdr) + ip_hdr_len_gf + sizeof(UdpHdr);
                    uint32_t payload_len = static_cast<uint32_t>(frame.size()) - udp_off;

                    auto* entry = FindNatEntry(g_sport, fwd_ip, fwd_port, IPPROTO_UDP);
                    if (!entry) {
                        entry = CreateNatEntry(
                            ntohl(ip->src_ip), g_sport, fwd_ip, fwd_port, IPPROTO_UDP);
                        if (!entry) continue;
                        entry->gateway_local = true;
                        entry->guestfwd_ip = dst;
                        entry->guestfwd_port = g_dport;
                    }
                    if (entry->host_socket != static_cast<uintptr_t>(SOCK_INVALID) && payload_len > 0) {
                        struct sockaddr_in dest{};
                        dest.sin_family = AF_INET;
                        dest.sin_addr.s_addr = htonl(fwd_ip);
                        dest.sin_port = htons(fwd_port);
                        sendto(static_cast<SocketHandle>(entry->host_socket),
                               SOCK_CCAST(frame.data() + udp_off),
                               static_cast<int>(payload_len), 0,
                               reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
                        entry->last_active_ms = GetMonotonicMs();
                    }
                    handled_by_gf = true;
                }
            } else if (dst != kGatewayIp) {
                handled_by_gf = true;
            }
        }
        if (handled_by_gf) continue;

        // Packets to gateway: feed to lwIP.
        // Port-forward PCBs are registered in lwIP and will match; everything
        // else gets TCP RST / ICMP port-unreachable naturally.
        if (dst == kGatewayIp) {
            struct pbuf* p = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(frame.size()), PBUF_RAM);
            if (p) {
                pbuf_take(p, frame.data(), static_cast<u16_t>(frame.size()));
                auto* nif = static_cast<struct netif*>(netif_);
                if (nif->input(p, nif) != ERR_OK) pbuf_free(p);
            }
            continue;
        }

        // External destination: NAT relay
        uint32_t ip_hdr_len = (ip->ver_ihl & 0xF) * 4;

        if (ip->proto == IPPROTO_TCP) {
            if (frame.size() < sizeof(EthHdr) + ip_hdr_len + sizeof(TcpHdr)) continue;
            auto* tcp = reinterpret_cast<TcpHdr*>(
                frame.data() + sizeof(EthHdr) + ip_hdr_len);

            auto* entry = FindNatEntry(
                ntohs(tcp->src_port), ntohl(ip->dst_ip), ntohs(tcp->dst_port), IPPROTO_TCP);
            if (!entry) {
                entry = CreateNatEntry(
                    ntohl(ip->src_ip), ntohs(tcp->src_port),
                    ntohl(ip->dst_ip), ntohs(tcp->dst_port), IPPROTO_TCP);
                if (!entry) continue;
            }
            RewriteAndFeed(frame.data(), static_cast<uint32_t>(frame.size()), entry);

        } else if (ip->proto == IPPROTO_UDP) {
            if (frame.size() < sizeof(EthHdr) + ip_hdr_len + sizeof(UdpHdr)) continue;
            auto* udp = reinterpret_cast<UdpHdr*>(
                frame.data() + sizeof(EthHdr) + ip_hdr_len);

            uint16_t g_sport = ntohs(udp->src_port);
            uint16_t g_dport = ntohs(udp->dst_port);
            uint32_t g_dip   = ntohl(ip->dst_ip);

            uint32_t udp_off = sizeof(EthHdr) + ip_hdr_len + sizeof(UdpHdr);
            uint32_t payload_len = static_cast<uint32_t>(frame.size()) - udp_off;

            auto* entry = FindNatEntry(g_sport, g_dip, g_dport, IPPROTO_UDP);
            if (!entry) {
                entry = CreateNatEntry(
                    ntohl(ip->src_ip), g_sport, g_dip, g_dport, IPPROTO_UDP);
                if (!entry) continue;
            }

            if (entry->host_socket != static_cast<uintptr_t>(SOCK_INVALID) && payload_len > 0) {
                struct sockaddr_in dest{};
                dest.sin_family = AF_INET;
                dest.sin_addr.s_addr = htonl(entry->real_dst_ip);
                dest.sin_port = htons(entry->real_dst_port);
                sendto(static_cast<SocketHandle>(entry->host_socket),
                       SOCK_CCAST(frame.data() + udp_off),
                       static_cast<int>(payload_len), 0,
                       reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
                entry->last_active_ms = GetMonotonicMs();
            }

        } else if (ip->proto == IPPROTO_ICMP) {
            uint32_t icmp_off = sizeof(EthHdr) + ip_hdr_len;
            uint32_t icmp_len = static_cast<uint32_t>(frame.size()) - icmp_off;
            if (icmp_len < 8) continue;

            HandleIcmpOut(ntohl(ip->src_ip), ntohl(ip->dst_ip),
                          frame.data() + icmp_off, icmp_len);
        }
    }
}
