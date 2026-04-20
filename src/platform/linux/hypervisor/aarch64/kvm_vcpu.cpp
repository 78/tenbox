#include "platform/linux/hypervisor/aarch64/kvm_vcpu.h"
#include "platform/linux/hypervisor/aarch64/kvm_vm.h"
#include "platform/linux/hypervisor/kvm_platform.h"
#include "core/vmm/types.h"

#include <cerrno>
#include <cstddef>
#include <csignal>
#include <cstring>
#include <linux/kvm.h>
#include <asm/kvm.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace kvm {

// Signal used to kick a vCPU out of KVM_RUN. Handler intentionally empty —
// arriving in userspace with a pending signal is enough for KVM_RUN to
// return with -EINTR / KVM_EXIT_INTR.
static constexpr int kCancelSignal = SIGUSR1;

static void CancelSignalHandler(int /*sig*/) {}

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

// Build a KVM_REG_ARM64 core register id from a field name inside struct
// kvm_regs (which starts with user_pt_regs "regs"). Offsets are expressed
// in 32-bit words per the KVM API convention.
static constexpr uint64_t CoreRegId(uint64_t byte_offset) {
    return KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
           (byte_offset / sizeof(uint32_t));
}

// Offsets within struct kvm_regs { user_pt_regs regs; ... } on aarch64.
// user_pt_regs = { u64 regs[31]; u64 sp; u64 pc; u64 pstate; }.
static constexpr uint64_t kOffX(uint32_t i) { return i * 8u; }
static constexpr uint64_t kOffSp     = 31u * 8u;          // 0xF8
static constexpr uint64_t kOffPc     = 32u * 8u;          // 0x100
static constexpr uint64_t kOffPstate = 33u * 8u;          // 0x108

// MPIDR_EL1 encoding for KVM_REG_ARM64 system register access.
//   op0=3 op1=0 CRn=0 CRm=0 op2=5
static constexpr uint64_t kSysRegMpidrEl1 = ARM64_SYS_REG(3, 0, 0, 0, 5);

static bool SetOneReg(int fd, uint64_t id, uint64_t value) {
    struct kvm_one_reg r{};
    r.id = id;
    r.addr = reinterpret_cast<uint64_t>(&value);
    return ::ioctl(fd, KVM_SET_ONE_REG, &r) == 0;
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
    vcpu->vm_ = &vm;
    vcpu->addr_space_ = addr_space;

    // arm64 KVM requires KVM_CREATE_VCPU / KVM_ARM_VCPU_INIT to be fully
    // serialised across vCPUs: once any vcpu has been INIT'd the kernel
    // returns -EBUSY on further KVM_CREATE_VCPU. Our vcpu worker threads
    // run in parallel, so guard the whole create + init sequence with a
    // process-wide mutex. (Single-process use of /dev/kvm is the norm for
    // this runtime, so a static lock is fine.)
    static std::mutex create_mutex;
    std::lock_guard<std::mutex> create_guard(create_mutex);

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

    // Secondary vCPUs start in POWER_OFF so the in-kernel PSCI layer blocks
    // them inside KVM_RUN until the BSP issues PSCI_CPU_ON.
    const bool power_off = (index != 0);
    if (!vcpu->InitVcpu(power_off)) {
        return nullptr;
    }

    // Program MPIDR_EL1 with a unique affinity value (Aff0 = index). KVM
    // defaults to derived affinity but being explicit matches QEMU/HVF.
    if (!SetOneReg(vcpu->vcpu_fd_, kSysRegMpidrEl1,
                   static_cast<uint64_t>(index) & 0xFFu)) {
        LOG_WARN("kvm: set MPIDR_EL1 for vCPU %u failed: %s",
                 index, strerror(errno));
    }

    LOG_INFO("kvm: aarch64 vCPU %u created (%s)",
             index, power_off ? "POWER_OFF" : "running");
    return vcpu;
}

bool KvmVCpu::InitVcpu(bool power_off) {
    // Query the preferred target from the host.
    struct kvm_vcpu_init init{};
    if (::ioctl(vm_->VmFd(), KVM_ARM_PREFERRED_TARGET, &init) < 0) {
        LOG_ERROR("kvm: KVM_ARM_PREFERRED_TARGET failed: %s", strerror(errno));
        return false;
    }

    // Enable in-kernel PSCI v0.2 handling. KVM will parse HVC PSCI calls
    // (CPU_ON / SYSTEM_OFF / SYSTEM_RESET) entirely in the kernel and expose
    // lifecycle events via KVM_EXIT_SYSTEM_EVENT.
    auto SetFeature = [&init](unsigned bit) {
        init.features[bit / 32] |= (1u << (bit % 32));
    };
    SetFeature(KVM_ARM_VCPU_PSCI_0_2);
    if (power_off) {
        SetFeature(KVM_ARM_VCPU_POWER_OFF);
    }

    if (::ioctl(vcpu_fd_, KVM_ARM_VCPU_INIT, &init) < 0) {
        LOG_ERROR("kvm: KVM_ARM_VCPU_INIT(%u) failed: %s",
                  index_, strerror(errno));
        return false;
    }
    return true;
}

