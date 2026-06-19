#include "daemon/rpc_server.h"

#include "daemon/resource_monitor.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <grp.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <unistd.h>
#endif

namespace tenbox::daemon {
namespace fs = std::filesystem;

namespace {

nlohmann::json Ok(nlohmann::json payload = nlohmann::json::object()) {
    return {{"ok", true}, {"payload", std::move(payload)}};
}

nlohmann::json Error(std::string code, std::string message) {
    return {{"ok", false}, {"error_code", std::move(code)}, {"error", std::move(message)}};
}

std::string PathToUtf8(const fs::path& path) {
    auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

bool CopyIfProvided(const std::string& source, const fs::path& destination_dir, std::string* out, std::string* error) {
    if (source.empty()) return true;
    std::error_code ec;
    fs::path src(source);
    if (!fs::exists(src, ec)) {
        if (error) *error = "source file not found: " + source;
        return false;
    }
    fs::path dst = destination_dir / src.filename();
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = "failed to copy " + source + ": " + ec.message();
        return false;
    }
    *out = PathToUtf8(dst);
    return true;
}

std::vector<uint8_t> HexDecode(const std::string& value) {
    auto digit = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
        if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
        return -1;
    };
    if ((value.size() % 2) != 0) return {};
    std::vector<uint8_t> out(value.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        int hi = digit(value[i * 2]);
        int lo = digit(value[i * 2 + 1]);
        if (hi < 0 || lo < 0) return {};
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return out;
}

}  // namespace

RpcServer::RpcServer(DaemonConfig config, VmStore& store, RuntimeManager& runtime_manager)
    : config_(std::move(config)), store_(store), runtime_manager_(runtime_manager) {}

RpcServer::~RpcServer() {
    Stop();
}

bool RpcServer::Start(std::string* error) {
    std::error_code ec;
    fs::create_directories(fs::path(config_.socket_path).parent_path(), ec);
    if (ec) {
        if (error) *error = "failed to create socket directory: " + ec.message();
        return false;
    }
    if (!server_.Listen(config_.socket_path)) {
        if (error) *error = "failed to listen on " + config_.socket_path;
        return false;
    }
    ApplySocketPermissions();
    running_ = true;
    return true;
}

void RpcServer::ApplySocketPermissions() {
#if defined(__unix__) || defined(__APPLE__)
    // Tighten permissions on the listening socket so that, on a
    // system-wide install (typically /run/tenbox/tenbox.sock), the
    // socket file is group-readable/writable by members of the
    // `tenbox` system group only — matching the libvirt / docker
    // model. The group is communicated via AGENTSPHERE_SOCKET_GROUP, set
    // by agentsphered.service. When the env var is unset (developer dev
    // run, XDG_RUNTIME_DIR socket) we leave permissions alone so the
    // user's umask wins.
    const char* group_name = std::getenv("AGENTSPHERE_SOCKET_GROUP");
    if (group_name == nullptr || *group_name == '\0') return;

    struct group* gr = ::getgrnam(group_name);
    if (gr == nullptr) {
        // Group missing is non-fatal: postinst should have created it,
        // but if a sysadmin nuked it the daemon should still come up
        // (just root-only) rather than refuse to start.
        return;
    }

    // Order matters: chown first, then chmod. If chmod 0660 landed
    // before chgrp, there'd be a brief window where the socket was
    // group-read/writable by *root's* primary group.
    if (::chown(config_.socket_path.c_str(), static_cast<uid_t>(-1), gr->gr_gid) != 0) {
        // Same rationale as above — log-and-continue rather than fail.
        // (The kernel only blocks chown if we're not root, which on
        // agentsphered we always are today.)
        return;
    }
    ::chmod(config_.socket_path.c_str(), 0660);
#endif
}

void RpcServer::Run() {
    while (running_) {
        auto client = server_.Accept();
        if (!client.IsValid()) continue;
        std::thread(&RpcServer::HandleClient, this, std::move(client)).detach();
    }
}

void RpcServer::Stop() {
    running_ = false;
    server_.Close();
}

void RpcServer::HandleClient(ipc::UnixSocketConnection client) {
    pthread_setname_np(pthread_self(), "rpc-handler");
    const std::string line = client.ReadLine();
    if (line.empty()) return;
    auto request = nlohmann::json::parse(line, nullptr, false);
    if (request.is_discarded()) {
        auto response = Error("bad_json", "request is not valid JSON").dump() + "\n";
        (void)client.Send(response);
        return;
    }

    if (request.value("type", "") == "vm.console.attach") {
        std::string error;
        if (!runtime_manager_.AttachConsole(request.value("vm_id", ""), &client, &error)) {
            auto response = Error("console_attach_failed", error).dump() + "\n";
            (void)client.Send(response);
            return;
        }
        return;
    }

    if (request.value("type", "") == "vm.logs.follow") {
        std::string error;
        if (!runtime_manager_.AttachLogFollower(request.value("vm_id", ""), &client, &error)) {
            auto response = Error("logs_follow_failed", error).dump() + "\n";
            (void)client.Send(response);
            return;
        }
        return;
    }

    auto response = HandleRequest(request).dump();
    response.push_back('\n');
    (void)client.Send(response);
}

nlohmann::json RpcServer::HandleRequest(const nlohmann::json& request) {
    const std::string type = request.value("type", "");
    if (type == "doctor") {
        return Ok(ToJson(RunKvmDoctor()));
    }
    if (type == "system.info") {
        return Ok({
            {"data_dir", config_.data_dir},
            {"socket_path", config_.socket_path},
            {"runtime_path", config_.runtime_path},
            {"resources", ToJson(ReadHostResources(config_.data_dir))},
            {"doctor", ToJson(RunKvmDoctor())},
        });
    }
    if (type == "vm.list") {
        nlohmann::json vms = nlohmann::json::array();
        for (const auto& vm : store_.List()) {
            auto item = ToJson(vm);
            item["resources"] = VmResources(vm);
            vms.push_back(std::move(item));
        }
        return Ok({{"vms", std::move(vms)}});
    }
    if (type == "vm.create") {
        return CreateVm(request);
    }
    if (type == "vm.edit") {
        return EditVm(request);
    }
    if (type == "vm.delete") {
        const std::string vm_id = request.value("vm_id", "");
        // Stop the runtime first so qemu/runtime stops touching the vm_dir
        // before we tear it down; otherwise the child process keeps writing
        // logs and VmStore::UpdateRuntime keeps emitting runtime_state.json,
        // racing the directory removal and resurrecting an orphan dir.
        if (auto record = store_.Get(vm_id); record &&
            (record->runtime.state == VmState::kRunning ||
             record->runtime.state == VmState::kStarting ||
             record->runtime.state == VmState::kStopping ||
             record->runtime.state == VmState::kRebooting)) {
            std::string stop_error;
            runtime_manager_.StopVm(vm_id, &stop_error);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline) {
                auto current = store_.Get(vm_id);
                if (!current || current->runtime.state == VmState::kStopped ||
                    current->runtime.state == VmState::kCrashed) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        std::string error;
        if (!store_.Remove(vm_id, &error)) return Error("vm_delete_failed", error);
        return Ok();
    }
    if (type == "vm.start") {
        const std::string vm_id = request.value("vm_id", "");
        std::string error;
        if (!runtime_manager_.StartVm(vm_id, &error)) {
            std::string code = "vm_start_failed";
            if (auto record = store_.Get(vm_id); record && record->runtime.last_failure) {
                code = record->runtime.last_failure->code;
            }
            return Error(code, error);
        }
        return Ok();
    }
    if (type == "vm.stop") {
        std::string error;
        if (!runtime_manager_.StopVm(request.value("vm_id", ""), &error)) return Error("vm_stop_failed", error);
        return Ok();
    }
    if (type == "vm.reboot" || type == "vm.shutdown") {
        // Graceful guest-driven path. RebootVm / ShutdownVm internally gate
        // on `guest_agent_connected`, so we surface that as a structured
        // error with a stable code so callers (CLI) can show "guest agent
        // not ready, use vm stop for hard kill" rather than a generic
        // failure.
        const std::string vm_id = request.value("vm_id", "");
        if (auto record = store_.Get(vm_id)) {
            if (!record->runtime.guest_agent_connected) {
                return Error("guest_agent_not_ready",
                             "guest agent not connected; use `vm stop` to force a hard kill");
            }
        } else {
            return Error("vm_not_found", "VM not found");
        }
        std::string error;
        const bool ok = (type == "vm.reboot")
            ? runtime_manager_.RebootVm(vm_id, &error)
            : runtime_manager_.ShutdownVm(vm_id, &error);
        if (!ok) return Error(type == "vm.reboot" ? "vm_reboot_failed" : "vm_shutdown_failed", error);
        return Ok();
    }
    if (type == "vm.logs") {
        return Ok(runtime_manager_.Logs(request.value("vm_id", ""), request.value("lines", 200)));
    }
    if (type == "remote_session.create") {
        const auto session = remote_sessions_.Create(
            request.value("vm_id", ""),
            request.value("owner_user_id", "local"),
            request.value("force", false));
        if (!session) return Error("remote_session_conflict", "VM already has an active remote session");
        return Ok(ToJson(*session));
    }
    if (type == "remote_session.close") {
        if (!remote_sessions_.Close(request.value("vm_id", ""), request.value("session_id", ""))) {
            return Error("remote_session_not_found", "remote session not found");
        }
        return Ok();
    }
    if (type == "console.input") {
        const auto bytes = HexDecode(request.value("data_hex", ""));
        if (!runtime_manager_.SendConsoleInput(request.value("vm_id", ""), bytes)) {
            return Error("console_input_failed", "failed to send console input");
        }
        return Ok();
    }
    return Error("unknown_request", "unknown request type: " + type);
}

nlohmann::json RpcServer::CreateVm(const nlohmann::json& request) {
    const auto payload = request.value("payload", nlohmann::json::object());
    VmSpec spec;
    spec.vm_id = GenerateUuid();
    spec.name = payload.value("name", spec.vm_id);
    spec.cmdline = "";
    spec.memory_mb = payload.value("memory_mb", static_cast<uint64_t>(4096));
    spec.cpu_count = payload.value("cpu_count", static_cast<uint32_t>(4));
    spec.nat_enabled = payload.value("nat_enabled", true);
    spec.debug_mode = payload.value("debug_mode", false);
    spec.creation_time = UnixNow();
    spec.vm_dir = PathToUtf8(store_.VmRoot() / spec.vm_id);

    std::error_code ec;
    fs::create_directories(spec.vm_dir, ec);
    if (ec) return Error("vm_create_failed", "failed to create VM directory: " + ec.message());

    std::string error;
    if (!CopyIfProvided(payload.value("kernel", ""), spec.vm_dir, &spec.kernel_path, &error) ||
        !CopyIfProvided(payload.value("initrd", ""), spec.vm_dir, &spec.initrd_path, &error) ||
        !CopyIfProvided(payload.value("disk", ""), spec.vm_dir, &spec.disk_path, &error)) {
        return Error("vm_create_failed", error);
    }
    if (spec.kernel_path.empty()) return Error("vm_create_failed", "kernel path is required");

    VmRecord created;
    if (!store_.Create(spec, &created, &error)) return Error("vm_create_failed", error);
    return Ok(ToJson(created));
}

nlohmann::json RpcServer::EditVm(const nlohmann::json& request) {
    const std::string vm_id = request.value("vm_id", "");
    auto record = store_.Get(vm_id);
    if (!record) return Error("vm_not_found", "VM not found");

    const auto payload = request.value("payload", nlohmann::json::object());
    VmSpec spec = record->spec;
    const bool running = record->runtime.state == VmState::kRunning ||
                         record->runtime.state == VmState::kStarting;

    if (payload.contains("name")) spec.name = payload.value("name", spec.name);
    if (payload.contains("memory_mb")) spec.memory_mb = payload.value("memory_mb", spec.memory_mb);
    if (payload.contains("cpu_count")) spec.cpu_count = payload.value("cpu_count", spec.cpu_count);
    if (payload.contains("net_enabled")) spec.nat_enabled = payload.value("net_enabled", spec.nat_enabled);
    const bool patch_net_enabled =
        payload.contains("net_enabled") && spec.nat_enabled != record->spec.nat_enabled;
    if (payload.contains("debug_mode")) spec.debug_mode = payload.value("debug_mode", spec.debug_mode);

    // Accept the new `host_forwards` key, with a `port_forwards` fallback for
    // older clients (mirrors the cloud tunnel's EditVm path).
    const auto* host_forwards_payload =
        payload.contains("host_forwards") && payload["host_forwards"].is_array() ? &payload["host_forwards"]
        : payload.contains("port_forwards") && payload["port_forwards"].is_array() ? &payload["port_forwards"]
        : nullptr;
    const bool patch_host_forwards = host_forwards_payload != nullptr;
    if (patch_host_forwards) {
        spec.host_forwards.clear();
        for (const auto& item : *host_forwards_payload) {
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

    const bool patch_guest_forwards =
        payload.contains("guest_forwards") && payload["guest_forwards"].is_array();
    if (patch_guest_forwards) {
        spec.guest_forwards.clear();
        for (const auto& item : payload["guest_forwards"]) {
            if (!item.is_object()) continue;
            GuestForward gf;
            const std::string guest_ip_str = item.value("guest_ip", "");
            if (guest_ip_str.empty() ||
                !GuestForward::Ip4FromString(guest_ip_str, gf.guest_ip)) {
                continue;
            }
            gf.guest_port = item.value("guest_port", 0);
            gf.host_addr = item.value("host_addr", "");
            gf.host_port = item.value("host_port", 0);
            if (gf.guest_port != 0 && gf.host_port != 0) {
                spec.guest_forwards.push_back(std::move(gf));
            }
        }
    }

    const bool patch_shared_folders =
        payload.contains("shared_folders") && payload["shared_folders"].is_array();
    if (patch_shared_folders) {
        spec.shared_folders.clear();
        for (const auto& item : payload["shared_folders"]) {
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

    if (spec.name.empty()) return Error("vm_edit_invalid", "name cannot be empty");
    if (spec.memory_mb < 16) return Error("vm_edit_invalid", "memory_mb must be at least 16");
    if (spec.cpu_count < 1 || spec.cpu_count > 128) {
        return Error("vm_edit_invalid", "cpu_count must be between 1 and 128");
    }

    // memory_mb / cpu_count change the QEMU command line and need a restart;
    // shared_folders / host_forwards / guest_forwards are hot-applied below.
    // We compare new vs old rather than payload.contains(...) because the
    // console sends every basic-tab field on every save, so a presence check
    // would block forward / shared-folder edits while running.
    if (running &&
        (spec.memory_mb != record->spec.memory_mb ||
         spec.cpu_count != record->spec.cpu_count ||
         spec.debug_mode != record->spec.debug_mode)) {
        return Error("vm_edit_requires_stopped", "memory/cpu/debug require the VM to be stopped");
    }

    std::string error;
    if (!store_.UpdateSpec(vm_id, spec, &error)) return Error("vm_edit_failed", error);

    // Persisted spec is now authoritative; if the runtime is up, push the
    // diffable subset live. Failures here are logged-but-not-fatal: the spec
    // is saved, so the next start (or a reboot) will pick up the change.
    if (running && (patch_host_forwards || patch_guest_forwards)) {
        runtime_manager_.ApplyForwards(vm_id);
    }
    if (running && patch_shared_folders) runtime_manager_.ApplySharedFolders(vm_id);
    if (running && patch_net_enabled) runtime_manager_.ApplyNetLink(vm_id, spec.nat_enabled);

    auto updated = store_.Get(vm_id);
    return Ok(updated ? ToJson(*updated) : ToJson(VmRecord{.spec = spec}));
}

nlohmann::json RpcServer::VmResources(const VmRecord& record) const {
    nlohmann::json out = {
        {"disk_usage_bytes", DirectorySizeBytes(record.spec.vm_dir)},
    };
    if (record.runtime.pid > 0 && record.runtime.state == VmState::kRunning) {
        out["process"] = ToJson(runtime_manager_.SampleProcessResources(record.spec.vm_id));
    }
    return out;
}

}  // namespace tenbox::daemon
