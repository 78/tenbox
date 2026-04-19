#include "core/device/virtio/virtio_mmio.h"

static constexpr uint64_t VIRTIO_RING_F_INDIRECT_DESC = (1ULL << 28);
static constexpr uint64_t VIRTIO_F_EVENT_IDX = (1ULL << 29);

void VirtioMmioDevice::Init(VirtioDeviceOps* ops, const GuestMemMap& mem) {
    ops_ = ops;
    mem_ = mem;

    uint32_t num_queues = ops_->GetNumQueues();
    queues_.resize(num_queues);
    queue_configs_.resize(num_queues);

    for (uint32_t i = 0; i < num_queues; i++) {
        queues_[i].Setup(ops_->GetQueueMaxSize(i), mem_);
    }
}

void VirtioMmioDevice::DoReset() {
    status_ = 0;
    device_features_sel_ = 0;
    driver_features_sel_ = 0;
    driver_features_ = 0;
    queue_sel_ = 0;
    shm_sel_ = 0;
    interrupt_status_.store(0, std::memory_order_relaxed);

    for (uint32_t i = 0; i < queues_.size(); i++) {
        queues_[i].Reset();
        queue_configs_[i] = {};
    }
}

void VirtioMmioDevice::MmioRead(uint64_t offset, uint8_t size,
                                  uint64_t* value) {
    uint32_t val = 0;

    if (offset >= kConfig) {
        ops_->ReadConfig(static_cast<uint32_t>(offset - kConfig), size, &val);
        *value = val;
        return;
    }

    switch (static_cast<uint32_t>(offset)) {
    case kMagicValue:
        val = kMagic;
        break;
    case kVersionReg:
        val = kVersion;
        break;
    case kDeviceID:
        val = ops_->GetDeviceId();
        break;
    case kVendorID:
        val = kVendorId;
        break;
    case kDeviceFeatures: {
        uint64_t features = ops_->GetDeviceFeatures() | VIRTIO_RING_F_INDIRECT_DESC | VIRTIO_F_EVENT_IDX;
        val = static_cast<uint32_t>(features >> (device_features_sel_ * 32));
        break;
    }
    case kQueueNumMax:
        if (queue_sel_ < queues_.size())
            val = ops_->GetQueueMaxSize(queue_sel_);
        break;
    case kQueueReady:
        if (queue_sel_ < queues_.size())
            val = queues_[queue_sel_].IsReady() ? 1 : 0;
        break;
    case kInterruptStatus:
        val = interrupt_status_.load(std::memory_order_acquire);
        break;
    case kStatus:
        val = status_;
        break;
    case kConfigGeneration:
        val = config_generation_;
        break;
    case kSHMLenLow:
    case kSHMLenHigh:
    case kSHMBaseLow:
    case kSHMBaseHigh:
        // No shared memory regions supported — return all-ones so the
        // kernel's virtio_get_shm_region() interprets this as "not present".
        val = 0xFFFFFFFF;
        break;
    default:
        LOG_DEBUG("VirtIO MMIO: unhandled read offset=0x%03X", (uint32_t)offset);
        break;
    }

    *value = val;
}

