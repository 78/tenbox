#include "core/vmm/vm.h"
#include "core/arch/x86_64/boot.h"
#include "platform/windows/console/std_console_port.h"
#include <algorithm>

static constexpr uint64_t kVirtioMmioBase       = 0xd0000000;
static constexpr uint8_t  kVirtioBlkIrq         = 5;
static constexpr uint64_t kVirtioNetMmioBase    = 0xd0000200;
static constexpr uint8_t  kVirtioNetIrq         = 6;
static constexpr uint64_t kVirtioKbdMmioBase    = 0xd0000400;
static constexpr uint8_t  kVirtioKbdIrq         = 11;
static constexpr uint64_t kVirtioTabletMmioBase = 0xd0000600;
static constexpr uint8_t  kVirtioTabletIrq      = 12;
static constexpr uint64_t kVirtioGpuMmioBase    = 0xd0000800;
static constexpr uint8_t  kVirtioGpuIrq         = 13;

Vm::~Vm() {
    running_ = false;
    if (input_thread_.joinable())
        input_thread_.join();
    if (hid_input_thread_.joinable())
        hid_input_thread_.join();
    for (auto& t : vcpu_threads_) {
        if (t.joinable()) t.join();
    }
    vcpus_.clear();
    whvp_vm_.reset();
    if (mem_.base) {
        VirtualFree(mem_.base, 0, MEM_RELEASE);
        mem_.base = nullptr;
    }
}

std::unique_ptr<Vm> Vm::Create(const VmConfig& config) {
    if (!whvp::IsHypervisorPresent()) {
        LOG_ERROR("Windows Hypervisor Platform is not available.");
        LOG_ERROR("Please enable Hyper-V in Windows Features.");
        return nullptr;
    }

    auto vm = std::unique_ptr<Vm>(new Vm());
    vm->console_port_ = config.console_port;
    vm->input_port_ = config.input_port;
    vm->display_port_ = config.display_port;
    if (!vm->console_port_ && config.interactive) {
        vm->console_port_ = std::make_shared<StdConsolePort>();
    }
    uint64_t ram_bytes = config.memory_mb * 1024 * 1024;

    vm->whvp_vm_ = whvp::WhvpVm::Create(config.cpu_count);
    if (!vm->whvp_vm_) return nullptr;

    if (!vm->AllocateMemory(ram_bytes)) return nullptr;

    if (!vm->SetupDevices()) return nullptr;

    if (!config.disk_path.empty()) {
        if (!vm->SetupVirtioBlk(config.disk_path)) return nullptr;
    }

    if (!vm->SetupVirtioNet(config.net_link_up, config.port_forwards))
        return nullptr;

    if (!vm->SetupVirtioInput()) return nullptr;

    if (!vm->SetupVirtioGpu(config.display_width, config.display_height))
        return nullptr;

    // Register virtio-mmio devices for ACPI DSDT so the kernel discovers
    // them via the "LNRO0005" HID in the virtio_mmio driver.
    if (vm->virtio_mmio_) {
        vm->virtio_acpi_devs_.push_back({
            kVirtioMmioBase,
            static_cast<uint32_t>(VirtioMmioDevice::kMmioSize),
            kVirtioBlkIrq});
    }
    if (vm->virtio_mmio_net_) {
        vm->virtio_acpi_devs_.push_back({
            kVirtioNetMmioBase,
            static_cast<uint32_t>(VirtioMmioDevice::kMmioSize),
            kVirtioNetIrq});
    }
    if (vm->virtio_mmio_kbd_) {
        vm->virtio_acpi_devs_.push_back({
            kVirtioKbdMmioBase,
            static_cast<uint32_t>(VirtioMmioDevice::kMmioSize),
            kVirtioKbdIrq});
    }
    if (vm->virtio_mmio_tablet_) {
        vm->virtio_acpi_devs_.push_back({
            kVirtioTabletMmioBase,
            static_cast<uint32_t>(VirtioMmioDevice::kMmioSize),
            kVirtioTabletIrq});
    }
    if (vm->virtio_mmio_gpu_) {
        vm->virtio_acpi_devs_.push_back({
            kVirtioGpuMmioBase,
            static_cast<uint32_t>(VirtioMmioDevice::kMmioSize),
            kVirtioGpuIrq});
    }

    if (!vm->LoadKernel(config)) return nullptr;

    vm->cpu_count_ = config.cpu_count;
    for (uint32_t i = 0; i < config.cpu_count; i++) {
        auto vcpu = whvp::WhvpVCpu::Create(
            *vm->whvp_vm_, i, &vm->addr_space_);
        if (!vcpu) return nullptr;
        vm->vcpus_.push_back(std::move(vcpu));
    }

    // Only BSP (vCPU 0) gets initial registers; APs wait for SIPI.
    WHV_REGISTER_NAME names[64]{};
    WHV_REGISTER_VALUE values[64]{};
    uint32_t count = 0;
    x86::BuildInitialRegisters(vm->mem_.base, names, values, &count);

    if (!vm->vcpus_[0]->SetRegisters(names, values, count)) {
        LOG_ERROR("Failed to set initial vCPU registers");
        return nullptr;
    }

    LOG_INFO("VM created successfully (%u vCPUs)", config.cpu_count);
    return vm;
}

