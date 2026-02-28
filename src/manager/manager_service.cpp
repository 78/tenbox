#include "manager/manager_service.h"

#include "core/vmm/types.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::string EncodeHex(const uint8_t* data, size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.resize(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out[2 * i]     = kDigits[(data[i] >> 4) & 0x0F];
        out[2 * i + 1] = kDigits[data[i] & 0x0F];
    }
    return out;
}

std::string EncodeHex(const std::string& str) {
    return EncodeHex(reinterpret_cast<const uint8_t*>(str.data()), str.size());
}

std::string DecodeHex(const std::string& value) {
    auto hex = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
        if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
        return -1;
    };
    if ((value.size() % 2) != 0) return {};
    std::string out(value.size() / 2, '\0');
    for (size_t i = 0; i < out.size(); ++i) {
        int hi = hex(value[2 * i]);
        int lo = hex(value[2 * i + 1]);
        if (hi < 0 || lo < 0) return {};
        out[i] = static_cast<char>((hi << 4) | lo);
    }
    return out;
}

std::string BuildRuntimeCommand(const std::string& exe, const VmSpec& spec, const std::string& pipe) {
    std::ostringstream cmd;
    cmd << '"' << exe << '"'
        << " --vm-id " << spec.vm_id
        << " --control-endpoint " << pipe
        << " --interactive off"
        << " --kernel \"" << spec.kernel_path << "\"";
    if (!spec.initrd_path.empty()) {
        cmd << " --initrd \"" << spec.initrd_path << '"';
    }
    if (!spec.disk_path.empty()) {
        cmd << " --disk \"" << spec.disk_path << '"';
    }
    if (!spec.cmdline.empty()) {
        cmd << " --cmdline \"" << spec.cmdline << '"';
    }
    cmd << " --memory " << spec.memory_mb
        << " --cpus " << spec.cpu_count;
    if (spec.nat_enabled) {
        cmd << " --net";
    }
    for (const auto& forward : spec.port_forwards) {
        cmd << " --forward " << forward.host_port << ':' << forward.guest_port;
    }
    for (const auto& sf : spec.shared_folders) {
        cmd << " --share \"" << sf.tag << ':' << sf.host_path;
        if (sf.readonly) cmd << ":ro";
        cmd << '"';
    }
    return cmd.str();
}

bool CopyFileChecked(const std::string& src, const std::string& dst, std::string* error) {
    if (src.empty()) return true;
    std::error_code ec;
    if (!fs::exists(src, ec)) {
        if (error) *error = "source file not found: " + src;
        return false;
    }
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = "failed to copy " + src + " -> " + dst + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace

ManagerService::ManagerService(std::string runtime_exe_path, std::string data_dir)
    : runtime_exe_path_(std::move(runtime_exe_path)),
      data_dir_(std::move(data_dir)) {
    InitJobObject();
    settings_ = settings::LoadSettings(data_dir_);
    LoadVms();
}

ManagerService::~ManagerService() {
    ShutdownAll();
    if (job_object_) {
        CloseHandle(reinterpret_cast<HANDLE>(job_object_));
    }
}

void ManagerService::InitJobObject() {
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (!job) return;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                            &info, sizeof(info));
    job_object_ = job;
}

// ── VM lifecycle ─────────────────────────────────────────────────────

bool ManagerService::CreateVm(const VmCreateRequest& req, std::string* error) {
    if (req.source_kernel.empty()) {
        if (error) *error = "kernel path is required";
        return false;
    }

    std::string uuid = settings::GenerateUuid();
    std::string vm_dir = (fs::path(settings::DefaultVmStorageDir()) / uuid).string();

    std::error_code ec;
    fs::create_directories(vm_dir, ec);
    if (ec) {
        if (error) *error = "failed to create VM directory: " + ec.message();
        return false;
    }

    auto DstPath = [&](const std::string& src) -> std::string {
        if (src.empty()) return {};
        return (fs::path(vm_dir) / fs::path(src).filename()).string();
    };

    std::string dst_kernel = DstPath(req.source_kernel);
    std::string dst_initrd = DstPath(req.source_initrd);
    std::string dst_disk   = DstPath(req.source_disk);

    if (!CopyFileChecked(req.source_kernel, dst_kernel, error)) return false;
    if (!CopyFileChecked(req.source_initrd, dst_initrd, error)) return false;
    if (!CopyFileChecked(req.source_disk,   dst_disk,   error)) return false;

    VmSpec spec;
    spec.name        = req.name.empty() ? uuid : req.name;
    spec.vm_id       = uuid;
    spec.vm_dir      = vm_dir;
    spec.kernel_path = dst_kernel;
    spec.initrd_path = dst_initrd;
    spec.disk_path   = dst_disk;
    spec.cmdline     = req.cmdline.empty()
        ? "console=ttyS0 earlyprintk=serial lapic no_timer_check tsc=reliable"
        : req.cmdline;
    spec.memory_mb   = req.memory_mb;
    spec.cpu_count   = req.cpu_count;
    spec.nat_enabled = req.nat_enabled;

    settings::SaveVmManifest(spec);
    vms_.emplace(uuid, VmRecord{spec});
    SaveVmPaths();
    return true;
}

