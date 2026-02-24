#pragma once

#include "vmm/types.h"
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

    // Auto-detect format by reading magic bytes and return the right backend.
    static std::unique_ptr<DiskImage> Create(const std::string& path);
};
