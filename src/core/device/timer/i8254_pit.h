#pragma once

#include "core/device/device.h"
#include <functional>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

class VmIoLoop;

// Minimal i8254 PIT emulation for Linux boot timing calibration.
// Uses host RDTSC for timing to stay perfectly synchronized with guest TSC
// (which is the same physical counter under WHVP).
class I8254Pit : public Device {
public:
    static constexpr uint16_t kBasePort = 0x40;
    static constexpr uint16_t kRegCount = 4;

    using IrqFunc = std::function<void()>;

    I8254Pit();
    ~I8254Pit();

    void SetIrqCallback(IrqFunc cb) { irq_callback_ = std::move(cb); }

    // Wire the per-VM libuv event loop used to drive channel-0 IRQ 0 ticks.
    // Must be called during machine setup before the guest programs the PIT.
    // Null disables the timer; the count-register view via RDTSC polling
    // keeps working (that's all Linux actually needs after LAPIC timer
    // calibration anyway).
    void SetIoLoop(VmIoLoop* loop) { io_loop_ = loop; }

    void PioRead(uint16_t offset, uint8_t size, uint32_t* value) override;
    void PioWrite(uint16_t offset, uint8_t size, uint32_t value) override;

    bool IsChannel2OutputHigh() const;

private:
    static constexpr double kPitFrequencyHz = 1193182.0;

    static uint64_t MeasureTscFrequency();

    uint64_t tsc_freq_;
    IrqFunc irq_callback_;

    struct Channel {
        uint16_t reload = 0;
        uint8_t  mode = 0;
        uint8_t  access = 0;      // 1=lo, 2=hi, 3=lo/hi
        bool     latch_pending = false;
        uint16_t latched_value = 0;
        bool     write_lo_next = true;
        bool     read_lo_next = true;
        bool     armed = false;
        uint64_t start_tsc = 0;
    };

    Channel channels_[3]{};

    uint64_t ElapsedPitTicks(int ch) const;
    uint16_t CurrentCount(int ch) const;
    bool OutputHigh(int ch) const;

    void ArmChannel0Timer();
    void StopChannel0Timer();

    VmIoLoop* io_loop_ = nullptr;
    uint64_t ch0_timer_id_ = 0;
};

// System Control Port B (port 0x61) - gates PIT channel 2.
class SystemControlB : public Device {
public:
    static constexpr uint16_t kPort = 0x61;
    static constexpr uint16_t kRegCount = 1;

    void SetPit(I8254Pit* pit) { pit_ = pit; }

    void PioRead(uint16_t offset, uint8_t size, uint32_t* value) override;
    void PioWrite(uint16_t offset, uint8_t size, uint32_t value) override;

private:
    I8254Pit* pit_ = nullptr;
    uint8_t value_ = 0;
};
