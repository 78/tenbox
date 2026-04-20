#include "core/arch/aarch64/aarch64_machine.h"
#include "core/arch/aarch64/fdt_builder.h"
#include "core/device/irq/gicv3.h"
#include "core/vmm/vm.h"
#include <cinttypes>
#include <cstring>
#include <cstdio>
#include <random>

#ifdef __APPLE__
#include "platform/macos/hypervisor/aarch64/hvf_vcpu.h"
#include "platform/macos/hypervisor/aarch64/hvf_vm.h"
#elif defined(__linux__) && defined(__aarch64__)
#include "platform/linux/hypervisor/aarch64/kvm_vcpu.h"
#include "platform/linux/hypervisor/aarch64/kvm_vm.h"
#endif

bool Aarch64Machine::SetupPlatformDevices(
    AddressSpace& addr_space,
    GuestMemMap& /*mem*/,
    HypervisorVm* hv_vm,
    std::shared_ptr<ConsolePort> console_port,
    VmIoLoop* io_loop,
    std::function<void()> shutdown_cb,
    std::function<void()> reboot_cb) {

    hv_vm_ = hv_vm;
    shutdown_cb_ = std::move(shutdown_cb);
    (void)reboot_cb;  // ARM64 PSCI reboot is handled differently

    uart_.SetIrqCallback([this]() {
        InjectIrq(hv_vm_, kUartIrq);
    });
    uart_.SetIrqLevelCallback([this](bool asserted) {
        SetIrqLevel(hv_vm_, kUartIrq, asserted);
    });
    // Thread the per-byte UART stream through a batcher so the downstream
    // ConsolePort sees larger chunks instead of N * 1-byte writes.
    tx_batcher_ = std::make_unique<ConsoleTxBatcher>(
        [console_port](const uint8_t* data, size_t size) {
            if (console_port) console_port->Write(data, size);
        });
    tx_batcher_->AttachIoLoop(io_loop);
    uart_.SetTxCallback([this](uint8_t byte) {
        tx_batcher_->Append(&byte, 1);
    });
    addr_space.AddMmioDevice(kUartBase, Pl011::kMmioSize, &uart_);

    // PL031 RTC
    rtc_.SetIrqLevelCallback([this](bool asserted) {
        SetIrqLevel(hv_vm_, kRtcIrq, asserted);
    });
    addr_space.AddMmioDevice(kRtcBase, Pl031Rtc::kMmioSize, &rtc_);

    // Register software GICv3 MMIO if HVF GIC is not available
#ifdef __APPLE__
    if (hv_vm) {
        auto* hvf = dynamic_cast<hvf::HvfVm*>(hv_vm);
        if (hvf && hvf->UsesSoftGic()) {
            hvf->GetSoftGic()->RegisterDevices(addr_space, kGicDistBase, kGicRedistBase);
            LOG_INFO("aarch64: software GICv3 MMIO registered at dist=0x%" PRIx64
                     " redist=0x%" PRIx64,
                     (uint64_t)kGicDistBase, (uint64_t)kGicRedistBase);
        }
    }
#endif

    return true;
}

