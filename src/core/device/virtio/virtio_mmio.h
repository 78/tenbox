#pragma once

#include "core/device/device.h"
#include "core/device/virtio/virtqueue.h"
#include <atomic>
#include <functional>
#include <vector>

// Abstract interface for virtio device-specific behavior.
class VirtioDeviceOps {
public:
    virtual ~VirtioDeviceOps() = default;
    virtual uint32_t GetDeviceId() const = 0;
    virtual uint64_t GetDeviceFeatures() const = 0;
    virtual uint32_t GetNumQueues() const = 0;
    virtual uint32_t GetQueueMaxSize(uint32_t queue_idx) const = 0;
    virtual void OnQueueNotify(uint32_t queue_idx, VirtQueue& vq) = 0;
    virtual void ReadConfig(uint32_t offset, uint8_t size, uint32_t* value) = 0;
    virtual void WriteConfig(uint32_t offset, uint8_t size, uint32_t value) = 0;
    virtual void OnStatusChange(uint32_t new_status) = 0;
};

// VirtIO MMIO transport device (spec v1.2, section 4.2).
// Register layout occupies 0x200 bytes at a fixed MMIO address.
class VirtioMmioDevice : public Device {
public:
    static constexpr uint64_t kMmioSize = 0x200;
    static constexpr uint32_t kMagic    = 0x74726976; // "virt"
    static constexpr uint32_t kVersion  = 2;
    static constexpr uint32_t kVendorId = 0x554D4551; // "QEMU" (conventional)

    using IrqCallback = std::function<void()>;

    void Init(VirtioDeviceOps* ops, const GuestMemMap& mem);
    void SetIrqCallback(IrqCallback cb) { irq_callback_ = std::move(cb); }

    void MmioRead(uint64_t offset, uint8_t size, uint64_t* value) override;
    void MmioWrite(uint64_t offset, uint8_t size, uint64_t value) override;

    // Called by the backend device to signal a used buffer notification.
    void NotifyUsedBuffer();

    // Called when device config changes (e.g. link status).
    // Sets VIRTIO_MMIO_INT_CONFIG (bit 1) and raises IRQ.
    void NotifyConfigChange();

    VirtQueue* GetQueue(uint32_t idx) {
        return idx < queues_.size() ? &queues_[idx] : nullptr;
    }

private:
    void DoReset();

    // MMIO register offsets (spec 4.2.2, Table 4.1)
    enum Reg : uint32_t {
        kMagicValue       = 0x000,
        kVersionReg       = 0x004,
        kDeviceID         = 0x008,
        kVendorID         = 0x00C,
        kDeviceFeatures   = 0x010,
        kDeviceFeaturesSel= 0x014,
        kDriverFeatures   = 0x020,
        kDriverFeaturesSel= 0x024,
        kQueueSel         = 0x030,
        kQueueNumMax      = 0x034,
        kQueueNum         = 0x038,
        kQueueReady       = 0x044,
        kQueueNotify      = 0x050,
        kInterruptStatus  = 0x060,
        kInterruptACK     = 0x064,
        kStatus           = 0x070,
        kQueueDescLow     = 0x080,
        kQueueDescHigh    = 0x084,
        kQueueDriverLow   = 0x090,
        kQueueDriverHigh  = 0x094,
        kQueueDeviceLow   = 0x0A0,
        kQueueDeviceHigh  = 0x0A4,
        kSHMSel           = 0x0AC,
        kSHMLenLow        = 0x0B0,
        kSHMLenHigh       = 0x0B4,
        kSHMBaseLow       = 0x0B8,
        kSHMBaseHigh      = 0x0BC,
        kConfigGeneration = 0x0FC,
        kConfig           = 0x100,
    };

    VirtioDeviceOps* ops_ = nullptr;
    GuestMemMap mem_;
    IrqCallback irq_callback_;

    // Transport state
    uint32_t status_ = 0;
    uint32_t device_features_sel_ = 0;
    uint32_t driver_features_sel_ = 0;
    uint64_t driver_features_ = 0;
    uint32_t queue_sel_ = 0;
    std::atomic<uint32_t> interrupt_status_{0};
    uint32_t config_generation_ = 0;

    std::vector<VirtQueue> queues_;

    // Per-queue staging for addresses being configured before QueueReady
    struct QueueConfig {
        uint32_t num = 0;
        uint64_t desc_addr = 0;
        uint64_t driver_addr = 0;
        uint64_t device_addr = 0;
    };
    std::vector<QueueConfig> queue_configs_;
    uint32_t shm_sel_ = 0;
};
