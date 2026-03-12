#include "platform/macos/hypervisor/aarch64/hvf_vcpu.h"
#include "platform/macos/hypervisor/aarch64/hvf_mmio_decode.h"
#include "core/vmm/types.h"
#include <chrono>
#include <cstdio>

namespace hvf {

HvfVCpu::ExitStats HvfVCpu::s_stats_;
std::atomic<bool> HvfVCpu::s_stats_enabled_{false};

// Exception Class values from ARM Architecture Reference Manual
static constexpr uint8_t kEcWfiWfe    = 0x01;
static constexpr uint8_t kEcHvc64     = 0x16;
static constexpr uint8_t kEcSmc64     = 0x17;
static constexpr uint8_t kEcSysReg    = 0x18;
static constexpr uint8_t kEcDabtLower = 0x24;
static constexpr uint8_t kEcDabtCurr  = 0x25;
static constexpr uint8_t kEcBrk       = 0x3C;

HvfVCpu::~HvfVCpu() {
    if (created_) {
        hv_vcpu_destroy(vcpu_);
    }
}

std::unique_ptr<HvfVCpu> HvfVCpu::Create(uint32_t index, AddressSpace* addr_space) {
    auto vcpu = std::unique_ptr<HvfVCpu>(new HvfVCpu());
    vcpu->index_ = index;
    vcpu->addr_space_ = addr_space;

    hv_vcpu_config_t config = hv_vcpu_config_create();

    hv_return_t ret = hv_vcpu_create(&vcpu->vcpu_, &vcpu->vcpu_exit_, config);
    if (ret != HV_SUCCESS) {
        LOG_ERROR("hvf: hv_vcpu_create(%u) failed: %d", index, (int)ret);
        return nullptr;
    }
    vcpu->created_ = true;

    uint64_t mpidr = static_cast<uint64_t>(index) & 0xFF;
    hv_vcpu_set_sys_reg(vcpu->vcpu_, HV_SYS_REG_MPIDR_EL1, mpidr);

    hv_vcpu_set_trap_debug_exceptions(vcpu->vcpu_, true);

    uint32_t vtimer_intid = 0;
    if (__builtin_available(macOS 15.0, *)) {
        hv_return_t gic_ret = hv_gic_get_intid(HV_GIC_INT_EL1_VIRTUAL_TIMER, &vtimer_intid);
        if (gic_ret == HV_SUCCESS) {
            vcpu->vtimer_intid_ = vtimer_intid;
        } else {
            vcpu->vtimer_intid_ = 27;
            LOG_WARN("hvf: vCPU %u failed to get vtimer INTID (ret=%d), using default 27",
                     index, (int)gic_ret);
        }
    } else {
        vcpu->vtimer_intid_ = 27;
    }

    LOG_INFO("hvf: vCPU %u created (vtimer INTID=%u)", index, vcpu->vtimer_intid_);
    return vcpu;
}

VCpuExitAction HvfVCpu::RunOnce() {
    if (vtimer_masked_) {
        uint64_t ctl = 0;
        hv_vcpu_get_sys_reg(vcpu_, HV_SYS_REG_CNTV_CTL_EL0, &ctl);
        bool irq_asserted = (ctl & 0x7) == 0x5;
        if (!irq_asserted) {
            hv_vcpu_set_vtimer_mask(vcpu_, false);
            vtimer_masked_ = false;
        }
    }

    hv_return_t ret = hv_vcpu_run(vcpu_);
    if (ret != HV_SUCCESS) {
        LOG_ERROR("hvf: hv_vcpu_run(%u) failed: %d", index_, (int)ret);
        return VCpuExitAction::kError;
    }

    if (s_stats_enabled_.load(std::memory_order_relaxed)) {
        s_stats_.total.fetch_add(1, std::memory_order_relaxed);
        PrintExitStats();
    }

    switch (vcpu_exit_->reason) {
    case HV_EXIT_REASON_EXCEPTION:
    {
        s_stats_.exception.fetch_add(1, std::memory_order_relaxed);
        return HandleException();
    }

    case HV_EXIT_REASON_CANCELED:
    {
        s_stats_.canceled.fetch_add(1, std::memory_order_relaxed);
        return VCpuExitAction::kContinue;
    }

    case HV_EXIT_REASON_VTIMER_ACTIVATED:
    {
        s_stats_.vtimer_activated.fetch_add(1, std::memory_order_relaxed);
        vtimer_masked_ = true;

        uint32_t ppi_bit = 1u << vtimer_intid_;
        if (__builtin_available(macOS 15.0, *)) {
            hv_return_t gic_ret = hv_gic_set_redistributor_reg(vcpu_,
                HV_GIC_REDISTRIBUTOR_REG_GICR_ISPENDR0, ppi_bit);
            if (gic_ret != HV_SUCCESS) {
                hv_vcpu_set_pending_interrupt(vcpu_, HV_INTERRUPT_TYPE_IRQ, true);
            }
        } else {
            hv_vcpu_set_pending_interrupt(vcpu_, HV_INTERRUPT_TYPE_IRQ, true);
        }

        return VCpuExitAction::kContinue;
    }

    default:
        LOG_ERROR("hvf: vCPU %u unexpected exit reason: %d",
                  index_, (int)vcpu_exit_->reason);
        return VCpuExitAction::kError;
    }
}

void HvfVCpu::CancelRun() {
    hv_vcpus_exit(&vcpu_, 1);
}

void HvfVCpu::OnStartup(const VCpuStartupState& state) {
    SetupSecondaryCpu(state.entry_addr, state.context_id);
}

bool HvfVCpu::SetupBootRegisters(uint8_t* /*ram*/) {
    return true;
}

bool HvfVCpu::SetupAarch64Boot(uint64_t entry_pc, uint64_t fdt_addr) {
    hv_return_t ret;

    ret = hv_vcpu_set_reg(vcpu_, HV_REG_PC, entry_pc);
    if (ret != HV_SUCCESS) {
        LOG_ERROR("hvf: failed to set PC: %d", (int)ret);
        return false;
    }

    ret = hv_vcpu_set_reg(vcpu_, HV_REG_X0, fdt_addr);
    if (ret != HV_SUCCESS) {
        LOG_ERROR("hvf: failed to set X0: %d", (int)ret);
        return false;
    }

    hv_vcpu_set_reg(vcpu_, HV_REG_X1, 0);
    hv_vcpu_set_reg(vcpu_, HV_REG_X2, 0);
    hv_vcpu_set_reg(vcpu_, HV_REG_X3, 0);

    ret = hv_vcpu_set_reg(vcpu_, HV_REG_CPSR, 0x3C5);
    if (ret != HV_SUCCESS) {
        LOG_ERROR("hvf: failed to set CPSR: %d", (int)ret);
        return false;
    }

    LOG_INFO("hvf: vCPU %u ARM64 boot: PC=0x%llx, X0(FDT)=0x%llx",
             index_, (unsigned long long)entry_pc,
             (unsigned long long)fdt_addr);
    return true;
}

bool HvfVCpu::SetupSecondaryCpu(uint64_t entry_pc, uint64_t context_id) {
    hv_vcpu_set_reg(vcpu_, HV_REG_PC, entry_pc);
    hv_vcpu_set_reg(vcpu_, HV_REG_X0, context_id);
    hv_vcpu_set_reg(vcpu_, HV_REG_CPSR, 0x3C5);
    LOG_INFO("hvf: vCPU %u secondary boot: PC=0x%llx, X0=0x%llx",
             index_, (unsigned long long)entry_pc,
             (unsigned long long)context_id);
    return true;
}

VCpuExitAction HvfVCpu::HandleException() {
    uint64_t syndrome = vcpu_exit_->exception.syndrome;
    uint8_t ec = (syndrome >> 26) & 0x3F;

    switch (ec) {
    case kEcWfiWfe:
        s_stats_.ec_wfi_wfe.fetch_add(1, std::memory_order_relaxed);
        return VCpuExitAction::kHalt;

    case kEcDabtLower:
        s_stats_.ec_dabt_lower.fetch_add(1, std::memory_order_relaxed);
        return HandleDataAbort(syndrome);

    case kEcDabtCurr:
        s_stats_.ec_dabt_curr.fetch_add(1, std::memory_order_relaxed);
        return HandleDataAbort(syndrome);

    case kEcHvc64:
        s_stats_.ec_hvc64.fetch_add(1, std::memory_order_relaxed);
        return HandleHvc();

    case kEcSmc64:
        s_stats_.ec_smc64.fetch_add(1, std::memory_order_relaxed);
        return HandleHvc();

    case kEcSysReg:
        {
            s_stats_.ec_sysreg.fetch_add(1, std::memory_order_relaxed);
            uint64_t pc;
            hv_vcpu_get_reg(vcpu_, HV_REG_PC, &pc);
            hv_vcpu_set_reg(vcpu_, HV_REG_PC, pc + 4);
        }
        return VCpuExitAction::kContinue;

    case kEcBrk:
    {
        s_stats_.ec_brk.fetch_add(1, std::memory_order_relaxed);
        uint16_t imm = syndrome & 0xFFFF;
        LOG_WARN("hvf: vCPU %u BRK #%u (syndrome=0x%llx) — skipping",
                 index_, imm, (unsigned long long)syndrome);
        uint64_t pc;
        hv_vcpu_get_reg(vcpu_, HV_REG_PC, &pc);
        hv_vcpu_set_reg(vcpu_, HV_REG_PC, pc + 4);
        return VCpuExitAction::kContinue;
    }

    default:
        s_stats_.ec_other.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR("hvf: vCPU %u unhandled EC=0x%02x (syndrome=0x%llx, "
                  "VA=0x%llx, IPA=0x%llx)",
                  index_, ec,
                  (unsigned long long)syndrome,
                  (unsigned long long)vcpu_exit_->exception.virtual_address,
                  (unsigned long long)vcpu_exit_->exception.physical_address);
        return VCpuExitAction::kError;
    }
}

VCpuExitAction HvfVCpu::HandleHvc() {
    uint64_t x0, x1, x2, x3;
    hv_vcpu_get_reg(vcpu_, HV_REG_X0, &x0);
    hv_vcpu_get_reg(vcpu_, HV_REG_X1, &x1);
    hv_vcpu_get_reg(vcpu_, HV_REG_X2, &x2);
    hv_vcpu_get_reg(vcpu_, HV_REG_X3, &x3);

    uint32_t func_id = static_cast<uint32_t>(x0);

    switch (func_id) {
    case kPsciVersion:
        s_stats_.hvc_version.fetch_add(1, std::memory_order_relaxed);
        hv_vcpu_set_reg(vcpu_, HV_REG_X0, 0x00010000);
        return VCpuExitAction::kContinue;

    case kPsciFeaturesCall:
    {
        s_stats_.hvc_features.fetch_add(1, std::memory_order_relaxed);
        uint32_t queried = static_cast<uint32_t>(x1);
        if (queried == kPsciCpuOn64 || queried == kPsciCpuOff ||
            queried == kPsciSystemOff || queried == kPsciSystemReset ||
            queried == kPsciVersion) {
            hv_vcpu_set_reg(vcpu_, HV_REG_X0, 0);
        } else {
            hv_vcpu_set_reg(vcpu_, HV_REG_X0, static_cast<uint64_t>(-1LL));
        }
        return VCpuExitAction::kContinue;
    }

    case kPsciCpuOn64:
    {
        s_stats_.hvc_cpu_on.fetch_add(1, std::memory_order_relaxed);
        PsciCpuOnRequest req;
        req.target_cpu = static_cast<uint32_t>(x1 & 0xFF);
        req.entry_addr = x2;
        req.context_id = x3;

        int result = -2;
        if (psci_cpu_on_cb_) {
            result = psci_cpu_on_cb_(req);
        }
        hv_vcpu_set_reg(vcpu_, HV_REG_X0, static_cast<uint64_t>(static_cast<int64_t>(result)));
        return VCpuExitAction::kContinue;
    }

    case kPsciCpuOff:
        s_stats_.hvc_cpu_off.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO("hvf: vCPU %u PSCI CPU_OFF", index_);
        return VCpuExitAction::kHalt;

    case kPsciSystemOff:
        s_stats_.hvc_system_off.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO("hvf: PSCI SYSTEM_OFF from vCPU %u", index_);
        if (psci_shutdown_cb_) psci_shutdown_cb_();
        return VCpuExitAction::kShutdown;

    case kPsciSystemReset:
        s_stats_.hvc_system_reset.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO("hvf: PSCI SYSTEM_RESET from vCPU %u", index_);
        if (psci_reboot_cb_) psci_reboot_cb_();
        else if (psci_shutdown_cb_) psci_shutdown_cb_();
        return VCpuExitAction::kShutdown;

    default:
        s_stats_.hvc_unknown.fetch_add(1, std::memory_order_relaxed);
        hv_vcpu_set_reg(vcpu_, HV_REG_X0, static_cast<uint64_t>(-1LL));
        return VCpuExitAction::kContinue;
    }
}

VCpuExitAction HvfVCpu::HandleDataAbort(uint64_t syndrome) {
    uint64_t gpa = vcpu_exit_->exception.physical_address;

    MmioDecodeResult decode{};
    bool isv = (syndrome >> 24) & 1;

    if (isv) {
        s_stats_.dabt_isv_valid.fetch_add(1, std::memory_order_relaxed);
        decode.is_write = (syndrome >> 6) & 1;
        uint8_t sas = (syndrome >> 22) & 3;
        decode.access_size = 1u << sas;
        decode.reg = (syndrome >> 16) & 0x1F;
        decode.is_pair = false;

        if (decode.is_write && decode.reg < 31) {
            hv_vcpu_get_reg(vcpu_, static_cast<hv_reg_t>(HV_REG_X0 + decode.reg),
                            &decode.write_value);
        }
    } else {
        s_stats_.dabt_isv_invalid.fetch_add(1, std::memory_order_relaxed);
        uint64_t pc;
        hv_vcpu_get_reg(vcpu_, HV_REG_PC, &pc);

        uint64_t far_el2 = vcpu_exit_->exception.virtual_address;
        (void)far_el2;

        LOG_WARN("hvf: vCPU %u DABT without ISV at GPA=0x%llx — "
                 "instruction decode not yet fully implemented",
                 index_, (unsigned long long)gpa);

        hv_vcpu_set_reg(vcpu_, HV_REG_PC, pc + 4);
        return VCpuExitAction::kContinue;
    }

    if (decode.is_write) {
        s_stats_.dabt_write.fetch_add(1, std::memory_order_relaxed);
        uint64_t value = decode.write_value;
        if (!addr_space_->HandleMmioWrite(gpa, decode.access_size, value)) {
            LOG_WARN("hvf: unhandled MMIO write at GPA=0x%llx size=%u",
                     (unsigned long long)gpa, decode.access_size);
        }
    } else {
        s_stats_.dabt_read.fetch_add(1, std::memory_order_relaxed);
        uint64_t value = 0;
        if (!addr_space_->HandleMmioRead(gpa, decode.access_size, &value)) {
            LOG_WARN("hvf: unhandled MMIO read at GPA=0x%llx size=%u",
                     (unsigned long long)gpa, decode.access_size);
        }
        if (decode.reg < 31) {
            hv_vcpu_set_reg(vcpu_, static_cast<hv_reg_t>(HV_REG_X0 + decode.reg), value);
        }
    }

    uint64_t pc;
    hv_vcpu_get_reg(vcpu_, HV_REG_PC, &pc);
    hv_vcpu_set_reg(vcpu_, HV_REG_PC, pc + 4);

    return VCpuExitAction::kContinue;
}

void HvfVCpu::PrintExitStats() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    auto last_print = s_stats_.last_print_time.load(std::memory_order_relaxed);
    if (now_ms - last_print < 1000) {
        return;
    }

