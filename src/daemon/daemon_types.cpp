#include "daemon/daemon_types.h"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace tenbox::daemon {
namespace fs = std::filesystem;

std::string VmStateToString(VmState state) {
    switch (state) {
    case VmState::kStopped: return "stopped";
    case VmState::kStarting: return "starting";
    case VmState::kRunning: return "running";
    case VmState::kStopping: return "stopping";
    case VmState::kCrashed: return "crashed";
    case VmState::kRebooting: return "rebooting";
    }
    return "unknown";
}

VmState VmStateFromString(const std::string& value) {
    if (value == "starting") return VmState::kStarting;
    if (value == "running") return VmState::kRunning;
    if (value == "stopping") return VmState::kStopping;
    if (value == "crashed") return VmState::kCrashed;
    if (value == "rebooting") return VmState::kRebooting;
    return VmState::kStopped;
}

nlohmann::json ToJson(const HostForward& forward) {
    return {
        {"host_port", forward.host_port},
        {"guest_port", forward.guest_port},
        {"host_ip", forward.host_ip},
        {"guest_ip", forward.guest_ip},
    };
}

// guest_ip is stored as a host-order uint32 in C++ but we serialize it as a
// human-readable dotted quad ("10.0.2.3") for symmetry with HostForward and
// to keep the JSON shape stable for the cloud console.
nlohmann::json ToJson(const GuestForward& forward) {
    return {
        {"guest_ip", GuestForward::Ip4ToString(forward.guest_ip)},
        {"guest_port", forward.guest_port},
        {"host_addr", forward.host_addr},
        {"host_port", forward.host_port},
    };
}

nlohmann::json ToJson(const SharedFolder& folder) {
    return {
        {"tag", folder.tag},
        {"host_path", folder.host_path},
        {"readonly", folder.readonly},
    };
}

nlohmann::json ToJson(const VmSpec& spec) {
    nlohmann::json host_forwards = nlohmann::json::array();
    for (const auto& pf : spec.host_forwards) host_forwards.push_back(ToJson(pf));
    nlohmann::json guest_forwards = nlohmann::json::array();
    for (const auto& gf : spec.guest_forwards) guest_forwards.push_back(ToJson(gf));
    nlohmann::json shared_folders = nlohmann::json::array();
    for (const auto& sf : spec.shared_folders) shared_folders.push_back(ToJson(sf));

    return {
        {"id", spec.vm_id},
        {"name", spec.name},
        {"vm_dir", spec.vm_dir},
        {"kernel_path", spec.kernel_path},
        {"initrd_path", spec.initrd_path},
        {"disk_path", spec.disk_path},
        {"cmdline", spec.cmdline},
        {"memory_mb", spec.memory_mb},
        {"cpu_count", spec.cpu_count},
        {"net_enabled", spec.nat_enabled},
        {"debug_mode", spec.debug_mode},
        {"dpi_scaled", spec.dpi_scaled},
        {"creation_time", spec.creation_time},
        {"last_boot_time", spec.last_boot_time},
        {"host_forwards", std::move(host_forwards)},
        {"guest_forwards", std::move(guest_forwards)},
        {"shared_folders", std::move(shared_folders)},
    };
}

nlohmann::json ToJson(const FailureInfo& failure) {
    return {{"code", failure.code}, {"message", failure.message}};
}

nlohmann::json ToJson(const VmRuntimeInfo& runtime) {
    nlohmann::json json = {
        {"pid", runtime.pid},
        {"exit_code", runtime.exit_code},
        {"state", VmStateToString(runtime.state)},
        {"started_at", runtime.started_at},
        {"guest_agent_connected", runtime.guest_agent_connected},
    };
    if (runtime.last_failure) {
        json["last_failure"] = ToJson(*runtime.last_failure);
    }
    return json;
}

nlohmann::json ToJson(const VmRecord& record) {
    return {{"spec", ToJson(record.spec)}, {"runtime", ToJson(record.runtime)}};
}

