#include "platform/macos/hypervisor/x86_64/hvf_vcpu.h"
#include "core/arch/x86_64/boot.h"
#include "core/device/irq/local_apic.h"
#include "core/vmm/types.h"

#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <Hypervisor/hv_arch_vmx.h>

#include <cpuid.h>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <unistd.h>
#include <x86intrin.h>
#include <mach/mach_time.h>

namespace hvf {

// x86 CR0 bits
static constexpr uint64_t CR0_PE = (1ULL << 0);
static constexpr uint64_t CR0_NE = (1ULL << 5);
static constexpr uint64_t CR0_NW = (1ULL << 29);
static constexpr uint64_t CR0_CD = (1ULL << 30);
static constexpr uint64_t CR0_PG = (1ULL << 31);

// x86 CR4 bits
static constexpr uint64_t CR4_VMXE = (1ULL << 13);

// EFER bits
static constexpr uint64_t EFER_LME = (1ULL << 8);
static constexpr uint64_t EFER_LMA = (1ULL << 10);

// cr0_ones_mask: bits forced to 1 in guest CR0; cr0_zeros_mask: bits forced to 0
static constexpr uint64_t cr0_ones_mask  = CR0_NE;
static constexpr uint64_t cr0_zeros_mask = CR0_NW | CR0_CD;
// cr4_ones_mask: VMXE must be 1 in guest CR4
static constexpr uint64_t cr4_ones_mask  = CR4_VMXE;

static uint64_t FixCr0(uint64_t cr0) {
    return (cr0 | cr0_ones_mask) & ~cr0_zeros_mask;
}

// Convert VMX capability MSR value to a valid control field value.
// Low 32 bits = allowed-0 (must-be-1), high 32 bits = allowed-1 (can-be-1).
uint64_t HvfVCpu::Cap2Ctrl(uint64_t cap, uint64_t ctrl) {
    return (ctrl | (cap & 0xFFFFFFFF)) & (cap >> 32);
}

HvfVCpu::~HvfVCpu() {
    if (created_) {
        hv_vcpu_destroy(vcpuid_);
    }
}

std::unique_ptr<HvfVCpu> HvfVCpu::Create(uint32_t index, AddressSpace* addr_space,
                                             uint8_t* ram, uint64_t ram_size,
                                             const GuestMemMap* guest_mem) {
    if (index == 0) {
        const char* env = getenv("TENBOX_EXIT_STATS");
        if (env && (env[0] == '1' || env[0] == 'y' || env[0] == 'Y')) {
            s_stats_enabled_.store(true, std::memory_order_relaxed);
            LOG_INFO("hvf: VM exit statistics enabled (TENBOX_EXIT_STATS)");
        }
    }

    auto vcpu = std::unique_ptr<HvfVCpu>(new HvfVCpu());
    vcpu->index_ = index;
    vcpu->addr_space_ = addr_space;
    vcpu->ram_ = ram;
    vcpu->ram_size_ = ram_size;
    vcpu->guest_mem_ = guest_mem;
    // IA32_APIC_BASE: xAPIC enabled at 0xFEE00000; BSP flag for vCPU 0 only
    vcpu->apic_base_msr_ = 0xFEE00000ULL | (1ULL << 11) | (index == 0 ? (1ULL << 8) : 0);

    hv_return_t ret = hv_vcpu_create(&vcpu->vcpuid_, HV_VCPU_DEFAULT);
    if (ret != HV_SUCCESS) {
        LOG_ERROR("hvf: hv_vcpu_create(%u) failed: %d", index, (int)ret);
        return nullptr;
    }
    vcpu->created_ = true;

    // Enable native passthrough for frequently-used syscall MSRs (xhyve pattern)
    hv_vcpu_enable_native_msr(vcpu->vcpuid_, 0xC0000100, 1); // MSR_FSBASE
    hv_vcpu_enable_native_msr(vcpu->vcpuid_, 0xC0000101, 1); // MSR_GSBASE
    hv_vcpu_enable_native_msr(vcpu->vcpuid_, 0x174, 1);      // MSR_SYSENTER_CS
    hv_vcpu_enable_native_msr(vcpu->vcpuid_, 0x175, 1);      // MSR_SYSENTER_ESP
    hv_vcpu_enable_native_msr(vcpu->vcpuid_, 0x176, 1);      // MSR_SYSENTER_EIP
    hv_vcpu_enable_native_msr(vcpu->vcpuid_, 0x10, 1);       // MSR_TSC
    hv_vcpu_enable_native_msr(vcpu->vcpuid_, 0xC0000081, 1); // MSR_STAR
    hv_vcpu_enable_native_msr(vcpu->vcpuid_, 0xC0000082, 1); // MSR_LSTAR
    hv_vcpu_enable_native_msr(vcpu->vcpuid_, 0xC0000083, 1); // MSR_CSTAR
    hv_vcpu_enable_native_msr(vcpu->vcpuid_, 0xC0000084, 1); // MSR_SF_MASK
    hv_vcpu_enable_native_msr(vcpu->vcpuid_, 0xC0000102, 1); // MSR_KGSBASE
    hv_vcpu_enable_native_msr(vcpu->vcpuid_, 0x48, 1);       // MSR_IA32_SPEC_CTRL (IBRS/STIBP)
    hv_vcpu_enable_native_msr(vcpu->vcpuid_, 0x49, 1);       // MSR_IA32_PRED_CMD (IBPB)

    // Measure TSC frequency for kvmclock
    {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        uint64_t mach_start = mach_absolute_time();
        uint64_t tsc_start = __rdtsc();
        usleep(10000); // 10ms
        uint64_t tsc_end = __rdtsc();
        uint64_t mach_end = mach_absolute_time();
        double elapsed = static_cast<double>(mach_end - mach_start) * tb.numer / tb.denom / 1e9;
        vcpu->tsc_freq_ = static_cast<uint64_t>((tsc_end - tsc_start) / elapsed);
    }

    if (!vcpu->SetupVmcs()) {
        LOG_ERROR("hvf: VMCS setup failed for vCPU %u", index);
        return nullptr;
    }

    LOG_INFO("hvf: x86_64 vCPU %u created", index);
    return vcpu;
}

bool HvfVCpu::SetupVmcs() {
    uint64_t cap_pin, cap_proc, cap_proc2, cap_entry, cap_exit;
    hv_vmx_read_capability(HV_VMX_CAP_PINBASED, &cap_pin);
    hv_vmx_read_capability(HV_VMX_CAP_PROCBASED, &cap_proc);
    hv_vmx_read_capability(HV_VMX_CAP_PROCBASED2, &cap_proc2);
    hv_vmx_read_capability(HV_VMX_CAP_ENTRY, &cap_entry);
    hv_vmx_read_capability(HV_VMX_CAP_EXIT, &cap_exit);

    uint64_t pin_ctrl = Cap2Ctrl(cap_pin,
        PIN_BASED_INTR | PIN_BASED_NMI | PIN_BASED_VIRTUAL_NMI);

    uint64_t proc_ctrl = Cap2Ctrl(cap_proc,
        CPU_BASED_HLT |
        CPU_BASED_UNCOND_IO |
        CPU_BASED_MSR_BITMAPS |
        CPU_BASED_SECONDARY_CTLS |
        CPU_BASED_CR8_LOAD |
        CPU_BASED_CR8_STORE);

    uint64_t proc2_ctrl = Cap2Ctrl(cap_proc2, CPU_BASED2_UNRESTRICTED);

    // xhyve: VM_ENTRY_LOAD_EFER must be set; IA-32e mode guest bit starts as 0
    uint64_t entry_ctrl = Cap2Ctrl(cap_entry, VMENTRY_LOAD_EFER);
    // Clear IA-32e mode guest bit initially (32-bit protected mode boot)
    entry_ctrl &= ~(uint64_t)VMENTRY_GUEST_IA32E;

    uint64_t exit_ctrl = Cap2Ctrl(cap_exit,
        VMEXIT_HOST_IA32E |
        VMEXIT_ACK_INTR |
        VMEXIT_LOAD_EFER);

    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_PIN_BASED, pin_ctrl);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_CPU_BASED, proc_ctrl);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_CPU_BASED2, proc2_ctrl);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_VMENTRY_CONTROLS, entry_ctrl);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_VMEXIT_CONTROLS, exit_ctrl);

    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_EXC_BITMAP, 0);

    // CR0 mask: intercept PG and PE changes (xhyve pattern)
    uint64_t cr0_mask = (cr0_ones_mask | cr0_zeros_mask) | CR0_PG | CR0_PE;
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_CR0_MASK, cr0_mask);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_CR0_SHADOW, 0x60000010);

    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_CR4_MASK, cr4_ones_mask);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_CR4_SHADOW, 0);

    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_TPR_THRESHOLD, 0);

    // Guest segments: initial state before SetupBootRegisters
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CS, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CS_BASE, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CS_LIMIT, 0xFFFF);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CS_AR, 0x9B);

    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_DS, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_DS_BASE, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_DS_LIMIT, 0xFFFF);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_DS_AR, 0x93);

    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_ES, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_ES_BASE, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_ES_LIMIT, 0xFFFF);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_ES_AR, 0x93);

    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_SS, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_SS_BASE, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_SS_LIMIT, 0xFFFF);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_SS_AR, 0x93);

    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_FS, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_FS_BASE, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_FS_LIMIT, 0xFFFF);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_FS_AR, 0x93);

    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_GS, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_GS_BASE, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_GS_LIMIT, 0xFFFF);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_GS_AR, 0x93);

    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_LDTR, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_LDTR_BASE, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_LDTR_LIMIT, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_LDTR_AR, 0x0082);

    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_TR, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_TR_BASE, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_TR_LIMIT, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_TR_AR, 0x008B);

    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_GDTR_BASE, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_GDTR_LIMIT, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_IDTR_BASE, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_IDTR_LIMIT, 0);

    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CR0, FixCr0(0x60000010));
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CR3, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CR4, cr4_ones_mask);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_IA32_EFER, 0);

    hv_vcpu_write_register(vcpuid_, HV_X86_RIP, 0);
    hv_vcpu_write_register(vcpuid_, HV_X86_RFLAGS, 0x2);
    hv_vcpu_write_register(vcpuid_, HV_X86_RSP, 0);

    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_ACTIVITY_STATE, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_INTERRUPTIBILITY, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_LINK_POINTER, ~0ull);

    return true;
}