bool Aarch64Machine::LoadKernel(
    const VmConfig& config,
    GuestMemMap& mem,
    const std::vector<VirtioDeviceSlot>& virtio_slots) {

    using namespace aarch64;

    // Load the ARM64 Image
    BootConfig boot_cfg;
    boot_cfg.kernel_path = config.kernel_path;
    boot_cfg.initrd_path = config.initrd_path;
    boot_cfg.cmdline = config.cmdline;
    boot_cfg.mem = mem;
    boot_cfg.cpu_count = config.cpu_count;

    kernel_entry_ = LoadLinuxImage(boot_cfg);
    if (kernel_entry_ == 0) return false;

    // Determine initrd location (after kernel, page-aligned)
    GPA initrd_start = 0;
    GPA initrd_end = 0;
    uint64_t initrd_size = 0;

    if (!config.initrd_path.empty()) {
        FILE* fp = fopen(config.initrd_path.c_str(), "rb");
        if (!fp) {
            LOG_ERROR("aarch64: cannot open initrd: %s", config.initrd_path.c_str());
            return false;
        }
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        if (sz <= 0) {
            LOG_ERROR("aarch64: initrd is empty");
            fclose(fp);
            return false;
        }
        initrd_size = static_cast<uint64_t>(sz);

        // Place initrd after kernel's reserved region, aligned to page boundary.
        // Must use image_size (not file size) — the kernel expands BSS/init in-place.
        uint64_t text_offset = kernel_entry_ - Layout::kRamBase;
        uint64_t kernel_reserved_end = text_offset;
        {
            FILE* kfp = fopen(config.kernel_path.c_str(), "rb");
            if (kfp) {
                LinuxImageHeader hdr{};
                if (fread(&hdr, sizeof(hdr), 1, kfp) == 1 && hdr.image_size > 0) {
                    kernel_reserved_end = text_offset + hdr.image_size;
                } else {
                    fseek(kfp, 0, SEEK_END);
                    kernel_reserved_end = text_offset + static_cast<uint64_t>(ftell(kfp));
                }
                fclose(kfp);
            }
        }

        uint64_t initrd_offset = AlignUp(kernel_reserved_end, kPageSize);
        if (initrd_offset + initrd_size > mem.alloc_size) {
            LOG_ERROR("aarch64: initrd doesn't fit in guest RAM");
            fclose(fp);
            return false;
        }

        uint8_t* dest = mem.base + initrd_offset;
        size_t read = fread(dest, 1, static_cast<size_t>(initrd_size), fp);
        fclose(fp);

        if (read != static_cast<size_t>(initrd_size)) {
            LOG_ERROR("aarch64: short initrd read (%zu / %" PRIu64 ")", read,
                      initrd_size);
            return false;
        }

        initrd_start = Layout::kRamBase + initrd_offset;
        initrd_end = initrd_start + initrd_size;

        LOG_INFO("aarch64: initrd loaded at GPA 0x%" PRIx64 " (%" PRIu64 " bytes)",
                 (uint64_t)initrd_start, initrd_size);
    }

    // Build FDT
    FdtBuilder fdt;

    // Pre-allocate GIC phandle so interrupt-parent can be set early
    uint32_t gic_phandle = fdt.AllocPhandle();

    fdt.BeginNode("");  // root node

    fdt.AddPropertyString("compatible", "linux,dummy-virt");
    fdt.AddPropertyU32("#address-cells", 2);
    fdt.AddPropertyU32("#size-cells", 2);
    fdt.AddPropertyU32("interrupt-parent", gic_phandle);

    // /chosen
    fdt.BeginNode("chosen");
    if (!config.cmdline.empty()) {
        fdt.AddPropertyString("bootargs", config.cmdline);
    }
    if (initrd_start) {
        fdt.AddPropertyU64("linux,initrd-start", initrd_start);
        fdt.AddPropertyU64("linux,initrd-end", initrd_end);
    }
    char stdout_path[64];
    snprintf(stdout_path, sizeof(stdout_path), "/pl011@%" PRIx64,
             (uint64_t)kUartBase);
    fdt.AddPropertyString("stdout-path", stdout_path);

    // Provide host entropy so the guest kernel can initialize CRNG
    // immediately at boot (requires random.trust_bootloader=on in cmdline).
    {
        uint8_t rng_seed[64];
        std::random_device rd;
        for (size_t i = 0; i < sizeof(rng_seed); i += 4) {
            uint32_t val = rd();
            std::memcpy(rng_seed + i, &val, 4);
        }
        fdt.AddPropertyBytes("rng-seed", rng_seed, sizeof(rng_seed));
    }

    fdt.EndNode();

    // /memory
    char mem_name[32];
    snprintf(mem_name, sizeof(mem_name), "memory@%" PRIx64,
             (uint64_t)Layout::kRamBase);
    fdt.BeginNode(mem_name);
    fdt.AddPropertyString("device_type", "memory");
    fdt.AddPropertyCells("reg", {
        static_cast<uint32_t>(Layout::kRamBase >> 32),
        static_cast<uint32_t>(Layout::kRamBase & 0xFFFFFFFF),
        static_cast<uint32_t>(mem.alloc_size >> 32),
        static_cast<uint32_t>(mem.alloc_size & 0xFFFFFFFF)
    });
    fdt.EndNode();

    // /cpus
    fdt.BeginNode("cpus");
    fdt.AddPropertyU32("#address-cells", 1);
    fdt.AddPropertyU32("#size-cells", 0);
    for (uint32_t i = 0; i < config.cpu_count; i++) {
        char cpu_name[32];
        snprintf(cpu_name, sizeof(cpu_name), "cpu@%u", i);
        fdt.BeginNode(cpu_name);
        fdt.AddPropertyString("device_type", "cpu");
        fdt.AddPropertyString("compatible", "arm,arm-v8");
        fdt.AddPropertyU32("reg", i);
        // PSCI is always available (in-kernel PSCI on KVM, userspace
        // emulation on HVF) so every CPU — including a single-core config —
        // uses "psci" as its enable-method. This also lets the guest use
        // PSCI SYSTEM_OFF / SYSTEM_RESET for shutdown/reboot.
        fdt.AddPropertyString("enable-method", "psci");
        fdt.EndNode();
    }
    fdt.EndNode();

    // PSCI node (always present so the guest can issue SYSTEM_OFF /
    // SYSTEM_RESET, even in single-CPU configurations).
    fdt.BeginNode("psci");
    fdt.AddPropertyString("compatible", "arm,psci-1.0");
    fdt.AddPropertyString("method", "hvc");
    fdt.EndNode();

    // /hypervisor node — ARM64 has no CPUID equivalent, so the paravirt
    // handshake happens through this FDT node + the SMCCC vendor-hyp HVC.
    // Only emit it under KVM: "linux,kvm" tells the guest to enable kvm_ptp,
    // lets systemd-detect-virt report "kvm", and gates drivers that probe
    // for the KVM SMCCC vendor UID. Under HVF there is no matching vendor
    // hyp UID, so claiming "linux,kvm" would either mislead systemd or
    // trigger failed SMCCC probes in kvm_ptp_arm; better to omit the node
    // entirely and let the guest see itself as a generic arm64 platform.
#if defined(__linux__) && defined(__aarch64__)
    if (dynamic_cast<kvm::KvmVm*>(hv_vm_)) {
        fdt.BeginNode("hypervisor");
        fdt.AddPropertyString("compatible", "linux,kvm");
        fdt.EndNode();
    }
#endif

    // /timer (ARM generic timer)
    fdt.BeginNode("timer");
    fdt.AddPropertyString("compatible", "arm,armv8-timer");
    // PPI interrupts: secure phys, non-secure phys, virt, hyp
    // Type 1 = PPI, IRQ_TYPE_LEVEL_HI = 4 (active-high level-sensitive)
    fdt.AddPropertyCells("interrupts", {
        1, 13, 4,   // secure physical timer   (INTID 29)
        1, 14, 4,   // non-secure physical timer (INTID 30)
        1, 11, 4,   // virtual timer            (INTID 27)
        1, 10, 4,   // hypervisor timer         (INTID 26)
    });
    fdt.AddPropertyEmpty("always-on");
    fdt.EndNode();

    // /intc — GICv3 by default, with a GICv2 fallback for hosts where the
    // in-kernel VGICv3 is unavailable (e.g. Raspberry Pi 5 with GIC-400).
    GPA actual_redist_base = kGicRedistBase;
    uint32_t redist_total_size = static_cast<uint32_t>(config.cpu_count * 0x20000);
    bool use_gic_v2 = false;
    GPA gic_v2_cpu_base = 0x08010000ULL;
    uint32_t gic_v2_cpu_size = 0x10000;
#ifdef __APPLE__
    if (hv_vm_) {
        auto* hvf = dynamic_cast<hvf::HvfVm*>(hv_vm_);
        if (hvf) {
            actual_redist_base = hvf->GetRedistBase();
            redist_total_size = static_cast<uint32_t>(hvf->GetRedistSizePerCpu()) * config.cpu_count;
        }
    }
#elif defined(__linux__) && defined(__aarch64__)
    if (hv_vm_) {
        auto* kvm_vm = dynamic_cast<kvm::KvmVm*>(hv_vm_);
        if (kvm_vm && kvm_vm->UsesGicV2()) {
            use_gic_v2 = true;
            gic_v2_cpu_base = kvm::KvmVm::kGicV2CpuBase;
            gic_v2_cpu_size = static_cast<uint32_t>(kvm::KvmVm::kGicV2CpuSize);
        }
    }
#endif

    char gic_name[64];
    snprintf(gic_name, sizeof(gic_name), "intc@%" PRIx64,
             (uint64_t)kGicDistBase);
    fdt.BeginNode(gic_name);
    if (use_gic_v2) {
        fdt.AddPropertyString("compatible", "arm,cortex-a15-gic");
    } else {
        fdt.AddPropertyString("compatible", "arm,gic-v3");
    }
    fdt.AddPropertyU32("#interrupt-cells", 3);
    fdt.AddPropertyEmpty("interrupt-controller");
    fdt.AddPropertyU32("phandle", gic_phandle);
    if (use_gic_v2) {
        fdt.AddPropertyCells("reg", {
            static_cast<uint32_t>(kGicDistBase >> 32),
            static_cast<uint32_t>(kGicDistBase & 0xFFFFFFFF),
            0, 0x10000,    // Distributor: 64 KiB (v2 only uses first 4 KiB)
            static_cast<uint32_t>(gic_v2_cpu_base >> 32),
            static_cast<uint32_t>(gic_v2_cpu_base & 0xFFFFFFFF),
            0, gic_v2_cpu_size,  // CPU interface (GICC)
        });
    } else {
        fdt.AddPropertyCells("reg", {
            static_cast<uint32_t>(kGicDistBase >> 32),
            static_cast<uint32_t>(kGicDistBase & 0xFFFFFFFF),
            0, 0x10000,    // Distributor: 64 KiB
            static_cast<uint32_t>(actual_redist_base >> 32),
            static_cast<uint32_t>(actual_redist_base & 0xFFFFFFFF),
            0, redist_total_size,
        });
    }
    fdt.EndNode();

    // Fixed clock for AMBA peripherals (PL011 requires clocks property)
    uint32_t apb_pclk_phandle = fdt.AllocPhandle();
    fdt.BeginNode("apb-pclk");
    fdt.AddPropertyString("compatible", "fixed-clock");
    fdt.AddPropertyU32("#clock-cells", 0);
    fdt.AddPropertyU32("clock-frequency", 24000000);
    fdt.AddPropertyString("clock-output-names", "clk24mhz");
    fdt.AddPropertyU32("phandle", apb_pclk_phandle);
    fdt.EndNode();

    // /pl011 UART (AMBA PL011 — needs arm,primecell compat and clocks)
    char uart_name[64];
    snprintf(uart_name, sizeof(uart_name), "pl011@%" PRIx64,
             (uint64_t)kUartBase);
    fdt.BeginNode(uart_name);
    {
        const char* compat[] = {"arm,pl011", "arm,primecell"};
        std::string compat_str;
        compat_str.append(compat[0], strlen(compat[0]) + 1);
        compat_str.append(compat[1], strlen(compat[1]) + 1);
        fdt.AddPropertyBytes("compatible",
            reinterpret_cast<const uint8_t*>(compat_str.data()),
            compat_str.size());
    }
    fdt.AddPropertyCells("reg", {
        static_cast<uint32_t>(kUartBase >> 32),
        static_cast<uint32_t>(kUartBase & 0xFFFFFFFF),
        0, static_cast<uint32_t>(Pl011::kMmioSize)
    });
    fdt.AddPropertyCells("interrupts", {0, kUartIrq, 4});
    fdt.AddPropertyCells("clocks", {apb_pclk_phandle, apb_pclk_phandle});
    {
        const char cn[] = "uartclk\0apb_pclk";
        fdt.AddPropertyBytes("clock-names",
            reinterpret_cast<const uint8_t*>(cn), sizeof(cn));
    }
    fdt.EndNode();

    // PL031 RTC (arm,pl031 — PrimeCell, needs clocks like PL011)
    {
        char rtc_name[64];
        snprintf(rtc_name, sizeof(rtc_name), "pl031@%" PRIx64,
                 (uint64_t)kRtcBase);
        fdt.BeginNode(rtc_name);
        {
            const char* compat[] = {"arm,pl031", "arm,primecell"};
            std::string compat_str;
            compat_str.append(compat[0], strlen(compat[0]) + 1);
            compat_str.append(compat[1], strlen(compat[1]) + 1);
            fdt.AddPropertyBytes("compatible",
                reinterpret_cast<const uint8_t*>(compat_str.data()),
                compat_str.size());
        }
        fdt.AddPropertyCells("reg", {
            static_cast<uint32_t>(kRtcBase >> 32),
            static_cast<uint32_t>(kRtcBase & 0xFFFFFFFF),
            0, static_cast<uint32_t>(Pl031Rtc::kMmioSize)
        });
        fdt.AddPropertyCells("interrupts", {0, kRtcIrq, 4});
        fdt.AddPropertyCells("clocks", {apb_pclk_phandle});
        fdt.AddPropertyBytes("clock-names",
            reinterpret_cast<const uint8_t*>("apb_pclk"), 9);
        fdt.EndNode();
    }

    // VirtIO MMIO devices
    for (size_t i = 0; i < virtio_slots.size(); i++) {
        const auto& slot = virtio_slots[i];
        char name[64];
        snprintf(name, sizeof(name), "virtio_mmio@%" PRIx64,
                 (uint64_t)slot.mmio_base);
        fdt.BeginNode(name);
        fdt.AddPropertyString("compatible", "virtio,mmio");
        fdt.AddPropertyCells("reg", {
            static_cast<uint32_t>(slot.mmio_base >> 32),
            static_cast<uint32_t>(slot.mmio_base & 0xFFFFFFFF),
            0, 0x200
        });
        // SPI interrupt (type=0), irq number, level-triggered active-high (4)
        fdt.AddPropertyCells("interrupts", {0, static_cast<uint32_t>(slot.irq), 4});
        fdt.AddPropertyEmpty("dma-coherent");
        fdt.EndNode();
    }

    fdt.EndNode();  // root node

    auto dtb = fdt.Finish();

    // Place FDT at the start of guest RAM
    fdt_gpa_ = Layout::kFdtBase;
    if (dtb.size() > Layout::kFdtMaxSize) {
        LOG_ERROR("aarch64: FDT too large (%zu bytes, max %" PRIu64 ")",
                  dtb.size(), (uint64_t)Layout::kFdtMaxSize);
        return false;
    }

    memcpy(mem.base, dtb.data(), dtb.size());
    LOG_INFO("aarch64: FDT placed at GPA 0x%" PRIx64 " (%zu bytes)",
             (uint64_t)fdt_gpa_, dtb.size());

    return true;
}

