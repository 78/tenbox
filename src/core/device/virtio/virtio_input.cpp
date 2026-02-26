#include "core/device/virtio/virtio_input.h"
#include "core/vmm/types.h"
#include <cstring>

#ifndef VIRTIO_F_VERSION_1_DEFINED
#define VIRTIO_F_VERSION_1_DEFINED
constexpr uint64_t VIRTIO_INPUT_F_VERSION_1 = 1ULL << 32;
#else
static constexpr uint64_t VIRTIO_INPUT_F_VERSION_1 = 1ULL << 32;
#endif

static void SetBit(uint8_t* bitmap, uint16_t bit) {
    bitmap[bit / 8] |= 1u << (bit % 8);
}

VirtioInputDevice::VirtioInputDevice(SubType type) : sub_type_(type) {
    std::memset(&config_, 0, sizeof(config_));
}

uint64_t VirtioInputDevice::GetDeviceFeatures() const {
    return VIRTIO_INPUT_F_VERSION_1;
}

void VirtioInputDevice::UpdateConfigData() {
    std::memset(config_.data, 0, sizeof(config_.data));
    config_.size = 0;

    switch (config_.select) {
    case VIRTIO_INPUT_CFG_ID_NAME: {
        const char* name = (sub_type_ == SubType::kKeyboard)
                               ? "virtio-keyboard"
                               : "virtio-tablet";
        size_t len = std::strlen(name);
        if (len > sizeof(config_.data)) len = sizeof(config_.data);
        std::memcpy(config_.data, name, len);
        config_.size = static_cast<uint8_t>(len);
        break;
    }
    case VIRTIO_INPUT_CFG_ID_SERIAL: {
        const char* serial = "tenbox-0";
        size_t len = std::strlen(serial);
        std::memcpy(config_.data, serial, len);
        config_.size = static_cast<uint8_t>(len);
        break;
    }
    case VIRTIO_INPUT_CFG_ID_DEVIDS: {
        VirtioInputDevIds ids{};
        ids.bustype = 0x06; // BUS_VIRTUAL
        ids.vendor = 0x0001;
        ids.product = (sub_type_ == SubType::kKeyboard) ? 0x0001u : 0x0002u;
        ids.version = 0x0001;
        std::memcpy(config_.data, &ids, sizeof(ids));
        config_.size = static_cast<uint8_t>(sizeof(ids));
        break;
    }
    case VIRTIO_INPUT_CFG_PROP_BITS:
        config_.size = 0;
        break;
    case VIRTIO_INPUT_CFG_EV_BITS: {
        if (sub_type_ == SubType::kKeyboard) {
            if (config_.subsel == EV_KEY) {
                // Report keys KEY_ESC(1) through KEY_COMPOSE(127)
                for (uint16_t k = 1; k <= 127; ++k) SetBit(config_.data, k);
                // Also report KEY_UP(103)..KEY_RIGHT(106) range (already in 1..127)
                config_.size = 16; // 128 bits = 16 bytes
            } else if (config_.subsel == EV_SYN) {
                SetBit(config_.data, SYN_REPORT);
                config_.size = 1;
            } else {
                config_.size = 0;
            }
        } else {
            // Tablet
            if (config_.subsel == EV_ABS) {
                SetBit(config_.data, ABS_X);
                SetBit(config_.data, ABS_Y);
                config_.size = 1;
            } else if (config_.subsel == EV_KEY) {
                SetBit(config_.data, BTN_LEFT & 0xFF);
                SetBit(config_.data, BTN_RIGHT & 0xFF);
                SetBit(config_.data, BTN_MIDDLE & 0xFF);
                // BTN_LEFT=0x110, need at least ceil(0x113/8)=35 bytes
                // Re-do with full codes:
                std::memset(config_.data, 0, sizeof(config_.data));
                SetBit(config_.data, BTN_LEFT);
                SetBit(config_.data, BTN_RIGHT);
                SetBit(config_.data, BTN_MIDDLE);
                SetBit(config_.data, BTN_TOUCH);
                config_.size = static_cast<uint8_t>((BTN_TOUCH / 8) + 1);
            } else if (config_.subsel == EV_SYN) {
                SetBit(config_.data, SYN_REPORT);
                config_.size = 1;
            } else {
                config_.size = 0;
            }
        }
        break;
    }
    case VIRTIO_INPUT_CFG_ABS_INFO: {
        if (sub_type_ == SubType::kTablet &&
            (config_.subsel == ABS_X || config_.subsel == ABS_Y)) {
            VirtioInputAbsInfo info{};
            info.min = 0;
            info.max = 32767;
            info.fuzz = 0;
            info.flat = 0;
            info.res = 0;
            std::memcpy(config_.data, &info, sizeof(info));
            config_.size = static_cast<uint8_t>(sizeof(info));
        } else {
            config_.size = 0;
        }
        break;
    }
    default:
        config_.size = 0;
        break;
    }
}

void VirtioInputDevice::ReadConfig(uint32_t offset, uint8_t size, uint32_t* value) {
    const auto* cfg = reinterpret_cast<const uint8_t*>(&config_);
    if (offset + size > sizeof(config_)) {
        *value = 0;
        return;
    }
    *value = 0;
    std::memcpy(value, cfg + offset, size);
}

void VirtioInputDevice::WriteConfig(uint32_t offset, uint8_t size, uint32_t value) {
    auto* cfg = reinterpret_cast<uint8_t*>(&config_);
    if (offset + size > sizeof(config_)) return;

    // Only select (offset 0) and subsel (offset 1) are writable
    if (offset <= 1 && offset + size <= 2) {
        std::memcpy(cfg + offset, &value, size);
        UpdateConfigData();
    }
}

void VirtioInputDevice::OnStatusChange(uint32_t new_status) {
    // Nothing to do
}

void VirtioInputDevice::OnQueueNotify(uint32_t queue_idx, VirtQueue& vq) {
    if (queue_idx == 1) {
        // Status queue - just consume and discard
        uint16_t head;
        while (vq.PopAvail(&head)) {
            vq.PushUsed(head, 0);
        }
    }
    // Queue 0 (eventq): guest provides empty buffers for us to fill.
    // We don't consume them here; InjectEvent will use them.
}

void VirtioInputDevice::InjectEvent(uint16_t type, uint16_t code, uint32_t value) {
    std::lock_guard<std::mutex> lock(inject_mutex_);
    if (!mmio_) return;

    VirtQueue* vq = mmio_->GetQueue(0);
    if (!vq || !vq->IsReady()) return;

    uint16_t head;
    if (!vq->PopAvail(&head)) return;

    std::vector<VirtqChainElem> chain;
    if (!vq->WalkChain(head, &chain)) return;

    VirtioInputEvent ev{type, code, value};
    uint32_t written = 0;
    for (auto& elem : chain) {
        if (!elem.writable) continue;
        uint32_t to_copy = (std::min)(elem.len, static_cast<uint32_t>(sizeof(ev) - written));
        std::memcpy(elem.addr, reinterpret_cast<const uint8_t*>(&ev) + written, to_copy);
        written += to_copy;
        if (written >= sizeof(ev)) break;
    }

    vq->PushUsed(head, written);
    mmio_->NotifyUsedBuffer();
}