void HvfVCpu::OnThreadInit() {
    LocalApic::SetCurrentCpu(index_);
}

void HvfVCpu::OnStartup(const VCpuStartupState& state) {
    SetupSipiRegisters(state.sipi_vector);
}

bool HvfVCpu::SetupBootRegisters(uint8_t* ram) {
    namespace Layout = x86::Layout;
    (void)ram;

    // 32-bit protected mode boot — matching xhyve kexec() exactly.
    // The GDT has already been written by LoadLinuxKernel (boot.cpp) at kGdtBase.
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_GDTR_BASE, Layout::kGdtBase);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_GDTR_LIMIT, 0x1F);

    // CS = selector 0x10 — 32-bit flat code
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CS, 0x10);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CS_BASE, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CS_LIMIT, 0xFFFFFFFF);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CS_AR, 0xC09B);

    // DS, ES, SS = selector 0x18 — 32-bit flat data
    auto SetDataSeg = [&](uint32_t sel_field, uint32_t base_field,
                          uint32_t limit_field, uint32_t ar_field) {
        hv_vmx_vcpu_write_vmcs(vcpuid_, sel_field, 0x18);
        hv_vmx_vcpu_write_vmcs(vcpuid_, base_field, 0);
        hv_vmx_vcpu_write_vmcs(vcpuid_, limit_field, 0xFFFFFFFF);
        hv_vmx_vcpu_write_vmcs(vcpuid_, ar_field, 0xC093);
    };
    SetDataSeg(VMCS_GUEST_DS, VMCS_GUEST_DS_BASE, VMCS_GUEST_DS_LIMIT, VMCS_GUEST_DS_AR);
    SetDataSeg(VMCS_GUEST_ES, VMCS_GUEST_ES_BASE, VMCS_GUEST_ES_LIMIT, VMCS_GUEST_ES_AR);
    SetDataSeg(VMCS_GUEST_SS, VMCS_GUEST_SS_BASE, VMCS_GUEST_SS_LIMIT, VMCS_GUEST_SS_AR);

    // CR0 = PE + NE (protected mode, numeric exception) — guest sees 0x21
    uint64_t guest_cr0 = FixCr0(0x21);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CR0, guest_cr0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_CR0_SHADOW, 0x21);

    // RIP = kKernelBase (32-bit entry), RSI = kBootParams
    hv_vcpu_write_register(vcpuid_, HV_X86_RIP, Layout::kKernelBase);
    hv_vcpu_write_register(vcpuid_, HV_X86_RSI, Layout::kBootParams);
    hv_vcpu_write_register(vcpuid_, HV_X86_RFLAGS, 0x2);
    hv_vcpu_write_register(vcpuid_, HV_X86_RAX, 0);
    hv_vcpu_write_register(vcpuid_, HV_X86_RBX, 0);
    hv_vcpu_write_register(vcpuid_, HV_X86_RCX, 0);
    hv_vcpu_write_register(vcpuid_, HV_X86_RDX, 0);
    hv_vcpu_write_register(vcpuid_, HV_X86_RDI, 0);
    hv_vcpu_write_register(vcpuid_, HV_X86_RBP, 0);
    hv_vcpu_write_register(vcpuid_, HV_X86_RSP, 0);

    LOG_INFO("hvf: vCPU %u boot: 32-bit protected mode, RIP=0x%llx RSI=0x%llx",
             index_, (unsigned long long)Layout::kKernelBase,
             (unsigned long long)Layout::kBootParams);
    return true;
}

bool HvfVCpu::SetupSipiRegisters(uint8_t sipi_vector) {
    uint16_t cs_sel = static_cast<uint16_t>(sipi_vector) << 8;
    uint64_t cs_base = static_cast<uint64_t>(sipi_vector) << 12;

    // Real mode segment descriptors
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CS, cs_sel);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CS_BASE, cs_base);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CS_LIMIT, 0xFFFF);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CS_AR, 0x9B);

    auto SetRealModeSeg = [&](uint32_t sel_f, uint32_t base_f,
                              uint32_t limit_f, uint32_t ar_f) {
        hv_vmx_vcpu_write_vmcs(vcpuid_, sel_f, 0);
        hv_vmx_vcpu_write_vmcs(vcpuid_, base_f, 0);
        hv_vmx_vcpu_write_vmcs(vcpuid_, limit_f, 0xFFFF);
        hv_vmx_vcpu_write_vmcs(vcpuid_, ar_f, 0x93);
    };
    SetRealModeSeg(VMCS_GUEST_DS, VMCS_GUEST_DS_BASE, VMCS_GUEST_DS_LIMIT, VMCS_GUEST_DS_AR);
    SetRealModeSeg(VMCS_GUEST_ES, VMCS_GUEST_ES_BASE, VMCS_GUEST_ES_LIMIT, VMCS_GUEST_ES_AR);
    SetRealModeSeg(VMCS_GUEST_SS, VMCS_GUEST_SS_BASE, VMCS_GUEST_SS_LIMIT, VMCS_GUEST_SS_AR);
    SetRealModeSeg(VMCS_GUEST_FS, VMCS_GUEST_FS_BASE, VMCS_GUEST_FS_LIMIT, VMCS_GUEST_FS_AR);
    SetRealModeSeg(VMCS_GUEST_GS, VMCS_GUEST_GS_BASE, VMCS_GUEST_GS_LIMIT, VMCS_GUEST_GS_AR);

    // CR0: no PE — real mode (unrestricted guest handles this)
    uint64_t cr0_val = FixCr0(0x60000010);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CR0, cr0_val);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_CR0_SHADOW, 0x60000010);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CR3, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CR4, cr4_ones_mask);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_CR4_SHADOW, 0);

    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_IA32_EFER, 0);

    // Clear IA-32e mode guest bit for real mode
    uint64_t entry_ctrl = 0;
    hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_CTRL_VMENTRY_CONTROLS, &entry_ctrl);
    entry_ctrl &= ~(uint64_t)VMENTRY_GUEST_IA32E;
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_VMENTRY_CONTROLS, entry_ctrl);

    hv_vcpu_write_register(vcpuid_, HV_X86_RIP, 0);
    hv_vcpu_write_register(vcpuid_, HV_X86_RFLAGS, 0x2);
    hv_vcpu_write_register(vcpuid_, HV_X86_RSP, 0);
    hv_vcpu_write_register(vcpuid_, HV_X86_RAX, 0);
    hv_vcpu_write_register(vcpuid_, HV_X86_RBX, 0);
    hv_vcpu_write_register(vcpuid_, HV_X86_RCX, 0);
    hv_vcpu_write_register(vcpuid_, HV_X86_RDX, 0);
    hv_vcpu_write_register(vcpuid_, HV_X86_RDI, 0);
    hv_vcpu_write_register(vcpuid_, HV_X86_RSI, 0);
    hv_vcpu_write_register(vcpuid_, HV_X86_RBP, 0);

    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_ACTIVITY_STATE, 0);
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_INTERRUPTIBILITY, 0);

    LOG_INFO("hvf: vCPU %u SIPI: real mode CS:IP=%04x:%04x (linear 0x%05llx)",
             index_, cs_sel, 0, (unsigned long long)cs_base);
    return true;
}

void HvfVCpu::AdvanceRip() {
    uint64_t insn_len = 0;
    hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_RO_VMEXIT_INSTR_LEN, &insn_len);
    uint64_t rip = 0;
    hv_vcpu_read_register(vcpuid_, HV_X86_RIP, &rip);
    hv_vcpu_write_register(vcpuid_, HV_X86_RIP, rip + insn_len);
}

void HvfVCpu::QueueInterrupt(uint32_t vector) {
    {
        std::lock_guard<std::mutex> lock(irq_mutex_);
        pending_irqs_.push(vector);
    }
    irq_cv_.notify_one();
}

void HvfVCpu::WakeFromHalt() {
    irq_cv_.notify_one();
}

bool HvfVCpu::HasPendingInterrupt() {
    std::lock_guard<std::mutex> lock(irq_mutex_);
    return !pending_irqs_.empty();
}

bool HvfVCpu::WaitForInterrupt(uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(irq_mutex_);
    if (!pending_irqs_.empty()) return true;
    irq_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                     [this]() { return !pending_irqs_.empty(); });
    return !pending_irqs_.empty();
}

