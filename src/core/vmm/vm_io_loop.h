#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <uv.h>

class VirtioMmioDevice;

// Per-Vm device I/O event loop. Owns a single libuv loop running on its own
// io_thread_, and is the central place to:
//   - Drive the irqfd resample path (Linux): uv_poll on a resample eventfd,
//     re-assert the trigger eventfd if the device still has pending bits.
//   - Host timers for virtio devices (e.g. virtio_snd period tick) so that
//     epoll_wait's timeout naturally folds in the next timer deadline.
//   - Serve as a single point for other components to Post work to a
//     known-safe thread (libuv handles are single-threaded).
//
// Concurrency contract (see plan):
//   * All public methods are thread-safe. Call them from any thread.
//   * Internally, every uv_* call other than uv_async_send happens on
//     io_thread_. Cross-thread entry points Post a closure onto a queue and
//     wake the loop with uv_async_send.
//   * Post is FIFO within a single caller thread; cross-thread ordering is
//     by the moment the task acquires the queue mutex.
//   * Stop() does NOT execute the remaining posted tasks: captures are
//     destroyed (releasing shared_ptr etc.), but the functions are not
//     called. Devices must not rely on "all work drains" semantics.
class VmIoLoop {
public:
    // Returned by callbacks in AddTimer: number of ms until the next fire,
    // or 0 to stop (and destroy) the timer.
    using TimerCallback = std::function<uint64_t()>;
    using Task = std::function<void()>;

    VmIoLoop();
    ~VmIoLoop();

    VmIoLoop(const VmIoLoop&) = delete;
    VmIoLoop& operator=(const VmIoLoop&) = delete;

    // Spawn io_thread_ and bring up the uv_loop. Must be called once before
    // any other method. Subsequent calls are a no-op and return true.
    bool Start();

    // Close all handles, join io_thread_, drop pending Post tasks without
    // running them. Idempotent.
    void Stop();

    bool running() const { return running_.load(std::memory_order_acquire); }

    // Submit fn to run on io_thread_ (FIFO per caller).
    void Post(Task fn);

    // Schedule a timer. `initial_ms` is the delay to the first fire;
    // subsequent fires use whatever ms the callback returns (0 = stop and
    // destroy). For a classic fixed-interval timer, have the callback always
    // return the interval.
    // Returns an opaque id usable with RemoveTimer (allocated eagerly on the
    // caller's thread; the underlying uv_timer_t is created asynchronously
    // on io_thread_).
    uint64_t AddTimer(uint64_t initial_ms, TimerCallback cb);

    // Cancel a timer scheduled by AddTimer. Safe to call even if the timer
    // has already self-destructed (returned 0 from its callback) or was
    // never actually installed (e.g. cancelled before AddTimer's post ran).
    void RemoveTimer(uint64_t id);

    // Attach a Linux eventfd pair to this loop for irqfd resample handling.
    // When `resample_fd` becomes readable (kernel signalled it on guest EOI),
    // we drain the counter and, if the device still has pending interrupt
    // bits, write(trigger_fd) to re-assert the GIC/IOAPIC line. The fds'
    // lifetime is the caller's responsibility; call DetachIrqFd before
    // closing them. No-op on non-Linux.
    void AttachIrqFd(VirtioMmioDevice* dev, int trigger_fd, int resample_fd);
    void DetachIrqFd(VirtioMmioDevice* dev);

public:
    // Public for static-callback access; treat as implementation detail.
    struct TimerCtx {
        uv_timer_t handle{};
        VmIoLoop* owner = nullptr;
        uint64_t id = 0;
        TimerCallback cb;
    };
    struct IrqFdCtx {
        uv_poll_t handle{};
        VmIoLoop* owner = nullptr;
        VirtioMmioDevice* dev = nullptr;
        int trigger_fd = -1;
        int resample_fd = -1;
    };

private:
    void ThreadMain();
    static void OnAsyncPost(uv_async_t* h);
    static void OnAsyncStop(uv_async_t* h);
    static void OnTimerFire(uv_timer_t* t);
    static void OnIrqFdReadable(uv_poll_t* p, int status, int events);

    uv_loop_t loop_{};
    uv_async_t async_post_{};
    uv_async_t async_stop_{};
    std::thread io_thread_;
    std::atomic<bool> running_{false};

    std::mutex post_mutex_;
    std::deque<Task> post_queue_;
    bool accepting_ = false;

    std::atomic<uint64_t> next_timer_id_{1};

    // Accessed only from io_thread_.
    std::unordered_map<uint64_t, TimerCtx*> timers_;
    std::unordered_map<VirtioMmioDevice*, IrqFdCtx*> irqfds_;
    bool io_stopped_ = false;
};
