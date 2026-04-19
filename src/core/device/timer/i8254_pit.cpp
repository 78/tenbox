#include "core/device/timer/i8254_pit.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <cpuid.h>
#include <mach/mach_time.h>
#include <unistd.h>
#else
#include <cpuid.h>
#include <time.h>
#include <unistd.h>
#endif

uint64_t I8254Pit::MeasureTscFrequency() {
    // Try CPUID 0x15 (TSC / Core Crystal Clock) first.
#ifdef _WIN32
    int info[4]{};
    __cpuid(info, 0x15);
#else
    unsigned int info[4]{};
    __cpuid(0x15, info[0], info[1], info[2], info[3]);
#endif
    uint32_t denom  = static_cast<uint32_t>(info[0]);
    uint32_t numer  = static_cast<uint32_t>(info[1]);
    uint32_t crystal = static_cast<uint32_t>(info[2]);
    if (denom && numer && crystal) {
        uint64_t freq = static_cast<uint64_t>(crystal) * numer / denom;
        LOG_INFO("TSC frequency from CPUID 0x15: %" PRIu64 " Hz", freq);
        return freq;
    }

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
    uint64_t freq = static_cast<uint64_t>((tsc_end - tsc_start) / elapsed);
    LOG_INFO("TSC frequency measured: %" PRIu64 " Hz", freq);
    return freq;
}

I8254Pit::I8254Pit() : tsc_freq_(MeasureTscFrequency()) {
#ifdef __APPLE__
    ch0_queue_ = dispatch_queue_create("pit.ch0", DISPATCH_QUEUE_SERIAL);
#endif
}

I8254Pit::~I8254Pit() {
    StopChannel0Timer();
#ifdef __APPLE__
    if (ch0_queue_) {
        dispatch_release(ch0_queue_);
        ch0_queue_ = nullptr;
    }
#endif
}

void I8254Pit::StopChannel0Timer() {
#ifdef __APPLE__
    if (ch0_timer_) {
        dispatch_source_cancel(ch0_timer_);
        dispatch_release(ch0_timer_);
        ch0_timer_ = nullptr;
    }
#endif
}

void I8254Pit::ArmChannel0Timer() {
#ifdef __APPLE__
    StopChannel0Timer();
    auto& ch = channels_[0];
    if (!ch.armed || ch.reload == 0) return;
    if (ch.mode != 2 && ch.mode != 3) return;

    uint32_t reload = (ch.reload == 0) ? 65536u : static_cast<uint32_t>(ch.reload);
    // Period in nanoseconds: reload / 1193182 Hz * 1e9
    uint64_t period_ns = static_cast<uint64_t>(
        static_cast<double>(reload) / kPitFrequencyHz * 1e9);
    if (period_ns < 100000) period_ns = 100000; // floor at 100us

    ch0_timer_ = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_TIMER, 0, 0, ch0_queue_);
    dispatch_source_set_timer(ch0_timer_,
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)period_ns),
        period_ns, 0);
    dispatch_source_set_event_handler(ch0_timer_, ^{
        if (irq_callback_) irq_callback_();
    });
    dispatch_resume(ch0_timer_);
#endif
}

uint64_t I8254Pit::ElapsedPitTicks(int ch) const {
    auto& c = channels_[ch];
    if (!c.armed) return 0;
    uint64_t tsc_now = __rdtsc();
    uint64_t tsc_elapsed = tsc_now - c.start_tsc;
    return static_cast<uint64_t>(
        static_cast<double>(tsc_elapsed) * kPitFrequencyHz
        / static_cast<double>(tsc_freq_));
}

uint16_t I8254Pit::CurrentCount(int ch) const {
    auto& c = channels_[ch];
    uint32_t reload = (c.reload == 0) ? 65536u : static_cast<uint32_t>(c.reload);
    uint64_t ticks = ElapsedPitTicks(ch);

    if (c.mode == 0) {
        if (ticks >= reload) return 0;
        return static_cast<uint16_t>(reload - ticks);
    }

    return static_cast<uint16_t>(reload - (ticks % reload));
}

bool I8254Pit::OutputHigh(int ch) const {
    auto& c = channels_[ch];
    uint32_t reload = (c.reload == 0) ? 65536u : static_cast<uint32_t>(c.reload);
    uint64_t ticks = ElapsedPitTicks(ch);

    switch (c.mode) {
    case 0:
        return c.armed && ticks >= reload;
    case 2:
        return (ticks % reload) != (reload - 1);
    case 3: {
        uint64_t phase = ticks % reload;
        return phase < (reload / 2);
    }
    default:
        return true;
    }
}

bool I8254Pit::IsChannel2OutputHigh() const {
    return OutputHigh(2);
}

void I8254Pit::PioRead(uint16_t offset, uint8_t size, uint32_t* value) {
    if (offset >= 3) {
        *value = 0xFF;
        return;
    }

    auto& ch = channels_[offset];
    uint16_t count;

    if (ch.latch_pending) {
        count = ch.latched_value;
    } else {
        count = CurrentCount(offset);
    }

    if (ch.access == 1) {
        *value = count & 0xFF;
        ch.latch_pending = false;
    } else if (ch.access == 2) {
        *value = (count >> 8) & 0xFF;
        ch.latch_pending = false;
    } else {
        if (ch.read_lo_next) {
            *value = count & 0xFF;
            ch.read_lo_next = false;
        } else {
            *value = (count >> 8) & 0xFF;
            ch.read_lo_next = true;
            ch.latch_pending = false;
        }
    }
}

void I8254Pit::PioWrite(uint16_t offset, uint8_t size, uint32_t value) {
    uint8_t val = static_cast<uint8_t>(value);

    if (offset == 3) {
        int ch_num = (val >> 6) & 0x03;
        if (ch_num == 3) return;

        auto& ch = channels_[ch_num];
        uint8_t access = (val >> 4) & 0x03;

        if (access == 0) {
            ch.latch_pending = true;
            ch.latched_value = CurrentCount(ch_num);
            return;
        }

        ch.access = access;
        ch.mode = (val >> 1) & 0x07;
        ch.armed = false;
        ch.write_lo_next = true;
        ch.read_lo_next = true;
        return;
    }

    if (offset >= 3) return;

    auto& ch = channels_[offset];

    if (ch.access == 1) {
        ch.reload = (ch.reload & 0xFF00) | val;
        ch.armed = true;
        ch.start_tsc = __rdtsc();
    } else if (ch.access == 2) {
        ch.reload = (ch.reload & 0x00FF) | (val << 8);
        ch.armed = true;
        ch.start_tsc = __rdtsc();
    } else {
        if (ch.write_lo_next) {
            ch.reload = (ch.reload & 0xFF00) | val;
            ch.write_lo_next = false;
        } else {
            ch.reload = (ch.reload & 0x00FF) | (val << 8);
            ch.write_lo_next = true;
            ch.armed = true;
            ch.start_tsc = __rdtsc();
        }
    }

    if (offset == 0 && ch.armed) {
        ArmChannel0Timer();
    }
}

// --- System Control Port B (0x61) ---

void SystemControlB::PioRead(uint16_t offset, uint8_t size, uint32_t* value) {
    uint8_t out = value_;
    if (pit_ && pit_->IsChannel2OutputHigh())
        out |= 0x20;
    else
        out &= ~0x20;
    *value = out;
}

void SystemControlB::PioWrite(uint16_t offset, uint8_t size, uint32_t value) {
    value_ = static_cast<uint8_t>(value);
}