void HvfVCpu::TryInjectInterrupt() {
    // Re-inject any event that was being delivered through the guest IDT
    // when the last VM exit occurred (Intel SDM Vol 3, 27.2.4).
    uint64_t idt_info = 0;
    hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_RO_IDT_VECTOR_INFO, &idt_info);
    if (idt_info & IRQ_INFO_VALID) {
        uint64_t entry_info = idt_info & ~(uint64_t)(1u << 12);
        hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_VMENTRY_IRQ_INFO, entry_info);
        if (idt_info & (1u << 11)) {
            uint64_t idt_err = 0;
            hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_RO_IDT_VECTOR_ERROR, &idt_err);
            hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_VMENTRY_EXC_ERROR, idt_err);
        }
        // VM entry slot is used for re-injection; if there are pending IRQs,
        // request an interrupt window exit so they get injected promptly.
        std::lock_guard<std::mutex> lock(irq_mutex_);
        if (!pending_irqs_.empty() && !irq_window_active_) {
            uint64_t proc = 0;
            hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_CTRL_CPU_BASED, &proc);
            proc |= CPU_BASED_IRQ_WND;
            hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_CPU_BASED, proc);
            irq_window_active_ = true;
        }
        return;
    }

    std::lock_guard<std::mutex> lock(irq_mutex_);

    if (pending_irqs_.empty()) {
        if (irq_window_active_) {
            uint64_t proc = 0;
            hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_CTRL_CPU_BASED, &proc);
            proc &= ~(uint64_t)CPU_BASED_IRQ_WND;
            hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_CPU_BASED, proc);
            irq_window_active_ = false;
        }
        return;
    }

    uint64_t rflags = 0;
    hv_vcpu_read_register(vcpuid_, HV_X86_RFLAGS, &rflags);
    uint64_t interruptibility = 0;
    hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_INTERRUPTIBILITY, &interruptibility);

    bool can_inject = (rflags & 0x200) && !(interruptibility & 0x3);

    if (!can_inject) {
        if (!irq_window_active_) {
            uint64_t proc = 0;
            hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_CTRL_CPU_BASED, &proc);
            proc |= CPU_BASED_IRQ_WND;
            hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_CPU_BASED, proc);
            irq_window_active_ = true;
        }
        return;
    }

    uint32_t vector = pending_irqs_.front();
    pending_irqs_.pop();

    bool more_pending = !pending_irqs_.empty();

    if (more_pending && !irq_window_active_) {
        uint64_t proc = 0;
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_CTRL_CPU_BASED, &proc);
        proc |= CPU_BASED_IRQ_WND;
        hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_CPU_BASED, proc);
        irq_window_active_ = true;
    } else if (!more_pending && irq_window_active_) {
        uint64_t proc = 0;
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_CTRL_CPU_BASED, &proc);
        proc &= ~(uint64_t)CPU_BASED_IRQ_WND;
        hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_CPU_BASED, proc);
        irq_window_active_ = false;
    }

    uint64_t inject_info = vector | IRQ_INFO_EXT_IRQ | IRQ_INFO_VALID;
    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_VMENTRY_IRQ_INFO, inject_info);
}

void HvfVCpu::CancelRun() {
    hv_return_t ret = hv_vcpu_interrupt(&vcpuid_, 1);
    if (ret != HV_SUCCESS) {
        LOG_WARN("hvf: hv_vcpu_interrupt(%u) failed: %d", index_, (int)ret);
    }
}

HvfVCpu::ExitStats HvfVCpu::s_stats_;
std::atomic<bool> HvfVCpu::s_stats_enabled_{false};

void HvfVCpu::PrintExitStats() {
    auto& s = s_stats_;
    uint64_t now = mach_absolute_time();
    uint64_t prev = s.last_print_time.load(std::memory_order_relaxed);
    if (prev == 0) { s.last_print_time.store(now, std::memory_order_relaxed); return; }

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double elapsed_s = (double)(now - prev) * tb.numer / tb.denom / 1e9;
    if (elapsed_s < 5.0) return;

    if (!s.last_print_time.compare_exchange_strong(prev, now, std::memory_order_acq_rel))
        return;

    // Snapshot and reset all counters
    #define SNAP(f) uint64_t f = s.f.exchange(0, std::memory_order_relaxed)
    SNAP(total); SNAP(irq); SNAP(irq_wnd); SNAP(hlt); SNAP(io); SNAP(ept);
    SNAP(cpuid); SNAP(rdmsr); SNAP(wrmsr); SNAP(cr); SNAP(xsetbv); SNAP(other);
    SNAP(io_uart); SNAP(io_pit); SNAP(io_acpi); SNAP(io_pci);
    SNAP(io_pic); SNAP(io_rtc); SNAP(io_sink); SNAP(io_other);
    SNAP(ept_lapic_eoi); SNAP(ept_lapic_tpr); SNAP(ept_lapic_icr);
    SNAP(ept_lapic_timer); SNAP(ept_lapic_other); SNAP(ept_ioapic); SNAP(ept_other);
    SNAP(cr0); SNAP(cr3); SNAP(cr4); SNAP(cr8); SNAP(cr_other);
    SNAP(wrmsr_kvmclock); SNAP(wrmsr_wallclock); SNAP(wrmsr_poll);
    SNAP(wrmsr_efer); SNAP(wrmsr_apicbase); SNAP(wrmsr_other);
    SNAP(wrmsr_top_count); SNAP(rdmsr_apicbase); SNAP(rdmsr_other);
    #undef SNAP
    uint32_t top_msr = s.wrmsr_top_msr.exchange(0, std::memory_order_relaxed);

    double rate = total / elapsed_s;
    LOG_INFO("=== VM exit stats (%.1fs, %.0f/s, total %llu) ===", elapsed_s, rate, total);
    LOG_INFO("  IRQ:%-7llu IRQ_WND:%-7llu HLT:%-7llu IO:%-7llu EPT:%-7llu CPUID:%-5llu",
             irq, irq_wnd, hlt, io, ept, cpuid);
    LOG_INFO("  RDMSR:%-5llu WRMSR:%-7llu CR:%-5llu XSETBV:%-3llu OTHER:%-3llu",
             rdmsr, wrmsr, cr, xsetbv, other);
    if (io > 0)
        LOG_INFO("  IO: UART:%-5llu PIT:%-5llu ACPI:%-5llu PCI:%-5llu PIC:%-4llu RTC:%-3llu SINK:%-3llu OTHER:%-3llu",
                 io_uart, io_pit, io_acpi, io_pci, io_pic, io_rtc, io_sink, io_other);
    if (ept > 0)
        LOG_INFO("  EPT: EOI:%-5llu TPR:%-5llu ICR:%-5llu TIMER:%-5llu LAPIC_X:%-4llu IOAPIC:%-4llu OTHER:%-4llu",
                 ept_lapic_eoi, ept_lapic_tpr, ept_lapic_icr, ept_lapic_timer,
                 ept_lapic_other, ept_ioapic, ept_other);
    if (cr > 0)
        LOG_INFO("  CR: CR0:%-5llu CR3:%-5llu CR4:%-5llu CR8:%-5llu OTHER:%-3llu",
                 cr0, cr3, cr4, cr8, cr_other);
    if (wrmsr > 0) {
        LOG_INFO("  WRMSR: KVMCLK:%-7llu WALL:%-3llu POLL:%-3llu EFER:%-3llu APIC:%-3llu OTHER:%-5llu",
                 wrmsr_kvmclock, wrmsr_wallclock, wrmsr_poll, wrmsr_efer, wrmsr_apicbase, wrmsr_other);
        if (wrmsr_other > 0)
            LOG_INFO("  WRMSR top: MSR 0x%X x%llu", top_msr, wrmsr_top_count);
    }
}

