#pragma once

#include <windows.h>
#include <WinHvPlatform.h>

// Dynamic resolver for WHPX exports that are NOT present on every supported
// Windows build. Required exports (CreatePartition, SetPartitionProperty,
// RunVirtualProcessor, emulator APIs, etc.) are linked statically via
// WinHvPlatform.lib / WinHvEmulation.lib because they have shipped since
// Windows 10 1803, the first release to include the Windows Hypervisor
// Platform at all.
//
// Resolve-at-runtime is only used for symbols whose presence varies by
// build:
//   - WHvRequestInterrupt        — 1809+ (missing on 1803)
//   - WHvCreateNotificationPort  — 1809+
//   - WHvDeleteNotificationPort  — 1809+
//
// Linking any of these statically would cause the OS loader on 1803 to
// refuse the EXE with "entry point not found" before main() runs, which
// is what we are avoiding.
//
// Load() must be called once before querying Has*() / invoking the
// wrappers. It opens WinHvPlatform.dll with LoadLibraryW (already mapped
// as a static-import side effect on hosts where all mandatory exports
// exist) and resolves the optional symbols via GetProcAddress.

namespace whvp {
namespace dyn {

bool Load();
void Unload();

bool HasRequestInterrupt();
bool HasCreateNotificationPort();
bool HasDeleteNotificationPort();

// Call only after Has*() returns true; otherwise returns E_NOTIMPL and
// callers must route through a fallback.
HRESULT RequestInterrupt(WHV_PARTITION_HANDLE partition,
                         const WHV_INTERRUPT_CONTROL* control,
                         UINT32 control_size);
HRESULT CreateNotificationPort(
    WHV_PARTITION_HANDLE partition,
    const WHV_NOTIFICATION_PORT_PARAMETERS* parameters,
    HANDLE event, WHV_NOTIFICATION_PORT_HANDLE* port);
HRESULT DeleteNotificationPort(WHV_PARTITION_HANDLE partition,
                               WHV_NOTIFICATION_PORT_HANDLE port);

} // namespace dyn
} // namespace whvp
