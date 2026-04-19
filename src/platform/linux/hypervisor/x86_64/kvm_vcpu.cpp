#include "platform/linux/hypervisor/x86_64/kvm_vcpu.h"
#include "platform/linux/hypervisor/x86_64/kvm_vm.h"
#include "platform/linux/hypervisor/kvm_platform.h"
#include "core/arch/x86_64/boot.h"
#include "core/device/irq/local_apic.h"
#include "core/vmm/types.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <linux/kvm.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace kvm {

// Signal used to kick a vCPU out of KVM_RUN. The handler does nothing —
// arriving in userspace with a pending signal is enough for KVM to bail out
// of the ioctl with KVM_EXIT_INTR or -EINTR.
static constexpr int kCancelSignal = SIGUSR1;

static void CancelSignalHandler(int /*sig*/) {
    // Intentionally empty; see above.
}

static void InstallCancelSignalHandler() {
    static bool installed = false;
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    if (installed) return;

    struct sigaction sa{};
    sa.sa_handler = CancelSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART: we want KVM_RUN to return EINTR
    ::sigaction(kCancelSignal, &sa, nullptr);
    installed = true;
}

KvmVCpu::~KvmVCpu() {
    if (run_) {
        ::munmap(run_, run_size_);
        run_ = nullptr;
    }
    if (vcpu_fd_ >= 0) {
        ::close(vcpu_fd_);
        vcpu_fd_ = -1;
    }
}

std::unique_ptr<KvmVCpu> KvmVCpu::Create(KvmVm& vm, uint32_t index,
                                         AddressSpace* addr_space) {
    auto vcpu = std::unique_ptr<KvmVCpu>(new KvmVCpu());
    vcpu->index_ = index;
    vcpu->addr_space_ = addr_space;

    vcpu->vcpu_fd_ = ::ioctl(vm.VmFd(), KVM_CREATE_VCPU, (unsigned long)index);
    if (vcpu->vcpu_fd_ < 0) {
        LOG_ERROR("kvm: KVM_CREATE_VCPU(%u) failed: %s", index, strerror(errno));
        return nullptr;
    }

    vcpu->run_size_ = vm.VcpuMmapSize();
    void* run = ::mmap(nullptr, vcpu->run_size_, PROT_READ | PROT_WRITE,
                       MAP_SHARED, vcpu->vcpu_fd_, 0);
    if (run == MAP_FAILED) {
        LOG_ERROR("kvm: mmap kvm_run for vCPU %u failed: %s",
                  index, strerror(errno));
        return nullptr;
    }
    vcpu->run_ = static_cast<struct kvm_run*>(run);

    if (!vcpu->SetupCpuid()) {
        return nullptr;
    }

    LOG_INFO("kvm: vCPU %u created", index);
    return vcpu;
}

bool KvmVCpu::SetupCpuid() {
    // We need the KVM top-level fd to query the supported CPUID list.
    int kvm_fd = GetKvmFd();
    if (kvm_fd < 0) return false;

    constexpr uint32_t kMaxEntries = 256;
    size_t bytes = sizeof(struct kvm_cpuid2) +
                   kMaxEntries * sizeof(struct kvm_cpuid_entry2);
    auto buf = std::unique_ptr<uint8_t[]>(new uint8_t[bytes]());
    auto* cpuid = reinterpret_cast<struct kvm_cpuid2*>(buf.get());
    cpuid->nent = kMaxEntries;

    if (::ioctl(kvm_fd, KVM_GET_SUPPORTED_CPUID, cpuid) < 0) {
        LOG_ERROR("kvm: KVM_GET_SUPPORTED_CPUID failed: %s", strerror(errno));
        return false;
    }

    for (uint32_t i = 0; i < cpuid->nent; i++) {
        auto& e = cpuid->entries[i];
        if (e.function == 1) {
            // Mask MONITOR/MWAIT (ECX bit 3) and TSC-deadline (ECX bit 24) to
            // stay in line with the userspace-LAPIC backends. Patch
            // EBX[31:24] with this vCPU's APIC ID (= index).
            constexpr uint32_t kMaskOutEcx = (1u << 3) | (1u << 24);
            e.ecx &= ~kMaskOutEcx;
            // Advertise that we are running under a hypervisor.  KVM does NOT
            // set this bit in KVM_GET_SUPPORTED_CPUID (it mirrors raw host
            // CPUID, where the bit is usually 0); userspace must OR it in.
            // Without it, Linux's init_hypervisor_platform() short-circuits
            // before scanning the 0x40000000 "KVMKVMKVM" signature, and none
            // of KVM's paravirt features (kvm-clock, PV-EOI, async PF, steal
            // time, PV UNHALT) get enabled.
            e.ecx |= (1u << 31);
            e.ebx = (e.ebx & 0x00FFFFFFu) | (index_ << 24);
        } else if (e.function == 0xB || e.function == 0x1F) {
            // x2APIC topology leaves: EDX = x2APIC ID.
            e.edx = index_;
        }
    }

    if (::ioctl(vcpu_fd_, KVM_SET_CPUID2, cpuid) < 0) {
        LOG_ERROR("kvm: KVM_SET_CPUID2 failed: %s", strerror(errno));
        return false;
    }
    return true;
}

