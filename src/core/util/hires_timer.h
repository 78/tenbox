#pragma once

#include <cstdint>
#include <functional>
#include <memory>

class VmIoLoop;

// High-resolution, one-shot-or-periodic timer used to drive guest-visible
// clocks (LAPIC timer, HPET) that need sub-millisecond precision.
//
// On Windows (since Windows 10 1803) we use CreateWaitableTimerExW with the
// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION flag, which gives a ~0.5 ms tick
// granularity without requiring timeBeginPeriod(1) (which would globally
// affect the host's scheduler tick).
//
// On other platforms (macOS, Linux) we fall back to VmIoLoop's libuv timer,
// which is also ~1 ms precision. This matches the status quo and costs us
// nothing: macOS runs LAPIC timer via TSC-deadline + APICv, so the software
// path doesn't fire often enough to matter; Linux KVM does it all in-kernel.
//
// Thread-safety: Arm()/Stop() may be called from any thread. The callback is
// invoked on an implementation-defined thread (Windows: a dedicated per-VM
// timer thread; other: the VmIoLoop thread). Callbacks must serialise their
// own state.
class HiResTimer {
public:
    // Returns the period in NANOSECONDS until the next fire, or 0 to stop.
    using Callback = std::function<uint64_t()>;

    virtual ~HiResTimer() = default;

    // Arm the timer. `period_ns` is the first-fire delay AND (for periodic
    // timers) the repeat interval. Calling Arm() on an already-armed timer
    // cancels the previous schedule and re-arms.
    virtual void Arm(uint64_t period_ns, Callback cb) = 0;

    // Cancel the pending fire, if any. After Stop() returns the callback is
    // guaranteed not to run again, although it may already be executing on
    // another thread.
    virtual void Stop() = 0;
};

// Factory. Returns the best available HiResTimer for the host. `fallback_loop`
// is used only on platforms where we fall back to VmIoLoop; it may be null if
// the caller doesn't want any fallback (in which case non-Windows platforms
// will get a no-op timer).
std::unique_ptr<HiResTimer> MakeHiResTimer(VmIoLoop* fallback_loop);