bool Aarch64Machine::SetupBootVCpu(HypervisorVCpu* vcpu, uint8_t* /*ram*/) {
#ifdef __APPLE__
    // Use the HVF-specific method to set ARM64 boot registers.
    // We include the HVF header to access the concrete type.
    auto* hvf_vcpu = dynamic_cast<hvf::HvfVCpu*>(vcpu);
    if (!hvf_vcpu) {
        LOG_ERROR("aarch64: SetupBootVCpu requires HvfVCpu");
        return false;
    }
    return hvf_vcpu->SetupAarch64Boot(kernel_entry_, fdt_gpa_);
#elif defined(__linux__) && defined(__aarch64__)
    auto* kvm_vcpu = dynamic_cast<kvm::KvmVCpu*>(vcpu);
    if (!kvm_vcpu) {
        LOG_ERROR("aarch64: SetupBootVCpu requires KvmVCpu on Linux");
        return false;
    }
    return kvm_vcpu->SetupAarch64Boot(kernel_entry_, fdt_gpa_);
#else
    (void)vcpu;
    LOG_ERROR("aarch64: SetupBootVCpu called on unsupported platform");
    return false;
#endif
}

void Aarch64Machine::InjectIrq(HypervisorVm* hv_vm, uint8_t irq) {
    SetIrqLevel(hv_vm, irq, true);
}

