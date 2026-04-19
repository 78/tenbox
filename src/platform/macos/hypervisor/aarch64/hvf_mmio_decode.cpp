#include "platform/macos/hypervisor/aarch64/hvf_mmio_decode.h"
#include "core/vmm/types.h"

namespace hvf {

static inline uint32_t ExtractBits(uint32_t insn, int hi, int lo) {
    return (insn >> lo) & ((1u << (hi - lo + 1)) - 1);
}

static uint64_t ReadReg(const uint64_t* regs, uint8_t reg) {
    if (reg == 31) return 0;  // XZR
    return regs[reg];
}

bool DecodeMmioInstruction(uint32_t insn, uint64_t syndrome,
                           const uint64_t* regs, MmioDecodeResult* result) {
    bool isv = (syndrome >> 24) & 1;

    if (isv) {
        result->is_write = (syndrome >> 6) & 1;
        uint8_t sas = (syndrome >> 22) & 3;
        result->access_size = 1u << sas;
        result->reg = (syndrome >> 16) & 0x1F;
        result->is_pair = false;
        result->reg2 = 0;
        result->write_value2 = 0;

        if (result->is_write) {
            result->write_value = ReadReg(regs, result->reg);
        }
        return true;
    }

    result->is_pair = false;
    result->reg2 = 0;
    result->write_value2 = 0;

    uint32_t op0 = ExtractBits(insn, 31, 30);
    uint32_t bits_29_27 = ExtractBits(insn, 29, 27);

    if (bits_29_27 == 0b101 && ((insn >> 26) & 1) == 0) {
        bool is_load = (insn >> 22) & 1;
        uint8_t rt = insn & 0x1F;
        uint8_t rt2 = ExtractBits(insn, 14, 10);

        uint8_t size;
        if ((op0 & 2) == 0) {
            size = 4;
        } else {
            size = 8;
        }

        result->is_write = !is_load;
        result->access_size = size;
        result->reg = rt;
        result->is_pair = true;
        result->reg2 = rt2;

        if (result->is_write) {
            result->write_value = ReadReg(regs, rt);
            result->write_value2 = ReadReg(regs, rt2);
        }
        return true;
    }

    uint32_t bits_29_24 = ExtractBits(insn, 29, 24);
    if ((bits_29_24 & 0b111011) == 0b111001) {
        uint8_t size_bits = ExtractBits(insn, 31, 30);
        uint8_t opc = ExtractBits(insn, 23, 22);
        uint8_t rt = insn & 0x1F;

        result->access_size = 1u << size_bits;
        result->is_write = (opc == 0);
        result->reg = rt;

        if (result->is_write) {
            result->write_value = ReadReg(regs, rt);
        }
        return true;
    }

    if ((bits_29_24 & 0b111011) == 0b111000) {
        uint8_t size_bits = ExtractBits(insn, 31, 30);
        uint8_t opc = ExtractBits(insn, 23, 22);
        uint8_t rt = insn & 0x1F;

        result->access_size = 1u << size_bits;
        result->is_write = (opc == 0);
        result->reg = rt;

        if (result->is_write) {
            result->write_value = ReadReg(regs, rt);
        }
        return true;
    }

    LOG_WARN("MMIO decode: unsupported instruction 0x%08x, syndrome 0x%" PRIx64,
             insn, (uint64_t)syndrome);
    return false;
}

} // namespace hvf
