// NetBackend: DHCP server (intercepts DHCP before lwIP).

#include "core/net/net_compat.h"
#include "core/net/net_backend.h"
#include "core/net/net_packet.h"

#include <cstring>
#include <memory>
#include <vector>
#include <algorithm>

// ============================================================
// Host DNS resolver lookup
// ============================================================

std::vector<uint32_t> NetBackend::GetHostDnsServers() {
    std::vector<uint32_t> result;
#ifdef _WIN32
    struct AdapterDnsInfo {
        uint32_t dns_addr;
        uint32_t metric;
    };

    ULONG buf_len = 15000;
    std::unique_ptr<uint8_t[]> buf;
    ULONG ret;

    for (int i = 0; i < 3; i++) {
        buf = std::make_unique<uint8_t[]>(buf_len);
        ret = GetAdaptersAddresses(AF_INET,
                GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                nullptr,
                reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.get()),
                &buf_len);
        if (ret == ERROR_SUCCESS) break;
        if (ret != ERROR_BUFFER_OVERFLOW) break;
    }

    if (ret == ERROR_SUCCESS) {
        std::vector<AdapterDnsInfo> candidates;
        auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.get());

        while (adapter) {
            if (adapter->OperStatus == IfOperStatusUp &&
                adapter->IfType != IF_TYPE_SOFTWARE_LOOPBACK) {

                uint32_t total_metric = adapter->Ipv4Metric;

                for (auto* dns = adapter->FirstDnsServerAddress; dns; dns = dns->Next) {
                    if (dns->Address.lpSockaddr->sa_family == AF_INET) {
                        auto* si = reinterpret_cast<sockaddr_in*>(dns->Address.lpSockaddr);
                        uint32_t addr = ntohl(si->sin_addr.s_addr);
                        if ((addr >> 24) != 127 && addr != 0) {
                            candidates.push_back({addr, total_metric});
                        }
                    }
                }
            }
            adapter = adapter->Next;
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const AdapterDnsInfo& a, const AdapterDnsInfo& b) {
                      return a.metric < b.metric;
                  });

        for (const auto& c : candidates) {
            if (std::find(result.begin(), result.end(), c.dns_addr) == result.end())
                result.push_back(c.dns_addr);
            if (result.size() >= 3) break;
        }
    }
#else
    FILE* fp = fopen("/etc/resolv.conf", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            char ns_str[64];
            if (sscanf(line, "nameserver %63s", ns_str) == 1) {
                struct in_addr addr;
                if (inet_pton(AF_INET, ns_str, &addr) == 1) {
                    uint32_t ip = ntohl(addr.s_addr);
                    if ((ip >> 24) != 127 && ip != 0) {
                        result.push_back(ip);
                        if (result.size() >= 3) break;
                    }
                }
            }
        }
        fclose(fp);
    }
#endif
    if (result.empty())
        result.push_back(0x08080808); // fallback: 8.8.8.8
    return result;
}

// ============================================================
// DHCP intercept
// ============================================================

bool NetBackend::HandleArpOrDhcp(const uint8_t* frame, uint32_t len) {
    auto* eth = reinterpret_cast<const EthHdr*>(frame);

    if (ntohs(eth->type) != 0x0800) return false;
    if (len < sizeof(EthHdr) + sizeof(IpHdr) + sizeof(UdpHdr)) return false;

    auto* ip = reinterpret_cast<const IpHdr*>(frame + sizeof(EthHdr));
    if (ip->proto != IPPROTO_UDP) return false;

    auto* udp = reinterpret_cast<const UdpHdr*>(
        frame + sizeof(EthHdr) + (ip->ver_ihl & 0xF) * 4);
    if (ntohs(udp->dst_port) != 67) return false;

    uint32_t udp_off = sizeof(EthHdr) + (ip->ver_ihl & 0xF) * 4 + sizeof(UdpHdr);
    if (len < udp_off + sizeof(DhcpMsg)) return false;
    auto* dhcp = reinterpret_cast<const DhcpMsg*>(frame + udp_off);

    if (ntohl(dhcp->magic) != 0x63825363) return false;

    uint8_t msg_type = 0;
    uint32_t req_ip = 0;
    const uint8_t* opts = frame + udp_off + sizeof(DhcpMsg);
    uint32_t opts_len = len - udp_off - sizeof(DhcpMsg);

    for (uint32_t i = 0; i < opts_len; ) {
        uint8_t opt = opts[i++];
        if (opt == 255) break;
        if (opt == 0) continue;
        if (i >= opts_len) break;
        uint8_t olen = opts[i++];
        if (i + olen > opts_len) break;
        if (opt == 53 && olen >= 1) msg_type = opts[i];
        if (opt == 50 && olen >= 4) memcpy(&req_ip, &opts[i], 4);
        i += olen;
    }

    if (msg_type == 1 || msg_type == 3) {
        uint8_t reply_type = (msg_type == 1) ? 2 : 5;
        SendDhcpReply(reply_type, dhcp->xid, dhcp->chaddr, req_ip);
        return true;
    }
    return false;
}

void NetBackend::SendDhcpReply(uint8_t type, uint32_t xid,
                                const uint8_t* chaddr, uint32_t req_ip) {
    uint8_t pkt[600]{};
    uint32_t off = 0;

    auto* eth = reinterpret_cast<EthHdr*>(pkt);
    memset(eth->dst, 0xFF, 6);
    memcpy(eth->src, kGatewayMac, 6);
    eth->type = htons(0x0800);
    off += sizeof(EthHdr);

    auto* ip = reinterpret_cast<IpHdr*>(pkt + off);
    ip->ver_ihl = 0x45;
    ip->ttl = 64;
    ip->proto = IPPROTO_UDP;
    ip->src_ip = htonl(kGatewayIp);
    ip->dst_ip = htonl(0xFFFFFFFF);
    uint32_t ip_off = off;
    off += 20;

    auto* udp = reinterpret_cast<UdpHdr*>(pkt + off);
    udp->src_port = htons(67);
    udp->dst_port = htons(68);
    off += sizeof(UdpHdr);

    auto* dhcp = reinterpret_cast<DhcpMsg*>(pkt + off);
    dhcp->op = 2;
    dhcp->htype = 1;
    dhcp->hlen = 6;
    dhcp->xid = xid;
    dhcp->yiaddr = htonl(kGuestIp);
    dhcp->siaddr = htonl(kGatewayIp);
    memcpy(dhcp->chaddr, chaddr, 6);
    dhcp->magic = htonl(0x63825363);
    off += sizeof(DhcpMsg);

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
    // DNS servers from the host
    auto dns_servers = GetHostDnsServers();
    *opt++ = 6; *opt++ = static_cast<uint8_t>(dns_servers.size() * 4);
    for (uint32_t dns : dns_servers) {
        uint32_t dns_net = htonl(dns);
        memcpy(opt, &dns_net, 4); opt += 4;
    }
    // End
    *opt++ = 255;
    off = static_cast<uint32_t>(opt - pkt);

    uint32_t udp_len = off - ip_off - 20;
    udp->length = htons(static_cast<uint16_t>(udp_len));
    ip->total_len = htons(static_cast<uint16_t>(off - sizeof(EthHdr)));
    RecalcIpChecksum(ip);

    InjectFrame(pkt, off);
}
