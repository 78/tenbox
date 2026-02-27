#pragma once

#include "core/device/virtio/virtio_mmio.h"
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// VirtIO Console feature bits
constexpr uint64_t VIRTIO_CONSOLE_F_SIZE       = 1ULL << 0;
constexpr uint64_t VIRTIO_CONSOLE_F_MULTIPORT  = 1ULL << 1;
constexpr uint64_t VIRTIO_CONSOLE_F_EMERG_WRITE = 1ULL << 2;
#ifndef VIRTIO_F_VERSION_1_DEFINED
#define VIRTIO_F_VERSION_1_DEFINED
constexpr uint64_t VIRTIO_SERIAL_F_VERSION_1   = 1ULL << 32;
#else
static constexpr uint64_t VIRTIO_SERIAL_F_VERSION_1 = 1ULL << 32;
#endif

// Control message event types
constexpr uint16_t VIRTIO_CONSOLE_DEVICE_READY   = 0;
constexpr uint16_t VIRTIO_CONSOLE_DEVICE_ADD     = 1;
constexpr uint16_t VIRTIO_CONSOLE_DEVICE_REMOVE  = 2;
constexpr uint16_t VIRTIO_CONSOLE_PORT_READY     = 3;
constexpr uint16_t VIRTIO_CONSOLE_CONSOLE_PORT   = 4;
constexpr uint16_t VIRTIO_CONSOLE_RESIZE         = 5;
constexpr uint16_t VIRTIO_CONSOLE_PORT_OPEN      = 6;
constexpr uint16_t VIRTIO_CONSOLE_PORT_NAME      = 7;

#pragma pack(push, 1)
struct VirtioConsoleConfig {
    uint16_t cols;
    uint16_t rows;
    uint32_t max_nr_ports;
    uint32_t emerg_wr;
};

struct VirtioConsoleControl {
    uint32_t id;     // port number
    uint16_t event;  // VIRTIO_CONSOLE_*
    uint16_t value;
};
#pragma pack(pop)

static_assert(sizeof(VirtioConsoleConfig) == 12);
static_assert(sizeof(VirtioConsoleControl) == 8);

class VirtioSerialDevice : public VirtioDeviceOps {
public:
    using DataCallback = std::function<void(uint32_t port_id, const uint8_t* data, size_t len)>;

    explicit VirtioSerialDevice(uint32_t max_ports = 1);
    ~VirtioSerialDevice() override = default;

    void SetMmioDevice(VirtioMmioDevice* mmio) { mmio_ = mmio; }

    // Set callback for data received from guest
    void SetDataCallback(DataCallback cb) { data_callback_ = std::move(cb); }

    // Configure port name (must be called before guest driver initialization)
    void SetPortName(uint32_t port_id, const std::string& name);

    // Send data to guest on specified port
    bool SendData(uint32_t port_id, const uint8_t* data, size_t len);

    // VirtioDeviceOps interface
    uint32_t GetDeviceId() const override { return 3; }
    uint64_t GetDeviceFeatures() const override;
    uint32_t GetNumQueues() const override;
    uint32_t GetQueueMaxSize(uint32_t queue_idx) const override { return 256; }
    void OnQueueNotify(uint32_t queue_idx, VirtQueue& vq) override;
    void ReadConfig(uint32_t offset, uint8_t size, uint32_t* value) override;
    void WriteConfig(uint32_t offset, uint8_t size, uint32_t value) override;
    void OnStatusChange(uint32_t new_status) override;

private:
    void HandleControlMessage(VirtQueue& vq);
    void HandlePortTx(uint32_t port_id, VirtQueue& vq);
    void SendControlMessage(uint32_t port_id, uint16_t event, uint16_t value);
    void SendPortName(uint32_t port_id);

    struct PortState {
        std::string name;
        bool guest_connected = false;
        bool host_connected = true;
    };

    VirtioMmioDevice* mmio_ = nullptr;
    VirtioConsoleConfig config_{};
    uint32_t max_ports_ = 1;
    std::vector<PortState> ports_;
    DataCallback data_callback_;
    std::recursive_mutex mutex_;
    bool driver_ready_ = false;
};
