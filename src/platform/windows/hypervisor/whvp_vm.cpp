#include "platform/windows/hypervisor/whvp_vm.h"
#include "platform/windows/hypervisor/whvp_vcpu.h"
#include "platform/windows/hypervisor/whvp_doorbell.h"
#include <cinttypes>
#include <intrin.h>

namespace whvp {

WhvpVm::~WhvpVm() {
    doorbell_.reset();
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

    // Build CPUID override list:
    //   0x15            : TSC / Core Crystal Clock frequency
    //   0x01            : feature flags (HV_PRESENT, APIC ID, masked MWAIT)
    //   0x40000000..06  : Hyper-V enlightenment signature
    //                     (HYPERCALL + reference counter + VP_INDEX +
    //                      reference TSC page + frequency MSRs +
    //                      invariant-TSC control)
    WHV_X64_CPUID_RESULT cpuid_overrides[9]{};
    int num_overrides = 0;

    // CPUID 0x15: TSC / Core Crystal Clock frequency.
    // This allows the guest kernel to determine TSC speed without PIT calibration.
    {
        int cpuid15[4]{};
        __cpuid(cpuid15, 0x15);
        uint32_t denom   = static_cast<uint32_t>(cpuid15[0]);
        uint32_t numer   = static_cast<uint32_t>(cpuid15[1]);
        uint32_t crystal = static_cast<uint32_t>(cpuid15[2]);

        uint64_t tsc_freq_hz = 0;
        if (denom && numer) {
            // Intel CPU with native CPUID 0x15 support.
            if (crystal == 0) crystal = 38400000;  // 38.4 MHz for modern Intel
            tsc_freq_hz = static_cast<uint64_t>(crystal) * numer / denom;
            LOG_INFO("CPUID 0x15 (native): crystal=%u Hz, TSC=%" PRIu64 " Hz",
                     crystal, tsc_freq_hz);
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
            tsc_freq_hz = static_cast<uint64_t>((tsc_end - tsc_start) / elapsed);

            // Synthesize: TSC = crystal * numer / denom
            // Use 1 MHz crystal, numer = tsc_freq_mhz, denom = 1
            crystal = 1000000;
            numer = static_cast<uint32_t>(tsc_freq_hz / 1000000);
            denom = 1;
            LOG_INFO("CPUID 0x15 (synthesized): crystal=%u Hz, TSC=%" PRIu64 " Hz",
                     crystal, tsc_freq_hz);
        }

        // Publish the TSC frequency to the Hyper-V enlightenment layer so the
        // reference-TSC-page builder (whvp_vcpu.cpp) can derive TscScale.
        // Also used by MSR 0x40000022 (HV_X64_MSR_TSC_FREQUENCY).
        ::whvp::SetHypervTscFrequency(tsc_freq_hz);

        auto& o = cpuid_overrides[num_overrides++];
        o.Function = 0x15;
        o.Eax = denom;
        o.Ebx = numer;
        o.Ecx = crystal;
        o.Edx = 0;
    }

    // Override CPUID leaf 1 to mask features WHVP doesn't support and to
    // advertise hypervisor presence:
    //   ECX bit  3: MONITOR/MWAIT   — causes #UD in WHVP (clear)
    //   ECX bit 24: TSC-Deadline    — WHVP xAPIC may not fire these (clear)
    //   ECX bit 31: Hypervisor-present — required for Linux to probe leaf
    //               0x40000000 and find the Hyper-V signature (set)
    // Also clear EBX[31:24] (Initial APIC ID) since the partition-level
    // override carries the host's APIC ID. Per-vCPU APIC ID patching
    // happens in the CPUID exit handler if WHVP triggers one; if not,
    // the guest reads the correct ID from the LAPIC register directly.
    int cpuid1[4]{};
    __cpuidex(cpuid1, 1, 0);
    {
        constexpr uint32_t kMaskOutEcx = (1u << 3) | (1u << 24);
        constexpr uint32_t kSetEcx     = (1u << 31);
        auto& o = cpuid_overrides[num_overrides++];
        o.Function = 1;
        o.Eax = static_cast<uint32_t>(cpuid1[0]);
        o.Ebx = static_cast<uint32_t>(cpuid1[1]) & 0x00FFFFFFu;
        o.Ecx = (static_cast<uint32_t>(cpuid1[2]) & ~kMaskOutEcx) | kSetEcx;
        o.Edx = static_cast<uint32_t>(cpuid1[3]);
        LOG_INFO("CPUID 1 override: ECX 0x%08X -> 0x%08X "
                 "(masked MWAIT+TSC-deadline, set HV_PRESENT)",
                 static_cast<uint32_t>(cpuid1[2]), o.Ecx);
    }

    // Hyper-V enlightenment signature (leaves 0x40000000..0x40000006).
    // We advertise: signature + HYPERCALL + reference counter + VP_INDEX
    // + reference TSC page + TSC/APIC frequency MSRs + invariant-TSC control.
    // Not advertised: SynIC (EAX[2]), SynthTimers (EAX[3]), APIC_ACCESS (EAX[4]).
    {
        auto& o = cpuid_overrides[num_overrides++];
        o.Function = 0x40000000;
        o.Eax = 0x40000006;     // MaxHvLeaf
        o.Ebx = 0x7263694D;     // "Micr"
        o.Ecx = 0x666F736F;     // "osof"
        o.Edx = 0x76482074;     // "t Hv"
    }
    {
        auto& o = cpuid_overrides[num_overrides++];
        o.Function = 0x40000001;
        o.Eax = 0x31237648;     // "Hv#1" interface signature
        o.Ebx = 0;
        o.Ecx = 0;
        o.Edx = 0;
    }
    {
        // Hypervisor version — advertise Hyper-V 10.0 build 20348 (arbitrary).
        auto& o = cpuid_overrides[num_overrides++];
        o.Function = 0x40000002;
        o.Eax = 0x4F84;                 // Build number
        o.Ebx = (10u << 16) | 0u;       // Major.Minor
        o.Ecx = 0;                      // Service Pack
        o.Edx = 0;                      // Service Branch / Number
    }
    // Reserve CPUID 0x40000003 now; its EAX feature mask depends on whether
    // MSR exits actually work (Windows 10 1803 lacks X64MsrExit, in which
    // case we must not advertise enlightenment MSRs — the guest would #GP
    // trying to access them). We fill EAX after the MsrExit probe below.
    const int enlightenment_leaf3_idx = num_overrides;
    {
        auto& o = cpuid_overrides[num_overrides++];
        o.Function = 0x40000003;
        o.Eax = 0; o.Ebx = 0; o.Ecx = 0; o.Edx = 0;
    }
    {
        // Recommendations (hypercall-based optimizations). None enabled yet.
        auto& o = cpuid_overrides[num_overrides++];
        o.Function = 0x40000004;
        o.Eax = 0; o.Ebx = 0; o.Ecx = 0; o.Edx = 0;
    }
    {
        // Implementation hardware limits.
        auto& o = cpuid_overrides[num_overrides++];
        o.Function = 0x40000005;
        o.Eax = 0; o.Ebx = 0; o.Ecx = 0; o.Edx = 0;
    }
    {
        // Implementation hardware features.
        auto& o = cpuid_overrides[num_overrides++];
        o.Function = 0x40000006;
        o.Eax = 0; o.Ebx = 0; o.Ecx = 0; o.Edx = 0;
    }

    // Probe whether this WHPX build supports routing Hyper-V MSRs to the
    // VMM. X64MsrExit is Windows 10 1809+; X64MsrExitBitmap is 1809+ too.
    // On 1803 both SetPartitionProperty calls return WHV_E_INVALID_PARTITION_CONFIG
    // or similar. We try twice: first requesting CpuidExit+MsrExit, and if
    // that fails, retry with just CpuidExit (which 1803 supports).
    bool msr_exit_enabled = false;
    memset(&prop, 0, sizeof(prop));
    prop.ExtendedVmExits.X64CpuidExit = 1;
    prop.ExtendedVmExits.X64MsrExit   = 1;
    hr = WHvSetPartitionProperty(vm->partition_,
        WHvPartitionPropertyCodeExtendedVmExits,
        &prop, sizeof(prop.ExtendedVmExits));
    if (SUCCEEDED(hr)) {
        msr_exit_enabled = true;
    } else {
        LOG_WARN("ExtendedVmExits.X64MsrExit not supported: 0x%08lX "
                 "(pre-1809 WHPX?); retrying with CpuidExit only", hr);
        memset(&prop, 0, sizeof(prop));
        prop.ExtendedVmExits.X64CpuidExit = 1;
        hr = WHvSetPartitionProperty(vm->partition_,
            WHvPartitionPropertyCodeExtendedVmExits,
            &prop, sizeof(prop.ExtendedVmExits));
        if (FAILED(hr)) {
            LOG_WARN("Set ExtendedVmExits.X64CpuidExit failed: 0x%08lX "
                     "(APIC ID per-vCPU patching will be skipped)", hr);
        }
    }

    // If MsrExit is on, also route the full synthetic MSR range (0x40000000+)
    // that WHPX doesn't natively emulate, so the guest doesn't #GP. This
    // property was added alongside X64MsrExit in 1809; skip it on 1803.
    if (msr_exit_enabled) {
        WHV_X64_MSR_EXIT_BITMAP msr_bitmap{};
        msr_bitmap.UnhandledMsrs = 1;
        hr = WHvSetPartitionProperty(vm->partition_,
            WHvPartitionPropertyCodeX64MsrExitBitmap,
            &msr_bitmap, sizeof(msr_bitmap));
        if (FAILED(hr)) {
            LOG_WARN("Set X64MsrExitBitmap failed: 0x%08lX "
                     "(disabling Hyper-V enlightenment MSRs)", hr);
            msr_exit_enabled = false;
        } else {
            LOG_INFO("WHPX MSR exit bitmap: UnhandledMsrs=1");
        }
    }

    // Now that we know whether enlightenment MSRs will actually reach our
    // handler, fill CPUID 0x40000003 EAX:
    //   EAX[1]  = PARTITION_REFERENCE_COUNTER_AVAILABLE (MSR 0x40000020)
    //   EAX[5]  = HYPERCALL_MSRS_AVAILABLE              (MSR 0x40000000/01)
    //   EAX[6]  = VP_INDEX_AVAILABLE                    (MSR 0x40000002)
    //   EAX[8]  = FREQUENCY_MSRS_AVAILABLE              (MSR 0x40000022/23)
    //   EAX[9]  = PARTITION_REFERENCE_TSC_AVAILABLE     (MSR 0x40000021)
    //   EAX[15] = ACCESS_TSC_INVARIANT_CONTROLS         (MSR 0x40000118)
    //
    // TSC page (EAX[9]) lets Linux switch to hyperv_clocksource_tsc_page:
    // gettimeofday/clock_gettime read a guest-local page and apply
    // (tsc * scale) >> 64 + offset — zero VM exits per time read.
    // EAX[15] avoids "tsc: Marking TSC unstable due to running on Hyper-V"
    // so sched_clock stays on TSC instead of falling back to jiffies.
    //
    // On 1803 (no MsrExit) we advertise only the signature + HV_PRESENT so
    // Linux still sees "Hypervisor detected: Microsoft Hyper-V" but doesn't
    // try to touch any synthetic MSR (all of which would #GP without our
    // handler). That mirrors the behaviour of Hyper-V-on-1803 itself.
    if (msr_exit_enabled) {
        auto& o = cpuid_overrides[enlightenment_leaf3_idx];
        o.Eax = (1u << 1) | (1u << 5) | (1u << 6) |
                (1u << 8) | (1u << 9) | (1u << 15);
        LOG_INFO("Hyper-V enlightenment: HYPERCALL + reference counter + "
                 "VP_INDEX + reference TSC page + frequency MSRs + "
                 "invariant-TSC control");
    } else {
        LOG_INFO("Hyper-V enlightenment: signature only "
                 "(pre-1809 WHPX or MsrExit unavailable)");
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

    {
        auto db = std::make_unique<WhvpDoorbellRegistrar>(vm->partition_);
        if (db->Available()) {
            LOG_INFO("WHPX doorbell: enabled");
            vm->doorbell_ = std::move(db);
        } else {
            LOG_INFO("WHPX doorbell: unavailable (virtio kicks use MMIO exit path)");
        }
    }

    LOG_INFO("WHVP partition created (cpus=%u)", cpu_count);
    return vm;
}

bool WhvpVm::RegisterQueueDoorbell(uint64_t mmio_addr, uint32_t len, uint32_t datamatch,
                                   std::function<void()> cb) {
    if (!doorbell_) return false;
    return doorbell_->Register(mmio_addr, len, datamatch, std::move(cb));
}

void WhvpVm::UnregisterAllQueueDoorbells() { doorbell_.reset(); }

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
    // Remember the HVA backing so the Hyper-V overlay helpers in
    // whvp_vcpu.cpp can bypass WHvWriteGpaRange and memcpy directly into
    // the guest RAM page (e.g. for the reference TSC and hypercall pages).
    ::whvp::RegisterRamMapping(static_cast<uint64_t>(gpa), hva, size);
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