VCpuExitAction HvfVCpu::RunOnce() {
    TryInjectInterrupt();

    hv_return_t ret = hv_vcpu_run_until(vcpuid_, HV_DEADLINE_FOREVER);
    if (ret != HV_SUCCESS) {
        LOG_ERROR("hvf: hv_vcpu_run(%u) failed: %d", index_, (int)ret);
        return VCpuExitAction::kError;
    }

    uint64_t exit_reason = 0;
    hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_RO_EXIT_REASON, &exit_reason);
    exit_reason &= 0xFFFF;

    uint64_t exit_qual = 0;
    hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_RO_EXIT_QUALIFIC, &exit_qual);

    if (s_stats_enabled_.load(std::memory_order_relaxed)) {
        s_stats_.total.fetch_add(1, std::memory_order_relaxed);
        PrintExitStats();
    }

    switch (exit_reason) {
    case VMX_REASON_IRQ:
        if (s_stats_enabled_.load(std::memory_order_relaxed))
            s_stats_.irq.fetch_add(1, std::memory_order_relaxed);
        return VCpuExitAction::kContinue;

    case VMX_REASON_HLT:
        if (s_stats_enabled_.load(std::memory_order_relaxed))
            s_stats_.hlt.fetch_add(1, std::memory_order_relaxed);
        return HandleHlt();

    case VMX_REASON_IO:
        if (s_stats_enabled_.load(std::memory_order_relaxed))
            s_stats_.io.fetch_add(1, std::memory_order_relaxed);
        return HandleIo(exit_qual);

    case VMX_REASON_EPT_VIOLATION:
        if (s_stats_enabled_.load(std::memory_order_relaxed))
            s_stats_.ept.fetch_add(1, std::memory_order_relaxed);
        return HandleEptViolation(exit_qual);

    case VMX_REASON_CPUID:
        if (s_stats_enabled_.load(std::memory_order_relaxed))
            s_stats_.cpuid.fetch_add(1, std::memory_order_relaxed);
        return HandleCpuid();

    case VMX_REASON_RDMSR:
        if (s_stats_enabled_.load(std::memory_order_relaxed))
            s_stats_.rdmsr.fetch_add(1, std::memory_order_relaxed);
        return HandleMsr(false);

    case VMX_REASON_WRMSR:
        if (s_stats_enabled_.load(std::memory_order_relaxed))
            s_stats_.wrmsr.fetch_add(1, std::memory_order_relaxed);
        return HandleMsr(true);

    case VMX_REASON_MOV_CR:
        if (s_stats_enabled_.load(std::memory_order_relaxed))
            s_stats_.cr.fetch_add(1, std::memory_order_relaxed);
        return HandleCrAccess(exit_qual);

    case VMX_REASON_TRIPLE_FAULT: {
        uint64_t rip = 0, rsp = 0, cr0 = 0, cr3 = 0, cr4 = 0, rflags = 0;
        uint64_t cs = 0, cs_base = 0, cs_limit = 0, cs_ar = 0;
        uint64_t idtr_base = 0, idtr_limit = 0, gdtr_base = 0, gdtr_limit = 0;
        uint64_t efer = 0, entry_ctrl = 0;
        uint64_t ss = 0, ss_ar = 0, tr = 0, tr_ar = 0, tr_base = 0;
        hv_vcpu_read_register(vcpuid_, HV_X86_RIP, &rip);
        hv_vcpu_read_register(vcpuid_, HV_X86_RSP, &rsp);
        hv_vcpu_read_register(vcpuid_, HV_X86_RFLAGS, &rflags);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_CR0, &cr0);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_CR3, &cr3);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_CR4, &cr4);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_CS, &cs);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_CS_BASE, &cs_base);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_CS_LIMIT, &cs_limit);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_CS_AR, &cs_ar);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_IDTR_BASE, &idtr_base);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_IDTR_LIMIT, &idtr_limit);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_GDTR_BASE, &gdtr_base);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_GDTR_LIMIT, &gdtr_limit);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_IA32_EFER, &efer);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_CTRL_VMENTRY_CONTROLS, &entry_ctrl);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_SS, &ss);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_SS_AR, &ss_ar);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_TR, &tr);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_TR_AR, &tr_ar);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_TR_BASE, &tr_base);
        LOG_ERROR("hvf: vCPU %u triple fault — RIP=0x%llx RSP=0x%llx RFLAGS=0x%llx "
                  "CR0=0x%llx CR3=0x%llx CR4=0x%llx CS=0x%llx(base=0x%llx lim=0x%llx ar=0x%llx)",
                  index_, (unsigned long long)rip, (unsigned long long)rsp,
                  (unsigned long long)rflags, (unsigned long long)cr0,
                  (unsigned long long)cr3, (unsigned long long)cr4,
                  (unsigned long long)cs, (unsigned long long)cs_base,
                  (unsigned long long)cs_limit, (unsigned long long)cs_ar);
        LOG_ERROR("hvf: vCPU %u  IDTR=0x%llx:0x%llx GDTR=0x%llx:0x%llx EFER=0x%llx entry_ctrl=0x%llx "
                  "SS=0x%llx(ar=0x%llx) TR=0x%llx(ar=0x%llx base=0x%llx)",
                  index_, (unsigned long long)idtr_base, (unsigned long long)idtr_limit,
                  (unsigned long long)gdtr_base, (unsigned long long)gdtr_limit,
                  (unsigned long long)efer, (unsigned long long)entry_ctrl,
                  (unsigned long long)ss, (unsigned long long)ss_ar,
                  (unsigned long long)tr, (unsigned long long)tr_ar,
                  (unsigned long long)tr_base);

        // Dump last EPT violation details
        LOG_ERROR("hvf: vCPU %u  last EPT GPA=0x%llx decode_fail_count=%u",
                  index_, (unsigned long long)last_ept_gpa_, last_decode_fail_count_);
        return VCpuExitAction::kError;
    }

    case VMX_REASON_XSETBV:
        if (s_stats_enabled_.load(std::memory_order_relaxed))
            s_stats_.xsetbv.fetch_add(1, std::memory_order_relaxed);
        return HandleXsetbv();

    case VMX_REASON_EPT_MISCONFIG:
        LOG_ERROR("hvf: vCPU %u EPT misconfiguration", index_);
        return VCpuExitAction::kError;

    case VMX_REASON_VMCALL: {
        hv_vcpu_write_register(vcpuid_, HV_X86_RAX, static_cast<uint64_t>(-1));
        AdvanceRip();
        return VCpuExitAction::kContinue;
    }

    case VMX_REASON_IRQ_WND:
        if (s_stats_enabled_.load(std::memory_order_relaxed))
            s_stats_.irq_wnd.fetch_add(1, std::memory_order_relaxed);
        return VCpuExitAction::kContinue;

    case VMX_REASON_VMENTRY_GUEST: {
        uint64_t rip = 0, cr0 = 0, cr4 = 0, efer = 0, entry_ctrl = 0;
        uint64_t proc2 = 0, cs = 0, cs_base = 0, cs_ar = 0, cs_limit = 0;
        uint64_t ss_ar = 0, tr_ar = 0, act_state = 0, rflags = 0;
        hv_vcpu_read_register(vcpuid_, HV_X86_RIP, &rip);
        hv_vcpu_read_register(vcpuid_, HV_X86_RFLAGS, &rflags);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_CR0, &cr0);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_CR4, &cr4);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_IA32_EFER, &efer);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_CTRL_VMENTRY_CONTROLS, &entry_ctrl);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_CTRL_CPU_BASED2, &proc2);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_CS, &cs);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_CS_BASE, &cs_base);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_CS_AR, &cs_ar);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_CS_LIMIT, &cs_limit);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_SS_AR, &ss_ar);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_TR_AR, &tr_ar);
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_ACTIVITY_STATE, &act_state);
        LOG_ERROR("hvf: vCPU %u VM-entry failure (invalid guest state): "
                  "RIP=0x%llx RFLAGS=0x%llx CR0=0x%llx CR4=0x%llx EFER=0x%llx "
                  "entry_ctrl=0x%llx proc2=0x%llx CS=%04llx base=0x%llx limit=0x%llx ar=0x%llx "
                  "SS_ar=0x%llx TR_ar=0x%llx activity=%llu",
                  index_, (unsigned long long)rip, (unsigned long long)rflags,
                  (unsigned long long)cr0, (unsigned long long)cr4,
                  (unsigned long long)efer, (unsigned long long)entry_ctrl,
                  (unsigned long long)proc2, (unsigned long long)cs,
                  (unsigned long long)cs_base, (unsigned long long)cs_limit,
                  (unsigned long long)cs_ar, (unsigned long long)ss_ar,
                  (unsigned long long)tr_ar, (unsigned long long)act_state);
        return VCpuExitAction::kError;
    }

    default: {
        if (s_stats_enabled_.load(std::memory_order_relaxed))
            s_stats_.other.fetch_add(1, std::memory_order_relaxed);
        uint64_t rip = 0;
        hv_vcpu_read_register(vcpuid_, HV_X86_RIP, &rip);
        LOG_WARN("hvf: vCPU %u unhandled exit reason %llu at RIP=0x%llx",
                 index_, (unsigned long long)exit_reason, (unsigned long long)rip);
        return VCpuExitAction::kError;
    }
    }
}

VCpuExitAction HvfVCpu::HandleHlt() {
    AdvanceRip();
    return VCpuExitAction::kHalt;
}

VCpuExitAction HvfVCpu::HandleIo(uint64_t exit_qual) {
    uint16_t port = (exit_qual >> 16) & 0xFFFF;
    uint8_t size = (exit_qual & 7) + 1;
    bool is_in = (exit_qual >> 3) & 1;
    bool is_string = (exit_qual >> 4) & 1;

    if (s_stats_enabled_.load(std::memory_order_relaxed)) {
        if (port >= 0x3F8 && port <= 0x3FF) s_stats_.io_uart.fetch_add(1, std::memory_order_relaxed);
        else if (port >= 0x40 && port <= 0x43) s_stats_.io_pit.fetch_add(1, std::memory_order_relaxed);
        else if (port >= 0x600 && port <= 0x60F) s_stats_.io_acpi.fetch_add(1, std::memory_order_relaxed);
        else if (port >= 0xCF8 && port <= 0xCFF) s_stats_.io_pci.fetch_add(1, std::memory_order_relaxed);
        else if ((port >= 0x20 && port <= 0x21) || (port >= 0xA0 && port <= 0xA1)) s_stats_.io_pic.fetch_add(1, std::memory_order_relaxed);
        else if (port >= 0x70 && port <= 0x71) s_stats_.io_rtc.fetch_add(1, std::memory_order_relaxed);
        else if (port == 0x80 || port == 0x87 ||
                 (port >= 0x2E8 && port <= 0x2EF) || (port >= 0x2F8 && port <= 0x2FF) ||
                 (port >= 0x3E8 && port <= 0x3EF) || (port >= 0xC000 && port <= 0xCFFF)) s_stats_.io_sink.fetch_add(1, std::memory_order_relaxed);
        else s_stats_.io_other.fetch_add(1, std::memory_order_relaxed);
    }

    if (is_string) {
        AdvanceRip();
        return VCpuExitAction::kContinue;
    }

    if (is_in) {
        uint32_t val = 0;
        addr_space_->HandlePortIn(port, size, &val);
        uint64_t rax = 0;
        hv_vcpu_read_register(vcpuid_, HV_X86_RAX, &rax);
        uint64_t mask = (1ULL << (size * 8)) - 1;
        rax = (rax & ~mask) | (val & mask);
        hv_vcpu_write_register(vcpuid_, HV_X86_RAX, rax);
    } else {
        uint64_t rax = 0;
        hv_vcpu_read_register(vcpuid_, HV_X86_RAX, &rax);
        uint32_t val = static_cast<uint32_t>(rax);
        addr_space_->HandlePortOut(port, size, val);
    }

    AdvanceRip();
    return VCpuExitAction::kContinue;
}

