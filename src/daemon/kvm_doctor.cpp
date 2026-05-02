#include "daemon/kvm_doctor.h"

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>

namespace tenbox::daemon {
namespace fs = std::filesystem;

namespace {

bool FileContains(const std::string& path, const std::string& needle) {
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

void Add(DoctorReport& report, std::string id, bool ok, std::string message) {
    report.checks.push_back(DoctorCheck{
        .id = std::move(id),
        .ok = ok,
        .message = std::move(message),
    });
}

DoctorReport RunKvmDoctorUncached() {
    DoctorReport report;

#if defined(__x86_64__) || defined(__amd64__)
    const bool has_vmx = FileContains("/proc/cpuinfo", "vmx");
    const bool has_svm = FileContains("/proc/cpuinfo", "svm");
    Add(report, "cpu_virtualization", has_vmx || has_svm,
        has_vmx ? "CPU exposes Intel VMX" :
        has_svm ? "CPU exposes AMD SVM" :
                  "CPU virtualization flag vmx/svm was not found in /proc/cpuinfo");
#else
    Add(report, "architecture", false, "this iteration supports x86_64 only");
#endif

    const bool has_kvm_module = fs::exists("/sys/module/kvm");
    Add(report, "kvm_module", has_kvm_module,
        has_kvm_module ? "kvm module is loaded" : "kvm kernel module is not loaded");

    const bool has_vendor_module =
        fs::exists("/sys/module/kvm_intel") || fs::exists("/sys/module/kvm_amd");
    Add(report, "kvm_vendor_module", has_vendor_module,
        has_vendor_module ? "vendor KVM module is loaded" :
                            "kvm_intel or kvm_amd module is not loaded");

    const bool has_dev_kvm = fs::exists("/dev/kvm");
    Add(report, "dev_kvm_exists", has_dev_kvm,
        has_dev_kvm ? "/dev/kvm exists" : "/dev/kvm does not exist");

    bool can_open = false;
    if (has_dev_kvm) {
        int fd = ::open("/dev/kvm", O_RDWR | O_CLOEXEC);
        can_open = fd >= 0;
        if (fd >= 0) ::close(fd);
    }
    Add(report, "dev_kvm_access", can_open,
        can_open ? "current user can open /dev/kvm read/write" :
                   "current user cannot open /dev/kvm read/write; check kvm group permissions");

    report.supported = true;
    for (const auto& check : report.checks) {
        report.supported = report.supported && check.ok;
    }
    return report;
}

std::mutex g_doctor_mu;
std::optional<DoctorReport> g_doctor_cache;

}  // namespace

DoctorReport RunKvmDoctor(bool force_refresh) {
    // Inputs (CPU flags, kernel modules, /dev/kvm presence and permissions)
    // do not change while the daemon is running, so a process-lifetime
    // cache is safe and saves the per-call /proc/cpuinfo scan + several
    // filesystem stats on every VM start / cloud doctor probe.
    std::lock_guard<std::mutex> lock(g_doctor_mu);
    if (force_refresh || !g_doctor_cache) {
        g_doctor_cache = RunKvmDoctorUncached();
    }
    return *g_doctor_cache;
}

nlohmann::json ToJson(const DoctorReport& report) {
    nlohmann::json checks = nlohmann::json::array();
    for (const auto& check : report.checks) {
        checks.push_back({
            {"id", check.id},
            {"ok", check.ok},
            {"message", check.message},
        });
    }
    return {{"supported", report.supported}, {"checks", std::move(checks)}};
}

}  // namespace tenbox::daemon
