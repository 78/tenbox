#include "core/vmm/vm.h"
#include "core/vmm/vm_platform.h"
#include <algorithm>

#if defined(__linux__)
#include <cerrno>
#include <cstring>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

#if defined(__APPLE__) && defined(__x86_64__)
#include "core/arch/x86_64/x86_machine.h"
#include "platform/macos/hypervisor/x86_64/hvf_vcpu.h"
#elif defined(__APPLE__) && defined(__aarch64__)
#include "core/arch/aarch64/aarch64_machine.h"
#include "platform/macos/hypervisor/aarch64/hvf_vcpu.h"
#include "platform/macos/hypervisor/aarch64/hvf_vm.h"
#elif defined(_WIN32)
#include "core/arch/x86_64/x86_machine.h"
#elif defined(__linux__) && defined(__x86_64__)
#include "core/arch/x86_64/x86_machine.h"
#elif defined(__linux__) && defined(__aarch64__)
#include "core/arch/aarch64/aarch64_machine.h"
#include "platform/linux/hypervisor/aarch64/kvm_vcpu.h"
#include "platform/linux/hypervisor/aarch64/kvm_vm.h"
#endif

static std::unique_ptr<MachineModel> CreateMachineModel() {
#if defined(_WIN32) || (defined(__APPLE__) && defined(__x86_64__)) || (defined(__linux__) && defined(__x86_64__))
    return std::make_unique<X86Machine>();
#elif (defined(__APPLE__) && defined(__aarch64__)) || (defined(__linux__) && defined(__aarch64__))
    return std::make_unique<Aarch64Machine>();
#else
    LOG_ERROR("No machine model available for this platform/architecture");
    return nullptr;
#endif
}

static std::string GetDefaultCmdline(bool debug_mode) {
#ifdef __aarch64__
    if (debug_mode) {
        return "console=ttyAMA0 earlycon root=/dev/vda1 rw random.trust_bootloader=on";
    } else {
        return "console=ttyAMA0 quiet loglevel=4 earlycon root=/dev/vda1 rw random.trust_bootloader=on";
    }
#else
    if (debug_mode) {
        return "console=ttyS0 earlyprintk=serial lapic tsc=reliable clocksource=kvm-clock no_timer_check i8042.noprobe random.trust_cpu=on";
    } else {
        return "console=ttyS0 quiet loglevel=4 lapic tsc=reliable clocksource=kvm-clock no_timer_check i8042.noprobe random.trust_cpu=on";
    }
#endif
}

Vm::~Vm() {
    running_ = false;
    if (console_input_thread_.joinable()) {
        console_input_thread_.join();
    }
    for (auto& t : vcpu_threads_) {
        if (t.joinable()) t.join();
    }

    // Tear down irqfds (detach uv_poll, unregister with kvm, close fds, stop
    // io_loop_) before destroying the hypervisor VM.
    ShutdownIrqFds();
    io_loop_.Stop();

    if (vdagent_handler_) {
        vdagent_handler_->SetClipboardCallback(nullptr);
    }
    if (virtio_serial_) {
        virtio_serial_->SetDataCallback(nullptr);
    }
    if (virtio_gpu_) {
        virtio_gpu_->SetFrameCallback(nullptr);
        virtio_gpu_->SetCursorCallback(nullptr);
        virtio_gpu_->SetScanoutStateCallback(nullptr);
    }

    if (net_backend_) {
        net_backend_->Stop();
    }

    vcpus_.clear();
    hv_vm_.reset();
    if (mem_.base) {
        VmPlatform::FreeRam(mem_.base, mem_.alloc_size);
        mem_.base = nullptr;
    }
}

