#include "runtime/runtime_service.h"

#include "core/vmm/types.h"
#include "core/vmm/vm.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace {

HANDLE AsHandle(void* handle) {
    return reinterpret_cast<HANDLE>(handle);
}

}  // namespace

void ManagedConsolePort::Write(const uint8_t* data, size_t size) {
    if (!data || size == 0) return;
    std::function<void()> notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool was_empty = pending_write_.empty();
        pending_write_.append(reinterpret_cast<const char*>(data), size);
        if (was_empty && data_available_callback_) {
            notify = data_available_callback_;
        }
    }
    if (notify) notify();
}

std::string ManagedConsolePort::FlushPending() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string result;
    result.swap(pending_write_);
    return result;
}

bool ManagedConsolePort::HasPending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !pending_write_.empty();
}

void ManagedConsolePort::SetDataAvailableCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_available_callback_ = std::move(callback);
}

size_t ManagedConsolePort::Read(uint8_t* out, size_t size) {
    if (!out || size == 0) return 0;
    std::unique_lock<std::mutex> lock(mutex_);
    if (queue_.empty()) {
        cv_.wait_for(lock, std::chrono::milliseconds(16));
    }
    size_t copied = 0;
    while (!queue_.empty() && copied < size) {
        out[copied++] = queue_.front();
        queue_.pop_front();
    }
    return copied;
}

void ManagedConsolePort::PushInput(const uint8_t* data, size_t size) {
    if (!data || size == 0) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < size; ++i) {
            queue_.push_back(data[i]);
        }
    }
    cv_.notify_all();
}

// ── ManagedInputPort ─────────────────────────────────────────────────

bool ManagedInputPort::PollKeyboard(KeyboardEvent* event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (key_queue_.empty()) return false;
    *event = key_queue_.front();
    key_queue_.pop_front();
    return true;
}

bool ManagedInputPort::PollPointer(PointerEvent* event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pointer_queue_.empty()) return false;
    *event = pointer_queue_.front();
    pointer_queue_.pop_front();
    return true;
}

void ManagedInputPort::PushKeyEvent(const KeyboardEvent& ev) {
    std::lock_guard<std::mutex> lock(mutex_);
    key_queue_.push_back(ev);
}

void ManagedInputPort::PushPointerEvent(const PointerEvent& ev) {
    std::lock_guard<std::mutex> lock(mutex_);
    pointer_queue_.push_back(ev);
}

// ── ManagedDisplayPort ───────────────────────────────────────────────

void ManagedDisplayPort::SubmitFrame(const DisplayFrame& frame) {
    std::function<void(const DisplayFrame&)> handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = frame_handler_;
    }
    if (handler) handler(frame);
}

void ManagedDisplayPort::SubmitCursor(const CursorInfo& cursor) {
    std::function<void(const CursorInfo&)> handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = cursor_handler_;
    }
    if (handler) handler(cursor);
}

void ManagedDisplayPort::SetFrameHandler(
    std::function<void(const DisplayFrame&)> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_handler_ = std::move(handler);
}

void ManagedDisplayPort::SetCursorHandler(
    std::function<void(const CursorInfo&)> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    cursor_handler_ = std::move(handler);
}

void ManagedDisplayPort::SubmitScanoutState(bool active, uint32_t width, uint32_t height) {
    std::function<void(bool, uint32_t, uint32_t)> handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = state_handler_;
    }
    if (handler) handler(active, width, height);
}

void ManagedDisplayPort::SetStateHandler(std::function<void(bool, uint32_t, uint32_t)> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_handler_ = std::move(handler);
}

// ── ManagedClipboardPort ──────────────────────────────────────────────

void ManagedClipboardPort::OnClipboardEvent(const ClipboardEvent& event) {
    std::function<void(const ClipboardEvent&)> handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = event_handler_;
    }
    if (handler) {
        handler(event);
    }
}

void ManagedClipboardPort::SetEventHandler(std::function<void(const ClipboardEvent&)> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    event_handler_ = std::move(handler);
}

