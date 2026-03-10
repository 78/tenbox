// NetBackend: ICMP relay via raw socket.

#include "core/net/net_compat.h"
#include "core/net/net_backend.h"
#include "core/net/net_packet.h"
#include "core/net/frame_builder.h"
#include "core/vmm/types.h"

#include <uv.h>

void NetBackend::HandleIcmpOut(uint32_t src_ip, uint32_t dst_ip,
                                const uint8_t* icmp_data, uint32_t icmp_len) {
    if (icmp_socket_ == ~(uintptr_t)0) {
        SocketHandle s = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
        if (s == SOCK_INVALID) {
            LOG_ERROR("Failed to create ICMP socket (need admin?)");
            return;
        }
        SOCK_SETNONBLOCK(s);
        icmp_socket_ = static_cast<uintptr_t>(s);
        uv_poll_init_socket(&loop_, &icmp_poll_, s);
        icmp_poll_.data = this;
        uv_poll_start(&icmp_poll_, UV_READABLE, [](uv_poll_t* h, int status, int) {
            if (status < 0) return;
            static_cast<NetBackend*>(h->data)->HandleIcmpReadable();
        });
        icmp_poll_active_ = true;
    }

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = htonl(dst_ip);
    sendto(static_cast<SocketHandle>(icmp_socket_),
           SOCK_CCAST(icmp_data),
           static_cast<int>(icmp_len), 0,
           reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
}

void NetBackend::HandleIcmpReadable() {
    if (icmp_socket_ == ~(uintptr_t)0) return;
    SocketHandle s = static_cast<SocketHandle>(icmp_socket_);

    for (;;) {
        char buf[2048];
        struct sockaddr_in from{};
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &fromlen);
        if (n <= 0) break;
        if (n < 20) continue;
        auto* recv_ip = reinterpret_cast<IpHdr*>(buf);
        uint32_t recv_ip_hdr_len = (recv_ip->ver_ihl & 0xF) * 4;
        if (static_cast<uint32_t>(n) < recv_ip_hdr_len + 8) continue;

        const uint8_t* icmp_payload = reinterpret_cast<const uint8_t*>(buf) + recv_ip_hdr_len;
        uint32_t icmp_len = n - recv_ip_hdr_len;
        uint32_t from_ip = ntohl(from.sin_addr.s_addr);

        auto frame = frame::BuildIpFrame(
            kGuestMac, kGatewayMac,
            from_ip, kGuestIp, IPPROTO_ICMP,
            icmp_payload, icmp_len);

        InjectFrame(frame.data(), static_cast<uint32_t>(frame.size()));
    }
}
