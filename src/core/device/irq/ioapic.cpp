#include "core/device/irq/ioapic.h"

bool IoApic::GetRedirEntry(uint8_t irq, uint64_t* entry) const {
    if (irq >= kMaxRedirEntries) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    *entry = redir_table_[irq];
    return true;
}

void IoApic::MmioRead(uint64_t offset, uint8_t size, uint64_t* value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (offset == 0x00) {
        *value = index_;
    } else if (offset == 0x10) {
        *value = ReadRegister();
    } else {
        *value = 0;
    }
}

void IoApic::MmioWrite(uint64_t offset, uint8_t size, uint64_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (offset == 0x00) {
        index_ = static_cast<uint32_t>(value) & 0xFF;
    } else if (offset == 0x10) {
        WriteRegister(static_cast<uint32_t>(value));
    } else if (offset == 0x40) {
        // EOI register: clear Remote IRR for the entry matching this vector.
        uint32_t vector = static_cast<uint32_t>(value) & 0xFF;
        for (auto& rte : redir_table_) {
            if ((rte & 0xFF) == vector) {
                rte &= ~(1ULL << 14);  // clear Remote IRR (bit 14)
            }
        }
    }
}

uint32_t IoApic::ReadRegister() const {
    switch (index_) {
    case kRegId:
        return id_ << 24;

    case kRegVer:
        // Version 0x20, max redir entries = 23 (24 total)
        return 0x00170020;

    case kRegArb:
        return 0;

    default:
        if (index_ >= kRegRedTbl && index_ < kRegRedTbl + kMaxRedirEntries * 2) {
            uint32_t entry_idx = (index_ - kRegRedTbl) / 2;
            bool high = (index_ - kRegRedTbl) % 2;
            if (entry_idx < kMaxRedirEntries) {
                uint64_t entry = redir_table_[entry_idx];
                return high ? static_cast<uint32_t>(entry >> 32)
                            : static_cast<uint32_t>(entry);
            }
        }
        return 0;
    }
}

void IoApic::WriteRegister(uint32_t value) {
    switch (index_) {
    case kRegId:
        id_ = (value >> 24) & 0x0F;
        break;

    default:
        if (index_ >= kRegRedTbl && index_ < kRegRedTbl + kMaxRedirEntries * 2) {
            uint32_t entry_idx = (index_ - kRegRedTbl) / 2;
            bool high = (index_ - kRegRedTbl) % 2;
            if (entry_idx < kMaxRedirEntries) {
                if (high) {
                    redir_table_[entry_idx] =
                        (redir_table_[entry_idx] & 0x00000000FFFFFFFFULL) |
                        (static_cast<uint64_t>(value) << 32);
                } else {
                    redir_table_[entry_idx] =
                        (redir_table_[entry_idx] & 0xFFFFFFFF00000000ULL) |
                        value;
                }
            }
        }
        break;
    }
}