void Aarch64Machine::SetIrqLevel(HypervisorVm* hv_vm, uint8_t irq, bool asserted) {
    // hv_gic_set_spi takes the absolute SPI INTID (starting at 32).
    InterruptRequest req{};
    req.vector = static_cast<uint32_t>(irq) + 32;
    req.level_triggered = asserted;
    hv_vm->RequestInterrupt(req);
}

void Aarch64Machine::InjectConsoleInput(const uint8_t* data, size_t size) {
    if (!data || size == 0) return;
    for (size_t i = 0; i < size; ++i) {
        uart_.PushInput(data[i]);
    }
    uart_.CheckAndRaiseIrq();
}

void Aarch64Machine::TriggerPowerButton() {
    // ARM64 doesn't have ACPI power button in the same way.
    // Use PSCI SYSTEM_OFF or just call shutdown callback directly.
    if (shutdown_cb_) {
        shutdown_cb_();
    }
}

std::vector<VirtioDeviceSlot> Aarch64Machine::GetVirtioSlots() const {
    // 8 VirtIO MMIO slots at 0x0A000000 + i*0x200, IRQ = kVirtioBaseIrq + i
    std::vector<VirtioDeviceSlot> slots;
    for (int i = 0; i < 8; i++) {
        slots.push_back({
            kVirtioMmioBase + static_cast<uint64_t>(i) * kVirtioStride,
            static_cast<uint8_t>(kVirtioBaseIrq + i)
        });
    }
    return slots;
}
