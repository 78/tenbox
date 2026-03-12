#pragma once

#include "core/vmm/hypervisor_vcpu.h"
#include "core/vmm/hypervisor_vm.h"
#include "core/vmm/address_space.h"
#include <cstdint>
#include <memory>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>

#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>

namespace hvf {

class HvfVCpu final : public HypervisorVCpu {
public:
    ~HvfVCpu() override;

    static std::unique_ptr<HvfVCpu> Create(uint32_t index, AddressSpace* addr_space,
                                              uint8_t* ram = nullptr, uint64_t ram_size = 0,
                                              const GuestMemMap* guest_mem = nullptr);

    VCpuExitAction RunOnce() override;
    void CancelRun() override;
    uint32_t Index() const override { return index_; }
    bool SetupBootRegisters(uint8_t* ram) override;

    void OnThreadInit() override;
    void OnStartup(const VCpuStartupState& state) override;

    void QueueInterrupt(uint32_t vector);
    void WakeFromHalt();
    bool HasPendingInterrupt();
    bool WaitForInterrupt(uint32_t timeout_ms) override;

    // Set AP to start in real mode at CS:IP = (sipi_vector<<8):0000
    bool SetupSipiRegisters(uint8_t sipi_vector);

private:
    HvfVCpu() = default;

    bool SetupVmcs();

    VCpuExitAction HandleIo(uint64_t exit_qual);
    VCpuExitAction HandleEptViolation(uint64_t exit_qual);
    VCpuExitAction HandleCpuid();
    VCpuExitAction HandleMsr(bool is_write);
    VCpuExitAction HandleHlt();
    VCpuExitAction HandleCrAccess(uint64_t exit_qual);
    VCpuExitAction HandleXsetbv();

    void AdvanceRip();
    void TryInjectInterrupt();

    enum class MmioOp : uint8_t {
        kMovLoad,    // MOV reg <- [mem]
        kMovStore,   // MOV [mem] <- reg
        kMovStoreImm,// MOV [mem] <- imm
        kMovzx,      // MOVZX reg <- [mem]
        kMovsx,      // MOVSX reg <- [mem]
        kTest,       // TEST [mem], imm (flags only, no write-back)
        kAnd,        // AND [mem], reg/imm (read-modify-write)
        kOr,         // OR  [mem], reg/imm (read-modify-write)
        kXor,        // XOR [mem], reg/imm (read-modify-write)
        kCmpxchg,    // CMPXCHG [mem], reg
    };

    struct MmioDecodeResult {
        MmioOp op;
        hv_x86_reg_t reg;
        uint8_t size;
        bool is_write;
        bool has_imm;
        uint8_t insn_len;
        uint64_t imm_value;
    };
    bool DecodeMmioInsn(uint64_t rip_gpa, MmioDecodeResult& out, const uint8_t* code);

    static uint64_t Cap2Ctrl(uint64_t cap, uint64_t ctrl);

    void UpdateKvmclock(uint64_t gpa);

    uint32_t index_ = 0;
    hv_vcpuid_t vcpuid_ = 0;
    AddressSpace* addr_space_ = nullptr;
    uint8_t* ram_ = nullptr;
    uint64_t ram_size_ = 0;
    const GuestMemMap* guest_mem_ = nullptr;
    bool created_ = false;

    uint64_t tsc_freq_ = 0;
    uint64_t kvmclock_system_time_gpa_ = 0;
    bool kvmclock_enabled_ = false;

    // IA32_APIC_BASE: bit 11 = APIC enable, bit 8 = BSP, base addr = 0xFEE00000
    uint64_t apic_base_msr_ = 0;

    uint64_t last_ept_gpa_ = 0;
    uint32_t last_decode_fail_count_ = 0;

    // Global exit stats shared across all vCPUs, guarded by atomic ops.
    // Enable with HvfVCpu::EnableExitStats(true).
    struct ExitStats {
        std::atomic<uint64_t> total{0};
        std::atomic<uint64_t> irq{0}, irq_wnd{0}, hlt{0}, io{0}, ept{0};
        std::atomic<uint64_t> cpuid{0}, rdmsr{0}, wrmsr{0}, cr{0}, xsetbv{0}, other{0};
        // I/O port breakdown
        std::atomic<uint64_t> io_uart{0}, io_pit{0}, io_acpi{0}, io_pci{0};
        std::atomic<uint64_t> io_pic{0}, io_rtc{0}, io_sink{0}, io_other{0};
        // EPT LAPIC breakdown
        std::atomic<uint64_t> ept_lapic_eoi{0}, ept_lapic_tpr{0}, ept_lapic_icr{0};
        std::atomic<uint64_t> ept_lapic_timer{0}, ept_lapic_other{0};
        std::atomic<uint64_t> ept_ioapic{0}, ept_other{0};
        // CR breakdown
        std::atomic<uint64_t> cr0{0}, cr3{0}, cr4{0}, cr8{0}, cr_other{0};
        // MSR write breakdown
        std::atomic<uint64_t> wrmsr_kvmclock{0}, wrmsr_wallclock{0}, wrmsr_poll{0};
        std::atomic<uint64_t> wrmsr_efer{0}, wrmsr_apicbase{0}, wrmsr_other{0};
        std::atomic<uint32_t> wrmsr_top_msr{0};
        std::atomic<uint64_t> wrmsr_top_count{0};
        // MSR read breakdown
        std::atomic<uint64_t> rdmsr_apicbase{0}, rdmsr_other{0};

        std::atomic<uint64_t> last_print_time{0};
    };
    static ExitStats s_stats_;
    static std::atomic<bool> s_stats_enabled_;
    static void PrintExitStats();
public:
    static void EnableExitStats(bool on) { s_stats_enabled_.store(on, std::memory_order_relaxed); }
private:

    std::mutex irq_mutex_;
    std::condition_variable irq_cv_;
    std::queue<uint32_t> pending_irqs_;
    bool irq_window_active_ = false;

};

} // namespace hvf
