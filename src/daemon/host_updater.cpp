#include "daemon/host_updater.h"

#include "daemon/daemon_types.h"
#include "daemon/vm_store.h"

#include <gnu/libc-version.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace tenbox::daemon::host_updater {

namespace {

constexpr const char* kAptSourceList = "/etc/apt/sources.list.d/tenbox.list";
constexpr const char* kDaemonBinary = "/usr/local/bin/tenboxd";

std::string Trim(std::string value) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string Unquote(std::string value) {
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

bool FileExists(const char* path) {
    struct stat st {};
    return ::stat(path, &st) == 0;
}

// Captures up to `tail_max` of the most recent stdout+stderr output of
// `cmd` (run via /bin/sh -c), and writes the full transcript to
// `log_path`. Returns the process exit code (-1 on plumbing failure).
int RunCapture(const std::string& cmd,
               const std::string& log_path,
               size_t tail_max,
               std::string* tail_out,
               std::string* error_out) {
    std::ofstream log(log_path, std::ios::binary | std::ios::trunc);
    if (!log) {
        if (error_out) *error_out = "failed to open " + log_path;
        return -1;
    }
    // 2>&1 so we capture stderr alongside stdout in a single stream;
    // apt is chatty on stderr for warnings even when it ultimately
    // succeeds, and we want both in the post-mortem log.
    const std::string full = cmd + " 2>&1";
    FILE* pipe = ::popen(full.c_str(), "r");
    if (!pipe) {
        if (error_out) *error_out = "popen failed";
        return -1;
    }
    std::string tail_buf;
    tail_buf.reserve(tail_max);
    std::array<char, 4096> chunk {};
    while (size_t n = std::fread(chunk.data(), 1, chunk.size(), pipe)) {
        log.write(chunk.data(), static_cast<std::streamsize>(n));
        // Maintain a rolling tail without the cost of reading the file
        // back later.
        tail_buf.append(chunk.data(), n);
        if (tail_buf.size() > tail_max) {
            tail_buf.erase(0, tail_buf.size() - tail_max);
        }
    }
    log.flush();
    const int status = ::pclose(pipe);
    if (tail_out) *tail_out = std::move(tail_buf);
    if (status == -1) {
        if (error_out) *error_out = "pclose failed";
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

std::string DpkgInstalledVersion() {
    // dpkg-query is the authoritative source for installed-version
    // strings; `dpkg -s tenbox | awk '/^Version:/{print $2}'` works too
    // but dpkg-query gives a stable single-line answer.
    FILE* pipe = ::popen("dpkg-query --show --showformat='${Version}' tenbox 2>/dev/null", "r");
    if (!pipe) return {};
    std::array<char, 256> buf {};
    std::string out;
    while (size_t n = std::fread(buf.data(), 1, buf.size(), pipe)) {
        out.append(buf.data(), n);
    }
    ::pclose(pipe);
    return Trim(std::move(out));
}

}  // namespace

AptInstallStatus CheckAptManaged() {
    AptInstallStatus status;
    if (!FileExists(kAptSourceList)) {
        status.reason = "missing apt source list at ";
        status.reason += kAptSourceList;
        return status;
    }
    if (!FileExists(kDaemonBinary)) {
        status.reason = "tenboxd binary not at expected path ";
        status.reason += kDaemonBinary;
        return status;
    }
    // dpkg-query failure means the binary on disk wasn't installed by
    // dpkg (e.g. a developer build from source). Refusing to upgrade
    // here protects that workflow from surprise apt-get install events.
    if (DpkgInstalledVersion().empty()) {
        status.reason = "dpkg has no record of the `tenbox` package; binary was not installed via apt";
        return status;
    }
    status.ok = true;
    return status;
}

std::vector<RunningVm> CollectRunningVms(const VmStore& store) {
    std::vector<RunningVm> running;
    for (const auto& record : store.List()) {
        switch (record.runtime.state) {
            case VmState::kStarting:
            case VmState::kRunning:
            case VmState::kStopping:
            case VmState::kRebooting:
                running.push_back({
                    record.spec.vm_id,
                    record.spec.name,
                    VmStateToString(record.runtime.state),
                });
                break;
            case VmState::kStopped:
            case VmState::kCrashed:
                break;
        }
    }
    return running;
}

AptResult RunAptUpgrade(const std::string& target_version,
                        const std::string& log_path,
                        size_t tail_max) {
    AptResult result;

    // Step 1: refresh metadata. Done in a separate process so a stale
    // index alone doesn't doom the whole upgrade — apt-get update
    // failures still let us proceed if the cache happens to already
    // know about the requested version.
    std::string update_tail, update_err;
    const int update_rc = RunCapture(
        "DEBIAN_FRONTEND=noninteractive apt-get update -y",
        log_path,
        tail_max,
        &update_tail,
        &update_err);
    if (!update_err.empty()) {
        result.error = "apt-get update plumbing: " + update_err;
        result.log_tail = update_tail;
        return result;
    }
    if (update_rc != 0) {
        // Non-fatal: the package may already be cached locally. Append
        // a marker to the log so post-mortem reflects what happened.
        std::ofstream log(log_path, std::ios::binary | std::ios::app);
        if (log) log << "\n[host_updater] apt-get update exited " << update_rc
                     << "; proceeding with install\n";
    }

    // Step 2: install / upgrade. We append rather than truncate so the
    // log carries both phases.
    // --no-install-recommends mirrors what the original install-linux.sh
    // does: keeps qemu-block-extra (Ceph/Gluster/RBD/NFS/RDMA) and other
    // optional Recommends out of an in-place self-update on hosts that
    // don't already have them. --only-upgrade means we never accidentally
    // install net-new packages from a future tenbox Recommends bump.
    std::string install_cmd =
        "DEBIAN_FRONTEND=noninteractive apt-get install -y "
        "--no-install-recommends --only-upgrade ";
    if (target_version.empty()) {
        install_cmd += "tenbox";
    } else {
        // Escape only the obvious shell-significant chars; apt versions
        // are limited to [A-Za-z0-9.+~:-] so this is normally a no-op.
        std::string clean;
        clean.reserve(target_version.size());
        for (char c : target_version) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
                c == '+' || c == '~' || c == ':' || c == '-') {
                clean.push_back(c);
            }
        }
        install_cmd += "tenbox=" + clean;
    }
    install_cmd += " 2>&1 | tee -a " + log_path;
    install_cmd += "; exit ${PIPESTATUS[0]}";

    // Note: PIPESTATUS is bash-only. RunCapture launches /bin/sh which
    // on Debian is dash by default, so use a direct bash invocation.
    const std::string bash_install =
        "/bin/bash -c '" + install_cmd + "'";
    std::string install_err;
    const int rc = RunCapture(bash_install,
                              log_path,
                              tail_max,
                              &result.log_tail,
                              &install_err);
    if (!install_err.empty()) {
        result.error = install_err;
        result.exit_code = rc;
        return result;
    }

    result.exit_code = rc;
    result.installed_version = DpkgInstalledVersion();
    result.ok = (rc == 0);
    return result;
}

nlohmann::json ReadOsRelease() {
    nlohmann::json out = nlohmann::json::object();
    std::ifstream in("/etc/os-release");
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string value = Unquote(Trim(line.substr(eq + 1)));
        if (key == "ID") out["id"] = value;
        else if (key == "VERSION_ID") out["version_id"] = value;
        else if (key == "VERSION_CODENAME") out["version_codename"] = value;
        else if (key == "PRETTY_NAME") out["pretty_name"] = value;
    }
    return out;
}

std::string RuntimeGlibcVersion() {
    const char* version = ::gnu_get_libc_version();
    return version ? std::string(version) : std::string();
}

const char* BuildArch() {
#if defined(__x86_64__)
    return "amd64";
#elif defined(__aarch64__)
    return "arm64";
#elif defined(__arm__)
    return "armhf";
#else
    return "unknown";
#endif
}

}  // namespace tenbox::daemon::host_updater
