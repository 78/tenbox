#include "core/arch/aarch64/boot.h"
#include <cstdio>
#include <cstring>

namespace aarch64 {

GPA LoadLinuxImage(BootConfig& config) {
    FILE* fp = fopen(config.kernel_path.c_str(), "rb");
    if (!fp) {
        LOG_ERROR("aarch64: cannot open kernel: %s", config.kernel_path.c_str());
        return 0;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size < static_cast<long>(sizeof(LinuxImageHeader))) {
        LOG_ERROR("aarch64: kernel too small (%ld bytes)", file_size);
        fclose(fp);
        return 0;
    }

    // Read and validate the ARM64 Image header
    LinuxImageHeader hdr{};
    fread(&hdr, sizeof(hdr), 1, fp);

    if (hdr.magic != kArmImageMagic) {
        LOG_ERROR("aarch64: invalid ARM64 Image magic (got 0x%08x, expected 0x%08x)",
                  hdr.magic, kArmImageMagic);
        fclose(fp);
        return 0;
    }

    uint64_t text_offset = hdr.text_offset;
    if (text_offset == 0) {
        text_offset = 0x200000;  // default 2 MiB offset
    }

    GPA kernel_gpa = Layout::kRamBase + text_offset;

    // Validate kernel fits in guest RAM
    uint64_t kernel_end_offset = text_offset + static_cast<uint64_t>(file_size);
    if (kernel_end_offset > config.mem.alloc_size) {
        LOG_ERROR("aarch64: kernel too large for guest RAM (%ld bytes)", file_size);
        fclose(fp);
        return 0;
    }

    // Load kernel at the computed offset within guest RAM
    // config.mem.base points to the host memory mapped at Layout::kRamBase
    uint8_t* dest = config.mem.base + text_offset;
    fseek(fp, 0, SEEK_SET);
    size_t read = fread(dest, 1, static_cast<size_t>(file_size), fp);
    fclose(fp);

    if (read != static_cast<size_t>(file_size)) {
        LOG_ERROR("aarch64: short kernel read (%zu / %ld)", read, file_size);
        return 0;
    }

    LOG_INFO("aarch64: kernel loaded at GPA 0x%" PRIx64 " (%ld bytes, text_offset=0x%" PRIx64 ")",
             (uint64_t)kernel_gpa, file_size, text_offset);

    return kernel_gpa;
}

} // namespace aarch64
