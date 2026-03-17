#include "manager/manager_service.h"

#include "core/vmm/types.h"
#include "manager/i18n.h"

// Override LOG macros to write to the manager log file instead of
// stdout/stderr, which are unavailable in a GUI subsystem process.
extern FILE* GetManagerLogFile();
#undef LOG_INFO
#undef LOG_WARN
#undef LOG_ERROR
#undef LOG_DEBUG
#define LOG_INFO(fmt, ...) do { \
    if (FILE* _f = GetManagerLogFile()) { \
        fprintf(_f, "[INFO]  " fmt "\r\n", ##__VA_ARGS__); fflush(_f); \
    } \
} while (0)
#define LOG_WARN(fmt, ...) do { \
    if (FILE* _f = GetManagerLogFile()) { \
        fprintf(_f, "[WARN]  " fmt "\r\n", ##__VA_ARGS__); fflush(_f); \
    } \
} while (0)
#define LOG_ERROR(fmt, ...) do { \
    if (FILE* _f = GetManagerLogFile()) { \
        fprintf(_f, "[ERROR] " fmt "\r\n", ##__VA_ARGS__); fflush(_f); \
    } \
} while (0)
#define LOG_DEBUG(fmt, ...) do { \
    if (FILE* _f = GetManagerLogFile()) { \
        fprintf(_f, "[DEBUG] " fmt "\r\n", ##__VA_ARGS__); fflush(_f); \
    } \
} while (0)

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
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

std::string BuildRuntimeCommand(const std::string& exe, const VmSpec& spec,
                                const std::string& pipe,
                                const std::vector<GuestForward>& guest_forwards = {}) {
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
    cmd << " --memory " << spec.memory_mb
        << " --cpus " << spec.cpu_count;
    cmd << " --net";
    if (spec.debug_mode) {
        cmd << " --debug";
    }
    // Port forwards are now configured dynamically after VM starts (via IPC)
    // to ensure uniform error feedback. The runtime CLI still supports --forward.
    for (const auto& gf : guest_forwards) {
        cmd << " --guestfwd " << gf.ToGuestfwd();
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

    for (auto& [id, vm] : vms_) {
        (void)id;
        StopVmLoop(vm);
    }

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

VmRecord* ManagerService::FindVm(const std::string& vm_id) {
    auto it = vms_.find(vm_id);
    return it != vms_.end() ? &it->second : nullptr;
}

const VmRecord* ManagerService::FindVm(const std::string& vm_id) const {
    auto it = vms_.find(vm_id);
    return it != vms_.end() ? &it->second : nullptr;
}

bool ManagerService::EraseVm(const std::string& vm_id) {
    vm_order_.erase(std::remove(vm_order_.begin(), vm_order_.end(), vm_id), vm_order_.end());
    return vms_.erase(vm_id) > 0;
}

// ── VM lifecycle ─────────────────────────────────────────────────────

bool ManagerService::CreateVm(const VmCreateRequest& req, std::string* error) {
    if (req.source_kernel.empty()) {
        if (error) *error = "kernel path is required";
        return false;
    }

    std::string uuid = settings::GenerateUuid();
    std::string base_dir = req.storage_dir.empty()
        ? settings::DefaultVmStorageDir() : req.storage_dir;
    std::string vm_dir = (fs::path(base_dir) / uuid).string();

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
    spec.cmdline     = req.cmdline;
    spec.memory_mb   = req.memory_mb;
    spec.cpu_count   = req.cpu_count;
    spec.nat_enabled = true;
    spec.debug_mode  = req.debug_mode;
    spec.creation_time = static_cast<int64_t>(std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now()));

    settings::SaveVmManifest(spec);
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        vms_.emplace(spec.vm_id, VmRecord{spec});
        vm_order_.push_back(spec.vm_id);
        SaveVmPathsLocked();
    }
    return true;
}

bool ManagerService::CloneVm(const std::string& vm_id, std::string* error) {
    VmSpec src_spec;
    std::string src_dir;
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        auto* rec = FindVm(vm_id);
        if (!rec) {
            if (error) *error = "VM not found";
            return false;
        }
        if (rec->state != VmPowerState::kStopped && rec->state != VmPowerState::kCrashed) {
            if (error) *error = "VM must be stopped before cloning";
            return false;
        }
        src_spec = rec->spec;
        src_dir = rec->spec.vm_dir;
    }

    // Generate a unique clone name by appending an incrementing number
    auto GenerateCloneName = [&](const std::string& base_name) -> std::string {
        std::string stem = base_name;
        int start_num = 2;
        // Strip trailing number to find the real stem (e.g. "My VM 3" -> "My VM", start from 4)
        if (!stem.empty()) {
            auto pos = stem.find_last_not_of("0123456789");
            if (pos != std::string::npos && pos + 1 < stem.size() && pos > 0 && stem[pos] == ' ') {
                int existing_num = std::atoi(stem.c_str() + pos + 1);
                if (existing_num > 0) {
                    stem = stem.substr(0, pos);
                    start_num = existing_num + 1;
                }
            }
        }

        std::lock_guard<std::mutex> lock(vms_mutex_);
        for (int n = start_num; ; ++n) {
            std::string candidate = stem + " " + std::to_string(n);
            bool conflict = false;
            for (const auto& [id, rec] : vms_) {
                if (rec.spec.name == candidate) { conflict = true; break; }
            }
            if (!conflict) return candidate;
        }
    };

    std::string new_uuid = settings::GenerateUuid();
    std::string parent_dir = settings::EffectiveVmStorageDir(settings_);
    std::string new_dir = (fs::path(parent_dir) / new_uuid).string();

    // Copy the entire VM directory
    std::error_code ec;
    fs::copy(src_dir, new_dir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = "failed to copy VM directory: " + ec.message();
        return false;
    }

    // Load the cloned manifest and update fields
    VmSpec new_spec;
    if (!settings::LoadVmManifest(new_dir, new_spec)) {
        if (error) *error = "failed to load cloned VM manifest";
        fs::remove_all(new_dir, ec);
        return false;
    }

    new_spec.name = GenerateCloneName(src_spec.name);
    new_spec.creation_time = static_cast<int64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    new_spec.last_boot_time = 0;
    // port_forwards would conflict on host ports, so clear them
    new_spec.port_forwards.clear();

    settings::SaveVmManifest(new_spec);

    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        vms_.emplace(new_spec.vm_id, VmRecord{new_spec});
        vm_order_.push_back(new_spec.vm_id);
        SaveVmPathsLocked();
    }
    return true;
}