void VirtioMmioDevice::MmioWrite(uint64_t offset, uint8_t size,
                                   uint64_t value) {
    uint32_t val = static_cast<uint32_t>(value);

    if (offset >= kConfig) {
        ops_->WriteConfig(static_cast<uint32_t>(offset - kConfig), size, val);
        return;
    }

    switch (static_cast<uint32_t>(offset)) {
    case kDeviceFeaturesSel:
        device_features_sel_ = val;
        break;
    case kDriverFeatures:
        if (driver_features_sel_ == 0)
            driver_features_ = (driver_features_ & 0xFFFFFFFF00000000ULL) | val;
        else if (driver_features_sel_ == 1)
            driver_features_ = (driver_features_ & 0x00000000FFFFFFFFULL) |
                               (static_cast<uint64_t>(val) << 32);
        break;
    case kDriverFeaturesSel:
        driver_features_sel_ = val;
        break;
    case kQueueSel:
        queue_sel_ = val;
        break;
    case kQueueNum:
        if (queue_sel_ < queue_configs_.size())
            queue_configs_[queue_sel_].num = val;
        break;
    case kQueueReady:
        if (queue_sel_ < queues_.size()) {
            if (val == 1) {
                auto& cfg = queue_configs_[queue_sel_];
                auto& vq = queues_[queue_sel_];
                uint32_t qs = cfg.num ? cfg.num
                                      : ops_->GetQueueMaxSize(queue_sel_);
                vq.Setup(qs, mem_);
                vq.SetDescAddr(cfg.desc_addr);
                vq.SetDriverAddr(cfg.driver_addr);
                vq.SetDeviceAddr(cfg.device_addr);
                vq.SetReady(true);
                LOG_DEBUG("VirtIO queue %u ready: size=%u desc=0x%" PRIX64
                         " driver=0x%" PRIX64 " device=0x%" PRIX64,
                         queue_sel_, qs, cfg.desc_addr, cfg.driver_addr, cfg.device_addr);
            } else {
                queues_[queue_sel_].SetReady(false);
            }
        }
        break;
    case kQueueNotify:
        if (val < queues_.size() && queues_[val].IsReady()) {
            ops_->OnQueueNotify(val, queues_[val]);
        }
        break;
    case kInterruptACK: {
        uint32_t prev = interrupt_status_.fetch_and(~val, std::memory_order_acq_rel);
        if ((prev & ~val) == 0 && irq_level_callback_) {
            irq_level_callback_(false);
        }
        break;
    }
    case kStatus:
        if (val == 0) {
            DoReset();
            ops_->OnStatusChange(0);
        } else {
            uint32_t prev = status_;
            status_ = val;
            // When FEATURES_OK (bit 3) is first set, feature negotiation is done.
            // Propagate EVENT_IDX to all queues.
            if ((val & 0x8) && !(prev & 0x8)) {
                bool event_idx = (driver_features_ & VIRTIO_F_EVENT_IDX) != 0;
                for (auto& vq : queues_)
                    vq.SetEventIdx(event_idx);
            }
            ops_->OnStatusChange(val);
        }
        break;
    case kQueueDescLow:
        if (queue_sel_ < queue_configs_.size())
            queue_configs_[queue_sel_].desc_addr =
                (queue_configs_[queue_sel_].desc_addr & 0xFFFFFFFF00000000ULL) | val;
        break;
    case kQueueDescHigh:
        if (queue_sel_ < queue_configs_.size())
            queue_configs_[queue_sel_].desc_addr =
                (queue_configs_[queue_sel_].desc_addr & 0x00000000FFFFFFFFULL) |
                (static_cast<uint64_t>(val) << 32);
        break;
    case kQueueDriverLow:
        if (queue_sel_ < queue_configs_.size())
            queue_configs_[queue_sel_].driver_addr =
                (queue_configs_[queue_sel_].driver_addr & 0xFFFFFFFF00000000ULL) | val;
        break;
    case kQueueDriverHigh:
        if (queue_sel_ < queue_configs_.size())
            queue_configs_[queue_sel_].driver_addr =
                (queue_configs_[queue_sel_].driver_addr & 0x00000000FFFFFFFFULL) |
                (static_cast<uint64_t>(val) << 32);
        break;
    case kQueueDeviceLow:
        if (queue_sel_ < queue_configs_.size())
            queue_configs_[queue_sel_].device_addr =
                (queue_configs_[queue_sel_].device_addr & 0xFFFFFFFF00000000ULL) | val;
        break;
    case kQueueDeviceHigh:
        if (queue_sel_ < queue_configs_.size())
            queue_configs_[queue_sel_].device_addr =
                (queue_configs_[queue_sel_].device_addr & 0x00000000FFFFFFFFULL) |
                (static_cast<uint64_t>(val) << 32);
        break;
    case kSHMSel:
        shm_sel_ = val;
        break;
    default:
        LOG_DEBUG("VirtIO MMIO: unhandled write offset=0x%03X val=0x%X",
                  (uint32_t)offset, val);
        break;
    }
}

void VirtioMmioDevice::NotifyUsedBuffer(int queue_idx) {
    if (queue_idx >= 0 && queue_idx < static_cast<int>(queues_.size())) {
        if (!queues_[queue_idx].ShouldNotifyGuest())
            return;
    }

    interrupt_status_.fetch_or(1, std::memory_order_release);  // VIRTIO_MMIO_INT_VRING
    if (irq_level_callback_)
        irq_level_callback_(true);
    else if (irq_callback_)
        irq_callback_();
}

void VirtioMmioDevice::NotifyConfigChange() {
    config_generation_++;
    interrupt_status_.fetch_or(2, std::memory_order_release);  // VIRTIO_MMIO_INT_CONFIG
    if (irq_level_callback_)
        irq_level_callback_(true);
    else if (irq_callback_)
        irq_callback_();
}
