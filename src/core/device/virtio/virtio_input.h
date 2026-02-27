#pragma once

#include "core/device/virtio/virtio_mmio.h"
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Linux evdev event types
constexpr uint16_t EV_SYN = 0x00;
constexpr uint16_t EV_KEY = 0x01;
constexpr uint16_t EV_REL = 0x02;
constexpr uint16_t EV_ABS = 0x03;
constexpr uint16_t EV_MSC = 0x04;
constexpr uint16_t EV_REP = 0x14;

constexpr uint16_t SYN_REPORT = 0x00;
constexpr uint16_t MSC_SCAN   = 0x04;

// Absolute axis codes
constexpr uint16_t ABS_X = 0x00;
constexpr uint16_t ABS_Y = 0x01;

// Relative axis codes (for mouse wheel)
constexpr uint16_t REL_WHEEL  = 0x08;  // Vertical scroll
constexpr uint16_t REL_HWHEEL = 0x06;  // Horizontal scroll

// Mouse button codes (evdev)
constexpr uint16_t BTN_LEFT   = 0x110;
constexpr uint16_t BTN_RIGHT  = 0x111;
constexpr uint16_t BTN_MIDDLE = 0x112;
constexpr uint16_t BTN_TOUCH  = 0x14a;

// Input properties (linux/input.h)
constexpr uint16_t INPUT_PROP_POINTER  = 0x00;
constexpr uint16_t INPUT_PROP_DIRECT   = 0x01;

// Config select values (virtio spec 5.8.2)
constexpr uint8_t VIRTIO_INPUT_CFG_UNSET    = 0x00;
constexpr uint8_t VIRTIO_INPUT_CFG_ID_NAME  = 0x01;
constexpr uint8_t VIRTIO_INPUT_CFG_ID_SERIAL= 0x02;
constexpr uint8_t VIRTIO_INPUT_CFG_ID_DEVIDS= 0x03;
constexpr uint8_t VIRTIO_INPUT_CFG_PROP_BITS= 0x10;
constexpr uint8_t VIRTIO_INPUT_CFG_EV_BITS  = 0x11;
constexpr uint8_t VIRTIO_INPUT_CFG_ABS_INFO = 0x12;

#pragma pack(push, 1)
struct VirtioInputEvent {
    uint16_t type;
    uint16_t code;
    uint32_t value;
};

struct VirtioInputAbsInfo {
    uint32_t min;
    uint32_t max;
    uint32_t fuzz;
    uint32_t flat;
    uint32_t res;
};

struct VirtioInputDevIds {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
};

// Config space layout: select(1) + subsel(1) + size(1) + reserved(5) + data(128)
struct VirtioInputConfig {
    uint8_t select;
    uint8_t subsel;
    uint8_t size;
    uint8_t reserved[5];
    uint8_t data[128];
};
#pragma pack(pop)

static_assert(sizeof(VirtioInputEvent) == 8);
static_assert(sizeof(VirtioInputConfig) == 136);

class VirtioInputDevice : public VirtioDeviceOps {
public:
    enum class SubType { kKeyboard, kTablet };

    explicit VirtioInputDevice(SubType type);
    ~VirtioInputDevice() override = default;

    void SetMmioDevice(VirtioMmioDevice* mmio) { mmio_ = mmio; }

    // Inject a single evdev event. Caller should follow a group of events
    // with InjectEvent(EV_SYN, SYN_REPORT, 0).
    // When |notify| is false the used-buffer interrupt is deferred; the caller
    // must eventually call InjectEvent with notify=true (typically for
    // SYN_REPORT) to deliver the batched interrupt to the guest.
    void InjectEvent(uint16_t type, uint16_t code, uint32_t value,
                     bool notify = true);

    uint32_t GetDeviceId() const override { return 18; }
    uint64_t GetDeviceFeatures() const override;
    uint32_t GetNumQueues() const override { return 2; }
    uint32_t GetQueueMaxSize(uint32_t queue_idx) const override { return 64; }
    void OnQueueNotify(uint32_t queue_idx, VirtQueue& vq) override;
    void ReadConfig(uint32_t offset, uint8_t size, uint32_t* value) override;
    void WriteConfig(uint32_t offset, uint8_t size, uint32_t value) override;
    void OnStatusChange(uint32_t new_status) override;

private:
    void UpdateConfigData();

    SubType sub_type_;
    VirtioMmioDevice* mmio_ = nullptr;
    VirtioInputConfig config_{};
    std::mutex inject_mutex_;
};
