#pragma once

#include "platform/windows/hypervisor/whvp_vm.h"
#include "core/vmm/address_space.h"
#include "core/vmm/hypervisor_vcpu.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>

namespace whvp {

class WhvpVCpu : public HypervisorVCpu {
public:
    ~WhvpVCpu() override;

    static std::unique_ptr<WhvpVCpu> Create(WhvpVm& vm, uint32_t vp_index,
                                             AddressSpace* addr_space);

    VCpuExitAction RunOnce() override;
    void CancelRun() override;
    uint32_t Index() const override { return vp_index_; }
    uint32_t ApicId() const override { return apic_id_; }
    bool SetupBootRegisters(uint8_t* ram) override;

    // Thread-local LAPIC CPU index must be set on the worker thread.
    void OnThreadInit() override;
    // On the hard-APIC path WHPX's built-in xAPIC emulation puts APs into
    // wait-for-SIPI at partition start and handles INIT/SIPI IPIs itself.
    // On the soft-APIC path (pre-1809 WHPX or TENBOX_SOFT_APIC=1) the
    // partition has no LAPIC, so APs would otherwise start executing the
    // reset vector immediately. Gate AP worker threads on the generic
    // init+SIPI CV and set CS:IP = (vector<<12):0 in OnStartup.
    bool NeedsStartupWait() const override;
    void OnStartup(const VCpuStartupState& state) override;

    // Soft-APIC path: queue an external interrupt for this vCPU. Wakes the
    // interrupt condition variable so a halted vCPU exits WaitForInterrupt.
    // Unlike CancelRun() we do NOT synchronously yank the vCPU out of
    // WHvRunVirtualProcessor here — the caller (WhvpVm) is expected to do
    // that after posting the vector so a single queued IRQ can target
    // multiple vCPUs with a single CancelRun each.
    void QueueInterrupt(uint32_t vector);
    bool HasPendingInterrupt();
    bool WaitForInterrupt(uint32_t timeout_ms) override;

    bool SetRegisters(const WHV_REGISTER_NAME* names,
                      const WHV_REGISTER_VALUE* values, uint32_t count);
    bool GetRegisters(const WHV_REGISTER_NAME* names,
                      WHV_REGISTER_VALUE* values, uint32_t count);

    WHV_PARTITION_HANDLE Partition() const { return partition_; }
    uint32_t VpIndex() const { return vp_index_; }

    WhvpVCpu(const WhvpVCpu&) = delete;
    WhvpVCpu& operator=(const WhvpVCpu&) = delete;

    // Global exit stats shared across all vCPUs, guarded by atomic ops.
    // Enable with WhvpVCpu::EnableExitStats(true).
    struct ExitStats {
        std::atomic<uint64_t> total{0};
        // Top-level exit type counters
        std::atomic<uint64_t> io{0}, mmio{0}, hlt{0}, canceled{0};
        std::atomic<uint64_t> cpuid{0}, msr{0}, irq_wnd{0}, apic_eoi{0};
        std::atomic<uint64_t> unsupported{0}, exception{0}, invalid_reg{0}, other{0};
        // I/O port breakdown
        std::atomic<uint64_t> io_uart{0}, io_pit{0}, io_acpi{0}, io_pci{0};
        std::atomic<uint64_t> io_pic{0}, io_rtc{0}, io_sink{0}, io_other{0};
        // MMIO LAPIC breakdown
        std::atomic<uint64_t> mmio_lapic_eoi{0}, mmio_lapic_tpr{0}, mmio_lapic_icr{0};
        std::atomic<uint64_t> mmio_lapic_timer{0}, mmio_lapic_other{0};
        std::atomic<uint64_t> mmio_ioapic{0}, mmio_other{0};
        // Per-virtio-device MMIO breakdown (8 slots at 0xD0000000 + i*0x200)
        static constexpr int kVirtioDevCount = 8;
        std::atomic<uint64_t> virtio_dev[kVirtioDevCount]{}; // blk,net,kbd,tablet,gpu,serial,fs,snd
        std::atomic<uint64_t> mmio_non_virtio{0};
        // MSR write breakdown
        std::atomic<uint64_t> wrmsr_kvmclock{0}, wrmsr_efer{0}, wrmsr_apicbase{0}, wrmsr_other{0};
        std::atomic<uint32_t> wrmsr_top_msr{0};
        std::atomic<uint64_t> wrmsr_top_count{0};
        // MSR read breakdown
        std::atomic<uint64_t> rdmsr_apicbase{0}, rdmsr_other{0};

