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

// Spawn `apt-get update && apt-get install --only-upgrade tenbox[=ver]`
// as a fully detached child (own session via setsid + closed stdio +
// stdout/stderr redirected to `log_path`) and return immediately.
//
// Detachment is mandatory, not a nicety: when apt's postinst calls
// `systemctl restart agentsphered`, systemd SIGTERMs the daemon. If apt
// were a child of the daemon's process group, that signal would
// cascade through the popen()'d shell and tear apt down mid-install,
// leaving dpkg's status file half-written and the system stuck on
// "dpkg was interrupted; you must run sudo dpkg --configure -a".
// Detaching makes apt an immediate child of PID 1, so SIGTERM to the
// daemon cannot reach it.
//
// Because the call returns before apt finishes, there is no exit code
// to report. The authoritative outcome comes from the next host tick
// reporting daemon_version after systemd restarts us; failures show
// up in `log_path` and remain visible for operator inspection.
struct AptSpawnResult {
    bool ok = false;        // true iff fork+exec plumbing succeeded
    pid_t pid = 0;          // detached apt PID, for journal logging
    std::string log_path;   // echoed back; convenient for log lines
    std::string error;      // populated when ok == false
};

AptSpawnResult SpawnAptUpgrade(
    const std::string& target_version,  // empty = "latest"
    const std::string& log_path);

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
