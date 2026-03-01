#include "manager/app_settings.h"

#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace settings {

namespace fs = std::filesystem;
using json = nlohmann::json;

// ── Helpers ──────────────────────────────────────────────────────────

static void EnsureDir(const std::string& dir) {
    if (!dir.empty()) {
        std::error_code ec;
        fs::create_directories(dir, ec);
    }
}

static std::string PathToUtf8(const fs::path& p) {
    return p.string();
}

std::string GetDataDir() {
    char path[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) {
        return std::string(path) + "\\TenBox";
    }
    return {};
}

std::string DefaultVmStorageDir() {
    auto base = GetDataDir();
    return base.empty() ? std::string{} : base + "\\vms";
}

std::string GenerateUuid() {
    // Use Windows CryptGenRandom for UUID v4
    HCRYPTPROV prov = 0;
    uint8_t bytes[16]{};
    if (CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        CryptGenRandom(prov, sizeof(bytes), bytes);
        CryptReleaseContext(prov, 0);
    } else {
        // Fallback: use GetTickCount64 + pid as entropy (not cryptographically secure)
        auto t = GetTickCount64();
        auto p = GetCurrentProcessId();
        std::memcpy(bytes, &t, 8);
        std::memcpy(bytes + 8, &p, 4);
    }
    // Set version (4) and variant (RFC 4122)
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;

    char buf[37];
    snprintf(buf, sizeof(buf),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5],
             bytes[6], bytes[7],
             bytes[8], bytes[9],
             bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return buf;
}

// ── AppSettings (settings.json) ──────────────────────────────────────

AppSettings LoadSettings(const std::string& data_dir) {
    AppSettings s;
    auto path = fs::path(data_dir) / "settings.json";
    std::ifstream ifs(path);
    if (!ifs) return s;

    try {
        json j = json::parse(ifs, nullptr, false);
        if (j.is_discarded()) return s;

        if (j.contains("window") && j["window"].is_object()) {
            auto& w = j["window"];
            if (w.contains("x"))      s.window.x      = w["x"].get<int>();
            if (w.contains("y"))      s.window.y      = w["y"].get<int>();
            if (w.contains("width"))  s.window.width  = w["width"].get<int>();
            if (w.contains("height")) s.window.height = w["height"].get<int>();
        }
        if (j.contains("show_toolbar") && j["show_toolbar"].is_boolean()) {
            s.show_toolbar = j["show_toolbar"].get<bool>();
        }
        if (j.contains("vm_paths") && j["vm_paths"].is_array()) {
            auto default_storage = DefaultVmStorageDir();
            for (auto& item : j["vm_paths"]) {
                if (item.is_string()) {
                    auto p = fs::path(item.get<std::string>());
                    if (p.is_relative() && !default_storage.empty()) {
                        p = fs::path(default_storage) / p;
                    }
                    s.vm_paths.push_back(PathToUtf8(p));
                }
            }
        }
    } catch (...) {}
    return s;
}

void SaveSettings(const std::string& data_dir, const AppSettings& s) {
    EnsureDir(data_dir);

    json w;
    w["x"]      = s.window.x;
    w["y"]      = s.window.y;
    w["width"]  = s.window.width;
    w["height"] = s.window.height;

    auto default_storage = DefaultVmStorageDir();
    json vm_paths_json = json::array();
    for (const auto& abs_path : s.vm_paths) {
        if (!default_storage.empty()) {
            std::error_code ec;
            auto rel = fs::relative(fs::path(abs_path), fs::path(default_storage), ec);
            if (!ec && !rel.empty() && *rel.string().begin() != '.') {
                vm_paths_json.push_back(PathToUtf8(rel));
                continue;
            }
        }
        vm_paths_json.push_back(abs_path);
    }

    json j;
    j["window"]       = w;
    j["show_toolbar"] = s.show_toolbar;
    j["vm_paths"]     = vm_paths_json;

    auto path = fs::path(data_dir) / "settings.json";
    std::ofstream ofs(path, std::ios::trunc);
    if (ofs) ofs << j.dump(2) << '\n';
}

// ── VM manifest (vm.json) ────────────────────────────────────────────