void KvmVCpu::OnThreadInit() {
    LocalApic::SetCurrentCpu(index_);
    InstallCancelSignalHandler();

    // Unblock the cancel signal on this (vCPU worker) thread, in case it was
    // inherited-blocked.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, kCancelSignal);
    pthread_sigmask(SIG_UNBLOCK, &set, nullptr);

    thread_id_.store(static_cast<unsigned long>(pthread_self()),
                     std::memory_order_release);
}

bool KvmVCpu::SetupBootRegisters(uint8_t* ram) {
    using x86::GdtEntry;
    namespace Layout = x86::Layout;

    // Write GDT into guest memory.
    auto* gdt = reinterpret_cast<GdtEntry*>(ram + Layout::kGdtBase);
    gdt->null   = 0x0000000000000000ULL;
    gdt->unused = 0x0000000000000000ULL;
    gdt->code32 = 0x00CF9B000000FFFFULL;  // 32-bit code, flat, DPL0
    gdt->data32 = 0x00CF93000000FFFFULL;  // 32-bit data, flat, DPL0

    struct kvm_sregs sregs{};
    if (::ioctl(vcpu_fd_, KVM_GET_SREGS, &sregs) < 0) {
        LOG_ERROR("kvm: KVM_GET_SREGS failed: %s", strerror(errno));
        return false;
    }

    auto make_seg = [](uint16_t selector, uint32_t base, uint32_t limit,
                       uint8_t type, uint8_t s, uint8_t dpl, uint8_t present,
                       uint8_t db, uint8_t l, uint8_t g) {
        struct kvm_segment s_out{};
        s_out.base = base;
        s_out.limit = limit;
        s_out.selector = selector;
        s_out.type = type;
        s_out.present = present;
        s_out.dpl = dpl;
        s_out.db = db;
        s_out.s = s;
        s_out.l = l;
        s_out.g = g;
        s_out.avl = 0;
        s_out.unusable = 0;
        s_out.padding = 0;
        return s_out;
    };

    // CS selector 0x10: 32-bit code, flat
    sregs.cs = make_seg(0x10, 0, 0xFFFFFFFF, /*type=*/0xB, /*s=*/1, /*dpl=*/0,
                        /*present=*/1, /*db=*/1, /*l=*/0, /*g=*/1);
    // DS/ES/SS selector 0x18: 32-bit data, flat
    struct kvm_segment data = make_seg(0x18, 0, 0xFFFFFFFF, /*type=*/0x3, /*s=*/1,
                                       /*dpl=*/0, /*present=*/1, /*db=*/1,
                                       /*l=*/0, /*g=*/1);
    sregs.ds = data;
    sregs.es = data;
    sregs.ss = data;

    // FS / GS null
    struct kvm_segment null_seg{};
    null_seg.unusable = 1;
    sregs.fs = null_seg;
    sregs.gs = null_seg;

    // TR (required to be valid for VMX entry).
    sregs.tr = make_seg(0, 0, 0xFFFF, /*type=*/0xB, /*s=*/0, /*dpl=*/0,
                        /*present=*/1, /*db=*/0, /*l=*/0, /*g=*/0);
    // LDTR: unusable / present type=LDT
    sregs.ldt = make_seg(0, 0, 0xFFFF, /*type=*/0x2, /*s=*/0, /*dpl=*/0,
                         /*present=*/1, /*db=*/0, /*l=*/0, /*g=*/0);

    sregs.gdt.base = Layout::kGdtBase;
    sregs.gdt.limit = sizeof(GdtEntry) - 1;
    sregs.idt.base = 0;
    sregs.idt.limit = 0;

    // CR0 = PE | ET (protected mode, FPU present). Keep paging off.
    sregs.cr0 = (sregs.cr0 & ~0x80000001ULL) | 0x11;
    sregs.cr2 = 0;
    sregs.cr3 = 0;
    sregs.cr4 = sregs.cr4 & 0;  // clear PAE/etc. while we're in 32-bit mode
    sregs.efer = 0;

    if (::ioctl(vcpu_fd_, KVM_SET_SREGS, &sregs) < 0) {
        LOG_ERROR("kvm: KVM_SET_SREGS failed: %s", strerror(errno));
        return false;
    }

    struct kvm_regs regs{};
    regs.rip = Layout::kKernelBase;
    regs.rsi = Layout::kBootParams;
    regs.rflags = 0x2;
    if (::ioctl(vcpu_fd_, KVM_SET_REGS, &regs) < 0) {
        LOG_ERROR("kvm: KVM_SET_REGS failed: %s", strerror(errno));
        return false;
    }

    return true;
}

