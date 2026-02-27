#include "core/device/acpi/acpi_pm.h"

void AcpiPm::TriggerPowerButton() {
    // FADT declares no fixed-hardware power button (PWR_BUTTON flag set).
    // The guest is expected to shut down via console `poweroff` command,
    // which writes SLP_EN+SLP_TYP to PM1_CNT and triggers shutdown_cb_.
    LOG_INFO("ACPI: TriggerPowerButton called (no-op; guest uses poweroff)");
}

void AcpiPm::RaiseSci() {
    if ((pm1_sts_ & pm1_en_) && sci_cb_) {
        sci_cb_();
    }
}

void AcpiPm::PioRead(uint16_t offset, uint8_t size, uint32_t* value) {
    switch (offset) {
    case 0:
        if (size == 4) {
            *value = pm1_sts_ | (static_cast<uint32_t>(pm1_en_) << 16);
        } else {
            *value = pm1_sts_;
        }
        break;
    case 2:
        *value = pm1_en_;
        break;
    case 4:
        *value = pm1_cnt_;
        break;
    case 8:
        // RESET_REG read returns 0
        *value = 0;
        break;
    default:
        *value = 0;
        break;
    }
}

void AcpiPm::PioWrite(uint16_t offset, uint8_t size, uint32_t value) {
    switch (offset) {
    case 0:
        pm1_sts_ &= ~static_cast<uint16_t>(value);
        if (size == 4) {
            pm1_en_ = static_cast<uint16_t>(value >> 16);
        }
        break;
    case 2:
        pm1_en_ = static_cast<uint16_t>(value);
        break;
    case 4: {
        pm1_cnt_ = static_cast<uint16_t>(value) | 1u;
        if (value & (1u << 13)) {
            uint8_t slp_typ = (value >> 10) & 7;
            LOG_INFO("ACPI: SLP_EN set (SLP_TYP=%u)", slp_typ);
            if (slp_typ == kSlpTypS5 && shutdown_cb_) {
                LOG_INFO("ACPI: S5 power off requested");
                shutdown_cb_();
            }
        }
        break;
    }
    case 8:
        // RESET_REG: writing kResetValue triggers system reset
        if ((value & 0xFF) == kResetValue && reset_cb_) {
            LOG_INFO("ACPI: system reset requested via RESET_REG");
            reset_cb_();
        }
        break;
    default:
        break;
    }
}
