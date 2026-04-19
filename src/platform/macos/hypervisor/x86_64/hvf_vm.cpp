#include "platform/macos/hypervisor/x86_64/hvf_vm.h"
#include "platform/macos/hypervisor/x86_64/hvf_vcpu.h"
#include "core/vmm/types.h"
#include <cinttypes>

#include <Hypervisor/hv.h>

namespace hvf {

HvfVm::~HvfVm() {
    if (vm_created_) {
        hv_vm_destroy();
    }
}

std::unique_ptr<HvfVm> HvfVm::Create(uint32_t cpu_count) {
    auto vm = std::unique_ptr<HvfVm>(new HvfVm());
    vm->cpu_count_ = cpu_count;

    hv_return_t ret = hv_vm_create(HV_VM_DEFAULT);
    if (ret != HV_SUCCESS) {
        LOG_ERROR("hvf: hv_vm_create failed: %d", (int)ret);
        return nullptr;
    }
    vm->vm_created_ = true;
    vm->vcpus_.resize(cpu_count, nullptr);

    LOG_INFO("hvf: x86_64 VM created (%u vCPUs)", cpu_count);
    return vm;
}

bool HvfVm::MapMemory(GPA gpa, void* hva, uint64_t size, bool writable) {
    if (gpa == 0 && ram_hva_ == nullptr) {
        ram_hva_ = static_cast<uint8_t*>(hva);
        ram_size_ = size;
    }
    hv_memory_flags_t flags = HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC;

    hv_return_t ret = hv_vm_map(hva, gpa, size, flags);
    if (ret != HV_SUCCESS) {
        LOG_ERROR("hvf: hv_vm_map(GPA=0x%" PRIx64 ", size=0x%" PRIx64 ", flags=0x%" PRIx64 ") failed: %d",
                  gpa, size, (uint64_t)flags, (int)ret);
        return false;
    }
    LOG_INFO("hvf: mapped GPA=0x%" PRIx64 " size=0x%" PRIx64 " HVA=%p flags=0x%" PRIx64,
             gpa, size, hva, (uint64_t)flags);

    // Verify the mapping by re-protecting with full RWX
    ret = hv_vm_protect(gpa, size, flags);
    if (ret != HV_SUCCESS) {
        LOG_WARN("hvf: hv_vm_protect(GPA=0x%" PRIx64 ") failed: %d",
                 gpa, (int)ret);
    }

    return true;
}

bool HvfVm::UnmapMemory(GPA gpa, uint64_t size) {
    hv_return_t ret = hv_vm_unmap(gpa, size);
    if (ret != HV_SUCCESS) {
        LOG_ERROR("hvf: hv_vm_unmap(GPA=0x%" PRIx64 ") failed: %d",
                  gpa, (int)ret);
        return false;
    }
    return true;
}

std::unique_ptr<HypervisorVCpu> HvfVm::CreateVCpu(
    uint32_t index, AddressSpace* addr_space) {
    auto vcpu = HvfVCpu::Create(index, addr_space, ram_hva_, ram_size_, guest_mem_);
    if (vcpu) {
        std::lock_guard<std::mutex> lock(vcpu_mutex_);
        if (index < vcpus_.size()) {
            vcpus_[index] = vcpu.get();
        }
    }
    return vcpu;
}

void HvfVm::RequestInterrupt(const InterruptRequest& req) {
    std::lock_guard<std::mutex> lock(vcpu_mutex_);
    if (req.logical_destination) {
        // Flat-model logical destination: bitmask, bit N = APIC ID N
        uint32_t mask = req.destination;
        for (uint32_t i = 0; i < cpu_count_; i++) {
            if ((mask & (1u << i)) && i < vcpus_.size() && vcpus_[i]) {
                vcpus_[i]->QueueInterrupt(req.vector);
                vcpus_[i]->CancelRun();
            }
        }
    } else {
        uint32_t dest = req.destination;
        if (dest >= cpu_count_) dest = 0;
        if (dest < vcpus_.size() && vcpus_[dest]) {
            vcpus_[dest]->QueueInterrupt(req.vector);
            vcpus_[dest]->CancelRun();
        }
    }
}

void HvfVm::QueueInterrupt(uint32_t vector, uint32_t dest_vcpu) {
    std::lock_guard<std::mutex> lock(vcpu_mutex_);
    if (dest_vcpu < vcpus_.size() && vcpus_[dest_vcpu]) {
        vcpus_[dest_vcpu]->QueueInterrupt(vector);
        vcpus_[dest_vcpu]->CancelRun();
    }
}

} // namespace hvf
