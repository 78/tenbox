#pragma once

#include "core/vmm/types.h"
#include "core/disk/disk_worker.h"
#include <functional>
#include <string>
#include <memory>

class DiskImage {
public:
    virtual ~DiskImage() = default;

    virtual bool Open(const std::string& path) = 0;
    virtual uint64_t GetSize() const = 0;
    virtual bool Read(uint64_t offset, void* buf, uint32_t len) = 0;
    virtual bool Write(uint64_t offset, const void* buf, uint32_t len) = 0;
    virtual bool Flush() = 0;
    virtual bool Discard(uint64_t offset, uint64_t len) { (void)offset; (void)len; return true; }
    virtual bool WriteZeros(uint64_t offset, uint64_t len) = 0;

    using IoCallback = std::function<void(bool success)>;
    void ReadAsync(uint64_t offset, void* buf, uint32_t len, IoCallback cb);
    void WriteAsync(uint64_t offset, const void* buf, uint32_t len, IoCallback cb);
    void FlushAsync(IoCallback cb);
    void DiscardAsync(uint64_t offset, uint64_t len, IoCallback cb);
    void WriteZerosAsync(uint64_t offset, uint64_t len, IoCallback cb);

    // Submit an arbitrary task to the disk's worker thread.
    void SubmitTask(DiskWorker::Task task) { worker_.Submit(std::move(task)); }

    // Auto-detect format by reading magic bytes and return the right backend.
    static std::unique_ptr<DiskImage> Create(const std::string& path);

private:
    DiskWorker worker_;
};
