#include "vmm/vm.h"
#include "arch/x86_64/boot.h"

static constexpr uint64_t kVirtioMmioBase    = 0xd0000000;
static constexpr uint8_t  kVirtioBlkIrq      = 5;
static constexpr uint64_t kVirtioNetMmioBase = 0xd0000200;
static constexpr uint8_t  kVirtioNetIrq      = 6;

Vm::~Vm() {
    running_ = false;
    if (input_thread_.joinable())
        input_thread_.join();
    vcpu_.reset();
    whvp_vm_.reset();
    if (ram_) {
        VirtualFree(ram_, 0, MEM_RELEASE);
        ram_ = nullptr;
    }
}

std::unique_ptr<Vm> Vm::Create(const VmConfig& config) {
    if (!whvp::IsHypervisorPresent()) {
        LOG_ERROR("Windows Hypervisor Platform is not available.");
        LOG_ERROR("Please enable Hyper-V in Windows Features.");
        return nullptr;
    }

    auto vm = std::unique_ptr<Vm>(new Vm());
    uint64_t ram_bytes = config.memory_mb * 1024 * 1024;

    vm->whvp_vm_ = whvp::WhvpVm::Create(config.cpu_count);
    if (!vm->whvp_vm_) return nullptr;

    if (!vm->AllocateMemory(ram_bytes)) return nullptr;

    if (!vm->SetupDevices()) return nullptr;

    if (!config.disk_path.empty()) {
        if (!vm->SetupVirtioBlk(config.disk_path)) return nullptr;
    }

    if (config.net_enabled) {
        if (!vm->SetupVirtioNet(config.port_forwards)) return nullptr;
    }

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

    if (!vm->LoadKernel(config)) return nullptr;

    vm->vcpu_ = whvp::WhvpVCpu::Create(
        *vm->whvp_vm_, 0, &vm->addr_space_);
    if (!vm->vcpu_) return nullptr;

    WHV_REGISTER_NAME names[64]{};
    WHV_REGISTER_VALUE values[64]{};
    uint32_t count = 0;
    x86::BuildInitialRegisters(vm->ram_, names, values, &count);

    if (!vm->vcpu_->SetRegisters(names, values, count)) {
        LOG_ERROR("Failed to set initial vCPU registers");
        return nullptr;
    }

    LOG_INFO("VM created successfully");
    return vm;
}

bool Vm::AllocateMemory(uint64_t size) {
    ram_size_ = AlignUp(size, kPageSize);
    ram_ = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, ram_size_,
                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!ram_) {
        LOG_ERROR("VirtualAlloc(%llu MB) failed", ram_size_ / (1024 * 1024));
        return false;
    }
    memset(ram_, 0, ram_size_);

    WHV_MAP_GPA_RANGE_FLAGS flags =
        WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite |
        WHvMapGpaRangeFlagExecute;

    if (!whvp_vm_->MapMemory(0, ram_, ram_size_, flags)) {
        return false;
    }

    LOG_INFO("Guest RAM: %llu MB at HVA %p",
             ram_size_ / (1024 * 1024), ram_);
    return true;
}

bool Vm::SetupDevices() {
    uart_.SetIrqCallback([this]() { InjectIrq(4); });
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
    addr_space_.AddPioDevice(
        AcpiPm::kBasePort, AcpiPm::kRegCount, &acpi_pm_);
    return true;
}

bool Vm::SetupVirtioBlk(const std::string& disk_path) {
    virtio_blk_ = std::make_unique<VirtioBlkDevice>();
    if (!virtio_blk_->Open(disk_path)) return false;

    virtio_mmio_ = std::make_unique<VirtioMmioDevice>();
    virtio_mmio_->Init(virtio_blk_.get(), ram_, ram_size_);
    virtio_mmio_->SetIrqCallback([this]() { InjectIrq(kVirtioBlkIrq); });
    virtio_blk_->SetMmioDevice(virtio_mmio_.get());

    addr_space_.AddMmioDevice(
        kVirtioMmioBase, VirtioMmioDevice::kMmioSize, virtio_mmio_.get());
    return true;
}

