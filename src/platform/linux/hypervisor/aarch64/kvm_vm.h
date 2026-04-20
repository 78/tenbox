#pragma once

#include "core/vmm/hypervisor_vm.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>

namespace kvm {

class KvmVCpu;

// ARM64 KVM VM backend.
// - Uses in-kernel VGICv3 (created via KVM_CREATE_DEVICE) with the
//   dist/redist layout expected by the generic Aarch64Machine:
//       GICD at 0x08000000 (64 KiB)
//       GICR at 0x080A0000 (2 * 64 KiB per vCPU)
// - Relies on in-kernel PSCI v0.2 for SYSTEM_OFF/RESET and secondary CPU
//   startup; no userspace PSCI dispatch is needed.
class KvmVm final : public HypervisorVm {
public:
    ~KvmVm() override;

    static std::unique_ptr<KvmVm> Create(uint32_t cpu_count);

    bool MapMemory(GPA gpa, void* hva, uint64_t size, bool writable) override;
    bool UnmapMemory(GPA gpa, uint64_t size) override;

    std::unique_ptr<HypervisorVCpu> CreateVCpu(
        uint32_t index, AddressSpace* addr_space) override;

    void RequestInterrupt(const InterruptRequest& req) override;

    // KVM has an in-kernel VGIC: SPI IRQ lines go through KVM_IRQ_LINE.
    bool AssertIrq(uint32_t gsi, bool level) override;

    // Register / unregister a KVM_IRQFD for a level-triggered SPI.
    // gsi is the absolute INTID (>= 32). resample_fd may be -1 to fall
    // back to edge semantics, but virtio-mmio requires a resample fd.
    bool RegisterLevelIrqFd(uint32_t gsi, int trigger_fd, int resample_fd) override;
    bool UnregisterIrqFd(uint32_t gsi, int trigger_fd) override;

    void SetGuestMemMap(const GuestMemMap* mem) override { guest_mem_ = mem; }

    // Issue KVM_DEV_ARM_VGIC_CTRL_INIT on the in-kernel VGIC. Must be called
    // exactly once, after *all* vCPUs have been created via KVM_CREATE_VCPU
    // (KVM rejects INIT otherwise). Safe to call multiple times: no-op after
    // the first success.
    bool FinalizeVgicInit();

    int VmFd() const { return vm_fd_; }
    int KvmFd() const { return kvm_fd_; }
    size_t VcpuMmapSize() const { return vcpu_mmap_size_; }
    uint32_t CpuCount() const { return cpu_count_; }

    // True when the fallback VGICv2 path was used (host GIC is v2 and the
    // kernel doesn't emulate v3 on top of it — common on Raspberry Pi 5 with
    // GIC-400). The machine model needs this to pick the right FDT compat.
    bool UsesGicV2() const { return uses_gic_v2_; }

    // GIC layout (shared between v2 and v3 wherever possible).
    static constexpr uint64_t kGicDistBase    = 0x08000000ULL;
    static constexpr uint64_t kGicDistSize    = 0x00010000ULL;  // 64 KiB
    // GICv3: redistributor region (2 * 64 KiB per vCPU).
    static constexpr uint64_t kGicRedistBase  = 0x080A0000ULL;
    static constexpr uint64_t kGicRedistStride = 0x00020000ULL;
    // GICv2 CPU interface (placed inside the unused redist slot).
    static constexpr uint64_t kGicV2CpuBase   = 0x08010000ULL;
    static constexpr uint64_t kGicV2CpuSize   = 0x00010000ULL;

private:
    KvmVm() = default;

    bool CreateInKernelVgic();
    bool TryCreateVgicV3();
    bool TryCreateVgicV2();

    bool UpdateIrqRoutingLocked();

    int kvm_fd_ = -1;
    int vm_fd_ = -1;
    int vgic_fd_ = -1;
    bool vgic_initialized_ = false;
    bool uses_gic_v2_ = false;
    uint32_t cpu_count_ = 0;
    size_t vcpu_mmap_size_ = 0;
    std::mutex vgic_init_mutex_;

    // GSIs (absolute SPI INTIDs) with an active irqfd. KVM requires us to
    // program explicit GSI routing on arm64 — there is no default routing
    // installed by VGIC creation. We rewrite the full route table every time
    // a slot is added/removed under irqfd_route_mutex_.
    std::mutex irqfd_route_mutex_;
    std::set<uint32_t> routed_gsis_;

    const GuestMemMap* guest_mem_ = nullptr;

    std::mutex slot_mutex_;
    uint32_t next_slot_ = 0;
};

} // namespace kvm
