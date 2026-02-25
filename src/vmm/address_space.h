#pragma once

#include "vmm/types.h"
#include "device/device.h"
#include <vector>
#include <mutex>

struct PioEntry {
    uint16_t base;
    uint16_t size;
    Device*  device;
};

struct MmioEntry {
    uint64_t base;
    uint64_t size;
    Device*  device;
};

class AddressSpace {
public:
    void AddPioDevice(uint16_t base, uint16_t size, Device* device);
    void AddMmioDevice(uint64_t base, uint64_t size, Device* device);

    bool HandlePortIn(uint16_t port, uint8_t size, uint32_t* value);
    bool HandlePortOut(uint16_t port, uint8_t size, uint32_t value);
    bool HandleMmioRead(uint64_t addr, uint8_t size, uint64_t* value);
    bool HandleMmioWrite(uint64_t addr, uint8_t size, uint64_t value);

private:
    Device* FindPioDevice(uint16_t port, uint16_t* offset) const;
    Device* FindMmioDevice(uint64_t addr, uint64_t* offset) const;

    mutable std::mutex io_mutex_;
    std::vector<PioEntry>  pio_devices_;
    std::vector<MmioEntry> mmio_devices_;
};
