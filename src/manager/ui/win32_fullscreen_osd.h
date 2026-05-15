#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

// Transient text overlay shown at center of fullscreen window on VM switch.
class FullscreenOsd {
public:
    static HWND Create(HINSTANCE hinst, HWND parent);
    static void Show(HWND hwnd, const std::wstring& text);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
};
