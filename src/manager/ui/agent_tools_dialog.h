#pragma once

#include "manager/manager_service.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

void ShowAgentToolsDialog(HWND parent, ManagerService& mgr, const std::string& vm_id);
