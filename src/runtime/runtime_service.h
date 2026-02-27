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

    // Returns and clears any pending output data.
    std::string FlushPending();
    bool HasPending() const;

    // Called when new output data is available.
    void SetDataAvailableCallback(std::function<void()> callback);

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<uint8_t> queue_;
    std::string pending_write_;
    std::function<void()> data_available_callback_;
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
    void SubmitCursor(const CursorInfo& cursor) override;
    void SubmitScanoutState(bool active, uint32_t width, uint32_t height) override;
    void SetFrameHandler(std::function<void(const DisplayFrame&)> handler);
    void SetCursorHandler(std::function<void(const CursorInfo&)> handler);
    void SetStateHandler(std::function<void(bool, uint32_t, uint32_t)> handler);

private:
    std::mutex mutex_;
    std::function<void(const DisplayFrame&)> frame_handler_;
    std::function<void(const CursorInfo&)> cursor_handler_;
    std::function<void(bool, uint32_t, uint32_t)> state_handler_;
};

class ManagedClipboardPort final : public ClipboardPort {
public:
    void OnClipboardEvent(const ClipboardEvent& event) override;
    void SetEventHandler(std::function<void(const ClipboardEvent&)> handler);

private:
    std::mutex mutex_;
    std::function<void(const ClipboardEvent&)> event_handler_;
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
    std::shared_ptr<ManagedClipboardPort> GetClipboardPort() const { return clipboard_port_; }
    void PublishState(const std::string& state, int exit_code = 0);

private:
    bool Send(const ipc::Message& message);
    bool SendWithPayload(const ipc::Message& message);
    void RunLoop();
    void HandleMessage(const ipc::Message& message);
    bool EnsureClientConnected();

    std::string vm_id_;
    std::string pipe_name_;
    std::shared_ptr<ManagedConsolePort> console_port_ = std::make_shared<ManagedConsolePort>();
    std::shared_ptr<ManagedInputPort> input_port_ = std::make_shared<ManagedInputPort>();
    std::shared_ptr<ManagedDisplayPort> display_port_ = std::make_shared<ManagedDisplayPort>();
    std::shared_ptr<ManagedClipboardPort> clipboard_port_ = std::make_shared<ManagedClipboardPort>();

    std::atomic<bool> running_{false};

    // Dedicated threads for sending and receiving IPC over the named pipe.
    std::thread send_thread_;
    std::thread recv_thread_;

    // Protects pipe_handle_ and low-level WriteFile operations.
    std::mutex send_mutex_;
    void* pipe_handle_ = nullptr;
    Vm* vm_ = nullptr;
    std::atomic<uint64_t> next_event_id_{1};

    // High-priority queue for small, latency-sensitive messages
    // (console output, control responses, etc.).
    std::mutex send_queue_mutex_;
    std::condition_variable send_cv_;
    std::deque<std::string> console_queue_;

    // Bounded queue for display frames. Preserves dirty-rect ordering
    // but caps memory usage.
    static constexpr size_t kMaxPendingFrames = 8;
    std::deque<std::string> frame_queue_;
};

std::string EncodeHex(const uint8_t* data, size_t size);
std::vector<uint8_t> DecodeHex(const std::string& value);
