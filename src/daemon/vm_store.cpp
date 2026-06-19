#include "daemon/vm_store.h"

#include <fstream>
#include <system_error>

namespace tenbox::daemon {
namespace fs = std::filesystem;

namespace {

std::string PathToUtf8(const fs::path& path) {
    auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string FileNameOrEmpty(const std::string& path) {
    if (path.empty()) return {};
    return PathToUtf8(fs::path(path).filename());
}

constexpr const char* kRuntimeStateFile = "runtime_state.json";

bool WriteFileAtomic(const fs::path& dst, const std::string& contents) {
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    const fs::path tmp = dst.parent_path() / (dst.filename().string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
        if (!out) return false;
        out << contents;
        if (!out) return false;
    }
    fs::rename(tmp, dst, ec);
    return !ec;
}

void WriteRuntimeStateFile(const fs::path& vm_dir, const VmRuntimeInfo& info) {
    nlohmann::json json = ToJson(info);
    (void)WriteFileAtomic(vm_dir / kRuntimeStateFile, json.dump(2));
}

void ReadRuntimeStateFile(const fs::path& vm_dir, VmRuntimeInfo& info) {
    std::ifstream input(vm_dir / kRuntimeStateFile);
    if (!input) return;
    auto json = nlohmann::json::parse(input, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return;
    // Persisted runtime state is a snapshot from the previous daemon run.
    // The pid is meaningless across daemon restarts (the kernel may have
    // recycled it), so we always reset it. State is normalized below by
    // the caller so an in-flight `running/starting/stopping` does not
    // outlive the daemon process that owned it.
    info.pid = 0;
    info.exit_code = json.value("exit_code", 0);
    info.state = VmStateFromString(json.value("state", "stopped"));
    info.started_at = json.value("started_at", static_cast<int64_t>(0));
    // `guest_agent_connected` is intentionally not restored: the QGA channel
    // does not survive a daemon restart, the next hello will set it true.
    info.guest_agent_connected = false;
    info.last_failure.reset();
    if (json.contains("last_failure") && json["last_failure"].is_object()) {
        FailureInfo failure;
        failure.code = json["last_failure"].value("code", "");
        failure.message = json["last_failure"].value("message", "");
        if (!failure.code.empty()) info.last_failure = std::move(failure);
    }
}

}  // namespace

VmStore::VmStore(std::string data_dir) : data_dir_(std::move(data_dir)) {}

void VmStore::SetVmCreatedCallback(VmCreatedCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    vm_created_callback_ = std::move(callback);
}

void VmStore::SetVmUpdatedCallback(VmUpdatedCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    vm_updated_callback_ = std::move(callback);
}

void VmStore::SetVmRemovedCallback(VmRemovedCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    vm_removed_callback_ = std::move(callback);
}

fs::path VmStore::VmRoot() const {
    return fs::path(data_dir_) / "vms";
}

bool VmStore::Load(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    vms_.clear();

    std::error_code ec;
    fs::create_directories(VmRoot(), ec);
    if (ec) {
        if (error) *error = "failed to create VM root: " + ec.message();
        return false;
    }

    for (const auto& entry : fs::directory_iterator(VmRoot(), ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;
        (void)LoadVmDir(entry.path(), error);
    }
    return true;
}

bool VmStore::LoadVmDir(const fs::path& vm_dir, std::string* error) {
    const auto manifest = vm_dir / "vm.json";
    std::ifstream input(manifest);
    if (!input) return false;

    auto json = nlohmann::json::parse(input, nullptr, false);
    if (json.is_discarded()) {
        if (error) *error = "invalid VM manifest: " + PathToUtf8(manifest);
        return false;
    }

    VmSpec spec;
    if (!FromJson(json, spec, error)) return false;
    spec.vm_id = PathToUtf8(vm_dir.filename());
    spec.vm_dir = PathToUtf8(vm_dir);

    auto resolve = [&](const char* key) -> std::string {
        const std::string rel = json.value(key, "");
        if (rel.empty()) return {};
        fs::path path(rel);
        if (path.is_relative()) path = vm_dir / path;
        return PathToUtf8(path);
    };
    spec.kernel_path = resolve("kernel");
    spec.initrd_path = resolve("initrd");
    spec.disk_path = resolve("disk");

    VmRuntimeInfo runtime;
    ReadRuntimeStateFile(vm_dir, runtime);
    // Daemon was not running just now (we're inside Load()), so any state
    // marked running/starting/stopping in the persisted file represents a
    // VM that the previous daemon owned but did not cleanly shut down.
    // Normalize to `crashed` so the user sees a clear "we don't know what
    // happened, treat as crash" rather than a phantom "running" entry.
    if (runtime.state == VmState::kRunning || runtime.state == VmState::kStarting ||
        runtime.state == VmState::kStopping || runtime.state == VmState::kRebooting) {
        runtime.state = VmState::kCrashed;
        if (!runtime.last_failure) {
            runtime.last_failure = FailureInfo{
                .code = "daemon_restart",
                .message = "agentsphered restarted while this VM was active; runtime state is unknown",
            };
        }
    }

    vms_.push_back(VmRecord{.spec = std::move(spec), .runtime = std::move(runtime)});
    return true;
}

bool VmStore::SaveVm(const VmSpec& spec, std::string* error) const {
    std::error_code ec;
    fs::create_directories(spec.vm_dir, ec);
    if (ec) {
        if (error) *error = "failed to create VM directory: " + ec.message();
        return false;
    }

    nlohmann::json json;
    json["name"] = spec.name;
    json["cmdline"] = spec.cmdline;
    json["memory_mb"] = spec.memory_mb;
    json["cpu_count"] = spec.cpu_count;
    json["net_enabled"] = spec.nat_enabled;
    json["debug_mode"] = spec.debug_mode;
    json["dpi_scaled"] = spec.dpi_scaled;
    json["kernel"] = FileNameOrEmpty(spec.kernel_path);
    json["initrd"] = FileNameOrEmpty(spec.initrd_path);
    json["disk"] = FileNameOrEmpty(spec.disk_path);
    json["creation_time"] = spec.creation_time;
    json["last_boot_time"] = spec.last_boot_time;

    json["host_forwards"] = nlohmann::json::array();
    for (const auto& pf : spec.host_forwards) json["host_forwards"].push_back(ToJson(pf));
    json["guest_forwards"] = nlohmann::json::array();
    for (const auto& gf : spec.guest_forwards) json["guest_forwards"].push_back(ToJson(gf));
    json["shared_folders"] = nlohmann::json::array();
    for (const auto& sf : spec.shared_folders) json["shared_folders"].push_back(ToJson(sf));

    std::ofstream output(fs::path(spec.vm_dir) / "vm.json", std::ios::trunc);
    if (!output) {
        if (error) *error = "failed to open VM manifest for writing";
        return false;
    }
    output << json.dump(2) << '\n';
    return true;
}

std::vector<VmRecord> VmStore::List() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return vms_;
}

std::optional<VmRecord> VmStore::Get(const std::string& vm_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& vm : vms_) {
        if (vm.spec.vm_id == vm_id) return vm;
    }
    return std::nullopt;
}

bool VmStore::Create(const VmSpec& spec, VmRecord* created, std::string* error) {
    if (!SaveVm(spec, error)) return false;
    VmRecord record{.spec = spec};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        vms_.push_back(record);
    }
    if (created) *created = record;
    // Fire callback outside the store mutex so subscribers can safely call
    // back into VmStore (e.g. via List()) without deadlocking.
    VmCreatedCallback cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = vm_created_callback_;
    }
    if (cb) cb(record);
    return true;
}

