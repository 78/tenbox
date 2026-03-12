#include "core/disk/disk_image.h"
#include "core/disk/raw_image.h"
#include "core/disk/qcow2.h"
#include <cstdio>

static constexpr uint32_t kQcow2Magic = 0x514649FB;

std::unique_ptr<DiskImage> DiskImage::Create(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        LOG_ERROR("DiskImage::Create: cannot open %s", path.c_str());
        return nullptr;
    }

    uint32_t magic = 0;
    if (fread(&magic, 1, 4, f) != 4) {
        fclose(f);
        LOG_ERROR("DiskImage::Create: cannot read magic from %s", path.c_str());
        return nullptr;
    }
    fclose(f);

    // qcow2 magic is big-endian 0x514649FB
    uint32_t magic_be = (magic >> 24) | ((magic >> 8) & 0xFF00)
                      | ((magic << 8) & 0xFF0000) | (magic << 24);

    std::unique_ptr<DiskImage> img;
    if (magic_be == kQcow2Magic) {
        LOG_INFO("DiskImage: detected qcow2 format");
        img = std::make_unique<Qcow2DiskImage>();
    } else {
        LOG_INFO("DiskImage: detected raw format");
        img = std::make_unique<RawDiskImage>();
    }

    if (!img->Open(path)) return nullptr;
    return img;
}

void DiskImage::ReadAsync(uint64_t offset, void* buf, uint32_t len, IoCallback cb) {
    worker_.Submit([this, offset, buf, len, cb = std::move(cb)] {
        cb(Read(offset, buf, len));
    });
}

void DiskImage::WriteAsync(uint64_t offset, const void* buf, uint32_t len, IoCallback cb) {
    worker_.Submit([this, offset, buf, len, cb = std::move(cb)] {
        cb(Write(offset, buf, len));
    });
}

void DiskImage::FlushAsync(IoCallback cb) {
    worker_.Submit([this, cb = std::move(cb)] {
        cb(Flush());
    });
}

void DiskImage::DiscardAsync(uint64_t offset, uint64_t len, IoCallback cb) {
    worker_.Submit([this, offset, len, cb = std::move(cb)] {
        cb(Discard(offset, len));
    });
}

void DiskImage::WriteZerosAsync(uint64_t offset, uint64_t len, IoCallback cb) {
    worker_.Submit([this, offset, len, cb = std::move(cb)] {
        cb(WriteZeros(offset, len));
    });
}
