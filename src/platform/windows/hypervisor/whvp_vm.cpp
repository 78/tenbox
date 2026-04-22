#include "platform/windows/hypervisor/whvp_vm.h"
#include "platform/windows/hypervisor/whvp_vcpu.h"
#include "platform/windows/hypervisor/whvp_doorbell.h"
#include "platform/windows/hypervisor/whvp_dyn.h"
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
    // Resolve the optional WHPX exports (WHvRequestInterrupt and the
    // notification-port APIs). Required exports are statically linked and
    // are guaranteed to be present on any host where WinHvPlatform.dll
    // loaded at all.
    dyn::Load();

    auto vm = std::unique_ptr<WhvpVm>(new WhvpVm());
    vm->cpu_count_ = cpu_count;
    vm->vcpus_.assign(cpu_count, nullptr);

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

    // Debug override: TENBOX_SOFT_APIC=1 pretends we are running on a
    // pre-1809 build (no partition LAPIC + no WHvRequestInterrupt) so the
    // software fallback can be regression-tested on modern hosts.
    char env_buf[8]{};
    DWORD env_len = GetEnvironmentVariableA("TENBOX_SOFT_APIC", env_buf,
                                            sizeof(env_buf));
    const bool force_soft = (env_len > 0 && env_len < sizeof(env_buf) &&
                             (env_buf[0] == '1' || env_buf[0] == 'y' ||
                              env_buf[0] == 'Y'));

    // Try to enable the in-partition xAPIC emulation. On 1803 this property
    // is rejected; on 1809+ it succeeds. The result, combined with whether
    // WHvRequestInterrupt was exported, determines whether we drive the
    // in-partition APIC (hard path) or the software pending queue + VMCS-
    // equivalent injection (soft path) at runtime.
    bool lapic_emu_ok = false;
    if (!force_soft) {
        memset(&prop, 0, sizeof(prop));
        prop.LocalApicEmulationMode = WHvX64LocalApicEmulationModeXApic;
        hr = WHvSetPartitionProperty(vm->partition_,
            WHvPartitionPropertyCodeLocalApicEmulationMode,
            &prop, sizeof(prop.LocalApicEmulationMode));
        if (SUCCEEDED(hr)) {
            lapic_emu_ok = true;
        } else {
            LOG_WARN("Set APIC emulation failed: 0x%08lX (pre-1809 WHPX?)", hr);
        }
    } else {
        LOG_WARN("TENBOX_SOFT_APIC=1: skipping in-partition LAPIC emulation");
    }

    const bool has_request_irq = !force_soft && dyn::HasRequestInterrupt();
    // Fall back to the software path if EITHER the partition-level APIC
    // emulation is missing OR the interrupt-injection export is missing.
    // The two normally flip together (both absent on 1803, both present on
    // 1809+), but guard independently to be robust.
    vm->soft_apic_ = !lapic_emu_ok || !has_request_irq;

    LOG_INFO("WHPX APIC path: %s (lapic_emu=%s, WHvRequestInterrupt=%s)",
             vm->soft_apic_ ? "software (pending queue)" : "hardware xAPIC",
             lapic_emu_ok ? "yes" : "no",
             has_request_irq ? "yes" : "no");

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
    WHV_X64_CPUID_RESULT cpuid_overrides[10]{};
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

    // CPUID leaf-1 / leaf-7 mask strategy
    // -----------------------------------
    // We split the feature masks into two groups:
    //
    //   A) "Always mask" — features we intentionally hide from the guest on
    //      EVERY WHPX build, because we cannot or do not want to virtualise
    //      them regardless of host OS version. These are MWAIT (#UD on exit),
    //      x2APIC (we only expose xAPIC MMIO), TSC-Deadline (LAPIC timer goes
    //      through our software path), and the AVX-512 family (macOS HVF
    //      hides them too, avoids glibc hwcaps picking AVX-512 code paths).
    //
    //   B) "Soft-APIC only" — features that modern WHPX (1809+) virtualises
    //      just fine, so leaving them enabled is a free performance win on
    //      modern hosts; but WHPX 1803 does NOT implement them, and leaving
    //      them advertised makes Linux 6.12 issue MSRs / instructions that
    //      #UD or #GP, killing the kernel. Only mask these when we're on
    //      the software-APIC fallback path (pre-1809 WHPX or TENBOX_SOFT_APIC
    //      override). They include:
    //        - XSAVE/OSXSAVE/AVX/F16C/FMA/AESNI/PCLMULQDQ — 1803 leaves
    //          CR4.OSXSAVE=0 and returns zeros in CPUID 0xD, so user-space
    //          XGETBV #UDs and busybox init dies on Debian 6.12.
    //        - INVPCID — WHPX 1803 #UDs on the instruction itself; Linux
    //          picks it for native_flush_tlb_global → early exception 0x06
    //          at boot.
    //        - IBRS/IBPB/STIBP/SSBD/ARCH_CAPABILITIES/MD_CLEAR/... — 1803
    //          doesn't expose SPEC_CTRL (MSR 0x48) / PRED_CMD (MSR 0x49) /
    //          ARCH_CAPABILITIES (MSR 0x10A). Linux writes them from
    //          switch_mm_irqs_off unconditionally, triggering #GP that
    //          kills the idle task.
    //        - PKU/CET/UINTR/RDPID — CR4 bits / instructions 1803 doesn't
    //          model.
    //
    //   ECX.bit 31 Hypervisor-present is always set (leaf 1) so Linux
    //   probes leaf 0x40000000 and finds our Hyper-V signature.
    //
    //   EBX[31:24] (Initial APIC ID) is always cleared so the partition-
    //   wide static override doesn't leak the host's APIC ID; per-vCPU
    //   APIC ID patching happens in the CPUID exit handler.
    const bool soft = vm->soft_apic_;

    int cpuid1[4]{};
    __cpuidex(cpuid1, 1, 0);
    {
        // Group A: hide even on modern WHPX.
        constexpr uint32_t kMaskOutEcx_Always =
            (1u <<  3) |   // MONITOR/MWAIT
            (1u << 21) |   // x2APIC
            (1u << 24);    // TSC-Deadline
        // Group B: only hide on pre-1809 WHPX.
        constexpr uint32_t kMaskOutEcx_SoftOnly =
            (1u <<  1) |   // PCLMULQDQ
            (1u << 12) |   // FMA
            (1u << 25) |   // AESNI
            (1u << 26) |   // XSAVE
            (1u << 27) |   // OSXSAVE
            (1u << 28) |   // AVX
            (1u << 29);    // F16C
        constexpr uint32_t kSetEcx = (1u << 31);

        uint32_t mask_ecx = kMaskOutEcx_Always |
                            (soft ? kMaskOutEcx_SoftOnly : 0u);
        auto& o = cpuid_overrides[num_overrides++];
        o.Function = 1;
        o.Eax = static_cast<uint32_t>(cpuid1[0]);
        o.Ebx = static_cast<uint32_t>(cpuid1[1]) & 0x00FFFFFFu;
        o.Ecx = (static_cast<uint32_t>(cpuid1[2]) & ~mask_ecx) | kSetEcx;
        o.Edx = static_cast<uint32_t>(cpuid1[3]);
        LOG_INFO("CPUID 1 override: ECX 0x%08X -> 0x%08X "
                 "(%s, set HV_PRESENT)",
                 static_cast<uint32_t>(cpuid1[2]), o.Ecx,
                 soft
                    ? "masked MWAIT+x2APIC+TSC-deadline+XSAVE/OSXSAVE/AVX/"
                      "F16C/FMA/AESNI/PCLMULQDQ"
                    : "masked MWAIT+x2APIC+TSC-deadline");
    }

    int cpuid7[4]{};
    __cpuidex(cpuid7, 7, 0);
    {
        // Group A: AVX-512 family — always clear so glibc hwcaps never picks
        // an AVX-512 code path (performance neutral — WHPX 1809+ exposes
        // AVX-512 on newer Windows builds only, but we prefer to avoid the
        // guest-side code-path divergence).
        constexpr uint32_t kMaskOutEbx7_Always =
            (1u << 16) |   // AVX-512 Foundation
            (1u << 17) |   // AVX-512 DQ
            (1u << 21) |   // AVX-512 IFMA
            (1u << 28) |   // AVX-512 CD
            (1u << 30) |   // AVX-512 BW
            (1u << 31);    // AVX-512 VL
        constexpr uint32_t kMaskOutEcx7_Always =
            (1u <<  1) |   // AVX-512 VBMI
            (1u <<  6) |   // AVX-512 VBMI2
            (1u << 11) |   // AVX-512 VNNI
            (1u << 12) |   // AVX-512 BITALG
            (1u << 14);    // AVX-512 VPOPCNTDQ
        constexpr uint32_t kMaskOutEdx7_Always =
            (1u <<  2) |   // AVX-512 4VNNIW
            (1u <<  3) |   // AVX-512 4FMAPS
            (1u <<  8) |   // AVX-512 VP2INTERSECT
            // Spectre / side-channel mitigation MSRs. Empirically WHPX does
            // NOT virtualise MSR_IA32_SPEC_CTRL (0x48), PRED_CMD (0x49),
            // FLUSH_CMD (0x10B), ARCH_CAPABILITIES (0x10A), CORE_CAPABILITIES
            // (0xCF) on ANY Windows version we've tested (1803 through Win11
            // 22H2+). Linux 6.x writes SPEC_CTRL from common_interrupt_return
            // unconditionally when it thinks IBRS/IBPB is available, so the
            // first WRMSR 0x48 #GPs and kills PID 1 with exitcode 0x0B.
            // Keep these cleared on ALL paths — we can't rely on host-
            // specific behaviour here.
            (1u << 10) |   // MD_CLEAR
            (1u << 11) |   // RTM_ALWAYS_ABORT
            (1u << 13) |   // TSX_FORCE_ABORT
            (1u << 14) |   // SERIALIZE
            (1u << 18) |   // PCONFIG
            (1u << 26) |   // IBRS / IBPB   (MSR 0x48 / 0x49)
            (1u << 27) |   // STIBP         (MSR 0x48 bit 1)
            (1u << 28) |   // FLUSH_L1D     (MSR 0x10B)
            (1u << 29) |   // ARCH_CAPABILITIES (MSR 0x10A)
            (1u << 30) |   // CORE_CAPABILITIES (MSR 0xCF)
            (1u << 31);    // SSBD          (MSR 0x48 bit 2)

        // Group B: WHPX 1803 specific blow-ups.
        constexpr uint32_t kMaskOutEbx7_SoftOnly =
            (1u <<  5) |   // AVX2 (needs XSAVE)
            (1u << 10);    // INVPCID — instruction #UD on 1803
        constexpr uint32_t kMaskOutEcx7_SoftOnly =
            (1u <<  3) |   // PKU       — CR4.PKE not on 1803
            (1u <<  4) |   // OSPKE
            (1u <<  7) |   // CET_SS    — CR4.CET not on 1803
            (1u << 10) |   // VPCLMULQDQ (needs XSAVE)
            (1u << 22) |   // RDPID (#UD on 1803)
            (1u << 31);    // PKS
        constexpr uint32_t kMaskOutEdx7_SoftOnly =
            (1u <<  5) |   // UINTR       — CR4.UINTR not supported
            (1u << 20);    // CET_IBT

        uint32_t mask_ebx = kMaskOutEbx7_Always |
                            (soft ? kMaskOutEbx7_SoftOnly : 0u);
        uint32_t mask_ecx = kMaskOutEcx7_Always |
                            (soft ? kMaskOutEcx7_SoftOnly : 0u);
        uint32_t mask_edx = kMaskOutEdx7_Always |
                            (soft ? kMaskOutEdx7_SoftOnly : 0u);
        auto& o = cpuid_overrides[num_overrides++];
        o.Function = 7;
        o.Eax = static_cast<uint32_t>(cpuid7[0]);
        o.Ebx = static_cast<uint32_t>(cpuid7[1]) & ~mask_ebx;
        o.Ecx = static_cast<uint32_t>(cpuid7[2]) & ~mask_ecx;
        o.Edx = static_cast<uint32_t>(cpuid7[3]) & ~mask_edx;
        LOG_INFO("CPUID 7.0 override: EBX 0x%08X -> 0x%08X, "
                 "ECX 0x%08X -> 0x%08X, EDX 0x%08X -> 0x%08X (%s)",
                 static_cast<uint32_t>(cpuid7[1]), o.Ebx,
                 static_cast<uint32_t>(cpuid7[2]), o.Ecx,
                 static_cast<uint32_t>(cpuid7[3]), o.Edx,
                 soft
                    ? "masked AVX-512 + AVX2/INVPCID/PKU/CET/UINTR/RDPID + "
                      "IBRS/IBPB/STIBP/SSBD/ARCH_CAP (soft APIC)"
                    : "masked AVX-512 + IBRS/IBPB/STIBP/SSBD/ARCH_CAP "
                      "(hard APIC; WHPX does not virtualise SPEC_CTRL MSRs)");
    }

    // Note: CPUID leaf 7 subleaf 2 (SPEC_CTRL extended bits — IPRED_CTRL,
    // RRSBA_CTRL, BHI_CTRL, ...) cannot be overridden via CpuidResultList
    // because WHV_X64_CPUID_RESULT has no subleaf field and our leaf-1 entry
    // above matches ALL subleaves of leaf 7. Instead we add leaf 7 to the
    // CpuidExitList below and clear the 7.2 extended-SPEC_CTRL bits in the
    // exit handler. See whvp_vcpu.cpp, WHvRunVpExitReasonX64Cpuid.
    //
    // (The leaf-7 static override we set here still gets applied to the
    // subleaf-0 exit via exit_ctx.CpuidAccess.DefaultResult*, so subleaf 0
    // is still fast-pathed through the hypervisor's filter.)

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

    UINT32 cpuid_exit_list[] = { 1, 7, 0xB, 0x1F };
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
            LOG_INFO("WHPX doorbell: enabled (WHvCreateNotificationPort)");
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

