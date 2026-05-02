#include "daemon/runtime_manager.h"

#include "daemon/resource_monitor.h"

#ifdef TENBOX_ENABLE_LIBYUV
#include <libyuv.h>
#endif

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>

namespace tenbox::daemon {
namespace fs = std::filesystem;

namespace {

struct DirtyRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

std::string HexEncode(const std::vector<uint8_t>& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(bytes.size() * 2, '\0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        out[i * 2] = kDigits[bytes[i] >> 4];
        out[i * 2 + 1] = kDigits[bytes[i] & 0x0f];
    }
    return out;
}

std::string Base64Encode(const std::vector<uint8_t>& bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t b0 = bytes[i];
        const uint32_t b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const uint32_t b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const uint32_t value = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kAlphabet[(value >> 18) & 0x3f]);
        out.push_back(kAlphabet[(value >> 12) & 0x3f]);
        out.push_back(i + 1 < bytes.size() ? kAlphabet[(value >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < bytes.size() ? kAlphabet[value & 0x3f] : '=');
    }
    return out;
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

int FieldInt(const ipc::Message& message, const std::string& key, int fallback = 0) {
    auto it = message.fields.find(key);
    if (it == message.fields.end()) return fallback;
    return std::atoi(it->second.c_str());
}

uint32_t FieldU32(const ipc::Message& message, const std::string& key, uint32_t fallback = 0) {
    auto it = message.fields.find(key);
    if (it == message.fields.end()) return fallback;
    return static_cast<uint32_t>(std::strtoul(it->second.c_str(), nullptr, 10));
}

uint64_t FieldU64(const ipc::Message& message, const std::string& key, uint64_t fallback = 0) {
    auto it = message.fields.find(key);
    if (it == message.fields.end()) return fallback;
    return static_cast<uint64_t>(std::strtoull(it->second.c_str(), nullptr, 10));
}

DirtyRect NormalizeDirtyRect(uint32_t x,
                             uint32_t y,
                             uint32_t width,
                             uint32_t height,
                             uint32_t frame_width,
                             uint32_t frame_height,
                             bool force_full_frame) {
    if (force_full_frame || width == 0 || height == 0 || x >= frame_width || y >= frame_height) {
        return {0, 0, frame_width, frame_height};
    }
    uint32_t right = std::min(frame_width, x + width);
    uint32_t bottom = std::min(frame_height, y + height);

    x &= ~uint32_t{1};
    y &= ~uint32_t{1};
    right = std::min(frame_width, (right + 1) & ~uint32_t{1});
    bottom = std::min(frame_height, (bottom + 1) & ~uint32_t{1});
    if (right <= x || bottom <= y) return {0, 0, frame_width, frame_height};
    return {x, y, right - x, bottom - y};
}

std::string PathToUtf8(const fs::path& path) {
    auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

}  // namespace

RuntimeManager::RuntimeManager(DaemonConfig config, VmStore& store)
    : config_(std::move(config)), store_(store) {
    reboot_worker_ = std::thread(&RuntimeManager::RebootWorkerLoop, this);
}

RuntimeManager::~RuntimeManager() {
    {
        std::lock_guard<std::mutex> lock(reboot_mutex_);
        reboot_shutdown_ = true;
        reboot_cv_.notify_all();
    }
    if (reboot_worker_.joinable()) reboot_worker_.join();

    std::vector<std::shared_ptr<RuntimeSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [_, session] : sessions_) sessions.push_back(session);
        sessions_.clear();
    }
    for (auto& session : sessions) {
        session->running = false;
        session->remote_frame_cv.notify_all();
        if (session->process_pid > 0) {
            ::kill(session->process_pid, SIGTERM);
        }
    }
}

void RuntimeManager::RebootWorkerLoop() {
    while (true) {
        std::string vm_id;
        {
            std::unique_lock<std::mutex> lock(reboot_mutex_);
            reboot_cv_.wait(lock, [this]() {
                return reboot_shutdown_ || !reboot_queue_.empty();
            });
            if (reboot_queue_.empty()) {
                if (reboot_shutdown_) return;
                continue;
            }
            vm_id = std::move(reboot_queue_.front());
            reboot_queue_.pop_front();
        }
        std::string err;
        if (!StartVm(vm_id, &err)) {
            FailureInfo info{
                .code = "reboot_failed",
                .message = "failed to relaunch runtime after guest reboot: " + err,
            };
            store_.SetFailure(vm_id, info);
            if (auto record = store_.Get(vm_id)) {
                NotifyStateChanged(vm_id, record->runtime);
            }
        }
    }
}

namespace {

constexpr uint64_t kLogRotateMaxBytes = 10ull * 1024 * 1024;  // 10 MB
constexpr int kLogRotateKeep = 2;  // keep .1 and .2

fs::path RuntimeLogPath(const std::string& vm_dir) {
    return fs::path(vm_dir) / "logs" / "runtime.log";
}

// Rotate runtime.log to runtime.log.1, runtime.log.1 to runtime.log.2, etc.,
// only when the active file already exceeds kLogRotateMaxBytes. Called once
// before each VM boot so each session starts with at most kLogRotateMaxBytes
// of historical content in the active file. Older files past `kLogRotateKeep`
// are discarded.
void RotateLogsIfNeeded(const fs::path& base) {
    std::error_code ec;
    if (!fs::exists(base, ec)) return;
    const auto size = fs::file_size(base, ec);
    if (ec || size <= kLogRotateMaxBytes) return;
    // Drop the oldest before shifting.
    const fs::path oldest = base.string() + "." + std::to_string(kLogRotateKeep);
    fs::remove(oldest, ec);
    for (int i = kLogRotateKeep; i >= 1; --i) {
        fs::path src = (i == 1) ? base : fs::path(base.string() + "." + std::to_string(i - 1));
        fs::path dst = base.string() + "." + std::to_string(i);
        if (fs::exists(src, ec)) {
            fs::rename(src, dst, ec);
        }
    }
}

// Read up to `max_lines` final lines from a log file by walking backwards a
// chunk at a time. Returns lines in chronological order (oldest first). Used
// by `RuntimeManager::Logs` when no live session exists, so the operator can
// still read crash output after the VM is gone.
std::vector<std::string> TailLogFile(const fs::path& path, size_t max_lines) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    input.seekg(0, std::ios::end);
    std::streamoff size = input.tellg();
    if (size <= 0) return {};

    constexpr std::streamoff kChunk = 8192;
    std::string buffer;
    std::streamoff pos = size;
    size_t newlines = 0;
    while (pos > 0 && newlines <= max_lines) {
        const std::streamoff read_size = std::min(kChunk, pos);
        pos -= read_size;
        input.seekg(pos);
        std::string chunk(static_cast<size_t>(read_size), '\0');
        input.read(chunk.data(), read_size);
        buffer.insert(0, chunk);
        newlines = std::count(buffer.begin(), buffer.end(), '\n');
    }

    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i < buffer.size(); ++i) {
        if (buffer[i] == '\n') {
            lines.emplace_back(buffer.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < buffer.size()) lines.emplace_back(buffer.substr(start));
    if (lines.size() > max_lines) lines.erase(lines.begin(), lines.end() - max_lines);
    return lines;
}

// Returns true if the host port can currently be bound (i.e. is free). We
// open a TCP socket with SO_REUSEADDR off and try to bind to the requested
// host_ip:host_port; success means the port is available right now. This is
// best-effort: a race between this probe and qemu's own bind is possible but
// vanishingly small in practice, and false positives degrade to the runtime's
// own (unstructured) bind error.
bool HostPortAvailable(const std::string& host_ip_str, uint16_t port) {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return true;  // Fail open: don't block start on probe failure.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    const std::string ip = host_ip_str.empty() ? std::string("127.0.0.1") : host_ip_str;
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        ::close(s);
        return true;
    }
    const int rc = ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::close(s);
    return rc == 0;
}

bool PathReadable(const std::string& path) {
    if (path.empty()) return true;
    return ::access(path.c_str(), R_OK) == 0;
}

}  // namespace

