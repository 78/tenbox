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

    // Register an eventfd (or platform equivalent) as an IRQFD for a
    // level-triggered GSI. When trigger_fd is signalled, the hypervisor
    // asserts the line directly in kernel space, bypassing the userspace
    // RequestInterrupt / AssertIrq ioctl path.
    //
    // gsi is the hypervisor-absolute interrupt number:
    //   - arm64 KVM: SPI absolute INTID (>= 32)
    //   - x86_64 KVM: IOAPIC pin (0..23 with the default routing)
    // The caller is responsible for computing the arch-specific offset.
    //
    // resample_fd (may be -1) is signalled by the hypervisor after the
    // guest EOIs the interrupt so the caller can re-assert if the device
    // still has a pending condition. Required for level-triggered lines.
    //
    // Default returns false; macOS HVF and any backend without irqfd
    // support falls back to the RequestInterrupt / SetIrqLevelCallback
    // path automatically.
    virtual bool RegisterLevelIrqFd(uint32_t /*gsi*/, int /*trigger_fd*/,
                                    int /*resample_fd*/) { return false; }
    virtual bool UnregisterIrqFd(uint32_t /*gsi*/, int /*trigger_fd*/) { return false; }

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
