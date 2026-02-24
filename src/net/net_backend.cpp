#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include "net/net_backend.h"
#include "device/virtio/virtio_net.h"
#include "vmm/types.h"

#include <winsock2.h>
#include <ws2tcpip.h>

extern "C" {
#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/ip4.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
}

#include <cstring>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

// ============================================================
// Packet header structures (host byte order helpers)
// ============================================================

#pragma pack(push, 1)
struct EthHdr {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type; // network byte order
};

struct IpHdr {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
};

struct TcpHdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
};

struct UdpHdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
};

struct DhcpMsg {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic; // 0x63825363
};
#pragma pack(pop)

// ============================================================
// Checksum utilities
// ============================================================

static uint16_t ChecksumFold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}

static uint32_t ChecksumPartial(const void* data, int len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t sum = 0;
    for (int i = 0; i + 1 < len; i += 2)
        sum += (p[i] << 8) | p[i + 1];
    if (len & 1)
        sum += p[len - 1] << 8;
    return sum;
}

static void RecalcIpChecksum(IpHdr* ip) {
    ip->checksum = 0;
    int hdr_len = (ip->ver_ihl & 0xF) * 4;
    uint32_t sum = ChecksumPartial(ip, hdr_len);
    ip->checksum = htons(ChecksumFold(sum));
}

// Incremental checksum update for changing a 32-bit value and a 16-bit value
static void IncrementalCksumUpdate(uint16_t* cksum,
                                   uint32_t old_ip, uint32_t new_ip,
                                   uint16_t old_port, uint16_t new_port) {
    uint32_t sum = static_cast<uint16_t>(~ntohs(*cksum));
    // Subtract old values, add new values (in network byte order, processed as 16-bit words)
    sum += static_cast<uint16_t>(~(ntohl(old_ip) >> 16) & 0xFFFF);
    sum += (ntohl(new_ip) >> 16) & 0xFFFF;
    sum += static_cast<uint16_t>(~(ntohl(old_ip) & 0xFFFF));
    sum += ntohl(new_ip) & 0xFFFF;
    sum += static_cast<uint16_t>(~ntohs(old_port) & 0xFFFF);
    sum += ntohs(new_port);
    *cksum = htons(ChecksumFold(sum));
}

// ============================================================
// lwIP sys_now (required for NO_SYS=1 timeout management)
// ============================================================

extern "C" u32_t sys_now(void) {
    return static_cast<u32_t>(GetTickCount64());
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
    // Linearize pbuf chain
    std::vector<uint8_t> buf(p->tot_len);
    pbuf_copy_partial(p, buf.data(), p->tot_len, 0);

    // Reverse-rewrite NAT responses before injecting to guest
    backend->ReverseRewrite(buf.data(), static_cast<uint32_t>(buf.size()));
    backend->InjectFrame(buf.data(), static_cast<uint32_t>(buf.size()));
    return ERR_OK;
}

// ============================================================
// NetBackend lifecycle
// ============================================================

NetBackend::NetBackend() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

NetBackend::~NetBackend() {
    Stop();
    WSACleanup();
}

bool NetBackend::Start(VirtioNetDevice* dev,
                       std::function<void()> irq_cb,
                       const std::vector<PortForward>& forwards) {
    virtio_net_ = dev;
    irq_callback_ = std::move(irq_cb);
    for (auto& f : forwards)
        port_forwards_.push_back({f.host_port, f.guest_port});

    running_ = true;
    net_thread_ = std::thread(&NetBackend::NetworkThread, this);
    return true;
}

void NetBackend::Stop() {
    running_ = false;
    if (net_thread_.joinable()) net_thread_.join();

    // Clean up NAT sockets
    for (auto& e : nat_entries_) {
        if (e->host_socket != INVALID_SOCKET)
            closesocket(static_cast<SOCKET>(e->host_socket));
        if (e->listen_pcb) tcp_close(static_cast<struct tcp_pcb*>(e->listen_pcb));
        if (e->conn_pcb) tcp_abort(static_cast<struct tcp_pcb*>(e->conn_pcb));
    }
    nat_entries_.clear();

    for (auto& pf : port_forwards_) {
        if (pf.listener != ~(uintptr_t)0)
            closesocket(static_cast<SOCKET>(pf.listener));
        for (auto& c : pf.conns) {
            if (c.host_sock != ~(uintptr_t)0)
                closesocket(static_cast<SOCKET>(c.host_sock));
        }
    }

    if (netif_) {
        netif_remove(static_cast<struct netif*>(netif_));
        delete static_cast<struct netif*>(netif_);
        netif_ = nullptr;
    }
}