bool ManagerService::DeleteVm(const std::string& vm_id, std::string* error) {
    std::string vm_dir;

    {
        std::string stop_err;
        StopVm(vm_id, &stop_err);
    }

    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        VmRecord* vm = FindVm(vm_id);
        if (!vm) {
            if (error) *error = "vm not found";
            return false;
        }

        vm_dir = vm->spec.vm_dir;
        EraseVm(vm_id);
    }

    SaveVmPaths();

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
    std::lock_guard<std::mutex> lock(vms_mutex_);
    VmRecord* vmp = FindVm(vm_id);
    if (!vmp) {
        if (error) *error = "vm not found";
        return false;
    }
    VmRecord& vm = *vmp;
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
    if (patch.debug_mode) vm.spec.debug_mode = *patch.debug_mode;
    if (patch.port_forwards) vm.spec.port_forwards = *patch.port_forwards;
    if (patch.shared_folders) vm.spec.shared_folders = *patch.shared_folders;

    if (!running || patch.apply_on_next_boot) {
        if (patch.memory_mb) vm.spec.memory_mb = *patch.memory_mb;
        if (patch.cpu_count) vm.spec.cpu_count = *patch.cpu_count;
    }

    settings::SaveVmManifest(vm.spec);

    if (running && (patch.port_forwards || patch.guest_forwards)) {
        ipc::Message msg;
        msg.channel = ipc::Channel::kControl;
        msg.kind = ipc::Kind::kRequest;
        msg.type = "runtime.update_network";
        msg.vm_id = vm_id;
        msg.request_id = GetTickCount64();
        msg.fields["link_up"] = "true";
        msg.fields["forward_count"] = std::to_string(vm.spec.port_forwards.size());
        for (size_t i = 0; i < vm.spec.port_forwards.size(); ++i) {
            msg.fields["forward_" + std::to_string(i)] =
                vm.spec.port_forwards[i].ToHostfwd();
        }
        AppendGuestFwdFields(msg, vm.spec.guest_forwards);
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
    if (!hypervisor_available_) {
        if (error) *error = i18n::tr(i18n::S::kErrHvNotEnabled);
        return false;
    }

    VmSpec spec_copy;
    std::string pipe_name;

    // StopVmLoop joins the loop thread which may acquire vms_mutex_ in its
    // callbacks, so it must be called outside the lock to avoid deadlock.
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        VmRecord* vmp = FindVm(vm_id);
        if (!vmp) {
            if (error) *error = i18n::tr(i18n::S::kErrVmNotFound);
            return false;
        }
        if (vmp->state == VmPowerState::kRunning || vmp->state == VmPowerState::kStarting) {
            return true;
        }
    }
    {
        VmRecord* vmp = nullptr;
        {
            std::lock_guard<std::mutex> lock(vms_mutex_);
            vmp = FindVm(vm_id);
            if (!vmp) {
                if (error) *error = i18n::tr(i18n::S::kErrVmNotFound);
                return false;
            }
        }
        StopVmLoop(*vmp);
    }
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        VmRecord* vmp = FindVm(vm_id);
        if (!vmp) {
            if (error) *error = i18n::tr(i18n::S::kErrVmNotFound);
            return false;
        }
        CleanupRuntimeHandles(*vmp);
        vmp->state = VmPowerState::kStarting;
        pipe_name = R"(\\.\pipe\tenbox_vm_)" + vmp->spec.vm_id.substr(0, 8)
                    + "_" + std::to_string(GetTickCount64());
        vmp->runtime.pipe_name = pipe_name;
        spec_copy = vmp->spec;
    }

    // Start the libuv loop thread which binds + listens the named pipe
    // BEFORE launching the runtime process, so the pipe exists when it connects.
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        VmRecord* vmp = FindVm(vm_id);
        if (!vmp) {
            if (error) *error = i18n::tr(i18n::S::kErrVmDisappearedDuringStart);
            return false;
        }
        if (!StartVmLoop(vm_id, *vmp)) {
            vmp->state = VmPowerState::kStopped;
            if (error) *error = i18n::tr(i18n::S::kErrIpcConnectionFailed);
            return false;
        }
    }

    std::vector<GuestForward> all_gf = global_guest_forwards_;
    all_gf.insert(all_gf.end(), spec_copy.guest_forwards.begin(), spec_copy.guest_forwards.end());
    const std::string cmd = BuildRuntimeCommand(runtime_exe_path_, spec_copy, pipe_name, all_gf);
    LOG_INFO("StartVm command: %s", cmd.c_str());

    int wide_len = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wide_cmd(wide_len);
    MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, wide_cmd.data(), wide_len);

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    HANDLE hLog = INVALID_HANDLE_VALUE;
    if (!spec_copy.vm_dir.empty()) {
        std::wstring log_path = (fs::path(spec_copy.vm_dir) / "runtime.log").wstring();
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        hLog = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hLog != INVALID_HANDLE_VALUE) {
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdOutput = hLog;
            si.hStdError  = hLog;
            si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
        }
    }

    BOOL ok = CreateProcessW(
            nullptr,
            wide_cmd.data(),
            nullptr,
            nullptr,
            hLog != INVALID_HANDLE_VALUE ? TRUE : FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi);

    if (hLog != INVALID_HANDLE_VALUE) {
        CloseHandle(hLog);
    }

    if (!ok) {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        VmRecord* vmp = FindVm(vm_id);
        if (vmp) {
            StopVmLoop(*vmp);
            vmp->state = VmPowerState::kStopped;
        }
        if (error) *error = i18n::tr(i18n::S::kErrLaunchRuntimeFailed);
        return false;
    }

    if (job_object_) {
        AssignProcessToJobObject(reinterpret_cast<HANDLE>(job_object_), pi.hProcess);
    }
    CloseHandle(pi.hThread);

    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        VmRecord* vmp = FindVm(vm_id);
        if (!vmp) {
            CloseHandle(pi.hProcess);
            if (error) *error = i18n::tr(i18n::S::kErrVmDisappearedDuringStart);
            return false;
        }
        vmp->runtime.process_handle = pi.hProcess;
        vmp->runtime.process_id = pi.dwProcessId;
        vmp->state = VmPowerState::kRunning;
        vmp->spec.last_boot_time = static_cast<int64_t>(std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now()));
        settings::SaveVmManifest(vmp->spec);
        return true;
    }
}

bool ManagerService::StopVm(const std::string& vm_id, std::string* error) {
    HANDLE process_handle = nullptr;
    bool crashed = false;

    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        VmRecord* vmp = FindVm(vm_id);
        if (!vmp) {
            if (error) *error = "vm not found";
            return false;
        }
        VmRecord& vm = *vmp;
        if (vm.state == VmPowerState::kStopped) return true;

        crashed = (vm.state == VmPowerState::kCrashed);
        vm.state = VmPowerState::kStopping;
        process_handle = reinterpret_cast<HANDLE>(vm.runtime.process_handle);

        if (!crashed && vm.runtime.pipe_connected) {
            ipc::Message msg;
            msg.channel = ipc::Channel::kControl;
            msg.kind = ipc::Kind::kRequest;
            msg.type = "runtime.command";
            msg.vm_id = vm_id;
            msg.request_id = GetTickCount64();
            msg.fields["command"] = "stop";
            SendRuntimeMessage(vm, msg);
        }
    }

    if (process_handle) {
        DWORD wait = WaitForSingleObject(process_handle, crashed ? 500 : 3000);
        if (wait == WAIT_TIMEOUT) {
            LOG_ERROR("VM %s runtime did not exit in time, terminating", vm_id.c_str());
            TerminateProcess(process_handle, 1);
            WaitForSingleObject(process_handle, 1000);
        }
    }

    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        VmRecord* vmp = FindVm(vm_id);
        if (!vmp) {
            if (error) *error = "vm not found";
            return false;
        }
        // StopVmLoop joins the loop thread (must be called without holding
        // the lock only if the loop callbacks need to re-acquire it, but
        // they do — so release and re-acquire).
    }

    // StopVmLoop needs to join the thread which may call HandleProcessExit
    // which acquires vms_mutex_, so call outside the lock.
    VmRecord* vmp = nullptr;
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        vmp = FindVm(vm_id);
        if (!vmp) {
            if (error) *error = "vm not found";
            return false;
        }
    }
    StopVmLoop(*vmp);

    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        VmRecord* vmp2 = FindVm(vm_id);
        if (!vmp2) {
            if (error) *error = "vm not found";
            return false;
        }
        CleanupRuntimeHandles(*vmp2);
        vmp2->state = VmPowerState::kStopped;
    }
    return true;
}

