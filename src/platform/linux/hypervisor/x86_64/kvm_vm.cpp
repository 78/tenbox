#include "platform/linux/hypervisor/x86_64/kvm_vm.h"
#include "platform/linux/hypervisor/x86_64/kvm_vcpu.h"
#include "platform/linux/hypervisor/kvm_platform.h"

#include <cerrno>
#include <cstring>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace kvm {

// KVM requires us to pick fixed GPAs for the TSS region and the identity
// page table on Intel. These are the standard QEMU values and live inside
// the 0xFE000000+ reserved window (above 4 GiB mapped RAM gap).
static constexpr uint64_t kTssAddr          = 0xfffbd000ULL;
static constexpr uint64_t kIdentityMapAddr  = 0xfeffc000ULL;

KvmVm::~KvmVm() {
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

    vm->vm_fd_ = ::ioctl(vm->kvm_fd_, KVM_CREATE_VM, 0);
    if (vm->vm_fd_ < 0) {
        LOG_ERROR("kvm: KVM_CREATE_VM failed: %s", strerror(errno));
        return nullptr;
    }

    if (::ioctl(vm->vm_fd_, KVM_SET_TSS_ADDR, (unsigned long)kTssAddr) < 0) {
        LOG_WARN("kvm: KVM_SET_TSS_ADDR failed: %s (non-fatal)", strerror(errno));
    }

    uint64_t idmap = kIdentityMapAddr;
    if (::ioctl(vm->vm_fd_, KVM_SET_IDENTITY_MAP_ADDR, &idmap) < 0) {
        LOG_WARN("kvm: KVM_SET_IDENTITY_MAP_ADDR failed: %s (non-fatal)",
                 strerror(errno));
    }

    if (::ioctl(vm->vm_fd_, KVM_CREATE_IRQCHIP, 0) < 0) {
        LOG_ERROR("kvm: KVM_CREATE_IRQCHIP failed: %s", strerror(errno));
        return nullptr;
    }

    struct kvm_pit_config pit_config{};
    pit_config.flags = KVM_PIT_SPEAKER_DUMMY;
    if (::ioctl(vm->vm_fd_, KVM_CREATE_PIT2, &pit_config) < 0) {
        LOG_WARN("kvm: KVM_CREATE_PIT2 failed: %s (non-fatal)", strerror(errno));
    }

    LOG_INFO("kvm: x86_64 VM created (%u vCPUs, mmap_size=%zu)",
             cpu_count, vm->vcpu_mmap_size_);
    return vm;
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

void KvmVm::RequestInterrupt(const InterruptRequest& /*req*/) {
    // With an in-kernel LAPIC, guest-generated IPIs are handled inside KVM.
    // This path is only exercised by the userspace LAPIC used by HVF/WHVP.
    LOG_WARN("kvm: RequestInterrupt called (no-op with in-kernel LAPIC)");
}

bool KvmVm::AssertIrq(uint32_t gsi, bool level) {
    struct kvm_irq_level irq{};
    irq.irq = gsi;
    irq.level = level ? 1 : 0;
    if (::ioctl(vm_fd_, KVM_IRQ_LINE, &irq) < 0) {
        LOG_WARN("kvm: KVM_IRQ_LINE(gsi=%u level=%d) failed: %s",
                 gsi, (int)level, strerror(errno));
        return true;  // still consumed: don't fall through to userspace IOAPIC
    }
    return true;
}

} // namespace kvm
