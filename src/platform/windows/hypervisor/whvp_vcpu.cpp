#include "platform/windows/hypervisor/whvp_vcpu.h"
#include "core/arch/x86_64/boot.h"
#include "core/device/irq/local_apic.h"
#include <chrono>
#include <cinttypes>
#include <cstdio>

namespace whvp {

// Static definitions
WhvpVCpu::ExitStats WhvpVCpu::s_stats_{};
std::atomic<bool>   WhvpVCpu::s_stats_enabled_{false};

// I/O port range helpers (mirrors HVF classification)
static bool IsUartPort(uint16_t port) {
    return (port >= 0x3F8 && port <= 0x3FF) ||  // COM1
           (port >= 0x2F8 && port <= 0x2FF);     // COM2
}
static bool IsPitPort(uint16_t port) {
    return port >= 0x40 && port <= 0x43;
}
static bool IsAcpiPort(uint16_t port) {
    return port == 0x600 || port == 0x604 ||     // ACPI PM1a event/ctrl
           (port >= 0xB000 && port <= 0xB0FF);   // ACPI GPE
}
static bool IsPicPort(uint16_t port) {
    return (port == 0x20 || port == 0x21) ||     // PIC1
           (port == 0xA0 || port == 0xA1);       // PIC2
}
static bool IsRtcPort(uint16_t port) {
    return port == 0x70 || port == 0x71;
}
static bool IsPciPort(uint16_t port) {
    return port == 0xCF8 || port == 0xCFC ||
           (port >= 0xCF8 && port <= 0xCFF);
}
// Sink ports: writes are silently ignored, reads return 0xFF
static bool IsSinkPort(uint16_t port) {
    return port == 0x80 ||                       // POST code port
           port == 0xED;                         // I/O delay port
}

// MMIO address range helpers
static constexpr uint64_t kLapicBase   = 0xFEE00000ULL;
static constexpr uint64_t kLapicEnd    = 0xFEE01000ULL;
static constexpr uint64_t kIoApicBase  = 0xFEC00000ULL;
static constexpr uint64_t kIoApicEnd   = 0xFEC01000ULL;
// LAPIC register offsets of interest
static constexpr uint32_t kLapicRegEoi   = 0x0B0;
static constexpr uint32_t kLapicRegTpr   = 0x080;
static constexpr uint32_t kLapicRegIcr   = 0x300;
static constexpr uint32_t kLapicRegTimer = 0x320;

void WhvpVCpu::PrintExitStats() {
    auto& s = s_stats_;
    uint64_t total = s.total.load(std::memory_order_relaxed);
    if (total == 0) return;

    fprintf(stderr,
        "[WHVP ExitStats] total=%" PRIu64 "\n"
        "  io=%" PRIu64 " mmio=%" PRIu64 " hlt=%" PRIu64
        " canceled=%" PRIu64 " cpuid=%" PRIu64 " msr=%" PRIu64
        " irq_wnd=%" PRIu64 " apic_eoi=%" PRIu64
        " unsupported=%" PRIu64 " exception=%" PRIu64
        " invalid_reg=%" PRIu64 " other=%" PRIu64 "\n"
        "  IO breakdown:"
        " uart=%" PRIu64 " pit=%" PRIu64 " acpi=%" PRIu64
        " pci=%" PRIu64 " pic=%" PRIu64 " rtc=%" PRIu64
        " sink=%" PRIu64 " other=%" PRIu64 "\n"
        "  MMIO LAPIC:"
        " eoi=%" PRIu64 " tpr=%" PRIu64 " icr=%" PRIu64
        " timer=%" PRIu64 " other=%" PRIu64
        "  ioapic=%" PRIu64 " mmio_other=%" PRIu64 "\n"
        "  WRMSR:"
        " kvmclock=%" PRIu64 " efer=%" PRIu64 " apicbase=%" PRIu64
        " other=%" PRIu64 " top_msr=0x%X(%" PRIu64 ")\n"
        "  RDMSR:"
        " apicbase=%" PRIu64 " other=%" PRIu64 "\n"
        "  MMIO top GPA pages:",
        total,
        s.io.load(), s.mmio.load(), s.hlt.load(),
        s.canceled.load(), s.cpuid.load(), s.msr.load(),
        s.irq_wnd.load(), s.apic_eoi.load(),
        s.unsupported.load(), s.exception.load(),
        s.invalid_reg.load(), s.other.load(),
        s.io_uart.load(), s.io_pit.load(), s.io_acpi.load(),
        s.io_pci.load(), s.io_pic.load(), s.io_rtc.load(),
        s.io_sink.load(), s.io_other.load(),
        s.mmio_lapic_eoi.load(), s.mmio_lapic_tpr.load(),
        s.mmio_lapic_icr.load(), s.mmio_lapic_timer.load(),
        s.mmio_lapic_other.load(), s.mmio_ioapic.load(), s.mmio_other.load(),
        s.wrmsr_kvmclock.load(), s.wrmsr_efer.load(),
        s.wrmsr_apicbase.load(), s.wrmsr_other.load(),
        s.wrmsr_top_msr.load(), s.wrmsr_top_count.load(),
        s.rdmsr_apicbase.load(), s.rdmsr_other.load());
    static const char* kVirtioNames[] = {
        "blk", "net", "kbd", "tablet", "gpu", "serial", "fs", "snd"
    };
    fprintf(stderr, "  VirtIO MMIO:");
    for (int i = 0; i < ExitStats::kVirtioDevCount; ++i) {
        uint64_t cnt = s.virtio_dev[i].load(std::memory_order_relaxed);
        if (cnt) fprintf(stderr, " %s=%" PRIu64, kVirtioNames[i], cnt);
    }
    uint64_t nv = s.mmio_non_virtio.load(std::memory_order_relaxed);
    if (nv) fprintf(stderr, " non_virtio=%" PRIu64, nv);
    fprintf(stderr, "\n");
}

WhvpVCpu::~WhvpVCpu() {
    if (emulator_) {
        WHvEmulatorDestroyEmulator(emulator_);
        emulator_ = nullptr;
    }
    if (partition_) {
        WHvDeleteVirtualProcessor(partition_, vp_index_);
    }
}

std::unique_ptr<WhvpVCpu> WhvpVCpu::Create(WhvpVm& vm, uint32_t vp_index,
                                             AddressSpace* addr_space) {
    auto vcpu = std::unique_ptr<WhvpVCpu>(new WhvpVCpu());
    vcpu->partition_ = vm.Handle();
    vcpu->vp_index_ = vp_index;
    vcpu->addr_space_ = addr_space;

    HRESULT hr = WHvCreateVirtualProcessor(vm.Handle(), vp_index, 0);
    if (FAILED(hr)) {
        LOG_ERROR("WHvCreateVirtualProcessor(%u) failed: 0x%08lX", vp_index, hr);
        return nullptr;
    }

    // Read the APIC ID that WHVP assigned (may differ from vp_index on
    // systems where the hypervisor maps host physical APIC IDs).
    WHV_REGISTER_NAME apic_name = WHvX64RegisterApicId;
    WHV_REGISTER_VALUE apic_val{};
    hr = WHvGetVirtualProcessorRegisters(vm.Handle(), vp_index, &apic_name, 1, &apic_val);
    if (SUCCEEDED(hr)) {
        // xAPIC: ID is in bits [31:24] of the register value
        vcpu->apic_id_ = static_cast<uint32_t>((apic_val.Reg64 >> 24) & 0xFF);
    } else {
        vcpu->apic_id_ = vp_index;
        LOG_WARN("Failed to read APIC ID for vCPU %u: 0x%08lX, assuming %u",
                 vp_index, hr, vp_index);
    }

    if (!vcpu->CreateEmulator()) {
        return nullptr;
    }

    LOG_INFO("vCPU %u created (APIC ID %u)", vp_index, vcpu->apic_id_);
    return vcpu;
}

void WhvpVCpu::OnThreadInit() {
    LocalApic::SetCurrentCpu(vp_index_);
}

bool WhvpVCpu::CreateEmulator() {
    WHV_EMULATOR_CALLBACKS cb{};
    cb.Size = sizeof(cb);
    cb.WHvEmulatorIoPortCallback = OnIoPort;
    cb.WHvEmulatorMemoryCallback = OnMemory;
    cb.WHvEmulatorGetVirtualProcessorRegisters = OnGetRegisters;
    cb.WHvEmulatorSetVirtualProcessorRegisters = OnSetRegisters;
    cb.WHvEmulatorTranslateGvaPage = OnTranslateGva;

    HRESULT hr = WHvEmulatorCreateEmulator(&cb, &emulator_);
    if (FAILED(hr)) {
        LOG_ERROR("WHvEmulatorCreateEmulator failed: 0x%08lX", hr);
        return false;
    }
    return true;
}

bool WhvpVCpu::SetRegisters(const WHV_REGISTER_NAME* names,
                             const WHV_REGISTER_VALUE* values, uint32_t count) {
    HRESULT hr = WHvSetVirtualProcessorRegisters(
        partition_, vp_index_, names, count, values);
    if (FAILED(hr)) {
        LOG_ERROR("WHvSetVirtualProcessorRegisters failed: 0x%08lX", hr);
        return false;
    }
    return true;
}

bool WhvpVCpu::GetRegisters(const WHV_REGISTER_NAME* names,
                             WHV_REGISTER_VALUE* values, uint32_t count) {
    HRESULT hr = WHvGetVirtualProcessorRegisters(
        partition_, vp_index_, names, count, values);
    if (FAILED(hr)) {
        LOG_ERROR("WHvGetVirtualProcessorRegisters failed: 0x%08lX", hr);
        return false;
    }
    return true;
}

void WhvpVCpu::CancelRun() {
    WHvCancelRunVirtualProcessor(partition_, vp_index_, 0);
}

bool WhvpVCpu::SetupBootRegisters(uint8_t* ram) {
    using x86::GdtEntry;
    namespace Layout = x86::Layout;

    // Write GDT into guest memory
    GdtEntry* gdt = reinterpret_cast<GdtEntry*>(ram + Layout::kGdtBase);
    gdt->null   = 0x0000000000000000ULL;
    gdt->unused = 0x0000000000000000ULL;
    // 32-bit code: base=0, limit=0xFFFFF, G=1 D=1 P=1 DPL=0 S=1 type=0xB
    gdt->code32 = 0x00CF9B000000FFFFULL;
    // 32-bit data: base=0, limit=0xFFFFF, G=1 D=1 P=1 DPL=0 S=1 type=0x3
    gdt->data32 = 0x00CF93000000FFFFULL;

    WHV_REGISTER_NAME names[64]{};
    WHV_REGISTER_VALUE values[64]{};
    uint32_t i = 0;

    // GDTR
    names[i] = WHvX64RegisterGdtr;
    values[i].Table.Base = Layout::kGdtBase;
    values[i].Table.Limit = sizeof(GdtEntry) - 1;
    i++;

    // IDTR (empty, kernel will set its own)
    names[i] = WHvX64RegisterIdtr;
    values[i].Table.Base = 0;
    values[i].Table.Limit = 0;
    i++;

    // CS = selector 0x10 (code segment)
    names[i] = WHvX64RegisterCs;
    values[i].Segment.Base = 0;
    values[i].Segment.Limit = 0xFFFFFFFF;
    values[i].Segment.Selector = 0x10;
    values[i].Segment.Attributes = 0xC09B;  // G=1 D=1 P=1 S=1 type=0xB
    i++;

    // DS, ES, SS = selector 0x18 (data segment)
    auto SetDataSeg = [&](WHV_REGISTER_NAME name) {
        names[i] = name;
        values[i].Segment.Base = 0;
        values[i].Segment.Limit = 0xFFFFFFFF;
        values[i].Segment.Selector = 0x18;
        values[i].Segment.Attributes = 0xC093;  // G=1 D=1 P=1 S=1 type=0x3
        i++;
    };
    SetDataSeg(WHvX64RegisterDs);
    SetDataSeg(WHvX64RegisterEs);
    SetDataSeg(WHvX64RegisterSs);

    // FS, GS with null selectors
    auto SetNullSeg = [&](WHV_REGISTER_NAME name) {
        names[i] = name;
        values[i].Segment.Base = 0;
        values[i].Segment.Limit = 0;
        values[i].Segment.Selector = 0;
        values[i].Segment.Attributes = 0;
        i++;
    };
    SetNullSeg(WHvX64RegisterFs);
    SetNullSeg(WHvX64RegisterGs);

    // TR (task register) - must be valid for WHVP
    names[i] = WHvX64RegisterTr;
    values[i].Segment.Base = 0;
    values[i].Segment.Limit = 0;
    values[i].Segment.Selector = 0;
    values[i].Segment.Attributes = 0x008B;  // P=1 type=busy 32-bit TSS
    i++;

    // LDTR
    names[i] = WHvX64RegisterLdtr;
    values[i].Segment.Base = 0;
    values[i].Segment.Limit = 0;
    values[i].Segment.Selector = 0;
    values[i].Segment.Attributes = 0x0082;  // P=1 type=LDT
    i++;

    // RIP = kernel entry point
    names[i] = WHvX64RegisterRip;
    values[i].Reg64 = Layout::kKernelBase;
    i++;

    // RSI = pointer to boot_params
    names[i] = WHvX64RegisterRsi;
    values[i].Reg64 = Layout::kBootParams;
    i++;

    // RFLAGS = bit 1 always set
    names[i] = WHvX64RegisterRflags;
    values[i].Reg64 = 0x2;
    i++;

    // CR0 = PE (protected mode) + ET (math coprocessor)
    names[i] = WHvX64RegisterCr0;
    values[i].Reg64 = 0x11;
    i++;

    // Zero out general purpose registers
    WHV_REGISTER_NAME gp_regs[] = {
        WHvX64RegisterRax, WHvX64RegisterRbx, WHvX64RegisterRcx,
        WHvX64RegisterRdx, WHvX64RegisterRdi, WHvX64RegisterRbp,
        WHvX64RegisterRsp,
    };
    for (auto name : gp_regs) {
        names[i] = name;
        values[i].Reg64 = 0;
        i++;
    }

    return SetRegisters(names, values, i);
}

VCpuExitAction WhvpVCpu::RunOnce() {
    WHV_RUN_VP_EXIT_CONTEXT exit_ctx{};
    HRESULT hr = WHvRunVirtualProcessor(
        partition_, vp_index_, &exit_ctx, sizeof(exit_ctx));
    if (FAILED(hr)) {
        LOG_ERROR("WHvRunVirtualProcessor failed: 0x%08lX", hr);
        return VCpuExitAction::kError;
    }

    const bool stats = s_stats_enabled_.load(std::memory_order_relaxed);
    if (stats) {
        s_stats_.total.fetch_add(1, std::memory_order_relaxed);

        // Print every ~5 seconds
        using namespace std::chrono;
        uint64_t now_ms = (uint64_t)duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count();
        uint64_t last = s_stats_.last_print_time.load(std::memory_order_relaxed);
        if (now_ms - last >= 5000 &&
            s_stats_.last_print_time.compare_exchange_weak(
                last, now_ms, std::memory_order_relaxed)) {
            PrintExitStats();
        }
    }

    switch (exit_ctx.ExitReason) {
    case WHvRunVpExitReasonX64IoPortAccess:
        if (stats) s_stats_.io.fetch_add(1, std::memory_order_relaxed);
        return HandleIoPort(exit_ctx.VpContext, exit_ctx.IoPortAccess);

    case WHvRunVpExitReasonMemoryAccess:
        if (stats) s_stats_.mmio.fetch_add(1, std::memory_order_relaxed);
        return HandleMmio(exit_ctx.VpContext, exit_ctx.MemoryAccess);

    case WHvRunVpExitReasonX64Halt: {
        if (stats) s_stats_.hlt.fetch_add(1, std::memory_order_relaxed);
        WHV_REGISTER_NAME rfl_name = WHvX64RegisterRflags;
        WHV_REGISTER_VALUE rfl_val{};
        GetRegisters(&rfl_name, &rfl_val, 1);
        if (!(rfl_val.Reg64 & 0x200)) {
            LOG_INFO("CPU halted with interrupts disabled — treating as shutdown");
            return VCpuExitAction::kShutdown;
        }
        return VCpuExitAction::kHalt;
    }

    case WHvRunVpExitReasonCanceled:
        if (stats) s_stats_.canceled.fetch_add(1, std::memory_order_relaxed);
        return VCpuExitAction::kContinue;

    case WHvRunVpExitReasonX64ApicEoi:
        if (stats) s_stats_.apic_eoi.fetch_add(1, std::memory_order_relaxed);
        return VCpuExitAction::kContinue;

    case WHvRunVpExitReasonUnsupportedFeature:
        if (stats) s_stats_.unsupported.fetch_add(1, std::memory_order_relaxed);
        LOG_WARN("Unsupported feature at RIP=0x%llX (feature=%u)",
                 exit_ctx.VpContext.Rip,
                 exit_ctx.UnsupportedFeature.FeatureCode);
        return VCpuExitAction::kContinue;

    case WHvRunVpExitReasonX64InterruptWindow:
        if (stats) s_stats_.irq_wnd.fetch_add(1, std::memory_order_relaxed);
        return VCpuExitAction::kContinue;

    case WHvRunVpExitReasonUnrecoverableException:
        if (stats) s_stats_.exception.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR("Unrecoverable guest exception at RIP=0x%llX",
                  exit_ctx.VpContext.Rip);
        return VCpuExitAction::kError;

    case WHvRunVpExitReasonInvalidVpRegisterValue:
        if (stats) s_stats_.invalid_reg.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR("Invalid VP register value at RIP=0x%llX",
                  exit_ctx.VpContext.Rip);
        return VCpuExitAction::kError;

    case WHvRunVpExitReasonX64Cpuid: {
        if (stats) s_stats_.cpuid.fetch_add(1, std::memory_order_relaxed);
        auto& cpuid = exit_ctx.CpuidAccess;
        WHV_REGISTER_NAME reg_names[] = {
            WHvX64RegisterRax, WHvX64RegisterRbx,
            WHvX64RegisterRcx, WHvX64RegisterRdx,
            WHvX64RegisterRip,
        };
        WHV_REGISTER_VALUE vals[5]{};
        vals[0].Reg64 = cpuid.DefaultResultRax;
        vals[1].Reg64 = cpuid.DefaultResultRbx;
        vals[2].Reg64 = cpuid.DefaultResultRcx;
        vals[3].Reg64 = cpuid.DefaultResultRdx;
        vals[4].Reg64 = exit_ctx.VpContext.Rip +
                         exit_ctx.VpContext.InstructionLength;

        if (cpuid.Rax == 1) {
            // EBX[31:24] = Initial APIC ID — must match MADT
            vals[1].Reg64 = (vals[1].Reg64 & 0x00FFFFFF) |
                            (static_cast<uint64_t>(apic_id_) << 24);
        } else if (cpuid.Rax == 0xB || cpuid.Rax == 0x1F) {
            // x2APIC topology leaves: EDX = x2APIC ID
            vals[3].Reg64 = apic_id_;
        }

        SetRegisters(reg_names, vals, 5);
        return VCpuExitAction::kContinue;
    }

    case WHvRunVpExitReasonX64MsrAccess: {
        if (stats) s_stats_.msr.fetch_add(1, std::memory_order_relaxed);
        auto& msr = exit_ctx.MsrAccess;
        WHV_REGISTER_NAME rip_name = WHvX64RegisterRip;
        WHV_REGISTER_VALUE rip_val{};
        rip_val.Reg64 = exit_ctx.VpContext.Rip +
                        exit_ctx.VpContext.InstructionLength;

        if (!msr.AccessInfo.IsWrite) {
            if (stats) {
                if (msr.MsrNumber == 0x1B)
                    s_stats_.rdmsr_apicbase.fetch_add(1, std::memory_order_relaxed);
                else
                    s_stats_.rdmsr_other.fetch_add(1, std::memory_order_relaxed);
            }
            WHV_REGISTER_NAME reg_names[] = {
                WHvX64RegisterRax, WHvX64RegisterRdx, WHvX64RegisterRip
            };
            WHV_REGISTER_VALUE vals[3]{};
            vals[0].Reg64 = 0;
            vals[1].Reg64 = 0;
            vals[2] = rip_val;
            SetRegisters(reg_names, vals, 3);
            LOG_DEBUG("MSR read: 0x%X -> 0", msr.MsrNumber);
        } else {
            if (stats) {
                switch (msr.MsrNumber) {
                case 0x11:  // MSR_KVM_SYSTEM_TIME / kvmclock
                case 0x4b564d01:
                    s_stats_.wrmsr_kvmclock.fetch_add(1, std::memory_order_relaxed); break;
                case 0xC0000080:  // IA32_EFER
                    s_stats_.wrmsr_efer.fetch_add(1, std::memory_order_relaxed); break;
                case 0x1B:  // IA32_APIC_BASE
                    s_stats_.wrmsr_apicbase.fetch_add(1, std::memory_order_relaxed); break;
                default: {
                    s_stats_.wrmsr_other.fetch_add(1, std::memory_order_relaxed);
                    // Track the single most frequent "other" MSR
                    uint32_t top = s_stats_.wrmsr_top_msr.load(std::memory_order_relaxed);
                    if (top == msr.MsrNumber) {
                        s_stats_.wrmsr_top_count.fetch_add(1, std::memory_order_relaxed);
                    } else if (top == 0) {
                        uint32_t expected = 0;
                        if (s_stats_.wrmsr_top_msr.compare_exchange_weak(
                                expected, msr.MsrNumber, std::memory_order_relaxed)) {
                            s_stats_.wrmsr_top_count.store(1, std::memory_order_relaxed);
                        }
                    }
                    break;
                }
                }
            }
            LOG_DEBUG("MSR write: 0x%X = 0x%llX", msr.MsrNumber,
                      (msr.Rdx << 32) | (msr.Rax & 0xFFFFFFFF));
            SetRegisters(&rip_name, &rip_val, 1);
        }
        return VCpuExitAction::kContinue;
    }

    default:
        if (stats) s_stats_.other.fetch_add(1, std::memory_order_relaxed);
        LOG_WARN("Unhandled VM exit reason: 0x%X at RIP=0x%llX",
                 exit_ctx.ExitReason, exit_ctx.VpContext.Rip);
        return VCpuExitAction::kError;
    }
}

VCpuExitAction WhvpVCpu::HandleIoPort(
    const WHV_VP_EXIT_CONTEXT& vp_ctx,
    const WHV_X64_IO_PORT_ACCESS_CONTEXT& io) {

    WHV_EMULATOR_STATUS status{};
    HRESULT hr = WHvEmulatorTryIoEmulation(
        emulator_, this, &vp_ctx, &io, &status);

    if (FAILED(hr) || !status.EmulationSuccessful) {
        LOG_WARN("IO emulation failed: port=0x%X hr=0x%08lX success=%d",
                 io.PortNumber, hr, status.EmulationSuccessful);
        WHV_REGISTER_NAME name = WHvX64RegisterRip;
        WHV_REGISTER_VALUE val{};
        val.Reg64 = vp_ctx.Rip + vp_ctx.InstructionLength;
        SetRegisters(&name, &val, 1);
    }
    return VCpuExitAction::kContinue;
}

VCpuExitAction WhvpVCpu::HandleMmio(
    const WHV_VP_EXIT_CONTEXT& vp_ctx,
    const WHV_MEMORY_ACCESS_CONTEXT& mem) {

    WHV_EMULATOR_STATUS status{};
    HRESULT hr = WHvEmulatorTryMmioEmulation(
        emulator_, this, &vp_ctx, &mem, &status);

    if (FAILED(hr) || !status.EmulationSuccessful) {
        LOG_WARN("MMIO emulation failed: gpa=0x%llX hr=0x%08lX success=%d",
                 mem.Gpa, hr, status.EmulationSuccessful);
        WHV_REGISTER_NAME name = WHvX64RegisterRip;
        WHV_REGISTER_VALUE val{};
        val.Reg64 = vp_ctx.Rip + vp_ctx.InstructionLength;
        SetRegisters(&name, &val, 1);
    }
    return VCpuExitAction::kContinue;
}

// --- Emulator Callbacks ---

HRESULT CALLBACK WhvpVCpu::OnIoPort(
    VOID* ctx, WHV_EMULATOR_IO_ACCESS_INFO* io) {
    auto* vcpu = static_cast<WhvpVCpu*>(ctx);

    if (s_stats_enabled_.load(std::memory_order_relaxed)) {
        auto& s = s_stats_;
        uint16_t port = io->Port;
        if      (IsUartPort(port)) s.io_uart.fetch_add(1, std::memory_order_relaxed);
        else if (IsPitPort(port))  s.io_pit.fetch_add(1, std::memory_order_relaxed);
        else if (IsAcpiPort(port)) s.io_acpi.fetch_add(1, std::memory_order_relaxed);
        else if (IsPciPort(port))  s.io_pci.fetch_add(1, std::memory_order_relaxed);
        else if (IsPicPort(port))  s.io_pic.fetch_add(1, std::memory_order_relaxed);
        else if (IsRtcPort(port))  s.io_rtc.fetch_add(1, std::memory_order_relaxed);
        else if (IsSinkPort(port)) s.io_sink.fetch_add(1, std::memory_order_relaxed);
        else                       s.io_other.fetch_add(1, std::memory_order_relaxed);
    }

    if (io->Direction == 0) {
        uint32_t val = 0;
        vcpu->addr_space_->HandlePortIn(io->Port, (uint8_t)io->AccessSize, &val);
        io->Data = val;
    } else {
        vcpu->addr_space_->HandlePortOut(
            io->Port, (uint8_t)io->AccessSize, io->Data);
    }
    return S_OK;
}

HRESULT CALLBACK WhvpVCpu::OnMemory(
    VOID* ctx, WHV_EMULATOR_MEMORY_ACCESS_INFO* mem) {
    auto* vcpu = static_cast<WhvpVCpu*>(ctx);

    if (s_stats_enabled_.load(std::memory_order_relaxed)) {
        auto& s = s_stats_;
        uint64_t gpa = mem->GpaAddress;
        if (gpa >= kLapicBase && gpa < kLapicEnd) {
            uint32_t off = (uint32_t)(gpa - kLapicBase);
            if      (off == kLapicRegEoi)   s.mmio_lapic_eoi.fetch_add(1, std::memory_order_relaxed);
            else if (off == kLapicRegTpr)   s.mmio_lapic_tpr.fetch_add(1, std::memory_order_relaxed);
            else if (off == kLapicRegIcr)   s.mmio_lapic_icr.fetch_add(1, std::memory_order_relaxed);
            else if (off == kLapicRegTimer) s.mmio_lapic_timer.fetch_add(1, std::memory_order_relaxed);
            else                            s.mmio_lapic_other.fetch_add(1, std::memory_order_relaxed);
        } else if (gpa >= kIoApicBase && gpa < kIoApicEnd) {
            s.mmio_ioapic.fetch_add(1, std::memory_order_relaxed);
        } else {
            s.mmio_other.fetch_add(1, std::memory_order_relaxed);
            constexpr uint64_t kVirtioBase = 0xD0000000ULL;
            constexpr uint64_t kVirtioStride = 0x200;
            constexpr uint64_t kVirtioEnd = kVirtioBase + kVirtioStride * ExitStats::kVirtioDevCount;
            if (gpa >= kVirtioBase && gpa < kVirtioEnd) {
                int idx = (int)((gpa - kVirtioBase) / kVirtioStride);
                s.virtio_dev[idx].fetch_add(1, std::memory_order_relaxed);
            } else {
                s.mmio_non_virtio.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    if (mem->Direction == 0) {
        uint64_t val = 0;
        vcpu->addr_space_->HandleMmioRead(
            mem->GpaAddress, mem->AccessSize, &val);
        memcpy(mem->Data, &val, mem->AccessSize);
    } else {
        uint64_t val = 0;
        memcpy(&val, mem->Data, mem->AccessSize);
        vcpu->addr_space_->HandleMmioWrite(
            mem->GpaAddress, mem->AccessSize, val);
    }
    return S_OK;
}

HRESULT CALLBACK WhvpVCpu::OnGetRegisters(
    VOID* ctx, const WHV_REGISTER_NAME* names,
    UINT32 count, WHV_REGISTER_VALUE* values) {
    auto* vcpu = static_cast<WhvpVCpu*>(ctx);
    return WHvGetVirtualProcessorRegisters(
        vcpu->partition_, vcpu->vp_index_, names, count, values);
}

HRESULT CALLBACK WhvpVCpu::OnSetRegisters(
    VOID* ctx, const WHV_REGISTER_NAME* names,
    UINT32 count, const WHV_REGISTER_VALUE* values) {
    auto* vcpu = static_cast<WhvpVCpu*>(ctx);
    return WHvSetVirtualProcessorRegisters(
        vcpu->partition_, vcpu->vp_index_, names, count, values);
}

HRESULT CALLBACK WhvpVCpu::OnTranslateGva(
    VOID* ctx, WHV_GUEST_VIRTUAL_ADDRESS gva,
    WHV_TRANSLATE_GVA_FLAGS flags,
    WHV_TRANSLATE_GVA_RESULT_CODE* result_code,
    WHV_GUEST_PHYSICAL_ADDRESS* gpa) {
    auto* vcpu = static_cast<WhvpVCpu*>(ctx);
    WHV_TRANSLATE_GVA_RESULT result{};
    HRESULT hr = WHvTranslateGva(
        vcpu->partition_, vcpu->vp_index_,
        gva, flags, &result, gpa);
    if (SUCCEEDED(hr)) {
        *result_code = result.ResultCode;
    }
    return hr;
}

} // namespace whvp