std::optional<RuntimeManager::StartFailure> RuntimeManager::ValidateStart(const VmSpec& spec) const {
    const auto doctor = RunKvmDoctor();
    if (!doctor.supported) {
        std::string detail = "KVM is not available on this host";
        for (const auto& check : doctor.checks) {
            if (!check.ok && !check.message.empty()) {
                detail += "; " + check.id + ": " + check.message;
            }
        }
        return StartFailure{"kvm_unsupported", std::move(detail)};
    }
    std::error_code ec;
    if (spec.kernel_path.empty() || !fs::exists(spec.kernel_path, ec)) {
        return StartFailure{"kernel_missing", "kernel image not found: " + spec.kernel_path};
    }
    if (!PathReadable(spec.kernel_path)) {
        return StartFailure{"permission_denied", "cannot read kernel image: " + spec.kernel_path};
    }
    if (!spec.initrd_path.empty()) {
        if (!fs::exists(spec.initrd_path, ec)) {
            return StartFailure{"initrd_missing", "initrd not found: " + spec.initrd_path};
        }
        if (!PathReadable(spec.initrd_path)) {
            return StartFailure{"permission_denied", "cannot read initrd: " + spec.initrd_path};
        }
    }
    if (!spec.disk_path.empty()) {
        if (!fs::exists(spec.disk_path, ec)) {
            return StartFailure{"disk_missing", "disk image not found: " + spec.disk_path};
        }
        if (!PathReadable(spec.disk_path)) {
            return StartFailure{"permission_denied", "cannot read disk image: " + spec.disk_path};
        }
    }
    const auto host = ReadHostResources(config_.data_dir);
    const uint64_t requested = spec.memory_mb * 1024ull * 1024ull;
    const uint64_t reserve = 512ull * 1024ull * 1024ull;
    if (host.memory_available_bytes > 0 && requested + reserve > host.memory_available_bytes) {
        std::ostringstream msg;
        msg << "not enough available memory: requested " << spec.memory_mb << " MB"
            << " (+ " << (reserve / (1024 * 1024)) << " MB reserve), host has only "
            << (host.memory_available_bytes / (1024 * 1024)) << " MB available";
        return StartFailure{"insufficient_memory", msg.str()};
    }
    for (const auto& pf : spec.host_forwards) {
        if (!HostPortAvailable(pf.EffectiveHostIp(), pf.host_port)) {
            std::ostringstream msg;
            msg << "host port already in use: " << pf.EffectiveHostIp() << ":" << pf.host_port;
            return StartFailure{"port_conflict", msg.str()};
        }
    }
    return std::nullopt;
}

std::vector<std::string> RuntimeManager::BuildRuntimeArgs(
    const VmSpec& spec,
    const std::string& control_socket) const {
    std::vector<std::string> args = {
        config_.runtime_path,
        "--vm-id", spec.vm_id,
        "--vm-dir", spec.vm_dir,
        "--control-endpoint", control_socket,
        "--interactive", "off",
        "--kernel", spec.kernel_path,
        "--memory", std::to_string(spec.memory_mb),
        "--cpus", std::to_string(spec.cpu_count),
    };
    if (!spec.initrd_path.empty()) {
        args.push_back("--initrd");
        args.push_back(spec.initrd_path);
    }
    if (!spec.disk_path.empty()) {
        args.push_back("--disk");
        args.push_back(spec.disk_path);
    }
    if (!spec.cmdline.empty()) {
        args.push_back("--cmdline");
        args.push_back(spec.cmdline);
    }
    if (spec.nat_enabled) {
        args.push_back("--net");
    }
    if (spec.debug_mode) {
        args.push_back("--debug");
    }
    for (const auto& pf : spec.host_forwards) {
        args.push_back("--hostfwd");
        args.push_back(pf.ToHostfwd());
    }
    for (const auto& gf : EffectiveGuestForwards(spec)) {
        args.push_back("--guestfwd");
        args.push_back(gf.ToGuestfwd());
    }
    for (const auto& sf : spec.shared_folders) {
        args.push_back("--share");
        args.push_back(sf.tag + ":" + sf.host_path + (sf.readonly ? ":ro" : ""));
    }
    return args;
}

std::vector<GuestForward> RuntimeManager::EffectiveGuestForwards(const VmSpec& spec) const {
    // Canonical guest-side endpoint that the LLM reverse proxy is
    // exposed at. Hardcoded to match the console hint, the openclaw /
    // hermes rootfs configs, and the manager-side launcher so guests
    // can address the proxy at a fixed http://10.0.2.3/ regardless of
    // which random host port the LlmProxyService grabbed.
    constexpr uint32_t kLlmGuestIp = 0x0A000203;  // 10.0.2.3
    constexpr uint16_t kLlmGuestPort = 80;

    std::vector<GuestForward> result;
    result.reserve(spec.guest_forwards.size() + 1);
    for (const auto& gf : spec.guest_forwards) {
        // Drop any user-supplied guestfwd that collides on 10.0.2.3:80
        // so slirp does not reject the duplicate, and so a stale rule
        // cannot accidentally divert LLM traffic away from the host
        // proxy.
        if (gf.guest_ip == kLlmGuestIp && gf.guest_port == kLlmGuestPort) continue;
        result.push_back(gf);
    }

    uint16_t llm_port = 0;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (llm_proxy_port_provider_) llm_port = llm_proxy_port_provider_();
    }
    if (llm_port != 0) {
        GuestForward gf;
        gf.guest_ip = kLlmGuestIp;
        gf.guest_port = kLlmGuestPort;
        gf.host_addr = "127.0.0.1";
        gf.host_port = llm_port;
        result.push_back(std::move(gf));
    }
    return result;
}

