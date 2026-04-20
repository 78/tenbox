#include "platform/linux/hypervisor/aarch64/kvm_vm.h"
#include "platform/linux/hypervisor/aarch64/kvm_vcpu.h"
#include "platform/linux/hypervisor/kvm_platform.h"

#include <cerrno>
#include <cstring>
#include <vector>
#include <linux/kvm.h>
#include <asm/kvm.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace kvm {

KvmVm::~KvmVm() {
    if (vgic_fd_ >= 0) {
        ::close(vgic_fd_);
        vgic_fd_ = -1;
    }
    if (vm_fd_ >= 0) {
        ::close(vm_fd_);
        vm_fd_ = -1;
    }
    // kvm_fd_ is owned by kvm_platform.cpp; do not close.
}

std::unique_ptr<KvmVm> KvmVm::Create(uint32_t cpu_count) {
    auto vm = std::unique_ptr<KvmVm>(new KvmVm());
    vm->cpu_count_ = cpu_count;

    vm->kvm_fd_ = GetKvmFd();
    if (vm->kvm_fd_ < 0) {
        LOG_ERROR("kvm: /dev/kvm not available");
        return nullptr;
    }

    int vcpu_mmap_size = ::ioctl(vm->kvm_fd_, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (vcpu_mmap_size < (int)sizeof(struct kvm_run)) {
        LOG_ERROR("kvm: KVM_GET_VCPU_MMAP_SIZE failed (%d): %s",
                  vcpu_mmap_size, strerror(errno));
        return nullptr;
    }
    vm->vcpu_mmap_size_ = static_cast<size_t>(vcpu_mmap_size);

    // KVM_CREATE_VM takes an IPA size (in bits) on arm64. 0 means "use the
    // KVM default" (40 bits on most hosts).
    vm->vm_fd_ = ::ioctl(vm->kvm_fd_, KVM_CREATE_VM, 0UL);
    if (vm->vm_fd_ < 0) {
        LOG_ERROR("kvm: KVM_CREATE_VM failed: %s", strerror(errno));
        return nullptr;
    }

    if (!vm->CreateInKernelVgic()) {
        return nullptr;
    }

    LOG_INFO("kvm: aarch64 VM created (%u vCPUs, mmap_size=%zu, vgic=%s)",
             cpu_count, vm->vcpu_mmap_size_,
             vm->uses_gic_v2_ ? "v2" : "v3");
    return vm;
}

bool KvmVm::CreateInKernelVgic() {
    // Prefer GICv3 (matches our default FDT + arm64 ABI). On hosts whose
    // kernel cannot emulate VGICv3 over a physical GICv2 (e.g. Raspberry Pi
    // 5's GIC-400 under certain kernel configs), KVM_CREATE_DEVICE returns
    // ENODEV: fall back to VGICv2.
    if (TryCreateVgicV3()) {
        uses_gic_v2_ = false;
        return true;
    }
    LOG_WARN("kvm: VGICv3 unavailable, falling back to VGICv2");
    if (TryCreateVgicV2()) {
        uses_gic_v2_ = true;
        return true;
    }
    LOG_ERROR("kvm: neither VGICv3 nor VGICv2 could be created");
    return false;
}

bool KvmVm::TryCreateVgicV3() {
    struct kvm_create_device cd{};
    cd.type = KVM_DEV_TYPE_ARM_VGIC_V3;
    cd.fd = 0;
    cd.flags = 0;
    if (::ioctl(vm_fd_, KVM_CREATE_DEVICE, &cd) < 0) {
        LOG_INFO("kvm: KVM_CREATE_DEVICE(VGIC_V3) unavailable: %s",
                 strerror(errno));
        return false;
    }
    vgic_fd_ = static_cast<int>(cd.fd);

    auto SetAddr = [this](uint64_t attr, uint64_t addr) -> bool {
        struct kvm_device_attr da{};
        da.group = KVM_DEV_ARM_VGIC_GRP_ADDR;
        da.attr = attr;
        da.addr = reinterpret_cast<uint64_t>(&addr);
        if (::ioctl(vgic_fd_, KVM_SET_DEVICE_ATTR, &da) < 0) {
            LOG_ERROR("kvm: VGIC_V3 SET_ADDR(attr=%" PRIu64 ") failed: %s",
                      attr, strerror(errno));
            return false;
        }
        return true;
    };

    if (!SetAddr(KVM_VGIC_V3_ADDR_TYPE_DIST, kGicDistBase)) return false;
    if (!SetAddr(KVM_VGIC_V3_ADDR_TYPE_REDIST, kGicRedistBase)) return false;
    return true;
}

bool KvmVm::TryCreateVgicV2() {
    struct kvm_create_device cd{};
    cd.type = KVM_DEV_TYPE_ARM_VGIC_V2;
    cd.fd = 0;
    cd.flags = 0;
    if (::ioctl(vm_fd_, KVM_CREATE_DEVICE, &cd) < 0) {
        LOG_ERROR("kvm: KVM_CREATE_DEVICE(VGIC_V2) failed: %s", strerror(errno));
        return false;
    }
    vgic_fd_ = static_cast<int>(cd.fd);

    auto SetAddr = [this](uint64_t attr, uint64_t addr) -> bool {
        struct kvm_device_attr da{};
        da.group = KVM_DEV_ARM_VGIC_GRP_ADDR;
        da.attr = attr;
        da.addr = reinterpret_cast<uint64_t>(&addr);
        if (::ioctl(vgic_fd_, KVM_SET_DEVICE_ATTR, &da) < 0) {
            LOG_ERROR("kvm: VGIC_V2 SET_ADDR(attr=%" PRIu64 ") failed: %s",
                      attr, strerror(errno));
            return false;
        }
        return true;
    };

    // GICv2 needs DIST and CPU interface addresses. We reuse the same 64 KiB
    // distributor slot as v3 and place the virtual CPU interface at
    // 0x08010000 (inside the space that v3 would use for redistributors).
    if (!SetAddr(KVM_VGIC_V2_ADDR_TYPE_DIST, kGicDistBase)) return false;
    if (!SetAddr(KVM_VGIC_V2_ADDR_TYPE_CPU, kGicV2CpuBase)) return false;
    return true;
}

bool KvmVm::FinalizeVgicInit() {
    std::lock_guard<std::mutex> lock(vgic_init_mutex_);
    if (vgic_initialized_) return true;
    if (vgic_fd_ < 0) {
        LOG_ERROR("kvm: FinalizeVgicInit called without a VGIC device");
        return false;
    }

    struct kvm_device_attr da{};
    da.group = KVM_DEV_ARM_VGIC_GRP_CTRL;
    da.attr = KVM_DEV_ARM_VGIC_CTRL_INIT;
    if (::ioctl(vgic_fd_, KVM_SET_DEVICE_ATTR, &da) < 0) {
        LOG_ERROR("kvm: VGIC CTRL_INIT failed: %s", strerror(errno));
        return false;
    }
    vgic_initialized_ = true;
    LOG_INFO("kvm: in-kernel VGIC%s initialized", uses_gic_v2_ ? "v2" : "v3");
    return true;
}

bool KvmVm::MapMemory(GPA gpa, void* hva, uint64_t size, bool writable) {
    uint32_t slot;
    {
        std::lock_guard<std::mutex> lock(slot_mutex_);
        slot = next_slot_++;
    }

    struct kvm_userspace_memory_region region{};
    region.slot = slot;
    region.flags = writable ? 0 : KVM_MEM_READONLY;
    region.guest_phys_addr = gpa;
    region.memory_size = size;
    region.userspace_addr = reinterpret_cast<uint64_t>(hva);

    if (::ioctl(vm_fd_, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        LOG_ERROR("kvm: KVM_SET_USER_MEMORY_REGION(slot=%u gpa=0x%" PRIx64
                  " size=0x%" PRIx64 ") failed: %s",
                  slot, gpa, size, strerror(errno));
        return false;
    }

    LOG_INFO("kvm: mapped slot=%u GPA=0x%" PRIx64 " size=0x%" PRIx64 " HVA=%p%s",
             slot, gpa, size, hva, writable ? "" : " [RO]");
    return true;
}

bool KvmVm::UnmapMemory(GPA /*gpa*/, uint64_t /*size*/) {
    // Not exercised by the current VM lifecycle (RAM is torn down with the
    // process). Implementing this cleanly requires tracking slot IDs per GPA.
    LOG_WARN("kvm: UnmapMemory not implemented");
    return false;
}

std::unique_ptr<HypervisorVCpu> KvmVm::CreateVCpu(
    uint32_t index, AddressSpace* addr_space) {
    return KvmVCpu::Create(*this, index, addr_space);
}

void KvmVm::RequestInterrupt(const InterruptRequest& req) {
    // Aarch64Machine::SetIrqLevel encodes SPIs as (hw_irq + 32), i.e. the
    // architectural GIC INTID. KVM's in-kernel VGIC expects the absolute
    // INTID (32..1019) as irq_id in the (type|vcpu|num) encoding, so we pass
    // it through unchanged. SGIs/PPIs do not flow through RequestInterrupt
    // in our codebase.
    if (req.vector < 32 || req.vector > 1019) {
        LOG_WARN("kvm: RequestInterrupt for out-of-range SPI vector %u ignored",
                 req.vector);
        return;
    }
    uint32_t encoded = (static_cast<uint32_t>(KVM_ARM_IRQ_TYPE_SPI) << 24) |
                       (0u << 16) |
                       (req.vector & 0xffffu);

    struct kvm_irq_level il{};
    il.irq = encoded;
    il.level = req.level_triggered ? 1 : 0;
    if (::ioctl(vm_fd_, KVM_IRQ_LINE, &il) < 0) {
        LOG_WARN("kvm: KVM_IRQ_LINE(intid=%u level=%d) failed: %s",
                 req.vector, (int)req.level_triggered, strerror(errno));
    }
}

bool KvmVm::AssertIrq(uint32_t gsi, bool level) {
    // gsi is the absolute architectural INTID (>= 32 for SPIs).
    if (gsi < 32 || gsi > 1019) return false;
    uint32_t encoded = (static_cast<uint32_t>(KVM_ARM_IRQ_TYPE_SPI) << 24) |
                       (gsi & 0xffffu);

    struct kvm_irq_level il{};
    il.irq = encoded;
    il.level = level ? 1 : 0;
    if (::ioctl(vm_fd_, KVM_IRQ_LINE, &il) < 0) {
        LOG_WARN("kvm: AssertIrq KVM_IRQ_LINE(intid=%u level=%d) failed: %s",
                 gsi, (int)level, strerror(errno));
    }
    return true;
}

bool KvmVm::UpdateIrqRoutingLocked() {
    // arm64 KVM has NO default GSI routing — we must install one entry per
    // SPI we want to drive through KVM_IRQFD (or KVM_IRQ_LINE with routing).
    // Build a routing table from routed_gsis_ and send it wholesale.
    size_t n = routed_gsis_.size();
    std::vector<uint8_t> buf(
        sizeof(struct kvm_irq_routing) +
        n * sizeof(struct kvm_irq_routing_entry), 0);
    auto* routing = reinterpret_cast<struct kvm_irq_routing*>(buf.data());
    routing->nr = static_cast<uint32_t>(n);

    auto* entries = reinterpret_cast<struct kvm_irq_routing_entry*>(
        buf.data() + sizeof(struct kvm_irq_routing));
    size_t i = 0;
    for (uint32_t gsi : routed_gsis_) {
        entries[i].gsi = gsi;
        entries[i].type = KVM_IRQ_ROUTING_IRQCHIP;
        entries[i].u.irqchip.irqchip = 0;         // only VGIC
        entries[i].u.irqchip.pin = gsi - 32;      // SPI pin is INTID - 32
        ++i;
    }

    if (::ioctl(vm_fd_, KVM_SET_GSI_ROUTING, routing) < 0) {
        LOG_WARN("kvm: KVM_SET_GSI_ROUTING(n=%zu) failed: %s",
                 n, strerror(errno));
        return false;
    }
    return true;
}

bool KvmVm::RegisterLevelIrqFd(uint32_t gsi, int trigger_fd, int resample_fd) {
    // arm64 GSI for KVM_IRQFD is the absolute SPI INTID (>= 32). We must
    // explicitly install a KVM_IRQ_ROUTING_IRQCHIP entry mapping gsi -> SPI
    // pin before KVM_IRQFD; otherwise the kernel happily accepts the irqfd
    // but never delivers the interrupt (no route found).
    if (gsi < 32 || gsi > 1019 || trigger_fd < 0) return false;

    {
        std::lock_guard<std::mutex> lock(irqfd_route_mutex_);
        routed_gsis_.insert(gsi);
        if (!UpdateIrqRoutingLocked()) {
            routed_gsis_.erase(gsi);
            return false;
        }
    }

    struct kvm_irqfd ifd{};
    ifd.fd = static_cast<uint32_t>(trigger_fd);
    ifd.gsi = gsi;
    if (resample_fd >= 0) {
        ifd.flags = KVM_IRQFD_FLAG_RESAMPLE;
        ifd.resamplefd = static_cast<uint32_t>(resample_fd);
    }
    if (::ioctl(vm_fd_, KVM_IRQFD, &ifd) < 0) {
        LOG_WARN("kvm: KVM_IRQFD(gsi=%u trigger=%d resample=%d) failed: %s",
                 gsi, trigger_fd, resample_fd, strerror(errno));
        std::lock_guard<std::mutex> lock(irqfd_route_mutex_);
        routed_gsis_.erase(gsi);
        UpdateIrqRoutingLocked();
        return false;
    }
    LOG_INFO("kvm: irqfd registered gsi=%u trigger=%d resample=%d",
             gsi, trigger_fd, resample_fd);
    return true;
}

bool KvmVm::UnregisterIrqFd(uint32_t gsi, int trigger_fd) {
    if (trigger_fd < 0) return false;

    struct kvm_irqfd ifd{};
    ifd.fd = static_cast<uint32_t>(trigger_fd);
    ifd.gsi = gsi;
    ifd.flags = KVM_IRQFD_FLAG_DEASSIGN;
    bool ok = (::ioctl(vm_fd_, KVM_IRQFD, &ifd) == 0);
    if (!ok) {
        LOG_WARN("kvm: KVM_IRQFD DEASSIGN(gsi=%u trigger=%d) failed: %s",
                 gsi, trigger_fd, strerror(errno));
    }

    std::lock_guard<std::mutex> lock(irqfd_route_mutex_);
    routed_gsis_.erase(gsi);
    UpdateIrqRoutingLocked();  // best-effort; ignore result on teardown
    return ok;
}

} // namespace kvm
