#pragma once

#include "core/vmm/hypervisor_vm.h"
#include <cstdint>
#include <memory>
#include <vector>
#include <mutex>

namespace hvf {

class HvfVCpu;

class HvfVm final : public HypervisorVm {
public:
    ~HvfVm() override;

    static std::unique_ptr<HvfVm> Create(uint32_t cpu_count);

    bool MapMemory(GPA gpa, void* hva, uint64_t size, bool writable) override;
    bool UnmapMemory(GPA gpa, uint64_t size) override;

    std::unique_ptr<HypervisorVCpu> CreateVCpu(
        uint32_t index, AddressSpace* addr_space) override;

    void RequestInterrupt(const InterruptRequest& req) override;

    void QueueInterrupt(uint32_t vector, uint32_t dest_vcpu) override;

    void SetGuestMemMap(const GuestMemMap* mem) override { guest_mem_ = mem; }

private:
    HvfVm() = default;
    uint32_t cpu_count_ = 0;
    bool vm_created_ = false;

    uint8_t* ram_hva_ = nullptr;
    uint64_t ram_size_ = 0;
    const GuestMemMap* guest_mem_ = nullptr;

    std::mutex vcpu_mutex_;
    std::vector<HvfVCpu*> vcpus_;
};

} // namespace hvf
