#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>

// Carries the per-vCPU startup parameters that an AP thread waits on.
// BSP (index 0) never blocks on this; APs wait until |started| is set.
//
// ARM64: entry_addr / context_id are populated by the PSCI CPU_ON handler.
// x86:   sipi_vector is populated by the LAPIC SIPI callback.
struct VCpuStartupState {
    std::mutex mutex;
    std::condition_variable cv;
    bool started = false;
    // ARM64 PSCI CPU_ON
    uint64_t entry_addr = 0;
    uint64_t context_id = 0;
    // x86 INIT/SIPI
    bool init_received = false;
    uint8_t sipi_vector = 0;
};
