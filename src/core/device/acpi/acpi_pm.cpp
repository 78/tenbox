#include "core/device/acpi/acpi_pm.h"

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#include <unistd.h>
#else
#include <time.h>
#include <unistd.h>
#endif

AcpiPm::AcpiPm() {
#ifdef _WIN32
    LARGE_INTEGER qpf, qpc_start, qpc_end;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&qpc_start);
    uint64_t tsc_start = __rdtsc();
    Sleep(50);
    uint64_t tsc_end = __rdtsc();
    QueryPerformanceCounter(&qpc_end);

    double elapsed = static_cast<double>(qpc_end.QuadPart - qpc_start.QuadPart)
                     / qpf.QuadPart;
#elif defined(__APPLE__)
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    uint64_t mach_start = mach_absolute_time();
    uint64_t tsc_start = __rdtsc();
    usleep(50000);
    uint64_t tsc_end = __rdtsc();
    uint64_t mach_end = mach_absolute_time();

    double elapsed = static_cast<double>(mach_end - mach_start) * tb.numer / tb.denom / 1e9;
#else
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    uint64_t tsc_start = __rdtsc();
    usleep(50000);
    uint64_t tsc_end = __rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    double elapsed = static_cast<double>(ts_end.tv_sec - ts_start.tv_sec) +
                     static_cast<double>(ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
#endif
    tsc_freq_ = static_cast<uint64_t>((tsc_end - tsc_start) / elapsed);
    tsc_base_ = __rdtsc();
}

uint32_t AcpiPm::ReadPmTimer() const {
    // Convert TSC ticks to PM Timer ticks (3.579545 MHz)
    uint64_t tsc_now = __rdtsc();
    uint64_t tsc_elapsed = tsc_now - tsc_base_;
    uint64_t pm_ticks = (tsc_elapsed * kPmTimerFreq) / tsc_freq_;
    return static_cast<uint32_t>(pm_ticks);
}

void AcpiPm::TriggerPowerButton() {
    LOG_INFO("ACPI: TriggerPowerButton called (no-op; guest uses poweroff)");
}

void AcpiPm::RaiseSci() {
    if ((pm1_sts_ & pm1_en_) && sci_cb_) {
        sci_cb_();
    }
}

void AcpiPm::PioRead(uint16_t offset, uint8_t size, uint32_t* value) {
    switch (offset) {
    case 0:  // PM1a_EVT_BLK (PM1_STS + PM1_EN)
        if (size == 4) {
            *value = pm1_sts_ | (static_cast<uint32_t>(pm1_en_) << 16);
        } else {
            *value = pm1_sts_;
        }
        break;
    case 2:  // PM1_EN
        *value = pm1_en_;
        break;
    case 4:  // PM1a_CNT_BLK
        *value = pm1_cnt_;
        break;
    case 8:  // PM_TMR_BLK
    case 9:
    case 10:
    case 11:
        *value = ReadPmTimer();
        break;
    case 15: // RESET_REG
        *value = 0;
        break;
    default:
        *value = 0;
        break;
    }
}

void AcpiPm::PioWrite(uint16_t offset, uint8_t size, uint32_t value) {
    switch (offset) {
    case 0:  // PM1a_EVT_BLK
        pm1_sts_ &= ~static_cast<uint16_t>(value);
        if (size == 4) {
            pm1_en_ = static_cast<uint16_t>(value >> 16);
        }
        break;
    case 2:  // PM1_EN
        pm1_en_ = static_cast<uint16_t>(value);
        break;
    case 4: {  // PM1a_CNT_BLK
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
    case 8:  // PM_TMR_BLK (read-only, ignore writes)
    case 9:
    case 10:
    case 11:
        break;
    case 15: // RESET_REG
        if ((value & 0xFF) == kResetValue && reset_cb_) {
            LOG_INFO("ACPI: system reset requested via RESET_REG");
            reset_cb_();
        }
        break;
    default:
        break;
    }
}
