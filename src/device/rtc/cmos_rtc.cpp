#include "device/rtc/cmos_rtc.h"

uint8_t CmosRtc::ToBcd(int val) {
    return static_cast<uint8_t>(((val / 10) << 4) | (val % 10));
}

uint8_t CmosRtc::ReadRegister(uint8_t reg) const {
    time_t now = time(nullptr);
    struct tm t;
    gmtime_s(&t, &now);

    switch (reg) {
    case kRegSeconds:    return ToBcd(t.tm_sec);
    case kRegMinutes:    return ToBcd(t.tm_min);
    case kRegHours:      return ToBcd(t.tm_hour);
    case kRegDayOfWeek:  return ToBcd(t.tm_wday + 1);
    case kRegDayOfMonth: return ToBcd(t.tm_mday);
    case kRegMonth:      return ToBcd(t.tm_mon + 1);
    case kRegYear:       return ToBcd((t.tm_year + 1900) % 100);
    case kRegCentury:    return ToBcd((t.tm_year + 1900) / 100);

    case kRegStatusA:
        // Bit 7 (UIP) = 0 -> no update in progress
        // Bits 6-4: divider = 010 (32.768 kHz)
        // Bits 3-0: rate = 0110 (1024 Hz)
        return 0x26;

    case kRegStatusB:
        // Bit 1: 24-hour mode
        // Bit 2: BCD mode (0 = BCD)
        return 0x02;

    case kRegStatusC:
        return 0x00;

    case kRegStatusD:
        // Bit 7: valid RAM and time
        return 0x80;

    default:
        return 0x00;
    }
}

void CmosRtc::PioRead(uint16_t offset, uint8_t size, uint32_t* value) {
    if (offset == 0) {
        *value = index_;
    } else {
        *value = ReadRegister(index_);
    }
}

void CmosRtc::PioWrite(uint16_t offset, uint8_t size, uint32_t value) {
    if (offset == 0) {
        index_ = static_cast<uint8_t>(value) & 0x7F;
    }
    // Writes to data port (offset 1) are ignored for now
}
