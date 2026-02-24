#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <unordered_map>

struct PortForward {
    uint16_t host_port;
    uint16_t guest_port;
};

class VirtioNetDevice;

// User-mode network backend: lwIP + NAT + DHCP + port forwarding.
// Runs a dedicated network thread for lwIP event loop and socket I/O.
class NetBackend {
public:
    NetBackend();
    ~NetBackend();

    bool Start(VirtioNetDevice* dev,
               std::function<void()> irq_cb,
               const std::vector<PortForward>& forwards);
    void Stop();

    // Called from vCPU thread when guest transmits an Ethernet frame.
    void EnqueueTx(const uint8_t* frame, uint32_t len);

private:
    void NetworkThread();
    void ProcessPendingTx();

    // ARP / DHCP handled at raw Ethernet level before lwIP
    bool HandleArpOrDhcp(const uint8_t* frame, uint32_t len);
    void SendDhcpReply(uint8_t type, uint32_t xid,
                       const uint8_t* chaddr, uint32_t req_ip);

    // NAT rewriting
    struct NatEntry;
    NatEntry* FindNatEntry(uint32_t guest_port, uint32_t dst_ip,
                           uint16_t dst_port, uint8_t proto);
    NatEntry* CreateNatEntry(uint32_t guest_ip, uint16_t guest_port,
                             uint32_t dst_ip, uint16_t dst_port,
                             uint8_t proto);
    void RemoveNatEntry(NatEntry* entry);
    uint16_t AllocProxyPort();

    void RewriteAndFeed(uint8_t* frame, uint32_t len, NatEntry* entry);

    // TCP NAT callbacks (static so they can be registered with lwIP)
    static void* AsTcpArg(NatEntry* e);
    void OnTcpAccepted(NatEntry* entry, void* new_pcb);
    void OnTcpRecv(NatEntry* entry, void* pcb, void* p);
    void OnTcpErr(NatEntry* entry);

    // UDP NAT
    void OnUdpRecv(NatEntry* entry, void* pcb, void* p);

    // Winsock polling
    bool PollSockets();
    void HandleTcpReadable(NatEntry* entry);
    void HandleUdpReadable(NatEntry* entry);

    // ICMP relay
    void HandleIcmpOut(uint32_t src_ip, uint32_t dst_ip,
                       const uint8_t* icmp_data, uint32_t icmp_len);
    void PollIcmpSocket();
    uintptr_t icmp_socket_ = ~(uintptr_t)0;

    // Port forwarding
    struct PfEntry;
    void SetupPortForwards();
    void PollPortForwards();

    VirtioNetDevice* virtio_net_ = nullptr;
    std::function<void()> irq_callback_;

    std::thread net_thread_;
    std::atomic<bool> running_{false};

    // TX queue (vCPU → net thread)
    std::mutex tx_mutex_;
    std::vector<std::vector<uint8_t>> tx_queue_;

    // lwIP netif (opaque pointer to avoid lwIP headers in .h)
    void* netif_ = nullptr;

    // NAT table
    struct NatEntry {
        uint8_t  proto;
        uint32_t guest_ip;
        uint16_t guest_port;
        uint32_t real_dst_ip;
        uint16_t real_dst_port;
        uint16_t proxy_port;
        void*    listen_pcb = nullptr;
        void*    conn_pcb   = nullptr;
        uintptr_t host_socket = ~(uintptr_t)0;
        bool     connecting  = false;
        std::vector<uint8_t> pending_data;
    };
    std::vector<std::unique_ptr<NatEntry>> nat_entries_;
    uint16_t next_proxy_port_ = 10000;

    // Port forwarding
    struct PfEntry {
        uint16_t host_port;
        uint16_t guest_port;
        uintptr_t listener = ~(uintptr_t)0;
        struct Conn {
            uintptr_t host_sock = ~(uintptr_t)0;
            void*     guest_pcb = nullptr;
            bool      guest_connected = false;
            std::vector<uint8_t> pending_to_guest;
            std::vector<uint8_t> pending_to_host;
        };
        std::vector<Conn> conns;
    };
    std::vector<PfEntry> port_forwards_;

public:
    // Network addresses (public for use by lwIP callbacks)
    static constexpr uint32_t kGatewayIp = 0x0A000202; // 10.0.2.2
    static constexpr uint32_t kGuestIp   = 0x0A00020F; // 10.0.2.15
    static constexpr uint32_t kNetmask   = 0xFFFFFF00; // 255.255.255.0
    static constexpr uint8_t  kGatewayMac[6] = {0x52,0x54,0x00,0x12,0x34,0x57};
    static constexpr uint8_t  kGuestMac[6]   = {0x52,0x54,0x00,0x12,0x34,0x56};

    // Public for lwIP free-function callbacks
    void ReverseRewrite(uint8_t* frame, uint32_t len);
    void InjectFrame(const uint8_t* frame, uint32_t len);
};