void KvmVCpu::OnThreadInit() {
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

bool KvmVCpu::SetupBootRegisters(uint8_t* /*ram*/) {
    // aarch64 BSP boot state is configured through SetupAarch64Boot, which
    // receives the (entry_pc, fdt_addr) pair from Aarch64Machine.
    return true;
}

bool KvmVCpu::SetupAarch64Boot(uint64_t entry_pc, uint64_t fdt_addr) {
    // Finalize the in-kernel VGIC now — by the time the BSP reaches this
    // point all vCPUs have been created (Vm::Run waits for all vCPUs ready
    // before invoking FinalizeBoot, which calls us).
    if (!vm_->FinalizeVgicInit()) {
        return false;
    }

    // PSTATE: EL1h with D/A/I/F masked = 0x3C5, same as the HVF path.
    constexpr uint64_t kPstateEl1h = 0x3C5ULL;

    bool ok = true;
    ok &= SetOneReg(vcpu_fd_, CoreRegId(kOffPc),     entry_pc);
    ok &= SetOneReg(vcpu_fd_, CoreRegId(kOffX(0)),   fdt_addr);
    ok &= SetOneReg(vcpu_fd_, CoreRegId(kOffX(1)),   0);
    ok &= SetOneReg(vcpu_fd_, CoreRegId(kOffX(2)),   0);
    ok &= SetOneReg(vcpu_fd_, CoreRegId(kOffX(3)),   0);
    ok &= SetOneReg(vcpu_fd_, CoreRegId(kOffPstate), kPstateEl1h);
    if (!ok) {
        LOG_ERROR("kvm: vCPU %u SetupAarch64Boot: KVM_SET_ONE_REG failed: %s",
                  index_, strerror(errno));
        return false;
    }

    LOG_INFO("kvm: vCPU %u ARM64 boot: PC=0x%" PRIx64 ", X0(FDT)=0x%" PRIx64,
             index_, entry_pc, fdt_addr);
    return true;
}

VCpuExitAction KvmVCpu::RunOnce() {
    int rc = ::ioctl(vcpu_fd_, KVM_RUN, 0);
    if (rc < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            run_->immediate_exit = 0;
            return VCpuExitAction::kContinue;
        }
        LOG_ERROR("kvm: KVM_RUN(%u) failed: %s", index_, strerror(errno));
        return VCpuExitAction::kError;
    }

    switch (run_->exit_reason) {
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
        if (shutdown_cb_) shutdown_cb_();
        return VCpuExitAction::kShutdown;

    case KVM_EXIT_SYSTEM_EVENT: {
        uint32_t type = run_->system_event.type;
        LOG_INFO("kvm: vCPU %u KVM_EXIT_SYSTEM_EVENT type=%u", index_, type);
        // PSCI SYSTEM_OFF / SYSTEM_RESET / etc. are delivered here by the
        // in-kernel PSCI emulator. Translate to the generic Vm lifecycle
        // callbacks so RequestReboot() can actually recycle the VM.
        if (type == KVM_SYSTEM_EVENT_RESET) {
            if (reboot_cb_) reboot_cb_();
        } else {
            if (shutdown_cb_) shutdown_cb_();
        }
        return VCpuExitAction::kShutdown;
    }

    case KVM_EXIT_FAIL_ENTRY:
        LOG_ERROR("kvm: KVM_EXIT_FAIL_ENTRY reason=0x%" PRIx64 " cpu=%u",
                  (uint64_t)run_->fail_entry.hardware_entry_failure_reason,
                  run_->fail_entry.cpu);
        return VCpuExitAction::kError;

    case KVM_EXIT_INTERNAL_ERROR:
        LOG_ERROR("kvm: KVM_EXIT_INTERNAL_ERROR suberror=%u",
                  run_->internal.suberror);
        return VCpuExitAction::kError;

    case KVM_EXIT_UNKNOWN:
    case KVM_EXIT_IRQ_WINDOW_OPEN:
        return VCpuExitAction::kContinue;

    default:
        LOG_WARN("kvm: vCPU %u unhandled exit reason %u",
                 index_, run_->exit_reason);
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
    // With an in-kernel VGIC, WFI is normally handled inside KVM and we do
    // not surface KVM_EXIT_HLT. If we do get here, just sleep briefly so the
    // run loop keeps responsive to CancelRun.
    if (timeout_ms == 0) timeout_ms = 1;
    ::usleep(static_cast<useconds_t>(timeout_ms) * 1000);
    return false;
}

} // namespace kvm
