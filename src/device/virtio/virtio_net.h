#pragma once

#include "device/virtio/virtio_mmio.h"
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

constexpr uint64_t VIRTIO_NET_F_MAC    = 1ULL << 5;
constexpr uint64_t VIRTIO_NET_F_STATUS = 1ULL << 16;
// VIRTIO_F_VERSION_1 is defined in virtio_blk.h; redeclare here
#ifndef VIRTIO_F_VERSION_1_DEFINED
#define VIRTIO_F_VERSION_1_DEFINED
constexpr uint64_t VIRTIO_NET_F_VERSION_1 = 1ULL << 32;
#endif

#pragma pack(push, 1)
// virtio 1.x (VIRTIO_F_VERSION_1) always includes num_buffers
struct VirtioNetHdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
};

struct VirtioNetConfig {
    uint8_t  mac[6];
    uint16_t status;
};
#pragma pack(pop)

static_assert(sizeof(VirtioNetHdr) == 12);

class VirtioNetDevice : public VirtioDeviceOps {
public:
    using TxCallback = std::function<void(const uint8_t* frame, uint32_t len)>;

    VirtioNetDevice();
    ~VirtioNetDevice() override = default;

    void SetMmioDevice(VirtioMmioDevice* mmio) { mmio_ = mmio; }
    void SetTxCallback(TxCallback cb) { tx_callback_ = std::move(cb); }

    // Inject a received Ethernet frame into the guest RX queue.
    // Thread-safe: called from the network thread.
    bool InjectRx(const uint8_t* frame, uint32_t len);

    uint32_t GetDeviceId() const override { return 1; }
    uint64_t GetDeviceFeatures() const override;
    uint32_t GetNumQueues() const override { return 2; }
    uint32_t GetQueueMaxSize(uint32_t queue_idx) const override { return 256; }
    void OnQueueNotify(uint32_t queue_idx, VirtQueue& vq) override;
    void ReadConfig(uint32_t offset, uint8_t size, uint32_t* value) override;
    void WriteConfig(uint32_t offset, uint8_t size, uint32_t value) override;
    void OnStatusChange(uint32_t new_status) override;

private:
    VirtioMmioDevice* mmio_ = nullptr;
    VirtioNetConfig config_{};
    TxCallback tx_callback_;
    std::mutex rx_mutex_;
};
