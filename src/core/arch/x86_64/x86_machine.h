#pragma once

#include "core/vmm/machine_model.h"
#include "core/device/serial/uart_16550.h"
#include "core/device/timer/i8254_pit.h"
#include "core/device/rtc/cmos_rtc.h"
#include "core/device/irq/ioapic.h"
#include "core/device/irq/i8259_pic.h"
#include "core/device/irq/local_apic.h"
#include "core/device/pci/pci_host.h"
#include "core/device/acpi/acpi_pm.h"
#include "core/arch/x86_64/acpi.h"

// x86 machine model: direct Linux boot protocol with minimal legacy devices.
// Uses ACPI DSDT for VirtIO MMIO device discovery.
class X86Machine final : public MachineModel {
public:
    X86Machine() = default;

    bool SetupPlatformDevices(
        AddressSpace& addr_space,
        GuestMemMap& mem,
        HypervisorVm* hv_vm,
        std::shared_ptr<ConsolePort> console_port,
        std::function<void()> shutdown_cb,
        std::function<void()> reboot_cb) override;

    bool LoadKernel(
        const VmConfig& config,
        GuestMemMap& mem,
        const std::vector<VirtioDeviceSlot>& virtio_slots) override;

    bool SetupBootVCpu(HypervisorVCpu* vcpu, uint8_t* ram) override;

    void InjectIrq(HypervisorVm* hv_vm, uint8_t irq) override;
    void SetIrqLevel(HypervisorVm* hv_vm, uint8_t irq, bool asserted) override;

    void InjectConsoleInput(const uint8_t* data, size_t size) override;

    void TriggerPowerButton() override;

    std::vector<VirtioDeviceSlot> GetVirtioSlots() const override;

    GPA RamBase() const override { return 0; }
    GPA MmioGapStart() const override { return kMmioGapStart; }
    GPA MmioGapEnd() const override { return kMmioGapEnd; }

    void SetSipiCallback(LocalApic::SipiFunc cb) { lapic_.SetSipiCallback(std::move(cb)); }
    void SetInitCallback(LocalApic::InitFunc cb) { lapic_.SetInitCallback(std::move(cb)); }
    void SetIpiCallback(LocalApic::IpiFunc cb) { lapic_.SetIpiCallback(std::move(cb)); }
    void InitLapic(uint32_t cpu_count) { lapic_.Init(cpu_count); }

    void SetApicIds(std::vector<uint32_t> ids) {
        apic_ids_ = ids;
        lapic_.SetApicIds(ids);
    }

private:
    Uart16550 uart_;
    I8254Pit pit_;
    SystemControlB sys_ctrl_b_;
    CmosRtc rtc_;
    IoApic ioapic_;
    LocalApic lapic_;
    I8259Pic pic_master_;
    I8259Pic pic_slave_;
    PciHostBridge pci_host_;
    AcpiPm acpi_pm_;
    Device port_sink_;

    std::function<void(uint8_t)> irq_injector_;
    std::vector<uint32_t> apic_ids_;
};