bool RuntimeManager::StartVm(const std::string& vm_id, std::string* error) {
    auto record = store_.Get(vm_id);
    if (!record) {
        if (error) *error = "VM not found";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sessions_.count(vm_id)) {
            if (error) *error = "VM is already running";
            return false;
        }
    }
    if (auto failure = ValidateStart(record->spec)) {
        if (error) *error = failure->message;
        store_.SetFailure(vm_id, FailureInfo{.code = failure->code, .message = failure->message});
        return false;
    }

    auto session = std::make_shared<RuntimeSession>();
    session->spec = record->spec;
    session->control_socket = (fs::path(config_.data_dir) / "run" / (vm_id + ".sock")).string();
    session->control_server = std::make_unique<ipc::UnixSocketServer>();
    std::error_code ec;
    fs::create_directories(fs::path(config_.data_dir) / "run", ec);
    if (ec || !session->control_server->Listen(session->control_socket)) {
        const std::string detail = "failed to create runtime control socket: " + session->control_socket;
        if (error) *error = detail;
        store_.SetFailure(vm_id, FailureInfo{.code = "runtime_control_socket_failed", .message = detail});
        return false;
    }

    const fs::path log_path = RuntimeLogPath(record->spec.vm_dir);
    fs::create_directories(log_path.parent_path(), ec);
    RotateLogsIfNeeded(log_path);
    session->log_file.open(log_path, std::ios::app | std::ios::binary);
    if (session->log_file) {
        session->log_file << "=== boot " << UnixNow() << " ===\n";
        session->log_file.flush();
    }

    int log_pipe[2];
    if (::pipe(log_pipe) != 0) {
        if (error) *error = "failed to create runtime log pipe";
        store_.SetFailure(vm_id, FailureInfo{.code = "runtime_spawn_failed", .message = "failed to create runtime log pipe"});
        return false;
    }

    const auto args = BuildRuntimeArgs(record->spec, session->control_socket);
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(log_pipe[0]);
        ::close(log_pipe[1]);
        if (error) *error = "failed to fork runtime process";
        store_.SetFailure(vm_id, FailureInfo{.code = "runtime_spawn_failed", .message = "failed to fork runtime process"});
        return false;
    }
    if (pid == 0) {
        // Ensure runtime exits if tenboxd dies unexpectedly.
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (::getppid() == 1) _exit(128);
        ::dup2(log_pipe[1], STDOUT_FILENO);
        ::dup2(log_pipe[1], STDERR_FILENO);
        ::close(log_pipe[0]);
        ::close(log_pipe[1]);
        ::execvp(config_.runtime_path.c_str(), argv.data());
        _exit(127);
    }

    ::close(log_pipe[1]);
    session->process_pid = pid;
    session->log_pipe_fd = log_pipe[0];
    session->running = true;
    session->info.pid = pid;
    session->info.state = VmState::kStarting;
    session->info.started_at = UnixNow();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_[vm_id] = session;
    }
    store_.UpdateRuntime(vm_id, session->info);
    NotifyStateChanged(vm_id, session->info);

    session->accept_thread = std::thread(&RuntimeManager::AcceptRuntime, this, session);
    session->accept_thread.detach();
    session->log_thread = std::thread(&RuntimeManager::ReadLogs, this, session);
    session->log_thread.detach();
    return true;
}

bool RuntimeManager::StopVm(const std::string& vm_id, std::string* error) {
    auto session = FindSession(vm_id);
    if (!session) {
        if (error) *error = "VM is not running";
        return false;
    }
    if (session->process_pid > 0) {
        session->stop_requested = true;
        ::kill(session->process_pid, SIGTERM);
    }
    session->info.state = VmState::kStopping;
    session->info.guest_agent_connected = false;
    store_.UpdateRuntime(vm_id, session->info);
    NotifyStateChanged(vm_id, session->info);
    return true;
}

bool RuntimeManager::RebootVm(const std::string& vm_id, std::string* error) {
    auto session = FindSession(vm_id);
    if (!session) {
        if (error) *error = "VM is not running";
        return false;
    }
    ipc::Message message;
    message.channel = ipc::Channel::kControl;
    message.kind = ipc::Kind::kRequest;
    message.type = "runtime.command";
    message.vm_id = vm_id;
    message.fields["command"] = "reboot";
    if (!SendRuntime(session, message)) {
        if (error) *error = "failed to deliver reboot to runtime";
        return false;
    }
    return true;
}

bool RuntimeManager::ShutdownVm(const std::string& vm_id, std::string* error) {
    auto session = FindSession(vm_id);
    if (!session) {
        if (error) *error = "VM is not running";
        return false;
    }
    ipc::Message message;
    message.channel = ipc::Channel::kControl;
    message.kind = ipc::Kind::kRequest;
    message.type = "runtime.command";
    message.vm_id = vm_id;
    message.fields["command"] = "shutdown";
    if (!SendRuntime(session, message)) {
        if (error) *error = "failed to deliver shutdown to runtime";
        return false;
    }
    return true;
}

void RuntimeManager::AcceptRuntime(std::shared_ptr<RuntimeSession> session) {
    auto conn = session->control_server->Accept();
    if (!conn.IsValid()) return;
    session->runtime_conn = std::make_unique<ipc::UnixSocketConnection>(std::move(conn));
    session->reader_thread = std::thread(&RuntimeManager::ReadRuntime, this, session);
    session->reader_thread.detach();
}

void RuntimeManager::ReadRuntime(std::shared_ptr<RuntimeSession> session) {
    while (session->running && session->runtime_conn && session->runtime_conn->IsValid()) {
        const std::string header = session->runtime_conn->ReadLine();
        if (header.empty()) break;
        auto message = ipc::Decode(header);
        if (!message) continue;
        auto payload_size_it = message->fields.find("payload_size");
        if (payload_size_it != message->fields.end()) {
            const size_t size = static_cast<size_t>(std::stoull(payload_size_it->second));
            message->payload.resize(size);
            if (!session->runtime_conn->ReadExact(message->payload.data(), size)) break;
        }
        HandleRuntimeMessage(session, std::move(*message));
    }
    session->running = false;
    session->remote_frame_cv.notify_all();
}

