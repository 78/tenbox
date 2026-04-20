#pragma once

#include "core/vmm/hypervisor_vm.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace kvm {

class KvmVCpu;

class KvmVm final : public HypervisorVm {
public:
    ~KvmVm() override;

    static std::unique_ptr<KvmVm> Create(uint32_t cpu_count);

    bool MapMemory(GPA gpa, void* hva, uint64_t size, bool writable) override;
    bool UnmapMemory(GPA gpa, uint64_t size) override;

    std::unique_ptr<HypervisorVCpu> CreateVCpu(
        uint32_t index, AddressSpace* addr_space) override;

    void RequestInterrupt(const InterruptRequest& req) override;

    // KVM has an in-kernel irqchip: IRQ lines go through KVM_IRQ_LINE.
    bool AssertIrq(uint32_t gsi, bool level) override;

    // Register / unregister a KVM_IRQFD for a level-triggered GSI (IOAPIC pin).
    bool RegisterLevelIrqFd(uint32_t gsi, int trigger_fd, int resample_fd) override;
    bool UnregisterIrqFd(uint32_t gsi, int trigger_fd) override;

    void SetGuestMemMap(const GuestMemMap* mem) override { guest_mem_ = mem; }

    int VmFd() const { return vm_fd_; }
    int KvmFd() const { return kvm_fd_; }
    size_t VcpuMmapSize() const { return vcpu_mmap_size_; }

private:
    KvmVm() = default;

    int kvm_fd_ = -1;
    int vm_fd_ = -1;
    uint32_t cpu_count_ = 0;
    size_t vcpu_mmap_size_ = 0;

    const GuestMemMap* guest_mem_ = nullptr;

    std::mutex slot_mutex_;
    uint32_t next_slot_ = 0;
};

} // namespace kvm
