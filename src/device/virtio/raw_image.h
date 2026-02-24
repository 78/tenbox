#pragma once

#include "device/virtio/disk_image.h"
#include <cstdio>

class RawDiskImage : public DiskImage {
public:
    ~RawDiskImage() override;

    bool Open(const std::string& path) override;
    uint64_t GetSize() const override { return disk_size_; }
    bool Read(uint64_t offset, void* buf, uint32_t len) override;
    bool Write(uint64_t offset, const void* buf, uint32_t len) override;
    bool Flush() override;

private:
    FILE* file_ = nullptr;
    uint64_t disk_size_ = 0;
};
