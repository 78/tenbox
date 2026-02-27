#include "core/arch/x86_64/acpi.h"
#include <cstring>

namespace x86 {

static uint8_t AcpiChecksum(const void* data, size_t len) {
    uint8_t sum = 0;
    auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; i++) sum += p[i];
    return static_cast<uint8_t>(-sum);
}

#pragma pack(push, 1)

struct AcpiRsdp {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
};
static_assert(sizeof(AcpiRsdp) == 36);

struct AcpiHeader {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    char     creator_id[4];
    uint32_t creator_revision;
};
static_assert(sizeof(AcpiHeader) == 36);

struct MadtLocalApic {
    uint8_t  type;        // 0 = Processor Local APIC
    uint8_t  length;      // 8
    uint8_t  processor_id;
    uint8_t  apic_id;
    uint32_t flags;       // bit 0 = enabled
};
static_assert(sizeof(MadtLocalApic) == 8);

struct MadtIoApic {
    uint8_t  type;        // 1 = I/O APIC
    uint8_t  length;      // 12
    uint8_t  io_apic_id;
    uint8_t  reserved;
    uint32_t io_apic_address;
    uint32_t gsi_base;
};
static_assert(sizeof(MadtIoApic) == 12);

struct MadtIntOverride {
    uint8_t  type;        // 2 = Interrupt Source Override
    uint8_t  length;      // 10
    uint8_t  bus;         // 0 = ISA
    uint8_t  source;      // IRQ source
    uint32_t gsi;         // Global System Interrupt
    uint16_t flags;       // MPS INTI flags
};
static_assert(sizeof(MadtIntOverride) == 10);

#pragma pack(pop)

static void FillHeader(AcpiHeader* h, const char sig[4], uint32_t len,
                       uint8_t rev) {
    memcpy(h->signature, sig, 4);
    h->length = len;
    h->revision = rev;
    memcpy(h->oem_id, "TENBOX", 6);
    memcpy(h->oem_table_id, "TENBOX  ", 8);
    h->oem_revision = 1;
    memcpy(h->creator_id, "TNBX", 4);
    h->creator_revision = 1;
}

// ---------------------------------------------------------------------------
// DSDT builder — emit minimal AML with virtio-mmio device nodes
// ---------------------------------------------------------------------------
// Each device produces:
//   Device(VRxx) { Name(_HID,"LNRO0005"), Name(_UID,N), Name(_CRS,Buffer) }
//
// Per-device body:  _HID(15) + _UID(7) + _CRS(32) = 54 bytes
// Per-device entry: ExtOp(1) + DevOp(1) + PkgLen(1) + NameSeg(4) + body = 61

