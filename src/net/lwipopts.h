#ifndef LWIPOPTS_H
#define LWIPOPTS_H

// NO_SYS=1: bare-metal / single-threaded mode (callback API only)
#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0

// No sequential/socket APIs in NO_SYS mode
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

// IPv4 only
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0

// Protocols
#define LWIP_ARP                    1
#define ETHARP_SUPPORT_STATIC_ENTRIES 1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    0
#define LWIP_DHCP                   0
#define LWIP_AUTOIP                 0
#define LWIP_IGMP                   0

// Memory configuration
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (2 * 1024 * 1024)
#define MEMP_NUM_PBUF               512
#define MEMP_NUM_RAW_PCB            4
#define MEMP_NUM_UDP_PCB            64
#define MEMP_NUM_TCP_PCB            512
#define MEMP_NUM_TCP_PCB_LISTEN     128
#define MEMP_NUM_TCP_SEG            1024

// Pbuf pool
#define PBUF_POOL_SIZE              512
#define PBUF_POOL_BUFSIZE           1600

// TCP tuning
#define TCP_MSS                     1460
#define LWIP_WND_SCALE              1
#define TCP_RCV_SCALE               7
#define TCP_WND                     (256 * 1024)
#define TCP_SND_BUF                 (256 * 1024)
#define TCP_SNDLOWAT                LWIP_MIN(LWIP_MAX(((TCP_SND_BUF)/2), (2 * TCP_MSS) + 1), (0xFFFFU - (4 * TCP_MSS) - 1))
#define TCP_SND_QUEUELEN            ((4 * TCP_SND_BUF) / TCP_MSS)
// Backlog tracking accesses pcb->listener after the accept callback,
// but the NAT proxy closes the listener inside the callback.  Disable
// to avoid use-after-free on the freed listen PCB.
#define TCP_LISTEN_BACKLOG          0

// NAT proxy doesn't need long TIME_WAIT; the real TIME_WAIT is
// handled by the host-side Winsock socket, not the lwIP PCB.
#define TCP_MSL                     5000

// Checksum — lwIP generates outgoing, skip incoming verification
// (we do our own incremental checksum updates for NAT rewriting)
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_GEN_ICMP           1
#define CHECKSUM_CHECK_IP           0
#define CHECKSUM_CHECK_UDP          0
#define CHECKSUM_CHECK_TCP          0
#define CHECKSUM_CHECK_ICMP         0

// Disable features we don't need
#define LWIP_NETIF_STATUS_CALLBACK  0
#define LWIP_NETIF_LINK_CALLBACK    0
#define LWIP_STATS                  0
#define LWIP_STATS_DISPLAY          0
#define LWIP_NETIF_TX_SINGLE_PBUF   1

// Don't check TCP checksum on incoming rewritten packets
// since we do incremental checksum updates
#define LWIP_TCP_TIMESTAMPS         0

#endif
