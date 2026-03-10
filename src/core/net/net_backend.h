#pragma once

#include "common/vm_model.h"
#include "core/net/poll_handle.h"

#include <uv.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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

    void SetLinkUp(bool up);
    void UpdatePortForwards(const std::vector<PortForward>& forwards);

    // Synchronous version that waits for the network thread to apply the update
    // and returns a list of host ports that failed to bind.
    std::vector<uint16_t> UpdatePortForwardsSync(const std::vector<PortForward>& forwards);

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
    void CleanupStaleEntries();
    uint16_t AllocProxyPort();
    bool IsProxyPortInUse(uint16_t port) const;

    void RewriteAndFeed(uint8_t* frame, uint32_t len, NatEntry* entry);

    // TCP NAT callbacks
    void OnTcpAccepted(NatEntry* entry, void* new_pcb);
    void OnTcpRecv(NatEntry* entry, void* pcb, void* p);
    void OnTcpErr(NatEntry* entry);

    // UDP NAT
    void OnUdpRecv(NatEntry* entry, void* pcb, void* p);

    // Socket I/O handlers
    void HandleTcpReadable(NatEntry* entry);
    void HandleUdpReadable(NatEntry* entry);
    void DrainTcpToGuest(NatEntry* entry);
    void DrainTcpToHost(NatEntry* entry);
    void UpdateNatPoll(NatEntry* entry);
    void OnNatPollEvent(NatEntry* entry, int status, int events);

    // Consolidated teardown helpers (NatEntry only; TeardownPfConn declared after PfEntry)
    void CloseHostSocket(NatEntry* e);
    void DetachAndCloseLwipPcb(NatEntry* e);
    void TeardownNatEntry(NatEntry* e);

    // ICMP relay
    void HandleIcmpOut(uint32_t src_ip, uint32_t dst_ip,
                       const uint8_t* icmp_data, uint32_t icmp_len);
    void HandleIcmpReadable();
    uintptr_t icmp_socket_ = ~(uintptr_t)0;
    uv_poll_t icmp_poll_{};
    bool icmp_poll_active_ = false;

    std::vector<uint16_t> SetupPortForwards();

    VirtioNetDevice* virtio_net_ = nullptr;
    std::function<void()> irq_callback_;

    std::thread net_thread_;
    std::atomic<bool> running_{false};

    // TX queue (vCPU → net thread)
    std::mutex tx_mutex_;
    std::vector<std::vector<uint8_t>> tx_queue_;

    // lwIP netif (opaque pointer to avoid lwIP headers in .h)
    void* netif_ = nullptr;

    // Listen PCBs to close after tcp_input returns (cannot close inside
    // accept callback because lwIP still accesses pcb->listener afterward).
    std::vector<void*> deferred_listen_close_;

    // NAT table
    enum class NatState : uint8_t {
        Listening,    // TCP: listen_pcb active, waiting for guest SYN
        Connecting,   // TCP: guest accepted, host socket connecting
        Established,  // Both sides connected (TCP) or active (UDP)
        HalfClosed,   // One side done, draining residual data
        Closed        // All resources released, pending cleanup removal
    };
    struct NatEntry {
        NetBackend* backend = nullptr;
        uint8_t  proto;
        uint32_t guest_ip;
        uint16_t guest_port;
        uint32_t real_dst_ip;
        uint16_t real_dst_port;
        uint16_t proxy_port;
        NatState state = NatState::Established;
        void*    listen_pcb = nullptr;
        void*    conn_pcb   = nullptr;
        uintptr_t host_socket = ~(uintptr_t)0;
        std::vector<uint8_t> pending_data;     // buffered during Connecting
        std::vector<uint8_t> pending_to_guest;
        std::vector<uint8_t> pending_to_host;
        uint64_t last_active_ms = 0;
        PollHandle poll;
    };
    std::vector<std::unique_ptr<NatEntry>> nat_entries_;
    uint16_t next_proxy_port_ = 10000;

    // Port forwarding
    struct PfEntry {
        NetBackend* backend = nullptr;
        uint16_t host_port;
        uint16_t guest_port;
        uintptr_t listener = ~(uintptr_t)0;
        PollHandle listener_poll;
        struct Conn {
            NetBackend* backend = nullptr;
            uintptr_t host_sock = ~(uintptr_t)0;
            void*     guest_pcb = nullptr;
            bool      guest_connected = false;
            std::vector<uint8_t> pending_to_guest;
            std::vector<uint8_t> pending_to_host;
            PollHandle poll;
        };
        std::list<Conn> conns;
    };
    std::vector<PfEntry> port_forwards_;
    void TeardownPfConn(PfEntry::Conn& c);
    void DrainPfToGuest(PfEntry::Conn& conn);
    void DrainPfToHost(PfEntry::Conn& conn);
    void UpdatePfConnPoll(PfEntry::Conn& conn);
    void OnPfConnPollEvent(PfEntry::Conn* conn, int status, int events);
    void OnPfListenerReadable(PfEntry* pf);
    void TeardownPortForwards();
    void CheckPendingUpdates();

    std::atomic<bool> link_up_{true};

    // libuv event loop (owned by net_thread_)
    uv_loop_t loop_{};
    uv_timer_t lwip_timer_{};
    uv_timer_t cleanup_timer_{};
    uv_async_t tx_wakeup_{};
    uv_async_t pf_update_wakeup_{};
    uv_async_t stop_wakeup_{};

    static void OnLwipTimer(uv_timer_t* handle);
    static void OnCleanupTimer(uv_timer_t* handle);
    static void OnTxReady(uv_async_t* handle);
    static void OnPfUpdateReady(uv_async_t* handle);
    static void OnStopSignal(uv_async_t* handle);
    void RescheduleLwipTimer();

    std::mutex pf_update_mutex_;
    std::optional<std::vector<PortForward>> pending_pf_update_;
    bool pf_update_sync_ = false;
    std::condition_variable pf_update_cv_;
    std::vector<uint16_t> pf_update_failed_ports_;

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