static uint32_t BuildDsdt(uint8_t* buf,
                           const std::vector<VirtioMmioAcpiInfo>& devs) {
    const uint32_t N = static_cast<uint32_t>(devs.size());
    const uint32_t kDevBody  = 54;
    const uint32_t kDevEntry = 61;

    // Name(\_S5, Package(4){5,5,0,0}) — 16 bytes AML at DSDT root scope
    const uint32_t kS5Size = 16;

    uint32_t scope_body = N * kDevEntry;
    uint32_t scope_namelen = 5;   // \_SB_
    uint32_t scope_remaining = scope_namelen + scope_body;
    uint32_t scope_pkglen_sz = (scope_remaining + 1 <= 63) ? 1 : 2;
    uint32_t scope_pkglen_val = scope_pkglen_sz + scope_remaining;
    uint32_t scope_total = 1 + scope_pkglen_sz + scope_remaining;
    uint32_t dsdt_total = sizeof(AcpiHeader) + kS5Size + scope_total;

    uint8_t* p = buf;

    AcpiHeader* hdr = reinterpret_cast<AcpiHeader*>(p);
    memset(hdr, 0, dsdt_total);
    FillHeader(hdr, "DSDT", dsdt_total, 2);
    p += sizeof(AcpiHeader);

    // Name(\_S5_, Package(4) { 5, 5, 0, 0 })
    // SLP_TYPa=5, SLP_TYPb=5 — must match AcpiPm::kSlpTypS5
    *p++ = 0x08;                            // NameOp
    *p++ = '_'; *p++ = 'S'; *p++ = '5'; *p++ = '_';
    *p++ = 0x12;                            // PackageOp
    *p++ = 0x0A;                            // PkgLen = 10
    *p++ = 0x04;                            // NumElements = 4
    *p++ = 0x0A; *p++ = 0x05;              // SLP_TYPa = 5
    *p++ = 0x0A; *p++ = 0x05;              // SLP_TYPb = 5
    *p++ = 0x0A; *p++ = 0x00;              // reserved
    *p++ = 0x0A; *p++ = 0x00;              // reserved

    // Scope(\_SB)
    *p++ = 0x10; // ScopeOp
    if (scope_pkglen_sz == 1) {
        *p++ = static_cast<uint8_t>(scope_pkglen_val);
    } else {
        *p++ = static_cast<uint8_t>((scope_pkglen_val & 0x0F) | 0x40);
        *p++ = static_cast<uint8_t>(scope_pkglen_val >> 4);
    }
    *p++ = 0x5C; // RootChar '\'
    *p++ = '_'; *p++ = 'S'; *p++ = 'B'; *p++ = '_';

    for (uint32_t i = 0; i < N; i++) {
        auto& d = devs[i];

        // Device(VRxx)
        *p++ = 0x5B; *p++ = 0x82; // ExtOpPrefix + DeviceOp
        *p++ = static_cast<uint8_t>(1 + 4 + kDevBody); // PkgLen
        *p++ = 'V'; *p++ = 'R';
        *p++ = "0123456789ABCDEF"[i / 16];
        *p++ = "0123456789ABCDEF"[i % 16];

        // Name(_HID, "LNRO0005")  — 15 bytes
        *p++ = 0x08; // NameOp
        *p++ = '_'; *p++ = 'H'; *p++ = 'I'; *p++ = 'D';
        *p++ = 0x0D; // StringPrefix
        memcpy(p, "LNRO0005", 8); p += 8;
        *p++ = 0x00; // NullChar

        // Name(_UID, ByteConst(i))  — 7 bytes
        *p++ = 0x08;
        *p++ = '_'; *p++ = 'U'; *p++ = 'I'; *p++ = 'D';
        *p++ = 0x0A; // BytePrefix
        *p++ = static_cast<uint8_t>(i);

        // Name(_CRS, Buffer(23) { Memory32Fixed + ExtInterrupt + EndTag })
        // 32 bytes total
        *p++ = 0x08;
        *p++ = '_'; *p++ = 'C'; *p++ = 'R'; *p++ = 'S';
        *p++ = 0x11; // BufferOp
        *p++ = 0x1A; // PkgLen = 26 (1+2+23)
        *p++ = 0x0A; *p++ = 0x17; // BytePrefix + BufferSize=23

        // Memory32Fixed(ReadWrite, base, size)  — 12 bytes
        *p++ = 0x86; // tag
        *p++ = 0x09; *p++ = 0x00; // length=9
        *p++ = 0x01; // ReadWrite
        *p++ = static_cast<uint8_t>(d.base);
        *p++ = static_cast<uint8_t>(d.base >> 8);
        *p++ = static_cast<uint8_t>(d.base >> 16);
        *p++ = static_cast<uint8_t>(d.base >> 24);
        *p++ = static_cast<uint8_t>(d.size);
        *p++ = static_cast<uint8_t>(d.size >> 8);
        *p++ = static_cast<uint8_t>(d.size >> 16);
        *p++ = static_cast<uint8_t>(d.size >> 24);

        // Interrupt(ResourceConsumer, Level, ActiveHigh, Exclusive) {irq}
        // — 9 bytes
        *p++ = 0x89; // tag
        *p++ = 0x06; *p++ = 0x00; // length=6
        *p++ = 0x01; // flags: ResourceConsumer, Level, ActiveHigh, Exclusive
        *p++ = 0x01; // interrupt table length
        *p++ = static_cast<uint8_t>(d.irq);
        *p++ = static_cast<uint8_t>(d.irq >> 8);
        *p++ = static_cast<uint8_t>(d.irq >> 16);
        *p++ = static_cast<uint8_t>(d.irq >> 24);

        // End tag — 2 bytes
        *p++ = 0x79; *p++ = 0x00;
    }

    hdr->checksum = AcpiChecksum(buf, dsdt_total);
    return dsdt_total;
}

// ---------------------------------------------------------------------------
// FADT — Fixed ACPI Description Table (rev 5)
// ---------------------------------------------------------------------------
// We provide minimal PM1 registers so the kernel can enable ACPI normally
// (SMI_CMD=0 → hardware-mode ACPI already active) without resorting to
// HW_REDUCED_ACPI, which would break legacy IRQ pre-allocation.

static constexpr uint32_t kFadtSize = 268; // FADT revision 5