bool HvfVCpu::DecodeMmioInsn(uint64_t rip_gpa, MmioDecodeResult& out, const uint8_t* code) {
    if (!code) return false;

    uint8_t pos = 0;
    bool rex_w = false, rex_r = false, rex_b = false;
    bool has_66 = false;
    bool has_lock = false;

    for (; pos < 8; pos++) {
        uint8_t b = code[pos];
        if (b == 0x66) { has_66 = true; continue; }
        if (b == 0xF0) { has_lock = true; continue; }
        if (b == 0x67 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 ||
            b == 0x64 || b == 0x65) { continue; }
        if ((b & 0xF0) == 0x40) {
            rex_w = (b >> 3) & 1;
            rex_r = (b >> 2) & 1;
            rex_b = (b >> 0) & 1;
            continue;
        }
        break;
    }

    static const hv_x86_reg_t reg_map[] = {
        HV_X86_RAX, HV_X86_RCX, HV_X86_RDX, HV_X86_RBX,
        HV_X86_RSP, HV_X86_RBP, HV_X86_RSI, HV_X86_RDI,
        HV_X86_R8,  HV_X86_R9,  HV_X86_R10, HV_X86_R11,
        HV_X86_R12, HV_X86_R13, HV_X86_R14, HV_X86_R15,
    };

    memset(&out, 0, sizeof(out));
    out.reg = HV_X86_RAX;

    auto OperandSize = [&](bool byte_variant) -> uint8_t {
        if (byte_variant) return 1;
        if (rex_w) return 4; // MMIO regs are 32-bit; treat REX.W as 4
        if (has_66) return 2;
        return 4;
    };

    // Helper: parse ModR/M and skip SIB + displacement
    auto SkipModRM = [&](uint8_t modrm) {
        uint8_t mod = (modrm >> 6) & 3;
        uint8_t rm  = modrm & 7;
        if (mod == 3) return; // register operand, no memory
        bool has_sib = (rm == 4);
        uint8_t sib_base = 0;
        if (has_sib) {
            sib_base = code[pos] & 7;
            pos++; // skip SIB byte
        }
        if (mod == 0) {
            // disp32 if rm==5 (no SIB) or SIB base==5 (with SIB)
            if (rm == 5 || (has_sib && sib_base == 5)) pos += 4;
        } else if (mod == 1) {
            pos += 1; // disp8
        } else { // mod == 2
            pos += 4; // disp32
        }
    };

    auto ReadImm = [&](uint8_t sz) -> uint64_t {
        uint64_t v = 0;
        for (uint8_t i = 0; i < sz && pos < 15; i++)
            v |= (uint64_t)code[pos++] << (i * 8);
        return v;
    };

    uint8_t opcode = code[pos++];

    // --- Two-byte opcodes (0x0F prefix) ---
    if (opcode == 0x0F) {
        uint8_t op2 = code[pos++];
        uint8_t modrm = code[pos++];
        uint8_t reg_field = ((modrm >> 3) & 7) | (rex_r ? 8 : 0);
        SkipModRM(modrm);

        switch (op2) {
        case 0xB6: // MOVZX r, r/m8
            out.op = MmioOp::kMovzx;
            out.is_write = false;
            out.size = 1;
            out.reg = reg_map[reg_field & 0xF];
            out.insn_len = pos;
            return true;
        case 0xB7: // MOVZX r, r/m16
            out.op = MmioOp::kMovzx;
            out.is_write = false;
            out.size = 2;
            out.reg = reg_map[reg_field & 0xF];
            out.insn_len = pos;
            return true;
        case 0xBE: // MOVSX r, r/m8
            out.op = MmioOp::kMovsx;
            out.is_write = false;
            out.size = 1;
            out.reg = reg_map[reg_field & 0xF];
            out.insn_len = pos;
            return true;
        case 0xBF: // MOVSX r, r/m16
            out.op = MmioOp::kMovsx;
            out.is_write = false;
            out.size = 2;
            out.reg = reg_map[reg_field & 0xF];
            out.insn_len = pos;
            return true;
        case 0xB1: // CMPXCHG r/m32, r32
            out.op = MmioOp::kCmpxchg;
            out.is_write = true;
            out.size = OperandSize(false);
            out.reg = reg_map[reg_field & 0xF];
            out.insn_len = pos;
            return true;
        case 0xB0: // CMPXCHG r/m8, r8
            out.op = MmioOp::kCmpxchg;
            out.is_write = true;
            out.size = 1;
            out.reg = reg_map[reg_field & 0xF];
            out.insn_len = pos;
            return true;
        default:
            return false;
        }
    }

    // --- Single-byte opcodes ---
    switch (opcode) {
    // MOV r/m, r (store)
    case 0x88: case 0x89: {
        bool byte = (opcode == 0x88);
        uint8_t modrm = code[pos++];
        uint8_t reg_field = ((modrm >> 3) & 7) | (rex_r ? 8 : 0);
        SkipModRM(modrm);
        out.op = MmioOp::kMovStore;
        out.is_write = true;
        out.size = OperandSize(byte);
        out.reg = reg_map[reg_field & 0xF];
        out.insn_len = pos;
        return true;
    }
    // MOV r, r/m (load)
    case 0x8A: case 0x8B: {
        bool byte = (opcode == 0x8A);
        uint8_t modrm = code[pos++];
        uint8_t reg_field = ((modrm >> 3) & 7) | (rex_r ? 8 : 0);
        SkipModRM(modrm);
        out.op = MmioOp::kMovLoad;
        out.is_write = false;
        out.size = OperandSize(byte);
        out.reg = reg_map[reg_field & 0xF];
        out.insn_len = pos;
        return true;
    }
    // MOV r/m, imm (store immediate)
    case 0xC6: case 0xC7: {
        bool byte = (opcode == 0xC6);
        uint8_t modrm = code[pos++];
        SkipModRM(modrm);
        uint8_t sz = OperandSize(byte);
        uint8_t imm_sz = (sz == 8) ? 4 : sz; // 64-bit uses sign-extended imm32
        out.op = MmioOp::kMovStoreImm;
        out.is_write = true;
        out.size = sz;
        out.has_imm = true;
        out.imm_value = ReadImm(imm_sz);
        out.insn_len = pos;
        return true;
    }
    // XCHG r, r/m
    case 0x86: case 0x87: {
        bool byte = (opcode == 0x86);
        uint8_t modrm = code[pos++];
        uint8_t reg_field = ((modrm >> 3) & 7) | (rex_r ? 8 : 0);
        SkipModRM(modrm);
        // Treat as store (the read side is handled in HandleEptViolation)
        out.op = MmioOp::kMovStore;
        out.is_write = true;
        out.size = OperandSize(byte);
        out.reg = reg_map[reg_field & 0xF];
        out.insn_len = pos;
        return true;
    }
    // AND r/m, r  (0x20/0x21)
    case 0x20: case 0x21: {
        bool byte = (opcode == 0x20);
        uint8_t modrm = code[pos++];
        uint8_t reg_field = ((modrm >> 3) & 7) | (rex_r ? 8 : 0);
        SkipModRM(modrm);
        out.op = MmioOp::kAnd;
        out.is_write = true;
        out.size = OperandSize(byte);
        out.reg = reg_map[reg_field & 0xF];
        out.insn_len = pos;
        return true;
    }
    // OR r/m, r  (0x08/0x09)
    case 0x08: case 0x09: {
        bool byte = (opcode == 0x08);
        uint8_t modrm = code[pos++];
        uint8_t reg_field = ((modrm >> 3) & 7) | (rex_r ? 8 : 0);
        SkipModRM(modrm);
        out.op = MmioOp::kOr;
        out.is_write = true;
        out.size = OperandSize(byte);
        out.reg = reg_map[reg_field & 0xF];
        out.insn_len = pos;
        return true;
    }
    // XOR r/m, r  (0x30/0x31)
    case 0x30: case 0x31: {
        bool byte = (opcode == 0x30);
        uint8_t modrm = code[pos++];
        uint8_t reg_field = ((modrm >> 3) & 7) | (rex_r ? 8 : 0);
        SkipModRM(modrm);
        out.op = MmioOp::kXor;
        out.is_write = true;
        out.size = OperandSize(byte);
        out.reg = reg_map[reg_field & 0xF];
        out.insn_len = pos;
        return true;
    }
    // Group 1: 0x80/0x81/0x83 — ALU r/m, imm
    case 0x80: case 0x81: case 0x83: {
        bool byte = (opcode == 0x80);
        uint8_t modrm = code[pos++];
        uint8_t alu_op = (modrm >> 3) & 7;
        SkipModRM(modrm);
        uint8_t sz = OperandSize(byte);
        uint8_t imm_sz = (opcode == 0x83) ? 1 : ((sz == 8) ? 4 : sz);
        uint64_t imm = ReadImm(imm_sz);
        // Sign-extend for 0x83
        if (opcode == 0x83 && (imm & 0x80)) imm |= ~0xFFULL;
        out.has_imm = true;
        out.imm_value = imm;
        out.size = sz;
        out.insn_len = pos;

        switch (alu_op) {
        case 1: out.op = MmioOp::kOr;  out.is_write = true; return true;  // OR
        case 4: out.op = MmioOp::kAnd; out.is_write = true; return true;  // AND
        case 6: out.op = MmioOp::kXor; out.is_write = true; return true;  // XOR
        case 7: out.op = MmioOp::kTest; out.is_write = false; return true; // CMP (flags only)
        default: return false; // ADD, ADC, SBB, SUB: not meaningful for MMIO
        }
    }
    // TEST r/m, imm — 0xF6/0xF7 with reg=0
    case 0xF6: case 0xF7: {
        bool byte = (opcode == 0xF6);
        uint8_t modrm = code[pos++];
        uint8_t reg_field = (modrm >> 3) & 7;
        if (reg_field != 0) return false; // only TEST
        SkipModRM(modrm);
        uint8_t sz = OperandSize(byte);
        uint8_t imm_sz = (sz == 8) ? 4 : sz;
        out.op = MmioOp::kTest;
        out.is_write = false;
        out.size = sz;
        out.has_imm = true;
        out.imm_value = ReadImm(imm_sz);
        out.insn_len = pos;
        return true;
    }
    // TEST r/m, r — 0x84/0x85
    case 0x84: case 0x85: {
        bool byte = (opcode == 0x84);
        uint8_t modrm = code[pos++];
        uint8_t reg_field = ((modrm >> 3) & 7) | (rex_r ? 8 : 0);
        SkipModRM(modrm);
        out.op = MmioOp::kTest;
        out.is_write = false;
        out.size = OperandSize(byte);
        out.reg = reg_map[reg_field & 0xF];
        out.insn_len = pos;
        return true;
    }
    default:
        return false;
    }
}

