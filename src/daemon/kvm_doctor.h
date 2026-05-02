#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace tenbox::daemon {

struct DoctorCheck {
    std::string id;
    bool ok = false;
    std::string message;
};

struct DoctorReport {
    bool supported = false;
    std::vector<DoctorCheck> checks;
};

// Probe KVM availability. The result is memoized for the lifetime of the
// process because the inputs (CPU flags, /dev/kvm permissions, kernel
// modules) do not change at runtime; subsequent calls return the cached
// report. Pass `force_refresh=true` to bypass the cache (e.g. on demand
// from the cloud console after the operator fixes permissions).
DoctorReport RunKvmDoctor(bool force_refresh = false);
nlohmann::json ToJson(const DoctorReport& report);

}  // namespace tenbox::daemon
