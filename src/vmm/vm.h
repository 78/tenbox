#pragma once

#include "vmm/types.h"
#include "vmm/address_space.h"
#include "hypervisor/whvp_vm.h"
#include "hypervisor/whvp_vcpu.h"
#include "device/serial/uart_16550.h"
#include "device/timer/i8254_pit.h"
#include "device/rtc/cmos_rtc.h"
#include "device/irq/ioapic.h"
#include "device/irq/i8259_pic.h"
#include "device/pci/pci_host.h"
#include "device/acpi/acpi_pm.h"
#include "device/virtio/virtio_mmio.h"
#include "device/virtio/virtio_blk.h"
#include "device/virtio/virtio_net.h"
#include "net/net_backend.h"
#include "arch/x86_64/acpi.h"
#include <memory>
#include <string>
#include <atomic>
#include <thread>
#include <vector>

struct VmConfig {
    std::string kernel_path;
    std::string initrd_path;
    std::string disk_path;
    std::string cmdline = "console=ttyS0 earlyprintk=serial lapic no_timer_check tsc=reliable";
    uint64_t memory_mb = 256;
    uint32_t cpu_count = 1;
    bool net_enabled = false;
    std::vector<PortForward> port_forwards;
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
    bool SetupVirtioNet(const std::vector<PortForward>& forwards);
    bool LoadKernel(const VmConfig& config);

    void InputThreadFunc();
    void VCpuThreadFunc(uint32_t vcpu_index);
    void InjectIrq(uint8_t irq);

    uint32_t cpu_count_ = 1;
    std::unique_ptr<whvp::WhvpVm> whvp_vm_;
    std::vector<std::unique_ptr<whvp::WhvpVCpu>> vcpus_;
    std::vector<std::thread> vcpu_threads_;
    std::atomic<int> exit_code_{0};

    uint8_t* ram_ = nullptr;
    uint64_t ram_size_ = 0;

    AddressSpace addr_space_;
    Uart16550 uart_;
    I8254Pit pit_;
    SystemControlB sys_ctrl_b_;
    CmosRtc rtc_;
    IoApic ioapic_;
    I8259Pic pic_master_;
    I8259Pic pic_slave_;
    PciHostBridge pci_host_;
    AcpiPm acpi_pm_;
    Device port_sink_;  // absorbs POST (0x80), DMA page (0x87), etc.

    // VirtIO block device (optional)
    std::unique_ptr<VirtioBlkDevice> virtio_blk_;
    std::unique_ptr<VirtioMmioDevice> virtio_mmio_;

    // VirtIO net device (optional)
    std::unique_ptr<VirtioNetDevice> virtio_net_;
    std::unique_ptr<VirtioMmioDevice> virtio_mmio_net_;
    std::unique_ptr<NetBackend> net_backend_;

    std::vector<x86::VirtioMmioAcpiInfo> virtio_acpi_devs_;

    std::atomic<bool> running_{false};
    std::thread input_thread_;
};
