#pragma once

#include <uv.h>

// RAII wrapper around uv_poll_t that tracks init/active/closing state.
// Eliminates the repeated {poll_handle, poll_inited, poll_active, poll_closing}
// pattern found in NatEntry and PfEntry::Conn.
class PollHandle {
public:
    PollHandle() = default;
    PollHandle(const PollHandle&) = delete;
    PollHandle& operator=(const PollHandle&) = delete;
    PollHandle(PollHandle&& o) noexcept
        : handle_(o.handle_), inited_(o.inited_), active_(o.active_), closing_(o.closing_) {
        o.inited_ = false;
        o.active_ = false;
        o.closing_ = false;
    }
    PollHandle& operator=(PollHandle&& o) noexcept {
        if (this != &o) {
            handle_ = o.handle_;
            inited_ = o.inited_;
            active_ = o.active_;
            closing_ = o.closing_;
            o.inited_ = false;
            o.active_ = false;
            o.closing_ = false;
        }
        return *this;
    }

    void Init(uv_loop_t* loop, uv_os_sock_t sock) {
        if (inited_) return;
        uv_poll_init_socket(loop, &handle_, sock);
        inited_ = true;
    }

    void Start(int events, uv_poll_cb cb, void* data) {
        if (closing_) return;
        handle_.data = data;
        if (!active_) active_ = true;
        uv_poll_start(&handle_, events, cb);
    }

    void Stop() {
        if (active_) {
            uv_poll_stop(&handle_);
            active_ = false;
        }
    }

    void Close() {
        if (closing_ || !inited_) return;
        Stop();
        closing_ = true;
        uv_close(reinterpret_cast<uv_handle_t*>(&handle_), nullptr);
    }

    bool inited() const { return inited_; }
    bool active() const { return active_; }
    bool closing() const { return closing_; }
    uv_poll_t* raw() { return &handle_; }

private:
    uv_poll_t handle_{};
    bool inited_ = false;
    bool active_ = false;
    bool closing_ = false;
};
