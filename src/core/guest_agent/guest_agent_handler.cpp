#include "core/guest_agent/guest_agent_handler.h"
#include "core/device/virtio/virtio_serial.h"
#include "core/vmm/types.h"
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <optional>
#include <random>
#include <sstream>

// Minimal JSON helpers to avoid pulling nlohmann/json into the core library.
// The QGA protocol uses simple one-line JSON objects terminated by \n.

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out += c; break;
        }
    }
    return out;
}

static std::string ShellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

static int64_t GenerateSyncId() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<int64_t> dist(1, INT64_MAX);
    return dist(gen);
}

// Very simple JSON field extractor for flat objects from qemu-ga responses.
// Looks for "key":value where value is a number, string, bool, or {}.
static bool JsonHasKey(const std::string& json, const std::string& key) {
    return json.find("\"" + key + "\"") != std::string::npos;
}

static int64_t JsonGetInt(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    pos += needle.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    return std::strtoll(json.c_str() + pos, nullptr, 10);
}

static std::optional<int64_t> JsonTryGetInt(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return std::nullopt;
    pos += needle.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    char* end = nullptr;
    int64_t value = std::strtoll(json.c_str() + pos, &end, 10);
    if (end == json.c_str() + pos) return std::nullopt;
    return value;
}

static std::optional<bool> JsonTryGetBool(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return std::nullopt;
    pos += needle.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    if (json.compare(pos, 4, "true") == 0) return true;
    if (json.compare(pos, 5, "false") == 0) return false;
    return std::nullopt;
}

static std::string JsonUnescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool escaped = false;
    for (char c : s) {
        if (!escaped) {
            if (c == '\\') {
                escaped = true;
            } else {
                out.push_back(c);
            }
            continue;
        }

        switch (c) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        default: out.push_back(c); break;
        }
        escaped = false;
    }
    return out;
}

static std::optional<std::string> JsonTryGetString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return std::nullopt;
    pos += needle.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    if (pos >= json.size() || json[pos] != '"') return std::nullopt;
    ++pos;

    std::string raw;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        char c = json[pos];
        if (!escaped && c == '"') {
            return JsonUnescape(raw);
        }
        if (!escaped && c == '\\') {
            escaped = true;
            raw.push_back(c);
            continue;
        }
        escaped = false;
        raw.push_back(c);
    }
    return std::nullopt;
}

GuestAgentHandler::GuestAgentHandler() = default;
GuestAgentHandler::~GuestAgentHandler() {
    stopping_ = true;

    std::vector<ResponseCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks.reserve(pending_responses_.size());
        for (auto& [_, cb] : pending_responses_) {
            callbacks.push_back(std::move(cb));
        }
        pending_responses_.clear();
    }
    for (auto& cb : callbacks) {
        if (cb) cb(R"({"error":{"desc":"guest agent stopped"}})");
    }

    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(exec_threads_mutex_);
        threads.swap(exec_threads_);
    }
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void GuestAgentHandler::SetSerialDevice(VirtioSerialDevice* device, uint32_t port_id) {
    serial_device_ = device;
    port_id_ = port_id;
}

void GuestAgentHandler::SetConnectedCallback(ConnectedCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_callback_ = std::move(cb);
}

void GuestAgentHandler::OnPortOpen(bool opened) {
    LOG_INFO("GuestAgent: port %s", opened ? "opened" : "closed");

    if (opened) {
        StartSyncHandshake();
    } else {
        bool was_connected = connected_.exchange(false);
        ConnectedCallback cb;
        std::vector<ResponseCallback> response_callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cb = connected_callback_;
            recv_buffer_.clear();
            sync_pending_ = false;
            response_callbacks.reserve(pending_responses_.size());
            for (auto& [_, response_cb] : pending_responses_) {
                response_callbacks.push_back(std::move(response_cb));
            }
            pending_responses_.clear();
        }
        for (auto& response_cb : response_callbacks) {
            if (response_cb) response_cb(R"({"error":{"desc":"guest agent disconnected"}})");
        }
        if (was_connected && cb) {
            cb(false);
        }
    }
}

