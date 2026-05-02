#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

class ManagerService;

void ShowPortForwardsDialog(HWND parent, ManagerService& mgr, const std::string& vm_id);