    if (!s_stats_.last_print_time.compare_exchange_strong(
            last_print, now_ms, std::memory_order_relaxed)) {
        return;
    }

    uint64_t total = s_stats_.total.load(std::memory_order_relaxed);
    uint64_t exception = s_stats_.exception.load(std::memory_order_relaxed);
    uint64_t vtimer = s_stats_.vtimer_activated.load(std::memory_order_relaxed);
    uint64_t canceled = s_stats_.canceled.load(std::memory_order_relaxed);

    printf("\n========== aarch64 Exit Statistics (per second) ==========\n");
    printf("Total exits: %llu\n", (unsigned long long)total);
    printf("  Exception: %llu\n", (unsigned long long)exception);
    printf("  VTimer:    %llu\n", (unsigned long long)vtimer);
    printf("  Canceled:  %llu\n", (unsigned long long)canceled);

    printf("\n--- Exception Classes ---\n");
    printf("  WFI/WFE:       %llu\n", (unsigned long long)s_stats_.ec_wfi_wfe.load(std::memory_order_relaxed));
    printf("  HVC64:         %llu\n", (unsigned long long)s_stats_.ec_hvc64.load(std::memory_order_relaxed));
    printf("  SMC64:         %llu\n", (unsigned long long)s_stats_.ec_smc64.load(std::memory_order_relaxed));
    printf("  SysReg:        %llu\n", (unsigned long long)s_stats_.ec_sysreg.load(std::memory_order_relaxed));
    printf("  DABT Lower:    %llu\n", (unsigned long long)s_stats_.ec_dabt_lower.load(std::memory_order_relaxed));
    printf("  DABT Curr:     %llu\n", (unsigned long long)s_stats_.ec_dabt_curr.load(std::memory_order_relaxed));
    printf("  BRK:           %llu\n", (unsigned long long)s_stats_.ec_brk.load(std::memory_order_relaxed));
    printf("  Other:         %llu\n", (unsigned long long)s_stats_.ec_other.load(std::memory_order_relaxed));