void WhvpVm::OnVCpuCreated(uint32_t index, WhvpVCpu* vcpu) {
    std::lock_guard<std::mutex> lk(vcpu_mutex_);
    if (index < vcpus_.size()) {
        vcpus_[index] = vcpu;
    }
}

void WhvpVm::OnVCpuDestroyed(uint32_t index) {
    std::lock_guard<std::mutex> lk(vcpu_mutex_);
    if (index < vcpus_.size()) {
        vcpus_[index] = nullptr;
    }
}

void WhvpVm::RequestInterrupt(const InterruptRequest& req) {
    if (soft_apic_) {
        // Software path: fan out to the target vCPU(s) and yank them out of
        // WHvRunVirtualProcessor so TryInjectInterrupt on the next RunOnce
        // pass drains the freshly queued vector into WHvRegisterPending-
        // Interruption.
        std::lock_guard<std::mutex> lk(vcpu_mutex_);
        if (req.logical_destination) {
            // Flat-model logical destination: bitmask, bit N = APIC ID N.
            uint32_t mask = req.destination;
            for (uint32_t i = 0; i < cpu_count_; i++) {
                if ((mask & (1u << i)) && i < vcpus_.size() && vcpus_[i]) {
                    vcpus_[i]->QueueInterrupt(req.vector);
                    vcpus_[i]->CancelRun();
                }
            }
        } else {
            uint32_t dest = req.destination;
            if (dest >= cpu_count_) dest = 0;
            if (dest < vcpus_.size() && vcpus_[dest]) {
                vcpus_[dest]->QueueInterrupt(req.vector);
                vcpus_[dest]->CancelRun();
            }
        }
        return;
    }

    // Hard path (1809+): hand the interrupt off to WHPX's in-partition APIC.
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

    HRESULT hr = dyn::RequestInterrupt(partition_, &ctrl, sizeof(ctrl));
    if (FAILED(hr)) {
        LOG_WARN("WHvRequestInterrupt(vec=%u, dest=%u) failed: 0x%08lX",
                 req.vector, req.destination, hr);
    }
}

void WhvpVm::QueueInterrupt(uint32_t vector, uint32_t dest_vcpu) {
    if (soft_apic_) {
        std::lock_guard<std::mutex> lk(vcpu_mutex_);
        if (dest_vcpu < vcpus_.size() && vcpus_[dest_vcpu]) {
            vcpus_[dest_vcpu]->QueueInterrupt(vector);
            vcpus_[dest_vcpu]->CancelRun();
        }
        return;
    }

    // Hard path: single-vCPU physical unicast through WHvRequestInterrupt.
    InterruptRequest req{};
    req.vector = vector;
    req.destination = dest_vcpu;
    req.logical_destination = false;
    req.level_triggered = false;
    RequestInterrupt(req);
}

} // namespace whvp
