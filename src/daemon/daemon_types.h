#pragma once

#include "common/vm_model.h"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace tenbox::daemon {

enum class VmState {
    kStopped,
    kStarting,
    kRunning,
    kStopping,
    kCrashed,
    // Transient state between the runtime exiting with code 128 ("guest
    // requested reboot") and the daemon re-spawning a fresh runtime for the
    // same VM. Surfaces as an orange "Rebooting" badge instead of the red
    // "Crashed" banner the user used to see after every guest reboot.
    kRebooting,
};

struct FailureInfo {
    std::string code;
    std::string message;
};

struct VmRuntimeInfo {
    int pid = 0;
    int exit_code = 0;
    VmState state = VmState::kStopped;
    int64_t started_at = 0;
    std::optional<FailureInfo> last_failure;
    // Whether the in-guest QEMU Guest Agent is currently connected. Set by
    // RuntimeManager from the runtime's `guest_agent.state` event, reset to
    // false on stop. Intentionally NOT persisted across daemon restarts -
    // the next QGA hello after the daemon comes back up will set it.
    bool guest_agent_connected = false;
};

struct VmRecord {
    VmSpec spec;
    VmRuntimeInfo runtime;
};

struct DaemonConfig {
    std::string data_dir;
    std::string socket_path;
    std::string runtime_path = "tenbox-vm-runtime";
    std::string cloud_url;
};

std::string VmStateToString(VmState state);
VmState VmStateFromString(const std::string& value);

nlohmann::json ToJson(const HostForward& forward);
nlohmann::json ToJson(const GuestForward& forward);
nlohmann::json ToJson(const SharedFolder& folder);
nlohmann::json ToJson(const VmSpec& spec);
nlohmann::json ToJson(const FailureInfo& failure);
nlohmann::json ToJson(const VmRuntimeInfo& runtime);
nlohmann::json ToJson(const VmRecord& record);

bool FromJson(const nlohmann::json& value, VmSpec& spec, std::string* error);

std::string DefaultDataDir();
std::string DefaultSocketPath();
std::string GenerateUuid();
int64_t UnixNow();

}  // namespace tenbox::daemon
