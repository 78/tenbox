#pragma once

#include "core/vmm/hypervisor_vcpu.h"
#include "core/vmm/address_space.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>

#include <Hypervisor/Hypervisor.h>

namespace hvf {

// PSCI function IDs (SMC Calling Convention)
static constexpr uint32_t kPsciCpuOn64      = 0xC4000003;
static constexpr uint32_t kPsciCpuOff       = 0x84000002;
static constexpr uint32_t kPsciSystemOff    = 0x84000008;
static constexpr uint32_t kPsciSystemReset  = 0x84000009;
static constexpr uint32_t kPsciVersion      = 0x84000000;
static constexpr uint32_t kPsciFeaturesCall = 0x8400000A;

struct PsciCpuOnRequest {
    uint32_t target_cpu;
    uint64_t entry_addr;
    uint64_t context_id;
};

using PsciCpuOnCallback = std::function<int(const PsciCpuOnRequest&)>;
using PsciShutdownCallback = std::function<void()>;
using PsciRebootCallback = std::function<void()>;

class HvfVCpu final : public HypervisorVCpu {
public:
    ~HvfVCpu() override;

    static std::unique_ptr<HvfVCpu> Create(uint32_t index, AddressSpace* addr_space);

    VCpuExitAction RunOnce() override;
    void CancelRun() override;
    uint32_t Index() const override { return index_; }
    bool SetupBootRegisters(uint8_t* ram) override;

    void OnStartup(const VCpuStartupState& state) override;

    bool SetupAarch64Boot(uint64_t entry_pc, uint64_t fdt_addr);
    bool SetupSecondaryCpu(uint64_t entry_pc, uint64_t context_id);

    void SetPsciCpuOnCallback(PsciCpuOnCallback cb) { psci_cpu_on_cb_ = std::move(cb); }
    void SetPsciShutdownCallback(PsciShutdownCallback cb) { psci_shutdown_cb_ = std::move(cb); }
    void SetPsciRebootCallback(PsciRebootCallback cb) { psci_reboot_cb_ = std::move(cb); }

    static void EnableExitStats(bool on) { s_stats_enabled_.store(on, std::memory_order_relaxed); }

private:
    HvfVCpu() = default;

    VCpuExitAction HandleException();
    VCpuExitAction HandleHvc();
    VCpuExitAction HandleDataAbort(uint64_t syndrome);
    bool DoMmioRead(uint64_t gpa, uint8_t size, uint8_t reg);
    bool DoMmioWrite(uint64_t gpa, uint8_t size, uint64_t value);

    struct ExitStats {
        std::atomic<uint64_t> total{0};
        std::atomic<uint64_t> exception{0};
        std::atomic<uint64_t> vtimer_activated{0};
        std::atomic<uint64_t> canceled{0};

        std::atomic<uint64_t> ec_wfi_wfe{0};
        std::atomic<uint64_t> ec_hvc64{0};
        std::atomic<uint64_t> ec_smc64{0};
        std::atomic<uint64_t> ec_sysreg{0};
        std::atomic<uint64_t> ec_dabt_lower{0};
        std::atomic<uint64_t> ec_dabt_curr{0};
        std::atomic<uint64_t> ec_brk{0};
        std::atomic<uint64_t> ec_other{0};

        std::atomic<uint64_t> hvc_version{0};
        std::atomic<uint64_t> hvc_features{0};
        std::atomic<uint64_t> hvc_cpu_on{0};
        std::atomic<uint64_t> hvc_cpu_off{0};
        std::atomic<uint64_t> hvc_system_off{0};
        std::atomic<uint64_t> hvc_system_reset{0};
        std::atomic<uint64_t> hvc_unknown{0};

        std::atomic<uint64_t> dabt_read{0};
        std::atomic<uint64_t> dabt_write{0};
        std::atomic<uint64_t> dabt_isv_valid{0};
        std::atomic<uint64_t> dabt_isv_invalid{0};

        std::atomic<uint64_t> last_print_time{0};
    };
    static ExitStats s_stats_;
    static std::atomic<bool> s_stats_enabled_;
    static void PrintExitStats();

    uint32_t index_ = 0;
    hv_vcpu_t vcpu_ = 0;
    hv_vcpu_exit_t* vcpu_exit_ = nullptr;
    AddressSpace* addr_space_ = nullptr;
    bool created_ = false;
    bool vtimer_masked_ = false;
    uint32_t vtimer_intid_ = 0;

    PsciCpuOnCallback psci_cpu_on_cb_;
    PsciShutdownCallback psci_shutdown_cb_;
    PsciRebootCallback psci_reboot_cb_;
};

} // namespace hvf
