#pragma once

#include "core/device/device.h"
#include <cstring>

// PCI Type 1 configuration mechanism (ports 0xCF8-0xCFF).
// Emulates a single device: bus 0, device 0, function 0 as a PCI host bridge
// so that the Linux kernel's pci_sanity_check() succeeds.  All other
// bus/device/function combinations return 0xFFFFFFFF (no device).
class PciHostBridge : public Device {
public:
    static constexpr uint16_t kBasePort = 0xCF8;
    static constexpr uint16_t kRegCount = 8;

    void PioRead(uint16_t offset, uint8_t size, uint32_t* value) override {
        if (offset == 0) {
            if (size == 4) {
                *value = config_addr_;
            } else if (size == 1) {
                *value = (config_addr_ >> 0) & 0xFF;
            } else {
                *value = 0;
            }
            return;
        }

        // CONFIG_DATA at offset 4..7
        if (offset >= 4 && offset <= 7 && (config_addr_ & 0x80000000)) {
            uint8_t reg  = config_addr_ & 0xFC;
            uint8_t func = (config_addr_ >> 8) & 0x7;
            uint8_t dev  = (config_addr_ >> 11) & 0x1F;
            uint8_t bus  = (config_addr_ >> 16) & 0xFF;

            uint32_t dword = 0xFFFFFFFF;
            if (bus == 0 && dev == 0 && func == 0) {
                dword = ReadHostBridgeConfig(reg);
            }

            uint8_t byte_off = (offset - 4) + (reg & 3);
            if (size == 4) {
                *value = dword;
            } else if (size == 2) {
                *value = (dword >> (8 * (byte_off & 2))) & 0xFFFF;
            } else {
                *value = (dword >> (8 * (byte_off & 3))) & 0xFF;
            }
            return;
        }

        *value = 0xFFFFFFFF;
    }

    void PioWrite(uint16_t offset, uint8_t size, uint32_t value) override {
        if (offset == 0 && size == 4)
            config_addr_ = value;
    }

private:
    uint32_t config_addr_ = 0;

    // Minimal 64-byte PCI config header for a host bridge at 00:00.0.
    static uint32_t ReadHostBridgeConfig(uint8_t reg) {
        // Vendor/Device: use a non-conflicting pair (0x1B36 = Red Hat / 0x0008)
        static constexpr uint32_t cfg[] = {
            0x00081B36,  // 00: Vendor 1B36h, Device 0008h
            0x00000006,  // 04: Command(MemSpace+BusMaster=0), Status
            0x06000001,  // 08: Class=0600 (Host bridge), Rev=01
            0x00000000,  // 0C: CacheLineSize, Latency, HeaderType=00, BIST
        };
        unsigned idx = reg / 4;
        if (idx < sizeof(cfg) / sizeof(cfg[0])) return cfg[idx];
        return 0;
    }
};
