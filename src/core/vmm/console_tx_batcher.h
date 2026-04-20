#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

class VmIoLoop;

// Coalesces the 1-byte-at-a-time UART tx stream into larger chunks before
// handing them to the downstream sink (ConsolePort -> stdout / IPC pipe).
//
// Guest UARTs (pl011, 16550) call into us via a TxCallback that fires once
// per MMIO/PIO write. Boot log alone can push thousands of bytes this way,
// each currently turning into one ::write()/WriteFile() syscall on the host.
// This class buffers writes and flushes them in two scenarios:
//   - The pending buffer reaches kFlushThreshold bytes: synchronously flush
//     on the caller (vCPU) thread. Bounds worst-case latency for bursts.
//   - An io_loop timer fires kFlushDelayMs after the first byte landed.
//     Handles the slow interactive-echo case where bytes dribble in below
//     the threshold.
//
// Thread-safety: all public methods are safe to call from any thread.
// The RawWriter is invoked without holding the internal mutex so that slow
// syscalls don't serialize Append() calls from different vCPUs.
//
// Lifetime: the loop must outlive this object. In the Vm ownership chain
// that's guaranteed because Vm::~Vm explicitly stops io_loop_ (which joins
// its thread and closes any armed timer) before machine_ -- and therefore
// this batcher -- is destroyed.
class ConsoleTxBatcher {
public:
    using RawWriter = std::function<void(const uint8_t*, size_t)>;

    explicit ConsoleTxBatcher(RawWriter writer);
    ~ConsoleTxBatcher();

    ConsoleTxBatcher(const ConsoleTxBatcher&) = delete;
    ConsoleTxBatcher& operator=(const ConsoleTxBatcher&) = delete;

    // Attach the io loop used for delayed flushes. Safe to call before
    // the loop is started; buffering begins when the loop becomes running.
    // Passing nullptr detaches and forces Append() to go synchronous.
    void AttachIoLoop(VmIoLoop* loop);

    // Append bytes to the tx buffer. If the loop is unavailable or not
    // running, bytes are written through synchronously to preserve output
    // across Vm startup/shutdown edges.
    void Append(const uint8_t* data, size_t size);

    // Synchronous flush of whatever is currently buffered.
    void Flush();

private:
    // Hands buf_ contents to writer_. Releases `lock` around the writer
    // call and re-acquires it before returning. Leaves buf_ empty.
    void FlushLocked(std::unique_lock<std::mutex>& lock);

    // Runs on io_thread_ when the coalesce timer fires. Returns 0 so the
    // timer self-destructs; the next Append() will re-arm it.
    uint64_t OnTimerFire();

    RawWriter writer_;
    VmIoLoop* io_loop_ = nullptr;

    std::mutex mu_;
    std::string buf_;
    bool timer_armed_ = false;
    uint64_t timer_id_ = 0;

    static constexpr size_t kFlushThreshold = 1024;
    static constexpr uint64_t kFlushDelayMs = 16;
};
