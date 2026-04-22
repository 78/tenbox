#include "core/device/irq/local_apic.h"
#include "core/vmm/types.h"
#include "core/vmm/vm_io_loop.h"

thread_local uint32_t LocalApic::current_cpu_ = 0;

LocalApic::LocalApic() = default;

LocalApic::~LocalApic() {
    Stop();
}

void LocalApic::Init(uint32_t cpu_count) {
    cpu_count_ = cpu_count;
    for (uint32_t i = 0; i < cpu_count && i < kMaxCpus; i++) {
        cpus_[i].id = i;
        cpus_[i].hires_timer = MakeHiResTimer(io_loop_);
    }
}

void LocalApic::SetApicIds(const std::vector<uint32_t>& ids) {
    for (uint32_t i = 0; i < ids.size() && i < kMaxCpus; i++) {
        cpus_[i].id = ids[i];
    }
}

void LocalApic::SetCurrentCpu(uint32_t cpu_index) {
    current_cpu_ = cpu_index;
}

LocalApic::CpuApicState& LocalApic::CurrentCpu() {
    uint32_t idx = current_cpu_;
    if (idx >= kMaxCpus) idx = 0;
    return cpus_[idx];
}

void LocalApic::Start() {
    // No-op. VmIoLoop is expected to already be running by the time the
    // guest arms any LAPIC timer; ArmTimer posts onto it directly.
}

void LocalApic::Stop() {
    for (uint32_t i = 0; i < cpu_count_ && i < kMaxCpus; i++) {
        StopTimer(i);
    }
}

uint32_t LocalApic::GetDivider(const CpuApicState& cpu) const {
    uint32_t v = (cpu.timer_div_conf & 3) | ((cpu.timer_div_conf >> 1) & 4);
    if (v == 7) return 1;
    return 2u << v;
}

void LocalApic::StopTimer(uint32_t cpu_idx) {
    if (cpu_idx >= kMaxCpus) return;
    auto& cpu = cpus_[cpu_idx];
    // Note: HiResTimer::Stop is thread-safe and re-entrant. We clear the
    // armed flag so FireTimer (which may be executing concurrently on the
    // timer thread) stops re-arming itself.
    cpu.timer_armed = false;
    if (cpu.hires_timer) cpu.hires_timer->Stop();
}

void LocalApic::ArmTimer(uint32_t cpu_idx) {
    if (cpu_idx >= kMaxCpus) return;
    auto& cpu = cpus_[cpu_idx];
    if (!cpu.hires_timer || cpu.timer_init_count == 0) return;

    // Stop any prior schedule. HiResTimer::Arm() also does this internally,
    // but resetting timer_armed first keeps a concurrent FireTimer from
    // re-arming between Stop() and the new Arm().
    cpu.timer_armed = false;
    cpu.hires_timer->Stop();

    // Bus frequency is 1 GHz (kBusFreqHz), so one tick = 1 ns.
    // Period in ns = init_count * divider.
    uint64_t period_ns = static_cast<uint64_t>(cpu.timer_init_count) * cpu.timer_divider;
    if (period_ns == 0) return;

    // Floor at 50 μs so we don't spin the host thread on pathologically small
    // init_count values. Linux normally programs ~150 μs on modern configs
    // (init_count=0x2580, div=16 → 153.6 μs). Going below 50 μs would give
    // tick rates beyond what any reasonable guest needs and starve the
    // callback thread.
    if (period_ns < 50'000) period_ns = 50'000;

    cpu.timer_load_time_ns = GetTimeNs();
    cpu.timer_period_ns = period_ns;
    cpu.timer_armed = true;

    cpu.hires_timer->Arm(period_ns, [this, cpu_idx]() -> uint64_t {
        return FireTimer(cpu_idx);
    });
}

uint64_t LocalApic::FireTimer(uint32_t cpu_idx) {
    uint32_t vector = 0;
    uint32_t target_cpu = 0;
    bool should_inject = false;
    uint64_t next_ns = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cpu_idx >= kMaxCpus) return 0;
        auto& cpu = cpus_[cpu_idx];

        // If StopTimer raced us between the wait returning and here,
        // don't re-arm. The mask bit (LVT bit 16) is also a stop signal.
        if (!cpu.timer_armed ||
            cpu.timer_init_count == 0 ||
            (cpu.lvt_timer & 0x10000)) {
            cpu.timer_armed = false;
            return 0;
        }

        vector = cpu.lvt_timer & 0xFF;
        target_cpu = cpu_idx;
        should_inject = (vector > 0 && inject_irq_);

        uint32_t mode = (cpu.lvt_timer >> 17) & 0x3;
        if (mode == kTimerOneShot) {
            cpu.timer_init_count = 0;
            cpu.timer_armed = false;
            next_ns = 0;
        } else {
            cpu.timer_load_time_ns += cpu.timer_period_ns;
            next_ns = cpu.timer_period_ns;
        }
    }

    if (should_inject) {
        inject_irq_(vector, target_cpu);
    }
    return next_ns;
}

