#include "platform/macos/hypervisor/aarch64/hvf_vm.h"
#include "platform/macos/hypervisor/aarch64/hvf_vcpu.h"
#include "core/device/irq/gicv3.h"
#include "core/vmm/types.h"
#include <cinttypes>

#include <Hypervisor/Hypervisor.h>
#include <cstdlib>

namespace hvf {

// GICv3 address layout (following QEMU virt convention)
static constexpr uint64_t kGicDistBase   = 0x08000000;
static constexpr uint64_t kGicRedistBase = 0x080A0000;
static constexpr uint64_t kGicMsiBase    = 0x08080000;

HvfVm::~HvfVm() {
    if (vm_created_) {
        hv_vm_destroy();
    }
}

static bool CreateVmWithHwGic(HvfVm* vm, uint32_t cpu_count,
                               uint64_t* out_redist_base,
                               size_t* out_redist_per_cpu) {
    if (__builtin_available(macOS 15.0, *)) {
        size_t dist_size = 0, dist_align = 0;
        size_t redist_region_size = 0, redist_size = 0, redist_align = 0;
        size_t msi_sz = 0, msi_align = 0;
        uint32_t spi_base = 0, spi_count = 0;
        hv_gic_get_distributor_size(&dist_size);
        hv_gic_get_distributor_base_alignment(&dist_align);
        hv_gic_get_redistributor_region_size(&redist_region_size);
        hv_gic_get_redistributor_size(&redist_size);
        hv_gic_get_redistributor_base_alignment(&redist_align);
        hv_gic_get_msi_region_size(&msi_sz);
        hv_gic_get_msi_region_base_alignment(&msi_align);
        hv_gic_get_spi_interrupt_range(&spi_base, &spi_count);
        LOG_INFO("hvf GIC params: dist_size=0x%zx dist_align=0x%zx "
                 "redist_region=0x%zx redist_per_cpu=0x%zx redist_align=0x%zx "
                 "msi_size=0x%zx msi_align=0x%zx spi_base=%u spi_count=%u",
                 dist_size, dist_align,
                 redist_region_size, redist_size, redist_align,
                 msi_sz, msi_align, spi_base, spi_count);

        hv_gic_config_t gic_config = hv_gic_config_create();
        if (!gic_config) {
            LOG_ERROR("hvf: failed to create GIC config");
            return false;
        }

        if (dist_size == 0) dist_size = 0x10000;
        size_t redist_per_cpu = redist_size;
        if (redist_per_cpu == 0) redist_per_cpu = 0x20000;
        if (redist_align == 0) redist_align = 0x10000;

        uint64_t actual_redist_base = kGicDistBase + dist_size;
        if (redist_align > 0 && actual_redist_base % redist_align != 0) {
            actual_redist_base = (actual_redist_base + redist_align - 1) & ~(redist_align - 1);
        }

        LOG_INFO("hvf: GIC layout: dist=0x%" PRIx64 "[0x%zx] redist=0x%" PRIx64
                 "[region=0x%zx per_cpu=0x%zx * %u]",
                 (uint64_t)kGicDistBase, dist_size,
                 (uint64_t)actual_redist_base, redist_region_size,
                 redist_per_cpu, cpu_count);

        hv_return_t ret;
        ret = hv_gic_config_set_distributor_base(gic_config, kGicDistBase);
        if (ret != HV_SUCCESS) {
            LOG_ERROR("hvf: set distributor base failed: %d", (int)ret);
        }
        ret = hv_gic_config_set_redistributor_base(gic_config, actual_redist_base);
        if (ret != HV_SUCCESS) {
            LOG_ERROR("hvf: set redistributor base failed: %d", (int)ret);
        }

        size_t msi_size = 0;
        hv_gic_get_msi_region_size(&msi_size);
        if (msi_size > 0) {
            uint64_t msi_base = actual_redist_base + redist_region_size;
            size_t msi_align_val = 0;
            hv_gic_get_msi_region_base_alignment(&msi_align_val);
            if (msi_align_val > 0 && msi_base % msi_align_val != 0) {
                msi_base = (msi_base + msi_align_val - 1) & ~(msi_align_val - 1);
            }
            LOG_INFO("hvf: GIC MSI at 0x%" PRIx64 "[0x%zx]", (uint64_t)msi_base, msi_size);
            hv_gic_config_set_msi_region_base(gic_config, msi_base);
            hv_gic_config_set_msi_interrupt_range(gic_config, 64, 64);
        }

        hv_vm_config_t vm_config = hv_vm_config_create();
        if (!vm_config) {
            LOG_ERROR("hvf: failed to create VM config");
            return false;
        }

        ret = hv_vm_create(vm_config);
        if (ret != HV_SUCCESS) {
            LOG_ERROR("hvf: hv_vm_create failed: %d", (int)ret);
            return false;
        }

        ret = hv_gic_create(gic_config);
        if (ret != HV_SUCCESS) {
            LOG_ERROR("hvf: hv_gic_create failed: %d", (int)ret);
            return false;
        }

        *out_redist_base = actual_redist_base;
        *out_redist_per_cpu = redist_per_cpu;
        return true;
    }
    return false;
}

static bool ShouldForceSoftGic() {
    const char* val = std::getenv("TENBOX_FORCE_SOFT_GIC");
    return val && val[0] == '1';
}

std::unique_ptr<HvfVm> HvfVm::Create(uint32_t cpu_count) {
    auto vm = std::unique_ptr<HvfVm>(new HvfVm());
    vm->cpu_count_ = cpu_count;

    uint64_t redist_base = 0;
    size_t redist_per_cpu = 0;

    bool force_soft = ShouldForceSoftGic();
    if (force_soft) {
        LOG_INFO("hvf: TENBOX_FORCE_SOFT_GIC=1, skipping hardware GIC");
    }

    if (!force_soft &&
        CreateVmWithHwGic(vm.get(), cpu_count, &redist_base, &redist_per_cpu)) {
        vm->vm_created_ = true;
        vm->gic_created_ = true;
        vm->redist_base_ = redist_base;
        vm->redist_size_per_cpu_ = redist_per_cpu;
        LOG_INFO("hvf: VM created with hardware GICv3 (%u vCPUs)", cpu_count);
    } else {
        // Fallback: create VM without HVF GIC, use software GICv3 emulation.
        LOG_INFO("hvf: hardware GICv3 unavailable, using software GICv3 emulation");

        hv_return_t ret = hv_vm_create(NULL);
        if (ret != HV_SUCCESS) {
            LOG_ERROR("hvf: hv_vm_create(NULL) failed (soft-gic path): %d", (int)ret);
            return nullptr;
        }
        vm->vm_created_ = true;

        vm->soft_gic_ = std::make_unique<gicv3::SoftGic>(cpu_count);
        vm->redist_base_ = kGicRedistBase;
        vm->redist_size_per_cpu_ = gicv3::kGicrSizePerCpu;

        LOG_INFO("hvf: VM created with software GICv3 (%u vCPUs)", cpu_count);
    }

    return vm;
}

bool HvfVm::MapMemory(GPA gpa, void* hva, uint64_t size, bool writable) {
    hv_memory_flags_t flags = HV_MEMORY_READ | HV_MEMORY_EXEC;
    if (writable) flags |= HV_MEMORY_WRITE;

    hv_return_t ret = hv_vm_map(hva, gpa, size, flags);
    if (ret != HV_SUCCESS) {
        LOG_ERROR("hvf: hv_vm_map(GPA=0x%" PRIx64 ", size=0x%" PRIx64 ") failed: %d",
                  gpa, size, (int)ret);
        return false;
    }
    return true;
}

bool HvfVm::UnmapMemory(GPA gpa, uint64_t size) {
    hv_return_t ret = hv_vm_unmap(gpa, size);
    if (ret != HV_SUCCESS) {
        LOG_ERROR("hvf: hv_vm_unmap(GPA=0x%" PRIx64 ") failed: %d",
                  gpa, (int)ret);
        return false;
    }
    return true;
}

std::unique_ptr<HypervisorVCpu> HvfVm::CreateVCpu(
    uint32_t index, AddressSpace* addr_space) {
    auto vcpu = HvfVCpu::Create(index, addr_space, UsesSoftGic());
    if (vcpu && soft_gic_) {
        vcpu->SetSoftGic(soft_gic_.get());
        hv_vcpu_t handle = vcpu->GetVCpuHandle();
        HvfVCpu* raw_vcpu = vcpu.get();
        soft_gic_->RegisterVCpuKick(index, [handle, raw_vcpu](bool assert) {
            raw_vcpu->SetIrqPending(assert);
            if (assert) {
                raw_vcpu->WakeFromHalt();
                hv_vcpu_t h = handle;
                hv_vcpus_exit(&h, 1);
            }
        });
    }
    return vcpu;
}

void HvfVm::RequestInterrupt(const InterruptRequest& req) {
    if (soft_gic_) {
        soft_gic_->SetSpiLevel(req.vector, req.level_triggered);
        return;
    }
    if (__builtin_available(macOS 15.0, *)) {
        hv_return_t ret = hv_gic_set_spi(req.vector, req.level_triggered);
        if (ret != HV_SUCCESS) {
            LOG_WARN("hvf: hv_gic_set_spi(%u, %s) failed: %d",
                     req.vector, req.level_triggered ? "assert" : "deassert",
                     (int)ret);
        }
    }
}

} // namespace hvf
