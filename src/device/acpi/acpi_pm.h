#pragma once

#include "device/device.h"
#include <functional>

// Minimal ACPI PM1 register emulation.
// Provides PM1a Event Block (4 bytes) and PM1a Control Block (2 bytes)
// at contiguous I/O ports so the kernel can enable ACPI without
// HW_REDUCED_ACPI (which breaks legacy IRQ pre-allocation).
class AcpiPm : public Device {
public:
    static constexpr uint16_t kBasePort   = 0x600;
    static constexpr uint16_t kRegCount   = 6;    // EVT(4) + CNT(2)
    static constexpr uint16_t kEvtPort    = 0x600; // PM1a_EVT_BLK
    static constexpr uint16_t kCntPort    = 0x604; // PM1a_CNT_BLK
    static constexpr uint8_t  kEvtLen     = 4;
    static constexpr uint8_t  kCntLen     = 2;
    static constexpr uint8_t  kSlpTypS5   = 5;    // Must match DSDT \_S5 package

    void SetShutdownCallback(std::function<void()> cb) { shutdown_cb_ = std::move(cb); }

    void PioRead(uint16_t offset, uint8_t size, uint32_t* value) override;
    void PioWrite(uint16_t offset, uint8_t size, uint32_t value) override;

private:
    uint16_t pm1_sts_ = 0;
    uint16_t pm1_en_  = 0;
    uint16_t pm1_cnt_ = 1; // SCI_EN (bit 0) always set
    std::function<void()> shutdown_cb_;
};
