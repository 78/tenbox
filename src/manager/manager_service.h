#pragma once

#include "common/ports.h"
#include "common/vm_model.h"
#include "ipc/protocol_v1.h"
#include "ipc/shared_framebuffer.h"
#include "manager/app_settings.h"
#include "core/vdagent/vdagent_protocol.h"

#ifdef _WIN32
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0A00
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif
#include <uv.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct VmRuntimeHandle {
    void* process_handle = nullptr;
    uint32_t process_id = 0;
    std::string pipe_name;

    // libuv event loop (one per VM, runs on loop_thread)
    uv_loop_t loop{};
    uv_pipe_t server_pipe{};
    uv_pipe_t client_pipe{};
    uv_async_t send_wakeup{};
    uv_async_t stop_wakeup{};
    std::thread loop_thread;
    bool loop_initialized = false;
    bool pipe_connected = false;

    // Signalled by the loop thread once uv_run is about to start,
    // so that StartVmLoop can block until the event loop is ready
    // to accept connections (avoids race with fast runtime startup).
    std::mutex loop_ready_mutex;
    std::condition_variable loop_ready_cv;
    bool loop_ready = false;

    // Send queue: filled by any thread, drained on loop thread
    std::mutex send_mutex;
    std::deque<std::string> send_queue;

    // Receive buffer (used only on loop thread)
    std::string recv_pending;
    size_t recv_payload_needed = 0;
    ipc::Message recv_pending_msg;

    VmRuntimeHandle() = default;
    VmRuntimeHandle(const VmRuntimeHandle& o)
        : process_handle(o.process_handle),
          process_id(o.process_id),
          pipe_name(o.pipe_name),
          pipe_connected(o.pipe_connected) {}
    VmRuntimeHandle& operator=(const VmRuntimeHandle& o) {
        if (this != &o) {
            process_handle = o.process_handle;
            process_id = o.process_id;
            pipe_name = o.pipe_name;
            pipe_connected = o.pipe_connected;
        }
        return *this;
    }
    VmRuntimeHandle(VmRuntimeHandle&&) = default;
    VmRuntimeHandle& operator=(VmRuntimeHandle&&) = default;
};

struct VmRecord {
    VmSpec spec;
    VmPowerState state = VmPowerState::kStopped;
    std::optional<VmMutablePatch> pending_patch;
    VmRuntimeHandle runtime;
    int last_exit_code = 0;
    bool reboot_pending = false;
    bool guest_agent_connected = false;

    VmRecord() = default;
    VmRecord(VmSpec s) : spec(std::move(s)) {}
    VmRecord(const VmRecord&) = default;
    VmRecord& operator=(const VmRecord&) = default;
    VmRecord(VmRecord&&) = default;
    VmRecord& operator=(VmRecord&&) = default;
};

// Source paths the user picked in the "Create VM" dialog.
// These files will be copied into the new VM directory.
struct VmCreateRequest {
    std::string name;
    std::string source_kernel;
    std::string source_initrd;
    std::string source_disk;
    std::string cmdline;
    std::string storage_dir;   // empty = use default
    uint64_t memory_mb = 4096;
    uint32_t cpu_count = 4;
    bool debug_mode = false;
};

class ManagerService {
public:
    ManagerService(std::string runtime_exe_path, std::string data_dir);
    ~ManagerService();

    bool CreateVm(const VmCreateRequest& req, std::string* error);
    bool CloneVm(const std::string& vm_id, std::string* error);
    bool DeleteVm(const std::string& vm_id, std::string* error);
    bool EditVm(const std::string& vm_id, const VmMutablePatch& patch, std::string* error);
    bool StartVm(const std::string& vm_id, std::string* error);
    bool StopVm(const std::string& vm_id, std::string* error);
    bool RebootVm(const std::string& vm_id, std::string* error);
    bool ShutdownVm(const std::string& vm_id, std::string* error);
    void ShutdownAll();

    std::vector<VmRecord> ListVms() const;
    std::optional<VmRecord> GetVm(const std::string& vm_id) const;
    void ReorderVm(int from, int to);