void RuntimeManager::ReadLogs(std::shared_ptr<RuntimeSession> session) {
    std::string pending;
    char buf[4096];
    while (true) {
        const ssize_t n = ::read(session->log_pipe_fd, buf, sizeof(buf));
        if (n <= 0) break;
        pending.append(buf, static_cast<size_t>(n));
        size_t pos = 0;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            std::vector<std::string> emitted;
            {
                std::lock_guard<std::mutex> lock(session->console_mutex);
                if (session->log_file) {
                    session->log_file << line << '\n';
                    session->log_file.flush();
                }
                session->log_lines.push_back(line);
                while (session->log_lines.size() > 2000) session->log_lines.pop_front();
                emitted.push_back(line);
                // Push to local RPC followers (`tenbox vm logs -f`). Same
                // event shape as the cloud bus so a future merged tail tool
                // can speak one format. Reap dead followers eagerly so a
                // disconnected CLI does not retain its slot indefinitely.
                if (!session->log_followers.empty()) {
                    nlohmann::json event = {
                        {"type", "vm.logs.append"},
                        {"vm_id", session->spec.vm_id},
                        {"payload", {{"lines", nlohmann::json::array({line})}}},
                    };
                    std::string framed = event.dump();
                    framed.push_back('\n');
                    for (auto it = session->log_followers.begin(); it != session->log_followers.end();) {
                        if (!(*it)->Send(framed)) it = session->log_followers.erase(it);
                        else ++it;
                    }
                }
            }
            // Notify subscribers (cloud_tunnel / RPC) outside the mutex so a
            // slow consumer cannot stall the runtime's stdout pipe and back
            // pressure the VM into stalling itself.
            LogAppendCallback cb;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                cb = log_append_callback_;
            }
            if (cb) cb(session->spec.vm_id, emitted);
        }
    }

    int status = 0;
    if (session->process_pid > 0) {
        (void)::waitpid(session->process_pid, &status, 0);
        process_sampler_.Forget(session->process_pid);
    }
    // Distinguish "process exited normally with status N" (WIFEXITED) from
    // "killed by signal N" (WIFSIGNALED). The previous fallback bucketed
    // signal kills into exit_code=128, colliding with the runtime's own
    // "guest requested reboot" sentinel and leading to bogus "crashed"
    // banners. We now use a separate negative range for signals.
    const bool exited_normally = WIFEXITED(status);
    const int exit_status = exited_normally ? WEXITSTATUS(status) : -1;
    const int term_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;

    // Exit code 128 from `tenbox-vm-runtime` means the guest issued a reboot
    // (see src/runtime/main.cpp). Re-spawn the runtime in place so the user
    // sees a Rebooting -> Running transition rather than a misleading
    // "Runtime crashed" banner. Don't auto-restart if the user explicitly
    // asked us to stop the VM; in that case the 128 is incidental.
    const bool wants_reboot = exited_normally && exit_status == 128 && !session->stop_requested;

    session->info.exit_code = exited_normally ? exit_status : -term_signal;
    session->info.pid = 0;
    session->info.guest_agent_connected = false;

    if (wants_reboot) {
        session->info.state = VmState::kRebooting;
        session->info.last_failure.reset();
    } else if (session->stop_requested || (exited_normally && exit_status == 0)) {
        session->info.state = VmState::kStopped;
    } else {
        session->info.state = VmState::kCrashed;
        std::string detail;
        if (exited_normally) {
            detail = "tenbox-vm-runtime exited with code " + std::to_string(exit_status);
        } else if (term_signal != 0) {
            detail = "tenbox-vm-runtime killed by signal " + std::to_string(term_signal);
        } else {
            detail = "tenbox-vm-runtime terminated abnormally";
        }
        session->info.last_failure = FailureInfo{
            .code = "runtime_crashed",
            .message = std::move(detail),
        };
    }
    store_.UpdateRuntime(session->spec.vm_id, session->info);
    NotifyStateChanged(session->spec.vm_id, session->info);
    BroadcastConsole(session, "");

    const std::string vm_id_to_reboot = wants_reboot ? session->spec.vm_id : std::string();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(session->spec.vm_id);
    }
    if (!vm_id_to_reboot.empty()) {
        // Hand the relaunch off to the long-lived reboot worker. We must NOT
        // call StartVm directly from this log thread (or a fresh detached
        // thread): the runtime's PR_SET_PDEATHSIG fires when the *fork()-ing
        // thread* exits, so a short-lived spawning thread would deliver
        // SIGTERM to the freshly launched runtime ("killed by signal 15").
        std::lock_guard<std::mutex> lock(reboot_mutex_);
        reboot_queue_.push_back(vm_id_to_reboot);
        reboot_cv_.notify_one();
    }
}

void RuntimeManager::HandleRuntimeMessage(std::shared_ptr<RuntimeSession> session, ipc::Message message) {
    if (message.type == "runtime.state") {
        session->info.state = VmStateFromString(message.fields["state"]);
        if (message.fields.count("exit_code")) {
            session->info.exit_code = std::stoi(message.fields["exit_code"]);
        }
        // Once the runtime confirms it is running, clear any stale preflight
        // failure so the console can stop nagging the user. We deliberately
        // keep `last_failure` set during `kStarting` because that is when an
        // earlier crash banner is most useful.
        if (session->info.state == VmState::kRunning) {
            session->info.last_failure.reset();
        }
        store_.UpdateRuntime(session->spec.vm_id, session->info);
        NotifyStateChanged(session->spec.vm_id, session->info);
        return;
    }
    if (message.type == "guest_agent.state") {
        // Runtime emits "1" / "0"; treat anything non-empty-non-"0" as connected.
        const auto it = message.fields.find("connected");
        const bool connected = (it != message.fields.end()) &&
                               !(it->second.empty() || it->second == "0" || it->second == "false");
        if (session->info.guest_agent_connected != connected) {
            session->info.guest_agent_connected = connected;
            store_.UpdateRuntime(session->spec.vm_id, session->info);
            NotifyStateChanged(session->spec.vm_id, session->info);
        }
        return;
    }
    if (message.type == "console.data") {
        std::string data;
        auto it = message.fields.find("data_hex");
        if (it != message.fields.end()) {
            auto bytes = HexDecode(it->second);
            data.assign(bytes.begin(), bytes.end());
        } else {
            data.assign(message.payload.begin(), message.payload.end());
        }
        BroadcastConsole(session, data);
        return;
    }
    if (message.channel == ipc::Channel::kDisplay) {
        nlohmann::json cursor_event;
        if (message.type == "display.state") {
            std::lock_guard<std::mutex> lock(session->console_mutex);
            session->display_state = {
                {"active", message.fields["active"] == "1" || message.fields["active"] == "true"},
                {"width", FieldU32(message, "width")},
                {"height", FieldU32(message, "height")},
            };
        } else if (message.type == "display.frame_ready") {
            {
                std::lock_guard<std::mutex> lock(session->console_mutex);
                session->last_frame = {
                    {"seq", FieldU64(message, "seq")},
                    {"width", FieldU32(message, "width")},
                    {"height", FieldU32(message, "height")},
                    {"stride", FieldU32(message, "stride")},
                    {"format", message.fields["format"]},
                    {"resource_width", FieldU32(message, "resource_width")},
                    {"resource_height", FieldU32(message, "resource_height")},
                    {"dirty_x", FieldU32(message, "dirty_x")},
                    {"dirty_y", FieldU32(message, "dirty_y")},
                    {"dirty_width", FieldU32(message, "dirty_width", FieldU32(message, "width"))},
                    {"dirty_height", FieldU32(message, "dirty_height", FieldU32(message, "height"))},
                    {"updated_at", UnixNow()},
                };
            }
            // libyuv ARGB->YUV conversion lives under frame_mutex so it does not block
            // log readers, console output, or RemoteRuntimeSnapshot.
            std::lock_guard<std::mutex> frame_lock(session->frame_mutex);
            UpdateRemoteVideoFrameLocked(*session, message);
        } else if (message.type == "display.shm_init") {
            const auto shm_it = message.fields.find("shm_name");
            const uint32_t width = FieldU32(message, "width");
            const uint32_t height = FieldU32(message, "height");
            if (shm_it != message.fields.end() && width > 0 && height > 0) {
                auto framebuffer = std::make_unique<ipc::SharedFramebuffer>();
                const bool opened = framebuffer->Open(shm_it->second, width, height);
                {
                    std::lock_guard<std::mutex> lock(session->console_mutex);
                    session->last_frame["shm_name"] = shm_it->second;
                    session->last_frame["shm_open"] = opened;
                    session->last_frame["width"] = width;
                    session->last_frame["height"] = height;
                    session->last_frame["resource_width"] = FieldU32(message, "resource_width", width);
                    session->last_frame["resource_height"] = FieldU32(message, "resource_height", height);
                }
                if (opened) {
                    std::lock_guard<std::mutex> frame_lock(session->frame_mutex);
                    session->framebuffer = std::move(framebuffer);
                    session->remote_frame = RemoteVideoFrame{};
                    session->remote_frame_force_full = true;
                }
            }
        } else if (message.type == "display.cursor") {
            std::lock_guard<std::mutex> lock(session->console_mutex);
            const auto previous_pixels = session->cursor.value("pixels_b64", std::string());
            session->cursor = nlohmann::json::object();
            for (const auto& [key, value] : message.fields) session->cursor[key] = value;
            if (message.fields["image_updated"] == "1" || message.fields["image_updated"] == "true") {
                session->cursor["pixels_b64"] = Base64Encode(message.payload);
            } else if (!previous_pixels.empty()) {
                session->cursor["pixels_b64"] = previous_pixels;
            }
            cursor_event = session->cursor;
        }
        if (!cursor_event.is_null()) {
            CursorCallback callback;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                callback = cursor_callback_;
            }
            if (callback) callback(session->spec.vm_id, std::move(cursor_event));
        }
        return;
    }
    if (message.channel == ipc::Channel::kAudio) {
        RemoteAudioChunk audio_chunk;
        audio_chunk.sample_rate = FieldU32(message, "sample_rate", 48000);
        audio_chunk.channels = FieldU32(message, "channels", 2);
        if (message.payload.size() >= sizeof(int16_t)) {
            const size_t samples = message.payload.size() / sizeof(int16_t);
            audio_chunk.pcm.resize(samples);
            std::memcpy(audio_chunk.pcm.data(), message.payload.data(), samples * sizeof(int16_t));
        }
        {
            std::lock_guard<std::mutex> lock(session->console_mutex);
            session->last_audio = {
                {"type", message.type},
                {"sample_rate", audio_chunk.sample_rate},
                {"channels", audio_chunk.channels},
                {"bytes", message.payload.size()},
                {"updated_at", UnixNow()},
            };
        }
        AudioCallback callback;
        {
            std::lock_guard<std::mutex> callback_lock(callback_mutex_);
            callback = audio_callback_;
        }
        if (callback && !audio_chunk.pcm.empty()) callback(session->spec.vm_id, std::move(audio_chunk));
        return;
    }
    if (message.channel == ipc::Channel::kClipboard) {
        {
            std::lock_guard<std::mutex> lock(session->console_mutex);
            session->last_clipboard = nlohmann::json::object();
            session->last_clipboard["type"] = message.type;
            for (const auto& [key, value] : message.fields) session->last_clipboard[key] = value;
            session->last_clipboard["bytes"] = message.payload.size();
            session->last_clipboard["updated_at"] = UnixNow();
        }
        // Surface to whoever is currently bridging this VM into a remote
        // session. Subscribers run outside the session lock so they can call
        // back into RuntimeManager without re-entrancy.
        ClipboardCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            auto it = clipboard_callbacks_.find(session->spec.vm_id);
            if (it != clipboard_callbacks_.end()) callback = it->second;
        }
        if (callback) {
            ClipboardEvent event;
            event.type = message.type;
            auto field_u32 = [&](const char* key) -> uint32_t {
                auto it = message.fields.find(key);
                if (it == message.fields.end()) return 0;
                return static_cast<uint32_t>(std::strtoul(it->second.c_str(), nullptr, 10));
            };
            event.selection = field_u32("selection");
            event.data_type = field_u32("data_type");
            auto types_it = message.fields.find("types");
            if (types_it != message.fields.end()) {
                std::string buf = types_it->second;
                size_t pos = 0;
                while (pos < buf.size()) {
                    size_t comma = buf.find(',', pos);
                    if (comma == std::string::npos) comma = buf.size();
                    if (comma > pos) {
                        event.available_types.push_back(static_cast<uint32_t>(
                            std::strtoul(buf.substr(pos, comma - pos).c_str(), nullptr, 10)));
                    }
                    pos = comma + 1;
                }
            }
            event.data = message.payload;
            callback(session->spec.vm_id, event);
        }
    }
}