void GuestAgentHandler::StartSyncHandshake() {
    int64_t id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        recv_buffer_.clear();
        sync_id_ = GenerateSyncId();
        sync_pending_ = true;
        id = sync_id_;
    }

    // Per QGA spec: send 0xFF sentinel to flush parser, then guest-sync-delimited.
    // Send outside the lock to avoid lock-ordering issues with VirtioSerialDevice.
    uint8_t sentinel = 0xFF;
    if (serial_device_) {
        serial_device_->SendData(port_id_, &sentinel, 1);
    }

    std::ostringstream oss;
    oss << R"({"execute":"guest-sync-delimited","arguments":{"id":)"
        << id << R"(}})";
    oss << '\n';

    SendRaw(oss.str());
    LOG_INFO("GuestAgent: sent guest-sync-delimited id=%" PRId64, id);
}

void GuestAgentHandler::OnDataReceived(const uint8_t* data, size_t len) {
    // Collect complete lines under lock, then process callbacks outside the lock
    // to avoid deadlock with VirtioSerialDevice's recursive_mutex.
    std::vector<std::string> complete_lines;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        for (size_t i = 0; i < len; ++i) {
            uint8_t ch = data[i];

            if (ch == 0xFF) {
                recv_buffer_.clear();
                continue;
            }

            if (ch == '\n' || ch == '\r') {
                if (!recv_buffer_.empty()) {
                    complete_lines.push_back(std::move(recv_buffer_));
                    recv_buffer_.clear();
                }
                continue;
            }

            recv_buffer_ += static_cast<char>(ch);
        }
    }

    for (const auto& line : complete_lines) {
        ProcessLine(line);
    }
}

void GuestAgentHandler::ProcessLine(const std::string& line) {
    LOG_DEBUG("GuestAgent: recv: %s", line.c_str());

    ConnectedCallback cb_to_fire;
    ResponseCallback response_cb;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (sync_pending_ && JsonHasKey(line, "return")) {
            int64_t returned = JsonGetInt(line, "return");
            if (returned == sync_id_) {
                sync_pending_ = false;
                bool was_connected = connected_.exchange(true);
                LOG_INFO("GuestAgent: sync complete, agent is ready");

                if (!was_connected && connected_callback_) {
                    cb_to_fire = connected_callback_;
                }
            }
        }

        auto id = JsonTryGetInt(line, "id");
        if (id && *id > 0) {
            auto it = pending_responses_.find(static_cast<uint64_t>(*id));
            if (it != pending_responses_.end()) {
                response_cb = std::move(it->second);
                pending_responses_.erase(it);
            }
        }

        if (JsonHasKey(line, "error")) {
            if (sync_pending_) {
                LOG_DEBUG("GuestAgent: error during sync (expected): %s", line.c_str());
            } else {
                LOG_WARN("GuestAgent: error response: %s", line.c_str());
            }
        }
    }

    if (cb_to_fire) {
        cb_to_fire(true);
    }
    if (response_cb) {
        response_cb(line);
    }
}

void GuestAgentHandler::SendRaw(const std::string& json_line) {
    if (!serial_device_) return;
    serial_device_->SendData(port_id_,
        reinterpret_cast<const uint8_t*>(json_line.data()),
        json_line.size());
}

void GuestAgentHandler::SendCommand(const std::string& command) {
    SendCommandRequest(command, "", nullptr);
}

void GuestAgentHandler::SendCommand(const std::string& command,
                                     const std::string& arguments_json) {
    SendCommandRequest(command, arguments_json, nullptr);
}

uint64_t GuestAgentHandler::SendCommandRequest(const std::string& command,
                                               const std::string& arguments_json,
                                               ResponseCallback callback) {
    if (!connected_.load()) {
        LOG_WARN("GuestAgent: not connected, cannot send %s", command.c_str());
        return 0;
    }

    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = next_id_++;
        if (callback) {
            pending_responses_[id] = std::move(callback);
        }
    }

    std::ostringstream oss;
    oss << R"({"execute":")" << JsonEscape(command)
        << R"(")";
    if (!arguments_json.empty()) {
        oss << R"(,"arguments":)" << arguments_json;
    }
    oss << R"(,"id":)" << id << "}\n";

    LOG_INFO("GuestAgent: sending %s (id=%" PRIu64 ")", command.c_str(), id);
    SendRaw(oss.str());
    return id;
}