    void set_hypervisor_available(bool available) { hypervisor_available_ = available; }
    bool hypervisor_available() const { return hypervisor_available_; }

    void set_global_guest_forwards(std::vector<GuestForward> gfs) {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        global_guest_forwards_ = std::move(gfs);
    }

    // Update guest forwards and broadcast to all running VMs.
    void UpdateGlobalGuestForwards(std::vector<GuestForward> gfs);

    const std::string& data_dir() const { return data_dir_; }
    settings::AppSettings& app_settings() { return settings_; }
    const settings::AppSettings& app_settings() const { return settings_; }

    void SaveAppSettings();

    using LlmProxyChangedCallback =
        std::function<void(const settings::LlmProxySettings& settings)>;
    void SetLlmProxyChangedCallback(LlmProxyChangedCallback cb);
    void NotifyLlmProxyChanged();

    using ConsoleCallback = std::function<void(const std::string& vm_id, const std::string& data)>;
    void SetConsoleCallback(ConsoleCallback cb);
    bool SendConsoleInput(const std::string& vm_id, const std::string& input);

    using StateChangeCallback = std::function<void(const std::string& vm_id)>;
    void SetStateChangeCallback(StateChangeCallback cb);

    using DisplayCallback = std::function<void(const std::string& vm_id, DisplayFrame frame)>;
    void SetDisplayCallback(DisplayCallback cb);

    using CursorCallback = std::function<void(const std::string& vm_id, const CursorInfo& cursor)>;
    void SetCursorCallback(CursorCallback cb);

    using DisplayStateCallback = std::function<void(const std::string& vm_id, bool active, uint32_t width, uint32_t height)>;
    void SetDisplayStateCallback(DisplayStateCallback cb);

    // Clipboard callbacks: events from VM to host
    using ClipboardGrabCallback = std::function<void(const std::string& vm_id,
        const std::vector<uint32_t>& types)>;
    using ClipboardDataCallback = std::function<void(const std::string& vm_id,
        uint32_t type, const std::vector<uint8_t>& data)>;
    using ClipboardRequestCallback = std::function<void(const std::string& vm_id,
        uint32_t type)>;

    void SetClipboardGrabCallback(ClipboardGrabCallback cb);
    void SetClipboardDataCallback(ClipboardDataCallback cb);
    void SetClipboardRequestCallback(ClipboardRequestCallback cb);

    // Audio PCM callback: audio data from VM to host
    using AudioPcmCallback = std::function<void(const std::string& vm_id,
        AudioChunk chunk)>;
    void SetAudioPcmCallback(AudioPcmCallback cb);

    // Guest Agent state callback
    using GuestAgentStateCallback = std::function<void(const std::string& vm_id, bool connected)>;
    void SetGuestAgentStateCallback(GuestAgentStateCallback cb);
    bool IsGuestAgentConnected(const std::string& vm_id) const;

    // Port forward error callback: when host ports fail to bind
    // failed_mappings format: "host_port:guest_port" for each failed binding
    using PortForwardErrorCallback = std::function<void(const std::string& vm_id,
        const std::vector<std::string>& failed_mappings)>;
    void SetPortForwardErrorCallback(PortForwardErrorCallback cb);

    bool SendKeyEvent(const std::string& vm_id, uint32_t key_code, bool pressed);
    bool SendPointerEvent(const std::string& vm_id, int32_t x, int32_t y, uint32_t buttons);
    bool SendWheelEvent(const std::string& vm_id, int32_t delta);
    bool SetDisplaySize(const std::string& vm_id, uint32_t width, uint32_t height);
    void SetVmDpiScaled(const std::string& vm_id, bool scaled);

    // Clipboard operations: host to VM
    bool SendClipboardGrab(const std::string& vm_id, const std::vector<uint32_t>& types);
    bool SendClipboardData(const std::string& vm_id, uint32_t type,
                           const uint8_t* data, size_t len);
    bool SendClipboardRequest(const std::string& vm_id, uint32_t type);
    bool SendClipboardRelease(const std::string& vm_id);

