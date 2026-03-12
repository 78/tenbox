#pragma once

#include <cstdint>

namespace hvf {

// Result of decoding an ARM64 load/store instruction that caused a DABT.
struct MmioDecodeResult {
    bool     is_write;
    uint8_t  access_size;   // 1, 2, 4, or 8 bytes
    uint8_t  reg;           // destination/source general-purpose register (0-30, 31=xzr)
    uint64_t write_value;   // valid only when is_write == true

    // For LDP/STP pair instructions
    bool     is_pair;
    uint8_t  reg2;
    uint64_t write_value2;
};

// Decode a 32-bit ARM64 instruction at the faulting PC that triggered
// a Data Abort (DABT).  The |syndrome| is the ESR_EL2 value and |insn|
// is the 32-bit instruction word fetched from guest memory at the
// faulting PC.
//
// On success, populates |result| and returns true.
// On failure (unsupported encoding), returns false.
bool DecodeMmioInstruction(uint32_t insn, uint64_t syndrome,
                           const uint64_t* regs, MmioDecodeResult* result);

} // namespace hvf
