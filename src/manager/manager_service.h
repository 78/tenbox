#pragma once

#include "common/ports.h"
#include "common/vm_model.h"
#include "ipc/protocol_v1.h"
#include "manager/app_settings.h"
#include "core/vdagent/vdagent_protocol.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct PipeParseState {
    size_t payload_needed = 0;
    ipc::Message pending_msg;
};

struct VmRuntimeHandle {
    void* process_handle = nullptr;
    uint32_t process_id = 0;
    void* pipe_handle = nullptr;
    std::string pipe_name;
    std::thread read_thread;
    std::atomic<bool> read_running{false};

    VmRuntimeHandle() = default;
    VmRuntimeHandle(const VmRuntimeHandle& o)
        : process_handle(o.process_handle),
          process_id(o.process_id),
          pipe_handle(o.pipe_handle),
          pipe_name(o.pipe_name),
          read_running(o.read_running.load()) {}
    VmRuntimeHandle& operator=(const VmRuntimeHandle& o) {
        if (this != &o) {
            process_handle = o.process_handle;
            process_id = o.process_id;
            pipe_handle = o.pipe_handle;
            pipe_name = o.pipe_name;
            read_running.store(o.read_running.load());
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
    uint64_t memory_mb = 4096;
    uint32_t cpu_count = 4;
    bool nat_enabled = false;
};

class ManagerService {
public:
    ManagerService(std::string runtime_exe_path, std::string data_dir);
    ~ManagerService();

    bool CreateVm(const VmCreateRequest& req, std::string* error);
    bool DeleteVm(const std::string& vm_id, std::string* error);
    bool EditVm(const std::string& vm_id, const VmMutablePatch& patch, std::string* error);
    bool StartVm(const std::string& vm_id, std::string* error);
    bool StopVm(const std::string& vm_id, std::string* error);
    bool RebootVm(const std::string& vm_id, std::string* error);
    bool ShutdownVm(const std::string& vm_id, std::string* error);
    void ShutdownAll();

    std::vector<VmRecord> ListVms() const;
    std::optional<VmRecord> GetVm(const std::string& vm_id) const;

    const std::string& data_dir() const { return data_dir_; }
    settings::AppSettings& app_settings() { return settings_; }

    void SaveAppSettings();

    using ConsoleCallback = std::function<void(const std::string& vm_id, const std::string& data)>;
    void SetConsoleCallback(ConsoleCallback cb);
    bool SendConsoleInput(const std::string& vm_id, const std::string& input);

    using StateChangeCallback = std::function<void(const std::string& vm_id)>;
    void SetStateChangeCallback(StateChangeCallback cb);

    using DisplayCallback = std::function<void(const std::string& vm_id, const DisplayFrame& frame)>;
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

    bool SendKeyEvent(const std::string& vm_id, uint32_t key_code, bool pressed);
    bool SendPointerEvent(const std::string& vm_id, int32_t x, int32_t y, uint32_t buttons);
    bool SetDisplaySize(const std::string& vm_id, uint32_t width, uint32_t height);

    // Clipboard operations: host to VM
    bool SendClipboardGrab(const std::string& vm_id, const std::vector<uint32_t>& types);
    bool SendClipboardData(const std::string& vm_id, uint32_t type,
                           const uint8_t* data, size_t len);
    bool SendClipboardRequest(const std::string& vm_id, uint32_t type);
    bool SendClipboardRelease(const std::string& vm_id);

private:
    bool SendRuntimeMessage(VmRecord& vm, const ipc::Message& msg);
    bool EnsurePipeConnected(VmRecord& vm);
    void CloseRuntime(VmRecord& vm);
    void ApplyPendingPatchLocked(VmRecord& vm);
    void LoadVms();
    void SaveVmPaths();
    void StartReadThread(const std::string& vm_id, VmRecord& vm);
    void StopReadThread(VmRecord& vm);
    void PipeReadThreadFunc(const std::string& vm_id);
    void DispatchPipeData(std::string& pending, PipeParseState& parse,
                         const std::string& vm_id);
    void HandleProcessExit(const std::string& vm_id);
    void CleanupRuntimeHandles(VmRecord& vm);
    void HandleIncomingMessage(const std::string& vm_id, const ipc::Message& msg);

    void InitJobObject();

    std::string runtime_exe_path_;
    std::string data_dir_;
    settings::AppSettings settings_;
    std::unordered_map<std::string, VmRecord> vms_;
    std::mutex vms_mutex_;
    ConsoleCallback console_callback_;
    StateChangeCallback state_change_callback_;
    DisplayCallback display_callback_;
    CursorCallback cursor_callback_;
    DisplayStateCallback display_state_callback_;
    ClipboardGrabCallback clipboard_grab_callback_;
    ClipboardDataCallback clipboard_data_callback_;
    ClipboardRequestCallback clipboard_request_callback_;
    void* job_object_ = nullptr;
};
