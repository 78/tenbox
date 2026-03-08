#include "platform/macos/hypervisor/hvf_vcpu.h"
#include "platform/macos/hypervisor/hvf_mmio_decode.h"
#include "core/vmm/types.h"

namespace hvf {

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

    // Set MPIDR_EL1 (affinity) — required for GICv3 redistributor routing.
    // Use Aff0 = index for simple topology.
    uint64_t mpidr = static_cast<uint64_t>(index) & 0xFF;
    hv_vcpu_set_sys_reg(vcpu->vcpu_, HV_SYS_REG_MPIDR_EL1, mpidr);

    // Trap debug exceptions so BRK causes a VM exit
    hv_vcpu_set_trap_debug_exceptions(vcpu->vcpu_, true);

    // Query the vtimer INTID from the GIC for PPI injection
    uint32_t vtimer_intid = 0;
    hv_return_t gic_ret = hv_gic_get_intid(HV_GIC_INT_EL1_VIRTUAL_TIMER, &vtimer_intid);
    if (gic_ret == HV_SUCCESS) {
        vcpu->vtimer_intid_ = vtimer_intid;
    } else {
        vcpu->vtimer_intid_ = 27;
        LOG_WARN("hvf: vCPU %u failed to get vtimer INTID (ret=%d), using default 27",
                 index, (int)gic_ret);
    }

    LOG_INFO("hvf: vCPU %u created (vtimer INTID=%u)", index, vcpu->vtimer_intid_);
    return vcpu;
}

