#pragma once

#include "core/device/device.h"
#include <functional>

// Minimal ACPI PM1 register emulation.
// Provides PM1a Event Block (4 bytes) and PM1a Control Block (2 bytes)
// at contiguous I/O ports so the kernel can enable ACPI without
// HW_REDUCED_ACPI (which breaks legacy IRQ pre-allocation).
class AcpiPm : public Device {
public:
    static constexpr uint16_t kBasePort   = 0x600;
    static constexpr uint16_t kRegCount   = 9;    // EVT(4) + CNT(2) + gap(2) + RESET(1)
    static constexpr uint16_t kEvtPort    = 0x600; // PM1a_EVT_BLK
    static constexpr uint16_t kCntPort    = 0x604; // PM1a_CNT_BLK
    static constexpr uint8_t  kEvtLen     = 4;
    static constexpr uint8_t  kCntLen     = 2;
    static constexpr uint8_t  kSlpTypS5   = 5;    // Must match DSDT \_S5 package

    // ACPI RESET_REG: dedicated I/O port for system reset
    static constexpr uint16_t kResetPort  = 0x608;
    static constexpr uint8_t  kResetValue = 0x01; // Value to trigger reset

    void SetShutdownCallback(std::function<void()> cb) { shutdown_cb_ = std::move(cb); }
    void SetResetCallback(std::function<void()> cb) { reset_cb_ = std::move(cb); }
    void SetSciCallback(std::function<void()> cb) { sci_cb_ = std::move(cb); }

    void TriggerPowerButton();

    void PioRead(uint16_t offset, uint8_t size, uint32_t* value) override;
    void PioWrite(uint16_t offset, uint8_t size, uint32_t value) override;

private:
    void RaiseSci();

    uint16_t pm1_sts_ = 0;
    uint16_t pm1_en_  = 0;
    uint16_t pm1_cnt_ = 1; // SCI_EN (bit 0) always set
    std::function<void()> shutdown_cb_;
    std::function<void()> reset_cb_;
    std::function<void()> sci_cb_;
};
