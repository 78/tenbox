#pragma once

#include <windows.h>
#include <WinHvPlatform.h>
#include <WinHvEmulation.h>

// Some Windows SDK revisions ship WinHvEmulation.h without these declarations.
extern "C" {
HRESULT WINAPI WHvEmulatorCreateEmulator(
    const WHV_EMULATOR_CALLBACKS* callbacks,
    WHV_EMULATOR_HANDLE* emulator);

HRESULT WINAPI WHvEmulatorDestroyEmulator(
    WHV_EMULATOR_HANDLE emulator);

HRESULT WINAPI WHvEmulatorTryIoEmulation(
    WHV_EMULATOR_HANDLE emulator,
    VOID* context,
    const WHV_VP_EXIT_CONTEXT* vp_context,
    const WHV_X64_IO_PORT_ACCESS_CONTEXT* io_instruction_context,
    WHV_EMULATOR_STATUS* emulator_return_status);

HRESULT WINAPI WHvEmulatorTryMmioEmulation(
    WHV_EMULATOR_HANDLE emulator,
    VOID* context,
    const WHV_VP_EXIT_CONTEXT* vp_context,
    const WHV_MEMORY_ACCESS_CONTEXT* mmio_instruction_context,
    WHV_EMULATOR_STATUS* emulator_return_status);
}

namespace whvp {

bool IsHypervisorPresent();

} // namespace whvp
