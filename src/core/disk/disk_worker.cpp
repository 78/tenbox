#include "core/disk/disk_worker.h"
#include <utility>

DiskWorker::DiskWorker()
    : thread_(&DiskWorker::Run, this) {}

DiskWorker::~DiskWorker() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable())
        thread_.join();
}

void DiskWorker::Submit(Task task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
}

void DiskWorker::Run() {
    std::vector<Task> batch;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty())
                return;
            batch.swap(queue_);
        }
        for (auto& task : batch)
            task();
        batch.clear();
    }
}