void NetBackend::EnqueueTx(const uint8_t* frame, uint32_t len) {
    std::lock_guard<std::mutex> lock(tx_mutex_);
    tx_queue_.emplace_back(frame, frame + len);
}

// ============================================================
// Network thread
// ============================================================

void NetBackend::NetworkThread() {
    // Initialize lwIP
    lwip_init();

    auto* nif = new struct netif{};
    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip,   10, 0, 2, 2);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw,   10, 0, 2, 2);

    netif_add(nif, &ip, &mask, &gw, this, LwipNetifInit, ethernet_input);
    netif_set_default(nif);
    nif->linkoutput = LwipLinkOutput;
    netif_ = nif;

    // Pre-populate ARP cache with guest MAC
    ip4_addr_t guest_addr;
    IP4_ADDR(&guest_addr, 10, 0, 2, 15);
    struct eth_addr guest_eth;
    memcpy(guest_eth.addr, kGuestMac, 6);
    etharp_add_static_entry(&guest_addr, &guest_eth);

    SetupPortForwards();

    LOG_INFO("Network backend started (gateway 10.0.2.2, guest 10.0.2.15)");

    while (running_) {
        ProcessPendingTx();
        bool polled = PollSockets();
        PollIcmpSocket();
        PollPortForwards();
        sys_check_timeouts();
        if (!polled) Sleep(1);
    }
}

void NetBackend::ProcessPendingTx() {
    std::vector<std::vector<uint8_t>> frames;
    {
        std::lock_guard<std::mutex> lock(tx_mutex_);
        frames.swap(tx_queue_);
    }

    for (auto& frame : frames) {
        if (frame.size() < sizeof(EthHdr)) continue;

        // Handle ARP and DHCP before lwIP
        if (HandleArpOrDhcp(frame.data(), static_cast<uint32_t>(frame.size())))
            continue;

        auto* eth = reinterpret_cast<EthHdr*>(frame.data());
        uint16_t ethertype = ntohs(eth->type);

        // Feed ARP frames to lwIP so it can reply with gateway MAC
        if (ethertype == 0x0806) {
            struct pbuf* p = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(frame.size()), PBUF_RAM);
            if (p) {
                pbuf_take(p, frame.data(), static_cast<u16_t>(frame.size()));
                auto* nif = static_cast<struct netif*>(netif_);
                if (nif->input(p, nif) != ERR_OK) pbuf_free(p);
            }
            continue;
        }

        if (ethertype != 0x0800) continue; // Only IPv4 beyond this point
        if (frame.size() < sizeof(EthHdr) + sizeof(IpHdr)) continue;

        auto* ip = reinterpret_cast<IpHdr*>(frame.data() + sizeof(EthHdr));
        uint32_t dst = ntohl(ip->dst_ip);

        // Packets to the gateway itself: feed directly to lwIP (ping, etc.)
        if (dst == kGatewayIp) {
            struct pbuf* p = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(frame.size()), PBUF_RAM);
            if (p) {
                pbuf_take(p, frame.data(), static_cast<u16_t>(frame.size()));
                auto* nif = static_cast<struct netif*>(netif_);
                if (nif->input(p, nif) != ERR_OK) pbuf_free(p);
            }
            continue;
        }

        // External destination: NAT relay via Winsock
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

            // Direct relay: extract UDP payload and send via Winsock
            uint32_t udp_off = sizeof(EthHdr) + ip_hdr_len + sizeof(UdpHdr);
            uint32_t payload_len = static_cast<uint32_t>(frame.size()) - udp_off;

            auto* entry = FindNatEntry(g_sport, g_dip, g_dport, IPPROTO_UDP);
            if (!entry) {
                entry = CreateNatEntry(
                    ntohl(ip->src_ip), g_sport, g_dip, g_dport, IPPROTO_UDP);
                if (!entry) continue;
            }

            if (entry->host_socket != INVALID_SOCKET && payload_len > 0) {
                struct sockaddr_in dest{};
                dest.sin_family = AF_INET;
                dest.sin_addr.s_addr = htonl(entry->real_dst_ip);
                dest.sin_port = htons(entry->real_dst_port);
                sendto(static_cast<SOCKET>(entry->host_socket),
                       reinterpret_cast<const char*>(frame.data() + udp_off),
                       static_cast<int>(payload_len), 0,
                       reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
            }

        } else if (ip->proto == IPPROTO_ICMP) {
            // ICMP relay via raw socket
            uint32_t icmp_off = sizeof(EthHdr) + ip_hdr_len;
            uint32_t icmp_len = static_cast<uint32_t>(frame.size()) - icmp_off;
            if (icmp_len < 8) continue;

            HandleIcmpOut(ntohl(ip->src_ip), ntohl(ip->dst_ip),
                          frame.data() + icmp_off, icmp_len);
        }
    }
}