bool ManagerService::RebootVm(const std::string& vm_id, std::string* error) {
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        VmRecord* vmp = FindVm(vm_id);
        if (!vmp) {
            if (error) *error = "vm not found";
            return false;
        }
        if (vmp->state != VmPowerState::kStopped) {
            vmp->state = VmPowerState::kStopping;
            vmp->reboot_pending = true;
            ipc::Message msg;
            msg.channel = ipc::Channel::kControl;
            msg.kind = ipc::Kind::kRequest;
            msg.type = "runtime.command";
            msg.vm_id = vm_id;
            msg.request_id = GetTickCount64();
            msg.fields["command"] = "reboot";
            SendRuntimeMessage(*vmp, msg);
            return true;
        }
    }
    // VM is stopped -- start it (StartVm manages its own lock)
    return StartVm(vm_id, error);
}

bool ManagerService::ShutdownVm(const std::string& vm_id, std::string* error) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    VmRecord* vmp = FindVm(vm_id);
    if (!vmp) {
        if (error) *error = "vm not found";
        return false;
    }
    VmRecord& vm = *vmp;
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
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        ids = vm_order_;
    }
    for (const auto& id : ids) {
        std::string ignored;
        StopVm(id, &ignored);
    }
}

// ── Queries ──────────────────────────────────────────────────────────

std::vector<VmRecord> ManagerService::ListVms() const {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    std::vector<VmRecord> result;
    result.reserve(vm_order_.size());
    for (const auto& id : vm_order_) {
        auto it = vms_.find(id);
        if (it != vms_.end()) result.push_back(it->second);
    }
    return result;
}

std::optional<VmRecord> ManagerService::GetVm(const std::string& vm_id) const {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    const VmRecord* vm = FindVm(vm_id);
    if (!vm) return std::nullopt;
    return *vm;
}

// ── Settings persistence ─────────────────────────────────────────────

void ManagerService::SaveAppSettings() {
    settings::SaveSettings(data_dir_, settings_);
}

void ManagerService::SetLlmProxyChangedCallback(LlmProxyChangedCallback cb) {
    llm_proxy_changed_callback_ = std::move(cb);
}

void ManagerService::NotifyLlmProxyChanged() {
    SaveAppSettings();
    if (llm_proxy_changed_callback_)
        llm_proxy_changed_callback_(settings_.llm_proxy);
}

void ManagerService::AppendGuestFwdFields(ipc::Message& msg,
                                           const std::vector<GuestForward>& vm_guest_forwards) const {
    std::vector<GuestForward> merged;
    merged.insert(merged.end(), global_guest_forwards_.begin(), global_guest_forwards_.end());
    merged.insert(merged.end(), vm_guest_forwards.begin(), vm_guest_forwards.end());
    if (merged.empty()) return;
    msg.fields["guestfwd_count"] = std::to_string(merged.size());
    for (size_t i = 0; i < merged.size(); ++i) {
        msg.fields["guestfwd_" + std::to_string(i)] = merged[i].ToGuestfwd();
    }
}

void ManagerService::UpdateGlobalGuestForwards(std::vector<GuestForward> gfs) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    global_guest_forwards_ = std::move(gfs);

    for (auto& [id, vm] : vms_) {
        if (vm.state != VmPowerState::kRunning) continue;
        ipc::Message msg;
        msg.channel = ipc::Channel::kControl;
        msg.kind = ipc::Kind::kRequest;
        msg.type = "runtime.update_network";
        msg.vm_id = id;
        msg.request_id = GetTickCount64();
        msg.fields["link_up"] = "true";
        msg.fields["forward_count"] = std::to_string(vm.spec.port_forwards.size());
        for (size_t i = 0; i < vm.spec.port_forwards.size(); ++i) {
            msg.fields["forward_" + std::to_string(i)] =
                vm.spec.port_forwards[i].ToHostfwd();
        }
        AppendGuestFwdFields(msg, vm.spec.guest_forwards);
        SendRuntimeMessage(vm, msg);
    }
}

void ManagerService::SaveVmPathsLocked() {
    settings_.vm_paths.clear();
    settings_.vm_paths.reserve(vm_order_.size());
    for (const auto& id : vm_order_) {
        auto it = vms_.find(id);
        if (it != vms_.end())
            settings_.vm_paths.push_back(it->second.spec.vm_dir);
    }
    SaveAppSettings();
}

void ManagerService::SaveVmPaths() {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    SaveVmPathsLocked();
}

void ManagerService::ReorderVm(int from, int to) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    if (from < 0 || from >= static_cast<int>(vm_order_.size())) return;
    if (to < 0 || to >= static_cast<int>(vm_order_.size())) return;
    if (from == to) return;

    if (from < to) {
        std::rotate(vm_order_.begin() + from, vm_order_.begin() + from + 1, vm_order_.begin() + to + 1);
    } else {
        std::rotate(vm_order_.begin() + to, vm_order_.begin() + from, vm_order_.begin() + from + 1);
    }
    SaveVmPathsLocked();
}

void ManagerService::LoadVms() {
    for (const auto& vm_path : settings_.vm_paths) {
        VmSpec spec;
        if (settings::LoadVmManifest(vm_path, spec) && !spec.vm_id.empty()) {
            std::string id = spec.vm_id;
            vms_.emplace(id, VmRecord{std::move(spec)});
            vm_order_.push_back(std::move(id));
        }
    }
}

// ── Runtime IPC ──────────────────────────────────────────────────────

bool ManagerService::SendRuntimeMessage(VmRecord& vm, const ipc::Message& msg) {
    if (!vm.runtime.pipe_connected) return false;
    std::string encoded = ipc::Encode(msg);
    {
        std::lock_guard<std::mutex> lock(vm.runtime.send_mutex);
        vm.runtime.send_queue.push_back(std::move(encoded));
    }
    if (vm.runtime.loop_initialized) {
        uv_async_send(&vm.runtime.send_wakeup);
    }
    return true;
}

void ManagerService::ApplyPendingPatchLocked(VmRecord& vm) {
    if (!vm.pending_patch) return;
    const auto patch = *vm.pending_patch;
    if (patch.memory_mb) vm.spec.memory_mb = *patch.memory_mb;
    if (patch.cpu_count) vm.spec.cpu_count = *patch.cpu_count;
    vm.pending_patch.reset();
}

