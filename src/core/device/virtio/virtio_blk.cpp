#include "core/device/virtio/virtio_blk.h"
#include <cstring>
#include <algorithm>

bool VirtioBlkDevice::Open(const std::string& path) {
    disk_ = DiskImage::Create(path);
    if (!disk_) return false;

    is_qcow2_ = (path.size() >= 6 &&
                  path.compare(path.size() - 6, 6, ".qcow2") == 0);

    uint64_t disk_size = disk_->GetSize();

    memset(&config_, 0, sizeof(config_));
    config_.capacity  = disk_size / 512;
    config_.size_max  = 1u << 20;
    config_.seg_max   = 254;
    config_.blk_size  = 512;
    config_.num_queues = static_cast<uint16_t>(num_queues_);

    uint64_t total_sectors = disk_size / 512;
    uint32_t max_sectors = (total_sectors > UINT32_MAX) ? UINT32_MAX
                           : static_cast<uint32_t>(total_sectors);
    config_.max_discard_sectors       = max_sectors;
    config_.max_discard_seg           = 1;
    config_.discard_sector_alignment  = 1;
    config_.max_write_zeroes_sectors  = max_sectors;
    config_.max_write_zeroes_seg      = 1;
    config_.write_zeroes_may_unmap    = is_qcow2_ ? 1 : 0;

    LOG_INFO("VirtIO block: %s, %llu sectors (%llu MB), %u queues",
             path.c_str(), config_.capacity,
             disk_size / (1024 * 1024), num_queues_);
    return true;
}

uint64_t VirtioBlkDevice::GetDeviceFeatures() const {
    uint64_t features = VIRTIO_BLK_F_SIZE_MAX
                      | VIRTIO_BLK_F_SEG_MAX
                      | VIRTIO_BLK_F_BLK_SIZE
                      | VIRTIO_BLK_F_FLUSH
                      | VIRTIO_BLK_F_MQ
                      | VIRTIO_BLK_F_WRITE_ZEROES
                      | VIRTIO_F_VERSION_1;
    if (is_qcow2_)
        features |= VIRTIO_BLK_F_DISCARD;
    return features;
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
    if (queue_idx >= num_queues_) return;

    uint16_t head;
    while (vq.PopAvail(&head)) {
        SubmitRequest(vq, head, queue_idx);
    }
}

void VirtioBlkDevice::SubmitRequest(VirtQueue& vq, uint16_t head_idx, uint32_t queue_idx) {
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

    uint8_t* status_ptr = status_elem.addr;

    // Capture data segment pointers (between header and status descriptors).
    // These point into guest RAM which stays mapped for the VM lifetime,
    // so the pointers remain valid when the worker thread uses them.
    struct Segment { uint8_t* addr; uint32_t len; bool writable; };
    std::vector<Segment> segments;
    for (size_t i = 1; i + 1 < chain.size(); i++) {
        segments.push_back({chain[i].addr, chain[i].len, chain[i].writable});
    }

    // Submit the entire request to the disk worker thread.
    // The lambda captures everything needed; the vCPU thread returns immediately.
    disk_->SubmitTask([this, &vq, head_idx, queue_idx, status_ptr, hdr,
                           segments = std::move(segments)] {
        uint8_t status = VIRTIO_BLK_S_OK;
        uint32_t total_data_len = 0;

        switch (hdr.type) {
        case VIRTIO_BLK_T_IN: {
            uint64_t byte_offset = hdr.sector * 512;
            for (auto& seg : segments) {
                if (!seg.writable) continue;
                if (!disk_->Read(byte_offset, seg.addr, seg.len)) {
                    status = VIRTIO_BLK_S_IOERR;
                    break;
                }
                byte_offset += seg.len;
                total_data_len += seg.len;
            }
            break;
        }
        case VIRTIO_BLK_T_OUT: {
            uint64_t byte_offset = hdr.sector * 512;
            for (auto& seg : segments) {
                if (seg.writable) continue;
                if (!disk_->Write(byte_offset, seg.addr, seg.len)) {
                    status = VIRTIO_BLK_S_IOERR;
                    break;
                }
                byte_offset += seg.len;
                total_data_len += seg.len;
            }
            break;
        }
        case VIRTIO_BLK_T_FLUSH:
            if (!disk_->Flush())
                status = VIRTIO_BLK_S_IOERR;
            break;
        case VIRTIO_BLK_T_GET_ID: {
            const char* id_str = "tenbox-vblk";
            for (auto& seg : segments) {
                if (!seg.writable) continue;
                uint32_t copy_len = std::min(seg.len, 20u);
                memset(seg.addr, 0, seg.len);
                memcpy(seg.addr, id_str, std::min(copy_len,
                       static_cast<uint32_t>(strlen(id_str))));
                total_data_len += seg.len;
            }
            break;
        }
        case VIRTIO_BLK_T_DISCARD: {
            if (!segments.empty()) {
                auto& seg = segments[0];
                if (seg.len >= sizeof(VirtioBlkDiscardWriteZeroes)) {
                    VirtioBlkDiscardWriteZeroes dw;
                    memcpy(&dw, seg.addr, sizeof(dw));
                    uint64_t pos = dw.sector * 512;
                    uint64_t length = static_cast<uint64_t>(dw.num_sectors) * 512;
                    if (!disk_->Discard(pos, length))
                        status = VIRTIO_BLK_S_IOERR;
                } else {
                    status = VIRTIO_BLK_S_IOERR;
                }
            }
            break;
        }
        case VIRTIO_BLK_T_WRITE_ZEROES: {
            if (!segments.empty()) {
                auto& seg = segments[0];
                if (seg.len >= sizeof(VirtioBlkDiscardWriteZeroes)) {
                    VirtioBlkDiscardWriteZeroes dw;
                    memcpy(&dw, seg.addr, sizeof(dw));
                    uint64_t pos = dw.sector * 512;
                    uint64_t length = static_cast<uint64_t>(dw.num_sectors) * 512;
                    if (!disk_->WriteZeros(pos, length))
                        status = VIRTIO_BLK_S_IOERR;
                } else {
                    status = VIRTIO_BLK_S_IOERR;
                }
            }
            break;
        }
        default:
            LOG_WARN("VirtIO block: unsupported request type %u", hdr.type);
            status = VIRTIO_BLK_S_UNSUPP;
            break;
        }

        // Complete the request: write status, push to used ring, raise IRQ.
        // The mutex protects against concurrent completions from multiple queues.
        status_ptr[0] = status;
        std::lock_guard<std::mutex> lock(completion_mutex_);
        vq.PushUsed(head_idx, total_data_len + 1);
        if (mmio_) mmio_->NotifyUsedBuffer(queue_idx);
    });
}
