#include "runtime/runtime_service.h"

#include "core/vmm/types.h"
#include "core/vmm/vm.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unordered_set>

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
    InputCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = input_callback_;
    }
    if (cb) {
        cb(data, size);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < size; ++i) {
            queue_.push_back(data[i]);
        }
    }
    cv_.notify_all();
}

void ManagedConsolePort::SetInputCallback(InputCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    input_callback_ = std::move(cb);
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
    KeyCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = key_callback_;
    }
    if (cb) {
        cb(ev);
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    key_queue_.push_back(ev);
}

void ManagedInputPort::PushPointerEvent(const PointerEvent& ev) {
    PointerCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = pointer_callback_;
    }
    if (cb) {
        cb(ev);
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    pointer_queue_.push_back(ev);
}

void ManagedInputPort::SetKeyEventCallback(KeyCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    key_callback_ = std::move(cb);
}

void ManagedInputPort::SetPointerEventCallback(PointerCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    pointer_callback_ = std::move(cb);
}

// ── ManagedDisplayPort ───────────────────────────────────────────────

void ManagedDisplayPort::SubmitFrame(DisplayFrame frame) {
    std::function<void(DisplayFrame)> handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = frame_handler_;
    }
    if (handler) handler(std::move(frame));
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
    std::function<void(DisplayFrame)> handler) {
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

// ── ManagedAudioPort ──────────────────────────────────────────────────

void ManagedAudioPort::SubmitPcm(AudioChunk chunk) {
    std::function<void(AudioChunk)> handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = pcm_handler_;
    }
    if (handler) handler(std::move(chunk));
}

void ManagedAudioPort::SetPcmHandler(std::function<void(AudioChunk)> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    pcm_handler_ = std::move(handler);
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
        if (running_) uv_async_send(&send_wakeup_);
    });

    display_port_->SetFrameHandler([this](DisplayFrame frame) {
        uint32_t resW = frame.resource_width ? frame.resource_width : frame.width;
        uint32_t resH = frame.resource_height ? frame.resource_height : frame.height;

        // Create or resize shared framebuffer when resource dimensions change.
        // Each resize uses a unique name (generation suffix) so the Manager
        // can open the new mapping without conflicting with the old one that
        // may still be mapped on its side.
        if (!shm_fb_.IsValid() || shm_fb_.width() != resW || shm_fb_.height() != resH) {
            if (shm_fb_.IsValid()) {
                shm_fb_.Close();
            }
            ++shm_generation_;
            std::string shm_name = ipc::GetSharedFramebufferName(vm_id_)
                                   + "_" + std::to_string(shm_generation_);
            if (!shm_fb_.Create(shm_name, resW, resH)) {
                LOG_ERROR("RuntimeService: failed to create shared framebuffer %ux%u", resW, resH);
                return;
            }
            shm_init_sent_ = false;
            shm_frame_seq_ = 0;
        }

        // Send shm_init notification so the manager can open the mapping.
        if (!shm_init_sent_) {
            ipc::Message init;
            init.kind = ipc::Kind::kEvent;
            init.channel = ipc::Channel::kDisplay;
            init.type = "display.shm_init";
            init.vm_id = vm_id_;
            init.request_id = next_event_id_++;
            init.fields["shm_name"] = shm_fb_.name();
            init.fields["width"] = std::to_string(resW);
            init.fields["height"] = std::to_string(resH);
            Send(init);
            shm_init_sent_ = true;
        }

        // Blit dirty rect into shared memory.
        uint32_t dx = frame.dirty_x;
        uint32_t dy = frame.dirty_y;
        uint32_t dw = frame.width;
        uint32_t dh = frame.height;
        uint32_t src_stride = frame.stride;
        uint32_t dst_stride = shm_fb_.stride();
        const uint8_t* src = frame.pixels.data();
        uint8_t* dst = shm_fb_.data();

        for (uint32_t row = 0; row < dh; ++row) {
            size_t src_off = static_cast<size_t>(row) * src_stride;
            size_t dst_off = static_cast<size_t>(dy + row) * dst_stride +
                             static_cast<size_t>(dx) * 4;
            if (src_off + dw * 4 > frame.pixels.size()) break;
            if (dst_off + dw * 4 > shm_fb_.size()) break;
            std::memcpy(dst + dst_off, src + src_off, dw * 4);
        }

        // Send lightweight metadata-only notification.
        uint64_t seq = ++shm_frame_seq_;
        ipc::Message notify;
        notify.kind = ipc::Kind::kEvent;
        notify.channel = ipc::Channel::kDisplay;
        notify.type = "display.frame_ready";
        notify.vm_id = vm_id_;
        notify.request_id = next_event_id_++;
        notify.fields["width"] = std::to_string(dw);
        notify.fields["height"] = std::to_string(dh);
        notify.fields["stride"] = std::to_string(dst_stride);
        notify.fields["format"] = std::to_string(frame.format);
        notify.fields["resource_width"] = std::to_string(resW);
        notify.fields["resource_height"] = std::to_string(resH);
        notify.fields["dirty_x"] = std::to_string(dx);
        notify.fields["dirty_y"] = std::to_string(dy);
        notify.fields["seq"] = std::to_string(seq);

        std::string encoded = ipc::Encode(notify);
        {
            std::lock_guard<std::mutex> lock(send_queue_mutex_);
            // Shared-memory path: only keep the latest notification.
            frame_queue_.clear();
            frame_queue_.push_back(std::move(encoded));
        }
        if (running_) uv_async_send(&send_wakeup_);
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
            size_t expected = static_cast<size_t>(cursor.width) * cursor.height * 4;
            if (cursor.pixels.size() >= expected) {
                event.payload.assign(cursor.pixels.begin(),
                                     cursor.pixels.begin() + expected);
            } else {
                event.payload = cursor.pixels;
            }
        }

        std::string encoded = ipc::Encode(event);
        {
            std::lock_guard<std::mutex> lock(send_queue_mutex_);
            console_queue_.push_back(std::move(encoded));
        }
        if (running_) uv_async_send(&send_wakeup_);
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
        if (running_) uv_async_send(&send_wakeup_);
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
        if (running_) uv_async_send(&send_wakeup_);
    });

    audio_port_->SetPcmHandler([this](AudioChunk chunk) {
        ipc::Message event;
        event.kind = ipc::Kind::kEvent;
        event.channel = ipc::Channel::kAudio;
        event.type = "audio.pcm";
        event.vm_id = vm_id_;
        event.request_id = next_event_id_++;
        event.fields["sample_rate"] = std::to_string(chunk.sample_rate);
        event.fields["channels"] = std::to_string(chunk.channels);
        size_t byte_len = chunk.pcm.size() * sizeof(int16_t);
        event.payload.resize(byte_len);
        std::memcpy(event.payload.data(), chunk.pcm.data(), byte_len);

        std::string encoded = ipc::Encode(event);
        {
            std::lock_guard<std::mutex> lock(send_queue_mutex_);
            audio_queue_.push_back(std::move(encoded));
            if (audio_queue_.size() > kMaxPendingAudio) {
                audio_queue_.pop_front();
            }
        }
        if (running_) uv_async_send(&send_wakeup_);
    });
}

