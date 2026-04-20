#include "core/vmm/vm_io_loop.h"

#include "core/device/virtio/virtio_mmio.h"
#include "core/vmm/types.h"

#if defined(__linux__)
#include <unistd.h>
#endif

VmIoLoop::VmIoLoop() = default;

VmIoLoop::~VmIoLoop() {
    Stop();
}

bool VmIoLoop::Start() {
    {
        std::lock_guard<std::mutex> lock(post_mutex_);
        if (accepting_) return true;
    }

    int rc = uv_loop_init(&loop_);
    if (rc != 0) {
        LOG_ERROR("VmIoLoop: uv_loop_init failed: %s", uv_strerror(rc));
        return false;
    }

    async_post_.data = this;
    rc = uv_async_init(&loop_, &async_post_, OnAsyncPost);
    if (rc != 0) {
        LOG_ERROR("VmIoLoop: uv_async_init(post) failed: %s", uv_strerror(rc));
        (void)uv_loop_close(&loop_);
        return false;
    }

    async_stop_.data = this;
    rc = uv_async_init(&loop_, &async_stop_, OnAsyncStop);
    if (rc != 0) {
        LOG_ERROR("VmIoLoop: uv_async_init(stop) failed: %s", uv_strerror(rc));
        uv_close(reinterpret_cast<uv_handle_t*>(&async_post_), nullptr);
        while (uv_run(&loop_, UV_RUN_NOWAIT) != 0) {}
        (void)uv_loop_close(&loop_);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(post_mutex_);
        accepting_ = true;
    }
    running_.store(true, std::memory_order_release);
    io_thread_ = std::thread(&VmIoLoop::ThreadMain, this);
    return true;
}

void VmIoLoop::Stop() {
    {
        std::lock_guard<std::mutex> lock(post_mutex_);
        if (!accepting_) return;
        accepting_ = false;
    }

    uv_async_send(&async_stop_);

    if (io_thread_.joinable()) io_thread_.join();

    running_.store(false, std::memory_order_release);

    // Drop pending Post tasks without running them; captures destruct.
    std::deque<Task> drained;
    {
        std::lock_guard<std::mutex> lock(post_mutex_);
        drained.swap(post_queue_);
    }
}

void VmIoLoop::ThreadMain() {
    uv_run(&loop_, UV_RUN_DEFAULT);
    while (uv_run(&loop_, UV_RUN_NOWAIT) != 0) {}
    (void)uv_loop_close(&loop_);
}

void VmIoLoop::Post(Task fn) {
    std::lock_guard<std::mutex> lock(post_mutex_);
    if (!accepting_) return;
    post_queue_.push_back(std::move(fn));
    uv_async_send(&async_post_);
}

void VmIoLoop::OnAsyncPost(uv_async_t* h) {
    auto* self = static_cast<VmIoLoop*>(h->data);

    // uv_async coalesces wakeups, so drain everything in one shot.
    std::deque<Task> drained;
    {
        std::lock_guard<std::mutex> lock(self->post_mutex_);
        drained.swap(self->post_queue_);
    }

    // If the stop callback ran first this iteration, drop everything: newly
    // created uv handles here would never be closed before uv_loop_close.
    if (self->io_stopped_) return;

    for (auto& fn : drained) {
        if (fn) fn();
    }
}

void VmIoLoop::OnAsyncStop(uv_async_t* h) {
    auto* self = static_cast<VmIoLoop*>(h->data);
    self->io_stopped_ = true;

    // Destroy outstanding timers. Close callbacks delete the ctx on the
    // next iteration (uv_close is async).
    for (auto& kv : self->timers_) {
        auto* ctx = kv.second;
        uv_timer_stop(&ctx->handle);
        uv_close(reinterpret_cast<uv_handle_t*>(&ctx->handle),
                 [](uv_handle_t* h2) {
                     delete static_cast<VmIoLoop::TimerCtx*>(h2->data);
                 });
    }
    self->timers_.clear();

    for (auto& kv : self->irqfds_) {
        auto* ctx = kv.second;
        uv_poll_stop(&ctx->handle);
        uv_close(reinterpret_cast<uv_handle_t*>(&ctx->handle),
                 [](uv_handle_t* h2) {
                     delete static_cast<VmIoLoop::IrqFdCtx*>(h2->data);
                 });
    }
    self->irqfds_.clear();

    uv_close(reinterpret_cast<uv_handle_t*>(&self->async_post_), nullptr);
    uv_close(reinterpret_cast<uv_handle_t*>(h), nullptr);
}