std::unique_ptr<Vm> Vm::Create(const VmConfig& config) {
    if (!VmPlatform::IsHypervisorPresent()) {
        LOG_ERROR("Hardware hypervisor is not available on this platform.");
        return nullptr;
    }

    auto vm = std::unique_ptr<Vm>(new Vm());
    vm->console_port_ = config.console_port;
    vm->input_port_ = config.input_port;
    vm->display_port_ = config.display_port;
    vm->clipboard_port_ = config.clipboard_port;
    vm->audio_port_ = config.audio_port;
    if (!vm->console_port_ && config.interactive) {
        vm->console_port_ = VmPlatform::CreateConsolePort();
        // We own the console port: no external IPC controller will pump stdin
        // for us, so we need to run our own input thread.
        vm->owned_console_input_ = true;
    }

    vm->machine_ = CreateMachineModel();
    if (!vm->machine_) return nullptr;

    vm->hv_vm_ = VmPlatform::CreateHypervisor(config.cpu_count);
    if (!vm->hv_vm_) return nullptr;

    uint64_t ram_bytes = config.memory_mb * 1024 * 1024;
    if (!vm->AllocateMemory(ram_bytes)) return nullptr;

    vm->hv_vm_->SetGuestMemMap(&vm->mem_);

    if (!vm->machine_->SetupPlatformDevices(
            vm->addr_space_, vm->mem_, vm->hv_vm_.get(),
            vm->console_port_,
            &vm->io_loop_,
            [&vm_ref = *vm]() { vm_ref.RequestStop(); },
            [&vm_ref = *vm]() { vm_ref.RequestReboot(); })) {
        LOG_ERROR("Failed to set up platform devices");
        return nullptr;
    }

    vm->vcpu_startup_.resize(config.cpu_count);
    for (uint32_t i = 0; i < config.cpu_count; i++) {
        vm->vcpu_startup_[i] = std::make_unique<VCpuStartupState>();
    }

#if defined(_WIN32) || (defined(__APPLE__) && defined(__x86_64__)) || (defined(__linux__) && defined(__x86_64__))
    {
        auto* x86m = dynamic_cast<X86Machine*>(vm->machine_.get());
        if (x86m) {
            x86m->InitLapic(config.cpu_count);
            x86m->SetInitCallback([&vm_ref = *vm](uint32_t target_cpu) {
                if (target_cpu >= vm_ref.cpu_count_) return;
                auto& state = vm_ref.vcpu_startup_[target_cpu];
                std::lock_guard<std::mutex> lock(state->mutex);
                state->init_received = true;
            });
            x86m->SetSipiCallback([&vm_ref = *vm](uint32_t target_cpu, uint8_t vector) {
                if (target_cpu >= vm_ref.cpu_count_) return;
                auto& state = vm_ref.vcpu_startup_[target_cpu];
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->init_received) return;
                if (state->started) return;
                state->sipi_vector = vector;
                state->started = true;
                state->cv.notify_one();
            });
            x86m->SetIpiCallback([&vm_ref = *vm](uint32_t vector, uint32_t dest, uint8_t shorthand) {
                uint32_t sender = LocalApic::GetCurrentCpu();
                if (shorthand == 0) {
                    vm_ref.hv_vm_->QueueInterrupt(vector, dest);
                } else if (shorthand == 1) {
                    vm_ref.hv_vm_->QueueInterrupt(vector, sender);
                } else if (shorthand == 2) {
                    for (uint32_t i = 0; i < vm_ref.cpu_count_; i++)
                        vm_ref.hv_vm_->QueueInterrupt(vector, i);
                } else if (shorthand == 3) {
                    for (uint32_t i = 0; i < vm_ref.cpu_count_; i++)
                        if (i != sender) vm_ref.hv_vm_->QueueInterrupt(vector, i);
                }
            });
        }
    }
#endif

    auto slots = vm->machine_->GetVirtioSlots();

    if (!config.disk_path.empty()) {
        if (!vm->SetupVirtioBlk(config.disk_path, slots[0])) return nullptr;
    }

    if (!vm->SetupVirtioNet(config.net_link_up, config.port_forwards, config.guest_forwards, slots[1]))
        return nullptr;

    if (!vm->SetupVirtioInput(slots[2], slots[3])) return nullptr;

    if (!vm->SetupVirtioGpu(config.display_width, config.display_height, slots[4]))
        return nullptr;

    if (!vm->SetupVirtioSerial(slots[5]))
        return nullptr;

    if (!vm->SetupVirtioFs(config.shared_folders, slots[6]))
        return nullptr;

    if (!vm->SetupVirtioSnd(slots[7]))
        return nullptr;

    vm->cpu_count_ = config.cpu_count;
    // Resize the vCPU slot vector; actual vCPU objects are created per-thread.
    vm->vcpus_.resize(config.cpu_count);

    // Save config for FinalizeBoot (called inside Run after all vCPUs are ready).
    vm->boot_config_ = config;
    if (vm->boot_config_.cmdline.empty()) {
        vm->boot_config_.cmdline = GetDefaultCmdline(config.debug_mode);
    }

#if defined(__APPLE__) && defined(__aarch64__)
    // macOS < 15 Hypervisor.framework doesn't properly virtualise PAC
    // instructions and exposes inconsistent ID registers across vCPUs.
    // arm64.nopauth tells the Linux kernel to disable pointer authentication
    // regardless of what the ID registers advertise.
    {
        auto* hvf = dynamic_cast<hvf::HvfVm*>(vm->hv_vm_.get());
        if (hvf && hvf->UsesSoftGic()) {
            auto& cl = vm->boot_config_.cmdline;
            if (cl.find("arm64.nopauth") == std::string::npos) {
                cl += " arm64.nopauth";
                LOG_INFO("Software GIC active (macOS < 15): "
                         "appended arm64.nopauth to kernel cmdline");
            }
        }
    }
#endif

    LOG_INFO("VM created successfully (%u vCPUs)", config.cpu_count);
    return vm;
}