RuntimeControlService::~RuntimeControlService() {
    Stop();
}

bool RuntimeControlService::Start() {
    if (running_) return true;
    if (pipe_name_.empty()) return false;

    loop_thread_ = std::thread(&RuntimeControlService::EventLoopThread, this);

    std::unique_lock<std::mutex> lock(start_mutex_);
    start_cv_.wait(lock, [this] { return loop_ready_; });

    if (!pipe_connected_) {
        if (loop_thread_.joinable()) loop_thread_.join();
        return false;
    }

    running_ = true;
    return true;
}

void RuntimeControlService::Stop() {
    if (!running_) return;
    running_ = false;
    uv_async_send(&stop_wakeup_);

    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }
}

void RuntimeControlService::AttachVm(Vm* vm) {
    vm_ = vm;

    if (vm_) {
        console_port_->SetInputCallback([vm](const uint8_t* data, size_t size) {
            vm->InjectConsoleBytes(data, size);
        });

        input_port_->SetKeyEventCallback([vm](const KeyboardEvent& ev) {
            vm->InjectKeyEvent(ev.key_code, ev.pressed);
        });

        input_port_->SetPointerEventCallback([vm](const PointerEvent& ev) {
            vm->InjectPointerEvent(ev.x, ev.y, ev.buttons);
        });
    }

    if (vm_ && vm_->GetGuestAgentHandler()) {
        vm_->GetGuestAgentHandler()->SetConnectedCallback([this](bool connected) {
            ipc::Message event;
            event.kind = ipc::Kind::kEvent;
            event.channel = ipc::Channel::kControl;
            event.type = "guest_agent.state";
            event.vm_id = vm_id_;
            event.request_id = next_event_id_++;
            event.fields["connected"] = connected ? "1" : "0";
            Send(event);
            LOG_INFO("RuntimeService: guest agent %s", connected ? "connected" : "disconnected");
        });
    }
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

void RuntimeControlService::WriteRaw(const std::string& data) {
    if (!pipe_connected_ || data.empty()) return;

    auto* write_data = new std::string(data);
    auto* req = new uv_write_t;
    req->data = write_data;

    uv_buf_t buf = uv_buf_init(const_cast<char*>(write_data->data()),
                                static_cast<unsigned int>(write_data->size()));
    uv_write(req, reinterpret_cast<uv_stream_t*>(&pipe_), &buf, 1, OnWriteDone);
}

void RuntimeControlService::OnWriteDone(uv_write_t* req, int /*status*/) {
    delete static_cast<std::string*>(req->data);
    delete req;
}

bool RuntimeControlService::Send(const ipc::Message& message) {
    std::string encoded = ipc::Encode(message);
    {
        std::lock_guard<std::mutex> lock(send_queue_mutex_);
        console_queue_.push_back(std::move(encoded));
    }
    if (running_) uv_async_send(&send_wakeup_);
    return true;
}

bool RuntimeControlService::SendWithPayload(const ipc::Message& message) {
    std::string encoded = ipc::Encode(message);
    {
        std::lock_guard<std::mutex> lock(send_queue_mutex_);
        console_queue_.push_back(std::move(encoded));
    }
    if (running_) uv_async_send(&send_wakeup_);
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
            if (vm_ && vm_->IsGuestAgentConnected()) {
                vm_->GuestAgentShutdown("powerdown");
            } else if (vm_) {
                vm_->TriggerPowerButton();
                static const char kPoweroff[] = "\npoweroff\n";
                vm_->InjectConsoleBytes(
                    reinterpret_cast<const uint8_t*>(kPoweroff),
                    sizeof(kPoweroff) - 1);
            }
        } else if (cmd == "reboot") {
            if (vm_ && vm_->IsGuestAgentConnected()) {
                vm_->GuestAgentShutdown("reboot");
            } else if (vm_) {
                vm_->RequestStop();
                resp.fields["note"] = "guest agent unavailable, performed stop";
            }
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
        if (!vm_) {
            ipc::Message resp;
            resp.kind = ipc::Kind::kResponse;
            resp.channel = ipc::Channel::kControl;
            resp.type = "runtime.update_network.result";
            resp.vm_id = vm_id_;
            resp.request_id = message.request_id;
            resp.fields["ok"] = "false";
            resp.fields["error"] = "vm not attached";
            Send(resp);
            return;
        }

        auto it_link = message.fields.find("link_up");
        if (it_link != message.fields.end()) {
            vm_->SetNetLinkUp(it_link->second == "true");
        }

        std::vector<PortForward> forwards;
        auto it_count = message.fields.find("forward_count");
        if (it_count != message.fields.end()) {
            int count = 0;
            auto [p, ec] = std::from_chars(
                it_count->second.data(),
                it_count->second.data() + it_count->second.size(), count);
            if (ec == std::errc{} && count >= 0) {
                for (int i = 0; i < count; ++i) {
                    auto it_f = message.fields.find("forward_" + std::to_string(i));
                    if (it_f == message.fields.end()) continue;
                    PortForward pf;
                    if (PortForward::FromHostfwd(it_f->second.c_str(), pf)) {
                        forwards.push_back(pf);
                    }
                }
            }
        }

        // Parse guest forwards (guestfwd_count / guestfwd_N)
        std::vector<GuestForward> guest_forwards;
        auto it_gf_count = message.fields.find("guestfwd_count");
        if (it_gf_count != message.fields.end()) {
            int gf_count = 0;
            auto [gp, gec] = std::from_chars(
                it_gf_count->second.data(),
                it_gf_count->second.data() + it_gf_count->second.size(), gf_count);
            if (gec == std::errc{} && gf_count >= 0) {
                for (int i = 0; i < gf_count; ++i) {
                    auto it_gf = message.fields.find("guestfwd_" + std::to_string(i));
                    if (it_gf == message.fields.end()) continue;
                    GuestForward gf;
                    if (GuestForward::FromGuestfwd(it_gf->second.c_str(), gf)) {
                        guest_forwards.push_back(gf);
                    }
                }
            }
        }
        if (!guest_forwards.empty()) {
            vm_->UpdateGuestForwards(guest_forwards);
        }

        uint64_t req_id = message.request_id;
        vm_->UpdatePortForwards(forwards, [this, req_id](std::vector<uint16_t> failed_ports) {
            ipc::Message resp;
            resp.kind = ipc::Kind::kResponse;
            resp.channel = ipc::Channel::kControl;
            resp.type = "runtime.update_network.result";
            resp.vm_id = vm_id_;
            resp.request_id = req_id;
            if (!failed_ports.empty()) {
                resp.fields["ok"] = "false";
                resp.fields["failed_count"] = std::to_string(failed_ports.size());
                for (size_t i = 0; i < failed_ports.size(); ++i) {
                    resp.fields["failed_" + std::to_string(i)] =
                        std::to_string(failed_ports[i]);
                }
            } else {
                resp.fields["ok"] = "true";
            }
            Send(resp);
        });
        return;
    }

    if (message.channel == ipc::Channel::kControl &&
        message.kind == ipc::Kind::kRequest &&
        message.type == "runtime.update_shared_folders") {
        ipc::Message resp;
        resp.kind = ipc::Kind::kResponse;
        resp.channel = ipc::Channel::kControl;
        resp.type = "runtime.update_shared_folders.result";
        resp.vm_id = vm_id_;
        resp.request_id = message.request_id;

        if (!vm_) {
            resp.fields["ok"] = "false";
            resp.fields["error"] = "vm not attached";
            Send(resp);
            return;
        }

        auto it_count = message.fields.find("folder_count");
        if (it_count == message.fields.end()) {
            resp.fields["ok"] = "false";
            resp.fields["error"] = "missing folder_count";
            Send(resp);
            return;
        }

        int count = 0;
        auto [p, ec] = std::from_chars(
            it_count->second.data(),
            it_count->second.data() + it_count->second.size(), count);
        if (ec != std::errc{} || count < 0) {
            resp.fields["ok"] = "false";
            resp.fields["error"] = "invalid folder_count";
            Send(resp);
            return;
        }

        struct FolderSpec {
            std::string tag;
            std::string host_path;
            bool readonly;
        };
        std::vector<FolderSpec> new_folders;
        new_folders.reserve(count);

        for (int i = 0; i < count; ++i) {
            auto it_f = message.fields.find("folder_" + std::to_string(i));
            if (it_f == message.fields.end()) continue;

            const std::string& val = it_f->second;
            size_t pos1 = val.find('|');
            if (pos1 == std::string::npos) continue;
            size_t pos2 = val.find('|', pos1 + 1);
            if (pos2 == std::string::npos) continue;

            FolderSpec spec;
            spec.tag = val.substr(0, pos1);
            spec.host_path = val.substr(pos1 + 1, pos2 - pos1 - 1);
            spec.readonly = (val.substr(pos2 + 1) == "1");
            new_folders.push_back(std::move(spec));
        }

        std::vector<std::string> current_tags = vm_->GetSharedFolderTags();
        std::unordered_set<std::string> new_tags;
        for (const auto& f : new_folders) {
            new_tags.insert(f.tag);
        }

        for (const auto& tag : current_tags) {
            if (new_tags.find(tag) == new_tags.end()) {
                vm_->RemoveSharedFolder(tag);
            }
        }

        std::unordered_set<std::string> current_set(current_tags.begin(), current_tags.end());
        for (const auto& f : new_folders) {
            if (current_set.find(f.tag) == current_set.end()) {
                vm_->AddSharedFolder(f.tag, f.host_path, f.readonly);
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
            LOG_INFO("RuntimeService: display.set_size %ux%u", w, h);
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

void RuntimeControlService::FlushConsoleData() {
    std::string console_data = console_port_->FlushPending();
    if (console_data.empty()) return;

    ipc::Message event;
    event.kind = ipc::Kind::kEvent;
    event.channel = ipc::Channel::kConsole;
    event.type = "console.data";
    event.vm_id = vm_id_;
    event.request_id = next_event_id_++;
    event.fields["data_hex"] = EncodeHex(
        reinterpret_cast<const uint8_t*>(console_data.data()),
        console_data.size());
    WriteRaw(ipc::Encode(event));
}

void RuntimeControlService::DrainSendQueues() {
    std::string batch;
    {
        std::lock_guard<std::mutex> lock(send_queue_mutex_);

        while (!console_queue_.empty()) {
            batch += std::move(console_queue_.front());
            console_queue_.pop_front();
        }

        size_t audio_to_send = audio_queue_.size();
        while (audio_to_send-- > 0 && !audio_queue_.empty()) {
            batch += std::move(audio_queue_.front());
            audio_queue_.pop_front();
        }

        while (!frame_queue_.empty()) {
            batch += std::move(frame_queue_.front());
            frame_queue_.pop_front();
        }
    }

    if (batch.empty()) return;
    WriteRaw(batch);
}

void RuntimeControlService::OnAllocBuffer(uv_handle_t* /*handle*/, size_t suggested, uv_buf_t* buf) {
    buf->base = new char[suggested];
    buf->len = static_cast<decltype(buf->len)>(suggested);
}

void RuntimeControlService::OnPipeRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    auto* self = static_cast<RuntimeControlService*>(stream->data);

    if (nread < 0) {
        delete[] buf->base;
        uv_read_stop(stream);
        return;
    }
    if (nread == 0) {
        delete[] buf->base;
        return;
    }

    self->recv_pending_.append(buf->base, static_cast<size_t>(nread));
    delete[] buf->base;

    while (!self->recv_pending_.empty()) {
        if (self->recv_payload_needed_ > 0) {
            if (self->recv_pending_.size() < self->recv_payload_needed_) break;
            self->recv_pending_msg_.payload.assign(
                reinterpret_cast<const uint8_t*>(self->recv_pending_.data()),
                reinterpret_cast<const uint8_t*>(self->recv_pending_.data()) + self->recv_payload_needed_);
            self->recv_pending_.erase(0, self->recv_payload_needed_);
            self->recv_payload_needed_ = 0;
            self->HandleMessage(self->recv_pending_msg_);
            continue;
        }

        size_t nl = self->recv_pending_.find('\n');
        if (nl == std::string::npos) break;
        std::string line = self->recv_pending_.substr(0, nl + 1);
        self->recv_pending_.erase(0, nl + 1);
        auto decoded = ipc::Decode(line);
        if (!decoded) continue;

        auto ps_it = decoded->fields.find("payload_size");
        if (ps_it != decoded->fields.end()) {
            self->recv_payload_needed_ = std::strtoull(ps_it->second.c_str(), nullptr, 10);
            decoded->fields.erase(ps_it);
            if (self->recv_payload_needed_ > 0) {
                self->recv_pending_msg_ = std::move(*decoded);
                continue;
            }
        }
        self->HandleMessage(*decoded);
    }
}

void RuntimeControlService::OnSendWakeup(uv_async_t* handle) {
    auto* self = static_cast<RuntimeControlService*>(handle->data);

    // Start coalesce timer for console data if pending and timer not running
    if (self->console_port_->HasPending() && !self->coalesce_running_) {
        uv_timer_start(&self->console_coalesce_timer_, OnConsoleCoalesce, 20, 0);
        self->coalesce_running_ = true;
    }

    // Immediately drain non-console queues
    self->DrainSendQueues();
}

void RuntimeControlService::OnConsoleCoalesce(uv_timer_t* handle) {
    auto* self = static_cast<RuntimeControlService*>(handle->data);
    self->coalesce_running_ = false;
    self->FlushConsoleData();
}

static void RtcCloseWalkCb(uv_handle_t* handle, void*) {
    if (!uv_is_closing(handle)) uv_close(handle, nullptr);
}

void RuntimeControlService::OnPipeConnect(uv_connect_t* req, int status) {
    auto* self = static_cast<RuntimeControlService*>(req->data);
    if (status < 0) {
        LOG_ERROR("RuntimeService: uv_pipe_connect failed: %s", uv_strerror(status));
        {
            std::lock_guard<std::mutex> lock(self->start_mutex_);
            self->loop_ready_ = true;
        }
        self->start_cv_.notify_one();
        uv_walk(&self->loop_, RtcCloseWalkCb, nullptr);
        return;
    }

    self->pipe_connected_ = true;
    uv_read_start(reinterpret_cast<uv_stream_t*>(&self->pipe_), OnAllocBuffer, OnPipeRead);

    {
        std::lock_guard<std::mutex> lock(self->start_mutex_);
        self->loop_ready_ = true;
    }
    self->start_cv_.notify_one();
}

void RuntimeControlService::OnStopSignal(uv_async_t* handle) {
    auto* self = static_cast<RuntimeControlService*>(handle->data);
    self->pipe_connected_ = false;
    uv_walk(&self->loop_, RtcCloseWalkCb, nullptr);
}

void RuntimeControlService::EventLoopThread() {
    uv_loop_init(&loop_);

    pipe_.data = this;
    uv_pipe_init(&loop_, &pipe_, 0);

    send_wakeup_.data = this;
    uv_async_init(&loop_, &send_wakeup_, OnSendWakeup);

    stop_wakeup_.data = this;
    uv_async_init(&loop_, &stop_wakeup_, OnStopSignal);

    console_coalesce_timer_.data = this;
    uv_timer_init(&loop_, &console_coalesce_timer_);

    connect_req_.data = this;
    uv_pipe_connect(&connect_req_, &pipe_, pipe_name_.c_str(), OnPipeConnect);

    uv_run(&loop_, UV_RUN_DEFAULT);
    uv_loop_close(&loop_);
}
