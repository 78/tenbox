#include "device/virtio/virtio_blk.h"
#include <cstring>
#include <algorithm>

bool VirtioBlkDevice::Open(const std::string& path) {
    disk_ = DiskImage::Create(path);
    if (!disk_) return false;

    uint64_t disk_size = disk_->GetSize();

    memset(&config_, 0, sizeof(config_));
    config_.capacity = disk_size / 512;
    config_.size_max = 1u << 20;
    config_.seg_max  = 126;
    config_.blk_size = 512;

    LOG_INFO("VirtIO block: %s, %llu sectors (%llu MB)",
             path.c_str(), config_.capacity,
             disk_size / (1024 * 1024));
    return true;
}

uint64_t VirtioBlkDevice::GetDeviceFeatures() const {
    return VIRTIO_BLK_F_SIZE_MAX
         | VIRTIO_BLK_F_SEG_MAX
         | VIRTIO_BLK_F_BLK_SIZE
         | VIRTIO_BLK_F_FLUSH
         | VIRTIO_F_VERSION_1;
}

void VirtioBlkDevice::ReadConfig(uint32_t offset, uint8_t size,
                                  uint32_t* value) {
    const auto* cfg = reinterpret_cast<const uint8_t*>(&config_);
    uint32_t cfg_size = sizeof(config_);

    if (offset >= cfg_size) {
        *value = 0;
        return;
    }

    uint32_t avail = cfg_size - offset;
    uint32_t read_size = std::min(static_cast<uint32_t>(size), avail);
    *value = 0;
    memcpy(value, cfg + offset, read_size);
}

void VirtioBlkDevice::WriteConfig(uint32_t, uint8_t, uint32_t) {
}

void VirtioBlkDevice::OnStatusChange(uint32_t new_status) {
    if (new_status == 0) {
        LOG_INFO("VirtIO block: device reset");
    }
}

void VirtioBlkDevice::OnQueueNotify(uint32_t queue_idx, VirtQueue& vq) {
    if (queue_idx != 0) return;

    uint16_t head;
    while (vq.PopAvail(&head)) {
        ProcessRequest(vq, head);
    }

    if (mmio_) mmio_->NotifyUsedBuffer();
}

void VirtioBlkDevice::ProcessRequest(VirtQueue& vq, uint16_t head_idx) {
    std::vector<VirtqChainElem> chain;
    if (!vq.WalkChain(head_idx, &chain)) {
        LOG_ERROR("VirtIO block: failed to walk descriptor chain");
        return;
    }

    if (chain.size() < 2) {
        LOG_ERROR("VirtIO block: chain too short (%zu)", chain.size());
        return;
    }

    auto& hdr_elem = chain[0];
    if (hdr_elem.len < sizeof(VirtioBlkReqHeader)) {
        LOG_ERROR("VirtIO block: header too small (%u)", hdr_elem.len);
        return;
    }

    VirtioBlkReqHeader hdr;
    memcpy(&hdr, hdr_elem.addr, sizeof(hdr));

    auto& status_elem = chain.back();
    if (!status_elem.writable || status_elem.len < 1) {
        LOG_ERROR("VirtIO block: missing writable status descriptor");
        return;
    }

    uint8_t status = VIRTIO_BLK_S_OK;
    uint32_t total_data_len = 0;

    switch (hdr.type) {
    case VIRTIO_BLK_T_IN: {
        uint64_t byte_offset = hdr.sector * 512;

        for (size_t i = 1; i + 1 < chain.size(); i++) {
            auto& elem = chain[i];
            if (!elem.writable) continue;

            if (!disk_->Read(byte_offset, elem.addr, elem.len)) {
                status = VIRTIO_BLK_S_IOERR;
                break;
            }
            byte_offset += elem.len;
            total_data_len += elem.len;
        }
        break;
    }
    case VIRTIO_BLK_T_OUT: {
        uint64_t byte_offset = hdr.sector * 512;

        for (size_t i = 1; i + 1 < chain.size(); i++) {
            auto& elem = chain[i];
            if (elem.writable) continue;

            if (!disk_->Write(byte_offset, elem.addr, elem.len)) {
                status = VIRTIO_BLK_S_IOERR;
                break;
            }
            byte_offset += elem.len;
            total_data_len += elem.len;
        }
        break;
    }
    case VIRTIO_BLK_T_FLUSH:
        disk_->Flush();
        break;
    case VIRTIO_BLK_T_GET_ID: {
        const char* id_str = "tenclaw-vblk";
        for (size_t i = 1; i + 1 < chain.size(); i++) {
            auto& elem = chain[i];
            if (!elem.writable) continue;
            uint32_t copy_len = std::min(elem.len, 20u);
            memset(elem.addr, 0, elem.len);
            memcpy(elem.addr, id_str, std::min(copy_len,
                   static_cast<uint32_t>(strlen(id_str))));
            total_data_len += elem.len;
        }
        break;
    }
    default:
        LOG_WARN("VirtIO block: unsupported request type %u", hdr.type);
        status = VIRTIO_BLK_S_UNSUPP;
        break;
    }

    status_elem.addr[0] = status;
    vq.PushUsed(head_idx, total_data_len + 1);
}
