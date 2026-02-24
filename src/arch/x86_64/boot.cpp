#include "arch/x86_64/boot.h"
#include "arch/x86_64/acpi.h"
#include <fstream>
#include <cstring>
#include <algorithm>

namespace x86 {

static std::vector<uint8_t> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

uint64_t LoadLinuxKernel(const BootConfig& config, uint8_t* ram,
                          uint64_t ram_size) {
    auto kernel = ReadFile(config.kernel_path);
    if (kernel.size() < 1024) {
        LOG_ERROR("Kernel file too small or not found: %s",
                  config.kernel_path.c_str());
        return 0;
    }

    // Verify bzImage header magic "HdrS" at offset 0x202
    if (memcmp(kernel.data() + BootOffset::kHeaderMagic, "HdrS", 4) != 0) {
        LOG_ERROR("Invalid bzImage: missing HdrS magic");
        return 0;
    }

    uint16_t version = *reinterpret_cast<uint16_t*>(
        kernel.data() + BootOffset::kVersion);
    LOG_INFO("Linux boot protocol version: %d.%d",
             version >> 8, version & 0xFF);

    if (version < 0x0206) {
        LOG_ERROR("Boot protocol version too old (need >= 2.06)");
        return 0;
    }

    uint8_t setup_sects = kernel[BootOffset::kSetupSects];
    if (setup_sects == 0) setup_sects = 4;

    uint32_t setup_size = (1 + setup_sects) * 512;
    uint32_t kernel_size = static_cast<uint32_t>(kernel.size()) - setup_size;

    if (Layout::kKernelBase + kernel_size > ram_size) {
        LOG_ERROR("Kernel too large for guest RAM");
        return 0;
    }

    // Copy protected-mode kernel to 1MB
    memcpy(ram + Layout::kKernelBase, kernel.data() + setup_size, kernel_size);
    LOG_INFO("Kernel loaded at GPA 0x%llX (%u bytes)",
             Layout::kKernelBase, kernel_size);

    // --- Set up boot_params (zero page) ---
    uint8_t* bp = ram + Layout::kBootParams;
    memset(bp, 0, 4096);

    // Copy setup header from bzImage into boot_params
    // The setup_header starts at file offset 0x1F1 and its size varies
    // We copy from 0x1F1 to the end of the setup data
    uint32_t header_offset = BootOffset::kSetupSects;  // 0x1F1
    uint32_t header_end = std::min(setup_size, (uint32_t)0x290);
    if (header_end > header_offset) {
        memcpy(bp + header_offset,
               kernel.data() + header_offset,
               header_end - header_offset);
    }

    // Patch boot_params fields
    bp[BootOffset::kTypeOfLoader] = 0xFF;
    bp[BootOffset::kLoadflags] |= 0x01;   // LOADED_HIGH

    // Kernel command line
    if (!config.cmdline.empty()) {
        size_t len = std::min(config.cmdline.size(),
                              (size_t)Layout::kCmdlineMaxLen - 1);
        memcpy(ram + Layout::kCmdlineBase, config.cmdline.c_str(), len);
        ram[Layout::kCmdlineBase + len] = '\0';
        *reinterpret_cast<uint32_t*>(bp + BootOffset::kCmdLinePtr) =
            static_cast<uint32_t>(Layout::kCmdlineBase);
    }

    // Load initrd
    if (!config.initrd_path.empty()) {
        auto initrd = ReadFile(config.initrd_path);
        if (initrd.empty()) {
            LOG_ERROR("Failed to read initrd: %s", config.initrd_path.c_str());
            return 0;
        }

        // Place initrd at the highest aligned address that fits
        uint64_t initrd_addr = AlignDown(
            ram_size - initrd.size(), kPageSize);

        if (initrd_addr <= Layout::kKernelBase + kernel_size) {
            LOG_ERROR("Not enough RAM for initrd (%zu bytes)", initrd.size());
            return 0;
        }

        memcpy(ram + initrd_addr, initrd.data(), initrd.size());

        *reinterpret_cast<uint32_t*>(bp + BootOffset::kRamdiskImage) =
            static_cast<uint32_t>(initrd_addr);
        *reinterpret_cast<uint32_t*>(bp + BootOffset::kRamdiskSize) =
            static_cast<uint32_t>(initrd.size());

        LOG_INFO("Initrd loaded at GPA 0x%llX (%zu bytes)",
                 initrd_addr, initrd.size());
    }

    // E820 memory map
    // Entry 0: 0 - 0x9FFFF (640 KB, conventional memory)
    // Entry 1: 0x100000 - (ram_size - 1) (extended memory)
    E820Entry* e820 = reinterpret_cast<E820Entry*>(
        bp + BootOffset::kE820Table);

    e820[0] = {0, 0xA0000, kE820Ram};
    e820[1] = {0x100000, ram_size - 0x100000, kE820Ram};
    bp[BootOffset::kE820Entries] = 2;

    LOG_INFO("E820: [0-0x9FFFF RAM] [0x100000-0x%llX RAM]",
             ram_size - 1);

    // Build ACPI tables (RSDP, XSDT, MADT, FADT, DSDT) and store RSDP GPA
    GPA rsdp_addr = BuildAcpiTables(ram, 1, config.virtio_devs);
    *reinterpret_cast<uint64_t*>(bp + BootOffset::kAcpiRsdpAddr) = rsdp_addr;

    return kernel_size;
}

void BuildInitialRegisters(
    uint8_t* ram,
    WHV_REGISTER_NAME* out_names,
    WHV_REGISTER_VALUE* out_values,
    uint32_t* out_count) {

    // Write GDT into guest memory at kGdtBase
    GdtEntry* gdt = reinterpret_cast<GdtEntry*>(ram + Layout::kGdtBase);
    gdt->null   = 0x0000000000000000ULL;
    gdt->unused = 0x0000000000000000ULL;
    // 32-bit code: base=0, limit=0xFFFFF, G=1 D=1 P=1 DPL=0 S=1 type=0xB
    gdt->code32 = 0x00CF9B000000FFFFULL;
    // 32-bit data: base=0, limit=0xFFFFF, G=1 D=1 P=1 DPL=0 S=1 type=0x3
    gdt->data32 = 0x00CF93000000FFFFULL;

    uint32_t i = 0;

    // GDTR
    out_names[i] = WHvX64RegisterGdtr;
    out_values[i].Table.Base = Layout::kGdtBase;
    out_values[i].Table.Limit = sizeof(GdtEntry) - 1;
    i++;

    // IDTR (empty, kernel will set its own)
    out_names[i] = WHvX64RegisterIdtr;
    out_values[i].Table.Base = 0;
    out_values[i].Table.Limit = 0;
    i++;

    // CS = selector 0x10 (code segment)
    out_names[i] = WHvX64RegisterCs;
    out_values[i].Segment.Base = 0;
    out_values[i].Segment.Limit = 0xFFFFFFFF;
    out_values[i].Segment.Selector = 0x10;
    out_values[i].Segment.Attributes = 0xC09B;  // G=1 D=1 P=1 S=1 type=0xB
    i++;

    // DS = selector 0x18 (data segment)
    auto SetDataSeg = [&](WHV_REGISTER_NAME name) {
        out_names[i] = name;
        out_values[i].Segment.Base = 0;
        out_values[i].Segment.Limit = 0xFFFFFFFF;
        out_values[i].Segment.Selector = 0x18;
        out_values[i].Segment.Attributes = 0xC093;  // G=1 D=1 P=1 S=1 type=0x3
        i++;
    };
    SetDataSeg(WHvX64RegisterDs);
    SetDataSeg(WHvX64RegisterEs);
    SetDataSeg(WHvX64RegisterSs);

    // FS, GS with null selectors
    auto SetNullSeg = [&](WHV_REGISTER_NAME name) {
        out_names[i] = name;
        out_values[i].Segment.Base = 0;
        out_values[i].Segment.Limit = 0;
        out_values[i].Segment.Selector = 0;
        out_values[i].Segment.Attributes = 0;
        i++;
    };
    SetNullSeg(WHvX64RegisterFs);
    SetNullSeg(WHvX64RegisterGs);

    // TR (task register) - must be valid for WHVP
    out_names[i] = WHvX64RegisterTr;
    out_values[i].Segment.Base = 0;
    out_values[i].Segment.Limit = 0;
    out_values[i].Segment.Selector = 0;
    out_values[i].Segment.Attributes = 0x008B;  // P=1 type=busy 32-bit TSS
    i++;

    // LDTR
    out_names[i] = WHvX64RegisterLdtr;
    out_values[i].Segment.Base = 0;
    out_values[i].Segment.Limit = 0;
    out_values[i].Segment.Selector = 0;
    out_values[i].Segment.Attributes = 0x0082;  // P=1 type=LDT
    i++;

    // RIP = kernel entry point
    out_names[i] = WHvX64RegisterRip;
    out_values[i].Reg64 = Layout::kKernelBase;
    i++;

    // RSI = pointer to boot_params
    out_names[i] = WHvX64RegisterRsi;
    out_values[i].Reg64 = Layout::kBootParams;
    i++;

    // RFLAGS = bit 1 always set
    out_names[i] = WHvX64RegisterRflags;
    out_values[i].Reg64 = 0x2;
    i++;

    // CR0 = PE (protected mode) + ET (math coprocessor)
    out_names[i] = WHvX64RegisterCr0;
    out_values[i].Reg64 = 0x11;
    i++;

    // Zero out general purpose registers
    WHV_REGISTER_NAME gp_regs[] = {
        WHvX64RegisterRax, WHvX64RegisterRbx, WHvX64RegisterRcx,
        WHvX64RegisterRdx, WHvX64RegisterRdi, WHvX64RegisterRbp,
        WHvX64RegisterRsp,
    };
    for (auto name : gp_regs) {
        out_names[i] = name;
        out_values[i].Reg64 = 0;
        i++;
    }

    *out_count = i;
}

} // namespace x86
