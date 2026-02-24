#pragma once

#include "vmm/types.h"
#include "vmm/address_space.h"
#include "hypervisor/whvp_vm.h"
#include "hypervisor/whvp_vcpu.h"
#include "device/serial/uart_16550.h"
#include "device/timer/i8254_pit.h"
#include "device/rtc/cmos_rtc.h"
#include "device/irq/ioapic.h"
#include "device/acpi/acpi_pm.h"
#include "device/virtio/virtio_mmio.h"
#include "device/virtio/virtio_blk.h"
#include "arch/x86_64/acpi.h"
#include <memory>
#include <string>
#include <atomic>
#include <thread>

struct VmConfig {
    std::string kernel_path;
    std::string initrd_path;
    std::string disk_path;
    std::string cmdline = "console=ttyS0 earlyprintk=serial lapic no_timer_check tsc=reliable";
    uint64_t memory_mb = 256;
    uint32_t cpu_count = 1;
};

class Vm {
public:
    ~Vm();

    static std::unique_ptr<Vm> Create(const VmConfig& config);

    int Run();

    void RequestStop();

private:
    Vm() = default;

    bool AllocateMemory(uint64_t size);
    bool SetupDevices();
    bool SetupVirtioBlk(const std::string& disk_path);
    bool LoadKernel(const VmConfig& config);

    void InputThreadFunc();
    void InjectIrq(uint8_t irq);

    std::unique_ptr<whvp::WhvpVm> whvp_vm_;
    std::unique_ptr<whvp::WhvpVCpu> vcpu_;

    uint8_t* ram_ = nullptr;
    uint64_t ram_size_ = 0;

    AddressSpace addr_space_;
    Uart16550 uart_;
    I8254Pit pit_;
    SystemControlB sys_ctrl_b_;
    CmosRtc rtc_;
    IoApic ioapic_;
    AcpiPm acpi_pm_;

    // VirtIO block device (optional)
    std::unique_ptr<VirtioBlkDevice> virtio_blk_;
    std::unique_ptr<VirtioMmioDevice> virtio_mmio_;
    std::vector<x86::VirtioMmioAcpiInfo> virtio_acpi_devs_;

    std::atomic<bool> running_{false};
    std::thread input_thread_;
};
