#pragma once

#include "manager/manager_service.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

// Modal dialogs for VM creation and editing.
// Return true if the user confirmed and the operation succeeded.
bool ShowCreateVmDialog(HWND parent, ManagerService& mgr, std::string* error);
bool ShowEditVmDialog(HWND parent, ManagerService& mgr,
                      const VmRecord& rec, std::string* error);

// Modal dialog for managing shared folders of a VM.
void ShowSharedFoldersDialog(HWND parent, ManagerService& mgr, const std::string& vm_id);