void ManagerService::CleanupRuntimeHandles(VmRecord& vm) {
    vm.runtime.pipe_connected = false;
    if (vm.runtime.process_handle) {
        HANDLE proc = reinterpret_cast<HANDLE>(vm.runtime.process_handle);
        DWORD wait = WaitForSingleObject(proc, 1000);
        if (wait == WAIT_TIMEOUT) {
            TerminateProcess(proc, 1);
            WaitForSingleObject(proc, 500);
        }
        DWORD exit_code = 0;
        GetExitCodeProcess(proc, &exit_code);
        vm.last_exit_code = static_cast<int>(exit_code);
        CloseHandle(proc);
        vm.runtime.process_handle = nullptr;
    }
    vm.runtime.process_id = 0;
    vm.runtime.recv_pending.clear();
    vm.runtime.recv_payload_needed = 0;
    vm.runtime.recv_pending_msg = {};
    {
        std::lock_guard<std::mutex> lock(vm.runtime.send_mutex);
        vm.runtime.send_queue.clear();
    }
    bool had_patch = vm.pending_patch.has_value();
    ApplyPendingPatchLocked(vm);
    if (had_patch) settings::SaveVmManifest(vm.spec);
}

void ManagerService::CloseRuntime(VmRecord& vm) {
    StopVmLoop(vm);
    CleanupRuntimeHandles(vm);
}

// ── Console I/O ──────────────────────────────────────────────────────

void ManagerService::SetConsoleCallback(ConsoleCallback cb) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    console_callback_ = std::move(cb);
}

void ManagerService::SetStateChangeCallback(StateChangeCallback cb) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
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

void ManagerService::SetAudioPcmCallback(AudioPcmCallback cb) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    audio_pcm_callback_ = std::move(cb);
}

void ManagerService::SetGuestAgentStateCallback(GuestAgentStateCallback cb) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    guest_agent_state_callback_ = std::move(cb);
}

void ManagerService::SetPortForwardErrorCallback(PortForwardErrorCallback cb) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    port_forward_error_callback_ = std::move(cb);
}

bool ManagerService::IsGuestAgentConnected(const std::string& vm_id) const {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    const VmRecord* vm = FindVm(vm_id);
    if (!vm) return false;
    return vm->guest_agent_connected;
}

bool ManagerService::SendKeyEvent(const std::string& vm_id, uint32_t key_code, bool pressed) {
    // try_lock: these are called from WndProc which can be re-entered while
    // the UI thread holds vms_mutex_ (e.g. WM_ACTIVATEAPP during WaitForSingleObject).
    std::unique_lock<std::mutex> lock(vms_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    VmRecord* vm = FindVm(vm_id);
    if (!vm) return false;

    ipc::Message msg;
    msg.channel = ipc::Channel::kInput;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "input.key_event";
    msg.vm_id = vm_id;
    msg.request_id = GetTickCount64();
    msg.fields["key_code"] = std::to_string(key_code);
    msg.fields["pressed"] = pressed ? "1" : "0";
    return SendRuntimeMessage(*vm, msg);
}

bool ManagerService::SendPointerEvent(const std::string& vm_id,
                                       int32_t x, int32_t y, uint32_t buttons) {
    std::unique_lock<std::mutex> lock(vms_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    VmRecord* vm = FindVm(vm_id);
    if (!vm) return false;

    ipc::Message msg;
    msg.channel = ipc::Channel::kInput;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "input.pointer_event";
    msg.vm_id = vm_id;
    msg.request_id = GetTickCount64();
    msg.fields["x"] = std::to_string(x);
    msg.fields["y"] = std::to_string(y);
    msg.fields["buttons"] = std::to_string(buttons);
    return SendRuntimeMessage(*vm, msg);
}

bool ManagerService::SendWheelEvent(const std::string& vm_id, int32_t delta) {
    std::unique_lock<std::mutex> lock(vms_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    VmRecord* vm = FindVm(vm_id);
    if (!vm) return false;

    ipc::Message msg;
    msg.channel = ipc::Channel::kInput;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "input.wheel_event";
    msg.vm_id = vm_id;
    msg.request_id = GetTickCount64();
    msg.fields["delta"] = std::to_string(delta);
    return SendRuntimeMessage(*vm, msg);
}

bool ManagerService::SetDisplaySize(const std::string& vm_id, uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    VmRecord* vm = FindVm(vm_id);
    if (!vm) return false;

    ipc::Message msg;
    msg.channel = ipc::Channel::kDisplay;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "display.set_size";
    msg.vm_id = vm_id;
    msg.request_id = GetTickCount64();
    msg.fields["width"] = std::to_string(width);
    msg.fields["height"] = std::to_string(height);
    return SendRuntimeMessage(*vm, msg);
}

void ManagerService::SetVmDpiScaled(const std::string& vm_id, bool scaled) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    VmRecord* vm = FindVm(vm_id);
    if (!vm) return;
    vm->spec.dpi_scaled = scaled;
    settings::SaveVmManifest(vm->spec);
}

bool ManagerService::SendClipboardGrab(const std::string& vm_id,
                                       const std::vector<uint32_t>& types) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    VmRecord* vm = FindVm(vm_id);
    if (!vm) return false;

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
    return SendRuntimeMessage(*vm, msg);
}

bool ManagerService::SendClipboardData(const std::string& vm_id, uint32_t type,
                                       const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    VmRecord* vm = FindVm(vm_id);
    if (!vm) return false;

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
    return SendRuntimeMessage(*vm, msg);
}

bool ManagerService::SendClipboardRequest(const std::string& vm_id, uint32_t type) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    VmRecord* vm = FindVm(vm_id);
    if (!vm) return false;

    ipc::Message msg;
    msg.channel = ipc::Channel::kClipboard;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "clipboard.request";
    msg.vm_id = vm_id;
    msg.request_id = GetTickCount64();
    msg.fields["data_type"] = std::to_string(type);
    return SendRuntimeMessage(*vm, msg);
}

bool ManagerService::SendClipboardRelease(const std::string& vm_id) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    VmRecord* vm = FindVm(vm_id);
    if (!vm) return false;

    ipc::Message msg;
    msg.channel = ipc::Channel::kClipboard;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "clipboard.release";
    msg.vm_id = vm_id;
    msg.request_id = GetTickCount64();
    return SendRuntimeMessage(*vm, msg);
}

bool ManagerService::AddSharedFolder(const std::string& vm_id, const SharedFolder& folder, 
                                      std::string* error) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    VmRecord* vmp = FindVm(vm_id);
    if (!vmp) {
        if (error) *error = "vm not found";
        return false;
    }
    VmRecord& vm = *vmp;
    
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
    VmRecord* vmp = FindVm(vm_id);
    if (!vmp) {
        if (error) *error = "vm not found";
        return false;
    }
    VmRecord& vm = *vmp;
    
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
    std::lock_guard<std::mutex> lock(vms_mutex_);
    const VmRecord* vm = FindVm(vm_id);
    if (!vm) {
        return {};
    }
    return vm->spec.shared_folders;
}

