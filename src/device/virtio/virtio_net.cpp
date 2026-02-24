#include "device/virtio/virtio_net.h"
#include <cstring>
#include <algorithm>

static constexpr uint8_t kDefaultMac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

VirtioNetDevice::VirtioNetDevice() {
    memcpy(config_.mac, kDefaultMac, 6);
    config_.status = 1; // VIRTIO_NET_S_LINK_UP
}

uint64_t VirtioNetDevice::GetDeviceFeatures() const {
    return VIRTIO_NET_F_MAC | VIRTIO_NET_F_VERSION_1;
}

void VirtioNetDevice::OnQueueNotify(uint32_t queue_idx, VirtQueue& vq) {
    if (queue_idx == 0) {
        // RX queue: guest provided new receive buffers — nothing to do now,
        // packets are injected via InjectRx() from the network thread.
        return;
    }

    if (queue_idx != 1) return;

    // TX queue: guest wants to send packets
    uint16_t head;
    while (vq.PopAvail(&head)) {
        std::vector<VirtqChainElem> chain;
        if (!vq.WalkChain(head, &chain) || chain.empty()) {
            vq.PushUsed(head, 0);
            continue;
        }

        // Gather the full TX buffer (virtio_net_hdr + Ethernet frame)
        uint32_t total = 0;
        for (auto& e : chain) total += e.len;

        if (total <= sizeof(VirtioNetHdr)) {
            vq.PushUsed(head, 0);
            continue;
        }

        // Linearize into a contiguous buffer
        std::vector<uint8_t> buf(total);
        uint32_t off = 0;
        for (auto& e : chain) {
            memcpy(buf.data() + off, e.addr, e.len);
            off += e.len;
        }

        // Skip the virtio_net_hdr, pass raw Ethernet frame to backend
        const uint8_t* frame = buf.data() + sizeof(VirtioNetHdr);
        uint32_t frame_len = total - sizeof(VirtioNetHdr);

        if (tx_callback_ && frame_len >= 14) {
            tx_callback_(frame, frame_len);
        }

        vq.PushUsed(head, 0);
    }
    mmio_->NotifyUsedBuffer();
}

bool VirtioNetDevice::InjectRx(const uint8_t* frame, uint32_t len) {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    if (!mmio_) return false;

    VirtQueue* vq = mmio_->GetQueue(0);
    if (!vq || !vq->IsReady()) return false;

    uint16_t head;
    if (!vq->PopAvail(&head)) return false;

    std::vector<VirtqChainElem> chain;
    if (!vq->WalkChain(head, &chain) || chain.empty()) {
        vq->PushUsed(head, 0);
        return false;
    }

    VirtioNetHdr hdr{};
    uint32_t total = sizeof(hdr) + len;
    uint32_t written = 0;

    for (auto& elem : chain) {
        if (!elem.writable || written >= total) continue;
        uint32_t to_copy = std::min(elem.len, total - written);
        for (uint32_t i = 0; i < to_copy; i++) {
            uint32_t pos = written + i;
            if (pos < sizeof(hdr))
                elem.addr[i] = reinterpret_cast<const uint8_t*>(&hdr)[pos];
            else
                elem.addr[i] = frame[pos - sizeof(hdr)];
        }
        written += to_copy;
    }

    vq->PushUsed(head, written);
    mmio_->NotifyUsedBuffer();
    return true;
}

void VirtioNetDevice::ReadConfig(uint32_t offset, uint8_t size, uint32_t* value) {
    const uint8_t* cfg = reinterpret_cast<const uint8_t*>(&config_);
    uint32_t cfg_size = sizeof(config_);
    *value = 0;
    if (offset + size <= cfg_size) {
        memcpy(value, cfg + offset, size);
    }
}

void VirtioNetDevice::WriteConfig(uint32_t offset, uint8_t size, uint32_t value) {
    // Config is read-only for net device
}

void VirtioNetDevice::OnStatusChange(uint32_t new_status) {
    if (new_status == 0) {
        LOG_INFO("VirtIO net: device reset");
    }
}
