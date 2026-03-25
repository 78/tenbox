#include "core/guest_agent/guest_agent_handler.h"
#include "core/device/virtio/virtio_serial.h"
#include "core/vmm/types.h"
#include <chrono>
#include <cstring>
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

GuestAgentHandler::GuestAgentHandler() = default;
GuestAgentHandler::~GuestAgentHandler() = default;

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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cb = connected_callback_;
            recv_buffer_.clear();
            sync_pending_ = false;
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
    LOG_INFO("GuestAgent: sent guest-sync-delimited id=%lld", (long long)id);
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
}

void GuestAgentHandler::SendRaw(const std::string& json_line) {
    if (!serial_device_) return;
    serial_device_->SendData(port_id_,
        reinterpret_cast<const uint8_t*>(json_line.data()),
        json_line.size());
}

void GuestAgentHandler::SendCommand(const std::string& command) {
    if (!connected_.load()) {
        LOG_WARN("GuestAgent: not connected, cannot send %s", command.c_str());
        return;
    }

    uint64_t id = next_id_++;
    std::ostringstream oss;
    oss << R"({"execute":")" << JsonEscape(command)
        << R"(","id":)" << id << "}\n";

    LOG_INFO("GuestAgent: sending %s (id=%llu)", command.c_str(), (unsigned long long)id);
    SendRaw(oss.str());
}

void GuestAgentHandler::SendCommand(const std::string& command,
                                     const std::string& arguments_json) {
    if (!connected_.load()) {
        LOG_WARN("GuestAgent: not connected, cannot send %s", command.c_str());
        return;
    }

    uint64_t id = next_id_++;
    std::ostringstream oss;
    oss << R"({"execute":")" << JsonEscape(command)
        << R"(","arguments":)" << arguments_json
        << R"(,"id":)" << id << "}\n";

    LOG_INFO("GuestAgent: sending %s (id=%llu)", command.c_str(), (unsigned long long)id);
    SendRaw(oss.str());
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