bool ManagerService::AddPortForward(const std::string& vm_id, const PortForward& forward,
                                     std::string* error) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    VmRecord* vmp = FindVm(vm_id);
    if (!vmp) {
        if (error) *error = "vm not found";
        return false;
    }
    VmRecord& vm = *vmp;

    if (forward.host_port == 0 || forward.guest_port == 0) {
        if (error) *error = "invalid port number";
        return false;
    }

    for (const auto& pf : vm.spec.port_forwards) {
        if (pf.host_port == forward.host_port) {
            if (error) *error = "host port " + std::to_string(forward.host_port) + " already in use";
            return false;
        }
    }

    vm.spec.port_forwards.push_back(forward);
    settings::SaveVmManifest(vm.spec);

    if (vm.state == VmPowerState::kRunning) {
        ipc::Message msg;
        msg.channel = ipc::Channel::kControl;
        msg.kind = ipc::Kind::kRequest;
        msg.type = "runtime.update_network";
        msg.vm_id = vm_id;
        msg.request_id = GetTickCount64();
        msg.fields["link_up"] = "true";
        msg.fields["forward_count"] = std::to_string(vm.spec.port_forwards.size());
        for (size_t i = 0; i < vm.spec.port_forwards.size(); ++i) {
            msg.fields["forward_" + std::to_string(i)] =
                vm.spec.port_forwards[i].ToHostfwd();
        }
        AppendGuestFwdFields(msg, vm.spec.guest_forwards);
        SendRuntimeMessage(vm, msg);
    }

    return true;
}

bool ManagerService::RemovePortForward(const std::string& vm_id, uint16_t host_port,
                                        std::string* error) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    VmRecord* vmp = FindVm(vm_id);
    if (!vmp) {
        if (error) *error = "vm not found";
        return false;
    }
    VmRecord& vm = *vmp;

    auto pf_it = std::find_if(vm.spec.port_forwards.begin(), vm.spec.port_forwards.end(),
                              [host_port](const PortForward& pf) { return pf.host_port == host_port; });
    if (pf_it == vm.spec.port_forwards.end()) {
        if (error) *error = "port forward with host port " + std::to_string(host_port) + " not found";
        return false;
    }

    vm.spec.port_forwards.erase(pf_it);
    settings::SaveVmManifest(vm.spec);

    if (vm.state == VmPowerState::kRunning) {
        ipc::Message msg;
        msg.channel = ipc::Channel::kControl;
        msg.kind = ipc::Kind::kRequest;
        msg.type = "runtime.update_network";
        msg.vm_id = vm_id;
        msg.request_id = GetTickCount64();
        msg.fields["link_up"] = "true";
        msg.fields["forward_count"] = std::to_string(vm.spec.port_forwards.size());
        for (size_t i = 0; i < vm.spec.port_forwards.size(); ++i) {
            msg.fields["forward_" + std::to_string(i)] =
                vm.spec.port_forwards[i].ToHostfwd();
        }
        AppendGuestFwdFields(msg, vm.spec.guest_forwards);
        SendRuntimeMessage(vm, msg);
    }

    return true;
}

std::vector<PortForward> ManagerService::GetPortForwards(const std::string& vm_id) const {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    const VmRecord* vm = FindVm(vm_id);
    if (!vm) {
        return {};
    }
    return vm->spec.port_forwards;
}

bool ManagerService::AddGuestForward(const std::string& vm_id, const GuestForward& gf,
                                      std::string* error) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    VmRecord* vmp = FindVm(vm_id);
    if (!vmp) {
        if (error) *error = "vm not found";
        return false;
    }
    VmRecord& vm = *vmp;

    if (gf.guest_ip == 0 || gf.guest_port == 0 || gf.host_port == 0) {
        if (error) *error = "invalid guest forward parameters";
        return false;
    }

    for (const auto& existing : vm.spec.guest_forwards) {
        if (existing.guest_ip == gf.guest_ip && existing.guest_port == gf.guest_port) {
            if (error) *error = "guest forward " + GuestForward::Ip4ToString(gf.guest_ip) +
                                ":" + std::to_string(gf.guest_port) + " already exists";
            return false;
        }
    }

    vm.spec.guest_forwards.push_back(gf);
    settings::SaveVmManifest(vm.spec);

    if (vm.state == VmPowerState::kRunning) {
        ipc::Message msg;
        msg.channel = ipc::Channel::kControl;
        msg.kind = ipc::Kind::kRequest;
        msg.type = "runtime.update_network";
        msg.vm_id = vm_id;
        msg.request_id = GetTickCount64();
        msg.fields["link_up"] = "true";
        msg.fields["forward_count"] = std::to_string(vm.spec.port_forwards.size());
        for (size_t i = 0; i < vm.spec.port_forwards.size(); ++i) {
            msg.fields["forward_" + std::to_string(i)] =
                vm.spec.port_forwards[i].ToHostfwd();
        }
        AppendGuestFwdFields(msg, vm.spec.guest_forwards);
        SendRuntimeMessage(vm, msg);
    }

    return true;
}

bool ManagerService::RemoveGuestForward(const std::string& vm_id, uint32_t guest_ip,
                                         uint16_t guest_port, std::string* error) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    VmRecord* vmp = FindVm(vm_id);
    if (!vmp) {
        if (error) *error = "vm not found";
        return false;
    }
    VmRecord& vm = *vmp;

    auto it = std::find_if(vm.spec.guest_forwards.begin(), vm.spec.guest_forwards.end(),
                           [guest_ip, guest_port](const GuestForward& g) {
                               return g.guest_ip == guest_ip && g.guest_port == guest_port;
                           });
    if (it == vm.spec.guest_forwards.end()) {
        if (error) *error = "guest forward not found";
        return false;
    }

    vm.spec.guest_forwards.erase(it);
    settings::SaveVmManifest(vm.spec);

    if (vm.state == VmPowerState::kRunning) {
        ipc::Message msg;
        msg.channel = ipc::Channel::kControl;
        msg.kind = ipc::Kind::kRequest;
        msg.type = "runtime.update_network";
        msg.vm_id = vm_id;
        msg.request_id = GetTickCount64();
        msg.fields["link_up"] = "true";
        msg.fields["forward_count"] = std::to_string(vm.spec.port_forwards.size());
        for (size_t i = 0; i < vm.spec.port_forwards.size(); ++i) {
            msg.fields["forward_" + std::to_string(i)] =
                vm.spec.port_forwards[i].ToHostfwd();
        }
        AppendGuestFwdFields(msg, vm.spec.guest_forwards);
        SendRuntimeMessage(vm, msg);
    }

    return true;
}

std::vector<GuestForward> ManagerService::GetGuestForwards(const std::string& vm_id) const {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    const VmRecord* vm = FindVm(vm_id);
    if (!vm) return {};
    return vm->spec.guest_forwards;
}

bool ManagerService::SendConsoleInput(const std::string& vm_id, const std::string& input) {
    std::lock_guard<std::mutex> lock(vms_mutex_);
    VmRecord* vm = FindVm(vm_id);
    if (!vm) return false;

    ipc::Message msg;
    msg.channel = ipc::Channel::kConsole;
    msg.kind = ipc::Kind::kRequest;
    msg.type = "console.input";
    msg.vm_id = vm_id;
    msg.request_id = GetTickCount64();
    msg.fields["data_hex"] = EncodeHex(input);
    return SendRuntimeMessage(*vm, msg);
}