void Vm::SetupVCpuCallbacks(uint32_t vcpu_index) {
#if defined(__APPLE__) && defined(__aarch64__)
    auto* hvf_vcpu = dynamic_cast<hvf::HvfVCpu*>(vcpus_[vcpu_index].get());
    if (hvf_vcpu) {
        hvf_vcpu->SetPsciCpuOnCallback([this](const hvf::PsciCpuOnRequest& req) -> int {
            if (req.target_cpu >= cpu_count_) return -2;
            auto& state = vcpu_startup_[req.target_cpu];
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->started) return -4;
            state->entry_addr = req.entry_addr;
            state->context_id = req.context_id;
            state->started = true;
            state->cv.notify_one();
            return 0;
        });
        hvf_vcpu->SetPsciShutdownCallback([this]() { RequestStop(); });
        hvf_vcpu->SetPsciRebootCallback([this]() { RequestReboot(); });
    }
#elif defined(__linux__) && defined(__aarch64__)
    // In-kernel PSCI handles CPU_ON; only SYSTEM_OFF / SYSTEM_RESET bubble
    // up to userspace as KVM_EXIT_SYSTEM_EVENT.
    auto* kvm_vcpu = dynamic_cast<kvm::KvmVCpu*>(vcpus_[vcpu_index].get());
    if (kvm_vcpu) {
        kvm_vcpu->SetShutdownCallback([this]() { RequestStop(); });
        kvm_vcpu->SetRebootCallback([this]() { RequestReboot(); });
    }
#else
    (void)vcpu_index;
#endif
}

void Vm::FinalizeBoot(const VmConfig& config) {
#if defined(_WIN32) || (defined(__APPLE__) && defined(__x86_64__)) || (defined(__linux__) && defined(__x86_64__))
    {
        auto* x86m = dynamic_cast<X86Machine*>(machine_.get());
        if (x86m) {
            std::vector<uint32_t> apic_ids;
            for (auto& vcpu : vcpus_) {
                apic_ids.push_back(vcpu->ApicId());
            }
            x86m->SetApicIds(std::move(apic_ids));
        }
    }
#endif

    if (!machine_->LoadKernel(config, mem_, active_virtio_slots_)) {
        LOG_ERROR("Failed to load kernel");
        RequestStop();
        return;
    }

    // NOTE: SetupBootVCpu is called from vCPU 0's own thread (after
    // boot_complete_) because HVF requires register writes to happen on the
    // thread that created the vCPU.
}

bool Vm::AllocateMemory(uint64_t size) {
    uint64_t alloc = AlignUp(size, kPageSize);

    uint8_t* base = VmPlatform::AllocateRam(alloc);
    if (!base) {
        LOG_ERROR("Failed to allocate %" PRIu64 " MB guest RAM",
                  alloc / (1024 * 1024));
        return false;
    }

    GPA mmio_gap_start = machine_->MmioGapStart();
    GPA mmio_gap_end = machine_->MmioGapEnd();
    GPA ram_base = machine_->RamBase();

    mem_.base = base;
    mem_.alloc_size = alloc;
    mem_.ram_base = ram_base;

    if (ram_base == 0) {
        // x86-style layout: RAM at GPA 0 with MMIO gap in the middle
        mem_.low_size  = std::min(alloc, mmio_gap_start);
        mem_.high_size = (alloc > mmio_gap_start) ? (alloc - mmio_gap_start) : 0;
        mem_.high_base = mem_.high_size ? mmio_gap_end : 0;

        if (!hv_vm_->MapMemory(0, base, mem_.low_size, true))
            return false;

        if (mem_.high_size) {
            if (!hv_vm_->MapMemory(mmio_gap_end, base + mem_.low_size,
                                    mem_.high_size, true))
                return false;
            LOG_INFO("Guest RAM: %" PRIu64 " MB  [0-0x%" PRIX64 "] + "
                     "[0x%" PRIX64 "-0x%" PRIX64 "] at HVA %p",
                     alloc / (1024 * 1024),
                     mem_.low_size - 1,
                     mmio_gap_end,
                     mmio_gap_end + mem_.high_size - 1,
                     base);
        } else {
            LOG_INFO("Guest RAM: %" PRIu64 " MB at HVA %p",
                     alloc / (1024 * 1024), base);
        }
    } else {
        // ARM-style layout: RAM starts at a high base, MMIO below
        mem_.low_size  = alloc;
        mem_.high_size = 0;
        mem_.high_base = 0;

        if (!hv_vm_->MapMemory(ram_base, base, alloc, true))
            return false;

        LOG_INFO("Guest RAM: %" PRIu64 " MB at GPA 0x%" PRIX64 ", HVA %p",
                 alloc / (1024 * 1024), ram_base, base);
    }
    return true;
}

void Vm::InjectIrq(uint8_t irq) {
    machine_->InjectIrq(hv_vm_.get(), irq);
}

void Vm::SetIrqLevel(uint8_t irq, bool asserted) {
    machine_->SetIrqLevel(hv_vm_.get(), irq, asserted);
}

