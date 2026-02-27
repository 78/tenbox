#include "core/device/virtio/virtio_serial.h"
#include "core/vmm/types.h"
#include <cstring>
#include <algorithm>

VirtioSerialDevice::VirtioSerialDevice(uint32_t max_ports)
    : max_ports_(max_ports) {
    std::memset(&config_, 0, sizeof(config_));
    config_.max_nr_ports = max_ports;
    ports_.resize(max_ports);
}

uint64_t VirtioSerialDevice::GetDeviceFeatures() const {
    return VIRTIO_CONSOLE_F_MULTIPORT | VIRTIO_SERIAL_F_VERSION_1;
}

uint32_t VirtioSerialDevice::GetNumQueues() const {
    // Queue layout for multiport:
    // 0: receiveq (port 0)
    // 1: transmitq (port 0)
    // 2: control receiveq
    // 3: control transmitq
    // For additional ports: 4+port*2 = receiveq, 5+port*2 = transmitq
    return 4 + (max_ports_ > 1 ? (max_ports_ - 1) * 2 : 0);
}

void VirtioSerialDevice::SetPortName(uint32_t port_id, const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (port_id < ports_.size()) {
        ports_[port_id].name = name;
    }
}

void VirtioSerialDevice::ReadConfig(uint32_t offset, uint8_t size, uint32_t* value) {
    const auto* cfg = reinterpret_cast<const uint8_t*>(&config_);
    if (offset + size > sizeof(config_)) {
        *value = 0;
        return;
    }
    *value = 0;
    std::memcpy(value, cfg + offset, size);
}

void VirtioSerialDevice::WriteConfig(uint32_t offset, uint8_t size, uint32_t value) {
    // Config is read-only for console device
}

void VirtioSerialDevice::OnStatusChange(uint32_t new_status) {
    if ((new_status & 0x4) && !driver_ready_) {
        driver_ready_ = true;
        LOG_INFO("VirtIO Serial: driver ready");
    }
}

void VirtioSerialDevice::OnQueueNotify(uint32_t queue_idx, VirtQueue& vq) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (queue_idx == 2) {
        // Control receiveq - guest provides buffers for us to fill
        return;
    } else if (queue_idx == 3) {
        // Control transmitq - guest sends control messages
        HandleControlMessage(vq);
    } else if (queue_idx == 1) {
        // Port 0 transmitq
        HandlePortTx(0, vq);
    } else if (queue_idx >= 5 && ((queue_idx - 5) % 2 == 0)) {
        // Additional port transmitq
        uint32_t port_id = 1 + (queue_idx - 5) / 2;
        if (port_id < max_ports_) {
            HandlePortTx(port_id, vq);
        }
    }
    // Receiveq notifications (0, 4, 6, ...) are for guest providing buffers
}

void VirtioSerialDevice::HandleControlMessage(VirtQueue& vq) {
    uint16_t head;
    while (vq.PopAvail(&head)) {
        std::vector<VirtqChainElem> chain;
        if (!vq.WalkChain(head, &chain)) {
            vq.PushUsed(head, 0);
            continue;
        }

        VirtioConsoleControl ctrl{};
        uint32_t read = 0;
        for (const auto& elem : chain) {
            if (elem.writable) continue;
            uint32_t to_copy = std::min(elem.len, static_cast<uint32_t>(sizeof(ctrl) - read));
            std::memcpy(reinterpret_cast<uint8_t*>(&ctrl) + read, elem.addr, to_copy);
            read += to_copy;
            if (read >= sizeof(ctrl)) break;
        }

        vq.PushUsed(head, 0);

        if (read < sizeof(ctrl)) continue;

        LOG_DEBUG("VirtIO Serial control: port=%u event=%u value=%u",
                  ctrl.id, ctrl.event, ctrl.value);

        switch (ctrl.event) {
        case VIRTIO_CONSOLE_DEVICE_READY:
            if (ctrl.value == 1) {
                for (uint32_t i = 0; i < max_ports_; ++i) {
                    SendControlMessage(i, VIRTIO_CONSOLE_DEVICE_ADD, 1);
                }
            }
            break;

        case VIRTIO_CONSOLE_PORT_READY:
            if (ctrl.id < ports_.size() && ctrl.value == 1) {
                if (!ports_[ctrl.id].name.empty()) {
                    SendPortName(ctrl.id);
                }
                SendControlMessage(ctrl.id, VIRTIO_CONSOLE_PORT_OPEN, 1);
            }
            break;

        case VIRTIO_CONSOLE_PORT_OPEN:
            if (ctrl.id < ports_.size()) {
                ports_[ctrl.id].guest_connected = (ctrl.value == 1);
                LOG_INFO("VirtIO Serial port %u: guest %s",
                         ctrl.id, ctrl.value ? "opened" : "closed");
            }
            break;

        default:
            break;
        }
    }

    if (mmio_) {
        mmio_->NotifyUsedBuffer();
    }
}

