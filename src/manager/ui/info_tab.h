#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <vector>

struct VmSpec;

// Info Tab component showing VM details (ID, location, kernel, disk, etc.)
class InfoTab {
public:
    InfoTab() = default;
    ~InfoTab() = default;

    void Create(HWND parent, HINSTANCE hinst, HFONT ui_font);
    void Show(bool visible);
    void Layout(HWND hwnd, HFONT ui_font, int px, int py, int pw, int ph);
    void Update(const VmSpec* spec);

    static constexpr int kDetailRows = 6;

private:
    HWND labels_[kDetailRows] = {};
    HWND values_[kDetailRows] = {};
};
