#include "core/vmm/vm_platform.h"
#include "platform/windows/hypervisor/whvp_vm.h"
#include "platform/windows/hypervisor/whvp_vcpu.h"
#include "platform/windows/console/std_console_port.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

bool VmPlatform::IsHypervisorPresent() {
    return whvp::IsHypervisorPresent();
}

std::unique_ptr<HypervisorVm> VmPlatform::CreateHypervisor(uint32_t cpu_count) {
    auto vm = whvp::WhvpVm::Create(cpu_count);
    if (vm) {
        whvp::WhvpVCpu::EnableExitStats(false);
    }
    return vm;
}

uint8_t* VmPlatform::AllocateRam(uint64_t size) {
    return static_cast<uint8_t*>(
        VirtualAlloc(nullptr, size,
                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
}

void VmPlatform::FreeRam(uint8_t* base, uint64_t /*size*/) {
    if (base) {
        VirtualFree(base, 0, MEM_RELEASE);
    }
}

std::shared_ptr<ConsolePort> VmPlatform::CreateConsolePort() {
    return std::make_shared<StdConsolePort>();
}

void VmPlatform::YieldCpu() {
    SwitchToThread();
}

void VmPlatform::SleepMs(uint32_t ms) {
    Sleep(ms);
}
