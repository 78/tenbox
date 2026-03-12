#pragma once

#include "core/vmm/vcpu_startup_state.h"
#include <cstdint>

enum class VCpuExitAction {
    kContinue,
    kHalt,
    kShutdown,
    kError,
};

class HypervisorVCpu {
public:
    virtual ~HypervisorVCpu() = default;
    virtual VCpuExitAction RunOnce() = 0;
    virtual void CancelRun() = 0;
    virtual uint32_t Index() const = 0;

    // Set up x86 32-bit protected mode boot registers in guest RAM.
    // Called once on vCPU 0 after the kernel is loaded.
    virtual bool SetupBootRegisters(uint8_t* ram) = 0;

    // Block until an interrupt is queued or timeout_ms elapses.
    // Returns true if woken by an interrupt, false on timeout.
    virtual bool WaitForInterrupt(uint32_t timeout_ms) { (void)timeout_ms; return false; }

    // Hardware APIC ID as assigned by the hypervisor. Defaults to Index().
    virtual uint32_t ApicId() const { return Index(); }

    // Called on the vCPU worker thread immediately after the vCPU is created.
    // Use for thread-local state (e.g. setting the current CPU index).
    virtual void OnThreadInit() {}

    // Called on AP threads after the boot barrier is released and the AP's
    // startup state has been signalled. Used to apply SIPI/PSCI registers.
    virtual void OnStartup(const VCpuStartupState& /*state*/) {}

    // Returns false if this hypervisor handles AP startup internally (e.g.
    // WHVP xAPIC emulation) so the generic AP wait should be skipped.
    virtual bool NeedsStartupWait() const { return true; }
};
