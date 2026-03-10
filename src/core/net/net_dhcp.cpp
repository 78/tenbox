// NetBackend: DHCP server (intercepts DHCP before lwIP).

#include "core/net/net_compat.h"
#include "core/net/net_backend.h"
#include "core/net/net_packet.h"

#include <cstring>
#include <memory>

// ============================================================
// Host DNS resolver lookup
// ============================================================

static uint32_t GetHostDnsServer() {
#ifdef _WIN32
    ULONG buf_len = sizeof(FIXED_INFO);
    auto buf = std::make_unique<uint8_t[]>(buf_len);
    DWORD ret = GetNetworkParams(reinterpret_cast<FIXED_INFO*>(buf.get()), &buf_len);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buf = std::make_unique<uint8_t[]>(buf_len);
        ret = GetNetworkParams(reinterpret_cast<FIXED_INFO*>(buf.get()), &buf_len);
    }
    if (ret == NO_ERROR) {
        auto* info = reinterpret_cast<FIXED_INFO*>(buf.get());
        for (auto* entry = &info->DnsServerList; entry; entry = entry->Next) {
            unsigned long addr = inet_addr(entry->IpAddress.String);
            if (addr == INADDR_NONE || addr == 0)
                continue;
            if ((ntohl(addr) >> 24) == 127)
                continue;
            return ntohl(addr);
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
                        fclose(fp);
                        return ip;
                    }
                }
            }
        }
        fclose(fp);
    }
#endif
    return 0x08080808; // fallback: 8.8.8.8
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
    // DNS
    *opt++ = 6; *opt++ = 4;
    uint32_t dns = htonl(GetHostDnsServer());
    memcpy(opt, &dns, 4); opt += 4;
    // End
    *opt++ = 255;
    off = static_cast<uint32_t>(opt - pkt);

    uint32_t udp_len = off - ip_off - 20;
    udp->length = htons(static_cast<uint16_t>(udp_len));
    ip->total_len = htons(static_cast<uint16_t>(off - sizeof(EthHdr)));
    RecalcIpChecksum(ip);

    InjectFrame(pkt, off);
}
