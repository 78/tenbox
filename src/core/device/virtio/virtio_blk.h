#pragma once

#include "core/device/virtio/virtio_mmio.h"
#include "core/disk/disk_image.h"
#include <mutex>
#include <string>
#include <memory>

// Feature bits
constexpr uint64_t VIRTIO_BLK_F_SIZE_MAX    = 1ULL << 1;
constexpr uint64_t VIRTIO_BLK_F_SEG_MAX     = 1ULL << 2;
constexpr uint64_t VIRTIO_BLK_F_BLK_SIZE    = 1ULL << 6;
constexpr uint64_t VIRTIO_BLK_F_FLUSH       = 1ULL << 9;
constexpr uint64_t VIRTIO_BLK_F_MQ          = 1ULL << 12;
constexpr uint64_t VIRTIO_BLK_F_DISCARD     = 1ULL << 13;
constexpr uint64_t VIRTIO_BLK_F_WRITE_ZEROES = 1ULL << 14;
#ifndef VIRTIO_F_VERSION_1_DEFINED
#define VIRTIO_F_VERSION_1_DEFINED
constexpr uint64_t VIRTIO_F_VERSION_1       = 1ULL << 32;
#endif

// Request types
constexpr uint32_t VIRTIO_BLK_T_IN          = 0;
constexpr uint32_t VIRTIO_BLK_T_OUT         = 1;
constexpr uint32_t VIRTIO_BLK_T_FLUSH       = 4;
constexpr uint32_t VIRTIO_BLK_T_GET_ID      = 8;
constexpr uint32_t VIRTIO_BLK_T_DISCARD     = 11;
constexpr uint32_t VIRTIO_BLK_T_WRITE_ZEROES = 13;

// Status codes
constexpr uint8_t VIRTIO_BLK_S_OK     = 0;
constexpr uint8_t VIRTIO_BLK_S_IOERR  = 1;
constexpr uint8_t VIRTIO_BLK_S_UNSUPP = 2;

#pragma pack(push, 1)
struct VirtioBlkReqHeader {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

struct VirtioBlkDiscardWriteZeroes {
    uint64_t sector;
    uint32_t num_sectors;
    uint32_t flags;
};

struct VirtioBlkConfig {
    uint64_t capacity;    // in 512-byte sectors
    uint32_t size_max;
    uint32_t seg_max;
    // geometry
    uint16_t cylinders;
    uint8_t  heads;
    uint8_t  sectors;
    uint32_t blk_size;
    // topology
    uint8_t  physical_block_exp;
    uint8_t  alignment_offset;
    uint16_t min_io_size;
    uint32_t opt_io_size;
    // writeback
    uint8_t  wce;
    uint8_t  unused0;
    // multi-queue
    uint16_t num_queues;
    // discard
    uint32_t max_discard_sectors;
    uint32_t max_discard_seg;
    uint32_t discard_sector_alignment;
    // write zeroes
    uint32_t max_write_zeroes_sectors;
    uint32_t max_write_zeroes_seg;
    uint8_t  write_zeroes_may_unmap;
    uint8_t  unused1[3];
};
#pragma pack(pop)

class VirtioBlkDevice : public VirtioDeviceOps {
public:
    ~VirtioBlkDevice() override = default;

    bool Open(const std::string& path);

    void SetMmioDevice(VirtioMmioDevice* mmio) { mmio_ = mmio; }

    uint32_t GetDeviceId() const override { return 2; }
    uint64_t GetDeviceFeatures() const override;
    uint32_t GetNumQueues() const override { return num_queues_; }
    uint32_t GetQueueMaxSize(uint32_t queue_idx) const override { return 256; }
    void OnQueueNotify(uint32_t queue_idx, VirtQueue& vq) override;
    void ReadConfig(uint32_t offset, uint8_t size, uint32_t* value) override;
    void WriteConfig(uint32_t offset, uint8_t size, uint32_t value) override;
    void OnStatusChange(uint32_t new_status) override;

private:
    void SubmitRequest(VirtQueue& vq, uint16_t head_idx, uint32_t queue_idx);

    VirtioMmioDevice* mmio_ = nullptr;
    std::unique_ptr<DiskImage> disk_;
    VirtioBlkConfig config_{};
    uint32_t num_queues_ = 4;
    bool is_qcow2_ = false;

    // Protects PushUsed + NotifyUsedBuffer from concurrent worker callbacks
    std::mutex completion_mutex_;
};
