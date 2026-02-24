#include "device/virtio/raw_image.h"

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

    _fseeki64(file_, 0, SEEK_END);
    disk_size_ = static_cast<uint64_t>(_ftelli64(file_));
    _fseeki64(file_, 0, SEEK_SET);

    if (disk_size_ < 512) {
        LOG_ERROR("RawDiskImage: image too small (%llu bytes)", disk_size_);
        fclose(file_);
        file_ = nullptr;
        return false;
    }

    LOG_INFO("RawDiskImage: %s, %llu bytes (%llu MB)",
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