bool ManagerService::DeleteVm(const std::string& vm_id, std::string* error) {
    std::string vm_dir;
    std::thread read_thread_to_join;
    
    // Step 1: Stop VM if running (this acquires lock internally)
    {
        std::string stop_err;
        StopVm(vm_id, &stop_err);
    }

    // Step 2: Get VM directory and ensure read thread is stopped
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        auto it = vms_.find(vm_id);
        if (it == vms_.end()) {
            if (error) *error = "vm not found";
            return false;
        }
        
        vm_dir = it->second.spec.vm_dir;
        
        // Stop read thread if still running
        if (it->second.runtime.read_thread.joinable()) {
            it->second.runtime.read_running.store(false);
            if (it->second.runtime.pipe_handle) {
                CancelIoEx(reinterpret_cast<HANDLE>(it->second.runtime.pipe_handle), nullptr);
            }
            // Move thread out to join outside the lock
            read_thread_to_join = std::move(it->second.runtime.read_thread);
        }
        
        // Erase VM record
        vms_.erase(it);
    }
    
    // Step 3: Join read thread outside the lock (if needed)
    if (read_thread_to_join.joinable()) {
        read_thread_to_join.join();
    }

    SaveVmPaths();

    // Step 4: Delete VM directory
    if (!vm_dir.empty()) {
        std::error_code ec;
        fs::remove_all(vm_dir, ec);
        if (ec && error) {
            *error = "VM removed but failed to delete directory: " + ec.message();
            return false;
        }
    }
    return true;
}

bool ManagerService::EditVm(const std::string& vm_id, const VmMutablePatch& patch, std::string* error) {
    auto it = vms_.find(vm_id);
    if (it == vms_.end()) {
        if (error) *error = "vm not found";
        return false;
    }
    VmRecord& vm = it->second;
    const bool running = vm.state == VmPowerState::kRunning || vm.state == VmPowerState::kStarting;
    const bool has_offline_fields = patch.memory_mb.has_value() || patch.cpu_count.has_value();

    if (running && has_offline_fields && !patch.apply_on_next_boot) {
        if (error) *error = "cpu_count/memory_mb require powered off state";
        return false;
    }
    if (running && has_offline_fields && patch.apply_on_next_boot) {
        vm.pending_patch = patch;
    }

    if (patch.name) vm.spec.name = *patch.name;
    if (patch.nat_enabled) vm.spec.nat_enabled = *patch.nat_enabled;
    if (patch.port_forwards) vm.spec.port_forwards = *patch.port_forwards;
    if (patch.shared_folders) vm.spec.shared_folders = *patch.shared_folders;

    if (!running || patch.apply_on_next_boot) {
        if (patch.memory_mb) vm.spec.memory_mb = *patch.memory_mb;
        if (patch.cpu_count) vm.spec.cpu_count = *patch.cpu_count;
    }

    settings::SaveVmManifest(vm.spec);

    if (running && (patch.nat_enabled || patch.port_forwards)) {
        ipc::Message msg;
        msg.channel = ipc::Channel::kControl;
        msg.kind = ipc::Kind::kRequest;
        msg.type = "runtime.update_network";
        msg.vm_id = vm_id;
        msg.request_id = GetTickCount64();
        msg.fields["link_up"] = vm.spec.nat_enabled ? "true" : "false";
        msg.fields["forward_count"] = std::to_string(vm.spec.port_forwards.size());
        for (size_t i = 0; i < vm.spec.port_forwards.size(); ++i) {
            msg.fields["forward_" + std::to_string(i)] =
                std::to_string(vm.spec.port_forwards[i].host_port) + ":" +
                std::to_string(vm.spec.port_forwards[i].guest_port);
        }
        SendRuntimeMessage(vm, msg);
    }

    if (running && patch.shared_folders) {
        ipc::Message msg;
        msg.channel = ipc::Channel::kControl;
        msg.kind = ipc::Kind::kRequest;
        msg.type = "runtime.update_shared_folders";
        msg.vm_id = vm_id;
        msg.request_id = GetTickCount64();
        msg.fields["folder_count"] = std::to_string(vm.spec.shared_folders.size());
        for (size_t i = 0; i < vm.spec.shared_folders.size(); ++i) {
            const auto& f = vm.spec.shared_folders[i];
            msg.fields["folder_" + std::to_string(i)] =
                f.tag + "|" + f.host_path + "|" + (f.readonly ? "1" : "0");
        }
        SendRuntimeMessage(vm, msg);
    }

    return true;
}

bool ManagerService::StartVm(const std::string& vm_id, std::string* error) {
    auto it = vms_.find(vm_id);
    if (it == vms_.end()) {
        if (error) *error = "vm not found";
        return false;
    }
    VmRecord& vm = it->second;
    if (vm.state == VmPowerState::kRunning || vm.state == VmPowerState::kStarting) {
        return true;
    }

    vm.runtime.pipe_name = "tenbox_vm_" + vm.spec.vm_id;
    const std::string cmd = BuildRuntimeCommand(runtime_exe_path_, vm.spec, vm.runtime.pipe_name);

    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    std::vector<char> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back('\0');

    if (!CreateProcessA(
            nullptr,
            cmdline.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED,
            nullptr,
            nullptr,
            &si,
            &pi)) {
        if (error) *error = "failed to launch vm-runtime process";
        return false;
    }

    if (job_object_) {
        AssignProcessToJobObject(reinterpret_cast<HANDLE>(job_object_), pi.hProcess);
    }
    ResumeThread(pi.hThread);

    vm.runtime.process_handle = pi.hProcess;
    vm.runtime.process_id = pi.dwProcessId;
    CloseHandle(pi.hThread);
    vm.state = VmPowerState::kStarting;

    if (EnsurePipeConnected(vm)) {
        vm.state = VmPowerState::kRunning;
        StartReadThread(vm_id, vm);
    } else {
        vm.state = VmPowerState::kCrashed;
    }
    return vm.state == VmPowerState::kRunning;
}