std::string EncodeHex(const uint8_t* data, size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.resize(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out[2 * i] = kDigits[(data[i] >> 4) & 0x0F];
        out[2 * i + 1] = kDigits[data[i] & 0x0F];
    }
    return out;
}

std::vector<uint8_t> DecodeHex(const std::string& value) {
    auto hex = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
        if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
        return -1;
    };

    if ((value.size() % 2) != 0) return {};
    std::vector<uint8_t> out(value.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        int hi = hex(value[2 * i]);
        int lo = hex(value[2 * i + 1]);
        if (hi < 0 || lo < 0) return {};
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return out;
}

RuntimeControlService::RuntimeControlService(std::string vm_id, std::string pipe_name)
    : vm_id_(std::move(vm_id)), pipe_name_(std::move(pipe_name)) {
    console_port_->SetDataAvailableCallback([this]() {
        send_cv_.notify_one();
    });

    display_port_->SetFrameHandler([this](const DisplayFrame& frame) {
        ipc::Message event;
        event.kind = ipc::Kind::kEvent;
        event.channel = ipc::Channel::kDisplay;
        event.type = "display.frame";
        event.vm_id = vm_id_;
        event.request_id = next_event_id_++;
        event.fields["width"] = std::to_string(frame.width);
        event.fields["height"] = std::to_string(frame.height);
        event.fields["stride"] = std::to_string(frame.stride);
        event.fields["format"] = std::to_string(frame.format);
        event.fields["resource_width"] = std::to_string(frame.resource_width);
        event.fields["resource_height"] = std::to_string(frame.resource_height);
        event.fields["dirty_x"] = std::to_string(frame.dirty_x);
        event.fields["dirty_y"] = std::to_string(frame.dirty_y);
        event.payload = frame.pixels;

        std::string encoded = ipc::Encode(event);
        {
            std::lock_guard<std::mutex> lock(send_queue_mutex_);
            frame_queue_.push_back(std::move(encoded));
            if (frame_queue_.size() > kMaxPendingFrames) {
                frame_queue_.pop_front();
            }
        }
        send_cv_.notify_one();
    });

    display_port_->SetCursorHandler([this](const CursorInfo& cursor) {
        ipc::Message event;
        event.kind = ipc::Kind::kEvent;
        event.channel = ipc::Channel::kDisplay;
        event.type = "display.cursor";
        event.vm_id = vm_id_;
        event.request_id = next_event_id_++;
        event.fields["x"] = std::to_string(cursor.x);
        event.fields["y"] = std::to_string(cursor.y);
        event.fields["hot_x"] = std::to_string(cursor.hot_x);
        event.fields["hot_y"] = std::to_string(cursor.hot_y);
        event.fields["width"] = std::to_string(cursor.width);
        event.fields["height"] = std::to_string(cursor.height);
        event.fields["visible"] = cursor.visible ? "1" : "0";
        event.fields["image_updated"] = cursor.image_updated ? "1" : "0";
        if (cursor.image_updated && !cursor.pixels.empty()) {
            event.payload = cursor.pixels;
        }

        std::string encoded = ipc::Encode(event);
        {
            std::lock_guard<std::mutex> lock(send_queue_mutex_);
            console_queue_.push_back(std::move(encoded));
        }
        send_cv_.notify_one();
    });

    display_port_->SetStateHandler([this](bool active, uint32_t width, uint32_t height) {
        ipc::Message event;
        event.kind = ipc::Kind::kEvent;
        event.channel = ipc::Channel::kDisplay;
        event.type = "display.state";
        event.vm_id = vm_id_;
        event.request_id = next_event_id_++;
        event.fields["active"] = active ? "1" : "0";
        event.fields["width"] = std::to_string(width);
        event.fields["height"] = std::to_string(height);

        std::string encoded = ipc::Encode(event);
        {
            std::lock_guard<std::mutex> lock(send_queue_mutex_);
            console_queue_.push_back(std::move(encoded));
        }
        send_cv_.notify_one();
    });

    clipboard_port_->SetEventHandler([this](const ClipboardEvent& clip_event) {
        ipc::Message event;
        event.kind = ipc::Kind::kEvent;
        event.channel = ipc::Channel::kClipboard;
        event.vm_id = vm_id_;
        event.request_id = next_event_id_++;

        switch (clip_event.type) {
        case ClipboardEvent::Type::kGrab:
            event.type = "clipboard.grab";
            event.fields["selection"] = std::to_string(clip_event.selection);
            {
                std::string types_str;
                for (size_t i = 0; i < clip_event.available_types.size(); ++i) {
                    if (i > 0) types_str += ",";
                    types_str += std::to_string(clip_event.available_types[i]);
                }
                event.fields["types"] = types_str;
            }
            break;

        case ClipboardEvent::Type::kData:
            event.type = "clipboard.data";
            event.fields["selection"] = std::to_string(clip_event.selection);
            event.fields["data_type"] = std::to_string(clip_event.data_type);
            event.payload = clip_event.data;
            break;

        case ClipboardEvent::Type::kRequest:
            event.type = "clipboard.request";
            event.fields["selection"] = std::to_string(clip_event.selection);
            event.fields["data_type"] = std::to_string(clip_event.data_type);
            break;

        case ClipboardEvent::Type::kRelease:
            event.type = "clipboard.release";
            event.fields["selection"] = std::to_string(clip_event.selection);
            break;
        }

        std::string encoded = ipc::Encode(event);
        {
            std::lock_guard<std::mutex> lock(send_queue_mutex_);
            console_queue_.push_back(std::move(encoded));
        }
        send_cv_.notify_one();
    });
}