// ============================================================
// ARP proxy + DHCP server (handled before lwIP)
// ============================================================

bool NetBackend::HandleArpOrDhcp(const uint8_t* frame, uint32_t len) {
    auto* eth = reinterpret_cast<const EthHdr*>(frame);

    // ARP handling: respond to any ARP request with gateway MAC
    // (lwIP handles ARP for its own IP, but we also need proxy ARP
    //  for the case where guest ARPs for the gateway before lwIP is ready)
    // Actually, let lwIP handle all ARP by passing through.
    // We only intercept DHCP here.

    if (ntohs(eth->type) != 0x0800) return false; // Let non-IP through
    if (len < sizeof(EthHdr) + sizeof(IpHdr) + sizeof(UdpHdr)) return false;

    auto* ip = reinterpret_cast<const IpHdr*>(frame + sizeof(EthHdr));
    if (ip->proto != IPPROTO_UDP) return false;

    auto* udp = reinterpret_cast<const UdpHdr*>(
        frame + sizeof(EthHdr) + (ip->ver_ihl & 0xF) * 4);
    if (ntohs(udp->dst_port) != 67) return false; // Not DHCP

    // Parse DHCP message
    uint32_t udp_off = sizeof(EthHdr) + (ip->ver_ihl & 0xF) * 4 + sizeof(UdpHdr);
    if (len < udp_off + sizeof(DhcpMsg)) return false;
    auto* dhcp = reinterpret_cast<const DhcpMsg*>(frame + udp_off);

    if (ntohl(dhcp->magic) != 0x63825363) return false;

    // Find DHCP message type option
    uint8_t msg_type = 0;
    uint32_t req_ip = 0;
    const uint8_t* opts = frame + udp_off + sizeof(DhcpMsg);
    uint32_t opts_len = len - udp_off - sizeof(DhcpMsg);

    for (uint32_t i = 0; i < opts_len; ) {
        uint8_t opt = opts[i++];
        if (opt == 255) break; // End
        if (opt == 0) continue; // Pad
        if (i >= opts_len) break;
        uint8_t olen = opts[i++];
        if (i + olen > opts_len) break;
        if (opt == 53 && olen >= 1) msg_type = opts[i];
        if (opt == 50 && olen >= 4) memcpy(&req_ip, &opts[i], 4);
        i += olen;
    }

    if (msg_type == 1 || msg_type == 3) { // DISCOVER or REQUEST
        uint8_t reply_type = (msg_type == 1) ? 2 : 5; // OFFER or ACK
        SendDhcpReply(reply_type, dhcp->xid, dhcp->chaddr, req_ip);
        return true;
    }
    return false;
}