bool Vm::AllocateMemory(uint64_t size) {
    uint64_t alloc = AlignUp(size, kPageSize);

    uint8_t* base = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, alloc,
                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!base) {
        LOG_ERROR("VirtualAlloc(%llu MB) failed", alloc / (1024 * 1024));
        return false;
    }

    mem_.base = base;
    mem_.alloc_size = alloc;

    // If total RAM fits below the MMIO gap there is no split needed.
    mem_.low_size  = std::min(alloc, kMmioGapStart);
    mem_.high_size = (alloc > kMmioGapStart) ? (alloc - kMmioGapStart) : 0;
    mem_.high_base = mem_.high_size ? kMmioGapEnd : 0;

    WHV_MAP_GPA_RANGE_FLAGS flags =
        WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite |
        WHvMapGpaRangeFlagExecute;

    // Map the low region: GPA [0, low_size) -> HVA [base, base+low_size)
    if (!whvp_vm_->MapMemory(0, base, mem_.low_size, flags))
        return false;

    // Map the high region above the 4 GiB boundary if present.
    if (mem_.high_size) {
        if (!whvp_vm_->MapMemory(kMmioGapEnd, base + mem_.low_size,
                                  mem_.high_size, flags))
            return false;
        LOG_INFO("Guest RAM: %llu MB  [0-0x%llX] + [0x%llX-0x%llX] at HVA %p",
                 alloc / (1024 * 1024),
                 mem_.low_size - 1,
                 kMmioGapEnd, kMmioGapEnd + mem_.high_size - 1,
                 base);
    } else {
        LOG_INFO("Guest RAM: %llu MB at HVA %p",
                 alloc / (1024 * 1024), base);
    }
    return true;
}

bool Vm::SetupDevices() {
    uart_.SetIrqCallback([this]() { InjectIrq(4); });
    uart_.SetTxCallback([this](uint8_t byte) {
        if (!console_port_) return;
        console_port_->Write(&byte, 1);
    });
    addr_space_.AddPioDevice(
        Uart16550::kCom1Base, Uart16550::kRegCount, &uart_);
    addr_space_.AddPioDevice(
        I8254Pit::kBasePort, I8254Pit::kRegCount, &pit_);
    sys_ctrl_b_.SetPit(&pit_);
    addr_space_.AddPioDevice(
        SystemControlB::kPort, SystemControlB::kRegCount, &sys_ctrl_b_);
    addr_space_.AddPioDevice(
        CmosRtc::kBasePort, CmosRtc::kRegCount, &rtc_);
    addr_space_.AddMmioDevice(
        IoApic::kBaseAddress, IoApic::kSize, &ioapic_);
    acpi_pm_.SetShutdownCallback([this]() { RequestStop(); });
    acpi_pm_.SetSciCallback([this]() { InjectIrq(9); });
    addr_space_.AddPioDevice(
        AcpiPm::kBasePort, AcpiPm::kRegCount, &acpi_pm_);

    addr_space_.AddPioDevice(
        I8259Pic::kMasterBase, I8259Pic::kRegCount, &pic_master_);
    addr_space_.AddPioDevice(
        I8259Pic::kSlaveBase, I8259Pic::kRegCount, &pic_slave_);
    addr_space_.AddPioDevice(
        PciHostBridge::kBasePort, PciHostBridge::kRegCount, &pci_host_);

    // Silent sinks for harmless legacy ports:
    //   0x80  — POST diagnostic / IO delay
    //   0x87  — DMA page register
    //   0x2E8 — COM4   0x2F8 — COM2   0x3E8 — COM3
    addr_space_.AddPioDevice(0x80,  1, &port_sink_);
    addr_space_.AddPioDevice(0x87,  1, &port_sink_);
    addr_space_.AddPioDevice(0x2E8, 8, &port_sink_);
    addr_space_.AddPioDevice(0x2F8, 8, &port_sink_);
    addr_space_.AddPioDevice(0x3E8, 8, &port_sink_);
    addr_space_.AddPioDevice(0xC000, 0x1000, &port_sink_);  // PCI mechanism #2 data ports
    return true;
}

