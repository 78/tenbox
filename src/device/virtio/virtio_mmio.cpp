#include "device/virtio/virtio_mmio.h"

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
    interrupt_status_ = 0;

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
        uint64_t features = ops_->GetDeviceFeatures();
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
        val = interrupt_status_;
        break;
    case kStatus:
        val = status_;
        break;
    case kConfigGeneration:
        val = config_generation_;
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
                LOG_INFO("VirtIO queue %u ready: size=%u desc=0x%llX "
                         "driver=0x%llX device=0x%llX",
                         queue_sel_, qs, cfg.desc_addr,
                         cfg.driver_addr, cfg.device_addr);
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
    case kInterruptACK:
        interrupt_status_ &= ~val;
        break;
    case kStatus:
        if (val == 0) {
            DoReset();
            ops_->OnStatusChange(0);
        } else {
            status_ = val;
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
    default:
        LOG_DEBUG("VirtIO MMIO: unhandled write offset=0x%03X val=0x%X",
                  (uint32_t)offset, val);
        break;
    }
}

void VirtioMmioDevice::NotifyUsedBuffer() {
    interrupt_status_ |= 1;
    if (irq_callback_) irq_callback_();
}