void NetBackend::SendDhcpReply(uint8_t type, uint32_t xid,
                                const uint8_t* chaddr, uint32_t req_ip) {
    // Build: Ethernet + IP + UDP + DHCP reply
    uint8_t pkt[600]{};
    uint32_t off = 0;

    // Ethernet header — use broadcast since client has no IP yet
    auto* eth = reinterpret_cast<EthHdr*>(pkt);
    memset(eth->dst, 0xFF, 6);
    memcpy(eth->src, kGatewayMac, 6);
    eth->type = htons(0x0800);
    off += sizeof(EthHdr);

    // IP header
    auto* ip = reinterpret_cast<IpHdr*>(pkt + off);
    ip->ver_ihl = 0x45;
    ip->ttl = 64;
    ip->proto = IPPROTO_UDP;
    ip->src_ip = htonl(kGatewayIp);
    ip->dst_ip = htonl(0xFFFFFFFF); // broadcast
    uint32_t ip_off = off;
    off += 20;

    // UDP header
    auto* udp = reinterpret_cast<UdpHdr*>(pkt + off);
    udp->src_port = htons(67);
    udp->dst_port = htons(68);
    off += sizeof(UdpHdr);

    // DHCP message
    auto* dhcp = reinterpret_cast<DhcpMsg*>(pkt + off);
    dhcp->op = 2; // reply
    dhcp->htype = 1;
    dhcp->hlen = 6;
    dhcp->xid = xid;
    dhcp->yiaddr = htonl(kGuestIp);
    dhcp->siaddr = htonl(kGatewayIp);
    memcpy(dhcp->chaddr, chaddr, 6);
    dhcp->magic = htonl(0x63825363);
    off += sizeof(DhcpMsg);

    // DHCP options
    uint8_t* opt = pkt + off;
    // Message type
    *opt++ = 53; *opt++ = 1; *opt++ = type;
    // Server identifier
    *opt++ = 54; *opt++ = 4;
    uint32_t gw_net = htonl(kGatewayIp);
    memcpy(opt, &gw_net, 4); opt += 4;
    // Lease time (1 day)
    *opt++ = 51; *opt++ = 4;
    uint32_t lease = htonl(86400);
    memcpy(opt, &lease, 4); opt += 4;
    // Subnet mask
    *opt++ = 1; *opt++ = 4;
    uint32_t mask_net = htonl(kNetmask);
    memcpy(opt, &mask_net, 4); opt += 4;
    // Router (gateway)
    *opt++ = 3; *opt++ = 4;
    memcpy(opt, &gw_net, 4); opt += 4;
    // DNS — use Google public DNS
    *opt++ = 6; *opt++ = 4;
    uint32_t dns = htonl(0x08080808); // 8.8.8.8
    memcpy(opt, &dns, 4); opt += 4;
    // End
    *opt++ = 255;
    off = static_cast<uint32_t>(opt - pkt);

    // Fill in lengths
    uint32_t udp_len = off - ip_off - 20;
    udp->length = htons(static_cast<uint16_t>(udp_len));
    ip->total_len = htons(static_cast<uint16_t>(off - sizeof(EthHdr)));
    RecalcIpChecksum(ip);
    // UDP checksum optional for IPv4, leave as 0

    InjectFrame(pkt, off);
    LOG_INFO("DHCP: sent %s to 10.0.2.15",
             type == 2 ? "OFFER" : "ACK");
}

// ============================================================
// Frame injection to guest RX queue
// ============================================================

void NetBackend::InjectFrame(const uint8_t* frame, uint32_t len) {
    if (virtio_net_ && virtio_net_->InjectRx(frame, len)) {
        if (irq_callback_) irq_callback_();
    }
}

// ============================================================
// NAT table management
// ============================================================

NetBackend::NatEntry* NetBackend::FindNatEntry(
    uint32_t guest_port, uint32_t dst_ip, uint16_t dst_port, uint8_t proto) {
    for (auto& e : nat_entries_) {
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
    entry->proto = proto;
    entry->guest_ip = guest_ip;
    entry->guest_port = guest_port;
    entry->real_dst_ip = dst_ip;
    entry->real_dst_port = dst_port;
    entry->proxy_port = AllocProxyPort();

    if (proto == IPPROTO_TCP) {
        // Create lwIP listener on proxy port for this connection
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
            // Find the NetBackend from the netif state
            // We use a thread-local approach since all lwIP runs on net thread
            extern NetBackend* g_net_backend;
            g_net_backend->OnTcpAccepted(self_entry, new_pcb);
            return ERR_OK;
        });
    } else {
        // UDP: Winsock socket only — no lwIP PCB needed
        SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s == INVALID_SOCKET) return nullptr;
        u_long nonblock = 1;
        ioctlsocket(s, FIONBIO, &nonblock);
        entry->host_socket = static_cast<uintptr_t>(s);
    }

    auto* ptr = entry.get();
    nat_entries_.push_back(std::move(entry));
    return ptr;
}

uint16_t NetBackend::AllocProxyPort() {
    uint16_t port = next_proxy_port_++;
    if (next_proxy_port_ > 60000) next_proxy_port_ = 10000;
    return port;
}

void NetBackend::RemoveNatEntry(NatEntry* entry) {
    if (entry->host_socket != INVALID_SOCKET)
        closesocket(static_cast<SOCKET>(entry->host_socket));
    if (entry->listen_pcb)
        tcp_close(static_cast<struct tcp_pcb*>(entry->listen_pcb));
    if (entry->conn_pcb) {
        if (entry->proto == IPPROTO_TCP)
            tcp_abort(static_cast<struct tcp_pcb*>(entry->conn_pcb));
        else
            udp_remove(static_cast<struct udp_pcb*>(entry->conn_pcb));
    }

    nat_entries_.erase(
        std::remove_if(nat_entries_.begin(), nat_entries_.end(),
                        [entry](auto& e) { return e.get() == entry; }),
        nat_entries_.end());
}