RuntimeControlService::~RuntimeControlService() {
    Stop();
}

bool RuntimeControlService::Start() {
    if (running_) return true;
    if (!EnsureClientConnected()) {
        return false;
    }

    running_ = true;

    send_thread_ = std::thread([this]() {
        HANDLE h = AsHandle(pipe_handle_);
        constexpr auto kFlushInterval = std::chrono::milliseconds(20);

        while (running_) {
            std::string batch;
            {
                std::unique_lock<std::mutex> lock(send_queue_mutex_);

                // Determine wait duration based on whether console has pending data.
                bool has_pending = console_port_->HasPending();
                if (has_pending) {
                    send_cv_.wait_for(lock, kFlushInterval);
                } else {
                    send_cv_.wait(lock, [this]() {
                        return !running_ || !console_queue_.empty() ||
                               !frame_queue_.empty() || console_port_->HasPending();
                    });
                }

                if (!running_) {
                    break;
                }

                // Flush any pending console output from the port.
                std::string console_data = console_port_->FlushPending();
                if (!console_data.empty()) {
                    ipc::Message event;
                    event.kind = ipc::Kind::kEvent;
                    event.channel = ipc::Channel::kConsole;
                    event.type = "console.data";
                    event.vm_id = vm_id_;
                    event.request_id = next_event_id_++;
                    event.fields["data_hex"] = EncodeHex(
                        reinterpret_cast<const uint8_t*>(console_data.data()),
                        console_data.size());
                    batch += ipc::Encode(event);
                }

                // Send all queued high-priority messages (control responses, etc.).
                while (!console_queue_.empty()) {
                    batch += std::move(console_queue_.front());
                    console_queue_.pop_front();
                }

                // Send display frames (bounded to avoid blocking too long).
                size_t frames_to_send = frame_queue_.size();
                while (frames_to_send-- > 0 && !frame_queue_.empty()) {
                    batch += std::move(frame_queue_.front());
                    frame_queue_.pop_front();
                }
            }

            if (batch.empty()) {
                continue;
            }

            std::lock_guard<std::mutex> send_lock(send_mutex_);
            h = AsHandle(pipe_handle_);
            if (!h || h == INVALID_HANDLE_VALUE) {
                break;
            }

            const char* data = batch.data();
            size_t remaining = batch.size();
            while (remaining > 0) {
                DWORD written = 0;
                if (!WriteFile(h, data, static_cast<DWORD>(remaining), &written, nullptr)) {
                    break;
                }
                if (written == 0) {
                    break;
                }
                data += written;
                remaining -= written;
            }
        }
    });

    recv_thread_ = std::thread(&RuntimeControlService::RunLoop, this);
    return true;
}

