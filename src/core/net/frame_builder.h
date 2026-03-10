#pragma once

#include "core/net/net_packet.h"
#include <cstdlib>
#include <vector>

// Helpers to build complete Ethernet frames for injection to the guest.

namespace frame {

inline std::vector<uint8_t> BuildIpFrame(
    const uint8_t* dst_mac, const uint8_t* src_mac,
    uint32_t src_ip, uint32_t dst_ip, uint8_t proto,
    const void* payload, uint32_t payload_len) {

    uint32_t frame_len = sizeof(EthHdr) + 20 + payload_len;
    std::vector<uint8_t> frame(frame_len);

    auto* eth = reinterpret_cast<EthHdr*>(frame.data());
    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, src_mac, 6);
    eth->type = htons(0x0800);

    auto* ip = reinterpret_cast<IpHdr*>(frame.data() + sizeof(EthHdr));
    ip->ver_ihl = 0x45;
    ip->ttl = 64;
    ip->proto = proto;
    ip->src_ip = htonl(src_ip);
    ip->dst_ip = htonl(dst_ip);
    ip->total_len = htons(static_cast<uint16_t>(20 + payload_len));
    ip->id = htons(static_cast<uint16_t>(rand()));
    RecalcIpChecksum(ip);

    memcpy(frame.data() + sizeof(EthHdr) + 20, payload, payload_len);
    return frame;
}

inline std::vector<uint8_t> BuildUdpFrame(
    const uint8_t* dst_mac, const uint8_t* src_mac,
    uint32_t src_ip, uint32_t dst_ip,
    uint16_t src_port, uint16_t dst_port,
    const void* payload, uint32_t payload_len) {

    uint32_t l4_len = sizeof(UdpHdr) + payload_len;
    uint32_t frame_len = sizeof(EthHdr) + 20 + l4_len;
    std::vector<uint8_t> frame(frame_len);

    auto* eth = reinterpret_cast<EthHdr*>(frame.data());
    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, src_mac, 6);
    eth->type = htons(0x0800);

    auto* ip = reinterpret_cast<IpHdr*>(frame.data() + sizeof(EthHdr));
    ip->ver_ihl = 0x45;
    ip->ttl = 64;
    ip->proto = IPPROTO_UDP;
    ip->src_ip = htonl(src_ip);
    ip->dst_ip = htonl(dst_ip);
    ip->total_len = htons(static_cast<uint16_t>(20 + l4_len));
    ip->id = htons(static_cast<uint16_t>(rand()));
    RecalcIpChecksum(ip);

    auto* udp = reinterpret_cast<UdpHdr*>(frame.data() + sizeof(EthHdr) + 20);
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length = htons(static_cast<uint16_t>(l4_len));
    udp->checksum = 0;

    memcpy(frame.data() + sizeof(EthHdr) + 20 + sizeof(UdpHdr), payload, payload_len);
    return frame;
}

} // namespace frame