bool ManagerService::StopVm(const std::string& vm_id, std::string* error) {
    HANDLE process_handle = nullptr;
    std::thread read_thread_to_join;
    HANDLE pipe_handle = nullptr;
    
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        auto it = vms_.find(vm_id);
        if (it == vms_.end()) {
            if (error) *error = "vm not found";
            return false;
        }
        VmRecord& vm = it->second;
        if (vm.state == VmPowerState::kStopped) return true;

        vm.state = VmPowerState::kStopping;
        process_handle = reinterpret_cast<HANDLE>(vm.runtime.process_handle);
        pipe_handle = reinterpret_cast<HANDLE>(vm.runtime.pipe_handle);
        
        ipc::Message msg;
        msg.channel = ipc::Channel::kControl;
        msg.kind = ipc::Kind::kRequest;
        msg.type = "runtime.command";
        msg.vm_id = vm_id;
        msg.request_id = GetTickCount64();
        msg.fields["command"] = "stop";
        SendRuntimeMessage(vm, msg);

        // Stop read thread: set flag and cancel IO (inside lock)
        if (vm.runtime.read_thread.joinable()) {
            vm.runtime.read_running.store(false);
            if (pipe_handle) {
                CancelIoEx(pipe_handle, nullptr);
            }
            // Move thread out to join outside the lock
            read_thread_to_join = std::move(vm.runtime.read_thread);
        }
    }

    // Wait for process and join read thread outside the lock
    if (process_handle) {
        WaitForSingleObject(process_handle, 3000);
    }
    if (read_thread_to_join.joinable()) {
        read_thread_to_join.join();
    }

    // Cleanup handles and update state (inside lock)
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        auto it = vms_.find(vm_id);
        if (it == vms_.end()) {
            if (error) *error = "vm not found";
            return false;
        }
        CleanupRuntimeHandles(it->second);
        it->second.state = VmPowerState::kStopped;
    }
    return true;
}

bool ManagerService::RebootVm(const std::string& vm_id, std::string* error) {
    if (!StopVm(vm_id, error)) return false;
    return StartVm(vm_id, error);
}

bool ManagerService::ShutdownVm(const std::string& vm_id, std::string* error) {
    auto it = vms_.find(vm_id);
    if (it == vms_.end()) {
        if (error) *error = "vm not found";
        return false;
    }
    VmRecord& vm = it->second;
    if (vm.state == VmPowerState::kStopped) return true;

    vm.state = VmPowerState::kStopping;
    ipc::Message msg;
    msg.channel = ipc::Channel::kControl;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "runtime.command";
    msg.vm_id = vm_id;
    msg.request_id = GetTickCount64();
    msg.fields["command"] = "shutdown";
    SendRuntimeMessage(vm, msg);
    return true;
}

void ManagerService::ShutdownAll() {
    for (auto& [id, vm] : vms_) {
        (void)id;
        std::string ignored;
        StopVm(vm.spec.vm_id, &ignored);
        if (vm.runtime.read_thread.joinable()) {
            vm.runtime.read_thread.join();
        }
    }
}

// ── Queries ──────────────────────────────────────────────────────────

std::vector<VmRecord> ManagerService::ListVms() const {
    std::vector<VmRecord> out;
    out.reserve(vms_.size());
    for (const auto& [id, vm] : vms_) {
        (void)id;
        out.push_back(vm);
    }
    return out;
}

std::optional<VmRecord> ManagerService::GetVm(const std::string& vm_id) const {
    auto it = vms_.find(vm_id);
    if (it == vms_.end()) return std::nullopt;
    return it->second;
}

// ── Settings persistence ─────────────────────────────────────────────

void ManagerService::SaveAppSettings() {
    settings::SaveSettings(data_dir_, settings_);
}

void ManagerService::SaveVmPaths() {
    settings_.vm_paths.clear();
    for (const auto& [id, vm] : vms_) {
        (void)id;
        settings_.vm_paths.push_back(vm.spec.vm_dir);
    }
    SaveAppSettings();
}

void ManagerService::LoadVms() {
    for (const auto& vm_path : settings_.vm_paths) {
        VmSpec spec;
        if (settings::LoadVmManifest(vm_path, spec) && !spec.vm_id.empty()) {
            // Avoid relying on argument evaluation order when moving `spec`.
            const auto vm_id = spec.vm_id;
            vms_.emplace(vm_id, VmRecord{std::move(spec)});
        }
    }
}

// ── Runtime IPC ──────────────────────────────────────────────────────