bool RuntimeManager::SendRuntime(std::shared_ptr<RuntimeSession> session, const ipc::Message& message) {
    if (!session || !session->runtime_conn || !session->runtime_conn->IsValid()) return false;
    std::lock_guard<std::mutex> lock(session->send_mutex);
    return session->runtime_conn->Send(ipc::Encode(message));
}

bool RuntimeManager::SendConsoleInput(const std::string& vm_id, const std::vector<uint8_t>& bytes) {
    auto session = FindSession(vm_id);
    if (!session) return false;
    ipc::Message message;
    message.channel = ipc::Channel::kConsole;
    message.kind = ipc::Kind::kRequest;
    message.type = "console.input";
    message.vm_id = vm_id;
    message.fields["data_hex"] = HexEncode(bytes);
    return SendRuntime(session, message);
}

bool RuntimeManager::SendKeyEvent(const std::string& vm_id, uint32_t key_code, bool pressed) {
    auto session = FindSession(vm_id);
    if (!session) return false;
    ipc::Message message;
    message.channel = ipc::Channel::kInput;
    message.kind = ipc::Kind::kRequest;
    message.type = "input.key_event";
    message.vm_id = vm_id;
    message.fields["key_code"] = std::to_string(key_code);
    message.fields["pressed"] = pressed ? "1" : "0";
    return SendRuntime(session, message);
}

bool RuntimeManager::SendPointerEvent(const std::string& vm_id, int x, int y, uint32_t buttons) {
    auto session = FindSession(vm_id);
    if (!session) return false;
    ipc::Message message;
    message.channel = ipc::Channel::kInput;
    message.kind = ipc::Kind::kRequest;
    message.type = "input.pointer_event";
    message.vm_id = vm_id;
    message.fields["x"] = std::to_string(x);
    message.fields["y"] = std::to_string(y);
    message.fields["buttons"] = std::to_string(buttons);
    return SendRuntime(session, message);
}

bool RuntimeManager::SendWheelEvent(const std::string& vm_id, int delta) {
    auto session = FindSession(vm_id);
    if (!session) return false;
    ipc::Message message;
    message.channel = ipc::Channel::kInput;
    message.kind = ipc::Kind::kRequest;
    message.type = "input.wheel_event";
    message.vm_id = vm_id;
    message.fields["delta"] = std::to_string(delta);
    return SendRuntime(session, message);
}

bool RuntimeManager::SetDisplaySize(const std::string& vm_id, uint32_t width, uint32_t height) {
    auto session = FindSession(vm_id);
    if (!session) return false;
    ipc::Message message;
    message.channel = ipc::Channel::kDisplay;
    message.kind = ipc::Kind::kRequest;
    message.type = "display.set_size";
    message.vm_id = vm_id;
    message.fields["width"] = std::to_string(width);
    message.fields["height"] = std::to_string(height);
    return SendRuntime(session, message);
}

bool RuntimeManager::ApplyForwards(const std::string& vm_id) {
    auto session = FindSession(vm_id);
    if (!session) return false;
    auto record = store_.Get(vm_id);
    if (!record) return false;

    // Mirror the persisted spec onto the live session so a guest-initiated
    // reboot path (which copies session->spec into the next start) does not
    // lose the change before the next StartVm refresh from store_.
    session->spec.host_forwards = record->spec.host_forwards;
    session->spec.guest_forwards = record->spec.guest_forwards;

    ipc::Message message;
    message.channel = ipc::Channel::kControl;
    message.kind = ipc::Kind::kRequest;
    message.type = "runtime.update_network";
    message.vm_id = vm_id;
    // The runtime expects link_up alongside the forward set; we never want to
    // bring the link down here, only re-apply forwards on top of it.
    message.fields["link_up"] = "true";
    // Host -> Guest (qemu hostfwd, listens on host).
    message.fields["forward_count"] = std::to_string(record->spec.host_forwards.size());
    for (size_t i = 0; i < record->spec.host_forwards.size(); ++i) {
        message.fields["forward_" + std::to_string(i)] =
            record->spec.host_forwards[i].ToHostfwd();
    }
    // Guest -> Host (qemu guestfwd, exposes a host service inside the slirp
    // subnet at guest_ip:guest_port). Same effective set as BuildRuntimeArgs
    // so a `host.llm_proxy.set` follow-up can re-publish the proxy guestfwd
    // to running VMs without a reboot.
    const auto effective_gf = EffectiveGuestForwards(record->spec);
    message.fields["guestfwd_count"] = std::to_string(effective_gf.size());
    for (size_t i = 0; i < effective_gf.size(); ++i) {
        message.fields["guestfwd_" + std::to_string(i)] = effective_gf[i].ToGuestfwd();
    }
    return SendRuntime(session, message);
}