bool Vm::TryEnableIrqFd(VirtioMmioDevice* dev, uint8_t slot_irq) {
#if defined(__linux__)
    IrqFdSlot slot;
    #if defined(__aarch64__)
        slot.gsi = static_cast<uint32_t>(slot_irq) + 32;  // absolute SPI INTID
    #elif defined(__x86_64__)
        slot.gsi = static_cast<uint32_t>(slot_irq);       // IOAPIC pin
    #else
        (void)slot_irq;
        return false;
    #endif
    slot.dev = dev;
    irqfd_slots_.push_back(slot);
    return true;
#else
    (void)dev;
    (void)slot_irq;
    return false;
#endif
}

void Vm::InstallIrqFds() {
#if defined(__linux__)
    if (irqfd_slots_.empty() || !hv_vm_) return;

    // Allocate trigger + resample eventfds per slot, then ask the hypervisor
    // to register each one. On any failure, drop the slot from the list
    // (its virtio device keeps using the SetIrqLevelCallback fallback).
    size_t write_idx = 0;
    for (size_t read_idx = 0; read_idx < irqfd_slots_.size(); ++read_idx) {
        IrqFdSlot& s = irqfd_slots_[read_idx];

        int trig = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        int resamp = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (trig < 0 || resamp < 0) {
            LOG_WARN("irqfd: eventfd() failed: %s", strerror(errno));
            if (trig >= 0) ::close(trig);
            if (resamp >= 0) ::close(resamp);
            continue;
        }
        if (!hv_vm_->RegisterLevelIrqFd(s.gsi, trig, resamp)) {
            ::close(trig);
            ::close(resamp);
            continue;
        }

        s.trigger_fd = trig;
        s.resample_fd = resamp;
        s.dev->SetIrqEventFd(trig);
        io_loop_.AttachIrqFd(s.dev, trig, resamp);
        irqfd_slots_[write_idx++] = s;
    }
    irqfd_slots_.resize(write_idx);

    if (!irqfd_slots_.empty()) {
        LOG_INFO("irqfd: %zu slots attached to io_loop", irqfd_slots_.size());
    }
#endif
}

void Vm::ShutdownIrqFds() {
#if defined(__linux__)
    // Detach uv_poll on the io_loop first (synchronously-ish: Post returns
    // immediately but the detach closure runs in the io_thread before Stop
    // completes). Then unregister with the kernel and close fds.
    for (auto& s : irqfd_slots_) {
        if (s.dev) io_loop_.DetachIrqFd(s.dev);
    }
    for (auto& s : irqfd_slots_) {
        if (s.trigger_fd >= 0) {
            if (hv_vm_) hv_vm_->UnregisterIrqFd(s.gsi, s.trigger_fd);
            ::close(s.trigger_fd);
            s.trigger_fd = -1;
        }
        if (s.resample_fd >= 0) {
            ::close(s.resample_fd);
            s.resample_fd = -1;
        }
        if (s.dev) {
            s.dev->SetIrqEventFd(-1);  // revert to callback path on teardown
        }
    }
    irqfd_slots_.clear();
#endif
}

bool Vm::SetupVirtioBlk(const std::string& disk_path, const VirtioDeviceSlot& slot) {
    virtio_blk_ = std::make_unique<VirtioBlkDevice>();
    if (!virtio_blk_->Open(disk_path)) return false;

    virtio_mmio_ = std::make_unique<VirtioMmioDevice>();
    virtio_mmio_->Init(virtio_blk_.get(), mem_);
    virtio_mmio_->SetIrqCallback([this, irq = slot.irq]() { InjectIrq(irq); });
    virtio_mmio_->SetIrqLevelCallback([this, irq = slot.irq](bool a) { SetIrqLevel(irq, a); });
    TryEnableIrqFd(virtio_mmio_.get(), slot.irq);
    virtio_blk_->SetMmioDevice(virtio_mmio_.get());

    addr_space_.AddMmioDevice(
        slot.mmio_base, VirtioMmioDevice::kMmioSize, virtio_mmio_.get());
    active_virtio_slots_.push_back(slot);
    return true;
}

bool Vm::SetupVirtioNet(bool link_up, const std::vector<PortForward>& forwards,
                        const std::vector<GuestForward>& guest_forwards,
                        const VirtioDeviceSlot& slot) {
    net_backend_ = std::make_unique<NetBackend>();
    virtio_net_ = std::make_unique<VirtioNetDevice>(link_up);
    net_backend_->SetLinkUp(link_up);

    virtio_mmio_net_ = std::make_unique<VirtioMmioDevice>();
    virtio_mmio_net_->Init(virtio_net_.get(), mem_);
    virtio_mmio_net_->SetIrqCallback([this, irq = slot.irq]() { InjectIrq(irq); });
    virtio_mmio_net_->SetIrqLevelCallback([this, irq = slot.irq](bool a) { SetIrqLevel(irq, a); });
    TryEnableIrqFd(virtio_mmio_net_.get(), slot.irq);
    virtio_net_->SetMmioDevice(virtio_mmio_net_.get());

    virtio_net_->SetTxCallback([this](const uint8_t* frame, uint32_t len) {
        net_backend_->EnqueueTx(frame, len);
    });

    addr_space_.AddMmioDevice(
        slot.mmio_base, VirtioMmioDevice::kMmioSize, virtio_mmio_net_.get());

    if (!net_backend_->Start(virtio_net_.get(),
                              [this, irq = slot.irq]() { InjectIrq(irq); },
                              forwards, guest_forwards)) {
        LOG_ERROR("Failed to start network backend");
        return false;
    }
    active_virtio_slots_.push_back(slot);
    return true;
}