    uint64_t total_dabt = s_stats_.ec_dabt_lower.load(std::memory_order_relaxed) + 
                          s_stats_.ec_dabt_curr.load(std::memory_order_relaxed);
    if (total_dabt > 0) {
        printf("\n--- Data Abort Details ---\n");
        printf("  Total DABT:    %llu\n", (unsigned long long)total_dabt);
        printf("  Read:          %llu\n", (unsigned long long)s_stats_.dabt_read.load(std::memory_order_relaxed));
        printf("  Write:         %llu\n", (unsigned long long)s_stats_.dabt_write.load(std::memory_order_relaxed));
        printf("  ISV Valid:     %llu\n", (unsigned long long)s_stats_.dabt_isv_valid.load(std::memory_order_relaxed));
        printf("  ISV Invalid:   %llu\n", (unsigned long long)s_stats_.dabt_isv_invalid.load(std::memory_order_relaxed));
    }

    uint64_t total_hvc = s_stats_.ec_hvc64.load(std::memory_order_relaxed) + 
                         s_stats_.ec_smc64.load(std::memory_order_relaxed);
    if (total_hvc > 0) {
        printf("\n--- HVC/SMC Details ---\n");
        printf("  Total HVC/SMC: %llu\n", (unsigned long long)total_hvc);
        printf("  PSCI Version:  %llu\n", (unsigned long long)s_stats_.hvc_version.load(std::memory_order_relaxed));
        printf("  PSCI Features: %llu\n", (unsigned long long)s_stats_.hvc_features.load(std::memory_order_relaxed));
        printf("  PSCI CPU_ON:   %llu\n", (unsigned long long)s_stats_.hvc_cpu_on.load(std::memory_order_relaxed));
        printf("  PSCI CPU_OFF:  %llu\n", (unsigned long long)s_stats_.hvc_cpu_off.load(std::memory_order_relaxed));
        printf("  PSCI SYS_OFF:  %llu\n", (unsigned long long)s_stats_.hvc_system_off.load(std::memory_order_relaxed));
        printf("  PSCI SYS_RST:  %llu\n", (unsigned long long)s_stats_.hvc_system_reset.load(std::memory_order_relaxed));
        printf("  Unknown:       %llu\n", (unsigned long long)s_stats_.hvc_unknown.load(std::memory_order_relaxed));
    }