bool Vm::SetupVirtioBlk(const std::string& disk_path) {
    virtio_blk_ = std::make_unique<VirtioBlkDevice>();
    if (!virtio_blk_->Open(disk_path)) return false;

    virtio_mmio_ = std::make_unique<VirtioMmioDevice>();
    virtio_mmio_->Init(virtio_blk_.get(), mem_);
    virtio_mmio_->SetIrqCallback([this]() { InjectIrq(kVirtioBlkIrq); });
    virtio_blk_->SetMmioDevice(virtio_mmio_.get());

    addr_space_.AddMmioDevice(
        kVirtioMmioBase, VirtioMmioDevice::kMmioSize, virtio_mmio_.get());
    return true;
}

bool Vm::SetupVirtioNet(bool link_up, const std::vector<PortForward>& forwards) {
    net_backend_ = std::make_unique<NetBackend>();
    virtio_net_ = std::make_unique<VirtioNetDevice>(link_up);
    net_backend_->SetLinkUp(link_up);

    virtio_mmio_net_ = std::make_unique<VirtioMmioDevice>();
    virtio_mmio_net_->Init(virtio_net_.get(), mem_);
    virtio_mmio_net_->SetIrqCallback([this]() { InjectIrq(kVirtioNetIrq); });
    virtio_net_->SetMmioDevice(virtio_mmio_net_.get());

    virtio_net_->SetTxCallback([this](const uint8_t* frame, uint32_t len) {
        net_backend_->EnqueueTx(frame, len);
    });

    addr_space_.AddMmioDevice(
        kVirtioNetMmioBase, VirtioMmioDevice::kMmioSize, virtio_mmio_net_.get());

    if (!net_backend_->Start(virtio_net_.get(),
                              [this]() { InjectIrq(kVirtioNetIrq); },
                              forwards)) {
        LOG_ERROR("Failed to start network backend");
        return false;
    }
    return true;
}

bool Vm::SetupVirtioInput() {
    virtio_kbd_ = std::make_unique<VirtioInputDevice>(VirtioInputDevice::SubType::kKeyboard);
    virtio_mmio_kbd_ = std::make_unique<VirtioMmioDevice>();
    virtio_mmio_kbd_->Init(virtio_kbd_.get(), mem_);
    virtio_mmio_kbd_->SetIrqCallback([this]() { InjectIrq(kVirtioKbdIrq); });
    virtio_kbd_->SetMmioDevice(virtio_mmio_kbd_.get());
    addr_space_.AddMmioDevice(
        kVirtioKbdMmioBase, VirtioMmioDevice::kMmioSize, virtio_mmio_kbd_.get());

    virtio_tablet_ = std::make_unique<VirtioInputDevice>(VirtioInputDevice::SubType::kTablet);
    virtio_mmio_tablet_ = std::make_unique<VirtioMmioDevice>();
    virtio_mmio_tablet_->Init(virtio_tablet_.get(), mem_);
    virtio_mmio_tablet_->SetIrqCallback([this]() { InjectIrq(kVirtioTabletIrq); });
    virtio_tablet_->SetMmioDevice(virtio_mmio_tablet_.get());
    addr_space_.AddMmioDevice(
        kVirtioTabletMmioBase, VirtioMmioDevice::kMmioSize, virtio_mmio_tablet_.get());

    return true;
}