bool ManagerService::EnsurePipeConnected(VmRecord& vm) {
    if (vm.runtime.pipe_name.empty()) return false;
    if (vm.runtime.pipe_handle) return true;

    const std::string full = R"(\\.\pipe\)" + vm.runtime.pipe_name;
    for (int i = 0; i < 60; ++i) {
        if (WaitNamedPipeA(full.c_str(), 50)) {
            HANDLE pipe = CreateFileA(
                full.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                DWORD mode = PIPE_READMODE_BYTE;
                SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
                vm.runtime.pipe_handle = pipe;
                return true;
            }
        }
        Sleep(50);
    }
    return false;
}

bool ManagerService::SendRuntimeMessage(VmRecord& vm, const ipc::Message& msg) {
    if (!EnsurePipeConnected(vm)) return false;
    HANDLE pipe = reinterpret_cast<HANDLE>(vm.runtime.pipe_handle);
    std::string encoded = ipc::Encode(msg);
    DWORD written = 0;
    if (!WriteFile(pipe, encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr)) {
        return false;
    }
    return written == encoded.size();
}

void ManagerService::ApplyPendingPatchLocked(VmRecord& vm) {
    if (!vm.pending_patch) return;
    const auto patch = *vm.pending_patch;
    if (patch.memory_mb) vm.spec.memory_mb = *patch.memory_mb;
    if (patch.cpu_count) vm.spec.cpu_count = *patch.cpu_count;
    vm.pending_patch.reset();
}

void ManagerService::CleanupRuntimeHandles(VmRecord& vm) {
    if (vm.runtime.pipe_handle) {
        CloseHandle(reinterpret_cast<HANDLE>(vm.runtime.pipe_handle));
        vm.runtime.pipe_handle = nullptr;
    }
    if (vm.runtime.process_handle) {
        HANDLE proc = reinterpret_cast<HANDLE>(vm.runtime.process_handle);
        WaitForSingleObject(proc, 1000);
        DWORD exit_code = 0;
        GetExitCodeProcess(proc, &exit_code);
        vm.last_exit_code = static_cast<int>(exit_code);
        CloseHandle(proc);
        vm.runtime.process_handle = nullptr;
    }
    vm.runtime.read_running.store(false);
    bool had_patch = vm.pending_patch.has_value();
    ApplyPendingPatchLocked(vm);
    if (had_patch) settings::SaveVmManifest(vm.spec);
}

void ManagerService::CloseRuntime(VmRecord& vm) {
    StopReadThread(vm);
    CleanupRuntimeHandles(vm);
}

// ── Console I/O ──────────────────────────────────────────────────────

void ManagerService::SetConsoleCallback(ConsoleCallback cb) {
    console_callback_ = std::move(cb);
}

void ManagerService::SetStateChangeCallback(StateChangeCallback cb) {
    state_change_callback_ = std::move(cb);
}

void ManagerService::SetDisplayCallback(DisplayCallback cb) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    display_callback_ = std::move(cb);
}

void ManagerService::SetCursorCallback(CursorCallback cb) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    cursor_callback_ = std::move(cb);
}

void ManagerService::SetDisplayStateCallback(DisplayStateCallback cb) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    display_state_callback_ = std::move(cb);
}

void ManagerService::SetClipboardGrabCallback(ClipboardGrabCallback cb) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    clipboard_grab_callback_ = std::move(cb);
}

void ManagerService::SetClipboardDataCallback(ClipboardDataCallback cb) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    clipboard_data_callback_ = std::move(cb);
}

void ManagerService::SetClipboardRequestCallback(ClipboardRequestCallback cb) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    clipboard_request_callback_ = std::move(cb);
}

bool ManagerService::SendKeyEvent(const std::string& vm_id, uint32_t key_code, bool pressed) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        auto it = vms_.find(vm_id);
        if (it == vms_.end()) return false;
        pipe = reinterpret_cast<HANDLE>(it->second.runtime.pipe_handle);
    }
    if (!pipe || pipe == INVALID_HANDLE_VALUE) return false;

    ipc::Message msg;
    msg.channel = ipc::Channel::kInput;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "input.key_event";
    msg.vm_id = vm_id;
    msg.request_id = GetTickCount64();
    msg.fields["key_code"] = std::to_string(key_code);
    msg.fields["pressed"] = pressed ? "1" : "0";

    std::string encoded = ipc::Encode(msg);
    DWORD written = 0;
    return WriteFile(pipe, encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr)
        && written == encoded.size();
}

bool ManagerService::SendPointerEvent(const std::string& vm_id,
                                       int32_t x, int32_t y, uint32_t buttons) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        auto it = vms_.find(vm_id);
        if (it == vms_.end()) return false;
        pipe = reinterpret_cast<HANDLE>(it->second.runtime.pipe_handle);
    }
    if (!pipe || pipe == INVALID_HANDLE_VALUE) return false;

    ipc::Message msg;
    msg.channel = ipc::Channel::kInput;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "input.pointer_event";
    msg.vm_id = vm_id;
    msg.request_id = GetTickCount64();
    msg.fields["x"] = std::to_string(x);
    msg.fields["y"] = std::to_string(y);
    msg.fields["buttons"] = std::to_string(buttons);

    std::string encoded = ipc::Encode(msg);
    DWORD written = 0;
    return WriteFile(pipe, encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr)
        && written == encoded.size();
}

