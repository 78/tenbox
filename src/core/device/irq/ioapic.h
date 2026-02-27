#pragma once

#include "core/device/device.h"
#include <array>
#include <mutex>

// Minimal I/O APIC emulation at 0xFEC00000.
// Provides the register interface the kernel expects during initialization.
// Does not actually route interrupts yet (Phase 2).
class IoApic : public Device {
public:
    static constexpr uint64_t kBaseAddress = 0xFEC00000;
    static constexpr uint64_t kSize = 0x100000;  // 1 MB window

    static constexpr uint8_t kMaxRedirEntries = 24;

    IoApic() { ResetRedirTable(); }

    void MmioRead(uint64_t offset, uint8_t size, uint64_t* value) override;
    void MmioWrite(uint64_t offset, uint8_t size, uint64_t value) override;

    // Returns the 64-bit redirection table entry for a given IRQ pin.
    bool GetRedirEntry(uint8_t irq, uint64_t* entry) const;

private:
    // I/O APIC registers
    static constexpr uint32_t kRegId      = 0x00;
    static constexpr uint32_t kRegVer     = 0x01;
    static constexpr uint32_t kRegArb     = 0x02;
    static constexpr uint32_t kRegRedTbl  = 0x10; // 0x10-0x3F

    uint32_t index_ = 0;
    uint32_t id_ = 0;

    // Each redirection entry is 64 bits (low + high).
    // Per Intel I/O APIC spec, reset value has bit 16 (mask) set.
    mutable std::mutex mutex_;
    std::array<uint64_t, kMaxRedirEntries> redir_table_;

    void ResetRedirTable() {
        redir_table_.fill(0x0000000000010000ULL);
    }

    uint32_t ReadRegister() const;
    void WriteRegister(uint32_t value);
};