bool FromJson(const nlohmann::json& value, VmSpec& spec, std::string* error) {
    if (!value.is_object()) {
        if (error) *error = "expected VM object";
        return false;
    }
    spec.name = value.value("name", "");
    spec.cmdline = value.value("cmdline", "");
    spec.memory_mb = value.value("memory_mb", static_cast<uint64_t>(4096));
    spec.cpu_count = value.value("cpu_count", static_cast<uint32_t>(4));
    spec.nat_enabled = value.value("net_enabled", value.value("nat_enabled", true));
    spec.debug_mode = value.value("debug_mode", false);
    spec.dpi_scaled = value.value("dpi_scaled", false);
    spec.creation_time = value.value("creation_time", static_cast<int64_t>(0));
    spec.last_boot_time = value.value("last_boot_time", static_cast<int64_t>(0));
    if (spec.name.empty()) spec.name = value.value("id", "");

    // Read the new `host_forwards` key, falling back to the legacy
    // `port_forwards` so we don't drop H->G forwards from older vm.json files.
    const auto* host_forwards_json = value.contains("host_forwards") ? &value["host_forwards"]
                                   : value.contains("port_forwards") ? &value["port_forwards"]
                                                                     : nullptr;
    if (host_forwards_json && host_forwards_json->is_array()) {
        for (const auto& item : *host_forwards_json) {
            if (!item.is_object()) continue;
            HostForward pf;
            pf.host_port = item.value("host_port", 0);
            pf.guest_port = item.value("guest_port", 0);
            pf.host_ip = item.value("host_ip", "");
            pf.guest_ip = item.value("guest_ip", "");
            if (pf.host_port != 0 && pf.guest_port != 0) {
                spec.host_forwards.push_back(std::move(pf));
            }
        }
    }

    if (value.contains("guest_forwards") && value["guest_forwards"].is_array()) {
        for (const auto& item : value["guest_forwards"]) {
            if (!item.is_object()) continue;
            GuestForward gf;
            const std::string guest_ip_str = item.value("guest_ip", "");
            if (!guest_ip_str.empty() && !GuestForward::Ip4FromString(guest_ip_str, gf.guest_ip)) {
                continue;
            }
            gf.guest_port = item.value("guest_port", 0);
            gf.host_addr = item.value("host_addr", "");
            gf.host_port = item.value("host_port", 0);
            if (gf.guest_port != 0 && gf.host_port != 0 && gf.guest_ip != 0) {
                spec.guest_forwards.push_back(std::move(gf));
            }
        }
    }

    if (value.contains("shared_folders") && value["shared_folders"].is_array()) {
        for (const auto& item : value["shared_folders"]) {
            if (!item.is_object()) continue;
            SharedFolder sf;
            sf.tag = item.value("tag", "");
            sf.host_path = item.value("host_path", "");
            sf.readonly = item.value("readonly", false);
            if (!sf.tag.empty() && !sf.host_path.empty()) {
                spec.shared_folders.push_back(std::move(sf));
            }
        }
    }
    return true;
}

std::string DefaultDataDir() {
    if (const char* explicit_dir = std::getenv("TENBOX_DATA_DIR")) {
        if (*explicit_dir) return explicit_dir;
    }
    if (::geteuid() == 0) return "/var/lib/tenbox";
    if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
        if (*xdg) return std::string(xdg) + "/tenbox";
    }
    if (const char* home = std::getenv("HOME")) {
        if (*home) return std::string(home) + "/.local/share/tenbox";
    }
    return "/tmp/tenbox-data";
}

std::string DefaultSocketPath() {
    if (const char* explicit_path = std::getenv("TENBOX_SOCK")) {
        if (*explicit_path) return explicit_path;
    }
    if (const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR")) {
        if (*runtime_dir) return std::string(runtime_dir) + "/tenbox.sock";
    }
    return "/tmp/tenbox-" + std::to_string(::getuid()) + ".sock";
}

std::string GenerateUuid() {
    std::random_device rd;
    std::mt19937_64 rng(rd());
    uint8_t bytes[16];
    for (auto& byte : bytes) byte = static_cast<uint8_t>(rng() & 0xff);
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);

    char out[37];
    snprintf(out, sizeof(out),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
             bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);
    return out;
}

int64_t UnixNow() {
    return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}

}  // namespace tenbox::daemon
