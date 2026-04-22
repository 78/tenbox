#pragma once

#include "platform/windows/hypervisor/whvp_platform.h"
#include "core/vmm/types.h"
#include "core/vmm/hypervisor_vm.h"
#include <memory>
#include <mutex>
#include <vector>

class AddressSpace;

namespace whvp {

class WhvpDoorbellRegistrar;
class WhvpVCpu;

class WhvpVm : public HypervisorVm {
public:
    ~WhvpVm() override;

    static std::unique_ptr<WhvpVm> Create(uint32_t cpu_count);

    WHV_PARTITION_HANDLE Handle() const { return partition_; }

    // True when the hypervisor's in-partition APIC emulation is unavailable
    // (e.g. Windows 10 1803: LocalApicEmulationMode set fails and/or
    // WHvRequestInterrupt is not exported). In that mode interrupts are
    // injected via a software pending queue + WHvRegisterPendingInterruption
    // on each vCPU.
    bool SoftApic() const { return soft_apic_; }

    bool MapMemory(GPA gpa, void* hva, uint64_t size, bool writable) override;
    bool UnmapMemory(GPA gpa, uint64_t size) override;
    std::unique_ptr<HypervisorVCpu> CreateVCpu(
        uint32_t index, AddressSpace* addr_space) override;
    void RequestInterrupt(const InterruptRequest& req) override;
    void QueueInterrupt(uint32_t vector, uint32_t dest_vcpu) override;

    bool RegisterQueueDoorbell(uint64_t mmio_addr, uint32_t len, uint32_t datamatch,
                               std::function<void()> cb) override;
    void UnregisterAllQueueDoorbells() override;

    WhvpVm(const WhvpVm&) = delete;
    WhvpVm& operator=(const WhvpVm&) = delete;

private:
    WhvpVm() = default;
    WHV_PARTITION_HANDLE partition_ = nullptr;
    std::unique_ptr<WhvpDoorbellRegistrar> doorbell_;

    bool soft_apic_ = false;
    uint32_t cpu_count_ = 0;
    // Registry of created vCPUs, used by the soft-APIC path to fan out
    // InterruptRequest -> per-vCPU QueueInterrupt + CancelRun. Entries are
    // raw pointers owned by whoever called CreateVCpu; cleared when the
    // vCPU is destroyed (unique_ptr<HypervisorVCpu> returned to the caller).
    std::mutex vcpu_mutex_;
    std::vector<WhvpVCpu*> vcpus_;

    friend class WhvpVCpu;
    void OnVCpuCreated(uint32_t index, WhvpVCpu* vcpu);
    void OnVCpuDestroyed(uint32_t index);
};

} // namespace whvp