    // Shared folder management
    bool AddSharedFolder(const std::string& vm_id, const SharedFolder& folder, std::string* error);
    bool RemoveSharedFolder(const std::string& vm_id, const std::string& tag, std::string* error);
    std::vector<SharedFolder> GetSharedFolders(const std::string& vm_id) const;

    // Port forward management
    bool AddPortForward(const std::string& vm_id, const PortForward& forward, std::string* error);
    bool RemovePortForward(const std::string& vm_id, uint16_t host_port, std::string* error);
    std::vector<PortForward> GetPortForwards(const std::string& vm_id) const;

    // Per-VM guest forward management
    bool AddGuestForward(const std::string& vm_id, const GuestForward& gf, std::string* error);
    bool RemoveGuestForward(const std::string& vm_id, uint32_t guest_ip, uint16_t guest_port, std::string* error);
    std::vector<GuestForward> GetGuestForwards(const std::string& vm_id) const;

private:
    VmRecord* FindVm(const std::string& vm_id);
    const VmRecord* FindVm(const std::string& vm_id) const;
    bool EraseVm(const std::string& vm_id);

    bool SendRuntimeMessage(VmRecord& vm, const ipc::Message& msg);
    void CloseRuntime(VmRecord& vm);
    void ApplyPendingPatchLocked(VmRecord& vm);
    void LoadVms();
    void SaveVmPaths();
    void SaveVmPathsLocked();  // caller must hold vms_mutex_

    // libuv per-VM event loop management
    bool StartVmLoop(const std::string& vm_id, VmRecord& vm);
    void StopVmLoop(VmRecord& vm);
    void VmLoopThread(const std::string& vm_id, VmRuntimeHandle* rt);

    // libuv callbacks (static, pointer to ManagerService via handle->data)
    struct LoopContext {
        ManagerService* service;
        std::string vm_id;
    };
    static void OnConnection(uv_stream_t* server, int status);
    static void OnAllocBuffer(uv_handle_t* handle, size_t suggested, uv_buf_t* buf);
    static void OnPipeRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
    static void OnWriteDone(uv_write_t* req, int status);
    static void OnSendWakeup(uv_async_t* handle);
    static void OnStopSignal(uv_async_t* handle);

    void DispatchPipeData(VmRuntimeHandle& rt, const std::string& vm_id);
    void HandleProcessExit(const std::string& vm_id);
    void CleanupRuntimeHandles(VmRecord& vm);
    void HandleIncomingMessage(const std::string& vm_id, const ipc::Message& msg);

    void InitJobObject();

    std::string runtime_exe_path_;
    std::string data_dir_;
    settings::AppSettings settings_;
    std::map<std::string, VmRecord> vms_;
    std::vector<std::string> vm_order_;
    mutable std::mutex vms_mutex_;
    ConsoleCallback console_callback_;
    StateChangeCallback state_change_callback_;
    DisplayCallback display_callback_;
    CursorCallback cursor_callback_;
    DisplayStateCallback display_state_callback_;
    ClipboardGrabCallback clipboard_grab_callback_;
    ClipboardDataCallback clipboard_data_callback_;
    ClipboardRequestCallback clipboard_request_callback_;
    AudioPcmCallback audio_pcm_callback_;
    GuestAgentStateCallback guest_agent_state_callback_;
    PortForwardErrorCallback port_forward_error_callback_;
    LlmProxyChangedCallback llm_proxy_changed_callback_;
    bool hypervisor_available_ = true;
    void* job_object_ = nullptr;

    // Guest forwards injected into every VM (e.g. LLM proxy guestfwd)
    std::vector<GuestForward> global_guest_forwards_;

    void AppendGuestFwdFields(ipc::Message& msg,
                              const std::vector<GuestForward>& vm_guest_forwards = {}) const;

    // Per-VM shared-memory framebuffers for zero-copy display transport.
    std::map<std::string, std::unique_ptr<ipc::SharedFramebuffer>> shm_framebuffers_;
};
