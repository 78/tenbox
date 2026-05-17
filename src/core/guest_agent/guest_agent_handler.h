#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class VirtioSerialDevice;

class GuestAgentHandler {
public:
    using ConnectedCallback = std::function<void(bool connected)>;
    struct ExecResult {
        bool ok = false;
        bool exited = false;
        int exit_code = -1;
        std::string out_data;
        std::string err_data;
        std::string error;
    };
    using ExecCallback = std::function<void(ExecResult result)>;

    GuestAgentHandler();
    ~GuestAgentHandler();

    void SetSerialDevice(VirtioSerialDevice* device, uint32_t port_id);
    void SetConnectedCallback(ConnectedCallback cb);

    // Called when the guest opens/closes the port
    void OnPortOpen(bool opened);

    // Called when raw data arrives from the guest agent
    void OnDataReceived(const uint8_t* data, size_t len);

    bool IsConnected() const { return connected_.load(); }

    // Send a guest-shutdown command (mode: "halt", "powerdown", or "reboot")
    void Shutdown(const std::string& mode = "powerdown");

    // Send a guest-ping command; returns true if the request was sent
    void Ping();

    // Sync guest wall clock to host (QGA guest-set-time, nanoseconds since epoch)
    void SyncTime();

    // Execute a shell command through qemu-guest-agent guest-exec.
    bool RunShellCommand(const std::string& command,
                         const std::string& user,
                         std::chrono::milliseconds timeout,
                         ExecCallback callback);

private:
    using ResponseCallback = std::function<void(const std::string& response)>;

    void SendCommand(const std::string& command);
    void SendCommand(const std::string& command,
                     const std::string& arguments_json);
    uint64_t SendCommandRequest(const std::string& command,
                                const std::string& arguments_json,
                                ResponseCallback callback);
    bool SendCommandSync(const std::string& command,
                         const std::string& arguments_json,
                         std::chrono::milliseconds timeout,
                         std::string* response,
                         std::string* error);
    void RunShellCommandWorker(const std::string& command,
                               const std::string& user,
                               std::chrono::milliseconds timeout,
                               ExecCallback callback);
    void SendRaw(const std::string& json_line);
    void ProcessLine(const std::string& line);
    void StartSyncHandshake();

    VirtioSerialDevice* serial_device_ = nullptr;
    uint32_t port_id_ = 0;

    std::mutex mutex_;
    std::string recv_buffer_;
    std::atomic<bool> connected_{false};
    bool sync_pending_ = false;
    int64_t sync_id_ = 0;
    uint64_t next_id_ = 1;
    ConnectedCallback connected_callback_;
    std::unordered_map<uint64_t, ResponseCallback> pending_responses_;

    std::atomic<bool> stopping_{false};
    std::mutex exec_threads_mutex_;
    std::vector<std::thread> exec_threads_;
};