bool Vm::SetupVirtioInput(const VirtioDeviceSlot& kbd_slot,
                          const VirtioDeviceSlot& tablet_slot) {
    virtio_kbd_ = std::make_unique<VirtioInputDevice>(VirtioInputDevice::SubType::kKeyboard);
    virtio_mmio_kbd_ = std::make_unique<VirtioMmioDevice>();
    virtio_mmio_kbd_->Init(virtio_kbd_.get(), mem_);
    virtio_mmio_kbd_->SetIrqCallback([this, irq = kbd_slot.irq]() { InjectIrq(irq); });
    virtio_mmio_kbd_->SetIrqLevelCallback([this, irq = kbd_slot.irq](bool a) { SetIrqLevel(irq, a); });
    TryEnableIrqFd(virtio_mmio_kbd_.get(), kbd_slot.irq);
    virtio_kbd_->SetMmioDevice(virtio_mmio_kbd_.get());
    addr_space_.AddMmioDevice(
        kbd_slot.mmio_base, VirtioMmioDevice::kMmioSize, virtio_mmio_kbd_.get());
    active_virtio_slots_.push_back(kbd_slot);

    virtio_tablet_ = std::make_unique<VirtioInputDevice>(VirtioInputDevice::SubType::kTablet);
    virtio_mmio_tablet_ = std::make_unique<VirtioMmioDevice>();
    virtio_mmio_tablet_->Init(virtio_tablet_.get(), mem_);
    virtio_mmio_tablet_->SetIrqCallback([this, irq = tablet_slot.irq]() { InjectIrq(irq); });
    virtio_mmio_tablet_->SetIrqLevelCallback([this, irq = tablet_slot.irq](bool a) { SetIrqLevel(irq, a); });
    TryEnableIrqFd(virtio_mmio_tablet_.get(), tablet_slot.irq);
    virtio_tablet_->SetMmioDevice(virtio_mmio_tablet_.get());
    addr_space_.AddMmioDevice(
        tablet_slot.mmio_base, VirtioMmioDevice::kMmioSize, virtio_mmio_tablet_.get());
    active_virtio_slots_.push_back(tablet_slot);

    return true;
}

bool Vm::SetupVirtioGpu(uint32_t width, uint32_t height, const VirtioDeviceSlot& slot) {
    virtio_gpu_ = std::make_unique<VirtioGpuDevice>(width, height);
    virtio_gpu_->SetMemMap(mem_);

    if (display_port_) {
        virtio_gpu_->SetFrameCallback([this](DisplayFrame frame) {
            display_port_->SubmitFrame(std::move(frame));
        });
        virtio_gpu_->SetCursorCallback([this](const CursorInfo& cursor) {
            display_port_->SubmitCursor(cursor);
        });
        virtio_gpu_->SetScanoutStateCallback([this](bool active, uint32_t w, uint32_t h) {
            display_port_->SubmitScanoutState(active, w, h);
        });
    }

    virtio_mmio_gpu_ = std::make_unique<VirtioMmioDevice>();
    virtio_mmio_gpu_->Init(virtio_gpu_.get(), mem_);
    virtio_mmio_gpu_->SetIrqCallback([this, irq = slot.irq]() { InjectIrq(irq); });
    virtio_mmio_gpu_->SetIrqLevelCallback([this, irq = slot.irq](bool a) { SetIrqLevel(irq, a); });
    TryEnableIrqFd(virtio_mmio_gpu_.get(), slot.irq);
    virtio_gpu_->SetMmioDevice(virtio_mmio_gpu_.get());
    addr_space_.AddMmioDevice(
        slot.mmio_base, VirtioMmioDevice::kMmioSize, virtio_mmio_gpu_.get());
    active_virtio_slots_.push_back(slot);

    return true;
}

