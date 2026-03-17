#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "manager/app_settings.h"

#include <functional>

// Callback: invoked on every add/edit/remove so the caller can persist
// settings and update the proxy service immediately.
using LlmProxySettingsCallback =
    std::function<void(const settings::LlmProxySettings&)>;

void ShowLlmProxyDialog(HWND parent,
                         settings::LlmProxySettings current_settings,
                         LlmProxySettingsCallback on_change);