void ManagerService::DispatchPipeData(VmRuntimeHandle& rt, const std::string& vm_id) {
    while (!rt.recv_pending.empty()) {
        if (rt.recv_payload_needed > 0) {
            if (rt.recv_pending.size() < rt.recv_payload_needed) return;
            rt.recv_pending_msg.payload.assign(
                reinterpret_cast<const uint8_t*>(rt.recv_pending.data()),
                reinterpret_cast<const uint8_t*>(rt.recv_pending.data()) + rt.recv_payload_needed);
            rt.recv_pending.erase(0, rt.recv_payload_needed);
            rt.recv_payload_needed = 0;
            HandleIncomingMessage(vm_id, rt.recv_pending_msg);
            continue;
        }

        size_t nl = rt.recv_pending.find('\n');
        if (nl == std::string::npos) return;
        std::string line = rt.recv_pending.substr(0, nl + 1);
        rt.recv_pending.erase(0, nl + 1);
        auto decoded = ipc::Decode(line);
        if (!decoded) continue;

        auto ps_it = decoded->fields.find("payload_size");
        if (ps_it != decoded->fields.end()) {
            rt.recv_payload_needed = std::strtoull(ps_it->second.c_str(), nullptr, 10);
            decoded->fields.erase(ps_it);
            if (rt.recv_payload_needed > 0) {
                rt.recv_pending_msg = std::move(*decoded);
                continue;
            }
        }
        HandleIncomingMessage(vm_id, *decoded);
    }
}

void ManagerService::HandleProcessExit(const std::string& vm_id) {
    // Called from the libuv read callback on the loop thread.
    // We only update state here; actual handle cleanup happens in StopVm.
    StateChangeCallback cb;
    bool needs_reboot = false;
    {
        std::lock_guard<std::mutex> lock(vms_mutex_);
        VmRecord* vmp = FindVm(vm_id);
        if (!vmp) return;
        if (vmp->state == VmPowerState::kStopped) return;
        VmRecord& vm = *vmp;
        bool was_crashed = (vm.state == VmPowerState::kCrashed);

        // Read exit code from the process handle (without closing it yet).
        if (vm.runtime.process_handle) {
            HANDLE proc = reinterpret_cast<HANDLE>(vm.runtime.process_handle);
            WaitForSingleObject(proc, 500);
            DWORD ec = 0;
            GetExitCodeProcess(proc, &ec);
            vm.last_exit_code = static_cast<int>(ec);
        }

        needs_reboot = vm.reboot_pending || (vm.last_exit_code == 128);
        vm.reboot_pending = false;
        vm.guest_agent_connected = false;
        if (!was_crashed) {
            vm.state = (vm.last_exit_code != 0)
                ? VmPowerState::kCrashed : VmPowerState::kStopped;
        }
        shm_framebuffers_.erase(vm_id);
        cb = state_change_callback_;
    }
    if (cb) cb(vm_id);

    if (needs_reboot) {
        LOG_INFO("VM %s requested reboot, restarting...", vm_id.c_str());
        // StartVm is called on a separate thread because it will join the
        // current loop_thread (can't join self).
        std::thread([this, vm_id]() {
            std::string error;
            if (!StartVm(vm_id, &error)) {
                LOG_ERROR("Failed to restart VM %s: %s", vm_id.c_str(), error.c_str());
                return;
            }
            StateChangeCallback cb_after;
            {
                std::lock_guard<std::mutex> lock(vms_mutex_);
                cb_after = state_change_callback_;
            }
            if (cb_after) cb_after(vm_id);
        }).detach();
    }
}

// ── libuv per-VM event loop ──────────────────────────────────────────

static void MgrCloseWalkCb(uv_handle_t* handle, void*) {
    if (!uv_is_closing(handle)) uv_close(handle, nullptr);
}

bool ManagerService::StartVmLoop(const std::string& vm_id, VmRecord& vm) {
    if (vm.runtime.loop_initialized) return true;

    uv_loop_init(&vm.runtime.loop);
    vm.runtime.loop_initialized = true;

    auto* ctx = new LoopContext{this, vm_id};

    vm.runtime.server_pipe.data = ctx;
    uv_pipe_init(&vm.runtime.loop, &vm.runtime.server_pipe, 0);

    vm.runtime.send_wakeup.data = ctx;
    uv_async_init(&vm.runtime.loop, &vm.runtime.send_wakeup, OnSendWakeup);

    vm.runtime.stop_wakeup.data = ctx;
    uv_async_init(&vm.runtime.loop, &vm.runtime.stop_wakeup, OnStopSignal);

    const std::string& pipe_path = vm.runtime.pipe_name;
    int r = uv_pipe_bind(&vm.runtime.server_pipe, pipe_path.c_str());
    if (r < 0) {
        LOG_ERROR("uv_pipe_bind failed for %s: %s", pipe_path.c_str(), uv_strerror(r));
        uv_walk(&vm.runtime.loop, MgrCloseWalkCb, nullptr);
        uv_run(&vm.runtime.loop, UV_RUN_DEFAULT);
        uv_loop_close(&vm.runtime.loop);
        vm.runtime.loop_initialized = false;
        delete ctx;
        return false;
    }

    r = uv_listen(reinterpret_cast<uv_stream_t*>(&vm.runtime.server_pipe), 1, OnConnection);
    if (r < 0) {
        LOG_ERROR("uv_listen failed for %s: %s", pipe_path.c_str(), uv_strerror(r));
        uv_walk(&vm.runtime.loop, MgrCloseWalkCb, nullptr);
        uv_run(&vm.runtime.loop, UV_RUN_DEFAULT);
        uv_loop_close(&vm.runtime.loop);
        vm.runtime.loop_initialized = false;
        delete ctx;
        return false;
    }

    vm.runtime.loop_ready = false;
    vm.runtime.loop_thread = std::thread(&ManagerService::VmLoopThread, this, vm_id, &vm.runtime);

    // Wait until the event loop is actually spinning so that the named pipe
    // is fully ready to accept connections before we launch the runtime.
    // NOTE: this wait uses loop_ready_mutex (not vms_mutex_), so the loop
    // thread can proceed without contending for vms_mutex_ before uv_run.
    {
        std::unique_lock<std::mutex> lk(vm.runtime.loop_ready_mutex);
        vm.runtime.loop_ready_cv.wait(lk, [&] { return vm.runtime.loop_ready; });
    }
    return true;
}

void ManagerService::StopVmLoop(VmRecord& vm) {
    if (!vm.runtime.loop_initialized) return;
    uv_async_send(&vm.runtime.stop_wakeup);
    if (vm.runtime.loop_thread.joinable()) {
        vm.runtime.loop_thread.join();
    }
    vm.runtime.loop_initialized = false;
    vm.runtime.pipe_connected = false;
    vm.runtime.loop_ready = false;
}