bool Vm::SetupVirtioSerial(const VirtioDeviceSlot& slot) {
    virtio_serial_ = std::make_unique<VirtioSerialDevice>(2);
    virtio_serial_->SetPortName(0, "com.redhat.spice.0");
    virtio_serial_->SetPortName(1, "org.qemu.guest_agent.0");

    vdagent_handler_ = std::make_unique<VDAgentHandler>();
    vdagent_handler_->SetSerialDevice(virtio_serial_.get(), 0);

    guest_agent_handler_ = std::make_unique<GuestAgentHandler>();
    guest_agent_handler_->SetSerialDevice(virtio_serial_.get(), 1);

    if (clipboard_port_) {
        vdagent_handler_->SetClipboardCallback([this](const ClipboardEvent& event) {
            clipboard_port_->OnClipboardEvent(event);
        });
    }

    virtio_serial_->SetDataCallback([this](uint32_t port_id, const uint8_t* data, size_t len) {
        if (vdagent_handler_ && port_id == 0) {
            vdagent_handler_->OnDataReceived(data, len);
        }
        if (guest_agent_handler_ && port_id == 1) {
            guest_agent_handler_->OnDataReceived(data, len);
        }
    });

    virtio_serial_->SetPortOpenCallback([this](uint32_t port_id, bool opened) {
        if (guest_agent_handler_ && port_id == 1) {
            guest_agent_handler_->OnPortOpen(opened);
        }
    });

    virtio_mmio_serial_ = std::make_unique<VirtioMmioDevice>();
    virtio_mmio_serial_->Init(virtio_serial_.get(), mem_);
    virtio_mmio_serial_->SetIrqCallback([this, irq = slot.irq]() { InjectIrq(irq); });
    virtio_mmio_serial_->SetIrqLevelCallback([this, irq = slot.irq](bool a) { SetIrqLevel(irq, a); });
    TryEnableIrqFd(virtio_mmio_serial_.get(), slot.irq);
    virtio_serial_->SetMmioDevice(virtio_mmio_serial_.get());
    addr_space_.AddMmioDevice(
        slot.mmio_base, VirtioMmioDevice::kMmioSize, virtio_mmio_serial_.get());
    active_virtio_slots_.push_back(slot);

    LOG_INFO("VirtIO Serial device initialized (vdagent + guest-agent)");
    return true;
}

bool Vm::SetupVirtioFs(const std::vector<VmSharedFolder>& initial_folders,
                       const VirtioDeviceSlot& slot) {
    virtio_fs_ = std::make_unique<VirtioFsDevice>("shared");

    virtio_mmio_fs_ = std::make_unique<VirtioMmioDevice>();
    virtio_mmio_fs_->Init(virtio_fs_.get(), mem_);
    virtio_mmio_fs_->SetIrqCallback([this, irq = slot.irq]() { InjectIrq(irq); });
    virtio_mmio_fs_->SetIrqLevelCallback([this, irq = slot.irq](bool a) { SetIrqLevel(irq, a); });
    TryEnableIrqFd(virtio_mmio_fs_.get(), slot.irq);
    virtio_fs_->SetMmioDevice(virtio_mmio_fs_.get());

    addr_space_.AddMmioDevice(slot.mmio_base, VirtioMmioDevice::kMmioSize, virtio_mmio_fs_.get());
    active_virtio_slots_.push_back(slot);

    for (const auto& folder : initial_folders) {
        if (!virtio_fs_->AddShare(folder.tag, folder.host_path, folder.readonly)) {
            LOG_WARN("Failed to add initial share: %s -> %s", folder.tag.c_str(), folder.host_path.c_str());
        }
    }

    LOG_INFO("VirtIO FS device initialized (mount tag: shared, %zu initial shares)", initial_folders.size());
    return true;
}

bool Vm::SetupVirtioSnd(const VirtioDeviceSlot& slot) {
    virtio_snd_ = std::make_unique<VirtioSndDevice>();
    virtio_snd_->SetMemMap(mem_);
    virtio_snd_->SetIoLoop(&io_loop_);

    if (audio_port_) {
        virtio_snd_->SetAudioPort(audio_port_);
    }

    virtio_mmio_snd_ = std::make_unique<VirtioMmioDevice>();
    virtio_mmio_snd_->Init(virtio_snd_.get(), mem_);
    virtio_mmio_snd_->SetIrqCallback([this, irq = slot.irq]() { InjectIrq(irq); });
    virtio_mmio_snd_->SetIrqLevelCallback([this, irq = slot.irq](bool a) { SetIrqLevel(irq, a); });
    TryEnableIrqFd(virtio_mmio_snd_.get(), slot.irq);
    virtio_snd_->SetMmioDevice(virtio_mmio_snd_.get());
    addr_space_.AddMmioDevice(
        slot.mmio_base, VirtioMmioDevice::kMmioSize, virtio_mmio_snd_.get());
    active_virtio_slots_.push_back(slot);

    LOG_INFO("VirtIO Sound device initialized (playback)");
    return true;
}

