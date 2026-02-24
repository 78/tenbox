#include "device/acpi/acpi_pm.h"

void AcpiPm::PioRead(uint16_t offset, uint8_t size, uint32_t* value) {
    switch (offset) {
    case 0: // PM1_STS (16-bit)
        *value = pm1_sts_;
        break;
    case 2: // PM1_EN (16-bit)
        *value = pm1_en_;
        break;
    case 4: // PM1_CNT (16-bit) — SCI_EN is always set
        *value = pm1_cnt_;
        break;
    default:
        *value = 0;
        break;
    }
}

void AcpiPm::PioWrite(uint16_t offset, uint8_t size, uint32_t value) {
    switch (offset) {
    case 0: // PM1_STS — write-1-to-clear
        pm1_sts_ &= ~static_cast<uint16_t>(value);
        break;
    case 2: // PM1_EN
        pm1_en_ = static_cast<uint16_t>(value);
        break;
    case 4: { // PM1_CNT
        pm1_cnt_ = static_cast<uint16_t>(value) | 1u; // preserve SCI_EN
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
    default:
        break;
    }
}