bool ManagerService::SendWheelEvent(const std::string& vm_id, int32_t delta) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        auto it = vms_.find(vm_id);
        if (it == vms_.end()) return false;
        pipe = reinterpret_cast<HANDLE>(it->second.runtime.pipe_handle);
    }
    if (!pipe || pipe == INVALID_HANDLE_VALUE) return false;

    ipc::Message msg;
    msg.channel = ipc::Channel::kInput;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "input.wheel_event";
    msg.vm_id = vm_id;
    msg.request_id = GetTickCount64();
    msg.fields["delta"] = std::to_string(delta);

    std::string encoded = ipc::Encode(msg);
    DWORD written = 0;
    return WriteFile(pipe, encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr)
        && written == encoded.size();
}

bool ManagerService::SetDisplaySize(const std::string& vm_id, uint32_t width, uint32_t height) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        auto it = vms_.find(vm_id);
        if (it == vms_.end()) return false;
        pipe = reinterpret_cast<HANDLE>(it->second.runtime.pipe_handle);
    }
    if (!pipe || pipe == INVALID_HANDLE_VALUE) return false;

    ipc::Message msg;
    msg.channel = ipc::Channel::kDisplay;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "display.set_size";
    msg.vm_id = vm_id;
    msg.request_id = GetTickCount64();
    msg.fields["width"] = std::to_string(width);
    msg.fields["height"] = std::to_string(height);

    std::string encoded = ipc::Encode(msg);
    DWORD written = 0;
    return WriteFile(pipe, encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr)
        && written == encoded.size();
}

bool ManagerService::SendClipboardGrab(const std::string& vm_id,
                                       const std::vector<uint32_t>& types) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        auto it = vms_.find(vm_id);
        if (it == vms_.end()) return false;
        pipe = reinterpret_cast<HANDLE>(it->second.runtime.pipe_handle);
    }
    if (!pipe || pipe == INVALID_HANDLE_VALUE) return false;

    ipc::Message msg;
    msg.channel = ipc::Channel::kClipboard;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "clipboard.grab";
    msg.vm_id = vm_id;
    msg.request_id = GetTickCount64();

    std::string types_str;
    for (size_t i = 0; i < types.size(); ++i) {
        if (i > 0) types_str += ",";
        types_str += std::to_string(types[i]);
    }
    msg.fields["types"] = types_str;

    std::string encoded = ipc::Encode(msg);
    DWORD written = 0;
    return WriteFile(pipe, encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr)
        && written == encoded.size();
}

bool ManagerService::SendClipboardData(const std::string& vm_id, uint32_t type,
                                       const uint8_t* data, size_t len) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        auto it = vms_.find(vm_id);
        if (it == vms_.end()) return false;
        pipe = reinterpret_cast<HANDLE>(it->second.runtime.pipe_handle);
    }
    if (!pipe || pipe == INVALID_HANDLE_VALUE) return false;

    ipc::Message msg;
    msg.channel = ipc::Channel::kClipboard;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "clipboard.data";
    msg.vm_id = vm_id;
    msg.request_id = GetTickCount64();
    msg.fields["data_type"] = std::to_string(type);
    if (data && len > 0) {
        msg.payload.assign(data, data + len);
    }

    std::string encoded = ipc::Encode(msg);
    DWORD written = 0;
    return WriteFile(pipe, encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr)
        && written == encoded.size();
}

bool ManagerService::SendClipboardRequest(const std::string& vm_id, uint32_t type) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        auto it = vms_.find(vm_id);
        if (it == vms_.end()) return false;
        pipe = reinterpret_cast<HANDLE>(it->second.runtime.pipe_handle);
    }
    if (!pipe || pipe == INVALID_HANDLE_VALUE) return false;

    ipc::Message msg;
    msg.channel = ipc::Channel::kClipboard;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "clipboard.request";
    msg.vm_id = vm_id;
    msg.request_id = GetTickCount64();
    msg.fields["data_type"] = std::to_string(type);

    std::string encoded = ipc::Encode(msg);
    DWORD written = 0;
    return WriteFile(pipe, encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr)
        && written == encoded.size();
}

bool ManagerService::SendClipboardRelease(const std::string& vm_id) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        auto it = vms_.find(vm_id);
        if (it == vms_.end()) return false;
        pipe = reinterpret_cast<HANDLE>(it->second.runtime.pipe_handle);
    }
    if (!pipe || pipe == INVALID_HANDLE_VALUE) return false;

    ipc::Message msg;
    msg.channel = ipc::Channel::kClipboard;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "clipboard.release";
    msg.vm_id = vm_id;
    msg.request_id = GetTickCount64();

    std::string encoded = ipc::Encode(msg);
    DWORD written = 0;
    return WriteFile(pipe, encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr)
        && written == encoded.size();
}