void VirtioSerialDevice::HandlePortTx(uint32_t port_id, VirtQueue& vq) {
    uint16_t head;
    while (vq.PopAvail(&head)) {
        std::vector<VirtqChainElem> chain;
        if (!vq.WalkChain(head, &chain)) {
            vq.PushUsed(head, 0);
            continue;
        }

        // Collect data from chain
        std::vector<uint8_t> data;
        for (const auto& elem : chain) {
            if (elem.writable) continue;
            data.insert(data.end(), elem.addr, elem.addr + elem.len);
        }

        vq.PushUsed(head, 0);

        if (!data.empty() && data_callback_) {
            data_callback_(port_id, data.data(), data.size());
        }
    }

    if (mmio_) {
        mmio_->NotifyUsedBuffer();
    }
}

void VirtioSerialDevice::SendControlMessage(uint32_t port_id, uint16_t event, uint16_t value) {
    if (!mmio_) return;

    VirtQueue* vq = mmio_->GetQueue(2);  // Control receiveq
    if (!vq || !vq->IsReady()) return;

    uint16_t head;
    if (!vq->PopAvail(&head)) {
        LOG_DEBUG("VirtIO Serial: no buffers for control message");
        return;
    }

    std::vector<VirtqChainElem> chain;
    if (!vq->WalkChain(head, &chain)) {
        vq->PushUsed(head, 0);
        mmio_->NotifyUsedBuffer();
        return;
    }

    VirtioConsoleControl ctrl{};
    ctrl.id = port_id;
    ctrl.event = event;
    ctrl.value = value;

    uint32_t written = 0;
    for (auto& elem : chain) {
        if (!elem.writable) continue;
        uint32_t to_copy = std::min(elem.len, static_cast<uint32_t>(sizeof(ctrl) - written));
        std::memcpy(elem.addr, reinterpret_cast<const uint8_t*>(&ctrl) + written, to_copy);
        written += to_copy;
        if (written >= sizeof(ctrl)) break;
    }

    vq->PushUsed(head, written);
    mmio_->NotifyUsedBuffer();

    LOG_DEBUG("VirtIO Serial: sent control port=%u event=%u value=%u",
              port_id, event, value);
}

void VirtioSerialDevice::SendPortName(uint32_t port_id) {
    if (!mmio_ || port_id >= ports_.size()) return;
    const std::string& name = ports_[port_id].name;
    if (name.empty()) return;

    VirtQueue* vq = mmio_->GetQueue(2);  // Control receiveq
    if (!vq || !vq->IsReady()) return;

    uint16_t head;
    if (!vq->PopAvail(&head)) return;

    std::vector<VirtqChainElem> chain;
    if (!vq->WalkChain(head, &chain)) {
        vq->PushUsed(head, 0);
        mmio_->NotifyUsedBuffer();
        return;
    }

    VirtioConsoleControl ctrl{};
    ctrl.id = port_id;
    ctrl.event = VIRTIO_CONSOLE_PORT_NAME;
    ctrl.value = 1;

    std::vector<uint8_t> msg(sizeof(ctrl) + name.size());
    std::memcpy(msg.data(), &ctrl, sizeof(ctrl));
    std::memcpy(msg.data() + sizeof(ctrl), name.data(), name.size());

    uint32_t written = 0;
    for (auto& elem : chain) {
        if (!elem.writable) continue;
        uint32_t to_copy = std::min(elem.len, static_cast<uint32_t>(msg.size() - written));
        std::memcpy(elem.addr, msg.data() + written, to_copy);
        written += to_copy;
        if (written >= msg.size()) break;
    }

    vq->PushUsed(head, written);
    mmio_->NotifyUsedBuffer();

    LOG_INFO("VirtIO Serial: sent port name '%s' for port %u", name.c_str(), port_id);
}

bool VirtioSerialDevice::SendData(uint32_t port_id, const uint8_t* data, size_t len) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!mmio_ || port_id >= ports_.size() || !data || len == 0) {
        return false;
    }

    if (!ports_[port_id].guest_connected) {
        LOG_DEBUG("VirtIO Serial: port %u not connected, dropping data", port_id);
        return false;
    }

    // Determine receive queue index for this port
    uint32_t rx_queue = (port_id == 0) ? 0 : (4 + (port_id - 1) * 2);

    VirtQueue* vq = mmio_->GetQueue(rx_queue);
    if (!vq || !vq->IsReady()) {
        return false;
    }

    size_t offset = 0;
    while (offset < len) {
        uint16_t head;
        if (!vq->PopAvail(&head)) {
            LOG_DEBUG("VirtIO Serial: no buffers for port %u data", port_id);
            break;
        }

        std::vector<VirtqChainElem> chain;
        if (!vq->WalkChain(head, &chain)) {
            vq->PushUsed(head, 0);
            continue;
        }

        uint32_t written = 0;
        for (auto& elem : chain) {
            if (!elem.writable) continue;
            uint32_t to_copy = std::min(elem.len, static_cast<uint32_t>(len - offset));
            std::memcpy(elem.addr, data + offset, to_copy);
            written += to_copy;
            offset += to_copy;
            if (offset >= len) break;
        }

        vq->PushUsed(head, written);
    }

    mmio_->NotifyUsedBuffer();
    return offset == len;
}