bool RuntimeManager::ApplySharedFolders(const std::string& vm_id) {
    auto session = FindSession(vm_id);
    if (!session) return false;
    auto record = store_.Get(vm_id);
    if (!record) return false;

    session->spec.shared_folders = record->spec.shared_folders;

    ipc::Message message;
    message.channel = ipc::Channel::kControl;
    message.kind = ipc::Kind::kRequest;
    message.type = "runtime.update_shared_folders";
    message.vm_id = vm_id;
    message.fields["folder_count"] = std::to_string(record->spec.shared_folders.size());
    for (size_t i = 0; i < record->spec.shared_folders.size(); ++i) {
        const auto& folder = record->spec.shared_folders[i];
        message.fields["folder_" + std::to_string(i)] =
            folder.tag + "|" + folder.host_path + "|" + (folder.readonly ? "1" : "0");
    }
    return SendRuntime(session, message);
}

bool RuntimeManager::ApplyNetLink(const std::string& vm_id, bool up) {
    auto session = FindSession(vm_id);
    if (!session) return false;
    auto record = store_.Get(vm_id);
    if (!record) return false;

    // Mirror onto the live session so a guest-initiated reboot path picks
    // up the new link state when it copies session->spec into the next
    // start. Without this mirror, a reboot would re-enable the link from
    // the stale session spec even if the user toggled it off seconds ago.
    session->spec.nat_enabled = record->spec.nat_enabled;

    ipc::Message message;
    message.channel = ipc::Channel::kControl;
    message.kind = ipc::Kind::kRequest;
    message.type = "runtime.update_network";
    message.vm_id = vm_id;
    message.fields["link_up"] = up ? "true" : "false";
    // Send the current forward set alongside the link toggle so the runtime
    // never "forgets" forwards on a link-down/link-up cycle. Same payload
    // shape as ApplyForwards keeps the runtime handler symmetrical.
    message.fields["forward_count"] = std::to_string(record->spec.host_forwards.size());
    for (size_t i = 0; i < record->spec.host_forwards.size(); ++i) {
        message.fields["forward_" + std::to_string(i)] =
            record->spec.host_forwards[i].ToHostfwd();
    }
    const auto effective_gf = EffectiveGuestForwards(record->spec);
    message.fields["guestfwd_count"] = std::to_string(effective_gf.size());
    for (size_t i = 0; i < effective_gf.size(); ++i) {
        message.fields["guestfwd_" + std::to_string(i)] = effective_gf[i].ToGuestfwd();
    }
    return SendRuntime(session, message);
}

bool RuntimeManager::SetRemoteVideoPixelFormat(const std::string& vm_id, PixelFormat format) {
    if (format != PixelFormat::kYuv420p && format != PixelFormat::kYuv444p) return false;
    auto session = FindSession(vm_id);
    if (!session) return false;
    std::lock_guard<std::mutex> lock(session->frame_mutex);
    if (session->remote_video_format == format) return true;
    session->remote_video_format = format;
    session->remote_frame = RemoteVideoFrame{};
    session->remote_frame.format = format;
    session->remote_frame_force_full = true;
    session->remote_frame_cv.notify_all();
    return true;
}

nlohmann::json RuntimeManager::RemoteRuntimeSnapshot(const std::string& vm_id) const {
    auto session = FindSession(vm_id);
    if (!session) return {{"running", false}};
    bool framebuffer_ready = false;
    {
        std::lock_guard<std::mutex> frame_lock(session->frame_mutex);
        framebuffer_ready = session->framebuffer && session->framebuffer->IsValid();
    }
    std::lock_guard<std::mutex> lock(session->console_mutex);
    return {
        {"running", true},
        {"display", session->display_state},
        {"frame", session->last_frame},
        {"framebuffer_ready", framebuffer_ready},
        {"cursor", session->cursor},
        {"audio", session->last_audio},
        {"clipboard", session->last_clipboard},
    };
}

nlohmann::json RuntimeManager::CursorSnapshot(const std::string& vm_id) const {
    auto session = FindSession(vm_id);
    if (!session) return nlohmann::json::object();
    std::lock_guard<std::mutex> lock(session->console_mutex);
    return session->cursor;
}

void RuntimeManager::SetCursorCallback(CursorCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    cursor_callback_ = std::move(callback);
}

void RuntimeManager::SetAudioCallback(AudioCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    audio_callback_ = std::move(callback);
}

void RuntimeManager::SetStateChangedCallback(StateChangedCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    state_changed_callback_ = std::move(callback);
}

void RuntimeManager::SetLogAppendCallback(LogAppendCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    log_append_callback_ = std::move(callback);
}

void RuntimeManager::SetLlmProxyPortProvider(LlmProxyPortProvider provider) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    llm_proxy_port_provider_ = std::move(provider);
}

void RuntimeManager::SetClipboardCallback(const std::string& vm_id, ClipboardCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (callback) clipboard_callbacks_[vm_id] = std::move(callback);
    else clipboard_callbacks_.erase(vm_id);
}

void RuntimeManager::ClearClipboardCallback(const std::string& vm_id) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    clipboard_callbacks_.erase(vm_id);
}

bool RuntimeManager::SendClipboardGrabToGuest(const std::string& vm_id,
                                              uint32_t selection,
                                              const std::vector<uint32_t>& types) {
    auto session = FindSession(vm_id);
    if (!session) return false;
    ipc::Message message;
    message.channel = ipc::Channel::kClipboard;
    message.kind = ipc::Kind::kRequest;
    message.type = "clipboard.grab";
    message.vm_id = vm_id;
    message.fields["selection"] = std::to_string(selection);
    std::string types_str;
    for (size_t i = 0; i < types.size(); ++i) {
        if (i > 0) types_str += ",";
        types_str += std::to_string(types[i]);
    }
    message.fields["types"] = types_str;
    return SendRuntime(session, message);
}

bool RuntimeManager::SendClipboardDataToGuest(const std::string& vm_id,
                                              uint32_t selection,
                                              uint32_t data_type,
                                              const std::vector<uint8_t>& bytes) {
    auto session = FindSession(vm_id);
    if (!session) return false;
    ipc::Message message;
    message.channel = ipc::Channel::kClipboard;
    message.kind = ipc::Kind::kRequest;
    message.type = "clipboard.data";
    message.vm_id = vm_id;
    message.fields["selection"] = std::to_string(selection);
    message.fields["data_type"] = std::to_string(data_type);
    message.payload = bytes;
    return SendRuntime(session, message);
}