VCpuExitAction KvmVCpu::RunOnce() {
    int rc = ::ioctl(vcpu_fd_, KVM_RUN, 0);
    if (rc < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            // Signal or immediate_exit request.
            run_->immediate_exit = 0;
            return VCpuExitAction::kContinue;
        }
        LOG_ERROR("kvm: KVM_RUN failed: %s", strerror(errno));
        return VCpuExitAction::kError;
    }

    switch (run_->exit_reason) {
    case KVM_EXIT_IO: {
        auto& io = run_->io;
        uint8_t* base = reinterpret_cast<uint8_t*>(run_) + io.data_offset;
        for (uint32_t i = 0; i < io.count; i++) {
            uint8_t* buf = base + i * io.size;
            if (io.direction == KVM_EXIT_IO_OUT) {
                uint32_t val = 0;
                ::memcpy(&val, buf, io.size);
                addr_space_->HandlePortOut(io.port, io.size, val);
            } else {
                uint32_t val = 0;
                addr_space_->HandlePortIn(io.port, io.size, &val);
                ::memcpy(buf, &val, io.size);
            }
        }
        return VCpuExitAction::kContinue;
    }

    case KVM_EXIT_MMIO: {
        auto& mmio = run_->mmio;
        if (mmio.is_write) {
            uint64_t val = 0;
            ::memcpy(&val, mmio.data, mmio.len);
            addr_space_->HandleMmioWrite(mmio.phys_addr, mmio.len, val);
        } else {
            uint64_t val = 0;
            addr_space_->HandleMmioRead(mmio.phys_addr, mmio.len, &val);
            ::memcpy(mmio.data, &val, mmio.len);
        }
        return VCpuExitAction::kContinue;
    }

    case KVM_EXIT_HLT:
        return VCpuExitAction::kHalt;

    case KVM_EXIT_INTR:
        return VCpuExitAction::kContinue;

    case KVM_EXIT_SHUTDOWN:
        LOG_INFO("kvm: vCPU %u KVM_EXIT_SHUTDOWN", index_);
        return VCpuExitAction::kShutdown;

    case KVM_EXIT_FAIL_ENTRY:
        LOG_ERROR("kvm: KVM_EXIT_FAIL_ENTRY reason=0x%" PRIx64 " cpu=%u",
                  (uint64_t)run_->fail_entry.hardware_entry_failure_reason,
                  run_->fail_entry.cpu);
        return VCpuExitAction::kError;

    case KVM_EXIT_INTERNAL_ERROR:
        LOG_ERROR("kvm: KVM_EXIT_INTERNAL_ERROR suberror=%u",
                  run_->internal.suberror);
        return VCpuExitAction::kError;

    case KVM_EXIT_SYSTEM_EVENT:
        LOG_INFO("kvm: KVM_EXIT_SYSTEM_EVENT type=%u", run_->system_event.type);
        return VCpuExitAction::kShutdown;

    case KVM_EXIT_IRQ_WINDOW_OPEN:
    case KVM_EXIT_UNKNOWN:
        return VCpuExitAction::kContinue;

    default:
        LOG_WARN("kvm: unhandled exit reason %u", run_->exit_reason);
        return VCpuExitAction::kContinue;
    }
}

void KvmVCpu::CancelRun() {
    if (run_) {
        run_->immediate_exit = 1;
    }
    unsigned long tid = thread_id_.load(std::memory_order_acquire);
    if (tid) {
        ::pthread_kill(static_cast<pthread_t>(tid), kCancelSignal);
    }
}

bool KvmVCpu::WaitForInterrupt(uint32_t timeout_ms) {
    // With an in-kernel LAPIC, HLT is normally handled inside KVM and we do
    // not surface KVM_EXIT_HLT. If we do get here, just sleep briefly so the
    // run loop keeps responsive to CancelRun.
    if (timeout_ms == 0) timeout_ms = 1;
    ::usleep(static_cast<useconds_t>(timeout_ms) * 1000);
    return false;
}

} // namespace kvm