uint64_t VmIoLoop::AddTimer(uint64_t initial_ms, TimerCallback cb) {
    uint64_t id = next_timer_id_.fetch_add(1, std::memory_order_relaxed);
    Post([this, id, initial_ms, cb = std::move(cb)]() mutable {
        if (io_stopped_) return;
        auto* ctx = new TimerCtx{};
        ctx->owner = this;
        ctx->id = id;
        ctx->cb = std::move(cb);
        ctx->handle.data = ctx;
        if (uv_timer_init(&loop_, &ctx->handle) != 0) {
            delete ctx;
            return;
        }
        timers_[id] = ctx;
        uv_timer_start(&ctx->handle, OnTimerFire, initial_ms, 0);
    });
    return id;
}

void VmIoLoop::RemoveTimer(uint64_t id) {
    Post([this, id]() {
        auto it = timers_.find(id);
        if (it == timers_.end()) return;
        auto* ctx = it->second;
        timers_.erase(it);
        uv_timer_stop(&ctx->handle);
        uv_close(reinterpret_cast<uv_handle_t*>(&ctx->handle),
                 [](uv_handle_t* h) {
                     delete static_cast<TimerCtx*>(h->data);
                 });
    });
}

void VmIoLoop::OnTimerFire(uv_timer_t* t) {
    auto* ctx = static_cast<TimerCtx*>(t->data);
    uint64_t next_ms = ctx->cb ? ctx->cb() : 0;
    if (next_ms == 0) {
        auto* owner = ctx->owner;
        owner->timers_.erase(ctx->id);
        uv_timer_stop(&ctx->handle);
        uv_close(reinterpret_cast<uv_handle_t*>(&ctx->handle),
                 [](uv_handle_t* h) {
                     delete static_cast<TimerCtx*>(h->data);
                 });
    } else {
        uv_timer_start(&ctx->handle, OnTimerFire, next_ms, 0);
    }
}

void VmIoLoop::AttachIrqFd(VirtioMmioDevice* dev, int trigger_fd, int resample_fd) {
#if defined(__linux__)
    if (!dev || trigger_fd < 0 || resample_fd < 0) return;
    Post([this, dev, trigger_fd, resample_fd]() {
        if (io_stopped_) return;
        if (irqfds_.count(dev)) return;  // idempotent
        auto* ctx = new IrqFdCtx{};
        ctx->owner = this;
        ctx->dev = dev;
        ctx->trigger_fd = trigger_fd;
        ctx->resample_fd = resample_fd;
        ctx->handle.data = ctx;
        if (uv_poll_init(&loop_, &ctx->handle, resample_fd) != 0) {
            LOG_WARN("VmIoLoop: uv_poll_init(irqfd) failed");
            delete ctx;
            return;
        }
        irqfds_[dev] = ctx;
        uv_poll_start(&ctx->handle, UV_READABLE, OnIrqFdReadable);
    });
#else
    (void)dev;
    (void)trigger_fd;
    (void)resample_fd;
#endif
}

void VmIoLoop::DetachIrqFd(VirtioMmioDevice* dev) {
#if defined(__linux__)
    if (!dev) return;
    Post([this, dev]() {
        auto it = irqfds_.find(dev);
        if (it == irqfds_.end()) return;
        auto* ctx = it->second;
        irqfds_.erase(it);
        uv_poll_stop(&ctx->handle);
        uv_close(reinterpret_cast<uv_handle_t*>(&ctx->handle),
                 [](uv_handle_t* h) {
                     delete static_cast<IrqFdCtx*>(h->data);
                 });
    });
#else
    (void)dev;
#endif
}

void VmIoLoop::OnIrqFdReadable(uv_poll_t* p, int status, int events) {
#if defined(__linux__)
    (void)events;
    auto* ctx = static_cast<IrqFdCtx*>(p->data);
    if (status < 0) {
        LOG_WARN("VmIoLoop: irqfd poll error: %s", uv_strerror(status));
        return;
    }
    uint64_t v = 0;
    (void)::read(ctx->resample_fd, &v, sizeof(v));
    if (ctx->dev && ctx->dev->GetInterruptStatus() != 0 && ctx->trigger_fd >= 0) {
        uint64_t one = 1;
        (void)::write(ctx->trigger_fd, &one, sizeof(one));
    }
#else
    (void)p;
    (void)status;
    (void)events;
#endif
}
