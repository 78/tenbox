#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

namespace tenbox::daemon {

struct HostResources {
    uint64_t memory_total_bytes = 0;
    uint64_t memory_available_bytes = 0;
    uint64_t disk_total_bytes = 0;
    uint64_t disk_available_bytes = 0;
    uint32_t cpu_count = 0;
    double load_1m = 0.0;
};

struct ProcessResources {
    uint64_t rss_bytes = 0;
    // Single-VM CPU usage normalized so 100.0 means "fully saturating one
    // logical core". A 4-vCPU VM at full tilt therefore reads ~400.0. The
    // first sample after the VM starts is 0 because we need two readings to
    // diff CPU jiffies; subsequent samples come from `ProcessSampler`.
    double cpu_percent = 0.0;
};

uint64_t DirectorySizeBytes(const std::string& path);
HostResources ReadHostResources(const std::string& data_dir);

// Stateful sampler: keeps the previous (utime+stime, wall_clock) reading per
// pid so we can compute CPU%. `Sample` is safe to call concurrently from
// different pids; same-pid calls serialize via the internal mutex. Call
// `Forget(pid)` when a VM stops so old entries do not leak across recycled
// pids.
class ProcessSampler {
public:
    ProcessResources Sample(int pid);
    void Forget(int pid);

private:
    struct PidSample {
        uint64_t cpu_ticks = 0;
        int64_t wall_ms = 0;
    };
    mutable std::mutex mutex_;
    std::map<int, PidSample> last_;
};

// Stateless probe used when no sampler is available (e.g. CLI quick reads).
// Always returns cpu_percent = 0.
ProcessResources ReadProcessResources(int pid);

nlohmann::json ToJson(const HostResources& resources);
nlohmann::json ToJson(const ProcessResources& resources);

}  // namespace tenbox::daemon
