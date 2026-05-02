#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace tenbox::daemon {

class VmStore;

namespace host_updater {

// Snapshot of "is the daemon binary genuinely managed by apt?" used by
// the cloud `host.update` precondition check. Only true when both the
// `tenbox` apt source list lives at the canonical path AND the binary
// is in a writable-by-dpkg location. Anything else (developer build,
// hand-installed tarball) is rejected as `update_disabled` so we never
// trash a manually-managed install.
struct AptInstallStatus {
    bool ok = false;
    std::string reason;
};

AptInstallStatus CheckAptManaged();

// VM that is currently in a "do not interrupt" state. host.update lists
// these in its `vms_running` error so the console can render the actual
// vm names instead of just "some vm is running".
struct RunningVm {
    std::string vm_id;
    std::string name;
    std::string state;  // VmStateToString equivalent
};

std::vector<RunningVm> CollectRunningVms(const VmStore& store);

// Run `apt-get update && apt-get install --only-upgrade tenbox[=ver]`.
// Streams stdout+stderr into `log_path` (truncated on each call) for
// post-mortem inspection, also keeps the last `tail_max` bytes in
// `log_tail`. Sets `installed_version` to the dpkg-reported version
// after a successful install.
//
// Returns true iff the apt-get exit code was zero. `error` is filled on
// any subprocess plumbing failure (fork, popen) before apt itself ran.
struct AptResult {
    bool ok = false;
    int exit_code = -1;
    std::string installed_version;
    std::string log_tail;
    std::string error;
};

AptResult RunAptUpgrade(
    const std::string& target_version,  // empty = "latest"
    const std::string& log_path,
    size_t tail_max = 4096);

// Read /etc/os-release into a JSON object. Keys: id, version_id,
// version_codename, pretty_name. Missing fields are absent (no empty
// strings) so the cloud side can do a clean .value(key, "") fallback.
nlohmann::json ReadOsRelease();

// Returns the runtime glibc version, e.g. "2.35". Empty string on
// platforms where gnu_get_libc_version is unavailable.
std::string RuntimeGlibcVersion();

// Build-time architecture string, "amd64" or "arm64".
const char* BuildArch();

}  // namespace host_updater
}  // namespace tenbox::daemon