VCpuExitAction HvfVCpu::RunOnce() {
    // Sync vtimer state: if masked, check if guest has cleared the interrupt
    // (CNTV_CTL_EL0.ISTATUS=0 or ENABLE=0 or IMASK=1) and unmask.
    if (vtimer_masked_) {
        uint64_t ctl = 0;
        hv_vcpu_get_sys_reg(vcpu_, HV_SYS_REG_CNTV_CTL_EL0, &ctl);
        bool irq_asserted = (ctl & 0x7) == 0x5;  // ENABLE=1, IMASK=0, ISTATUS=1
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

    switch (vcpu_exit_->reason) {
    case HV_EXIT_REASON_EXCEPTION:
        return HandleException();

    case HV_EXIT_REASON_CANCELED:
        return VCpuExitAction::kContinue;

    case HV_EXIT_REASON_VTIMER_ACTIVATED:
    {
        // VTimer fired. HVF auto-masks the vtimer on this exit.
        vtimer_masked_ = true;

        // Set PPI pending in GIC redistributor via GICR_ISPENDR0
        uint32_t ppi_bit = 1u << vtimer_intid_;
        hv_return_t gic_ret = hv_gic_set_redistributor_reg(vcpu_,
            HV_GIC_REDISTRIBUTOR_REG_GICR_ISPENDR0, ppi_bit);
        if (gic_ret != HV_SUCCESS) {
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

bool HvfVCpu::SetupBootRegisters(uint8_t* /*ram*/) {
    // ARM64 boot registers are set up via SetupAarch64Boot().
    // This x86-oriented method is a no-op on ARM64.
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
        return VCpuExitAction::kHalt;

    case kEcDabtLower:
    case kEcDabtCurr:
        return HandleDataAbort(syndrome);

    case kEcHvc64:
    case kEcSmc64:
        return HandleHvc();

    case kEcSysReg:
        // System register access trap — skip the instruction
        {
            uint64_t pc;
            hv_vcpu_get_reg(vcpu_, HV_REG_PC, &pc);
            hv_vcpu_set_reg(vcpu_, HV_REG_PC, pc + 4);
        }
        return VCpuExitAction::kContinue;

    case kEcBrk:
    {
        uint16_t imm = syndrome & 0xFFFF;
        LOG_WARN("hvf: vCPU %u BRK #%u (syndrome=0x%llx) — skipping",
                 index_, imm, (unsigned long long)syndrome);
        uint64_t pc;
        hv_vcpu_get_reg(vcpu_, HV_REG_PC, &pc);
        hv_vcpu_set_reg(vcpu_, HV_REG_PC, pc + 4);
        return VCpuExitAction::kContinue;
    }

    default:
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

    // HVF advances PC past HVC/SMC automatically on exception exit;
    // do NOT add 4 here.
    uint64_t pc;
    hv_vcpu_get_reg(vcpu_, HV_REG_PC, &pc);

    LOG_INFO("hvf: vCPU %u HVC func_id=0x%08x x1=0x%llx x2=0x%llx x3=0x%llx PC=0x%llx",
             index_, func_id,
             (unsigned long long)x1, (unsigned long long)x2,
             (unsigned long long)x3, (unsigned long long)pc);

    switch (func_id) {
    case kPsciVersion:
        hv_vcpu_set_reg(vcpu_, HV_REG_X0, 0x00010000);
        return VCpuExitAction::kContinue;

    case kPsciFeaturesCall:
    {
        uint32_t queried = static_cast<uint32_t>(x1);
        if (queried == kPsciCpuOn64 || queried == kPsciCpuOff ||
            queried == kPsciSystemOff || queried == kPsciSystemReset ||
            queried == kPsciVersion) {
            hv_vcpu_set_reg(vcpu_, HV_REG_X0, 0);  // supported
        } else {
            hv_vcpu_set_reg(vcpu_, HV_REG_X0, static_cast<uint64_t>(-1LL));  // not supported
        }
        return VCpuExitAction::kContinue;
    }

    case kPsciCpuOn64:
    {
        PsciCpuOnRequest req;
        req.target_cpu = static_cast<uint32_t>(x1 & 0xFF);  // Aff0
        req.entry_addr = x2;
        req.context_id = x3;

        int result = -2;  // PSCI_RET_INVALID_PARAMETERS
        if (psci_cpu_on_cb_) {
            result = psci_cpu_on_cb_(req);
        }
        hv_vcpu_set_reg(vcpu_, HV_REG_X0, static_cast<uint64_t>(static_cast<int64_t>(result)));
        return VCpuExitAction::kContinue;
    }

    case kPsciCpuOff:
        LOG_INFO("hvf: vCPU %u PSCI CPU_OFF", index_);
        return VCpuExitAction::kHalt;

    case kPsciSystemOff:
        LOG_INFO("hvf: PSCI SYSTEM_OFF from vCPU %u", index_);
        if (psci_shutdown_cb_) psci_shutdown_cb_();
        return VCpuExitAction::kShutdown;

    case kPsciSystemReset:
        LOG_INFO("hvf: PSCI SYSTEM_RESET from vCPU %u", index_);
        if (psci_reboot_cb_) psci_reboot_cb_();
        else if (psci_shutdown_cb_) psci_shutdown_cb_();
        return VCpuExitAction::kShutdown;

    default:
        // Unknown PSCI/HVC call — return NOT_SUPPORTED
        hv_vcpu_set_reg(vcpu_, HV_REG_X0, static_cast<uint64_t>(-1LL));
        return VCpuExitAction::kContinue;
    }
}

VCpuExitAction HvfVCpu::HandleDataAbort(uint64_t syndrome) {
    uint64_t gpa = vcpu_exit_->exception.physical_address;

    // Read all general-purpose registers for instruction decode
    uint64_t regs[31] = {};
    for (int i = 0; i < 31; i++) {
        hv_vcpu_get_reg(vcpu_, static_cast<hv_reg_t>(HV_REG_X0 + i), &regs[i]);
    }

    // Try ISV fast path first, fall back to instruction decode
    MmioDecodeResult decode{};
    bool isv = (syndrome >> 24) & 1;

    if (isv) {
        decode.is_write = (syndrome >> 6) & 1;
        uint8_t sas = (syndrome >> 22) & 3;
        decode.access_size = 1u << sas;
        decode.reg = (syndrome >> 16) & 0x1F;
        decode.is_pair = false;

        if (decode.is_write) {
            decode.write_value = (decode.reg == 31) ? 0 : regs[decode.reg];
        }
    } else {
        // Must decode the instruction from guest memory
        uint64_t pc;
        hv_vcpu_get_reg(vcpu_, HV_REG_PC, &pc);

        // Fetch the 4-byte instruction from guest memory
        // PC is a virtual address, but since we're at EL1 with identity mapping
        // during early boot, we can use it as a physical address hint.
        // For a proper implementation, we'd need to walk page tables.
        // For now, assume the instruction is accessible at the faulting PC.
        uint64_t far_el2 = vcpu_exit_->exception.virtual_address;
        (void)far_el2;

        // Read the instruction from ELR_EL2 (which is the guest PC)
        // This requires the PC to be in mapped guest memory.
        // TODO: implement proper instruction fetch from guest virtual address
        LOG_WARN("hvf: vCPU %u DABT without ISV at GPA=0x%llx — "
                 "instruction decode not yet fully implemented",
                 index_, (unsigned long long)gpa);

        // Advance PC past the faulting instruction and continue
        hv_vcpu_set_reg(vcpu_, HV_REG_PC, pc + 4);
        return VCpuExitAction::kContinue;
    }

    if (decode.is_write) {
        uint64_t value = decode.write_value;
        if (!addr_space_->HandleMmioWrite(gpa, decode.access_size, value)) {
            LOG_WARN("hvf: unhandled MMIO write at GPA=0x%llx size=%u",
                     (unsigned long long)gpa, decode.access_size);
        }
    } else {
        uint64_t value = 0;
        if (!addr_space_->HandleMmioRead(gpa, decode.access_size, &value)) {
            LOG_WARN("hvf: unhandled MMIO read at GPA=0x%llx size=%u",
                     (unsigned long long)gpa, decode.access_size);
        }
        // Write the value back to the destination register
        if (decode.reg < 31) {
            hv_vcpu_set_reg(vcpu_, static_cast<hv_reg_t>(HV_REG_X0 + decode.reg), value);
        }
    }

    // Advance PC past the faulting instruction
    uint64_t pc;
    hv_vcpu_get_reg(vcpu_, HV_REG_PC, &pc);
    hv_vcpu_set_reg(vcpu_, HV_REG_PC, pc + 4);

    return VCpuExitAction::kContinue;
}

} // namespace hvf
