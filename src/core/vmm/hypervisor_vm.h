#pragma once

#include "core/vmm/types.h"
#include "core/vmm/hypervisor_vcpu.h"
#include <cstdint>
#include <memory>

class AddressSpace;

struct InterruptRequest {
    uint32_t vector;
    uint32_t destination;
    bool logical_destination;
    bool level_triggered;
};

class HypervisorVm {
public:
    virtual ~HypervisorVm() = default;

    virtual bool MapMemory(GPA gpa, void* hva, uint64_t size, bool writable) = 0;
    virtual bool UnmapMemory(GPA gpa, uint64_t size) = 0;

    virtual std::unique_ptr<HypervisorVCpu> CreateVCpu(
        uint32_t index, AddressSpace* addr_space) = 0;

    virtual void RequestInterrupt(const InterruptRequest& req) = 0;

    // Optional hook for hypervisors with an in-kernel irqchip (e.g. KVM).
    // Returns true if the IRQ was injected through the hypervisor's own
    // interrupt controller and the generic userspace IOAPIC path must be
    // skipped. Default returns false so HVF / WHVP keep their current path.
    virtual bool AssertIrq(uint32_t /*gsi*/, bool /*level*/) { return false; }

    virtual void SetGuestMemMap(const GuestMemMap*) {}

    virtual void QueueInterrupt(uint32_t vector, uint32_t dest_vcpu) {
        InterruptRequest req{};
        req.vector = vector;
        req.destination = dest_vcpu;
        req.logical_destination = false;
        req.level_triggered = false;
        RequestInterrupt(req);
    }
};
