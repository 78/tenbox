#include "core/vmm/console_tx_batcher.h"

#include "core/vmm/vm_io_loop.h"

#include <utility>

ConsoleTxBatcher::ConsoleTxBatcher(RawWriter writer) : writer_(std::move(writer)) {}

ConsoleTxBatcher::~ConsoleTxBatcher() {
    // By contract the io_loop has already been stopped by whoever owns us
    // (Vm::~Vm), so any armed timer has been closed and its capture of
    // `this` released. We just need to drain whatever the guest wrote
    // after the last timer flush so tail-end console output isn't lost.
    Flush();
}

void ConsoleTxBatcher::AttachIoLoop(VmIoLoop* loop) {
    std::lock_guard<std::mutex> lock(mu_);
    io_loop_ = loop;
}

void ConsoleTxBatcher::Append(const uint8_t* data, size_t size) {
    if (!data || size == 0) return;

    std::unique_lock<std::mutex> lock(mu_);

    // If no loop is attached yet, or it's been torn down, we can't
    // schedule a delayed flush. Drain any previously-buffered bytes
    // first (preserve order) and write this chunk through synchronously.
    if (!io_loop_ || !io_loop_->running()) {
        if (!buf_.empty()) FlushLocked(lock);
        RawWriter w = writer_;
        lock.unlock();
        if (w) w(data, size);
        return;
    }

    buf_.append(reinterpret_cast<const char*>(data), size);

    if (buf_.size() >= kFlushThreshold) {
        FlushLocked(lock);
        return;
    }

    if (!timer_armed_) {
        timer_armed_ = true;
        timer_id_ = io_loop_->AddTimer(kFlushDelayMs, [this]() -> uint64_t {
            return OnTimerFire();
        });
    }
}

void ConsoleTxBatcher::Flush() {
    std::unique_lock<std::mutex> lock(mu_);
    if (!buf_.empty()) FlushLocked(lock);
}

void ConsoleTxBatcher::FlushLocked(std::unique_lock<std::mutex>& lock) {
    std::string pending;
    pending.swap(buf_);
    RawWriter w = writer_;
    lock.unlock();
    if (w && !pending.empty()) {
        w(reinterpret_cast<const uint8_t*>(pending.data()), pending.size());
    }
    lock.lock();
}

uint64_t ConsoleTxBatcher::OnTimerFire() {
    std::unique_lock<std::mutex> lock(mu_);
    timer_armed_ = false;
    timer_id_ = 0;
    if (!buf_.empty()) FlushLocked(lock);
    return 0;  // self-destruct; next Append() re-arms
}
