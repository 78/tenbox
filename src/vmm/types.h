#pragma once

#define NOMINMAX

#include <cstdint>
#include <cstddef>
#include <string>
#include <memory>
#include <functional>
#include <stdexcept>
#include <cstdio>

using GPA = uint64_t;
using HVA = uint8_t*;

constexpr uint64_t kPageSize = 4096;
constexpr uint64_t kPageMask = ~(kPageSize - 1);

inline uint64_t AlignUp(uint64_t val, uint64_t align) {
    return (val + align - 1) & ~(align - 1);
}

inline uint64_t AlignDown(uint64_t val, uint64_t align) {
    return val & ~(align - 1);
}

// The 32-bit address space between kMmioGapStart and kMmioGapEnd is reserved
// for memory-mapped I/O (I/O APIC, VirtIO MMIO, etc.).  When guest RAM exceeds
// kMmioGapStart the allocation is split into a low region [0, kMmioGapStart)
// and a high region starting at kMmioGapEnd.
constexpr GPA kMmioGapStart = 0xC0000000;  // 3 GiB
constexpr GPA kMmioGapEnd   = 0x100000000; // 4 GiB

struct GuestMemMap {
    uint8_t* base = nullptr;
    uint64_t alloc_size = 0;   // total bytes of the host VirtualAlloc
    uint64_t low_size   = 0;   // guest RAM in [0, low_size)
    GPA      high_base  = 0;   // GPA where high RAM begins (kMmioGapEnd)
    uint64_t high_size  = 0;   // guest RAM in [high_base, high_base+high_size)

    uint8_t* GpaToHva(GPA gpa) const {
        if (gpa < low_size)
            return base + gpa;
        if (high_size && gpa >= high_base && gpa - high_base < high_size)
            return base + low_size + (gpa - high_base);
        return nullptr;
    }

    uint64_t TotalRam() const { return alloc_size; }
};

#define LOG_INFO(fmt, ...)  fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

#ifdef _DEBUG
#define LOG_DEBUG(fmt, ...) fprintf(stdout, "[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) ((void)0)
#endif