void ManagerService::VmLoopThread(const std::string& vm_id, VmRuntimeHandle* rt) {
    // Use a one-shot uv_prepare handle to signal that the event loop is
    // spinning and ready to accept pipe connections.  The prepare callback
    // fires at the very beginning of the first loop iteration — after that
    // we close it so it doesn't keep firing.
    uv_prepare_t ready_signal;
    ready_signal.data = rt;
    uv_prepare_init(&rt->loop, &ready_signal);
    uv_prepare_start(&ready_signal, [](uv_prepare_t* h) {
        auto* handle = static_cast<VmRuntimeHandle*>(h->data);
        {
            std::lock_guard<std::mutex> lock(handle->loop_ready_mutex);
            handle->loop_ready = true;
        }
        handle->loop_ready_cv.notify_one();
        uv_prepare_stop(h);
        uv_close(reinterpret_cast<uv_handle_t*>(h), nullptr);
    });

    uv_run(&rt->loop, UV_RUN_DEFAULT);

    auto* ctx = static_cast<LoopContext*>(rt->server_pipe.data);
    uv_loop_close(&rt->loop);
    delete ctx;
}

void ManagerService::OnConnection(uv_stream_t* server, int status) {
    auto* ctx = static_cast<LoopContext*>(server->data);
    if (status < 0) {
        LOG_ERROR("OnConnection error: %s", uv_strerror(status));
        return;
    }

    VmRuntimeHandle* rt = nullptr;
    {
        std::lock_guard<std::mutex> lock(ctx->service->vms_mutex_);
        VmRecord* vm = ctx->service->FindVm(ctx->vm_id);
        if (!vm) return;
        rt = &vm->runtime;
    }

    if (rt->pipe_connected) return;

    rt->client_pipe.data = ctx;
    uv_pipe_init(&rt->loop, &rt->client_pipe, 0);

    if (uv_accept(server, reinterpret_cast<uv_stream_t*>(&rt->client_pipe)) == 0) {
        rt->pipe_connected = true;
        uv_read_start(reinterpret_cast<uv_stream_t*>(&rt->client_pipe),
                       OnAllocBuffer, OnPipeRead);
    } else {
        uv_close(reinterpret_cast<uv_handle_t*>(&rt->client_pipe), nullptr);
    }
}

void ManagerService::OnAllocBuffer(uv_handle_t*, size_t suggested, uv_buf_t* buf) {
    buf->base = new char[suggested];
    buf->len = static_cast<decltype(buf->len)>(suggested);
}

void ManagerService::OnPipeRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    auto* ctx = static_cast<LoopContext*>(stream->data);

    if (nread < 0) {
        delete[] buf->base;
        uv_read_stop(stream);

        VmRuntimeHandle* rt = nullptr;
        {
            std::lock_guard<std::mutex> lock(ctx->service->vms_mutex_);
            VmRecord* vm = ctx->service->FindVm(ctx->vm_id);
            if (vm) {
                rt = &vm->runtime;
                rt->pipe_connected = false;
            }
        }
        ctx->service->HandleProcessExit(ctx->vm_id);
        return;
    }
    if (nread == 0) {
        delete[] buf->base;
        return;
    }

    VmRuntimeHandle* rt = nullptr;
    {
        std::lock_guard<std::mutex> lock(ctx->service->vms_mutex_);
        VmRecord* vm = ctx->service->FindVm(ctx->vm_id);
        if (!vm) { delete[] buf->base; return; }
        rt = &vm->runtime;
    }

    rt->recv_pending.append(buf->base, static_cast<size_t>(nread));
    delete[] buf->base;

    ctx->service->DispatchPipeData(*rt, ctx->vm_id);
}

void ManagerService::OnWriteDone(uv_write_t* req, int) {
    delete static_cast<std::string*>(req->data);
    delete req;
}

void ManagerService::OnSendWakeup(uv_async_t* handle) {
    auto* ctx = static_cast<LoopContext*>(handle->data);

    VmRuntimeHandle* rt = nullptr;
    {
        std::lock_guard<std::mutex> lock(ctx->service->vms_mutex_);
        VmRecord* vm = ctx->service->FindVm(ctx->vm_id);
        if (!vm) return;
        rt = &vm->runtime;
    }
    if (!rt->pipe_connected) return;

    std::deque<std::string> msgs;
    {
        std::lock_guard<std::mutex> lock(rt->send_mutex);
        msgs.swap(rt->send_queue);
    }

    std::string batch;
    for (auto& m : msgs) {
        batch += std::move(m);
    }
    if (batch.empty()) return;

    auto* write_data = new std::string(std::move(batch));
    auto* req = new uv_write_t;
    req->data = write_data;
    uv_buf_t uv_buf = uv_buf_init(const_cast<char*>(write_data->data()),
                                    static_cast<unsigned int>(write_data->size()));
    uv_write(req, reinterpret_cast<uv_stream_t*>(&rt->client_pipe),
             &uv_buf, 1, OnWriteDone);
}

void ManagerService::OnStopSignal(uv_async_t* handle) {
    // Don't acquire vms_mutex_ here — the caller (StopVmLoop) may already
    // hold it, and join()ing this thread while holding the lock would deadlock.
    // pipe_connected is only read on the loop thread after this point, and
    // StopVmLoop sets it to false after join() returns.
    uv_walk(handle->loop, MgrCloseWalkCb, nullptr);
}