uint64_t LocalApic::GetTimeNs() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
}

void LocalApic::MmioRead(uint64_t offset, uint8_t /*size*/, uint64_t* value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& cpu = CurrentCpu();
    uint32_t v = 0;

    switch (static_cast<uint32_t>(offset)) {
    case kRegId:            v = cpu.id << 24; break;
    case kRegVersion:       v = 0x00050014; break;
    case kRegTPR:           v = cpu.tpr; break;
    case kRegLogicalDest:   v = cpu.logical_dest; break;
    case kRegDestFormat:    v = cpu.dest_format; break;
    case kRegSpurious:      v = cpu.spurious; break;
    case kRegErrorStatus:   v = cpu.error_status; break;
    case kRegICR_Low:       v = cpu.icr_low; break;
    case kRegICR_High:      v = cpu.icr_high; break;
    case kRegLvtTimer:      v = cpu.lvt_timer; break;
    case kRegLvtThermal:    v = cpu.lvt_thermal; break;
    case kRegLvtPerfmon:    v = cpu.lvt_perfmon; break;
    case kRegLvtLint0:      v = cpu.lvt_lint0; break;
    case kRegLvtLint1:      v = cpu.lvt_lint1; break;
    case kRegLvtError:      v = cpu.lvt_error; break;
    case kRegTimerInitCount: v = cpu.timer_init_count; break;
    case kRegTimerCurCount: {
        if (cpu.timer_init_count == 0) {
            v = 0;
        } else {
            uint64_t now_ns = GetTimeNs();
            uint64_t elapsed_ns = (now_ns > cpu.timer_load_time_ns) ?
                                   now_ns - cpu.timer_load_time_ns : 0;
            uint64_t elapsed_ticks = elapsed_ns / cpu.timer_divider;
            uint32_t mode = (cpu.lvt_timer >> 17) & 0x3;
            if (mode == kTimerPeriodic) {
                if (cpu.timer_init_count > 0) {
                    v = cpu.timer_init_count -
                        (uint32_t)(elapsed_ticks % ((uint64_t)cpu.timer_init_count));
                }
            } else {
                v = (elapsed_ticks >= cpu.timer_init_count) ?
                    0 : cpu.timer_init_count - (uint32_t)elapsed_ticks;
            }
        }
        break;
    }
    case kRegTimerDivConf:   v = cpu.timer_div_conf; break;
    default:
        break;
    }

    *value = v;
}

// Deferred timer action after MmioWrite releases mutex_
struct TimerAction {
    enum { kNone, kArm, kStop } type = kNone;
    uint32_t cpu_idx = 0;
};