bool Vm::SetupVirtioGpu(uint32_t width, uint32_t height) {
    virtio_gpu_ = std::make_unique<VirtioGpuDevice>(width, height);
    virtio_gpu_->SetMemMap(mem_);

    if (display_port_) {
        virtio_gpu_->SetFrameCallback([this](const DisplayFrame& frame) {
            display_port_->SubmitFrame(frame);
        });
    }

    virtio_mmio_gpu_ = std::make_unique<VirtioMmioDevice>();
    virtio_mmio_gpu_->Init(virtio_gpu_.get(), mem_);
    virtio_mmio_gpu_->SetIrqCallback([this]() { InjectIrq(kVirtioGpuIrq); });
    virtio_gpu_->SetMmioDevice(virtio_mmio_gpu_.get());
    addr_space_.AddMmioDevice(
        kVirtioGpuMmioBase, VirtioMmioDevice::kMmioSize, virtio_mmio_gpu_.get());

    return true;
}

bool Vm::LoadKernel(const VmConfig& config) {
    x86::BootConfig boot_cfg;
    boot_cfg.kernel_path = config.kernel_path;
    boot_cfg.initrd_path = config.initrd_path;
    boot_cfg.cmdline = config.cmdline;
    boot_cfg.mem = mem_;
    boot_cfg.cpu_count = config.cpu_count;
    boot_cfg.virtio_devs = virtio_acpi_devs_;

    uint64_t kernel_size = x86::LoadLinuxKernel(boot_cfg);
    if (kernel_size == 0) {
        return false;
    }
    return true;
}

void Vm::InputThreadFunc() {
    if (!console_port_) return;

    uint8_t buf[32]{};
    while (running_) {
        size_t read = console_port_->Read(buf, sizeof(buf));
        if (read == 0) {
            continue;
        }
        for (size_t i = 0; i < read; ++i) {
            uart_.PushInput(buf[i]);
        }
        InjectIrq(4);
    }
}

void Vm::InjectIrq(uint8_t irq) {
    uint64_t rte = 0;
    if (!ioapic_.GetRedirEntry(irq, &rte)) return;

    bool masked = (rte >> 16) & 1;
    if (masked) return;

    uint32_t vector = rte & 0xFF;
    if (vector == 0) return;

    WHV_INTERRUPT_CONTROL ctrl{};
    ctrl.Type = WHvX64InterruptTypeFixed;
    ctrl.DestinationMode = ((rte >> 11) & 1)
        ? WHvX64InterruptDestinationModeLogical
        : WHvX64InterruptDestinationModePhysical;
    ctrl.TriggerMode = ((rte >> 15) & 1)
        ? WHvX64InterruptTriggerModeLevel
        : WHvX64InterruptTriggerModeEdge;
    ctrl.Destination = static_cast<uint32_t>(rte >> 56);
    ctrl.Vector = vector;

    WHvRequestInterrupt(whvp_vm_->Handle(), &ctrl, sizeof(ctrl));
}

void Vm::VCpuThreadFunc(uint32_t vcpu_index) {
    auto& vcpu = vcpus_[vcpu_index];
    uint64_t exit_count = 0;

    while (running_) {
        auto action = vcpu->RunOnce();
        exit_count++;

        switch (action) {
        case whvp::VCpuExitAction::kContinue:
            break;

        case whvp::VCpuExitAction::kHalt:
            SwitchToThread();
            break;

        case whvp::VCpuExitAction::kShutdown:
            LOG_INFO("vCPU %u: shutdown (after %llu exits)", vcpu_index, exit_count);
            RequestStop();
            return;

        case whvp::VCpuExitAction::kError:
            LOG_ERROR("vCPU %u: error (after %llu exits)", vcpu_index, exit_count);
            exit_code_.store(1);
            RequestStop();
            return;
        }
    }

    LOG_INFO("vCPU %u stopped (total exits: %llu)", vcpu_index, exit_count);
}