bool ManagerService::AddSharedFolder(const std::string& vm_id, const SharedFolder& folder, 
                                      std::string* error) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    auto it = vms_.find(vm_id);
    if (it == vms_.end()) {
        if (error) *error = "vm not found";
        return false;
    }
    VmRecord& vm = it->second;
    
    // Check for duplicate tag
    for (const auto& sf : vm.spec.shared_folders) {
        if (sf.tag == folder.tag) {
            if (error) *error = "shared folder with tag '" + folder.tag + "' already exists";
            return false;
        }
    }
    
    // Check host path exists
    DWORD attrs = GetFileAttributesA(folder.host_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        if (error) *error = "host path does not exist or is not a directory";
        return false;
    }
    
    vm.spec.shared_folders.push_back(folder);
    settings::SaveVmManifest(vm.spec);
    
    if (vm.state == VmPowerState::kRunning) {
        ipc::Message msg;
        msg.channel = ipc::Channel::kControl;
        msg.kind = ipc::Kind::kRequest;
        msg.type = "runtime.update_shared_folders";
        msg.vm_id = vm_id;
        msg.request_id = GetTickCount64();
        msg.fields["folder_count"] = std::to_string(vm.spec.shared_folders.size());
        for (size_t i = 0; i < vm.spec.shared_folders.size(); ++i) {
            const auto& f = vm.spec.shared_folders[i];
            msg.fields["folder_" + std::to_string(i)] =
                f.tag + "|" + f.host_path + "|" + (f.readonly ? "1" : "0");
        }
        SendRuntimeMessage(vm, msg);
    }
    
    return true;
}

bool ManagerService::RemoveSharedFolder(const std::string& vm_id, const std::string& tag, 
                                         std::string* error) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    auto it = vms_.find(vm_id);
    if (it == vms_.end()) {
        if (error) *error = "vm not found";
        return false;
    }
    VmRecord& vm = it->second;
    
    auto sf_it = std::find_if(vm.spec.shared_folders.begin(), vm.spec.shared_folders.end(),
                              [&tag](const SharedFolder& sf) { return sf.tag == tag; });
    if (sf_it == vm.spec.shared_folders.end()) {
        if (error) *error = "shared folder with tag '" + tag + "' not found";
        return false;
    }
    
    vm.spec.shared_folders.erase(sf_it);
    settings::SaveVmManifest(vm.spec);
    
    if (vm.state == VmPowerState::kRunning) {
        ipc::Message msg;
        msg.channel = ipc::Channel::kControl;
        msg.kind = ipc::Kind::kRequest;
        msg.type = "runtime.update_shared_folders";
        msg.vm_id = vm_id;
        msg.request_id = GetTickCount64();
        msg.fields["folder_count"] = std::to_string(vm.spec.shared_folders.size());
        for (size_t i = 0; i < vm.spec.shared_folders.size(); ++i) {
            const auto& f = vm.spec.shared_folders[i];
            msg.fields["folder_" + std::to_string(i)] =
                f.tag + "|" + f.host_path + "|" + (f.readonly ? "1" : "0");
        }
        SendRuntimeMessage(vm, msg);
    }
    
    return true;
}

std::vector<SharedFolder> ManagerService::GetSharedFolders(const std::string& vm_id) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(vms_mutex_));
    auto it = vms_.find(vm_id);
    if (it == vms_.end()) {
        return {};
    }
    return it->second.spec.shared_folders;
}

bool ManagerService::SendConsoleInput(const std::string& vm_id, const std::string& input) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        auto it = vms_.find(vm_id);
        if (it == vms_.end()) return false;
        pipe = reinterpret_cast<HANDLE>(it->second.runtime.pipe_handle);
    }
    if (!pipe || pipe == INVALID_HANDLE_VALUE) return false;

    ipc::Message msg;
    msg.channel = ipc::Channel::kConsole;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "console.input";
    msg.vm_id = vm_id;
    msg.request_id = GetTickCount64();
    msg.fields["data_hex"] = EncodeHex(input);

    std::string encoded = ipc::Encode(msg);
    DWORD written = 0;
    return WriteFile(pipe, encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr)
        && written == encoded.size();
}

void ManagerService::StartReadThread(const std::string& vm_id, VmRecord& vm) {
    if (vm.runtime.read_running.load()) return;
    if (vm.runtime.read_thread.joinable()) {
        vm.runtime.read_thread.join();
    }
    vm.runtime.read_running.store(true);
    vm.runtime.read_thread = std::thread(&ManagerService::PipeReadThreadFunc, this, vm_id);
}

void ManagerService::StopReadThread(VmRecord& vm) {
    vm.runtime.read_running.store(false);
    if (vm.runtime.pipe_handle) {
        CancelIoEx(reinterpret_cast<HANDLE>(vm.runtime.pipe_handle), nullptr);
    }
    if (vm.runtime.read_thread.joinable()) {
        vm.runtime.read_thread.join();
    }
}

void ManagerService::DispatchPipeData(std::string& pending, PipeParseState& parse,
                                      const std::string& vm_id) {
    while (!pending.empty()) {
        if (parse.payload_needed > 0) {
            if (pending.size() < parse.payload_needed) return;
            parse.pending_msg.payload.assign(
                reinterpret_cast<const uint8_t*>(pending.data()),
                reinterpret_cast<const uint8_t*>(pending.data()) + parse.payload_needed);
            pending.erase(0, parse.payload_needed);
            parse.payload_needed = 0;
            HandleIncomingMessage(vm_id, parse.pending_msg);
            continue;
        }

        size_t nl = pending.find('\n');
        if (nl == std::string::npos) return;
        std::string line = pending.substr(0, nl + 1);
        pending.erase(0, nl + 1);
        auto decoded = ipc::Decode(line);
        if (!decoded) continue;

        auto ps_it = decoded->fields.find("payload_size");
        if (ps_it != decoded->fields.end()) {
            parse.payload_needed = std::strtoull(ps_it->second.c_str(), nullptr, 10);
            decoded->fields.erase(ps_it);
            if (parse.payload_needed > 0) {
                parse.pending_msg = std::move(*decoded);
                continue;
            }
        }
        HandleIncomingMessage(vm_id, *decoded);
    }
}

