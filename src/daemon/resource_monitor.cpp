#include "daemon/resource_monitor.h"

#include <sys/statvfs.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace tenbox::daemon {
namespace fs = std::filesystem;

namespace {

int64_t WallClockMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Reads utime+stime from /proc/<pid>/stat. Returns 0 if the pid is gone.
// /proc/<pid>/stat format: pid (comm) state ppid ... utime(14) stime(15) ...
// The comm field can contain spaces and parentheses, so we scan from the
// last ')' to skip past it safely.
uint64_t ReadCpuTicks(int pid) {
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    if (!stat) return 0;
    std::string contents((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
    auto rparen = contents.rfind(')');
    if (rparen == std::string::npos || rparen + 2 >= contents.size()) return 0;
    std::istringstream remainder(contents.substr(rparen + 2));
    std::string field;
    // After the trailing ')' the next fields are: state(3) ppid(4) pgrp(5)
    // session(6) tty_nr(7) tpgid(8) flags(9) minflt(10) cminflt(11) majflt(12)
    // cmajflt(13) utime(14) stime(15). We already consumed pid+comm, so utime
    // is the 12th token in `remainder`.
    constexpr int kUtimeIndex = 12;
    constexpr int kStimeIndex = 13;
    uint64_t utime = 0;
    uint64_t stime = 0;
    for (int i = 1; remainder >> field; ++i) {
        if (i == kUtimeIndex) utime = std::stoull(field);
        if (i == kStimeIndex) {
            stime = std::stoull(field);
            break;
        }
    }
    return utime + stime;
}

uint64_t ReadRssBytes(int pid) {
    std::ifstream statm("/proc/" + std::to_string(pid) + "/statm");
    uint64_t pages = 0;
    uint64_t resident = 0;
    if (statm >> pages >> resident) {
        return resident * static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
    }
    return 0;
}

}  // namespace

uint64_t DirectorySizeBytes(const std::string& path) {
    std::error_code ec;
    uint64_t total = 0;
    if (path.empty() || !fs::exists(path, ec)) return 0;
    for (const auto& entry : fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        total += entry.file_size(ec);
    }
    return total;
}

HostResources ReadHostResources(const std::string& data_dir) {
    HostResources out;
    out.cpu_count = static_cast<uint32_t>(::sysconf(_SC_NPROCESSORS_ONLN));

    std::ifstream meminfo("/proc/meminfo");
    std::string key;
    uint64_t kb = 0;
    std::string unit;
    while (meminfo >> key >> kb >> unit) {
        if (key == "MemTotal:") out.memory_total_bytes = kb * 1024;
        if (key == "MemAvailable:") out.memory_available_bytes = kb * 1024;
    }

    double loads[3] = {};
    if (::getloadavg(loads, 3) >= 1) out.load_1m = loads[0];

    struct statvfs vfs {};
    if (::statvfs(data_dir.c_str(), &vfs) == 0) {
        out.disk_total_bytes = static_cast<uint64_t>(vfs.f_blocks) * vfs.f_frsize;
        out.disk_available_bytes = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
    }
    return out;
}

ProcessResources ProcessSampler::Sample(int pid) {
    ProcessResources out;
    if (pid <= 0) return out;
    out.rss_bytes = ReadRssBytes(pid);

    const uint64_t ticks_now = ReadCpuTicks(pid);
    const int64_t wall_now = WallClockMs();
    const long clk_tck = ::sysconf(_SC_CLK_TCK);
    if (ticks_now == 0 || clk_tck <= 0) return out;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = last_.find(pid);
    if (it != last_.end() && it->second.cpu_ticks > 0 && wall_now > it->second.wall_ms) {
        const uint64_t d_ticks = (ticks_now > it->second.cpu_ticks)
            ? (ticks_now - it->second.cpu_ticks)
            : 0;
        const int64_t d_wall_ms = wall_now - it->second.wall_ms;
        if (d_wall_ms > 0) {
            // d_ticks/clk_tck = CPU seconds consumed; divide by wall seconds
            // (d_wall_ms/1000) and multiply by 100 to express as "% of one
            // logical core".
            const double cpu_seconds = static_cast<double>(d_ticks) / static_cast<double>(clk_tck);
            const double wall_seconds = static_cast<double>(d_wall_ms) / 1000.0;
            out.cpu_percent = (cpu_seconds / wall_seconds) * 100.0;
            if (out.cpu_percent < 0.0) out.cpu_percent = 0.0;
        }
    }
    last_[pid] = PidSample{.cpu_ticks = ticks_now, .wall_ms = wall_now};
    return out;
}

void ProcessSampler::Forget(int pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_.erase(pid);
}

ProcessResources ReadProcessResources(int pid) {
    ProcessResources out;
    if (pid <= 0) return out;
    out.rss_bytes = ReadRssBytes(pid);
    return out;
}

nlohmann::json ToJson(const HostResources& resources) {
    return {
        {"memory_total_bytes", resources.memory_total_bytes},
        {"memory_available_bytes", resources.memory_available_bytes},
        {"disk_total_bytes", resources.disk_total_bytes},
        {"disk_available_bytes", resources.disk_available_bytes},
        {"cpu_count", resources.cpu_count},
        {"load_1m", resources.load_1m},
    };
}

nlohmann::json ToJson(const ProcessResources& resources) {
    return {
        {"rss_bytes", resources.rss_bytes},
        {"cpu_percent", resources.cpu_percent},
    };
}

}  // namespace tenbox::daemon