// PM1 register I/O ports (must match AcpiPm device in the VMM)
static constexpr uint16_t kPm1aEvtPort = 0x600;
static constexpr uint16_t kPm1aCntPort = 0x604;
static constexpr uint16_t kResetPort   = 0x608;
static constexpr uint8_t  kResetValue  = 0x01;

static void BuildFadt(uint8_t* buf, GPA dsdt_addr) {
    memset(buf, 0, kFadtSize);

    AcpiHeader* hdr = reinterpret_cast<AcpiHeader*>(buf);
    FillHeader(hdr, "FACP", kFadtSize, 5);

    // DSDT pointer (32-bit legacy field, offset 40)
    uint32_t dsdt32 = static_cast<uint32_t>(dsdt_addr);
    memcpy(buf + 40, &dsdt32, 4);

    // SCI_INT (offset 46) — SCI interrupt number (GSI 9 is conventional)
    uint16_t sci_int = 9;
    memcpy(buf + 46, &sci_int, 2);

    // SMI_CMD (offset 48) = 0 → ACPI is already in hardware mode
    // (kernel skips SMI-based ACPI enable when this is zero)

    // PM1a_EVT_BLK (offset 56, 4 bytes) — PM1 Status + Enable registers
    uint32_t pm1a_evt = kPm1aEvtPort;
    memcpy(buf + 56, &pm1a_evt, 4);

    // PM1a_CNT_BLK (offset 64, 4 bytes) — PM1 Control register
    uint32_t pm1a_cnt = kPm1aCntPort;
    memcpy(buf + 64, &pm1a_cnt, 4);

    // PM1_EVT_LEN (offset 88) = 4
    buf[88] = 4;
    // PM1_CNT_LEN (offset 89) = 2
    buf[89] = 2;

    // Flags (offset 112, uint32_t):
    //   Bit 4 (PWR_BUTTON): 1 = no fixed-hardware power button
    //   Bit 5 (SLP_BUTTON): 1 = no fixed-hardware sleep button
    //   Bit 10 (RESET_REG_SUP): 1 = RESET_REG is supported
    // We don't emulate fixed power/sleep button events, so set both bits
    // to prevent the kernel from looking for handlers that don't exist.
    uint32_t fadt_flags = (1u << 4) | (1u << 5) | (1u << 10);
    memcpy(buf + 112, &fadt_flags, 4);

    // RESET_REG — Generic Address Structure (offset 116, 12 bytes)
    buf[116] = 1;   // AddressSpaceId = System I/O
    buf[117] = 8;   // RegisterBitWidth = 8 bits
    buf[118] = 0;   // RegisterBitOffset
    buf[119] = 1;   // AccessSize = Byte
    uint64_t reset_addr = kResetPort;
    memcpy(buf + 120, &reset_addr, 8);

    // RESET_VALUE (offset 128) — value to write to RESET_REG
    buf[128] = kResetValue;

    // FADT minor version (offset 131)
    buf[131] = 1;

    // X_DSDT — 64-bit DSDT address (offset 140)
    uint64_t xdsdt = dsdt_addr;
    memcpy(buf + 140, &xdsdt, 8);

    // X_PM1a_EVT_BLK — Generic Address Structure (offset 148, 12 bytes)
    buf[148] = 1;   // AddressSpaceId = System I/O
    buf[149] = 32;  // RegisterBitWidth
    buf[150] = 0;   // RegisterBitOffset
    buf[151] = 3;   // AccessSize = DWord
    uint64_t evt_addr = kPm1aEvtPort;
    memcpy(buf + 152, &evt_addr, 8);

    // X_PM1a_CNT_BLK — Generic Address Structure (offset 172, 12 bytes)
    buf[172] = 1;   // AddressSpaceId = System I/O
    buf[173] = 16;  // RegisterBitWidth
    buf[174] = 0;   // RegisterBitOffset
    buf[175] = 2;   // AccessSize = Word
    uint64_t cnt_addr = kPm1aCntPort;
    memcpy(buf + 176, &cnt_addr, 8);

    hdr->checksum = AcpiChecksum(buf, kFadtSize);
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

GPA BuildAcpiTables(uint8_t* ram, uint32_t num_cpus,
                    const std::vector<VirtioMmioAcpiInfo>& virtio_devs) {

    // --- MADT ---
    uint8_t* madt_base = ram + AcpiLayout::kMadt;
    memset(madt_base, 0, 256);

    AcpiHeader* madt = reinterpret_cast<AcpiHeader*>(madt_base);
    FillHeader(madt, "APIC", 0, 3);

    uint8_t* p = madt_base + sizeof(AcpiHeader);

    // MADT body: Local APIC address + flags
    *reinterpret_cast<uint32_t*>(p) = 0xFEE00000;  // LAPIC address
    p += 4;
    *reinterpret_cast<uint32_t*>(p) = 0x00000001;  // Flags: PCAT_COMPAT
    p += 4;

    for (uint32_t i = 0; i < num_cpus; i++) {
        auto* entry = reinterpret_cast<MadtLocalApic*>(p);
        entry->type = 0;
        entry->length = sizeof(MadtLocalApic);
        entry->processor_id = static_cast<uint8_t>(i);
        entry->apic_id = static_cast<uint8_t>(i);
        entry->flags = 1;
        p += sizeof(MadtLocalApic);
    }

    auto* ioapic = reinterpret_cast<MadtIoApic*>(p);
    ioapic->type = 1;
    ioapic->length = sizeof(MadtIoApic);
    ioapic->io_apic_id = static_cast<uint8_t>(num_cpus);
    ioapic->reserved = 0;
    ioapic->io_apic_address = 0xFEC00000;
    ioapic->gsi_base = 0;
    p += sizeof(MadtIoApic);

    // SCI (IRQ 9) must be declared as active-low, level-triggered.
    auto* sci_ovr = reinterpret_cast<MadtIntOverride*>(p);
    sci_ovr->type = 2;
    sci_ovr->length = sizeof(MadtIntOverride);
    sci_ovr->bus = 0;
    sci_ovr->source = 9;
    sci_ovr->gsi = 9;
    sci_ovr->flags = 0x000F;  // active-low (bit 1:0 = 11), level-triggered (bit 3:2 = 11)
    p += sizeof(MadtIntOverride);

    madt->length = static_cast<uint32_t>(p - madt_base);
    madt->checksum = AcpiChecksum(madt_base, madt->length);

    // --- DSDT ---
    uint32_t dsdt_size = BuildDsdt(ram + AcpiLayout::kDsdt, virtio_devs);

    // --- FADT ---
    BuildFadt(ram + AcpiLayout::kFadt, AcpiLayout::kDsdt);

    // --- XSDT (now carries FADT + MADT) ---
    uint8_t* xsdt_base = ram + AcpiLayout::kXsdt;
    memset(xsdt_base, 0, 128);

    AcpiHeader* xsdt = reinterpret_cast<AcpiHeader*>(xsdt_base);
    FillHeader(xsdt, "XSDT", 0, 1);

    // FADT must be the first entry (ACPI spec requirement)
    uint64_t* entries = reinterpret_cast<uint64_t*>(
        xsdt_base + sizeof(AcpiHeader));
    entries[0] = AcpiLayout::kFadt;
    entries[1] = AcpiLayout::kMadt;

    xsdt->length = sizeof(AcpiHeader) + 2 * sizeof(uint64_t);
    xsdt->checksum = AcpiChecksum(xsdt_base, xsdt->length);

    // --- RSDP ---
    uint8_t* rsdp_base = ram + AcpiLayout::kRsdp;
    memset(rsdp_base, 0, sizeof(AcpiRsdp));

    AcpiRsdp* rsdp = reinterpret_cast<AcpiRsdp*>(rsdp_base);
    memcpy(rsdp->signature, "RSD PTR ", 8);
    memcpy(rsdp->oem_id, "TENBOX", 6);
    rsdp->revision = 2;
    rsdp->rsdt_address = 0;
    rsdp->length = sizeof(AcpiRsdp);
    rsdp->xsdt_address = AcpiLayout::kXsdt;

    rsdp->checksum = AcpiChecksum(rsdp_base, 20);
    rsdp->extended_checksum = AcpiChecksum(rsdp_base, sizeof(AcpiRsdp));

    LOG_INFO("ACPI tables: RSDP@0x%llX XSDT@0x%llX MADT@0x%llX "
             "FADT@0x%llX DSDT@0x%llX (%u virtio-mmio dev%s)",
             AcpiLayout::kRsdp, AcpiLayout::kXsdt, AcpiLayout::kMadt,
             AcpiLayout::kFadt, AcpiLayout::kDsdt,
             (uint32_t)virtio_devs.size(),
             virtio_devs.size() == 1 ? "" : "s");

    return AcpiLayout::kRsdp;
}

} // namespace x86