        std::atomic<uint64_t> last_print_time{0};
    };
    static ExitStats s_stats_;
    static std::atomic<bool> s_stats_enabled_;
    static void PrintExitStats();
    static void EnableExitStats(bool on) { s_stats_enabled_.store(on, std::memory_order_relaxed); }

private:
    WhvpVCpu() = default;

    bool CreateEmulator();

    // Soft-APIC path: inspect guest RFLAGS.IF / InterruptState, inject one
    // pending vector via WHvRegisterPendingInterruption if the guest is
    // ready, otherwise request an interrupt-window exit via
    // WHvX64RegisterDeliverability so we get called back as soon as the
    // guest re-enables interrupts. No-op on the hard-APIC path.
    // Must be called on the vCPU worker thread immediately before each
    // WHvRunVirtualProcessor.
    void TryInjectInterrupt();
    void SetInterruptWindowRequested(bool on);

    VCpuExitAction HandleIoPort(const WHV_VP_EXIT_CONTEXT& vp_ctx,
                                 const WHV_X64_IO_PORT_ACCESS_CONTEXT& io);
    VCpuExitAction HandleMmio(const WHV_VP_EXIT_CONTEXT& vp_ctx,
                               const WHV_MEMORY_ACCESS_CONTEXT& mem);

    static HRESULT CALLBACK OnIoPort(
        VOID* ctx, WHV_EMULATOR_IO_ACCESS_INFO* io);
    static HRESULT CALLBACK OnMemory(
        VOID* ctx, WHV_EMULATOR_MEMORY_ACCESS_INFO* mem);
    static HRESULT CALLBACK OnGetRegisters(
        VOID* ctx, const WHV_REGISTER_NAME* names,
        UINT32 count, WHV_REGISTER_VALUE* values);
    static HRESULT CALLBACK OnSetRegisters(
        VOID* ctx, const WHV_REGISTER_NAME* names,
        UINT32 count, const WHV_REGISTER_VALUE* values);
    static HRESULT CALLBACK OnTranslateGva(
        VOID* ctx, WHV_GUEST_VIRTUAL_ADDRESS gva,
        WHV_TRANSLATE_GVA_FLAGS flags,
        WHV_TRANSLATE_GVA_RESULT_CODE* result,
        WHV_GUEST_PHYSICAL_ADDRESS* gpa);

    WhvpVm* vm_ = nullptr;
    WHV_PARTITION_HANDLE partition_ = nullptr;
    uint32_t vp_index_ = 0;
    uint32_t apic_id_ = 0;
    AddressSpace* addr_space_ = nullptr;
    WHV_EMULATOR_HANDLE emulator_ = nullptr;

    // Soft-APIC interrupt plumbing (ignored on the hard-APIC path).
    std::mutex irq_mutex_;
    std::condition_variable irq_cv_;
    std::queue<uint32_t> pending_irqs_;
    bool irq_window_active_ = false;

    // Cached from exit_ctx.VpContext.ExecutionState after every
    // WHvRunVirtualProcessor. TryInjectInterrupt consults these instead of
    // calling WHvGetVirtualProcessorRegisters, which would otherwise return
    // the stale snapshot of WHvRegisterPendingInterruption (that register
    // only changes on actual VM entry — between exits and our re-entry it
    // keeps whatever value we wrote last time, falsely reporting
    // "InterruptionPending=1" and blocking all further injections). This
    // mirrors whpx_vcpu_post_run() / whpx_vcpu_pre_run() in QEMU.
    bool cached_interruption_pending_ = false;
    bool cached_interruptable_ = true;  // RFLAGS.IF=1 && !InterruptShadow
};

// ---------------------------------------------------------------------------
// Hyper-V enlightenment helpers (implemented in whvp_vcpu.cpp).
// ---------------------------------------------------------------------------

// Publish the measured TSC frequency (Hz). Must be called once during VM
// setup, before any vCPU executes guest code. Used to derive TscScale for the
// reference TSC page and to answer MSR 0x40000022 (HV_X64_MSR_TSC_FREQUENCY).
void SetHypervTscFrequency(uint64_t tsc_hz);

// Record a guest-RAM backing mapping so the Hyper-V overlay helpers can
// translate GPA -> host virtual address and write directly (bypassing
// WHvWriteGpaRange, which has proven unreliable for 4 KiB overlays on some
// WHPX builds). Called from WhvpVm::MapMemory for each RAM range we map.
void RegisterRamMapping(uint64_t gpa, void* hva, uint64_t size);

} // namespace whvp