// ============================================================
// NAT IP rewriting
// ============================================================

void NetBackend::RewriteAndFeed(uint8_t* frame, uint32_t len, NatEntry* entry) {
    auto* ip = reinterpret_cast<IpHdr*>(frame + sizeof(EthHdr));
    uint32_t ip_hdr_len = (ip->ver_ihl & 0xF) * 4;

    uint32_t old_dst_ip = ip->dst_ip;
    uint16_t new_dst_ip_net = htonl(kGatewayIp);

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

    // Feed rewritten frame to lwIP
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

    // Find NAT entry by proxy port
    NatEntry* entry = nullptr;
    for (auto& e : nat_entries_) {
        if (e->proxy_port == src_port && e->proto == ip->proto) {
            entry = e.get();
            break;
        }
    }
    if (!entry) return;

    // Rewrite src back to real destination
    uint32_t new_src_ip = htonl(entry->real_dst_ip);
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

    ip->src_ip = new_src_ip;
    RecalcIpChecksum(ip);
}

// ============================================================
// TCP NAT callbacks
// ============================================================

// Global pointer for lwIP callbacks (single-instance, net-thread only)
NetBackend* g_net_backend = nullptr;

void NetBackend::OnTcpAccepted(NatEntry* entry, void* new_pcb_v) {
    auto* new_pcb = static_cast<struct tcp_pcb*>(new_pcb_v);
    entry->conn_pcb = new_pcb;

    // Close the listener — one connection per proxy port
    if (entry->listen_pcb) {
        tcp_close(static_cast<struct tcp_pcb*>(entry->listen_pcb));
        entry->listen_pcb = nullptr;
    }

    // Set callbacks on connected PCB
    tcp_arg(new_pcb, entry);
    tcp_recv(new_pcb, [](void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err) -> err_t {
        auto* e = static_cast<NatEntry*>(arg);
        g_net_backend->OnTcpRecv(e, pcb, p);
        return ERR_OK;
    });
    tcp_err(new_pcb, [](void* arg, err_t err) {
        auto* e = static_cast<NatEntry*>(arg);
        g_net_backend->OnTcpErr(e);
    });

    // Create host socket and connect to real destination
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
        RemoveNatEntry(entry);
        return;
    }
    u_long nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(entry->real_dst_ip);
    addr.sin_port = htons(entry->real_dst_port);

    int ret = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s);
        RemoveNatEntry(entry);
        return;
    }

    entry->host_socket = static_cast<uintptr_t>(s);
    entry->connecting = (ret == SOCKET_ERROR);
}

void NetBackend::OnTcpRecv(NatEntry* entry, void* pcb_v, void* p_v) {
    auto* pcb = static_cast<struct tcp_pcb*>(pcb_v);
    auto* p = static_cast<struct pbuf*>(p_v);

    if (!p) {
        // Connection closed by guest
        if (entry->host_socket != INVALID_SOCKET) {
            closesocket(static_cast<SOCKET>(entry->host_socket));
            entry->host_socket = INVALID_SOCKET;
        }
        tcp_close(pcb);
        entry->conn_pcb = nullptr;
        return;
    }

    // Get data from pbuf
    std::vector<uint8_t> data(p->tot_len);
    pbuf_copy_partial(p, data.data(), p->tot_len, 0);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    if (entry->connecting) {
        // Queue data until host socket is connected
        entry->pending_data.insert(entry->pending_data.end(),
                                    data.begin(), data.end());
        return;
    }

    if (entry->host_socket != INVALID_SOCKET) {
        send(static_cast<SOCKET>(entry->host_socket),
             reinterpret_cast<const char*>(data.data()),
             static_cast<int>(data.size()), 0);
    }
}

