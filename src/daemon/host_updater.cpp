#include "daemon/host_updater.h"

#include "daemon/daemon_types.h"
#include "daemon/vm_store.h"

#include <dirent.h>
#include <fcntl.h>
#include <gnu/libc-version.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace tenbox::daemon::host_updater {

namespace {

constexpr const char* kAptSourceList = "/etc/apt/sources.list.d/tenbox.list";
constexpr const char* kDaemonBinary = "/usr/local/bin/agentsphered";

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

// Sanitize a user-supplied apt version string to the conservative set
// [A-Za-z0-9.+~:-] before pasting it into a shell command. Debian
// version syntax is a strict subset of this; anything else (a quote,
// a space, a `;`) is dropped, never escaped.
std::string SanitizeAptVersion(const std::string& v) {
    std::string clean;
    clean.reserve(v.size());
    for (char c : v) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
            c == '+' || c == '~' || c == ':' || c == '-') {
            clean.push_back(c);
        }
    }
    return clean;
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
        status.reason = "agentsphered binary not at expected path ";
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

AptSpawnResult SpawnAptUpgrade(const std::string& target_version,
                               const std::string& log_path) {
    AptSpawnResult result;
    result.log_path = log_path;

    // The hard problem: apt's postinst calls `systemctl restart
    // agentsphered`, which makes systemd SIGTERM the *entire* agentsphered.service
    // cgroup (KillMode=control-group is the default). A naked fork +
    // setsid is NOT enough: setsid escapes the session/pgrp but the
    // child stays in the service's cgroup and dies with it. We saw
    // this play out as "dpkg was interrupted; you must run
    // sudo dpkg --configure -a" reports from the field.
    //
    // The fix is to launch apt via `systemd-run --scope --collect`,
    // which moves the started command into its own transient cgroup
    // outside agentsphered.service. Once it's there, restarting our own
    // service cannot touch it and dpkg always gets to finish.
    //   --scope    : run synchronously in the caller's place but in a
    //                fresh transient scope unit, NOT a service unit.
    //   --collect  : auto-remove the scope unit when the command
    //                exits, even on failure.
    //   --quiet    : suppress the chatty "Running scope as unit..."
    //                line systemd-run prints to stderr by default.
    //   (no --unit): let systemd assign a unique run-XXXX.scope name
    //                so back-to-back upgrade attempts cannot collide
    //                on a stale unit from a previous run.
    //
    // The shell snippet we feed `--` mirrors the previous in-process
    // logic: refresh metadata (non-fatal), then install --only-upgrade.
    // We append a [host_updater] marker line for each phase so
    // operators can correlate the log with journalctl entries.
    //
    // --no-install-recommends mirrors install-linux.sh: keeps
    // qemu-block-extra and other optional Recommends out of in-place
    // self-updates on hosts that don't already have them.
    // --only-upgrade means we never accidentally install net-new
    // packages from a future tenbox Recommends bump.
    const std::string version_arg =
        target_version.empty()
            ? std::string("tenbox")
            : ("tenbox=" + SanitizeAptVersion(target_version));
    std::string script;
    script.reserve(768);
    script += "set +e\n";
    script += "exec >>\"$LOG\" 2>&1\n";
    script += "echo \"[host_updater] starting apt-get update at $(date -Is)\"\n";
    script += "DEBIAN_FRONTEND=noninteractive apt-get update -y\n";
    script += "rc=$?\n";
    script += "if [ \"$rc\" != 0 ]; then\n";
    script += "  echo \"[host_updater] apt-get update exited $rc; proceeding with install (cache may already have target)\"\n";
    script += "fi\n";
    script += "echo \"[host_updater] starting apt-get install --only-upgrade ";
    script += version_arg;
    script += " at $(date -Is)\"\n";
    script += "DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends --only-upgrade ";
    script += version_arg;
    script += "\n";
    script += "rc=$?\n";
    script += "echo \"[host_updater] apt-get install exited $rc at $(date -Is)\"\n";
    script += "exit $rc\n";

    // Truncate the log up front so each upgrade attempt starts with a
    // clean file. We open + close immediately; the actual writes are
    // done by the shell snippet above (exec >>"$LOG" 2>&1).
    {
        const int log_fd = ::open(log_path.c_str(),
                                  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                                  0644);
        if (log_fd < 0) {
            result.error = std::string("open(") + log_path + ") failed: " + std::strerror(errno);
            return result;
        }
        ::close(log_fd);
    }

    // Double-fork: the first child immediately forks again and
    // exits, so the grandchild (which goes on to exec systemd-run)
    // is reparented to PID 1. This lets the daemon waitpid() the
    // first child synchronously (a few ms) without ever needing to
    // reap the long-running grandchild -- crucial because the
    // daemon already uses waitpid in other paths (runtime_manager,
    // image downloads), so we cannot SIG_IGN SIGCHLD globally to
    // auto-reap.
    const pid_t first = ::fork();
    if (first < 0) {
        const int err = errno;
        result.error = std::string("fork failed: ") + std::strerror(err);
        return result;
    }

    if (first == 0) {
        // ===== first child =====
        // Detach from the daemon's session/process group. Strictly
        // belt-and-suspenders here -- the cgroup move performed by
        // systemd-run is what actually saves us from the SIGTERM
        // cascade -- but it also makes the process tree easier to
        // read in pstree.
        ::setsid();

        const pid_t second = ::fork();
        if (second < 0) {
            const int log_fd = ::open(log_path.c_str(),
                                      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                                      0644);
            if (log_fd >= 0) {
                const std::string msg =
                    std::string("[host_updater] second fork failed: ")
                    + std::strerror(errno) + "\n";
                (void)::write(log_fd, msg.data(), msg.size());
                ::close(log_fd);
            }
            _exit(127);
        }
        if (second > 0) {
            // First child's only job is done: it created the
            // grandchild. Exit immediately so the daemon's waitpid
            // returns promptly and the grandchild is reparented to
            // PID 1.
            _exit(0);
        }

        // ===== grandchild =====
        // Detach stdio. systemd-run inherits our fds for the wrapped
        // command, but the wrapped /bin/sh re-redirects to "$LOG" via
        // exec >>"$LOG" 2>&1, so all of apt's output still lands in
        // the log file. Closing them here keeps the daemon's pipes
        // / sockets out of the new scope.
        const int devnull = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }

        // Defensive: close any stray daemon fds (TLS sockets, log
        // pipes, etc.) that forgot O_CLOEXEC. Without this apt could
        // inherit the cloud tunnel socket and keep the connection
        // alive past restart. /proc/self/fd is the portable way to
        // enumerate without iterating up to RLIMIT_NOFILE.
        DIR* dirp = ::opendir("/proc/self/fd");
        if (dirp) {
            const int dir_fd = ::dirfd(dirp);
            struct dirent* ent = nullptr;
            while ((ent = ::readdir(dirp)) != nullptr) {
                if (ent->d_name[0] == '.') continue;
                const int fd = std::atoi(ent->d_name);
                if (fd <= STDERR_FILENO) continue;
                if (fd == dir_fd) continue;
                ::close(fd);
            }
            ::closedir(dirp);
        }

        // exec systemd-run, which moves us into a fresh transient
        // scope cgroup before exec-ing /bin/sh. Once we're in the new
        // cgroup, `systemctl restart agentsphered` can no longer reach us.
        //
        // CRITICAL: `--setenv` must be a single argv of the form
        // `--setenv=KEY=VALUE` (or `--setenv KEY=VALUE` as two argvs).
        // An earlier version split it as `--setenv=LOG` + `<path>`,
        // which made systemd-run treat the path as the command to
        // exec; the scope spun up and died in the same millisecond
        // ("Deactivated successfully"), the log file stayed empty,
        // and no upgrade ever ran. Build the env arg as one string.
        const std::string log_env = std::string("--setenv=LOG=") + log_path;
        const char* argv[] = {
            "systemd-run",
            "--scope",
            "--collect",
            "--quiet",
            log_env.c_str(),
            "/bin/sh", "-c", script.c_str(),
            nullptr,
        };
        ::execvp("systemd-run", const_cast<char* const*>(argv));

        // execvp failed (systemd-run not found, EACCES, etc.).
        // Append a diagnostic to the log so the operator has
        // something to grep for.
        const int log_fd = ::open(log_path.c_str(),
                                  O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                                  0644);
        if (log_fd >= 0) {
            const std::string msg =
                std::string("[host_updater] execvp systemd-run failed: ")
                + std::strerror(errno) + "\n";
            (void)::write(log_fd, msg.data(), msg.size());
            ::close(log_fd);
        }
        _exit(127);
    }

    // ===== parent (daemon) =====
    // Reap the first child synchronously. It exits within a couple
    // of milliseconds (its only job is to fork the grandchild).
    // Skipping this would leak a zombie since we cannot SIG_IGN
    // SIGCHLD (other daemon paths rely on waitpid working).
    int wstatus = 0;
    (void)::waitpid(first, &wstatus, 0);
    result.ok = true;
    result.pid = first;  // Reported to the journal; the actual apt
                         // PID is whatever systemd-run becomes after
                         // exec inside the new scope, not `first`.
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