    printf("==========================================================\n\n");

    s_stats_.total.store(0, std::memory_order_relaxed);
    s_stats_.exception.store(0, std::memory_order_relaxed);
    s_stats_.vtimer_activated.store(0, std::memory_order_relaxed);
    s_stats_.canceled.store(0, std::memory_order_relaxed);
    s_stats_.ec_wfi_wfe.store(0, std::memory_order_relaxed);
    s_stats_.ec_hvc64.store(0, std::memory_order_relaxed);
    s_stats_.ec_smc64.store(0, std::memory_order_relaxed);
    s_stats_.ec_sysreg.store(0, std::memory_order_relaxed);
    s_stats_.ec_dabt_lower.store(0, std::memory_order_relaxed);
    s_stats_.ec_dabt_curr.store(0, std::memory_order_relaxed);
    s_stats_.ec_brk.store(0, std::memory_order_relaxed);
    s_stats_.ec_other.store(0, std::memory_order_relaxed);
    s_stats_.hvc_version.store(0, std::memory_order_relaxed);
    s_stats_.hvc_features.store(0, std::memory_order_relaxed);
    s_stats_.hvc_cpu_on.store(0, std::memory_order_relaxed);
    s_stats_.hvc_cpu_off.store(0, std::memory_order_relaxed);
    s_stats_.hvc_system_off.store(0, std::memory_order_relaxed);
    s_stats_.hvc_system_reset.store(0, std::memory_order_relaxed);
    s_stats_.hvc_unknown.store(0, std::memory_order_relaxed);
    s_stats_.dabt_read.store(0, std::memory_order_relaxed);
    s_stats_.dabt_write.store(0, std::memory_order_relaxed);
    s_stats_.dabt_isv_valid.store(0, std::memory_order_relaxed);
    s_stats_.dabt_isv_invalid.store(0, std::memory_order_relaxed);
}

} // namespace hvf