void NetBackend::OnTcpErr(NatEntry* entry) {
    entry->conn_pcb = nullptr;
    if (entry->host_socket != INVALID_SOCKET) {
        closesocket(static_cast<SOCKET>(entry->host_socket));
        entry->host_socket = INVALID_SOCKET;
    }
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

    if (entry->host_socket != INVALID_SOCKET) {
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(entry->real_dst_ip);
        addr.sin_port = htons(entry->real_dst_port);
        sendto(static_cast<SOCKET>(entry->host_socket),
               reinterpret_cast<const char*>(data.data()),
               static_cast<int>(data.size()), 0,
               reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    }
}

// ============================================================
// Winsock socket polling
// ============================================================

bool NetBackend::PollSockets() {
    if (nat_entries_.empty()) return false;

    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    int count = 0;

    for (auto& e : nat_entries_) {
        SOCKET s = static_cast<SOCKET>(e->host_socket);
        if (s == INVALID_SOCKET) continue;
        if (e->connecting) {
            FD_SET(s, &wfds);
        } else {
            FD_SET(s, &rfds);
        }
        count++;
    }

    if (count == 0) return false;

    struct timeval tv = {0, 1000}; // 1ms timeout
    int n = select(0, &rfds, &wfds, nullptr, &tv);
    if (n <= 0) return true;

    for (size_t i = 0; i < nat_entries_.size(); i++) {
        auto* e = nat_entries_[i].get();
        SOCKET s = static_cast<SOCKET>(e->host_socket);
        if (s == INVALID_SOCKET) continue;

        if (e->connecting && FD_ISSET(s, &wfds)) {
            e->connecting = false;
            if (!e->pending_data.empty()) {
                send(s, reinterpret_cast<const char*>(e->pending_data.data()),
                     static_cast<int>(e->pending_data.size()), 0);
                e->pending_data.clear();
            }
        }

        if (!e->connecting && FD_ISSET(s, &rfds)) {
            if (e->proto == IPPROTO_TCP)
                HandleTcpReadable(e);
            else
                HandleUdpReadable(e);
        }
    }
    return true;
}

void NetBackend::HandleTcpReadable(NatEntry* entry) {
    char buf[4096];
    SOCKET s = static_cast<SOCKET>(entry->host_socket);
    int n = recv(s, buf, sizeof(buf), 0);

    if (n <= 0) {
        // Connection closed or error
        if (entry->conn_pcb) {
            tcp_close(static_cast<struct tcp_pcb*>(entry->conn_pcb));
            entry->conn_pcb = nullptr;
        }
        closesocket(s);
        entry->host_socket = INVALID_SOCKET;
        return;
    }

    if (entry->conn_pcb) {
        auto* pcb = static_cast<struct tcp_pcb*>(entry->conn_pcb);
        // Check if lwIP has space in send buffer
        uint16_t avail = tcp_sndbuf(pcb);
        uint16_t to_write = static_cast<uint16_t>(std::min(static_cast<int>(avail), n));
        if (to_write > 0) {
            tcp_write(pcb, buf, to_write, TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
        }
    }
}

void NetBackend::HandleUdpReadable(NatEntry* entry) {
    char buf[2048];
    SOCKET s = static_cast<SOCKET>(entry->host_socket);
    struct sockaddr_in from{};
    int fromlen = sizeof(from);
    int n = recvfrom(s, buf, sizeof(buf), 0,
                     reinterpret_cast<sockaddr*>(&from), &fromlen);
    if (n <= 0) return;

    // Build Ethernet + IP + UDP frame directly (bypass lwIP for responses)
    uint32_t frame_len = sizeof(EthHdr) + 20 + sizeof(UdpHdr) + n;
    std::vector<uint8_t> frame(frame_len);

    auto* eth = reinterpret_cast<EthHdr*>(frame.data());
    memcpy(eth->dst, kGuestMac, 6);
    memcpy(eth->src, kGatewayMac, 6);
    eth->type = htons(0x0800);

    auto* ip = reinterpret_cast<IpHdr*>(frame.data() + sizeof(EthHdr));
    ip->ver_ihl = 0x45;
    ip->ttl = 64;
    ip->proto = IPPROTO_UDP;
    ip->src_ip = htonl(entry->real_dst_ip);
    ip->dst_ip = htonl(entry->guest_ip);
    ip->total_len = htons(static_cast<uint16_t>(20 + sizeof(UdpHdr) + n));
    ip->id = htons(static_cast<uint16_t>(rand()));
    RecalcIpChecksum(ip);

    auto* udp = reinterpret_cast<UdpHdr*>(frame.data() + sizeof(EthHdr) + 20);
    udp->src_port = htons(entry->real_dst_port);
    udp->dst_port = htons(entry->guest_port);
    udp->length = htons(static_cast<uint16_t>(sizeof(UdpHdr) + n));
    udp->checksum = 0;

    memcpy(frame.data() + sizeof(EthHdr) + 20 + sizeof(UdpHdr), buf, n);

    InjectFrame(frame.data(), frame_len);
}

// ============================================================
// ICMP relay
// ============================================================

void NetBackend::HandleIcmpOut(uint32_t src_ip, uint32_t dst_ip,
                                const uint8_t* icmp_data, uint32_t icmp_len) {
    if (icmp_socket_ == ~(uintptr_t)0) {
        SOCKET s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
        if (s == INVALID_SOCKET) {
            LOG_ERROR("Failed to create ICMP socket (need admin?)");
            return;
        }
        u_long nonblock = 1;
        ioctlsocket(s, FIONBIO, &nonblock);
        icmp_socket_ = static_cast<uintptr_t>(s);
    }

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = htonl(dst_ip);
    int sent = sendto(static_cast<SOCKET>(icmp_socket_),
                      reinterpret_cast<const char*>(icmp_data),
                      static_cast<int>(icmp_len), 0,
                      reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    (void)sent;
}

void NetBackend::PollIcmpSocket() {
    if (icmp_socket_ == ~(uintptr_t)0) return;
    SOCKET s = static_cast<SOCKET>(icmp_socket_);

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    struct timeval tv = {0, 0};
    if (select(0, &rfds, nullptr, nullptr, &tv) <= 0) return;

    char buf[2048];
    struct sockaddr_in from{};
    int fromlen = sizeof(from);
    int n = recvfrom(s, buf, sizeof(buf), 0,
                     reinterpret_cast<sockaddr*>(&from), &fromlen);
    if (n <= 0) return;

    // Winsock raw ICMP recv includes the IP header; skip it
    if (n < 20) return;
    auto* recv_ip = reinterpret_cast<IpHdr*>(buf);
    uint32_t recv_ip_hdr_len = (recv_ip->ver_ihl & 0xF) * 4;
    if (static_cast<uint32_t>(n) < recv_ip_hdr_len + 8) return;

    const uint8_t* icmp_payload = reinterpret_cast<const uint8_t*>(buf) + recv_ip_hdr_len;
    uint32_t icmp_len = n - recv_ip_hdr_len;
    uint32_t from_ip = ntohl(from.sin_addr.s_addr);

    // Build response frame: Eth + IP + ICMP
    uint32_t frame_len = sizeof(EthHdr) + 20 + icmp_len;
    std::vector<uint8_t> frame(frame_len);

    auto* eth = reinterpret_cast<EthHdr*>(frame.data());
    memcpy(eth->dst, kGuestMac, 6);
    memcpy(eth->src, kGatewayMac, 6);
    eth->type = htons(0x0800);

    auto* ip = reinterpret_cast<IpHdr*>(frame.data() + sizeof(EthHdr));
    ip->ver_ihl = 0x45;
    ip->ttl = 64;
    ip->proto = IPPROTO_ICMP;
    ip->src_ip = htonl(from_ip);
    ip->dst_ip = htonl(kGuestIp);
    ip->total_len = htons(static_cast<uint16_t>(20 + icmp_len));
    ip->id = htons(static_cast<uint16_t>(rand()));
    RecalcIpChecksum(ip);

    memcpy(frame.data() + sizeof(EthHdr) + 20, icmp_payload, icmp_len);
    InjectFrame(frame.data(), frame_len);
}

// ============================================================
// Port forwarding
// ============================================================

void NetBackend::SetupPortForwards() {
    g_net_backend = this;

    for (auto& pf : port_forwards_) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) {
            LOG_ERROR("Port forward: failed to create listener for port %u", pf.host_port);
            continue;
        }
        int reuse = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        u_long nonblock = 1;
        ioctlsocket(s, FIONBIO, &nonblock);

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(pf.host_port);

        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
            listen(s, 5) == SOCKET_ERROR) {
            LOG_ERROR("Port forward: failed to bind/listen on port %u", pf.host_port);
            closesocket(s);
            continue;
        }

        pf.listener = static_cast<uintptr_t>(s);
        LOG_INFO("Port forward: host:%u -> guest:%u", pf.host_port, pf.guest_port);
    }
}

