#include "core/arch/x86_64/x86_machine.h"
#include "core/arch/x86_64/boot.h"
#include "core/vmm/vm.h"

static constexpr uint64_t kVirtioMmioBase       = 0xd0000000;
static constexpr uint64_t kVirtioMmioStride     = 0x200;
static constexpr uint8_t  kVirtioBaseIrq         = 5;

// Fixed IRQ assignments matching ACPI DSDT generation order.
static constexpr uint8_t kIrqBlk    = 5;
static constexpr uint8_t kIrqNet    = 6;
static constexpr uint8_t kIrqKbd    = 11;
static constexpr uint8_t kIrqTablet = 14;
static constexpr uint8_t kIrqGpu    = 15;
static constexpr uint8_t kIrqSerial = 7;
static constexpr uint8_t kIrqFs     = 16;
static constexpr uint8_t kIrqSnd    = 17;

bool X86Machine::SetupPlatformDevices(
    AddressSpace& addr_space,
    GuestMemMap& /*mem*/,
    HypervisorVm* hv_vm,
    std::shared_ptr<ConsolePort> console_port,
    std::function<void()> shutdown_cb,
    std::function<void()> reboot_cb) {

    irq_injector_ = [this, hv_vm](uint8_t irq) {
        InjectIrq(hv_vm, irq);
    };

    uart_.SetIrqCallback([this]() { irq_injector_(4); });
    uart_.SetTxCallback([console_port](uint8_t byte) {
        if (!console_port) return;
        console_port->Write(&byte, 1);
    });
    addr_space.AddPioDevice(
        Uart16550::kCom1Base, Uart16550::kRegCount, &uart_);

    pit_.SetIrqCallback([this]() { irq_injector_(0); });
    addr_space.AddPioDevice(
        I8254Pit::kBasePort, I8254Pit::kRegCount, &pit_);
    sys_ctrl_b_.SetPit(&pit_);
    addr_space.AddPioDevice(
        SystemControlB::kPort, SystemControlB::kRegCount, &sys_ctrl_b_);

    addr_space.AddPioDevice(
        CmosRtc::kBasePort, CmosRtc::kRegCount, &rtc_);

    addr_space.AddMmioDevice(
        IoApic::kBaseAddress, IoApic::kSize, &ioapic_);

    lapic_.SetIrqInjectCallback([hv_vm](uint32_t vector, uint32_t cpu) {
        hv_vm->QueueInterrupt(vector, cpu);
    });
    addr_space.AddMmioDevice(
        LocalApic::kBaseAddress, LocalApic::kSize, &lapic_);
    lapic_.Start();

    acpi_pm_.SetShutdownCallback(std::move(shutdown_cb));
    acpi_pm_.SetResetCallback(std::move(reboot_cb));
    acpi_pm_.SetSciCallback([this]() { irq_injector_(9); });
    addr_space.AddPioDevice(
        AcpiPm::kBasePort, AcpiPm::kRegCount, &acpi_pm_);

    addr_space.AddPioDevice(
        I8259Pic::kMasterBase, I8259Pic::kRegCount, &pic_master_);
    addr_space.AddPioDevice(
        I8259Pic::kSlaveBase, I8259Pic::kRegCount, &pic_slave_);
    addr_space.AddPioDevice(
        PciHostBridge::kBasePort, PciHostBridge::kRegCount, &pci_host_);

    addr_space.AddPioDevice(0x80,  1, &port_sink_);
    addr_space.AddPioDevice(0x87,  1, &port_sink_);
    addr_space.AddPioDevice(0x2E8, 8, &port_sink_);
    addr_space.AddPioDevice(0x2F8, 8, &port_sink_);
    addr_space.AddPioDevice(0x3E8, 8, &port_sink_);
    addr_space.AddPioDevice(0xC000, 0x1000, &port_sink_);

    return true;
}

bool X86Machine::LoadKernel(
    const VmConfig& config,
    GuestMemMap& mem,
    const std::vector<VirtioDeviceSlot>& virtio_slots) {

    std::vector<x86::VirtioMmioAcpiInfo> acpi_devs;
    for (auto& slot : virtio_slots) {
        acpi_devs.push_back({
            slot.mmio_base,
            static_cast<uint32_t>(0x200),
            static_cast<uint32_t>(slot.irq)});
    }

    x86::BootConfig boot_cfg;
    boot_cfg.kernel_path = config.kernel_path;
    boot_cfg.initrd_path = config.initrd_path;
    boot_cfg.cmdline = config.cmdline;
    boot_cfg.mem = mem;
    boot_cfg.cpu_count = config.cpu_count;
    boot_cfg.virtio_devs = acpi_devs;
    boot_cfg.apic_ids = apic_ids_;

    uint64_t kernel_size = x86::LoadLinuxKernel(boot_cfg);
    return kernel_size != 0;
}

bool X86Machine::SetupBootVCpu(HypervisorVCpu* vcpu, uint8_t* ram) {
    return vcpu->SetupBootRegisters(ram);
}

void X86Machine::InjectIrq(HypervisorVm* hv_vm, uint8_t irq) {
    // Hypervisors with an in-kernel irqchip (KVM) handle the IOAPIC/PIC/LAPIC
    // in the kernel.  InjectIrq() has pulse semantics (one-shot edge IRQ), so
    // pulse the GSI line: assert then de-assert.  Without the de-assert, an
    // edge-triggered line stays latched high and no further IRQs re-fire on
    // the same GSI (breaks UART THRE, PIT tick, etc.).
    if (hv_vm->AssertIrq(irq, true)) {
        hv_vm->AssertIrq(irq, false);
        return;
    }

    uint64_t rte = 0;
    if (!ioapic_.GetRedirEntry(irq, &rte)) return;

    bool masked = (rte >> 16) & 1;
    if (masked) return;

    uint32_t vector = rte & 0xFF;
    if (vector == 0) return;

    InterruptRequest req{};
    req.vector = vector;
    req.destination = static_cast<uint32_t>(rte >> 56);
    req.logical_destination = ((rte >> 11) & 1) != 0;
    req.level_triggered = ((rte >> 15) & 1) != 0;

    hv_vm->RequestInterrupt(req);
}

void X86Machine::SetIrqLevel(HypervisorVm* hv_vm, uint8_t irq, bool asserted) {
    if (hv_vm->AssertIrq(irq, asserted)) return;
    if (asserted) {
        InjectIrq(hv_vm, irq);
    }
}

void X86Machine::InjectConsoleInput(const uint8_t* data, size_t size) {
    if (!data || size == 0) return;
    for (size_t i = 0; i < size; ++i) {
        uart_.PushInput(data[i]);
    }
    uart_.CheckAndRaiseIrq();
}

void X86Machine::TriggerPowerButton() {
    acpi_pm_.TriggerPowerButton();
}

std::vector<VirtioDeviceSlot> X86Machine::GetVirtioSlots() const {
    return {
        {0xd0000000, kIrqBlk},
        {0xd0000200, kIrqNet},
        {0xd0000400, kIrqKbd},
        {0xd0000600, kIrqTablet},
        {0xd0000800, kIrqGpu},
        {0xd0000a00, kIrqSerial},
        {0xd0000c00, kIrqFs},
        {0xd0000e00, kIrqSnd},
    };
}
