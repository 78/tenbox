#include "platform/windows/hypervisor/whvp_vm.h"
#include "platform/windows/hypervisor/whvp_vcpu.h"
#include <cinttypes>
#include <intrin.h>

namespace whvp {

WhvpVm::~WhvpVm() {
    if (partition_) {
        WHvDeletePartition(partition_);
        partition_ = nullptr;
    }
}

std::unique_ptr<WhvpVm> WhvpVm::Create(uint32_t cpu_count) {
    auto vm = std::unique_ptr<WhvpVm>(new WhvpVm());

    HRESULT hr = WHvCreatePartition(&vm->partition_);
    if (FAILED(hr)) {
        LOG_ERROR("WHvCreatePartition failed: 0x%08lX", hr);
        return nullptr;
    }

    WHV_PARTITION_PROPERTY prop{};
    prop.ProcessorCount = cpu_count;
    hr = WHvSetPartitionProperty(vm->partition_,
        WHvPartitionPropertyCodeProcessorCount,
        &prop, sizeof(prop.ProcessorCount));
    if (FAILED(hr)) {
        LOG_ERROR("Set ProcessorCount failed: 0x%08lX", hr);
        return nullptr;
    }

    memset(&prop, 0, sizeof(prop));
    prop.LocalApicEmulationMode = WHvX64LocalApicEmulationModeXApic;
    hr = WHvSetPartitionProperty(vm->partition_,
        WHvPartitionPropertyCodeLocalApicEmulationMode,
        &prop, sizeof(prop.LocalApicEmulationMode));
    if (FAILED(hr)) {
        LOG_WARN("Set APIC emulation failed: 0x%08lX (non-fatal)", hr);
    }

    // Query WHVP clock frequencies for diagnostics.
    uint64_t proc_freq = 0, intr_freq = 0;
    hr = WHvGetCapability(WHvCapabilityCodeProcessorClockFrequency,
                          &proc_freq, sizeof(proc_freq), nullptr);
    if (SUCCEEDED(hr) && proc_freq) {
        LOG_INFO("WHVP ProcessorClockFrequency: %" PRIu64 " Hz", proc_freq);
    }
    hr = WHvGetCapability(WHvCapabilityCodeInterruptClockFrequency,
                          &intr_freq, sizeof(intr_freq), nullptr);
    if (SUCCEEDED(hr) && intr_freq) {
        LOG_INFO("WHVP InterruptClockFrequency: %" PRIu64 " Hz", intr_freq);
    }

    // Build CPUID override list: 0x15 (TSC freq) + 0x01 (features).
    WHV_X64_CPUID_RESULT cpuid_overrides[2]{};
    int num_overrides = 0;

    // CPUID 0x15: TSC / Core Crystal Clock frequency.
    // This allows the guest kernel to determine TSC speed without PIT calibration.
    {
        int cpuid15[4]{};
        __cpuid(cpuid15, 0x15);
        uint32_t denom   = static_cast<uint32_t>(cpuid15[0]);
        uint32_t numer   = static_cast<uint32_t>(cpuid15[1]);
        uint32_t crystal = static_cast<uint32_t>(cpuid15[2]);

        if (denom && numer) {
            // Intel CPU with native CPUID 0x15 support.
            if (crystal == 0) crystal = 38400000;  // 38.4 MHz for modern Intel
            uint64_t tsc_freq = static_cast<uint64_t>(crystal) * numer / denom;
            LOG_INFO("CPUID 0x15 (native): crystal=%u Hz, TSC=%" PRIu64 " Hz", crystal, tsc_freq);
        } else {
            // AMD or older CPU without CPUID 0x15 - synthesize it from QPC measurement.
            LARGE_INTEGER qpf, qpc_start, qpc_end;
            QueryPerformanceFrequency(&qpf);
            QueryPerformanceCounter(&qpc_start);
            uint64_t tsc_start = __rdtsc();
            Sleep(50);
            uint64_t tsc_end = __rdtsc();
            QueryPerformanceCounter(&qpc_end);

            double elapsed = static_cast<double>(qpc_end.QuadPart - qpc_start.QuadPart)
                             / qpf.QuadPart;
            uint64_t tsc_freq = static_cast<uint64_t>((tsc_end - tsc_start) / elapsed);

            // Synthesize: TSC = crystal * numer / denom
            // Use 1 MHz crystal, numer = tsc_freq_mhz, denom = 1
            crystal = 1000000;
            numer = static_cast<uint32_t>(tsc_freq / 1000000);
            denom = 1;
            LOG_INFO("CPUID 0x15 (synthesized): crystal=%u Hz, TSC=%" PRIu64 " Hz", crystal, tsc_freq);
        }

        auto& o = cpuid_overrides[num_overrides++];
        o.Function = 0x15;
        o.Eax = denom;
        o.Ebx = numer;
        o.Ecx = crystal;
        o.Edx = 0;
    }

    // Override CPUID leaf 1 to mask features WHVP doesn't support:
    //   ECX bit  3: MONITOR/MWAIT  — causes #UD in WHVP
    //   ECX bit 24: TSC-Deadline   — WHVP xAPIC may not fire these
    // Also clear EBX[31:24] (Initial APIC ID) since the partition-level
    // override carries the host's APIC ID. Per-vCPU APIC ID patching
    // happens in the CPUID exit handler if WHVP triggers one; if not,
    // the guest reads the correct ID from the LAPIC register directly.
    int cpuid1[4]{};
    __cpuidex(cpuid1, 1, 0);
    {
        constexpr uint32_t kMaskOutEcx = (1u << 3) | (1u << 24);
        auto& o = cpuid_overrides[num_overrides++];
        o.Function = 1;
        o.Eax = static_cast<uint32_t>(cpuid1[0]);
        o.Ebx = static_cast<uint32_t>(cpuid1[1]) & 0x00FFFFFFu;
        o.Ecx = static_cast<uint32_t>(cpuid1[2]) & ~kMaskOutEcx;
        o.Edx = static_cast<uint32_t>(cpuid1[3]);
        LOG_INFO("CPUID 1 override: ECX 0x%08X -> 0x%08X (masked MWAIT+TSC-deadline)",
                 static_cast<uint32_t>(cpuid1[2]), o.Ecx);
    }

    if (num_overrides > 0) {
        hr = WHvSetPartitionProperty(vm->partition_,
            WHvPartitionPropertyCodeCpuidResultList,
            cpuid_overrides,
            num_overrides * sizeof(WHV_X64_CPUID_RESULT));
        if (FAILED(hr)) {
            LOG_WARN("CpuidResultList failed: 0x%08lX (non-fatal)", hr);
        }
    }

    // Force CPUID leaf 1 to exit so the per-vCPU handler can patch
    // EBX[31:24] with the correct Initial APIC ID for each vCPU.
    memset(&prop, 0, sizeof(prop));
    prop.ExtendedVmExits.X64CpuidExit = 1;
    hr = WHvSetPartitionProperty(vm->partition_,
        WHvPartitionPropertyCodeExtendedVmExits,
        &prop, sizeof(prop.ExtendedVmExits));
    if (FAILED(hr)) {
        LOG_WARN("Set ExtendedVmExits.CpuidExit failed: 0x%08lX (non-fatal)", hr);
    }

    UINT32 cpuid_exit_list[] = { 1, 0xB, 0x1F };
    hr = WHvSetPartitionProperty(vm->partition_,
        WHvPartitionPropertyCodeCpuidExitList,
        cpuid_exit_list, sizeof(cpuid_exit_list));
    if (FAILED(hr)) {
        LOG_WARN("CpuidExitList failed: 0x%08lX (non-fatal)", hr);
    }

    hr = WHvSetupPartition(vm->partition_);
    if (FAILED(hr)) {
        LOG_ERROR("WHvSetupPartition failed: 0x%08lX", hr);
        return nullptr;
    }

    LOG_INFO("WHVP partition created (cpus=%u)", cpu_count);
    return vm;
}

bool WhvpVm::MapMemory(GPA gpa, void* hva, uint64_t size, bool writable) {
    WHV_MAP_GPA_RANGE_FLAGS flags =
        WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagExecute;
    if (writable) flags |= WHvMapGpaRangeFlagWrite;

    HRESULT hr = WHvMapGpaRange(partition_, hva, gpa, size, flags);
    if (FAILED(hr)) {
        LOG_ERROR("WHvMapGpaRange(gpa=0x%" PRIX64 ", size=0x%" PRIX64 ") failed: 0x%08lX",
                  gpa, size, hr);
        return false;
    }
    return true;
}

bool WhvpVm::UnmapMemory(GPA gpa, uint64_t size) {
    HRESULT hr = WHvUnmapGpaRange(partition_, gpa, size);
    if (FAILED(hr)) {
        LOG_ERROR("WHvUnmapGpaRange failed: 0x%08lX", hr);
        return false;
    }
    return true;
}

std::unique_ptr<HypervisorVCpu> WhvpVm::CreateVCpu(
    uint32_t index, AddressSpace* addr_space) {
    return WhvpVCpu::Create(*this, index, addr_space);
}

void WhvpVm::RequestInterrupt(const InterruptRequest& req) {
    WHV_INTERRUPT_CONTROL ctrl{};
    ctrl.Type = WHvX64InterruptTypeFixed;
    ctrl.DestinationMode = req.logical_destination
        ? WHvX64InterruptDestinationModeLogical
        : WHvX64InterruptDestinationModePhysical;
    ctrl.TriggerMode = req.level_triggered
        ? WHvX64InterruptTriggerModeLevel
        : WHvX64InterruptTriggerModeEdge;
    ctrl.Destination = req.destination;
    ctrl.Vector = req.vector;

    WHvRequestInterrupt(partition_, &ctrl, sizeof(ctrl));
}

} // namespace whvp
