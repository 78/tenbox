#pragma once

#include "core/vmm/types.h"
#include "core/vmm/address_space.h"
#include "hypervisor/whvp_vm.h"
#include "hypervisor/whvp_vcpu.h"
#include "core/device/serial/uart_16550.h"
#include "core/device/timer/i8254_pit.h"
#include "core/device/rtc/cmos_rtc.h"
#include "core/device/irq/ioapic.h"
#include "core/device/irq/i8259_pic.h"
#include "core/device/pci/pci_host.h"
#include "core/device/acpi/acpi_pm.h"
#include "core/device/virtio/virtio_mmio.h"
#include "core/device/virtio/virtio_blk.h"
#include "core/device/virtio/virtio_net.h"
#include "core/device/virtio/virtio_input.h"
#include "core/device/virtio/virtio_gpu.h"
#include "core/net/net_backend.h"
#include "core/arch/x86_64/acpi.h"
#include "common/ports.h"
#include <memory>
#include <string>
#include <atomic>
#include <thread>
#include <vector>

struct VmConfig {
    std::string kernel_path;
    std::string initrd_path;
    std::string disk_path;
    std::string cmdline = "console=ttyS0 earlyprintk=serial lapic no_timer_check tsc=reliable i8042.noprobe";
    uint64_t memory_mb = 256;
    uint32_t cpu_count = 1;
    bool net_link_up = false;
    std::vector<PortForward> port_forwards;
    bool interactive = true;
    std::shared_ptr<ConsolePort> console_port;
    std::shared_ptr<InputPort> input_port;
    std::shared_ptr<DisplayPort> display_port;
    uint32_t display_width = 1024;
    uint32_t display_height = 768;
};

class Vm {
public:
    ~Vm();

    static std::unique_ptr<Vm> Create(const VmConfig& config);

    int Run();

    void RequestStop();
    void RequestReboot();
    bool RebootRequested() const { return reboot_requested_.load(); }
    void TriggerPowerButton();
    void InjectConsoleBytes(const uint8_t* data, size_t size);
    void SetNetLinkUp(bool up);
    void UpdatePortForwards(const std::vector<PortForward>& forwards);
    void InjectKeyEvent(uint32_t evdev_code, bool pressed);
    void InjectPointerEvent(int32_t x, int32_t y, uint32_t buttons);

private:
    Vm() = default;

    bool AllocateMemory(uint64_t size);
    bool SetupDevices();
    bool SetupVirtioBlk(const std::string& disk_path);
    bool SetupVirtioNet(bool link_up, const std::vector<PortForward>& forwards);
    bool SetupVirtioInput();
    bool SetupVirtioGpu(uint32_t width, uint32_t height);
    bool LoadKernel(const VmConfig& config);

    void InputThreadFunc();
    void HidInputThreadFunc();
    void VCpuThreadFunc(uint32_t vcpu_index);
    void InjectIrq(uint8_t irq);

    uint32_t cpu_count_ = 1;
    std::unique_ptr<whvp::WhvpVm> whvp_vm_;
    std::vector<std::unique_ptr<whvp::WhvpVCpu>> vcpus_;
    std::vector<std::thread> vcpu_threads_;
    std::atomic<int> exit_code_{0};

    GuestMemMap mem_;

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
    Device port_sink_;

    // VirtIO block device (optional)
    std::unique_ptr<VirtioBlkDevice> virtio_blk_;
    std::unique_ptr<VirtioMmioDevice> virtio_mmio_;

    // VirtIO net device (optional)
    std::unique_ptr<VirtioNetDevice> virtio_net_;
    std::unique_ptr<VirtioMmioDevice> virtio_mmio_net_;
    std::unique_ptr<NetBackend> net_backend_;

    // VirtIO input devices (keyboard + tablet)
    std::unique_ptr<VirtioInputDevice> virtio_kbd_;
    std::unique_ptr<VirtioMmioDevice> virtio_mmio_kbd_;
    std::unique_ptr<VirtioInputDevice> virtio_tablet_;
    std::unique_ptr<VirtioMmioDevice> virtio_mmio_tablet_;

    // VirtIO GPU (2D)
    std::unique_ptr<VirtioGpuDevice> virtio_gpu_;
    std::unique_ptr<VirtioMmioDevice> virtio_mmio_gpu_;

    std::vector<x86::VirtioMmioAcpiInfo> virtio_acpi_devs_;

    std::atomic<bool> running_{false};
    std::atomic<bool> reboot_requested_{false};
    std::thread input_thread_;
    std::thread hid_input_thread_;
    std::shared_ptr<ConsolePort> console_port_;
    std::shared_ptr<InputPort> input_port_;
    std::shared_ptr<DisplayPort> display_port_;
    uint32_t inject_prev_buttons_ = 0;
};