VCpuExitAction HvfVCpu::HandleEptViolation(uint64_t exit_qual) {
    uint64_t gpa = 0;
    hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_PHYSICAL_ADDRESS, &gpa);
    last_ept_gpa_ = gpa;

    if (s_stats_enabled_.load(std::memory_order_relaxed)) {
        if (gpa >= 0xFEE00000 && gpa < 0xFEE01000) {
            uint32_t reg_off = gpa & 0xFFF;
            if (reg_off == 0x0B0) s_stats_.ept_lapic_eoi.fetch_add(1, std::memory_order_relaxed);
            else if (reg_off == 0x080) s_stats_.ept_lapic_tpr.fetch_add(1, std::memory_order_relaxed);
            else if (reg_off == 0x300 || reg_off == 0x310) s_stats_.ept_lapic_icr.fetch_add(1, std::memory_order_relaxed);
            else if (reg_off == 0x320 || reg_off == 0x380 || reg_off == 0x390 || reg_off == 0x3E0)
                s_stats_.ept_lapic_timer.fetch_add(1, std::memory_order_relaxed);
            else s_stats_.ept_lapic_other.fetch_add(1, std::memory_order_relaxed);
        } else if (gpa >= 0xFEC00000 && gpa < 0xFEC01000) {
            s_stats_.ept_ioapic.fetch_add(1, std::memory_order_relaxed);
        } else {
            s_stats_.ept_other.fetch_add(1, std::memory_order_relaxed);
        }
    }

    bool is_write = (exit_qual >> 1) & 1;
    bool is_fetch = (exit_qual >> 2) & 1;

    if (is_fetch) {
        return VCpuExitAction::kContinue;
    }

    bool is_mmio = addr_space_->IsMmioAddress(gpa);
    bool is_gap = (gpa >= kMmioGapStart && gpa < kMmioGapEnd);

    if (!is_mmio && !is_gap) {
        return VCpuExitAction::kContinue;
    }

    // Translate guest RIP to GPA for instruction decode.
    // In long mode, walk the page table from CR3.
    uint64_t rip = 0;
    hv_vcpu_read_register(vcpuid_, HV_X86_RIP, &rip);
    uint64_t insn_len = 0;
    hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_RO_VMEXIT_INSTR_LEN, &insn_len);

    // Helper: translate GPA to host pointer, supporting memory above MMIO gap
    auto GpaToHost = [this](uint64_t addr, uint64_t len) -> uint8_t* {
        if (guest_mem_) {
            uint8_t* p = guest_mem_->GpaToHva(addr);
            if (p) return p;
        }
        if (ram_ && addr + len <= ram_size_) return ram_ + addr;
        return nullptr;
    };

    // Translate guest RIP virtual address to GPA.
    // When paging is off (CR0.PG=0), the linear address IS the physical address.
    uint64_t cr0_shadow = 0;
    hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_CTRL_CR0_SHADOW, &cr0_shadow);
    uint64_t rip_gpa = 0;
    bool rip_translated = false;

    if (!(cr0_shadow & CR0_PG)) {
        // No paging: linear address = physical address.
        // In real mode, linear = CS.base + IP; HVF gives us the full linear RIP.
        uint64_t cs_base = 0;
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_CS_BASE, &cs_base);
        rip_gpa = cs_base + rip;
        rip_translated = true;
    } else {
        uint64_t cr3 = 0;
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_CR3, &cr3);
        uint64_t pml4_base = cr3 & ~0xFFFULL;
        uint8_t* pml4e_ptr = GpaToHost(pml4_base + ((rip >> 39) & 0x1FF) * 8, 8);
        if (pml4e_ptr) {
            uint64_t pml4e = *reinterpret_cast<uint64_t*>(pml4e_ptr);
            if (pml4e & 1) {
                uint64_t pdpt_base = pml4e & 0x000FFFFFFFFFF000ULL;
                uint8_t* pdpte_ptr = GpaToHost(pdpt_base + ((rip >> 30) & 0x1FF) * 8, 8);
                if (pdpte_ptr) {
                    uint64_t pdpte = *reinterpret_cast<uint64_t*>(pdpte_ptr);
                    if (pdpte & 1) {
                        if (pdpte & 0x80) {
                            rip_gpa = (pdpte & 0x000FFFFFC0000000ULL) | (rip & 0x3FFFFFFFULL);
                            rip_translated = true;
                        } else {
                            uint64_t pd_base = pdpte & 0x000FFFFFFFFFF000ULL;
                            uint8_t* pde_ptr = GpaToHost(pd_base + ((rip >> 21) & 0x1FF) * 8, 8);
                            if (pde_ptr) {
                                uint64_t pde = *reinterpret_cast<uint64_t*>(pde_ptr);
                                if (pde & 1) {
                                    if (pde & 0x80) {
                                        rip_gpa = (pde & 0x000FFFFFFFE00000ULL) | (rip & 0x1FFFFFULL);
                                        rip_translated = true;
                                    } else {
                                        uint64_t pt_base = pde & 0x000FFFFFFFFFF000ULL;
                                        uint8_t* pte_ptr = GpaToHost(pt_base + ((rip >> 12) & 0x1FF) * 8, 8);
                                        if (pte_ptr) {
                                            uint64_t pte = *reinterpret_cast<uint64_t*>(pte_ptr);
                                            if (pte & 1) {
                                                rip_gpa = (pte & 0x000FFFFFFFFFF000ULL) | (rip & 0xFFF);
                                                rip_translated = true;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    MmioDecodeResult decode = {};
    bool decoded = false;
    if (rip_translated) {
        uint8_t* code_ptr = GpaToHost(rip_gpa, 15);
        if (code_ptr) {
            decoded = DecodeMmioInsn(rip_gpa, decode, code_ptr);
        }
    }

    if (!decoded) {
        last_decode_fail_count_++;
        if (rip_translated) {
            uint8_t* code = GpaToHost(rip_gpa, 8);
            if (code) {
                LOG_WARN("hvf: vCPU %u MMIO decode fail GPA=0x%llx RIP=0x%llx rip_gpa=0x%llx "
                         "bytes=%02x %02x %02x %02x %02x %02x %02x %02x is_write=%d insn_len=%llu",
                         index_, (unsigned long long)gpa, (unsigned long long)rip,
                         (unsigned long long)rip_gpa,
                         code[0], code[1], code[2], code[3],
                         code[4], code[5], code[6], code[7],
                         (int)is_write, (unsigned long long)insn_len);
            }
        }
        uint8_t skip = (insn_len > 0 && insn_len <= 15) ? (uint8_t)insn_len : 0;
        if (skip > 0) {
            hv_vcpu_write_register(vcpuid_, HV_X86_RIP, rip + skip);
        }
        return VCpuExitAction::kContinue;
    }

    // Helper lambdas
    auto MmioRead = [&](uint64_t addr, uint8_t sz) -> uint64_t {
        uint64_t val = 0;
        if (is_mmio) addr_space_->HandleMmioRead(addr, sz, &val);
        return val;
    };
    auto MmioWrite = [&](uint64_t addr, uint8_t sz, uint64_t val) {
        if (is_mmio) addr_space_->HandleMmioWrite(addr, sz, val);
    };
    auto Mask = [](uint8_t sz) -> uint64_t {
        return (sz >= 8) ? ~0ULL : (1ULL << (sz * 8)) - 1;
    };

    switch (decode.op) {
    case MmioOp::kMovLoad: {
        uint64_t val = MmioRead(gpa, decode.size) & Mask(decode.size);
        hv_vcpu_write_register(vcpuid_, decode.reg, val);
        break;
    }
    case MmioOp::kMovStore: {
        uint64_t val = 0;
        hv_vcpu_read_register(vcpuid_, decode.reg, &val);
        MmioWrite(gpa, decode.size, val & Mask(decode.size));
        break;
    }
    case MmioOp::kMovStoreImm: {
        MmioWrite(gpa, decode.size, decode.imm_value & Mask(decode.size));
        break;
    }
    case MmioOp::kMovzx: {
        uint64_t val = MmioRead(gpa, decode.size) & Mask(decode.size);
        hv_vcpu_write_register(vcpuid_, decode.reg, val);
        break;
    }
    case MmioOp::kMovsx: {
        uint64_t val = MmioRead(gpa, decode.size) & Mask(decode.size);
        uint8_t sign_bit = decode.size * 8 - 1;
        if (val & (1ULL << sign_bit))
            val |= ~Mask(decode.size);
        hv_vcpu_write_register(vcpuid_, decode.reg, val);
        break;
    }
    case MmioOp::kTest: {
        // Flags-only operation: no write-back needed, just skip the instruction.
        // The guest will retry or proceed; for MMIO status polling this is safe.
        break;
    }
    case MmioOp::kAnd: {
        uint64_t old_val = MmioRead(gpa, decode.size);
        uint64_t operand = decode.has_imm ? decode.imm_value : 0;
        if (!decode.has_imm) hv_vcpu_read_register(vcpuid_, decode.reg, &operand);
        MmioWrite(gpa, decode.size, (old_val & operand) & Mask(decode.size));
        break;
    }
    case MmioOp::kOr: {
        uint64_t old_val = MmioRead(gpa, decode.size);
        uint64_t operand = decode.has_imm ? decode.imm_value : 0;
        if (!decode.has_imm) hv_vcpu_read_register(vcpuid_, decode.reg, &operand);
        MmioWrite(gpa, decode.size, (old_val | operand) & Mask(decode.size));
        break;
    }
    case MmioOp::kXor: {
        uint64_t old_val = MmioRead(gpa, decode.size);
        uint64_t operand = decode.has_imm ? decode.imm_value : 0;
        if (!decode.has_imm) hv_vcpu_read_register(vcpuid_, decode.reg, &operand);
        MmioWrite(gpa, decode.size, (old_val ^ operand) & Mask(decode.size));
        break;
    }
    case MmioOp::kCmpxchg: {
        uint64_t mem_val = MmioRead(gpa, decode.size) & Mask(decode.size);
        uint64_t rax_val = 0;
        hv_vcpu_read_register(vcpuid_, HV_X86_RAX, &rax_val);
        rax_val &= Mask(decode.size);
        if (rax_val == mem_val) {
            uint64_t src = 0;
            hv_vcpu_read_register(vcpuid_, decode.reg, &src);
            MmioWrite(gpa, decode.size, src & Mask(decode.size));
        } else {
            uint64_t full_rax = 0;
            hv_vcpu_read_register(vcpuid_, HV_X86_RAX, &full_rax);
            full_rax = (full_rax & ~Mask(decode.size)) | mem_val;
            hv_vcpu_write_register(vcpuid_, HV_X86_RAX, full_rax);
        }
        break;
    }
    }

    hv_vcpu_write_register(vcpuid_, HV_X86_RIP, rip + decode.insn_len);
    return VCpuExitAction::kContinue;
}

VCpuExitAction HvfVCpu::HandleCpuid() {
    uint64_t rax = 0, rcx = 0;
    hv_vcpu_read_register(vcpuid_, HV_X86_RAX, &rax);
    hv_vcpu_read_register(vcpuid_, HV_X86_RCX, &rcx);

    uint32_t leaf = static_cast<uint32_t>(rax);
    uint32_t subleaf = static_cast<uint32_t>(rcx);

    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;

    if (leaf == 0x40000000) {
        // KVM_CPUID_SIGNATURE: "KVMKVMKVM\0\0\0"
        eax = 0x40000001;
        ebx = 0x4b4d564b; // "KVMK"
        ecx = 0x564b4d56; // "VMKV"
        edx = 0x0000004d; // "M\0\0\0"
    } else if (leaf == 0x40000001) {
        // KVM_CPUID_FEATURES
        eax = (1u << 1)   // KVM_FEATURE_NOP_IO_DELAY
            | (1u << 3)   // KVM_FEATURE_CLOCKSOURCE2 (kvmclock v2)
            | (1u << 12)  // KVM_FEATURE_POLL_CONTROL
            | (1u << 24); // KVM_FEATURE_CLOCKSOURCE_STABLE_BIT
        ebx = 0;
        ecx = 0;
        edx = (1u << 0);  // KVM_HINTS_REALTIME
    } else {
        __cpuid_count(leaf, subleaf, eax, ebx, ecx, edx);
    }

    if (leaf == 1) {
        ecx &= ~(1u << 3);   // MONITOR/MWAIT
        ecx &= ~(1u << 5);   // VMX
        ecx &= ~(1u << 21);  // x2APIC — force xAPIC MMIO mode
        ecx &= ~(1u << 24);  // TSC-Deadline
        ecx &= ~(1u << 26);  // XSAVE (not fully emulated)
        ecx &= ~(1u << 27);  // OSXSAVE
        ecx |= (1u << 31);   // Hypervisor present
        ebx = (ebx & 0x00FFFFFF) | (index_ << 24);
    }

    if (leaf == 6) {
        // Thermal & Power Management: hide HWP/P-state features to prevent
        // intel_pstate from probing non-existent MSRs in the VM.
        eax = 0;
        ebx = 0;
        ecx = 0;
        edx = 0;
    }

    if (leaf == 7 && subleaf == 0) {
        ebx &= ~(1u << 10);  // INVPCID
        ebx &= ~(1u << 16);  // AVX-512 Foundation
        ecx &= ~(1u << 22);  // RDPID
    }

    if (leaf == 0xB) {
        // x2APIC topology — EDX = x2APIC ID = vCPU index
        edx = index_;
    }

    if (leaf == 0x80000001) {
        edx &= ~(1u << 27);  // RDTSCP
    }

    hv_vcpu_write_register(vcpuid_, HV_X86_RAX, eax);
    hv_vcpu_write_register(vcpuid_, HV_X86_RBX, ebx);
    hv_vcpu_write_register(vcpuid_, HV_X86_RCX, ecx);
    hv_vcpu_write_register(vcpuid_, HV_X86_RDX, edx);

    AdvanceRip();
    return VCpuExitAction::kContinue;
}

// pvclock_vcpu_time_info: shared memory struct for kvmclock (32 bytes, packed)
struct pvclock_vcpu_time_info {
    uint32_t version;
    uint32_t pad0;
    uint64_t tsc_timestamp;
    uint64_t system_time;
    uint32_t tsc_to_system_mul;
    int8_t   tsc_shift;
    uint8_t  flags;
    uint8_t  pad[2];
} __attribute__((__packed__));

struct pvclock_wall_clock {
    uint32_t version;
    uint32_t sec;
    uint32_t nsec;
} __attribute__((__packed__));

// KVM/QEMU approach: compute (tsc_shift, tsc_to_system_mul) such that
//   ns = ((tsc_delta << tsc_shift) * tsc_to_system_mul) >> 32
//   (negative tsc_shift means right shift)
// Equivalent to: tsc_to_system_mul = (10^9 / freq) << 32, adjusted by shift
static void ComputeKvmclockScale(uint64_t freq, int8_t* shift_out, uint32_t* mul_out) {
    if (freq == 0) freq = 1;

    int8_t shift = 0;
    uint64_t scaled_hz = freq;

    // The goal: mul = (10^9 << 32) / (freq << shift) fits in [1, UINT32_MAX]
    // If freq > 10^9 (GHz-class), we need negative shift to bring freq down
    // If freq < 10^9 (sub-GHz), we need positive shift
    while (scaled_hz > 1000000000ULL * 2 && shift > -31) {
        scaled_hz >>= 1;
        shift--;
    }
    while (scaled_hz < 1000000000ULL / 2 && shift < 31) {
        scaled_hz <<= 1;
        shift++;
    }

    // mul = (10^9 << 32) / scaled_hz
    __uint128_t dividend = (__uint128_t)1000000000ULL << 32;
    uint64_t mul64 = (uint64_t)(dividend / scaled_hz);
    if (mul64 > UINT32_MAX) mul64 = UINT32_MAX;

    *shift_out = shift;
    *mul_out = static_cast<uint32_t>(mul64);
}

void HvfVCpu::UpdateKvmclock(uint64_t gpa) {
    uint8_t* hva = nullptr;
    if (guest_mem_) {
        hva = guest_mem_->GpaToHva(gpa);
    } else if (ram_ && gpa + sizeof(pvclock_vcpu_time_info) <= ram_size_) {
        hva = ram_ + gpa;
    }
    if (!hva) return;

    auto* info = reinterpret_cast<pvclock_vcpu_time_info*>(hva);

    uint32_t ver = info->version;
    ver = (ver + 1) | 1;  // odd = update in progress
    info->version = ver;
    __sync_synchronize();

    uint64_t tsc_now = __rdtsc();
    auto now = std::chrono::steady_clock::now();
    uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();

    info->tsc_timestamp = tsc_now;
    info->system_time = ns;

    int8_t shift;
    uint32_t mul;
    ComputeKvmclockScale(tsc_freq_, &shift, &mul);
    info->tsc_to_system_mul = mul;
    info->tsc_shift = shift;

    // PVCLOCK_TSC_STABLE_BIT: TSC is stable across all CPUs
    info->flags = (1u << 0);

    __sync_synchronize();
    ver++;  // even = update complete
    info->version = ver;
}

VCpuExitAction HvfVCpu::HandleMsr(bool is_write) {
    uint64_t rcx = 0;
    hv_vcpu_read_register(vcpuid_, HV_X86_RCX, &rcx);
    uint32_t msr = static_cast<uint32_t>(rcx);

    constexpr uint32_t MSR_IA32_APIC_BASE = 0x1B;
    constexpr uint32_t MSR_IA32_EFER = 0xC0000080;
    constexpr uint32_t MSR_IA32_MISC_ENABLE = 0x1A0;
    constexpr uint32_t MSR_MCG_CAP = 0x179;
    constexpr uint32_t MSR_MCG_STATUS = 0x17A;
    constexpr uint32_t MSR_MTRRcap = 0xFE;
    constexpr uint32_t MSR_MTRRdefType = 0x2FF;
    constexpr uint32_t MSR_PAT = 0x277;
    constexpr uint32_t MSR_KVM_WALL_CLOCK_NEW = 0x4b564d00;
    constexpr uint32_t MSR_KVM_SYSTEM_TIME_NEW = 0x4b564d01;
    constexpr uint32_t MSR_KVM_POLL_CONTROL = 0x4b564d05;

    if (s_stats_enabled_.load(std::memory_order_relaxed)) {
        if (is_write) {
            if (msr == MSR_KVM_SYSTEM_TIME_NEW) s_stats_.wrmsr_kvmclock.fetch_add(1, std::memory_order_relaxed);
            else if (msr == MSR_KVM_WALL_CLOCK_NEW) s_stats_.wrmsr_wallclock.fetch_add(1, std::memory_order_relaxed);
            else if (msr == MSR_KVM_POLL_CONTROL) s_stats_.wrmsr_poll.fetch_add(1, std::memory_order_relaxed);
            else if (msr == MSR_IA32_EFER) s_stats_.wrmsr_efer.fetch_add(1, std::memory_order_relaxed);
            else if (msr == MSR_IA32_APIC_BASE) s_stats_.wrmsr_apicbase.fetch_add(1, std::memory_order_relaxed);
            else {
                s_stats_.wrmsr_other.fetch_add(1, std::memory_order_relaxed);
                s_stats_.wrmsr_top_msr.store(msr, std::memory_order_relaxed);
                s_stats_.wrmsr_top_count.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            if (msr == MSR_IA32_APIC_BASE) s_stats_.rdmsr_apicbase.fetch_add(1, std::memory_order_relaxed);
            else s_stats_.rdmsr_other.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (is_write) {
        uint64_t rax = 0, rdx = 0;
        hv_vcpu_read_register(vcpuid_, HV_X86_RAX, &rax);
        hv_vcpu_read_register(vcpuid_, HV_X86_RDX, &rdx);
        uint64_t val = (rdx << 32) | (rax & 0xFFFFFFFF);

        if (msr == MSR_IA32_APIC_BASE) {
            // Accept write but force xAPIC mode (bit 11 = enable, bit 10 = x2APIC off)
            apic_base_msr_ = (val & ~(1ULL << 10)) | (1ULL << 11);
        } else if (msr == MSR_IA32_EFER) {
            hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_IA32_EFER, val);
            uint64_t entry_ctrl = 0;
            hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_CTRL_VMENTRY_CONTROLS, &entry_ctrl);
            if (val & EFER_LMA) {
                entry_ctrl |= VMENTRY_GUEST_IA32E;
            } else {
                entry_ctrl &= ~(uint64_t)VMENTRY_GUEST_IA32E;
            }
            hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_VMENTRY_CONTROLS, entry_ctrl);
        } else if (msr == MSR_KVM_WALL_CLOCK_NEW) {
            uint64_t gpa = val;
            uint8_t* wc_hva = nullptr;
            if (guest_mem_) {
                wc_hva = guest_mem_->GpaToHva(gpa);
            } else if (ram_ && gpa + sizeof(pvclock_wall_clock) <= ram_size_) {
                wc_hva = ram_ + gpa;
            }
            if (wc_hva) {
                auto* wc = reinterpret_cast<pvclock_wall_clock*>(wc_hva);
                wc->version = 1u;
                __sync_synchronize();

                auto now = std::chrono::system_clock::now();
                auto epoch = now.time_since_epoch();
                auto secs = std::chrono::duration_cast<std::chrono::seconds>(epoch);
                auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(epoch - secs);

                wc->sec = static_cast<uint32_t>(secs.count());
                wc->nsec = static_cast<uint32_t>(nsecs.count());

                __sync_synchronize();
                wc->version = 2u;
                LOG_INFO("kvmclock: wall_clock GPA=0x%llx sec=%u nsec=%u",
                         (unsigned long long)gpa, wc->sec, wc->nsec);
            }
        } else if (msr == MSR_KVM_SYSTEM_TIME_NEW) {
            bool enable = val & 1;
            uint64_t gpa = val & ~0x3ULL;
            if (enable) {
                kvmclock_system_time_gpa_ = gpa;
                kvmclock_enabled_ = true;
                UpdateKvmclock(gpa);
                LOG_INFO("kvmclock: system_time enabled GPA=0x%llx tsc_freq=%llu",
                         (unsigned long long)gpa, (unsigned long long)tsc_freq_);
            } else {
                kvmclock_enabled_ = false;
                LOG_INFO("kvmclock: system_time disabled");
            }
        } else if (msr == MSR_KVM_POLL_CONTROL) {
            // Guest enables/disables host-side HLT polling; accept and ignore.
        } else if (msr == MSR_MCG_CAP || msr == MSR_MCG_STATUS) {
            // ignore
        } else if (msr == MSR_MTRRcap) {
            // read-only, inject #GP
        } else if ((msr >= 0x200 && msr <= 0x2FF) || msr == MSR_MTRRdefType) {
            // MTRR writes: ignore
        } else if (msr == MSR_IA32_MISC_ENABLE) {
            // Mostly ignore
        } else {
            hv_return_t ret = hv_vcpu_write_msr(vcpuid_, msr, val);
            if (ret != HV_SUCCESS) {
                LOG_DEBUG("hvf: MSR write 0x%X = 0x%llx unsupported",
                          msr, (unsigned long long)val);
            }
        }
    } else {
        uint64_t val = 0;

        if (msr == MSR_IA32_APIC_BASE) {
            // xAPIC enabled at 0xFEE00000, BSP flag for vCPU 0
            val = apic_base_msr_;
        } else if (msr == MSR_IA32_EFER) {
            hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_IA32_EFER, &val);
        } else if (msr == MSR_KVM_WALL_CLOCK_NEW || msr == MSR_KVM_SYSTEM_TIME_NEW) {
            val = 0;
        } else if (msr == MSR_KVM_POLL_CONTROL) {
            val = 1; // polling enabled by default
        } else if (msr == MSR_IA32_MISC_ENABLE) {
            val = (1ULL << 0) | (1ULL << 11) | (1ULL << 12);
        } else if (msr == MSR_MCG_CAP || msr == MSR_MCG_STATUS) {
            val = 0;
        } else if (msr == MSR_MTRRcap || (msr >= 0x200 && msr <= 0x2FF) ||
                   msr == MSR_MTRRdefType) {
            val = 0;
        } else if (msr == MSR_PAT) {
            val = 0x0007040600070406ULL;
        } else {
            hv_return_t ret = hv_vcpu_read_msr(vcpuid_, msr, &val);
            if (ret != HV_SUCCESS) {
                val = 0;
            }
        }

        hv_vcpu_write_register(vcpuid_, HV_X86_RAX, val & 0xFFFFFFFF);
        hv_vcpu_write_register(vcpuid_, HV_X86_RDX, val >> 32);
    }

    AdvanceRip();
    return VCpuExitAction::kContinue;
}

VCpuExitAction HvfVCpu::HandleCrAccess(uint64_t exit_qual) {
    uint8_t cr_num = exit_qual & 0xF;
    uint8_t access_type = (exit_qual >> 4) & 0x3;
    uint8_t reg = (exit_qual >> 8) & 0xF;

    if (s_stats_enabled_.load(std::memory_order_relaxed)) {
        if (cr_num == 0) s_stats_.cr0.fetch_add(1, std::memory_order_relaxed);
        else if (cr_num == 3) s_stats_.cr3.fetch_add(1, std::memory_order_relaxed);
        else if (cr_num == 4) s_stats_.cr4.fetch_add(1, std::memory_order_relaxed);
        else if (cr_num == 8) s_stats_.cr8.fetch_add(1, std::memory_order_relaxed);
        else s_stats_.cr_other.fetch_add(1, std::memory_order_relaxed);
    }

    static const hv_x86_reg_t reg_map[] = {
        HV_X86_RAX, HV_X86_RCX, HV_X86_RDX, HV_X86_RBX,
        HV_X86_RSP, HV_X86_RBP, HV_X86_RSI, HV_X86_RDI,
        HV_X86_R8,  HV_X86_R9,  HV_X86_R10, HV_X86_R11,
        HV_X86_R12, HV_X86_R13, HV_X86_R14, HV_X86_R15,
    };

    if (access_type == 0) {
        // MOV to CR (xhyve vmx_emulate_cr0_access / vmx_emulate_cr4_access)
        uint64_t regval = 0;
        hv_vcpu_read_register(vcpuid_, reg_map[reg], &regval);

        if (cr_num == 0) {
            // Update shadow to what guest intended
            hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_CR0_SHADOW, regval);

            // Apply fixed bits
            uint64_t crval = FixCr0(regval);
            hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CR0, crval);

            // Handle long mode activation / deactivation (xhyve pattern)
            uint64_t efer = 0;
            hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_IA32_EFER, &efer);

            if (regval & CR0_PG) {
                // Activating paging: if EFER.LME, set EFER.LMA + IA-32e mode guest
                if (efer & EFER_LME) {
                    efer |= EFER_LMA;
                    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_IA32_EFER, efer);
                    uint64_t entry_ctrl = 0;
                    hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_CTRL_VMENTRY_CONTROLS, &entry_ctrl);
                    entry_ctrl |= VMENTRY_GUEST_IA32E;
                    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_VMENTRY_CONTROLS, entry_ctrl);
                }
            } else {
                // Deactivating paging: clear EFER.LMA + IA-32e mode guest
                if (efer & EFER_LMA) {
                    efer &= ~EFER_LMA;
                    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_IA32_EFER, efer);
                    uint64_t entry_ctrl = 0;
                    hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_CTRL_VMENTRY_CONTROLS, &entry_ctrl);
                    entry_ctrl &= ~(uint64_t)VMENTRY_GUEST_IA32E;
                    hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_VMENTRY_CONTROLS, entry_ctrl);
                }
            }
        } else if (cr_num == 3) {
            hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CR3, regval);
        } else if (cr_num == 4) {
            hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_CR4_SHADOW, regval);
            uint64_t crval = (regval | cr4_ones_mask);
            hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CR4, crval);
        }
    } else if (access_type == 1) {
        // MOV from CR — return shadow value for CR0/CR4 so guest sees its own values
        uint64_t val = 0;
        if (cr_num == 0) {
            hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_CTRL_CR0_SHADOW, &val);
        } else if (cr_num == 3) {
            hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_CR3, &val);
        } else if (cr_num == 4) {
            hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_CTRL_CR4_SHADOW, &val);
        }
        hv_vcpu_write_register(vcpuid_, reg_map[reg], val);
    } else if (access_type == 2) {
        // CLTS
        uint64_t cr0 = 0;
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_CR0, &cr0);
        cr0 &= ~(1ULL << 3);
        hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CR0, cr0);
        uint64_t shadow = 0;
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_CTRL_CR0_SHADOW, &shadow);
        shadow &= ~(1ULL << 3);
        hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_CR0_SHADOW, shadow);
    } else if (access_type == 3) {
        // LMSW
        uint64_t lmsw_val = (exit_qual >> 16) & 0xFFFF;
        uint64_t cr0 = 0;
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_GUEST_CR0, &cr0);
        cr0 = (cr0 & ~0xEULL) | (lmsw_val & 0xE) | CR0_PE;
        cr0 = FixCr0(cr0);
        hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_GUEST_CR0, cr0);
        uint64_t shadow = 0;
        hv_vmx_vcpu_read_vmcs(vcpuid_, VMCS_CTRL_CR0_SHADOW, &shadow);
        shadow = (shadow & ~0xEULL) | (lmsw_val & 0xE) | CR0_PE;
        hv_vmx_vcpu_write_vmcs(vcpuid_, VMCS_CTRL_CR0_SHADOW, shadow);
    }

    AdvanceRip();
    return VCpuExitAction::kContinue;
}

VCpuExitAction HvfVCpu::HandleXsetbv() {
    uint64_t rcx = 0, rax = 0, rdx = 0;
    hv_vcpu_read_register(vcpuid_, HV_X86_RCX, &rcx);
    hv_vcpu_read_register(vcpuid_, HV_X86_RAX, &rax);
    hv_vcpu_read_register(vcpuid_, HV_X86_RDX, &rdx);

    if (rcx == 0) {
        uint64_t xcr0 = (rdx << 32) | (rax & 0xFFFFFFFF);
        hv_vcpu_write_register(vcpuid_, HV_X86_XCR0, xcr0);
    }

    AdvanceRip();
    return VCpuExitAction::kContinue;
}

} // namespace hvf
