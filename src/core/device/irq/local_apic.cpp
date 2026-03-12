#include "core/device/irq/local_apic.h"
#include "core/vmm/types.h"

thread_local uint32_t LocalApic::current_cpu_ = 0;

LocalApic::LocalApic() = default;

LocalApic::~LocalApic() {
    Stop();
}

void LocalApic::Init(uint32_t cpu_count) {
    cpu_count_ = cpu_count;
    for (uint32_t i = 0; i < cpu_count && i < kMaxCpus; i++) {
        cpus_[i].id = i;
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
#ifdef __APPLE__
    if (dispatch_queue_) return;
    dispatch_queue_ = dispatch_queue_create("lapic.timer", DISPATCH_QUEUE_SERIAL);
#endif
}

void LocalApic::Stop() {
#ifdef __APPLE__
    for (uint32_t i = 0; i < cpu_count_ && i < kMaxCpus; i++) {
        StopTimer(i);
    }
    if (dispatch_queue_) {
        dispatch_release(dispatch_queue_);
        dispatch_queue_ = nullptr;
    }
#endif
}

uint32_t LocalApic::GetDivider(const CpuApicState& cpu) const {
    uint32_t v = (cpu.timer_div_conf & 3) | ((cpu.timer_div_conf >> 1) & 4);
    if (v == 7) return 1;
    return 2u << v;
}

void LocalApic::StopTimer(uint32_t cpu_idx) {
#ifdef __APPLE__
    if (cpu_idx >= kMaxCpus) return;
    auto& cpu = cpus_[cpu_idx];
    if (cpu.dispatch_timer) {
        dispatch_source_cancel(cpu.dispatch_timer);
        dispatch_release(cpu.dispatch_timer);
        cpu.dispatch_timer = nullptr;
    }
#endif
}

void LocalApic::ArmTimer(uint32_t cpu_idx) {
#ifdef __APPLE__
    if (cpu_idx >= kMaxCpus) return;
    StopTimer(cpu_idx);
    auto& cpu = cpus_[cpu_idx];
    if (!dispatch_queue_ || cpu.timer_init_count == 0) return;

    uint64_t period_ns = (uint64_t)cpu.timer_init_count * cpu.timer_divider;
    if (period_ns == 0) return;

    cpu.timer_load_time_ns = GetTimeNs();
    cpu.timer_period_ns = period_ns;

    uint32_t mode = (cpu.lvt_timer >> 17) & 0x3;

    cpu.dispatch_timer = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_queue_);

    if (mode == kTimerPeriodic) {
        dispatch_source_set_timer(cpu.dispatch_timer,
            dispatch_time(DISPATCH_TIME_NOW, (int64_t)period_ns),
            period_ns, 0);
    } else {
        dispatch_source_set_timer(cpu.dispatch_timer,
            dispatch_time(DISPATCH_TIME_NOW, (int64_t)period_ns),
            DISPATCH_TIME_FOREVER, 0);
    }

    uint32_t ci = cpu_idx;
    dispatch_source_set_event_handler(cpu.dispatch_timer, ^{
        FireTimer(ci);
    });

    dispatch_resume(cpu.dispatch_timer);
#endif
}

void LocalApic::FireTimer(uint32_t cpu_idx) {
    uint32_t vector = 0;
    uint32_t target_cpu = 0;
    bool should_inject = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cpu_idx >= kMaxCpus) return;
        auto& cpu = cpus_[cpu_idx];

        if (cpu.timer_init_count == 0 || (cpu.lvt_timer & 0x10000)) return;

        vector = cpu.lvt_timer & 0xFF;
        target_cpu = cpu_idx;
        should_inject = (vector > 0 && inject_irq_);

        uint32_t mode = (cpu.lvt_timer >> 17) & 0x3;
        if (mode == kTimerOneShot) {
            cpu.timer_init_count = 0;
        } else {
            cpu.timer_load_time_ns += cpu.timer_period_ns;
        }
    }

    if (should_inject) {
        inject_irq_(vector, target_cpu);
    }
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
    // mutex_ released — safe to call dispatch APIs and external callbacks

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
