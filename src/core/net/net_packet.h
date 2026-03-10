#pragma once

// Packet header structures and checksum utilities.
// This header requires htons/htonl/ntohl/ntohs to be available.
// Include net_compat.h (or system/lwIP headers) before this header.

#include <cstdint>
#include <cstring>

// Packed protocol header structures for raw packet manipulation.

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

// Checksum utilities

inline uint16_t ChecksumFold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}

inline uint32_t ChecksumPartial(const void* data, int len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t sum = 0;
    for (int i = 0; i + 1 < len; i += 2)
        sum += (p[i] << 8) | p[i + 1];
    if (len & 1)
        sum += p[len - 1] << 8;
    return sum;
}

inline void RecalcIpChecksum(IpHdr* ip) {
    ip->checksum = 0;
    int hdr_len = (ip->ver_ihl & 0xF) * 4;
    uint32_t sum = ChecksumPartial(ip, hdr_len);
    ip->checksum = htons(ChecksumFold(sum));
}

inline void IncrementalCksumUpdate(uint16_t* cksum,
                                   uint32_t old_ip, uint32_t new_ip,
                                   uint16_t old_port, uint16_t new_port) {
    uint32_t sum = static_cast<uint16_t>(~ntohs(*cksum));
    sum += static_cast<uint16_t>(~(ntohl(old_ip) >> 16) & 0xFFFF);
    sum += (ntohl(new_ip) >> 16) & 0xFFFF;
    sum += static_cast<uint16_t>(~(ntohl(old_ip) & 0xFFFF));
    sum += ntohl(new_ip) & 0xFFFF;
    sum += static_cast<uint16_t>(~ntohs(old_port) & 0xFFFF);
    sum += ntohs(new_port);
    *cksum = htons(ChecksumFold(sum));
}
