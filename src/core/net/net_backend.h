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
               const std::vector<PortForward>& forwards,
               const std::vector<GuestForward>& guest_forwards = {});
    void Stop();

    void SetLinkUp(bool up);

    using PortForwardCallback = std::function<void(std::vector<uint16_t> failed_ports)>;
    void UpdatePortForwards(const std::vector<PortForward>& forwards,
                            PortForwardCallback cb = nullptr);

    void UpdateGuestForwards(const std::vector<GuestForward>& guest_forwards);

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

    // Returns host DNS server addresses (host byte order) for DHCP.
    static std::vector<uint32_t> GetHostDnsServers();

    // ICMP relay
    void HandleIcmpOut(uint32_t src_ip, uint32_t dst_ip,
                       const uint8_t* icmp_data, uint32_t icmp_len);
#ifdef _WIN32
    void IcmpWorkerThread();
    struct IcmpRequest {
        uint32_t src_ip;
        uint32_t dst_ip;
        std::vector<uint8_t> icmp_data;
    };
    std::mutex icmp_mutex_;
    std::vector<IcmpRequest> icmp_queue_;
    std::condition_variable icmp_cv_;
    std::thread icmp_thread_;
    std::atomic<bool> icmp_running_{false};
    void* icmp_handle_ = nullptr;   // HANDLE from IcmpCreateFile
#else
    void HandleIcmpReadable();
    uintptr_t icmp_socket_ = ~(uintptr_t)0;
    uv_poll_t icmp_poll_{};
    bool icmp_poll_active_ = false;
#endif

    std::vector<uint16_t> SetupPortForwards();

    VirtioNetDevice* virtio_net_ = nullptr;
    std::function<void()> irq_callback_;

    std::thread net_thread_;
    std::atomic<bool> running_{false};
    std::mutex loop_ready_mutex_;
    std::condition_variable loop_ready_cv_;
    bool loop_ready_{false};

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
        bool     gateway_local = false; // dst was gateway; skip src IP rewrite
        uint32_t guestfwd_ip = 0;     // if non-zero, rewrite src IP to this (host order)
        uint16_t guestfwd_port = 0;   // if non-zero, rewrite src port to this
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
        std::string host_ip;  // bind address: empty/"127.0.0.1" = loopback, "0.0.0.0" = LAN
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
    // PfEntry addresses are used as libuv callback data, so storage must stay stable.
    std::list<PfEntry> port_forwards_;
    void TeardownPfConn(PfEntry::Conn& c);
    void DrainPfToGuest(PfEntry::Conn& conn);
    void DrainPfToHost(PfEntry::Conn& conn);
    void UpdatePfConnPoll(PfEntry::Conn& conn);
    void OnPfConnPollEvent(PfEntry::Conn* conn, int status, int events);
    void OnPfListenerReadable(PfEntry* pf);
    void TeardownPortForwards();
    void CheckPendingUpdates();

    // Guest forwarding (guestfwd): guest connects to a virtual IP:port,
    // traffic is NAT'd to a specific host address:port.
    struct GfEntry {
        uint32_t guest_ip;
        uint16_t guest_port;
        uint32_t host_ip;     // resolved from host_addr
        uint16_t host_port;
    };
    std::vector<GfEntry> guest_forwards_;

    bool HandleGuestFwdArp(const uint8_t* frame, uint32_t len);
    bool IsGuestFwdIp(uint32_t ip) const;
    const GfEntry* FindGuestFwd(uint32_t guest_ip, uint16_t guest_port) const;
    void ApplyGuestForwards(const std::vector<GuestForward>& gfs);

    std::mutex gf_update_mutex_;
    std::optional<std::vector<GuestForward>> pending_gf_update_;
    uv_async_t gf_update_wakeup_{};
    static void OnGfUpdateReady(uv_async_t* handle);
    void CheckPendingGfUpdates();

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
    PortForwardCallback pf_update_cb_;

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
