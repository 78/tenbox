#pragma once

#include "platform/windows/hypervisor/whvp_vm.h"
#include "core/vmm/address_space.h"
#include "core/vmm/hypervisor_vcpu.h"
#include <atomic>

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
    // WHVP xAPIC emulation handles INIT/SIPI internally; no generic wait needed.
    bool NeedsStartupWait() const override { return false; }

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

    WHV_PARTITION_HANDLE partition_ = nullptr;
    uint32_t vp_index_ = 0;
    uint32_t apic_id_ = 0;
    AddressSpace* addr_space_ = nullptr;
    WHV_EMULATOR_HANDLE emulator_ = nullptr;
};

} // namespace whvp
