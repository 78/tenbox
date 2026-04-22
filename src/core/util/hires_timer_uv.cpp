#include "core/util/hires_timer.h"
#include "core/vmm/vm_io_loop.h"

#include <atomic>
#include <mutex>

namespace {

// VmIoLoop-backed fallback. Millisecond precision.
class UvHiResTimer : public HiResTimer {
public:
    explicit UvHiResTimer(VmIoLoop* loop) : loop_(loop) {}

    ~UvHiResTimer() override { Stop(); }

    void Arm(uint64_t period_ns, Callback cb) override {
        Stop();
        if (!loop_ || !cb) return;

        // Clamp to >=1 ms because libuv can't do better. Callers that need
        // sub-ms precision must use the platform-specific implementation.
        uint64_t period_ms = period_ns / 1'000'000ULL;
        if (period_ms == 0) period_ms = 1;

        // Capture cb by value so the lambda stays alive independently of the
        // caller. Chaining: the VmIoLoop callback returns ms-until-next-fire,
        // while our Callback returns ns-until-next-fire, so convert.
        Callback local_cb = std::move(cb);
        std::lock_guard<std::mutex> lk(mu_);
        id_ = loop_->AddTimer(period_ms, [local_cb]() -> uint64_t {
            uint64_t next_ns = local_cb();
            if (next_ns == 0) return 0;
            uint64_t next_ms = next_ns / 1'000'000ULL;
            if (next_ms == 0) next_ms = 1;
            return next_ms;
        });
    }

    void Stop() override {
        uint64_t id = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            id = id_;
            id_ = 0;
        }
        if (id && loop_) loop_->RemoveTimer(id);
    }

private:
    VmIoLoop* loop_;
    std::mutex mu_;
    uint64_t id_ = 0;
};

// No-op when the caller didn't provide a fallback loop and the platform
// doesn't ship a native HiResTimer.
class NoopHiResTimer : public HiResTimer {
public:
    void Arm(uint64_t, Callback) override {}
    void Stop() override {}
};

} // namespace

// Windows (hires_timer_win.cpp) and macOS (hires_timer_mac.cpp) provide their
// own MakeHiResTimer. On Linux / other Unixes the libuv fallback IS the
// hi-res timer — its precision is bounded by libuv (~1 ms on Linux), which
// is good enough because we don't actually target any Linux WHPX-equivalent
// that drives guest LAPIC timer through us.
#if !defined(_WIN32) && !defined(__APPLE__)
std::unique_ptr<HiResTimer> MakeHiResTimer(VmIoLoop* fallback_loop) {
    if (fallback_loop) {
        return std::make_unique<UvHiResTimer>(fallback_loop);
    }
    return std::make_unique<NoopHiResTimer>();
}
#else
// On Windows / macOS the native implementations may still want to fall back
// to the libuv timer (e.g. if the native API allocation fails). Expose a
// separate factory for that.
std::unique_ptr<HiResTimer> MakeUvHiResTimer(VmIoLoop* fallback_loop) {
    if (fallback_loop) return std::make_unique<UvHiResTimer>(fallback_loop);
    return std::make_unique<NoopHiResTimer>();
}
#endif