void RuntimeControlService::Stop() {
    running_ = false;
    send_cv_.notify_all();

    HANDLE h = AsHandle(pipe_handle_);
    if (h && h != INVALID_HANDLE_VALUE) {
        CancelIoEx(h, nullptr);
        DisconnectNamedPipe(h);
        CloseHandle(h);
        pipe_handle_ = nullptr;
    }
    if (send_thread_.joinable()) {
        send_thread_.join();
    }
    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }
}

void RuntimeControlService::AttachVm(Vm* vm) {
    vm_ = vm;
}

void RuntimeControlService::PublishState(const std::string& state, int exit_code) {
    ipc::Message event;
    event.kind = ipc::Kind::kEvent;
    event.channel = ipc::Channel::kControl;
    event.type = "runtime.state";
    event.vm_id = vm_id_;
    event.request_id = next_event_id_++;
    event.fields["state"] = state;
    event.fields["exit_code"] = std::to_string(exit_code);
    Send(event);
}

bool RuntimeControlService::EnsureClientConnected() {
    if (pipe_name_.empty()) return false;

    HANDLE h = AsHandle(pipe_handle_);
    if (h && h != INVALID_HANDLE_VALUE) return true;

    std::string full_name = R"(\\.\pipe\)" + pipe_name_;
    h = CreateNamedPipeA(
        full_name.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        64 * 1024,
        64 * 1024,
        0,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LOG_ERROR("CreateNamedPipe failed for %s", full_name.c_str());
        return false;
    }

    BOOL connected = ConnectNamedPipe(h, nullptr);
    if (!connected) {
        DWORD err = GetLastError();
        if (err != ERROR_PIPE_CONNECTED) {
            CloseHandle(h);
            LOG_ERROR("ConnectNamedPipe failed: %lu", err);
            return false;
        }
    }

    pipe_handle_ = h;
    return true;
}

bool RuntimeControlService::Send(const ipc::Message& message) {
    std::string encoded = ipc::Encode(message);
    {
        std::lock_guard<std::mutex> lock(send_queue_mutex_);
        console_queue_.push_back(std::move(encoded));
    }
    send_cv_.notify_one();
    return true;
}

bool RuntimeControlService::SendWithPayload(const ipc::Message& message) {
    std::string encoded = ipc::Encode(message);
    {
        std::lock_guard<std::mutex> lock(send_queue_mutex_);
        console_queue_.push_back(std::move(encoded));
    }
    send_cv_.notify_one();
    return true;
}

