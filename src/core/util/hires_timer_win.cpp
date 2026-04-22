#include "core/util/hires_timer.h"
#include "core/vmm/types.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

// Defined in hires_timer_uv.cpp for Windows builds so we can fall back to it
// if the high-resolution waitable timer isn't supported.
std::unique_ptr<HiResTimer> MakeUvHiResTimer(VmIoLoop* fallback_loop);

namespace {

// Detect whether CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is accepted by the
// host. Supported since Windows 10 1803 (same target as WHPX 1803 fallback
// — which is the whole reason this file exists). Cached after first probe.
bool ProbeHighResTimerSupport() {
    static std::atomic<int> cached{-1};  // -1 unknown, 0 no, 1 yes
    int v = cached.load(std::memory_order_acquire);
    if (v >= 0) return v == 1;

    // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is 0x00000002. Older SDK headers
    // may not define it, so hard-code the value.
    constexpr DWORD kHighRes = 0x00000002;
    HANDLE h = CreateWaitableTimerExW(nullptr, nullptr,
                                      kHighRes,
                                      TIMER_ALL_ACCESS);
    if (h) {
        CloseHandle(h);
        cached.store(1, std::memory_order_release);
        return true;
    }
    cached.store(0, std::memory_order_release);
    return false;
}

// Native implementation. One dedicated thread per timer because LAPIC has
// at most cpu_count (<=64) timers and they're long-lived; this avoids
// sharing a thread pool and the callback-skew that comes with it.
class WinHiResTimer : public HiResTimer {
public:
    WinHiResTimer() {
        constexpr DWORD kHighRes = 0x00000002;
        handle_ = CreateWaitableTimerExW(nullptr, nullptr,
                                         kHighRes,
                                         TIMER_ALL_ACCESS);
    }

    ~WinHiResTimer() override {
        Stop();
        if (handle_) {
            // Setting the stop_ flag and cancelling the timer wakes the
            // worker thread out of WaitForSingleObject.
            stop_.store(true, std::memory_order_release);
            CancelWaitableTimer(handle_);
            if (thread_.joinable()) thread_.join();
            CloseHandle(handle_);
            handle_ = nullptr;
        }
    }

    bool IsValid() const { return handle_ != nullptr; }

    void Arm(uint64_t period_ns, Callback cb) override {
        if (!handle_) return;

        // Ensure the previous worker is quiesced before changing the cb.
        Stop();

        {
            std::lock_guard<std::mutex> lk(mu_);
            cb_ = std::move(cb);
            period_ns_ = period_ns;
            stop_.store(false, std::memory_order_release);
        }

        // Kick the timer. Due time in 100-ns units, negative = relative.
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(period_ns / 100);
        if (due.QuadPart == 0) due.QuadPart = -1;  // at least 100 ns

        // Period param is in ms; we explicitly pass 0 and re-arm inside the
        // worker thread so we can stay at sub-ms precision.
        if (!SetWaitableTimer(handle_, &due, 0, nullptr, nullptr, FALSE)) {
            LOG_WARN("HiResTimer: SetWaitableTimer failed: %lu",
                     (unsigned long)GetLastError());
            return;
        }

        // Spawn the worker on first Arm(). It lives until destruction or
        // explicit Stop() with empty cb. Subsequent Arm() just updates cb_
        // / period_ns_ under mu_ and the worker picks it up.
        if (!thread_.joinable()) {
            thread_ = std::thread([this]() { WorkerLoop(); });
        }
    }

    void Stop() override {
        if (!handle_) return;
        CancelWaitableTimer(handle_);
        std::lock_guard<std::mutex> lk(mu_);
        cb_ = {};
        period_ns_ = 0;
    }

private:
    void WorkerLoop() {
        while (!stop_.load(std::memory_order_acquire)) {
            DWORD r = WaitForSingleObject(handle_, INFINITE);
            if (r != WAIT_OBJECT_0) {
                // Handle closed or wait abandoned: bail.
                break;
            }
            if (stop_.load(std::memory_order_acquire)) break;

            // Snapshot the callback under the lock so Arm()/Stop() from
            // another thread doesn't yank it mid-call.
            Callback cb_local;
            {
                std::lock_guard<std::mutex> lk(mu_);
                cb_local = cb_;
            }
            if (!cb_local) continue;

            uint64_t next_ns = cb_local();
            if (next_ns == 0) continue;  // one-shot done, worker idles

            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(next_ns / 100);
            if (due.QuadPart == 0) due.QuadPart = -1;
            SetWaitableTimer(handle_, &due, 0, nullptr, nullptr, FALSE);
        }
    }

    HANDLE handle_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};

    std::mutex mu_;
    Callback cb_;
    uint64_t period_ns_ = 0;
};

} // namespace

std::unique_ptr<HiResTimer> MakeHiResTimer(VmIoLoop* fallback_loop) {
    if (ProbeHighResTimerSupport()) {
        auto t = std::make_unique<WinHiResTimer>();
        if (t->IsValid()) return t;
    }
    // Build lacks high-res support — fall back to libuv's ms-precision timer.
    return MakeUvHiResTimer(fallback_loop);
}
