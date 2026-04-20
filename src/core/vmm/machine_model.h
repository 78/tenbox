#pragma once

#include "core/vmm/types.h"
#include "core/vmm/address_space.h"
#include "core/vmm/hypervisor_vm.h"
#include "core/vmm/hypervisor_vcpu.h"
#include "common/ports.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct VmSharedFolder;
struct VmConfig;
class VmIoLoop;

// Describes a VirtIO MMIO device slot with its base address and IRQ number.
struct VirtioDeviceSlot {
    uint64_t mmio_base;
    uint8_t  irq;
};

// Abstract machine model that encapsulates architecture-specific device setup,
// boot protocol, and interrupt routing.  Each concrete model (x86 direct boot,
// ARM64 virt, future i440fx/Q35, ...) implements this interface.
class MachineModel {
public:
    using IrqInjector = std::function<void(uint8_t irq)>;

    virtual ~MachineModel() = default;

    // Register platform devices (UART, interrupt controller, timers, etc.)
    // into the address space and wire up callbacks.
    virtual bool SetupPlatformDevices(
        AddressSpace& addr_space,
        GuestMemMap& mem,
        HypervisorVm* hv_vm,
        std::shared_ptr<ConsolePort> console_port,
        VmIoLoop* io_loop,
        std::function<void()> shutdown_cb,
        std::function<void()> reboot_cb) = 0;

    // Load kernel (and optionally initrd / firmware) into guest RAM.
    // May also set up boot-time structures (ACPI tables, device tree, etc.)
    // and configure initial vCPU registers on the BSP.
    virtual bool LoadKernel(
        const VmConfig& config,
        GuestMemMap& mem,
        const std::vector<VirtioDeviceSlot>& virtio_slots) = 0;

    // Set initial registers on the bootstrap vCPU after LoadKernel.
    virtual bool SetupBootVCpu(HypervisorVCpu* vcpu, uint8_t* ram) = 0;

    // Inject a device IRQ through the machine's interrupt controller.
    virtual void InjectIrq(HypervisorVm* hv_vm, uint8_t irq) = 0;

    // Set the level of a device IRQ line (for level-triggered SPI on ARM64/GICv3).
    // Default is no-op — only needed on platforms with level-triggered GIC SPIs.
    virtual void SetIrqLevel(HypervisorVm* hv_vm, uint8_t irq, bool asserted) {
        (void)hv_vm; (void)irq; (void)asserted;
    }

    // Push console input bytes to the machine's serial device.
    virtual void InjectConsoleInput(const uint8_t* data, size_t size) = 0;

    // Handle ACPI power button (if supported).
    virtual void TriggerPowerButton() = 0;

    // Return VirtIO device slots with per-architecture base addresses & IRQs.
    virtual std::vector<VirtioDeviceSlot> GetVirtioSlots() const = 0;

    // Memory layout: where guest RAM starts in GPA space.
    virtual GPA RamBase() const = 0;

    // Memory layout: MMIO gap boundaries (for splitting RAM around MMIO).
    virtual GPA MmioGapStart() const = 0;
    virtual GPA MmioGapEnd() const = 0;
};