void Vm::VCpuThreadFunc(uint32_t vcpu_index) {
    // Phase 1: create vCPU on this thread (required by HVF; harmless on WHVP).
    auto created = hv_vm_->CreateVCpu(vcpu_index, &addr_space_);
    if (!created) {
        LOG_ERROR("vCPU %u: failed to create", vcpu_index);
        RequestStop();
        return;
    }
    vcpus_[vcpu_index] = std::move(created);

    // Perform any thread-local initialisation (e.g. LocalApic::SetCurrentCpu).
    vcpus_[vcpu_index]->OnThreadInit();

    // Install platform-specific callbacks (PSCI on ARM64; no-op elsewhere).
    SetupVCpuCallbacks(vcpu_index);

    // Signal the main thread that this vCPU is ready.
    vcpus_ready_.fetch_add(1);
    boot_cv_.notify_all();

    // Wait for FinalizeBoot (kernel load) to complete.
    {
        std::unique_lock<std::mutex> lock(boot_mutex_);
        boot_cv_.wait(lock, [this] { return boot_complete_ || !running_; });
    }
    if (!running_) return;

    // BSP (vCPU 0) sets its own boot registers on this thread, because HVF
    // requires hv_vcpu_set_reg to be called from the creating thread.
    if (vcpu_index == 0) {
        if (!machine_->SetupBootVCpu(vcpus_[0].get(), mem_.base)) {
            LOG_ERROR("Failed to set initial vCPU registers");
            RequestStop();
            return;
        }
    }

    // Phase 2: AP threads wait for their startup signal (BSP runs immediately).
    // Hypervisors that manage AP startup internally (e.g. WHVP) skip the wait.
    if (vcpu_index > 0 && vcpus_[vcpu_index]->NeedsStartupWait()) {
        auto& state = vcpu_startup_[vcpu_index];
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait(lock, [&] { return state->started || !running_; });
        if (!running_) return;
        vcpus_[vcpu_index]->OnStartup(*state);
    }

    auto& vcpu = vcpus_[vcpu_index];
    uint64_t exit_count = 0;

    while (running_) {
        auto action = vcpu->RunOnce();
        exit_count++;

        switch (action) {
        case VCpuExitAction::kContinue:
            break;

        case VCpuExitAction::kHalt:
            vcpu->WaitForInterrupt(100);
            break;

        case VCpuExitAction::kShutdown:
            LOG_INFO("vCPU %u: shutdown (after %" PRIu64 " exits)",
                     vcpu_index, exit_count);
            RequestStop();
            return;

        case VCpuExitAction::kError:
            LOG_ERROR("vCPU %u: error (after %" PRIu64 " exits)",
                      vcpu_index, exit_count);
            exit_code_.store(1);
            RequestStop();
            return;
        }
    }

    LOG_INFO("vCPU %u stopped (total exits: %" PRIu64 ")",
             vcpu_index, exit_count);
}

int Vm::Run() {
    running_ = true;
    LOG_INFO("Starting VM execution...");

    if (owned_console_input_ && console_port_) {
        console_input_thread_ = std::thread([this]() {
            uint8_t buf[64];
            while (running_.load()) {
                size_t n = console_port_->Read(buf, sizeof(buf));
                if (n > 0) {
                    InjectConsoleBytes(buf, n);
                }
            }
        });
    }

    // Phase 1: launch threads; each creates its vCPU then signals ready.
    for (uint32_t i = 0; i < cpu_count_; i++) {
        vcpu_threads_.emplace_back(&Vm::VCpuThreadFunc, this, i);
    }

    // Wait until all vCPUs are created (or a failure triggers RequestStop).
    {
        std::unique_lock<std::mutex> lock(boot_mutex_);
        boot_cv_.wait(lock, [this] {
            return vcpus_ready_.load() >= cpu_count_ || !running_;
        });
    }

    if (running_) {
        // Load the kernel, set APIC IDs in ACPI MADT, and configure BSP registers.
        FinalizeBoot(boot_config_);
    }

    if (running_) {
#if defined(__linux__) && defined(__aarch64__)
        // KVM_IRQFD on arm64 requires the in-kernel VGIC to have had its
        // KVM_DEV_ARM_VGIC_CTRL_INIT issued. SetupAarch64Boot normally drives
        // that, but it runs on the BSP thread after boot_complete_ — i.e.
        // after we would try to register irqfds here. Force-finalize from
        // this (main) thread; FinalizeVgicInit is idempotent.
        if (auto* kvm_vm = dynamic_cast<kvm::KvmVm*>(hv_vm_.get())) {
            kvm_vm->FinalizeVgicInit();
        }
#endif
        // Bring up the central device I/O loop, then register each virtio
        // slot's irqfd with it. Slots that fail to register stay on the
        // classic KVM_IRQ_LINE fallback.
        io_loop_.Start();
        InstallIrqFds();
    }

    // Phase 2: release all threads into their run loops.
    {
        std::lock_guard<std::mutex> lock(boot_mutex_);
        boot_complete_ = true;
    }
    boot_cv_.notify_all();

    for (auto& t : vcpu_threads_) {
        t.join();
    }

    return exit_code_.load();
}

void Vm::RequestStop() {
    running_ = false;
    // Wake threads blocked in the boot barrier.
    boot_cv_.notify_all();
    // Wake any vCPUs blocked in their per-CPU startup wait.
    for (auto& state : vcpu_startup_) {
        if (state) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->cv.notify_all();
        }
    }
    for (auto& vcpu : vcpus_) {
        if (vcpu) vcpu->CancelRun();
    }
}

