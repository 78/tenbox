#include "device/virtio/disk_image.h"
#include "device/virtio/raw_image.h"
#include "device/virtio/qcow2.h"
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
