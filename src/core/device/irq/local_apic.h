#pragma once

#include "core/device/device.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif

class LocalApic : public Device {
public:
    static constexpr uint64_t kBaseAddress = 0xFEE00000;
    static constexpr uint64_t kSize = 0x1000;
    static constexpr uint32_t kMaxCpus = 64;

    static constexpr uint64_t kBusFreqHz = 1000000000ULL;

    using IrqInjectFunc = std::function<void(uint32_t vector, uint32_t cpu)>;
    using SipiFunc = std::function<void(uint32_t target_cpu, uint8_t vector)>;
    using InitFunc = std::function<void(uint32_t target_cpu)>;
    using IpiFunc = std::function<void(uint32_t vector, uint32_t dest, uint8_t shorthand)>;

    LocalApic();
    ~LocalApic();

    void Init(uint32_t cpu_count);
    void SetApicIds(const std::vector<uint32_t>& ids);

    void SetIrqInjectCallback(IrqInjectFunc cb) { inject_irq_ = std::move(cb); }
    void SetSipiCallback(SipiFunc cb) { sipi_callback_ = std::move(cb); }
    void SetInitCallback(InitFunc cb) { init_callback_ = std::move(cb); }
    void SetIpiCallback(IpiFunc cb) { ipi_callback_ = std::move(cb); }

    static void SetCurrentCpu(uint32_t cpu_index);
    static uint32_t GetCurrentCpu() { return current_cpu_; }

    void MmioRead(uint64_t offset, uint8_t size, uint64_t* value) override;
    void MmioWrite(uint64_t offset, uint8_t size, uint64_t value) override;

    void Start();
    void Stop();

private:
    static constexpr uint32_t kRegId              = 0x020;
    static constexpr uint32_t kRegVersion         = 0x030;
    static constexpr uint32_t kRegTPR             = 0x080;
    static constexpr uint32_t kRegEOI             = 0x0B0;
    static constexpr uint32_t kRegLogicalDest     = 0x0D0;
    static constexpr uint32_t kRegDestFormat      = 0x0E0;
    static constexpr uint32_t kRegSpurious        = 0x0F0;
    static constexpr uint32_t kRegISR_Base        = 0x100;
    static constexpr uint32_t kRegTMR_Base        = 0x180;
    static constexpr uint32_t kRegIRR_Base        = 0x200;
    static constexpr uint32_t kRegErrorStatus     = 0x280;
    static constexpr uint32_t kRegICR_Low         = 0x300;
    static constexpr uint32_t kRegICR_High        = 0x310;
    static constexpr uint32_t kRegLvtTimer        = 0x320;
    static constexpr uint32_t kRegLvtThermal      = 0x330;
    static constexpr uint32_t kRegLvtPerfmon      = 0x340;
    static constexpr uint32_t kRegLvtLint0        = 0x350;
    static constexpr uint32_t kRegLvtLint1        = 0x360;
    static constexpr uint32_t kRegLvtError        = 0x370;
    static constexpr uint32_t kRegTimerInitCount  = 0x380;
    static constexpr uint32_t kRegTimerCurCount   = 0x390;
    static constexpr uint32_t kRegTimerDivConf    = 0x3E0;

    static constexpr uint32_t kTimerOneShot  = 0;
    static constexpr uint32_t kTimerPeriodic = 1;

    struct CpuApicState {
        uint32_t id = 0;
        uint32_t tpr = 0;
        uint32_t spurious = 0xFF;
        uint32_t logical_dest = 0;
        uint32_t dest_format = 0xFFFFFFFF;
        uint32_t error_status = 0;
        uint32_t icr_low = 0;
        uint32_t icr_high = 0;

        uint32_t lvt_timer = 0x00010000;
        uint32_t lvt_thermal = 0x00010000;
        uint32_t lvt_perfmon = 0x00010000;
        uint32_t lvt_lint0 = 0x00010000;
        uint32_t lvt_lint1 = 0x00010000;
        uint32_t lvt_error = 0x00010000;

        uint32_t timer_init_count = 0;
        uint32_t timer_div_conf = 0;
        uint32_t timer_divider = 2;

        uint64_t timer_load_time_ns = 0;
        uint64_t timer_period_ns = 0;

#ifdef __APPLE__
        dispatch_source_t dispatch_timer = nullptr;
#endif
    };

    static thread_local uint32_t current_cpu_;
    uint32_t cpu_count_ = 1;

    mutable std::mutex mutex_;
    IrqInjectFunc inject_irq_;
    SipiFunc sipi_callback_;
    InitFunc init_callback_;
    IpiFunc ipi_callback_;

    CpuApicState cpus_[kMaxCpus];

    CpuApicState& CurrentCpu();
    uint32_t GetDivider(const CpuApicState& cpu) const;
    uint64_t GetTimeNs() const;
    void ArmTimer(uint32_t cpu_idx);
    void StopTimer(uint32_t cpu_idx);
    void FireTimer(uint32_t cpu_idx);

#ifdef __APPLE__
    dispatch_queue_t dispatch_queue_ = nullptr;
#endif
};