void ManagerService::HandleProcessExit(const std::string& vm_id) {
    StateChangeCallback cb;
    bool needs_reboot = false;
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        auto it = vms_.find(vm_id);
        if (it == vms_.end()) return;
        if (it->second.state != VmPowerState::kRunning &&
            it->second.state != VmPowerState::kStopping) return;
        auto& vm = it->second;
        CleanupRuntimeHandles(vm);
        // Exit code 128 indicates a reboot request from the guest
        needs_reboot = vm.reboot_pending || (vm.last_exit_code == 128);
        vm.reboot_pending = false;
        vm.state = VmPowerState::kStopped;
        cb = state_change_callback_;
    }
    if (cb) cb(vm_id);

    if (needs_reboot) {
        LOG_INFO("VM %s requested reboot, restarting...", vm_id.c_str());
        // Must spawn a new thread because we're currently in the read_thread
        // and StartVm will try to join it (can't join self).
        std::thread([this, vm_id]() {
            StateChangeCallback cb_after;
            {
                std::lock_guard<std::mutex> lock(vms_mutex_);
                std::string error;
                if (!StartVm(vm_id, &error)) {
                    LOG_ERROR("Failed to restart VM %s: %s", vm_id.c_str(), error.c_str());
                    return;
                }
                cb_after = state_change_callback_;
            }
            if (cb_after) cb_after(vm_id);
        }).detach();
    }
}

void ManagerService::PipeReadThreadFunc(const std::string& vm_id) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE process = INVALID_HANDLE_VALUE;
    std::atomic<bool>* running_flag = nullptr;
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        auto it = vms_.find(vm_id);
        if (it == vms_.end()) return;
        pipe = reinterpret_cast<HANDLE>(it->second.runtime.pipe_handle);
        process = reinterpret_cast<HANDLE>(it->second.runtime.process_handle);
        running_flag = &it->second.runtime.read_running;
    }
    if (!pipe || pipe == INVALID_HANDLE_VALUE || !running_flag) return;

    std::array<char, 65536> buf{};
    std::string pending;
    PipeParseState parse;
    bool process_exited = false;
    DWORD idle_count = 0;

    while (running_flag->load()) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_INVALID_HANDLE) {
                process_exited = true;
                break;
            }
            Sleep(10);
            idle_count++;
        } else if (available == 0) {
            Sleep(10);
            idle_count++;
        } else {
            idle_count = 0;
            DWORD to_read = (std::min)(available, static_cast<DWORD>(buf.size()));
            DWORD bytes_read = 0;
            if (!ReadFile(pipe, buf.data(), to_read, &bytes_read, nullptr)) {
                DWORD err = GetLastError();
                if (err == ERROR_BROKEN_PIPE || err == ERROR_OPERATION_ABORTED) {
                    process_exited = true;
                    break;
                }
                continue;
            }
            if (bytes_read > 0) {
                pending.append(buf.data(), bytes_read);
                DispatchPipeData(pending, parse, vm_id);
            }
        }

        if (idle_count >= 50 && process && process != INVALID_HANDLE_VALUE) {
            DWORD exit_code = 0;
            if (GetExitCodeProcess(process, &exit_code) && exit_code != STILL_ACTIVE) {
                process_exited = true;
                break;
            }
            idle_count = 0;
        }
    }

    if (process_exited) {
        HandleProcessExit(vm_id);
    }
}

