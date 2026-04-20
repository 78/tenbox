#include "core/vmm/vm_platform.h"
#include "platform/linux/hypervisor/kvm_platform.h"
#if defined(__x86_64__)
#include "platform/linux/hypervisor/x86_64/kvm_vm.h"
#elif defined(__aarch64__)
#include "platform/linux/hypervisor/aarch64/kvm_vm.h"
#else
#error "Unsupported Linux architecture for KVM backend"
#endif
#include "platform/posix/console/posix_console_port.h"

#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

bool VmPlatform::IsHypervisorPresent() {
    return kvm::IsHypervisorPresent();
}

std::unique_ptr<HypervisorVm> VmPlatform::CreateHypervisor(uint32_t cpu_count) {
    return kvm::KvmVm::Create(cpu_count);
}

uint8_t* VmPlatform::AllocateRam(uint64_t size) {
    void* ptr = ::mmap(nullptr, size,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) return nullptr;

    // Hint the kernel to back guest RAM with 2 MiB transparent huge pages.
    // Only a hint — kernel silently falls back to 4 KiB pages if the mapping
    // edges aren't 2 MiB aligned or if contiguous memory is unavailable, so
    // there's no failure path to handle. Reduces stage-2 TLB pressure on
    // arm64 and x86 alike.
    ::madvise(ptr, size, MADV_HUGEPAGE);

    return static_cast<uint8_t*>(ptr);
}

void VmPlatform::FreeRam(uint8_t* base, uint64_t size) {
    if (base) {
        ::munmap(base, size);
    }
}

std::shared_ptr<ConsolePort> VmPlatform::CreateConsolePort() {
    return std::make_shared<PosixConsolePort>();
}

void VmPlatform::YieldCpu() {
    ::sched_yield();
}

void VmPlatform::SleepMs(uint32_t ms) {
    ::usleep(static_cast<useconds_t>(ms) * 1000);
}