void NetBackend::PollPortForwards() {
    for (auto& pf : port_forwards_) {
        if (pf.listener == ~(uintptr_t)0) continue;
        SOCKET ls = static_cast<SOCKET>(pf.listener);

        // Check for new connections
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ls, &rfds);
        struct timeval tv = {0, 0};
        if (select(0, &rfds, nullptr, nullptr, &tv) > 0 && FD_ISSET(ls, &rfds)) {
            SOCKET cs = accept(ls, nullptr, nullptr);
            if (cs != INVALID_SOCKET) {
                u_long nonblock = 1;
                ioctlsocket(cs, FIONBIO, &nonblock);

                // Create lwIP TCP connection to guest
                struct tcp_pcb* pcb = tcp_new();
                ip_addr_t guest_addr;
                IP4_ADDR(ip_2_ip4(&guest_addr), 10, 0, 2, 15);

                PfEntry::Conn conn;
                conn.host_sock = static_cast<uintptr_t>(cs);
                conn.guest_pcb = pcb;
                pf.conns.push_back(conn);

                auto* conn_ptr = &pf.conns.back();
                tcp_arg(pcb, conn_ptr);
                tcp_recv(pcb, [](void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err) -> err_t {
                    auto* c = static_cast<PfEntry::Conn*>(arg);
                    if (!p) {
                        // Guest closed connection
                        if (c->host_sock != ~(uintptr_t)0) {
                            closesocket(static_cast<SOCKET>(c->host_sock));
                            c->host_sock = ~(uintptr_t)0;
                        }
                        tcp_close(pcb);
                        c->guest_pcb = nullptr;
                        return ERR_OK;
                    }
                    std::vector<uint8_t> data(p->tot_len);
                    pbuf_copy_partial(p, data.data(), p->tot_len, 0);
                    tcp_recved(pcb, p->tot_len);
                    pbuf_free(p);
                    if (c->host_sock != ~(uintptr_t)0) {
                        send(static_cast<SOCKET>(c->host_sock),
                             reinterpret_cast<const char*>(data.data()),
                             static_cast<int>(data.size()), 0);
                    }
                    return ERR_OK;
                });
                tcp_err(pcb, [](void* arg, err_t err) {
                    auto* c = static_cast<PfEntry::Conn*>(arg);
                    c->guest_pcb = nullptr;
                    if (c->host_sock != ~(uintptr_t)0) {
                        closesocket(static_cast<SOCKET>(c->host_sock));
                        c->host_sock = ~(uintptr_t)0;
                    }
                });

                tcp_connect(pcb, &guest_addr, pf.guest_port,
                    [](void* arg, struct tcp_pcb* pcb, err_t err) -> err_t {
                    auto* c = static_cast<PfEntry::Conn*>(arg);
                    c->guest_connected = true;
                    if (!c->pending_to_guest.empty()) {
                        tcp_write(pcb, c->pending_to_guest.data(),
                                  static_cast<u16_t>(c->pending_to_guest.size()),
                                  TCP_WRITE_FLAG_COPY);
                        tcp_output(pcb);
                        c->pending_to_guest.clear();
                    }
                    return ERR_OK;
                });

                LOG_INFO("Port forward: new connection on host:%u -> guest:%u",
                         pf.host_port, pf.guest_port);
            }
        }

        // Poll active connections for data from host
        for (auto& c : pf.conns) {
            if (c.host_sock == ~(uintptr_t)0) continue;
            SOCKET s = static_cast<SOCKET>(c.host_sock);
            FD_ZERO(&rfds);
            FD_SET(s, &rfds);
            tv = {0, 0};
            if (select(0, &rfds, nullptr, nullptr, &tv) > 0 && FD_ISSET(s, &rfds)) {
                char buf[4096];
                int n = recv(s, buf, sizeof(buf), 0);
                if (n <= 0) {
                    closesocket(s);
                    c.host_sock = ~(uintptr_t)0;
                    if (c.guest_pcb) {
                        tcp_close(static_cast<struct tcp_pcb*>(c.guest_pcb));
                        c.guest_pcb = nullptr;
                    }
                } else if (c.guest_connected && c.guest_pcb) {
                    tcp_write(static_cast<struct tcp_pcb*>(c.guest_pcb),
                              buf, static_cast<u16_t>(n), TCP_WRITE_FLAG_COPY);
                    tcp_output(static_cast<struct tcp_pcb*>(c.guest_pcb));
                } else {
                    c.pending_to_guest.insert(c.pending_to_guest.end(),
                                              buf, buf + n);
                }
            }
        }
    }
}