void RuntimeControlService::HandleMessage(const ipc::Message& message) {
    if (message.channel == ipc::Channel::kControl &&
        message.kind == ipc::Kind::kRequest &&
        message.type == "runtime.command") {
        ipc::Message resp;
        resp.kind = ipc::Kind::kResponse;
        resp.channel = ipc::Channel::kControl;
        resp.type = "runtime.command.result";
        resp.vm_id = vm_id_;
        resp.request_id = message.request_id;
        resp.fields["ok"] = "true";

        auto it = message.fields.find("command");
        if (it == message.fields.end()) {
            resp.fields["ok"] = "false";
            resp.fields["error"] = "missing command";
            Send(resp);
            return;
        }
        const std::string& cmd = it->second;
        if (cmd == "stop") {
            if (vm_) vm_->RequestStop();
        } else if (cmd == "shutdown") {
            if (vm_) {
                vm_->TriggerPowerButton();
                static const char kPoweroff[] = "\npoweroff\n";
                vm_->InjectConsoleBytes(
                    reinterpret_cast<const uint8_t*>(kPoweroff),
                    sizeof(kPoweroff) - 1);
            }
        } else if (cmd == "reboot") {
            if (vm_) vm_->RequestStop();
            resp.fields["note"] = "reboot not implemented, performed stop";
        } else if (cmd == "start") {
            resp.fields["note"] = "runtime already started by process launch";
        } else {
            resp.fields["ok"] = "false";
            resp.fields["error"] = "unknown command";
        }
        Send(resp);
        return;
    }

    if (message.channel == ipc::Channel::kControl &&
        message.kind == ipc::Kind::kRequest &&
        message.type == "runtime.update_network") {
        ipc::Message resp;
        resp.kind = ipc::Kind::kResponse;
        resp.channel = ipc::Channel::kControl;
        resp.type = "runtime.update_network.result";
        resp.vm_id = vm_id_;
        resp.request_id = message.request_id;

        if (!vm_) {
            resp.fields["ok"] = "false";
            resp.fields["error"] = "vm not attached";
            Send(resp);
            return;
        }

        auto it_link = message.fields.find("link_up");
        if (it_link != message.fields.end()) {
            vm_->SetNetLinkUp(it_link->second == "true");
        }

        auto it_count = message.fields.find("forward_count");
        if (it_count != message.fields.end()) {
            int count = 0;
            auto [p, ec] = std::from_chars(
                it_count->second.data(),
                it_count->second.data() + it_count->second.size(), count);
            if (ec == std::errc{} && count >= 0) {
                std::vector<PortForward> forwards;
                for (int i = 0; i < count; ++i) {
                    auto it_f = message.fields.find("forward_" + std::to_string(i));
                    if (it_f == message.fields.end()) continue;
                    unsigned hp = 0, gp = 0;
                    if (std::sscanf(it_f->second.c_str(), "%u:%u", &hp, &gp) == 2
                        && hp && gp) {
                        forwards.push_back({static_cast<uint16_t>(hp),
                                            static_cast<uint16_t>(gp)});
                    }
                }
                vm_->UpdatePortForwards(forwards);
            }
        }

        resp.fields["ok"] = "true";
        Send(resp);
        return;
    }

    if (message.channel == ipc::Channel::kConsole &&
        message.kind == ipc::Kind::kRequest &&
        message.type == "console.input") {
        auto it = message.fields.find("data_hex");
        if (it != message.fields.end()) {
            auto bytes = DecodeHex(it->second);
            console_port_->PushInput(bytes.data(), bytes.size());
        }
        return;
    }

    if (message.channel == ipc::Channel::kInput &&
        message.kind == ipc::Kind::kRequest &&
        message.type == "input.key_event") {
        auto it_code = message.fields.find("key_code");
        auto it_pressed = message.fields.find("pressed");
        if (it_code != message.fields.end() && it_pressed != message.fields.end()) {
            KeyboardEvent ev;
            ev.key_code = static_cast<uint32_t>(std::strtoul(it_code->second.c_str(), nullptr, 10));
            ev.pressed = (it_pressed->second == "1" || it_pressed->second == "true");
            input_port_->PushKeyEvent(ev);
        }
        return;
    }

    if (message.channel == ipc::Channel::kInput &&
        message.kind == ipc::Kind::kRequest &&
        message.type == "input.pointer_event") {
        PointerEvent ev;
        auto it_x = message.fields.find("x");
        auto it_y = message.fields.find("y");
        auto it_btn = message.fields.find("buttons");
        if (it_x != message.fields.end()) ev.x = std::atoi(it_x->second.c_str());
        if (it_y != message.fields.end()) ev.y = std::atoi(it_y->second.c_str());
        if (it_btn != message.fields.end()) ev.buttons = static_cast<uint32_t>(std::strtoul(it_btn->second.c_str(), nullptr, 10));
        input_port_->PushPointerEvent(ev);
        return;
    }

    if (message.channel == ipc::Channel::kInput &&
        message.kind == ipc::Kind::kRequest &&
        message.type == "input.wheel_event") {
        auto it_delta = message.fields.find("delta");
        if (it_delta != message.fields.end() && vm_) {
            int32_t delta = std::atoi(it_delta->second.c_str());
            vm_->InjectWheelEvent(delta);
        }
        return;
    }

    if (message.channel == ipc::Channel::kDisplay &&
        message.kind == ipc::Kind::kRequest &&
        message.type == "display.set_size") {
        auto it_w = message.fields.find("width");
        auto it_h = message.fields.find("height");
        if (it_w != message.fields.end() && it_h != message.fields.end() && vm_) {
            uint32_t w = static_cast<uint32_t>(std::strtoul(it_w->second.c_str(), nullptr, 10));
            uint32_t h = static_cast<uint32_t>(std::strtoul(it_h->second.c_str(), nullptr, 10));
            vm_->SetDisplaySize(w, h);
        }
        return;
    }

    if (message.channel == ipc::Channel::kControl &&
        message.kind == ipc::Kind::kRequest &&
        message.type == "runtime.ping") {
        ipc::Message resp;
        resp.kind = ipc::Kind::kResponse;
        resp.channel = ipc::Channel::kControl;
        resp.type = "runtime.pong";
        resp.vm_id = vm_id_;
        resp.request_id = message.request_id;
        Send(resp);
        return;
    }

    // Clipboard messages from manager to VM
    if (message.channel == ipc::Channel::kClipboard &&
        message.kind == ipc::Kind::kRequest) {
        if (!vm_) return;

        if (message.type == "clipboard.grab") {
            auto it_types = message.fields.find("types");
            if (it_types != message.fields.end()) {
                std::vector<uint32_t> types;
                std::string types_str = it_types->second;
                size_t pos = 0;
                while (pos < types_str.size()) {
                    size_t comma = types_str.find(',', pos);
                    if (comma == std::string::npos) comma = types_str.size();
                    std::string num_str = types_str.substr(pos, comma - pos);
                    if (!num_str.empty()) {
                        types.push_back(static_cast<uint32_t>(std::strtoul(num_str.c_str(), nullptr, 10)));
                    }
                    pos = comma + 1;
                }
                vm_->SendClipboardGrab(types);
            }
            return;
        }

        if (message.type == "clipboard.data") {
            auto it_type = message.fields.find("data_type");
            if (it_type != message.fields.end()) {
                uint32_t data_type = static_cast<uint32_t>(std::strtoul(it_type->second.c_str(), nullptr, 10));
                vm_->SendClipboardData(data_type, message.payload.data(), message.payload.size());
            }
            return;
        }

        if (message.type == "clipboard.request") {
            auto it_type = message.fields.find("data_type");
            if (it_type != message.fields.end()) {
                uint32_t data_type = static_cast<uint32_t>(std::strtoul(it_type->second.c_str(), nullptr, 10));
                vm_->SendClipboardRequest(data_type);
            }
            return;
        }

        if (message.type == "clipboard.release") {
            vm_->SendClipboardRelease();
            return;
        }
    }
}

