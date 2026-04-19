#pragma once

#include "core/vmm/address_space.h"
#include "core/vmm/hypervisor_vcpu.h"

#include <atomic>
#include <cstdint>
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

    bool SetupBootRegisters(uint8_t* ram) override;

    void OnThreadInit() override;

    bool WaitForInterrupt(uint32_t timeout_ms) override;

    // KVM delivers IPIs through the in-kernel LAPIC, so these are no-ops.
    void OnStartup(const VCpuStartupState&) override {}

    // KVM's in-kernel LAPIC intercepts BSP's ICR writes and handles the
    // INIT-SIPI-SIPI sequence entirely in the kernel: APs start in
    // MP_STATE_UNINITIALIZED and KVM_RUN blocks in-kernel until SIPI arrives,
    // at which point KVM sets CS:IP to the SIPI vector itself.  Userspace
    // SIPI callback never fires, so the generic wait would deadlock.
    bool NeedsStartupWait() const override { return false; }

private:
    KvmVCpu() = default;

    bool SetupCpuid();

    uint32_t index_ = 0;
    int vcpu_fd_ = -1;
    struct kvm_run* run_ = nullptr;
    size_t run_size_ = 0;

    AddressSpace* addr_space_ = nullptr;

    // CancelRun writes immediate_exit = 1 and raises SIGUSR1 on the vCPU
    // thread. OnThreadInit stashes the pthread id so that CancelRun, which
    // can be invoked from any thread, can deliver the signal to the right one.
    std::atomic<unsigned long> thread_id_{0};
};

} // namespace kvm