bool RuntimeManager::SendClipboardRequestToGuest(const std::string& vm_id,
                                                 uint32_t selection,
                                                 uint32_t data_type) {
    auto session = FindSession(vm_id);
    if (!session) return false;
    ipc::Message message;
    message.channel = ipc::Channel::kClipboard;
    message.kind = ipc::Kind::kRequest;
    message.type = "clipboard.request";
    message.vm_id = vm_id;
    message.fields["selection"] = std::to_string(selection);
    message.fields["data_type"] = std::to_string(data_type);
    return SendRuntime(session, message);
}

bool RuntimeManager::SendClipboardReleaseToGuest(const std::string& vm_id, uint32_t selection) {
    auto session = FindSession(vm_id);
    if (!session) return false;
    ipc::Message message;
    message.channel = ipc::Channel::kClipboard;
    message.kind = ipc::Kind::kRequest;
    message.type = "clipboard.release";
    message.vm_id = vm_id;
    message.fields["selection"] = std::to_string(selection);
    return SendRuntime(session, message);
}

void RuntimeManager::NotifyStateChanged(const std::string& vm_id, const VmRuntimeInfo& info) {
    StateChangedCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = state_changed_callback_;
    }
    if (callback) callback(vm_id, info);
}

namespace {

// Allocate a freshly-converted planar YUV slice from a sub-rectangle of a BGRA
// framebuffer. Returns an empty slice (data.empty()) on failure.
RemoteVideoSlice MakeSliceFromBgra(const uint8_t* bgra_base,
                                   int bgra_stride,
                                   uint32_t x,
                                   uint32_t y,
                                   uint32_t width,
                                   uint32_t height,
                                   PixelFormat format) {
    RemoteVideoSlice slice;
#ifdef TENBOX_ENABLE_LIBYUV
    if (!bgra_base || bgra_stride <= 0 || width == 0 || height == 0) return slice;
    if (format != PixelFormat::kYuv420p && format != PixelFormat::kYuv444p) return slice;

    slice.x = x;
    slice.y = y;
    slice.width = width;
    slice.height = height;
    slice.strides[0] = static_cast<int>(width);
    slice.strides[1] = static_cast<int>(
        format == PixelFormat::kYuv444p ? width : (width + 1) / 2);
    slice.strides[2] = slice.strides[1];

    const size_t y_size = static_cast<size_t>(slice.strides[0]) * height;
    const size_t uv_h = format == PixelFormat::kYuv444p
        ? static_cast<size_t>(height)
        : static_cast<size_t>((height + 1) / 2);
    const size_t uv_size = static_cast<size_t>(slice.strides[1]) * uv_h;
    slice.data.assign(y_size + uv_size * 2, 0);

    const uint8_t* src = bgra_base +
        static_cast<size_t>(y) * static_cast<size_t>(bgra_stride) +
        static_cast<size_t>(x) * 4;
    uint8_t* dst_y = slice.data.data();
    uint8_t* dst_u = slice.data.data() + y_size;
    uint8_t* dst_v = slice.data.data() + y_size + uv_size;
    const int rc = format == PixelFormat::kYuv444p
        ? libyuv::ARGBToI444(
            src, bgra_stride,
            dst_y, slice.strides[0],
            dst_u, slice.strides[1],
            dst_v, slice.strides[2],
            static_cast<int>(width),
            static_cast<int>(height))
        : libyuv::ARGBToI420(
            src, bgra_stride,
            dst_y, slice.strides[0],
            dst_u, slice.strides[1],
            dst_v, slice.strides[2],
            static_cast<int>(width),
            static_cast<int>(height));
    if (rc != 0) {
        return RemoteVideoSlice{};
    }
#else
    (void)bgra_base;
    (void)bgra_stride;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)format;
#endif
    return slice;
}

}  // namespace

void RuntimeManager::UpdateRemoteVideoFrameLocked(RuntimeSession& session, const ipc::Message& message) {
    if (!session.framebuffer || !session.framebuffer->IsValid()) return;

    const uint32_t frame_width = session.framebuffer->width();
    const uint32_t frame_height = session.framebuffer->height();
    if (frame_width == 0 || frame_height == 0 || session.framebuffer->size() == 0) return;

    RemoteVideoFrame& frame = session.remote_frame;
    const PixelFormat target_format = session.remote_video_format;
    const bool reinitialize =
        frame.width != frame_width ||
        frame.height != frame_height ||
        frame.format != target_format;

    if (reinitialize) {
        frame = RemoteVideoFrame{};
        frame.width = frame_width;
        frame.height = frame_height;
        frame.format = target_format;
        // The encoder must be re-seeded after a resize; force the next reader
        // drain to emit a full-frame slice.
        session.remote_frame_force_full = true;
    }

    // The producer either accumulates partial slices that update the encoder's
    // persistent input frame, OR (when explicitly requested) emits a single
    // full-frame slice that supersedes the queue. Mirrors sweet's
    // CreateEncodeSlice / DrawSlices pipeline.
    const bool emit_full = session.remote_frame_force_full || reinitialize;

    if (emit_full) {
        RemoteVideoSlice full = MakeSliceFromBgra(
            session.framebuffer->data(),
            static_cast<int>(session.framebuffer->stride()),
            0, 0, frame_width, frame_height,
            target_format);
        if (!full.data.empty()) {
            frame.slices.clear();
            frame.slices.push_back(std::move(full));
            session.remote_frame_force_full = false;
        }
    } else {
        const DirtyRect dirty = NormalizeDirtyRect(
            FieldU32(message, "dirty_x"),
            FieldU32(message, "dirty_y"),
            FieldU32(message, "dirty_width", FieldU32(message, "width")),
            FieldU32(message, "dirty_height", FieldU32(message, "height")),
            frame_width,
            frame_height,
            /*force_full_frame=*/false);

        if (dirty.width == frame_width && dirty.height == frame_height) {
            // A full-screen dirty rect supersedes any pending partials.
            frame.slices.clear();
        }

        RemoteVideoSlice slice = MakeSliceFromBgra(
            session.framebuffer->data(),
            static_cast<int>(session.framebuffer->stride()),
            dirty.x, dirty.y, dirty.width, dirty.height,
            target_format);
        if (!slice.data.empty()) {
            frame.slices.push_back(std::move(slice));
        }

        // If consumers fall too far behind, collapse the queue into a single
        // full-frame slice instead of growing memory unboundedly.
        constexpr size_t kMaxPendingSlices = 256;
        if (frame.slices.size() > kMaxPendingSlices) {
            RemoteVideoSlice full = MakeSliceFromBgra(
                session.framebuffer->data(),
                static_cast<int>(session.framebuffer->stride()),
                0, 0, frame_width, frame_height,
                target_format);
            if (!full.data.empty()) {
                frame.slices.clear();
                frame.slices.push_back(std::move(full));
            }
        }
    }

    frame.seq = FieldU64(message, "seq");

    // Wake any consumer blocked in ReadRemoteFrame. The caller already owns
    // session.console_mutex; notifying under the lock is safe.
    session.remote_frame_cv.notify_all();
}