// NOTE: kRegEOI, kRegIRR_Base and kRegISR_Base are intentionally *not*
// tracked as register-level state here. The two injection backends we talk
// to — HVF (VMCS VMENTRY_IRQ_INFO) and the soft-APIC path on WHPX
// (WhvpVCpu::pending_irqs_ + WHvRegisterPendingInterruption) — each keep
// their own per-vCPU FIFO that effectively plays the IRR's role at the
// granularity the hypervisor needs. EOI is likewise handled inside the
// backend's injection state machine (HVF auto-clears the inject slot;
// WHPX pops the FIFO head into PendingInterruption on the next entry).
// For the hard path on 1809+, WHPX's in-partition xAPIC maintains the
// real IRR/ISR for us. Future work: add real IRR/ISR bit tracking so we
// can honour TPR / priority arbitration across the FIFO head instead of
// delivering strictly in-order (see HvfVCpu comment for the same TODO).
void LocalApic::MmioWrite(uint64_t offset, uint8_t /*size*/, uint64_t value) {
    TimerAction timer_action;

    // IPI callback args — captured inside lock, called outside
    bool do_ipi = false;
    uint32_t ipi_vector = 0;
    uint32_t ipi_dest = 0;
    uint8_t ipi_shorthand = 0;
    bool ipi_dest_logical = false;

    bool do_init = false;
    uint32_t init_targets[kMaxCpus];
    uint32_t init_count = 0;

    bool do_sipi = false;
    uint32_t sipi_targets[kMaxCpus];
    uint8_t sipi_vectors[kMaxCpus];
    uint32_t sipi_count = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& cpu = CurrentCpu();
        uint32_t ci = current_cpu_;
        uint32_t v = static_cast<uint32_t>(value);

        switch (static_cast<uint32_t>(offset)) {
        case kRegId:            cpu.id = (v >> 24) & 0xFF; break;
        case kRegTPR:           cpu.tpr = v & 0xFF; break;
        case kRegEOI:           break;
        case kRegLogicalDest:   cpu.logical_dest = v; break;
        case kRegDestFormat:    cpu.dest_format = v; break;
        case kRegSpurious: {
            uint32_t old = cpu.spurious;
            cpu.spurious = v;
            if ((old & 0x100) && !(v & 0x100)) {
                timer_action = {TimerAction::kStop, ci};
            } else if (!(old & 0x100) && (v & 0x100)) {
                if (((cpu.lvt_timer >> 17) & 0x3) == kTimerPeriodic && cpu.timer_init_count != 0)
                    timer_action = {TimerAction::kArm, ci};
            }
            break;
        }
        case kRegErrorStatus:   cpu.error_status = 0; break;
        case kRegICR_Low: {
            cpu.icr_low = v;
            uint8_t delivery_mode = (v >> 8) & 0x7;
            uint8_t dest_shorthand = (v >> 18) & 0x3;
            uint8_t vector = v & 0xFF;
            uint32_t dest = (cpu.icr_high >> 24) & 0xFF;

            if (delivery_mode == 5) {
                do_init = true;
                if (dest_shorthand == 0) {
                    init_targets[0] = dest;
                    init_count = 1;
                } else if (dest_shorthand == 3) {
                    for (uint32_t i = 0; i < cpu_count_; i++) {
                        if (i != ci) {
                            init_targets[init_count++] = i;
                        }
                    }
                }
            } else if (delivery_mode == 6) {
                do_sipi = true;
                if (dest_shorthand == 0) {
                    sipi_targets[0] = dest;
                    sipi_vectors[0] = vector;
                    sipi_count = 1;
                } else if (dest_shorthand == 3) {
                    for (uint32_t i = 0; i < cpu_count_; i++) {
                        if (i != ci) {
                            sipi_targets[sipi_count] = i;
                            sipi_vectors[sipi_count] = vector;
                            sipi_count++;
                        }
                    }
                }
            } else if (delivery_mode == 0) {
                bool dest_logical = (v >> 11) & 1;
                do_ipi = true;
                ipi_vector = vector;
                ipi_shorthand = dest_shorthand;
                if (dest_logical && dest_shorthand == 0) {
                    // Flat-model logical destination: dest is a bitmask, bit N = APIC ID N.
                    // Send to each matching CPU individually.
                    ipi_dest = dest;
                    // We'll resolve below after releasing the lock.
                } else {
                    ipi_dest = dest;
                }
                ipi_dest_logical = dest_logical;
            }
            break;
        }
        case kRegICR_High:      cpu.icr_high = v; break;
        case kRegLvtTimer:
            cpu.lvt_timer = v;
            if (cpu.timer_init_count != 0 && !(v & 0x10000)) {
                timer_action = {TimerAction::kArm, ci};
            } else {
                timer_action = {TimerAction::kStop, ci};
            }
            break;
        case kRegLvtThermal:    cpu.lvt_thermal = v; break;
        case kRegLvtPerfmon:    cpu.lvt_perfmon = v; break;
        case kRegLvtLint0:      cpu.lvt_lint0 = v; break;
        case kRegLvtLint1:      cpu.lvt_lint1 = v; break;
        case kRegLvtError:      cpu.lvt_error = v; break;
        case kRegTimerInitCount:
            cpu.timer_init_count = v;
            cpu.timer_load_time_ns = GetTimeNs();
            if (v != 0 && !(cpu.lvt_timer & 0x10000)) {
                timer_action = {TimerAction::kArm, ci};
            } else {
                timer_action = {TimerAction::kStop, ci};
            }
            break;
        case kRegTimerDivConf:
            cpu.timer_div_conf = v & 0xB;
            cpu.timer_divider = GetDivider(cpu);
            break;
        default:
            break;
        }
    }
    // mutex_ released — safe to call VmIoLoop APIs and external callbacks.

    if (timer_action.type == TimerAction::kArm) {
        ArmTimer(timer_action.cpu_idx);
    } else if (timer_action.type == TimerAction::kStop) {
        StopTimer(timer_action.cpu_idx);
    }

    if (do_init && init_callback_) {
        for (uint32_t i = 0; i < init_count; i++) {
            init_callback_(init_targets[i]);
        }
    }
    if (do_sipi && sipi_callback_) {
        for (uint32_t i = 0; i < sipi_count; i++) {
            sipi_callback_(sipi_targets[i], sipi_vectors[i]);
        }
    }
    if (do_ipi && ipi_callback_) {
        if (ipi_dest_logical && ipi_shorthand == 0) {
            // Flat-model logical destination: bitmask, bit N = APIC ID N.
            for (uint32_t i = 0; i < cpu_count_; i++) {
                if (ipi_dest & (1u << i)) {
                    ipi_callback_(ipi_vector, i, 0);
                }
            }
        } else {
            ipi_callback_(ipi_vector, ipi_dest, ipi_shorthand);
        }
    }
}
