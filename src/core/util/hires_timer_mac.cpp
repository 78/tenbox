#include "core/util/hires_timer.h"

#include <dispatch/dispatch.h>

#include <atomic>
#include <memory>
#include <mutex>

// Defined in hires_timer_uv.cpp for non-Windows builds that want a fallback.
std::unique_ptr<HiResTimer> MakeUvHiResTimer(VmIoLoop* fallback_loop);

namespace {

// Grand Central Dispatch-backed timer. GCD's DISPATCH_SOURCE_TYPE_TIMER
// accepts nanosecond intervals and a leeway parameter, which the kernel
// uses to coalesce fires with other wakeups when possible. For LAPIC timer
// we want low latency, so leeway is set to 0 (strict) — the kernel may
// still coalesce but only with other strict timers.
//
// All timers share a single serial dispatch queue. This is important for
// correctness: FireTimer mutates shared LocalApic state under its own
// mutex, but running ArmTimer / StopTimer from the vCPU thread alongside
// many concurrent FireTimer callbacks on a concurrent queue would create
// pathological lock contention. A serial queue serialises tick delivery
// across all vCPUs, matching VmIoLoop's single-threaded dispatch.
//
// (We could also give every timer its own serial queue; empirically the
// shared-queue approach used to ship pre-refactor and worked fine up to
// 8-vCPU Linux guests.)
class MacHiResTimer : public HiResTimer {
public:
    MacHiResTimer() {
        queue_ = SharedQueue();
    }

    ~MacHiResTimer() override { Stop(); }

    void Arm(uint64_t period_ns, Callback cb) override {
        Stop();
        if (!cb) return;

        Callback local_cb = std::move(cb);

        // dispatch_source_create is safe from any thread.
        dispatch_source_t s = dispatch_source_create(
            DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue_);
        if (!s) return;

        // Periodic: interval = period_ns. We implement "reschedule based on
        // the callback's return value" by updating the timer from the
        // handler itself; this matches the Windows impl and lets the
        // caller decide whether each fire should repeat or stop.
        dispatch_source_set_timer(
            s,
            dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(period_ns)),
            period_ns,
            /*leeway*/ 0);

        // Capture cb_holder_ (a shared_ptr) by value into the block so the
        // handler keeps working even if Arm() is called again (which will
        // swap cb_holder_ before the old timer is fully cancelled). Each
        // Arm() gets its own cb copy; the cancel path drops the source.
        auto cb_holder = std::make_shared<Callback>(std::move(local_cb));
        dispatch_source_t src_copy = s;
        dispatch_source_set_event_handler(s, ^{
            uint64_t next_ns = (*cb_holder)();
            if (next_ns == 0) {
                // One-shot done or explicit stop: cancel ourselves.
                dispatch_source_cancel(src_copy);
                return;
            }
            // Reschedule at the new period.
            dispatch_source_set_timer(
                src_copy,
                dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(next_ns)),
                next_ns,
                /*leeway*/ 0);
        });

        dispatch_source_set_cancel_handler(s, ^{
            dispatch_release(src_copy);
        });

        {
            std::lock_guard<std::mutex> lk(mu_);
            source_ = s;
        }
        dispatch_resume(s);
    }

    void Stop() override {
        dispatch_source_t old = nullptr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            old = source_;
            source_ = nullptr;
        }
        if (old) {
            dispatch_source_cancel(old);
            // The cancel handler releases the source once pending fires have
            // drained. We can't release here without risking a double-release.
        }
    }

private:
    static dispatch_queue_t SharedQueue() {
        // Built-in QOS_CLASS_USER_INTERACTIVE serial queue, attached to a
        // high-priority global queue as its target. Using USER_INTERACTIVE
        // keeps LAPIC ticks out of the "background" bucket on Apple Silicon
        // E-cores.
        static dispatch_queue_t q = []() {
            dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
                DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
            return dispatch_queue_create("com.tenbox.lapic.timer", attr);
        }();
        return q;
    }

    dispatch_queue_t queue_ = nullptr;
    std::mutex mu_;
    dispatch_source_t source_ = nullptr;
};

} // namespace

std::unique_ptr<HiResTimer> MakeHiResTimer(VmIoLoop* fallback_loop) {
    auto t = std::make_unique<MacHiResTimer>();
    // MacHiResTimer ctor is infallible for our purposes; the underlying
    // queue/source allocations only fail under severe memory pressure,
    // in which case Arm() simply becomes a no-op. Return the libuv
    // fallback only when the caller signals that's preferred (null here
    // means "use platform best"; we pick dispatch every time on macOS).
    (void)fallback_loop;
    return t;
}