bool VmStore::UpdateSpec(const std::string& vm_id, const VmSpec& spec, std::string* error) {
    if (!SaveVm(spec, error)) return false;
    VmRecord updated_record;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& vm : vms_) {
            if (vm.spec.vm_id == vm_id) {
                vm.spec = spec;
                updated_record = vm;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        if (error) *error = "VM not found";
        return false;
    }
    VmUpdatedCallback cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = vm_updated_callback_;
    }
    if (cb) cb(updated_record);
    return true;
}

bool VmStore::Remove(const std::string& vm_id, std::string* error) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = vms_.begin(); it != vms_.end(); ++it) {
            if (it->spec.vm_id != vm_id) continue;
            std::error_code ec;
            fs::remove_all(it->spec.vm_dir, ec);
            if (ec) {
                if (error) *error = "failed to delete VM directory: " + ec.message();
                return false;
            }
            vms_.erase(it);
            removed = true;
            break;
        }
    }
    if (!removed) {
        if (error) *error = "VM not found";
        return false;
    }
    // Fire callback outside the store mutex so subscribers can safely call
    // back into VmStore without deadlocking.
    VmRemovedCallback cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = vm_removed_callback_;
    }
    if (cb) cb(vm_id);
    return true;
}

bool VmStore::UpdateRuntime(const std::string& vm_id, const VmRuntimeInfo& runtime) {
    std::string vm_dir;
    VmRuntimeInfo to_persist;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& vm : vms_) {
            if (vm.spec.vm_id == vm_id) {
                vm.runtime = runtime;
                vm_dir = vm.spec.vm_dir;
                to_persist = vm.runtime;
                break;
            }
        }
    }
    if (vm_dir.empty()) return false;
    WriteRuntimeStateFile(vm_dir, to_persist);
    return true;
}

bool VmStore::SetFailure(const std::string& vm_id, FailureInfo failure) {
    std::string vm_dir;
    VmRuntimeInfo to_persist;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& vm : vms_) {
            if (vm.spec.vm_id == vm_id) {
                vm.runtime.last_failure = std::move(failure);
                vm_dir = vm.spec.vm_dir;
                to_persist = vm.runtime;
                break;
            }
        }
    }
    if (vm_dir.empty()) return false;
    WriteRuntimeStateFile(vm_dir, to_persist);
    return true;
}

}  // namespace tenbox::daemon
