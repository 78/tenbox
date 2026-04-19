#pragma once

#ifdef _WIN32
#define NOMINMAX
#endif

#include <cstdint>
#include <cstddef>
#include <cinttypes>   // PRIu64 / PRIx64 / PRIX64 / PRId64 for portable uint64_t printf
#include <string>
#include <memory>
#include <functional>
#include <stdexcept>
#include <cstdio>
#include <mutex>

inline std::mutex& GetStdoutMutex() {
    static std::mutex m;
    return m;
}

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
    uint64_t alloc_size = 0;   // total bytes of the host allocation
    GPA      ram_base   = 0;   // GPA where the low RAM region starts (0 on x86, 0x40000000 on ARM)
    uint64_t low_size   = 0;   // guest RAM in [ram_base, ram_base + low_size)
    GPA      high_base  = 0;   // GPA where high RAM begins (kMmioGapEnd)
    uint64_t high_size  = 0;   // guest RAM in [high_base, high_base+high_size)

    uint8_t* GpaToHva(GPA gpa) const {
        if (gpa >= ram_base && gpa - ram_base < low_size)
            return base + (gpa - ram_base);
        if (high_size && gpa >= high_base && gpa - high_base < high_size)
            return base + low_size + (gpa - high_base);
        return nullptr;
    }

    uint64_t TotalRam() const { return alloc_size; }
};

#define LOG_INFO(fmt, ...)  do { \
    std::lock_guard<std::mutex> _lk(GetStdoutMutex()); \
    fprintf(stdout, "[INFO]  " fmt "\r\n", ##__VA_ARGS__); \
    fflush(stdout); \
} while (0)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN]  " fmt "\r\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\r\n", ##__VA_ARGS__)

#ifdef _DEBUG
#define LOG_DEBUG(fmt, ...) do { \
    std::lock_guard<std::mutex> _lk(GetStdoutMutex()); \
    fprintf(stdout, "[DEBUG] " fmt "\r\n", ##__VA_ARGS__); \
    fflush(stdout); \
} while (0)
#else
#define LOG_DEBUG(fmt, ...) ((void)0)
#endif