void ManagerService::HandleIncomingMessage(const std::string& vm_id, const ipc::Message& msg) {
    if (msg.channel == ipc::Channel::kConsole &&
        msg.kind == ipc::Kind::kEvent &&
        msg.type == "console.data") {
        auto it = msg.fields.find("data_hex");
        if (it != msg.fields.end()) {
            std::string data = DecodeHex(it->second);
            ConsoleCallback cb;
            {
                std::lock_guard<std::mutex> lock(vms_mutex_);
                cb = console_callback_;
            }
            if (cb) cb(vm_id, data);
        }
        return;
    }

    if (msg.channel == ipc::Channel::kDisplay &&
        msg.kind == ipc::Kind::kEvent &&
        msg.type == "display.frame") {
        DisplayFrame frame;
        auto get = [&](const char* key) -> uint32_t {
            auto it = msg.fields.find(key);
            return (it != msg.fields.end()) ? static_cast<uint32_t>(std::strtoul(it->second.c_str(), nullptr, 10)) : 0;
        };
        frame.width = get("width");
        frame.height = get("height");
        frame.stride = get("stride");
        frame.format = get("format");
        frame.resource_width = get("resource_width");
        frame.resource_height = get("resource_height");
        frame.dirty_x = get("dirty_x");
        frame.dirty_y = get("dirty_y");
        frame.pixels = msg.payload;

        DisplayCallback cb;
        {
            std::lock_guard<std::mutex> lock(vms_mutex_);
            cb = display_callback_;
        }
        if (cb) cb(vm_id, frame);
        return;
    }

    if (msg.channel == ipc::Channel::kDisplay &&
        msg.kind == ipc::Kind::kEvent &&
        msg.type == "display.cursor") {
        CursorInfo cursor;
        auto get = [&](const char* key) -> uint32_t {
            auto it = msg.fields.find(key);
            return (it != msg.fields.end()) ? static_cast<uint32_t>(std::strtoul(it->second.c_str(), nullptr, 10)) : 0;
        };
        auto get_signed = [&](const char* key) -> int32_t {
            auto it = msg.fields.find(key);
            return (it != msg.fields.end()) ? static_cast<int32_t>(std::strtol(it->second.c_str(), nullptr, 10)) : 0;
        };
        cursor.x = get_signed("x");
        cursor.y = get_signed("y");
        cursor.hot_x = get("hot_x");
        cursor.hot_y = get("hot_y");
        cursor.width = get("width");
        cursor.height = get("height");
        cursor.visible = (get("visible") != 0);
        cursor.image_updated = (get("image_updated") != 0);
        if (cursor.image_updated) {
            cursor.pixels = msg.payload;
        }

        CursorCallback cb;
        {
            std::lock_guard<std::mutex> lock(vms_mutex_);
            cb = cursor_callback_;
        }
        if (cb) cb(vm_id, cursor);
        return;
    }

    if (msg.channel == ipc::Channel::kDisplay &&
        msg.kind == ipc::Kind::kEvent &&
        msg.type == "display.state") {
        auto get = [&](const char* key) -> uint32_t {
            auto it = msg.fields.find(key);
            return (it != msg.fields.end()) ? static_cast<uint32_t>(std::strtoul(it->second.c_str(), nullptr, 10)) : 0;
        };
        auto it = msg.fields.find("active");
        bool active = (it != msg.fields.end() && it->second == "1");
        uint32_t width = get("width");
        uint32_t height = get("height");

        DisplayStateCallback cb;
        {
            std::lock_guard<std::mutex> lock(vms_mutex_);
            cb = display_state_callback_;
        }
        if (cb) cb(vm_id, active, width, height);
        return;
    }

    if (msg.channel == ipc::Channel::kControl &&
        msg.kind == ipc::Kind::kEvent &&
        msg.type == "runtime.state") {
        auto it = msg.fields.find("state");
        if (it != msg.fields.end()) {
            std::lock_guard<std::mutex> lock(vms_mutex_);
            auto vm_it = vms_.find(vm_id);
            if (vm_it != vms_.end()) {
                const std::string& state_str = it->second;
                if (state_str == "running") {
                    vm_it->second.state = VmPowerState::kRunning;
                } else if (state_str == "starting") {
                    vm_it->second.state = VmPowerState::kStarting;
                } else if (state_str == "stopped") {
                    vm_it->second.state = VmPowerState::kStopped;
                } else if (state_str == "crashed") {
                    vm_it->second.state = VmPowerState::kCrashed;
                } else if (state_str == "rebooting") {
                    vm_it->second.reboot_pending = true;
                }
            }
        }
        return;
    }

    // Clipboard events from VM
    if (msg.channel == ipc::Channel::kClipboard &&
        msg.kind == ipc::Kind::kEvent) {

        if (msg.type == "clipboard.grab") {
            auto it_types = msg.fields.find("types");
            if (it_types != msg.fields.end()) {
                std::vector<uint32_t> types;
                std::string types_str = it_types->second;
                size_t pos = 0;
                while (pos < types_str.size()) {
                    size_t comma = types_str.find(',', pos);
                    if (comma == std::string::npos) comma = types_str.size();
                    std::string num_str = types_str.substr(pos, comma - pos);
                    if (!num_str.empty()) {
                        types.push_back(static_cast<uint32_t>(std::strtoul(num_str.c_str(), nullptr, 10)));
                    }
                    pos = comma + 1;
                }

                ClipboardGrabCallback cb;
                {
                    std::lock_guard<std::mutex> lock(vms_mutex_);
                    cb = clipboard_grab_callback_;
                }
                if (cb) {
                    cb(vm_id, types);
                }
            }
            return;
        }

        if (msg.type == "clipboard.data") {
            auto it_type = msg.fields.find("data_type");
            if (it_type != msg.fields.end()) {
                uint32_t data_type = static_cast<uint32_t>(std::strtoul(it_type->second.c_str(), nullptr, 10));

                ClipboardDataCallback cb;
                {
                    std::lock_guard<std::mutex> lock(vms_mutex_);
                    cb = clipboard_data_callback_;
                }
                if (cb) cb(vm_id, data_type, msg.payload);
            }
            return;
        }

        if (msg.type == "clipboard.request") {
            auto it_type = msg.fields.find("data_type");
            if (it_type != msg.fields.end()) {
                uint32_t data_type = static_cast<uint32_t>(std::strtoul(it_type->second.c_str(), nullptr, 10));

                ClipboardRequestCallback cb;
                {
                    std::lock_guard<std::mutex> lock(vms_mutex_);
                    cb = clipboard_request_callback_;
                }
                if (cb) cb(vm_id, data_type);
            }
            return;
        }
    }
}
