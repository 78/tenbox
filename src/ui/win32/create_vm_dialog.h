#pragma once

#include "manager/manager_service.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Shows the new VM creation dialog
// This replaces the old ShowCreateVmDialog for online image workflow
// Returns true if a VM was created
bool ShowCreateVmDialog2(HWND parent, ManagerService& mgr, std::string* error);
