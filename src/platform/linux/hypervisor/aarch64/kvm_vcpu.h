#pragma once

#include "core/vmm/address_space.h"
#include "core/vmm/hypervisor_vcpu.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

struct kvm_run;

namespace kvm {

class KvmVm;

class KvmVCpu final : public HypervisorVCpu {
public:
    ~KvmVCpu() override;

    static std::unique_ptr<KvmVCpu> Create(KvmVm& vm, uint32_t index,
                                           AddressSpace* addr_space);

    VCpuExitAction RunOnce() override;
    void CancelRun() override;
    uint32_t Index() const override { return index_; }

    // Core HypervisorVCpu interface requires SetupBootRegisters; on aarch64
    // the real work happens through SetupAarch64Boot, invoked by
    // Aarch64Machine::SetupBootVCpu. Keep SetupBootRegisters as a no-op for
    // symmetry with the HVF backend.
    bool SetupBootRegisters(uint8_t* ram) override;

    // BSP startup: set PC, X0=FDT, PSTATE=0x3C5 (EL1h, DAIF masked). Called
    // once from the Aarch64Machine on the BSP thread *before* it enters
    // RunOnce. Must run on the vCPU's own worker thread.
    // Matches the HVF signature so aarch64_machine.cpp can share code paths.
    bool SetupAarch64Boot(uint64_t entry_pc, uint64_t fdt_addr);

    void OnThreadInit() override;

    bool WaitForInterrupt(uint32_t timeout_ms) override;

    // KVM's in-kernel PSCI handles AP bring-up entirely in the kernel: APs
    // are created in POWER_OFF state and KVM_RUN blocks until a
    // PSCI_CPU_ON HVC is dispatched. Userspace SIPI/PSCI callbacks never
    // fire, so the generic startup-wait would deadlock.
    bool NeedsStartupWait() const override { return false; }

    // Shutdown/reset callbacks invoked when KVM surfaces a PSCI
    // SYSTEM_OFF / SYSTEM_RESET via KVM_EXIT_SYSTEM_EVENT. Wired by
    // Vm::SetupVCpuCallbacks so the Vm can RequestStop/RequestReboot.
    using ShutdownCallback = std::function<void()>;
    using RebootCallback = std::function<void()>;
    void SetShutdownCallback(ShutdownCallback cb) { shutdown_cb_ = std::move(cb); }
    void SetRebootCallback(RebootCallback cb) { reboot_cb_ = std::move(cb); }

private:
    KvmVCpu() = default;

    bool InitVcpu(bool power_off);

    uint32_t index_ = 0;
    int vcpu_fd_ = -1;
    struct kvm_run* run_ = nullptr;
    size_t run_size_ = 0;

    KvmVm* vm_ = nullptr;
    AddressSpace* addr_space_ = nullptr;

    // CancelRun writes immediate_exit = 1 and raises SIGUSR1 on the vCPU
    // thread. OnThreadInit stashes the pthread id so CancelRun can deliver
    // the signal to the right thread.
    std::atomic<unsigned long> thread_id_{0};

    ShutdownCallback shutdown_cb_;
    RebootCallback reboot_cb_;
};

} // namespace kvm