void Vm::HidInputThreadFunc() {
    if (!input_port_) return;

    while (running_) {
        KeyboardEvent kev;
        while (input_port_->PollKeyboard(&kev)) {
            if (virtio_kbd_) {
                virtio_kbd_->InjectEvent(EV_KEY, static_cast<uint16_t>(kev.key_code),
                                         kev.pressed ? 1 : 0);
                virtio_kbd_->InjectEvent(EV_SYN, SYN_REPORT, 0);
            }
        }

        PointerEvent pev;
        while (input_port_->PollPointer(&pev)) {
            if (virtio_tablet_) {
                virtio_tablet_->InjectEvent(EV_ABS, ABS_X, static_cast<uint32_t>(pev.x));
                virtio_tablet_->InjectEvent(EV_ABS, ABS_Y, static_cast<uint32_t>(pev.y));
                virtio_tablet_->InjectEvent(EV_KEY, BTN_LEFT, (pev.buttons & 1) ? 1 : 0);
                virtio_tablet_->InjectEvent(EV_KEY, BTN_RIGHT, (pev.buttons & 2) ? 1 : 0);
                virtio_tablet_->InjectEvent(EV_KEY, BTN_MIDDLE, (pev.buttons & 4) ? 1 : 0);
                virtio_tablet_->InjectEvent(EV_SYN, SYN_REPORT, 0);
            }
        }

        Sleep(2);
    }
}

int Vm::Run() {
    running_ = true;
    LOG_INFO("Starting VM execution...");

    if (console_port_) {
        input_thread_ = std::thread(&Vm::InputThreadFunc, this);
    }

    if (input_port_) {
        hid_input_thread_ = std::thread(&Vm::HidInputThreadFunc, this);
    }

    for (uint32_t i = 0; i < cpu_count_; i++) {
        vcpu_threads_.emplace_back(&Vm::VCpuThreadFunc, this, i);
    }

    for (auto& t : vcpu_threads_) {
        t.join();
    }

    return exit_code_.load();
}

void Vm::RequestStop() {
    running_ = false;
    for (auto& vcpu : vcpus_) {
        WHvCancelRunVirtualProcessor(
            whvp_vm_->Handle(), vcpu->VpIndex(), 0);
    }
}

void Vm::TriggerPowerButton() {
    acpi_pm_.TriggerPowerButton();
}

void Vm::InjectConsoleBytes(const uint8_t* data, size_t size) {
    if (!data || size == 0) return;
    for (size_t i = 0; i < size; ++i) {
        uart_.PushInput(data[i]);
    }
    InjectIrq(4);
}

void Vm::SetNetLinkUp(bool up) {
    if (virtio_net_) virtio_net_->SetLinkUp(up);
    if (net_backend_) net_backend_->SetLinkUp(up);
}

void Vm::UpdatePortForwards(const std::vector<PortForward>& forwards) {
    if (net_backend_) net_backend_->UpdatePortForwards(forwards);
}

void Vm::InjectKeyEvent(uint32_t evdev_code, bool pressed) {
    if (virtio_kbd_) {
        virtio_kbd_->InjectEvent(EV_KEY, static_cast<uint16_t>(evdev_code),
                                 pressed ? 1 : 0);
        virtio_kbd_->InjectEvent(EV_SYN, SYN_REPORT, 0);
    }
}

void Vm::InjectPointerEvent(int32_t x, int32_t y, uint32_t buttons) {
    if (virtio_tablet_) {
        virtio_tablet_->InjectEvent(EV_ABS, ABS_X, static_cast<uint32_t>(x));
        virtio_tablet_->InjectEvent(EV_ABS, ABS_Y, static_cast<uint32_t>(y));
        virtio_tablet_->InjectEvent(EV_KEY, BTN_LEFT, (buttons & 1) ? 1 : 0);
        virtio_tablet_->InjectEvent(EV_KEY, BTN_RIGHT, (buttons & 2) ? 1 : 0);
        virtio_tablet_->InjectEvent(EV_KEY, BTN_MIDDLE, (buttons & 4) ? 1 : 0);
        virtio_tablet_->InjectEvent(EV_SYN, SYN_REPORT, 0);
    }
}
