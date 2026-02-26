#pragma once

#include "common/ports.h"
#include "ipc/protocol_v1.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class Vm;

class ManagedConsolePort final : public ConsolePort {
public:
    void Write(const uint8_t* data, size_t size) override;
    size_t Read(uint8_t* out, size_t size) override;

    void PushInput(const uint8_t* data, size_t size);
    void SetWriteHandler(std::function<void(const uint8_t*, size_t)> handler);

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<uint8_t> queue_;
    std::function<void(const uint8_t*, size_t)> write_handler_;
};

class ManagedInputPort final : public InputPort {
public:
    bool PollKeyboard(KeyboardEvent* event) override;
    bool PollPointer(PointerEvent* event) override;

    void PushKeyEvent(const KeyboardEvent& ev);
    void PushPointerEvent(const PointerEvent& ev);

private:
    std::mutex mutex_;
    std::deque<KeyboardEvent> key_queue_;
    std::deque<PointerEvent> pointer_queue_;
};

class ManagedDisplayPort final : public DisplayPort {
public:
    void SubmitFrame(const DisplayFrame& frame) override;
    void SetFrameHandler(std::function<void(const DisplayFrame&)> handler);

private:
    std::mutex mutex_;
    std::function<void(const DisplayFrame&)> handler_;
};

class RuntimeControlService {
public:
    RuntimeControlService(std::string vm_id, std::string pipe_name);
    ~RuntimeControlService();

    bool Start();
    void Stop();

    void AttachVm(Vm* vm);
    std::shared_ptr<ManagedConsolePort> ConsolePort() const { return console_port_; }
    std::shared_ptr<ManagedInputPort> GetInputPort() const { return input_port_; }
    std::shared_ptr<ManagedDisplayPort> GetDisplayPort() const { return display_port_; }
    void PublishState(const std::string& state, int exit_code = 0);

private:
    bool Send(const ipc::Message& message);
    bool SendWithPayload(const ipc::Message& message);
    void RunLoop();
    void HandleMessage(const ipc::Message& message);
    bool EnsureClientConnected();
    void FlushConsoleBuf();

    std::string vm_id_;
    std::string pipe_name_;
    std::shared_ptr<ManagedConsolePort> console_port_ = std::make_shared<ManagedConsolePort>();
    std::shared_ptr<ManagedInputPort> input_port_ = std::make_shared<ManagedInputPort>();
    std::shared_ptr<ManagedDisplayPort> display_port_ = std::make_shared<ManagedDisplayPort>();

    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex send_mutex_;
    void* pipe_handle_ = nullptr;
    Vm* vm_ = nullptr;
    uint64_t next_event_id_ = 1;

    std::mutex console_buf_mutex_;
    std::string console_buf_;
    std::chrono::steady_clock::time_point last_console_flush_{};
};

std::string EncodeHex(const uint8_t* data, size_t size);
std::vector<uint8_t> DecodeHex(const std::string& value);
