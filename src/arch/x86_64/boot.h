#pragma once

#include "vmm/types.h"
#include <windows.h>
#include <WinHvPlatform.h>
#include "arch/x86_64/acpi.h"
#include <string>
#include <vector>

namespace x86 {

// Guest physical memory layout constants
namespace Layout {
    constexpr GPA kGdtBase       = 0x1000;
    constexpr GPA kBootParams    = 0x7000;
    constexpr GPA kCmdlineBase   = 0x10000;
    constexpr GPA kCmdlineMaxLen = 0x10000;
    constexpr GPA kKernelBase    = 0x100000;   // 1 MB
}

// Linux boot_params (zero page) offsets
namespace BootOffset {
    constexpr uint32_t kAcpiRsdpAddr  = 0x070;
    constexpr uint32_t kE820Entries   = 0x1E8;
    constexpr uint32_t kSetupSects    = 0x1F1;
    constexpr uint32_t kBootFlag      = 0x1FE;
    constexpr uint32_t kHeaderMagic   = 0x202;
    constexpr uint32_t kVersion       = 0x206;
    constexpr uint32_t kTypeOfLoader  = 0x210;
    constexpr uint32_t kLoadflags     = 0x211;
    constexpr uint32_t kCode32Start   = 0x214;
    constexpr uint32_t kRamdiskImage  = 0x218;
    constexpr uint32_t kRamdiskSize   = 0x21C;
    constexpr uint32_t kHeapEndPtr    = 0x224;
    constexpr uint32_t kExtLoaderVer  = 0x226;
    constexpr uint32_t kCmdLinePtr    = 0x228;
    constexpr uint32_t kInitrdAddrMax = 0x22C;
    constexpr uint32_t kKernelAlign   = 0x230;
    constexpr uint32_t kE820Table     = 0x2D0;
}

// E820 memory map entry types
enum E820Type : uint32_t {
    kE820Ram      = 1,
    kE820Reserved = 2,
    kE820Acpi     = 3,
    kE820Nvs      = 4,
};

#pragma pack(push, 1)
struct E820Entry {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
};
#pragma pack(pop)

static_assert(sizeof(E820Entry) == 20);

struct BootConfig {
    std::string kernel_path;
    std::string initrd_path;
    std::string cmdline;
    GuestMemMap mem;
    uint32_t cpu_count = 1;
    std::vector<VirtioMmioAcpiInfo> virtio_devs;
};

// Load kernel, initrd, set up boot_params in guest RAM.
// Returns the number of bytes used by protected-mode kernel, or 0 on error.
// After success, caller should call SetupBootRegisters on the vCPU.
uint64_t LoadLinuxKernel(const BootConfig& config);

// GDT layout written at kGdtBase
struct GdtEntry {
    uint64_t null;      // selector 0x00
    uint64_t unused;    // selector 0x08
    uint64_t code32;    // selector 0x10 - 32-bit code, flat
    uint64_t data32;    // selector 0x18 - 32-bit data, flat
};

// Write GDT and return initial register values for 32-bit protected mode boot.
// Caller should pass these to WhvpVCpu::SetRegisters.
void BuildInitialRegisters(
    uint8_t* ram,
    WHV_REGISTER_NAME* out_names,
    WHV_REGISTER_VALUE* out_values,
    uint32_t* out_count);

} // namespace x86
