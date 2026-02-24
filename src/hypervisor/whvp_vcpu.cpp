#include "hypervisor/whvp_vcpu.h"

namespace whvp {

WhvpVCpu::~WhvpVCpu() {
    if (emulator_) {
        WHvEmulatorDestroyEmulator(emulator_);
        emulator_ = nullptr;
    }
    if (partition_) {
        WHvDeleteVirtualProcessor(partition_, vp_index_);
    }
}

std::unique_ptr<WhvpVCpu> WhvpVCpu::Create(WhvpVm& vm, uint32_t vp_index,
                                             AddressSpace* addr_space) {
    auto vcpu = std::unique_ptr<WhvpVCpu>(new WhvpVCpu());
    vcpu->partition_ = vm.Handle();
    vcpu->vp_index_ = vp_index;
    vcpu->addr_space_ = addr_space;

    HRESULT hr = WHvCreateVirtualProcessor(vm.Handle(), vp_index, 0);
    if (FAILED(hr)) {
        LOG_ERROR("WHvCreateVirtualProcessor(%u) failed: 0x%08lX", vp_index, hr);
        return nullptr;
    }

    if (!vcpu->CreateEmulator()) {
        return nullptr;
    }

    LOG_INFO("vCPU %u created", vp_index);
    return vcpu;
}

bool WhvpVCpu::CreateEmulator() {
    WHV_EMULATOR_CALLBACKS cb{};
    cb.Size = sizeof(cb);
    cb.WHvEmulatorIoPortCallback = OnIoPort;
    cb.WHvEmulatorMemoryCallback = OnMemory;
    cb.WHvEmulatorGetVirtualProcessorRegisters = OnGetRegisters;
    cb.WHvEmulatorSetVirtualProcessorRegisters = OnSetRegisters;
    cb.WHvEmulatorTranslateGvaPage = OnTranslateGva;

    HRESULT hr = WHvEmulatorCreateEmulator(&cb, &emulator_);
    if (FAILED(hr)) {
        LOG_ERROR("WHvEmulatorCreateEmulator failed: 0x%08lX", hr);
        return false;
    }
    return true;
}

bool WhvpVCpu::SetRegisters(const WHV_REGISTER_NAME* names,
                             const WHV_REGISTER_VALUE* values, uint32_t count) {
    HRESULT hr = WHvSetVirtualProcessorRegisters(
        partition_, vp_index_, names, count, values);
    if (FAILED(hr)) {
        LOG_ERROR("WHvSetVirtualProcessorRegisters failed: 0x%08lX", hr);
        return false;
    }
    return true;
}

bool WhvpVCpu::GetRegisters(const WHV_REGISTER_NAME* names,
                             WHV_REGISTER_VALUE* values, uint32_t count) {
    HRESULT hr = WHvGetVirtualProcessorRegisters(
        partition_, vp_index_, names, count, values);
    if (FAILED(hr)) {
        LOG_ERROR("WHvGetVirtualProcessorRegisters failed: 0x%08lX", hr);
        return false;
    }
    return true;
}

VCpuExitAction WhvpVCpu::RunOnce() {
    WHV_RUN_VP_EXIT_CONTEXT exit_ctx{};
    HRESULT hr = WHvRunVirtualProcessor(
        partition_, vp_index_, &exit_ctx, sizeof(exit_ctx));
    if (FAILED(hr)) {
        LOG_ERROR("WHvRunVirtualProcessor failed: 0x%08lX", hr);
        return VCpuExitAction::kError;
    }

    switch (exit_ctx.ExitReason) {
    case WHvRunVpExitReasonX64IoPortAccess:
        return HandleIoPort(exit_ctx.VpContext, exit_ctx.IoPortAccess);

    case WHvRunVpExitReasonMemoryAccess:
        return HandleMmio(exit_ctx.VpContext, exit_ctx.MemoryAccess);

    case WHvRunVpExitReasonX64Halt: {
        WHV_REGISTER_NAME rfl_name = WHvX64RegisterRflags;
        WHV_REGISTER_VALUE rfl_val{};
        GetRegisters(&rfl_name, &rfl_val, 1);
        if (!(rfl_val.Reg64 & 0x200)) {
            LOG_INFO("CPU halted with interrupts disabled — treating as shutdown");
            return VCpuExitAction::kShutdown;
        }
        return VCpuExitAction::kHalt;
    }

    case WHvRunVpExitReasonCanceled:
        return VCpuExitAction::kContinue;

    case WHvRunVpExitReasonX64ApicEoi:
        return VCpuExitAction::kContinue;

    case WHvRunVpExitReasonUnsupportedFeature:
        LOG_WARN("Unsupported feature at RIP=0x%llX (feature=%u)",
                 exit_ctx.VpContext.Rip,
                 exit_ctx.UnsupportedFeature.FeatureCode);
        return VCpuExitAction::kContinue;

    case WHvRunVpExitReasonX64InterruptWindow:
        return VCpuExitAction::kContinue;

    case WHvRunVpExitReasonUnrecoverableException:
        LOG_ERROR("Unrecoverable guest exception at RIP=0x%llX",
                  exit_ctx.VpContext.Rip);
        return VCpuExitAction::kError;

    case WHvRunVpExitReasonInvalidVpRegisterValue:
        LOG_ERROR("Invalid VP register value at RIP=0x%llX",
                  exit_ctx.VpContext.Rip);
        return VCpuExitAction::kError;

    case WHvRunVpExitReasonX64Cpuid: {
        auto& cpuid = exit_ctx.CpuidAccess;
        WHV_REGISTER_NAME names[] = {
            WHvX64RegisterRax, WHvX64RegisterRbx,
            WHvX64RegisterRcx, WHvX64RegisterRdx,
            WHvX64RegisterRip,
        };
        WHV_REGISTER_VALUE vals[5]{};
        vals[0].Reg64 = cpuid.DefaultResultRax;
        vals[1].Reg64 = cpuid.DefaultResultRbx;
        vals[2].Reg64 = cpuid.DefaultResultRcx;
        vals[3].Reg64 = cpuid.DefaultResultRdx;
        vals[4].Reg64 = exit_ctx.VpContext.Rip +
                         exit_ctx.VpContext.InstructionLength;
        SetRegisters(names, vals, 5);
        return VCpuExitAction::kContinue;
    }

    case WHvRunVpExitReasonX64MsrAccess: {
        auto& msr = exit_ctx.MsrAccess;
        WHV_REGISTER_NAME rip_name = WHvX64RegisterRip;
        WHV_REGISTER_VALUE rip_val{};
        rip_val.Reg64 = exit_ctx.VpContext.Rip +
                        exit_ctx.VpContext.InstructionLength;

        if (!msr.AccessInfo.IsWrite) {
            WHV_REGISTER_NAME names[] = {
                WHvX64RegisterRax, WHvX64RegisterRdx, WHvX64RegisterRip
            };
            WHV_REGISTER_VALUE vals[3]{};
            vals[0].Reg64 = 0;
            vals[1].Reg64 = 0;
            vals[2] = rip_val;
            SetRegisters(names, vals, 3);
            LOG_DEBUG("MSR read: 0x%X -> 0", msr.MsrNumber);
        } else {
            LOG_DEBUG("MSR write: 0x%X = 0x%llX", msr.MsrNumber,
                      (msr.Rdx << 32) | (msr.Rax & 0xFFFFFFFF));
            SetRegisters(&rip_name, &rip_val, 1);
        }
        return VCpuExitAction::kContinue;
    }

    default:
        LOG_WARN("Unhandled VM exit reason: 0x%X at RIP=0x%llX",
                 exit_ctx.ExitReason, exit_ctx.VpContext.Rip);
        return VCpuExitAction::kError;
    }
}

VCpuExitAction WhvpVCpu::HandleIoPort(
    const WHV_VP_EXIT_CONTEXT& vp_ctx,
    const WHV_X64_IO_PORT_ACCESS_CONTEXT& io) {

    WHV_EMULATOR_STATUS status{};
    HRESULT hr = WHvEmulatorTryIoEmulation(
        emulator_, this, &vp_ctx, &io, &status);

    if (FAILED(hr) || !status.EmulationSuccessful) {
        LOG_WARN("IO emulation failed: port=0x%X hr=0x%08lX success=%d",
                 io.PortNumber, hr, status.EmulationSuccessful);
        WHV_REGISTER_NAME name = WHvX64RegisterRip;
        WHV_REGISTER_VALUE val{};
        val.Reg64 = vp_ctx.Rip + vp_ctx.InstructionLength;
        SetRegisters(&name, &val, 1);
    }
    return VCpuExitAction::kContinue;
}

VCpuExitAction WhvpVCpu::HandleMmio(
    const WHV_VP_EXIT_CONTEXT& vp_ctx,
    const WHV_MEMORY_ACCESS_CONTEXT& mem) {

    WHV_EMULATOR_STATUS status{};
    HRESULT hr = WHvEmulatorTryMmioEmulation(
        emulator_, this, &vp_ctx, &mem, &status);

    if (FAILED(hr) || !status.EmulationSuccessful) {
        LOG_WARN("MMIO emulation failed: gpa=0x%llX hr=0x%08lX success=%d",
                 mem.Gpa, hr, status.EmulationSuccessful);
        WHV_REGISTER_NAME name = WHvX64RegisterRip;
        WHV_REGISTER_VALUE val{};
        val.Reg64 = vp_ctx.Rip + vp_ctx.InstructionLength;
        SetRegisters(&name, &val, 1);
    }
    return VCpuExitAction::kContinue;
}

// --- Emulator Callbacks ---

HRESULT CALLBACK WhvpVCpu::OnIoPort(
    VOID* ctx, WHV_EMULATOR_IO_ACCESS_INFO* io) {
    auto* vcpu = static_cast<WhvpVCpu*>(ctx);
    if (io->Direction == 0) {
        uint32_t val = 0;
        vcpu->addr_space_->HandlePortIn(io->Port, (uint8_t)io->AccessSize, &val);
        io->Data = val;
    } else {
        vcpu->addr_space_->HandlePortOut(
            io->Port, (uint8_t)io->AccessSize, io->Data);
    }
    return S_OK;
}

HRESULT CALLBACK WhvpVCpu::OnMemory(
    VOID* ctx, WHV_EMULATOR_MEMORY_ACCESS_INFO* mem) {
    auto* vcpu = static_cast<WhvpVCpu*>(ctx);
    if (mem->Direction == 0) {
        uint64_t val = 0;
        vcpu->addr_space_->HandleMmioRead(
            mem->GpaAddress, mem->AccessSize, &val);
        memcpy(mem->Data, &val, mem->AccessSize);
    } else {
        uint64_t val = 0;
        memcpy(&val, mem->Data, mem->AccessSize);
        vcpu->addr_space_->HandleMmioWrite(
            mem->GpaAddress, mem->AccessSize, val);
    }
    return S_OK;
}

HRESULT CALLBACK WhvpVCpu::OnGetRegisters(
    VOID* ctx, const WHV_REGISTER_NAME* names,
    UINT32 count, WHV_REGISTER_VALUE* values) {
    auto* vcpu = static_cast<WhvpVCpu*>(ctx);
    return WHvGetVirtualProcessorRegisters(
        vcpu->partition_, vcpu->vp_index_, names, count, values);
}

HRESULT CALLBACK WhvpVCpu::OnSetRegisters(
    VOID* ctx, const WHV_REGISTER_NAME* names,
    UINT32 count, const WHV_REGISTER_VALUE* values) {
    auto* vcpu = static_cast<WhvpVCpu*>(ctx);
    return WHvSetVirtualProcessorRegisters(
        vcpu->partition_, vcpu->vp_index_, names, count, values);
}

HRESULT CALLBACK WhvpVCpu::OnTranslateGva(
    VOID* ctx, WHV_GUEST_VIRTUAL_ADDRESS gva,
    WHV_TRANSLATE_GVA_FLAGS flags,
    WHV_TRANSLATE_GVA_RESULT_CODE* result_code,
    WHV_GUEST_PHYSICAL_ADDRESS* gpa) {
    auto* vcpu = static_cast<WhvpVCpu*>(ctx);
    WHV_TRANSLATE_GVA_RESULT result{};
    HRESULT hr = WHvTranslateGva(
        vcpu->partition_, vcpu->vp_index_,
        gva, flags, &result, gpa);
    if (SUCCEEDED(hr)) {
        *result_code = result.ResultCode;
    }
    return hr;
}

} // namespace whvp