void ManagerService::HandleIncomingMessage(const std::string& vm_id, const ipc::Message& msg) {
    if (msg.channel == ipc::Channel::kControl &&
        msg.kind == ipc::Kind::kResponse &&
        msg.type == "runtime.update_network.result") {
        auto it_ok = msg.fields.find("ok");
        if (it_ok != msg.fields.end() && it_ok->second == "false") {
            std::vector<uint16_t> failed_host_ports;
            auto it_count = msg.fields.find("failed_count");
            if (it_count != msg.fields.end()) {
                int count = std::stoi(it_count->second);
                for (int i = 0; i < count; ++i) {
                    auto it_p = msg.fields.find("failed_" + std::to_string(i));
                    if (it_p != msg.fields.end()) {
                        failed_host_ports.push_back(static_cast<uint16_t>(std::stoi(it_p->second)));
                    }
                }
            }
            // Build formatted strings with host_port:guest_port
            std::vector<std::string> failed_mappings;
            PortForwardErrorCallback cb;
            {
                std::lock_guard<std::mutex> lock(vms_mutex_);
                cb = port_forward_error_callback_;
                VmRecord* vm = FindVm(vm_id);
                if (vm) {
                    for (uint16_t hp : failed_host_ports) {
                        uint16_t gp = hp;  // default: same as host port
                        for (const auto& pf : vm->spec.port_forwards) {
                            if (pf.host_port == hp) {
                                gp = pf.guest_port;
                                break;
                            }
                        }
                        failed_mappings.push_back(
                            std::to_string(hp) + ":" + std::to_string(gp));
                    }
                }
            }
            if (cb && !failed_mappings.empty()) {
                cb(vm_id, failed_mappings);
            }
        }
        return;
    }

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
        msg.type == "display.shm_init") {
        auto ni = msg.fields.find("shm_name");
        if (ni == msg.fields.end()) return;
        auto get = [&](const char* key) -> uint32_t {
            auto it = msg.fields.find(key);
            return (it != msg.fields.end()) ? static_cast<uint32_t>(std::strtoul(it->second.c_str(), nullptr, 10)) : 0;
        };
        uint32_t w = get("width");
        uint32_t h = get("height");
        if (w == 0 || h == 0) return;
        {
            std::lock_guard<std::mutex> lock(vms_mutex_);
            auto& fb = shm_framebuffers_[vm_id];
            fb = std::make_unique<ipc::SharedFramebuffer>();
            fb->Open(ni->second, w, h);
        }
        return;
    }

    if (msg.channel == ipc::Channel::kDisplay &&
        msg.kind == ipc::Kind::kEvent &&
        msg.type == "display.frame_ready") {
        auto get = [&](const char* key) -> uint32_t {
            auto it = msg.fields.find(key);
            return (it != msg.fields.end()) ? static_cast<uint32_t>(std::strtoul(it->second.c_str(), nullptr, 10)) : 0;
        };
        uint32_t dw = get("width");
        uint32_t dh = get("height");
        uint32_t format = get("format");
        uint32_t resW = get("resource_width");
        uint32_t resH = get("resource_height");
        uint32_t dx = get("dirty_x");
        uint32_t dy = get("dirty_y");
        if (resW == 0) resW = dw;
        if (resH == 0) resH = dh;
        if (dw == 0 || dh == 0) return;

        ipc::SharedFramebuffer* fb = nullptr;
        {
            std::lock_guard<std::mutex> lock(vms_mutex_);
            auto it = shm_framebuffers_.find(vm_id);
            if (it != shm_framebuffers_.end() && it->second &&
                it->second->IsValid() &&
                it->second->width() == resW && it->second->height() == resH) {
                fb = it->second.get();
            }
        }
        if (!fb) return;

        DisplayFrame frame;
        frame.width = dw;
        frame.height = dh;
        frame.stride = dw * 4;
        frame.format = format;
        frame.resource_width = resW;
        frame.resource_height = resH;
        frame.dirty_x = dx;
        frame.dirty_y = dy;

        size_t row_bytes = static_cast<size_t>(dw) * 4;
        frame.pixels.resize(row_bytes * dh);
        uint32_t src_stride = fb->stride();
        const uint8_t* src = fb->data();
        for (uint32_t row = 0; row < dh; ++row) {
            size_t src_off = static_cast<size_t>(dy + row) * src_stride +
                             static_cast<size_t>(dx) * 4;
            size_t dst_off = static_cast<size_t>(row) * row_bytes;
            if (src_off + row_bytes > fb->size()) break;
            std::memcpy(frame.pixels.data() + dst_off, src + src_off, row_bytes);
        }

        DisplayCallback cb;
        {
            std::lock_guard<std::mutex> lock(vms_mutex_);
            cb = display_callback_;
        }
        if (cb) cb(vm_id, std::move(frame));
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
            StateChangeCallback cb;
            std::vector<PortForward> forwards_to_send;
            bool send_forwards = false;
            {
                std::lock_guard<std::mutex> lock(vms_mutex_);
                VmRecord* vm = FindVm(vm_id);
                if (vm) {
                    const std::string& state_str = it->second;
                    if (state_str == "running") {
                        vm->state = VmPowerState::kRunning;
                        // Send port forwards when VM becomes running
                        if (!vm->spec.port_forwards.empty()) {
                            forwards_to_send = vm->spec.port_forwards;
                            send_forwards = true;
                        }
                    } else if (state_str == "starting") {
                        vm->state = VmPowerState::kStarting;
                    } else if (state_str == "stopped") {
                        vm->state = VmPowerState::kStopped;
                    } else if (state_str == "crashed") {
                        vm->state = VmPowerState::kCrashed;
                    } else if (state_str == "rebooting") {
                        vm->reboot_pending = true;
                    }
                    cb = state_change_callback_;
                }
            }
            // Send port forward + guest forward config after lock is released
            if (send_forwards) {
                ipc::Message pf_msg;
                pf_msg.channel = ipc::Channel::kControl;
                pf_msg.kind = ipc::Kind::kRequest;
                pf_msg.type = "runtime.update_network";
                pf_msg.fields["forward_count"] = std::to_string(forwards_to_send.size());
                for (size_t i = 0; i < forwards_to_send.size(); ++i) {
                    pf_msg.fields["forward_" + std::to_string(i)] =
                        forwards_to_send[i].ToHostfwd();
                }
                std::lock_guard<std::mutex> lock(vms_mutex_);
                VmRecord* vm = FindVm(vm_id);
                if (vm) {
                    AppendGuestFwdFields(pf_msg, vm->spec.guest_forwards);
                    SendRuntimeMessage(*vm, pf_msg);
                }
            }
            if (cb) cb(vm_id);
        }
        return;
    }

    // Guest Agent state events
    if (msg.channel == ipc::Channel::kControl &&
        msg.kind == ipc::Kind::kEvent &&
        msg.type == "guest_agent.state") {
        auto it_conn = msg.fields.find("connected");
        if (it_conn != msg.fields.end()) {
            bool connected = (it_conn->second == "1");
            GuestAgentStateCallback cb;
            {
                std::lock_guard<std::mutex> lock(vms_mutex_);
                VmRecord* vm = FindVm(vm_id);
                if (vm) {
                    vm->guest_agent_connected = connected;
                }
                cb = guest_agent_state_callback_;
            }
            if (cb) cb(vm_id, connected);
        }
        return;
    }

    // Audio PCM events from VM
    if (msg.channel == ipc::Channel::kAudio &&
        msg.kind == ipc::Kind::kEvent &&
        msg.type == "audio.pcm") {
        AudioChunk chunk;
        auto get = [&](const char* key) -> uint32_t {
            auto it = msg.fields.find(key);
            return (it != msg.fields.end()) ? static_cast<uint32_t>(std::strtoul(it->second.c_str(), nullptr, 10)) : 0;
        };
        chunk.sample_rate = get("sample_rate");
        chunk.channels = static_cast<uint16_t>(get("channels"));
        if (!msg.payload.empty()) {
            size_t sample_count = msg.payload.size() / sizeof(int16_t);
            chunk.pcm.resize(sample_count);
            std::memcpy(chunk.pcm.data(), msg.payload.data(),
                         sample_count * sizeof(int16_t));
        }

        AudioPcmCallback cb;
        {
            std::lock_guard<std::mutex> lock(vms_mutex_);
            cb = audio_pcm_callback_;
        }
        if (cb) cb(vm_id, std::move(chunk));
        return;
    }

    // Clipboard events from VM
    if (msg.channel == ipc::Channel::kClipboard &&
        msg.kind == ipc::Kind::kEvent) {

        if (msg.type == "clipboard.grab") {
            // Only handle CLIPBOARD selection (0); ignore PRIMARY (1) and
            // SECONDARY (2) which are X11-specific and have no Windows
            // equivalent. Responding to PRIMARY grabs causes a ping-pong
            // loop between host and guest clipboard notifications.
            auto it_sel = msg.fields.find("selection");
            uint32_t selection = 0;
            if (it_sel != msg.fields.end()) {
                selection = static_cast<uint32_t>(std::strtoul(it_sel->second.c_str(), nullptr, 10));
            }
            if (selection != 0) {
                LOG_DEBUG("Ignoring clipboard grab with selection=%u from VM %s",
                          selection, vm_id.c_str());
                return;
            }

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