bool Vm::SetupVirtioNet(const std::vector<PortForward>& forwards) {
    net_backend_ = std::make_unique<NetBackend>();
    virtio_net_ = std::make_unique<VirtioNetDevice>();

    virtio_mmio_net_ = std::make_unique<VirtioMmioDevice>();
    virtio_mmio_net_->Init(virtio_net_.get(), ram_, ram_size_);
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

bool Vm::LoadKernel(const VmConfig& config) {
    x86::BootConfig boot_cfg;
    boot_cfg.kernel_path = config.kernel_path;
    boot_cfg.initrd_path = config.initrd_path;
    boot_cfg.cmdline = config.cmdline;
    boot_cfg.ram_size = ram_size_;
    boot_cfg.virtio_devs = virtio_acpi_devs_;

    uint64_t kernel_size = x86::LoadLinuxKernel(boot_cfg, ram_, ram_size_);
    if (kernel_size == 0) {
        return false;
    }
    return true;
}

void Vm::InputThreadFunc() {
    HANDLE h_stdin = GetStdHandle(STD_INPUT_HANDLE);
    if (h_stdin == INVALID_HANDLE_VALUE) return;

    DWORD old_mode = 0;
    BOOL is_console = GetConsoleMode(h_stdin, &old_mode);

    if (is_console) {
        SetConsoleMode(h_stdin, ENABLE_WINDOW_INPUT);
    }

    while (running_) {
        if (is_console) {
            DWORD avail = 0;
            if (!GetNumberOfConsoleInputEvents(h_stdin, &avail) || avail == 0) {
                Sleep(16);
                continue;
            }

            INPUT_RECORD rec{};
            DWORD read_count = 0;
            if (!ReadConsoleInput(h_stdin, &rec, 1, &read_count) || read_count == 0)
                continue;

            if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown)
                continue;

            char ch = rec.Event.KeyEvent.uChar.AsciiChar;
            if (ch == 0) continue;
            if (ch == '\r') ch = '\n';

            uart_.PushInput(static_cast<uint8_t>(ch));
            InjectIrq(4);
        } else {
            // Pipe/redirected stdin: blocking read
            char buf[1];
            DWORD bytes_read = 0;
            if (ReadFile(h_stdin, buf, 1, &bytes_read, nullptr) && bytes_read > 0) {
                char ch = buf[0];
                if (ch == '\r') ch = '\n';
                uart_.PushInput(static_cast<uint8_t>(ch));
                InjectIrq(4);
            } else {
                Sleep(16);
            }
        }
    }

    if (is_console) {
        SetConsoleMode(h_stdin, old_mode);
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

int Vm::Run() {
    running_ = true;
    LOG_INFO("Starting VM execution...");

    input_thread_ = std::thread(&Vm::InputThreadFunc, this);

    uint64_t exit_count = 0;
    while (running_) {
        auto action = vcpu_->RunOnce();
        exit_count++;

        switch (action) {
        case whvp::VCpuExitAction::kContinue:
            break;

        case whvp::VCpuExitAction::kHalt:
            SwitchToThread();
            break;

        case whvp::VCpuExitAction::kShutdown:
            LOG_INFO("VM shutdown (after %llu exits)", exit_count);
            running_ = false;
            return 0;

        case whvp::VCpuExitAction::kError:
            LOG_ERROR("VM error (after %llu exits)", exit_count);
            running_ = false;
            return 1;
        }
    }

    LOG_INFO("VM stopped (total exits: %llu)", exit_count);
    return 0;
}

void Vm::RequestStop() {
    running_ = false;
    if (vcpu_) {
        WHvCancelRunVirtualProcessor(
            whvp_vm_->Handle(), vcpu_->VpIndex(), 0);
    }
}