bool LoadVmManifest(const std::string& vm_dir, VmSpec& spec) {
    auto manifest = fs::path(vm_dir) / "vm.json";
    std::ifstream ifs(manifest);
    if (!ifs) return false;

    try {
        json j = json::parse(ifs, nullptr, false);
        if (j.is_discarded()) return false;

        // vm_id = directory name (UUID)
        spec.vm_id  = PathToUtf8(fs::path(vm_dir).filename());
        spec.vm_dir = PathToUtf8(fs::path(vm_dir));

        if (j.contains("name"))      spec.name      = j["name"].get<std::string>();
        if (j.contains("cmdline"))   spec.cmdline   = j["cmdline"].get<std::string>();
        if (j.contains("memory_mb")) spec.memory_mb = j["memory_mb"].get<uint64_t>();
        if (j.contains("cpu_count")) spec.cpu_count = j["cpu_count"].get<uint32_t>();
        if (j.contains("nat_enabled")) spec.nat_enabled = j["nat_enabled"].get<bool>();

        // Resolve relative paths to absolute
        auto Resolve = [&](const char* key) -> std::string {
            if (!j.contains(key)) return {};
            auto rel = j[key].get<std::string>();
            if (rel.empty()) return {};
            return PathToUtf8(fs::path(vm_dir) / rel);
        };
        spec.kernel_path = Resolve("kernel");
        spec.initrd_path = Resolve("initrd");
        spec.disk_path   = Resolve("disk");

        if (j.contains("port_forwards") && j["port_forwards"].is_array()) {
            for (auto& item : j["port_forwards"]) {
                if (item.contains("host_port") && item.contains("guest_port")) {
                    spec.port_forwards.push_back({
                        item["host_port"].get<uint16_t>(),
                        item["guest_port"].get<uint16_t>()
                    });
                }
            }
        }

        if (j.contains("shared_folders") && j["shared_folders"].is_array()) {
            for (auto& item : j["shared_folders"]) {
                if (item.contains("tag") && item.contains("host_path")) {
                    SharedFolder sf;
                    sf.tag = item["tag"].get<std::string>();
                    sf.host_path = item["host_path"].get<std::string>();
                    if (item.contains("readonly")) {
                        sf.readonly = item["readonly"].get<bool>();
                    }
                    spec.shared_folders.push_back(std::move(sf));
                }
            }
        }

        if (j.contains("creation_time") && j["creation_time"].is_number_integer()) {
            spec.creation_time = j["creation_time"].get<int64_t>();
        }
        if (j.contains("last_boot_time") && j["last_boot_time"].is_number_integer()) {
            spec.last_boot_time = j["last_boot_time"].get<int64_t>();
        }
    } catch (...) {
        return false;
    }

    return !spec.vm_id.empty();
}

void SaveVmManifest(const VmSpec& spec) {
    if (spec.vm_dir.empty()) return;
    EnsureDir(spec.vm_dir);

    // Store paths relative to vm_dir
    auto MakeRelative = [&](const std::string& abs_path) -> std::string {
        if (abs_path.empty()) return {};
        std::error_code ec;
        auto rel = fs::relative(fs::path(abs_path), fs::path(spec.vm_dir), ec);
        return ec ? abs_path : PathToUtf8(rel);
    };

    json j;
    j["name"]        = spec.name;
    j["kernel"]      = MakeRelative(spec.kernel_path);
    j["initrd"]      = MakeRelative(spec.initrd_path);
    j["disk"]        = MakeRelative(spec.disk_path);
    j["cmdline"]     = spec.cmdline;
    j["memory_mb"]   = spec.memory_mb;
    j["cpu_count"]   = spec.cpu_count;
    j["nat_enabled"] = spec.nat_enabled;
    if (spec.creation_time > 0) j["creation_time"] = spec.creation_time;
    if (spec.last_boot_time > 0) j["last_boot_time"] = spec.last_boot_time;

    json fwds = json::array();
    for (const auto& f : spec.port_forwards) {
        fwds.push_back({{"host_port", f.host_port}, {"guest_port", f.guest_port}});
    }
    j["port_forwards"] = fwds;

    json shared = json::array();
    for (const auto& sf : spec.shared_folders) {
        shared.push_back({
            {"tag", sf.tag},
            {"host_path", sf.host_path},
            {"readonly", sf.readonly}
        });
    }
    j["shared_folders"] = shared;

    auto path = fs::path(spec.vm_dir) / "vm.json";
    std::ofstream ofs(path, std::ios::trunc);
    if (ofs) ofs << j.dump(2) << '\n';
}

}  // namespace settings
