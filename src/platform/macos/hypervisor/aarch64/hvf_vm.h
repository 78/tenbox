#pragma once

#include "core/vmm/hypervisor_vm.h"
#include <cstdint>
#include <memory>

namespace hvf {

class HvfVm final : public HypervisorVm {
public:
    ~HvfVm() override;

    static std::unique_ptr<HvfVm> Create(uint32_t cpu_count);

    bool MapMemory(GPA gpa, void* hva, uint64_t size, bool writable) override;
    bool UnmapMemory(GPA gpa, uint64_t size) override;

    std::unique_ptr<HypervisorVCpu> CreateVCpu(
        uint32_t index, AddressSpace* addr_space) override;

    void RequestInterrupt(const InterruptRequest& req) override;

    uint64_t GetRedistBase() const { return redist_base_; }
    size_t GetRedistSizePerCpu() const { return redist_size_per_cpu_; }

private:
    HvfVm() = default;
    uint32_t cpu_count_ = 0;
    bool vm_created_ = false;
    bool gic_created_ = false;
    uint64_t redist_base_ = 0;
    size_t redist_size_per_cpu_ = 0;
};

} // namespace hvf