void Vm::RequestReboot() {
    LOG_INFO("VM reboot requested");
    reboot_requested_ = true;
    RequestStop();
}

void Vm::TriggerPowerButton() {
    machine_->TriggerPowerButton();
}

void Vm::InjectConsoleBytes(const uint8_t* data, size_t size) {
    machine_->InjectConsoleInput(data, size);
}

void Vm::SetNetLinkUp(bool up) {
    if (virtio_net_) virtio_net_->SetLinkUp(up);
    if (net_backend_) net_backend_->SetLinkUp(up);
}

void Vm::UpdatePortForwards(const std::vector<PortForward>& forwards,
                             PortForwardCallback cb) {
    if (net_backend_) {
        net_backend_->UpdatePortForwards(forwards, std::move(cb));
    } else if (cb) {
        cb({});
    }
}

void Vm::UpdateGuestForwards(const std::vector<GuestForward>& guest_forwards) {
    if (net_backend_) {
        net_backend_->UpdateGuestForwards(guest_forwards);
    }
}

void Vm::InjectKeyEvent(uint32_t evdev_code, bool pressed) {
    if (virtio_kbd_) {
        virtio_kbd_->InjectEvent(EV_KEY, static_cast<uint16_t>(evdev_code),
                                 pressed ? 1 : 0, false);
        virtio_kbd_->InjectEvent(EV_SYN, SYN_REPORT, 0, true);
    }
}

void Vm::InjectPointerEvent(int32_t x, int32_t y, uint32_t buttons) {
    if (virtio_tablet_) {
        virtio_tablet_->InjectEvent(EV_ABS, ABS_X,
            static_cast<uint32_t>(x), false);
        virtio_tablet_->InjectEvent(EV_ABS, ABS_Y,
            static_cast<uint32_t>(y), false);
        if ((buttons & 1) != (inject_prev_buttons_ & 1))
            virtio_tablet_->InjectEvent(EV_KEY, BTN_LEFT,
                (buttons & 1) ? 1 : 0, false);
        if ((buttons & 2) != (inject_prev_buttons_ & 2))
            virtio_tablet_->InjectEvent(EV_KEY, BTN_RIGHT,
                (buttons & 2) ? 1 : 0, false);
        if ((buttons & 4) != (inject_prev_buttons_ & 4))
            virtio_tablet_->InjectEvent(EV_KEY, BTN_MIDDLE,
                (buttons & 4) ? 1 : 0, false);
        inject_prev_buttons_ = buttons;
        virtio_tablet_->InjectEvent(EV_SYN, SYN_REPORT, 0, true);
    }
}

void Vm::InjectWheelEvent(int32_t delta) {
    if (virtio_tablet_ && delta != 0) {
        virtio_tablet_->InjectEvent(EV_REL, REL_WHEEL,
            static_cast<uint32_t>(delta), false);
        virtio_tablet_->InjectEvent(EV_SYN, SYN_REPORT, 0, true);
    }
}

void Vm::SetDisplaySize(uint32_t width, uint32_t height) {
    if (virtio_gpu_) {
        virtio_gpu_->SetDisplaySize(width, height);
    }
}

void Vm::SendClipboardGrab(const std::vector<uint32_t>& types) {
    if (vdagent_handler_) {
        vdagent_handler_->SendClipboardGrab(
            VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD, types);
    }
}

void Vm::SendClipboardData(uint32_t type, const uint8_t* data, size_t len) {
    if (vdagent_handler_) {
        vdagent_handler_->SendClipboardData(
            VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD, type, data, len);
    }
}

void Vm::SendClipboardRequest(uint32_t type) {
    if (vdagent_handler_) {
        vdagent_handler_->SendClipboardRequest(
            VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD, type);
    }
}

void Vm::SendClipboardRelease() {
    if (vdagent_handler_) {
        vdagent_handler_->SendClipboardRelease(
            VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD);
    }
}

bool Vm::AddSharedFolder(const std::string& tag, const std::string& host_path, bool readonly) {
    if (!virtio_fs_) {
        LOG_ERROR("VirtIO FS device not initialized");
        return false;
    }
    return virtio_fs_->AddShare(tag, host_path, readonly);
}

bool Vm::RemoveSharedFolder(const std::string& tag) {
    if (!virtio_fs_) {
        LOG_ERROR("VirtIO FS device not initialized");
        return false;
    }
    return virtio_fs_->RemoveShare(tag);
}

std::vector<std::string> Vm::GetSharedFolderTags() const {
    if (!virtio_fs_) {
        return {};
    }
    return virtio_fs_->GetShareTags();
}

bool Vm::IsGuestAgentConnected() const {
    return guest_agent_handler_ && guest_agent_handler_->IsConnected();
}

void Vm::GuestAgentShutdown(const std::string& mode) {
    if (guest_agent_handler_) {
        guest_agent_handler_->Shutdown(mode);
    }
}

void Vm::GuestAgentSyncTime() {
    if (guest_agent_handler_) {
        guest_agent_handler_->SyncTime();
    }
}