bool GuestAgentHandler::SendCommandSync(const std::string& command,
                                        const std::string& arguments_json,
                                        std::chrono::milliseconds timeout,
                                        std::string* response,
                                        std::string* error) {
    struct SyncState {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        std::string response;
    };

    auto state = std::make_shared<SyncState>();
    uint64_t request_id = SendCommandRequest(command, arguments_json, [state](const std::string& line) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->response = line;
            state->done = true;
        }
        state->cv.notify_all();
    });
    if (request_id == 0) {
        if (error) *error = "guest agent not connected";
        return false;
    }

    auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(state->mutex);
    while (!state->done && !stopping_.load()) {
        if (state->cv.wait_until(lock, deadline) == std::cv_status::timeout) {
            break;
        }
    }

    if (!state->done) {
        std::lock_guard<std::mutex> pending_lock(mutex_);
        pending_responses_.erase(request_id);
        if (error) *error = stopping_.load() ? "guest agent stopped" : "guest agent command timed out";
        return false;
    }

    if (response) *response = state->response;
    return true;
}

bool GuestAgentHandler::RunShellCommand(const std::string& command,
                                        const std::string& user,
                                        std::chrono::milliseconds timeout,
                                        ExecCallback callback) {
    if (!connected_.load()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(exec_threads_mutex_);
    exec_threads_.emplace_back(
        [this, command, user, timeout, callback = std::move(callback)]() mutable {
            RunShellCommandWorker(command, user, timeout, std::move(callback));
        });
    return true;
}

void GuestAgentHandler::RunShellCommandWorker(const std::string& command,
                                              const std::string& user,
                                              std::chrono::milliseconds timeout,
                                              ExecCallback callback) {
    ExecResult result;
    auto finish = [&](ExecResult r) {
        if (callback) callback(std::move(r));
    };

    std::string exec_command = command;
    if (!user.empty()) {
        const std::string quoted_user = ShellQuote(user);
        const std::string quoted_command = ShellQuote(command);
        exec_command =
            "if command -v runuser >/dev/null 2>&1 && id " + quoted_user + " >/dev/null 2>&1; then "
            "exec runuser -l " + quoted_user + " -c " + quoted_command + "; "
            "else exec /bin/sh -lc " + quoted_command + "; fi";
    }

    const std::string args =
        R"({"path":"/bin/sh","arg":["-lc",")" + JsonEscape(exec_command) +
        R"("],"capture-output":true})";

    std::string response;
    std::string error;
    if (!SendCommandSync("guest-exec", args, std::chrono::seconds(10), &response, &error)) {
        result.error = error.empty() ? "failed to start guest command" : error;
        finish(std::move(result));
        return;
    }
    if (auto desc = JsonTryGetString(response, "desc")) {
        result.error = *desc;
        finish(std::move(result));
        return;
    }
    auto pid = JsonTryGetInt(response, "pid");
    if (!pid || *pid <= 0) {
        result.error = "guest agent did not return a command pid";
        finish(std::move(result));
        return;
    }

    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!stopping_.load() && std::chrono::steady_clock::now() < deadline) {
        std::string status_response;
        std::string status_error;
        std::string status_args = R"({"pid":)" + std::to_string(*pid) + "}";
        if (!SendCommandSync("guest-exec-status", status_args,
                             std::chrono::seconds(10), &status_response, &status_error)) {
            result.error = status_error.empty() ? "failed to read guest command status" : status_error;
            finish(std::move(result));
            return;
        }
        if (auto desc = JsonTryGetString(status_response, "desc")) {
            result.error = *desc;
            finish(std::move(result));
            return;
        }

        auto exited = JsonTryGetBool(status_response, "exited");
        if (exited && *exited) {
            result.ok = true;
            result.exited = true;
            result.exit_code = static_cast<int>(JsonTryGetInt(status_response, "exitcode").value_or(0));
            result.out_data = JsonTryGetString(status_response, "out-data").value_or("");
            result.err_data = JsonTryGetString(status_response, "err-data").value_or("");
            finish(std::move(result));
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    result.error = stopping_.load() ? "guest agent stopped" : "guest command timed out";
    finish(std::move(result));
}

void GuestAgentHandler::Shutdown(const std::string& mode) {
    std::string args = R"({"mode":")" + JsonEscape(mode) + R"("})";
    SendCommand("guest-shutdown", args);
}

void GuestAgentHandler::Ping() {
    SendCommand("guest-ping");
}

void GuestAgentHandler::SyncTime() {
    auto now = std::chrono::system_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    std::string args = "{\"time\":" + std::to_string(ns) + "}";
    SendCommand("guest-set-time", args);
}
