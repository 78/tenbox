#include "platform/windows/hypervisor/whvp_platform.h"
#include "platform/windows/hypervisor/whvp_dyn.h"
#include "core/vmm/types.h"

#include <WinHvPlatform.h>

namespace whvp {

bool IsHypervisorPresent() {
    // Load optional exports once per process; presence of WHvGetCapability
    // itself is guaranteed on any host where WinHvPlatform.dll loaded.
    dyn::Load();

    WHV_CAPABILITY capability{};
    UINT32 size = 0;
    HRESULT hr = WHvGetCapability(
        WHvCapabilityCodeHypervisorPresent,
        &capability, sizeof(capability), &size);
    if (FAILED(hr)) {
        LOG_ERROR("WHvGetCapability failed: 0x%08lX", hr);
        return false;
    }
    return capability.HypervisorPresent != FALSE;
}

} // namespace whvp