bool RuntimeManager::ReadRemoteFrame(const std::string& vm_id,
                                     RemoteVideoFrame* frame,
                                     bool need_full_frame,
                                     std::chrono::milliseconds wait_timeout) {
    auto session = FindSession(vm_id);
    if (!session || !frame) return false;
    std::unique_lock<std::mutex> lock(session->frame_mutex);

    // Block the consumer until either the producer queues new slices or the
    // timeout expires. need_full_frame short-circuits the wait because we can
    // synthesize a full-frame slice from the shared framebuffer regardless of
    // whether new partials have arrived.
    if (wait_timeout.count() > 0 &&
        !need_full_frame &&
        session->remote_frame.slices.empty() &&
        session->running.load()) {
        session->remote_frame_cv.wait_for(lock, wait_timeout, [&] {
            return !session->remote_frame.slices.empty() ||
                   session->remote_frame_force_full ||
                   !session->running.load();
        });
    }

    if ((need_full_frame || session->remote_frame_force_full) &&
        session->framebuffer &&
        session->framebuffer->IsValid() &&
        session->framebuffer->width() > 0 &&
        session->framebuffer->height() > 0) {
        const PixelFormat target_format = session->remote_video_format;
        RemoteVideoSlice full = MakeSliceFromBgra(
            session->framebuffer->data(),
            static_cast<int>(session->framebuffer->stride()),
            0, 0,
            session->framebuffer->width(),
            session->framebuffer->height(),
            target_format);
        if (!full.data.empty()) {
            session->remote_frame.slices.clear();
            session->remote_frame.slices.push_back(std::move(full));
            session->remote_frame.width = session->framebuffer->width();
            session->remote_frame.height = session->framebuffer->height();
            session->remote_frame.format = target_format;
            session->remote_frame_force_full = false;
        }
    }

    if (session->remote_frame.slices.empty()) return false;

    frame->width = session->remote_frame.width;
    frame->height = session->remote_frame.height;
    frame->format = session->remote_frame.format;
    frame->seq = session->remote_frame.seq;
    frame->slices = std::move(session->remote_frame.slices);
    session->remote_frame.slices.clear();
    return true;
}

bool RuntimeManager::AttachConsole(const std::string& vm_id, ipc::UnixSocketConnection* client, std::string* error) {
    auto session = FindSession(vm_id);
    if (!session) {
        if (error) *error = "VM is not running";
        return false;
    }
    std::string response = nlohmann::json{{"ok", true}}.dump();
    response.push_back('\n');
    if (!client || !client->Send(response)) {
        if (error) *error = "failed to acknowledge console attach";
        return false;
    }
    auto shared_client = std::make_shared<ipc::UnixSocketConnection>(std::move(*client));
    {
        std::lock_guard<std::mutex> lock(session->console_mutex);
        session->console_clients.push_back(shared_client);
        if (!session->console_history.empty()) {
            std::vector<uint8_t> bytes(session->console_history.begin(), session->console_history.end());
            std::string replay = nlohmann::json{
                {"type", "console.data"},
                {"data_hex", HexEncode(bytes)},
            }.dump();
            replay.push_back('\n');
            (void)shared_client->Send(replay);
        }
    }
    std::thread(&RuntimeManager::ReadConsoleClient, this, session, shared_client).detach();
    return true;
}

bool RuntimeManager::AttachLogFollower(const std::string& vm_id, ipc::UnixSocketConnection* client, std::string* error) {
    auto session = FindSession(vm_id);
    if (!session) {
        if (error) *error = "VM is not running";
        return false;
    }
    std::string ack = nlohmann::json{{"ok", true}}.dump();
    ack.push_back('\n');
    if (!client || !client->Send(ack)) {
        if (error) *error = "failed to acknowledge log follow";
        return false;
    }
    auto shared_client = std::make_shared<ipc::UnixSocketConnection>(std::move(*client));
    std::lock_guard<std::mutex> lock(session->console_mutex);
    session->log_followers.push_back(shared_client);
    return true;
}

void RuntimeManager::BroadcastConsole(std::shared_ptr<RuntimeSession> session, const std::string& data) {
    nlohmann::json event;
    if (data.empty()) {
        event = {{"type", "console.closed"}};
    } else {
        std::vector<uint8_t> bytes(data.begin(), data.end());
        event = {{"type", "console.data"}, {"data_hex", HexEncode(bytes)}};
    }
    std::string line = event.dump();
    line.push_back('\n');

    std::lock_guard<std::mutex> lock(session->console_mutex);
    if (!data.empty()) {
        session->console_history.append(data);
        constexpr size_t kMaxConsoleHistory = 256 * 1024;
        if (session->console_history.size() > kMaxConsoleHistory) {
            session->console_history.erase(0, session->console_history.size() - kMaxConsoleHistory);
        }
    }
    for (auto it = session->console_clients.begin(); it != session->console_clients.end();) {
        if (!(*it)->Send(line)) {
            it = session->console_clients.erase(it);
        } else {
            ++it;
        }
    }
}

void RuntimeManager::ReadConsoleClient(
    std::shared_ptr<RuntimeSession> session,
    std::shared_ptr<ipc::UnixSocketConnection> client) {
    while (client && client->IsValid()) {
        std::string line = client->ReadLine();
        if (line.empty()) break;
        auto json = nlohmann::json::parse(line, nullptr, false);
        if (json.is_discarded()) continue;
        if (json.value("type", "") != "console.input") continue;
        const std::vector<uint8_t> bytes = HexDecode(json.value("data_hex", ""));
        if (!bytes.empty()) {
            (void)SendConsoleInput(session->spec.vm_id, bytes);
        }
    }

    std::lock_guard<std::mutex> lock(session->console_mutex);
    session->console_clients.erase(
        std::remove(session->console_clients.begin(), session->console_clients.end(), client),
        session->console_clients.end());
}

nlohmann::json RuntimeManager::Logs(const std::string& vm_id, size_t max_lines) const {
    nlohmann::json lines = nlohmann::json::array();
    if (auto session = FindSession(vm_id)) {
        std::lock_guard<std::mutex> lock(session->console_mutex);
        const size_t start = session->log_lines.size() > max_lines ? session->log_lines.size() - max_lines : 0;
        for (size_t i = start; i < session->log_lines.size(); ++i) {
            lines.push_back(session->log_lines[i]);
        }
        return {{"lines", std::move(lines)}};
    }
    // No live session: tail the on-disk log so the operator can still read
    // crash output. Walk back through current + rotated files until we have
    // enough lines (or run out of files).
    auto record = store_.Get(vm_id);
    if (!record) return {{"lines", lines}};
    const fs::path base = RuntimeLogPath(record->spec.vm_dir);
    std::vector<std::string> collected;
    auto absorb = [&](const fs::path& path) {
        if (collected.size() >= max_lines) return;
        auto chunk = TailLogFile(path, max_lines - collected.size());
        chunk.insert(chunk.end(),
                     std::make_move_iterator(collected.begin()),
                     std::make_move_iterator(collected.end()));
        collected = std::move(chunk);
    };
    absorb(base);
    for (int i = 1; i <= kLogRotateKeep && collected.size() < max_lines; ++i) {
        absorb(base.string() + "." + std::to_string(i));
    }
    for (auto& l : collected) lines.push_back(std::move(l));
    return {{"lines", std::move(lines)}};
}

ProcessResources RuntimeManager::SampleProcessResources(const std::string& vm_id) const {
    auto session = FindSession(vm_id);
    if (!session || session->process_pid <= 0) return {};
    return process_sampler_.Sample(session->process_pid);
}

std::shared_ptr<RuntimeManager::RuntimeSession> RuntimeManager::FindSession(const std::string& vm_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(vm_id);
    return it == sessions_.end() ? nullptr : it->second;
}

}  // namespace tenbox::daemon
