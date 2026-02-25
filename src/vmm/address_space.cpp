#include "vmm/address_space.h"

void AddressSpace::AddPioDevice(uint16_t base, uint16_t size, Device* device) {
    pio_devices_.push_back({base, size, device});
}

void AddressSpace::AddMmioDevice(uint64_t base, uint64_t size, Device* device) {
    mmio_devices_.push_back({base, size, device});
}

Device* AddressSpace::FindPioDevice(uint16_t port, uint16_t* offset) const {
    for (auto& entry : pio_devices_) {
        if (port >= entry.base && port < entry.base + entry.size) {
            *offset = port - entry.base;
            return entry.device;
        }
    }
    return nullptr;
}

Device* AddressSpace::FindMmioDevice(uint64_t addr, uint64_t* offset) const {
    for (auto& entry : mmio_devices_) {
        if (addr >= entry.base && addr < entry.base + entry.size) {
            *offset = addr - entry.base;
            return entry.device;
        }
    }
    return nullptr;
}

bool AddressSpace::HandlePortIn(uint16_t port, uint8_t size, uint32_t* value) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    uint16_t offset = 0;
    Device* dev = FindPioDevice(port, &offset);
    if (dev) {
        dev->PioRead(offset, size, value);
        return true;
    }
    *value = 0xFFFFFFFF;
    LOG_DEBUG("Unhandled PIO read: port=0x%X size=%u", port, size);
    return false;
}

bool AddressSpace::HandlePortOut(uint16_t port, uint8_t size, uint32_t value) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    uint16_t offset = 0;
    Device* dev = FindPioDevice(port, &offset);
    if (dev) {
        dev->PioWrite(offset, size, value);
        return true;
    }
    LOG_DEBUG("Unhandled PIO write: port=0x%X size=%u val=0x%X",
              port, size, value);
    return false;
}

bool AddressSpace::HandleMmioRead(uint64_t addr, uint8_t size,
                                   uint64_t* value) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    uint64_t offset = 0;
    Device* dev = FindMmioDevice(addr, &offset);
    if (dev) {
        dev->MmioRead(offset, size, value);
        return true;
    }
    *value = 0;
    LOG_DEBUG("Unhandled MMIO read: addr=0x%llX size=%u", addr, size);
    return false;
}

bool AddressSpace::HandleMmioWrite(uint64_t addr, uint8_t size,
                                    uint64_t value) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    uint64_t offset = 0;
    Device* dev = FindMmioDevice(addr, &offset);
    if (dev) {
        dev->MmioWrite(offset, size, value);
        return true;
    }
    LOG_DEBUG("Unhandled MMIO write: addr=0x%llX size=%u val=0x%llX",
              addr, size, value);
    return false;
}
