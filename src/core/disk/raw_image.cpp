#include "core/disk/raw_image.h"

#ifndef _WIN32
#define _fseeki64 fseeko
#define _ftelli64 ftello
#endif

RawDiskImage::~RawDiskImage() {
    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }
}

bool RawDiskImage::Open(const std::string& path) {
    file_ = fopen(path.c_str(), "r+b");
    if (!file_) {
        LOG_ERROR("RawDiskImage: failed to open %s", path.c_str());
        return false;
    }

    if (!AcquireExclusiveLock(file_, path)) {
        fclose(file_);
        file_ = nullptr;
        return false;
    }

    _fseeki64(file_, 0, SEEK_END);
    disk_size_ = static_cast<uint64_t>(_ftelli64(file_));
    _fseeki64(file_, 0, SEEK_SET);

    if (disk_size_ < 512) {
        LOG_ERROR("RawDiskImage: image too small (%" PRIu64 " bytes)", disk_size_);
        fclose(file_);
        file_ = nullptr;
        return false;
    }

    LOG_INFO("RawDiskImage: %s, %" PRIu64 " bytes (%" PRIu64 " MB)",
             path.c_str(), disk_size_, disk_size_ / (1024 * 1024));
    return true;
}

bool RawDiskImage::Read(uint64_t offset, void* buf, uint32_t len) {
    if (offset + len > disk_size_) return false;
    if (_fseeki64(file_, offset, SEEK_SET) != 0) return false;
    return fread(buf, 1, len, file_) == len;
}

bool RawDiskImage::Write(uint64_t offset, const void* buf, uint32_t len) {
    if (offset + len > disk_size_) return false;
    if (_fseeki64(file_, offset, SEEK_SET) != 0) return false;
    return fwrite(buf, 1, len, file_) == len;
}

bool RawDiskImage::Flush() {
    return fflush(file_) == 0;
}

bool RawDiskImage::WriteZeros(uint64_t offset, uint64_t len) {
    if (offset + len > disk_size_) return false;
    if (_fseeki64(file_, offset, SEEK_SET) != 0) return false;

    static constexpr uint32_t kChunkSize = 4096;
    uint8_t zeros[kChunkSize] = {};
    while (len > 0) {
        uint32_t chunk = (len < kChunkSize) ? static_cast<uint32_t>(len) : kChunkSize;
        if (fwrite(zeros, 1, chunk, file_) != chunk) return false;
        len -= chunk;
    }
    return true;
}