void RuntimeControlService::RunLoop() {
    if (!EnsureClientConnected()) {
        return;
    }

    HANDLE h = AsHandle(pipe_handle_);
    std::array<char, 65536> buf{};
    std::string pending;
    size_t payload_needed = 0;
    ipc::Message pending_msg;

    while (running_) {
        DWORD available = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE) {
                break;
            }
            Sleep(1);
            continue;
        }
        if (available == 0) {
            Sleep(1);
            continue;
        }

        DWORD to_read = (std::min)(available, static_cast<DWORD>(buf.size()));
        DWORD read = 0;
        if (!ReadFile(h, buf.data(), to_read, &read, nullptr)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_OPERATION_ABORTED) {
                break;
            }
            continue;
        }
        if (read == 0) {
            continue;
        }

        pending.append(buf.data(), read);

        while (!pending.empty()) {
            if (payload_needed > 0) {
                if (pending.size() < payload_needed) break;
                pending_msg.payload.assign(
                    reinterpret_cast<const uint8_t*>(pending.data()),
                    reinterpret_cast<const uint8_t*>(pending.data()) + payload_needed);
                pending.erase(0, payload_needed);
                payload_needed = 0;
                HandleMessage(pending_msg);
                continue;
            }

            size_t nl = pending.find('\n');
            if (nl == std::string::npos) break;
            std::string line = pending.substr(0, nl + 1);
            pending.erase(0, nl + 1);
            auto decoded = ipc::Decode(line);
            if (!decoded) continue;

            auto ps_it = decoded->fields.find("payload_size");
            if (ps_it != decoded->fields.end()) {
                payload_needed = std::strtoull(ps_it->second.c_str(), nullptr, 10);
                decoded->fields.erase(ps_it);
                if (payload_needed > 0) {
                    pending_msg = std::move(*decoded);
                    continue;
                }
            }
            HandleMessage(*decoded);
        }
    }
}
