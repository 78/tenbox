#pragma once

#include "vmm/types.h"
#include <vector>

namespace x86 {

struct VirtioMmioAcpiInfo {
    uint64_t base;
    uint32_t size;
    uint32_t irq;
};

namespace AcpiLayout {
    constexpr GPA kRsdp = 0x4000;
    constexpr GPA kXsdt = 0x4100;
    constexpr GPA kMadt = 0x4200;
    constexpr GPA kFadt = 0x4300;
    // FADT rev5 is 268 bytes → ends at 0x440C, so DSDT at 0x4500 is safe.
    constexpr GPA kDsdt = 0x4500;
}

// Build ACPI tables (RSDP, XSDT, MADT, FADT, DSDT) in guest RAM.
// The DSDT includes device nodes for each virtio-mmio device in |virtio_devs|.
// Returns the GPA of the RSDP for boot_params.acpi_rsdp_addr.
GPA BuildAcpiTables(uint8_t* ram, uint32_t num_cpus,
                    const std::vector<VirtioMmioAcpiInfo>& virtio_devs = {});

} // namespace x86
