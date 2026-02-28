#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>

#include <string>
#include <vector>

class ManagerService;

// Shared Folders Tab component for the VM detail view.
// Provides UI for managing shared folders between host and guest VM.
class SharedFoldersTab {
public:
    SharedFoldersTab() = default;
    ~SharedFoldersTab() = default;

    // Create the tab's controls as children of the given parent window.
    void Create(HWND parent, HINSTANCE hinst, HFONT ui_font);

    // Show or hide all controls belonging to this tab.
    void Show(bool visible);

    // Layout controls within the given bounds (px, py, pw, ph).
    void Layout(int px, int py, int pw, int ph);

    // Refresh the list of shared folders for the given VM.
    void Refresh(ManagerService& manager, const std::string& vm_id);

    // Handle WM_COMMAND messages. Returns true if handled.
    bool HandleCommand(HWND hwnd, UINT cmd, UINT code, ManagerService& manager,
                       const std::string& vm_id);

    // Control IDs used by this component
    static constexpr UINT kListViewId = 2008;
    static constexpr UINT kAddButtonId = 2009;
    static constexpr UINT kRemoveButtonId = 2010;

private:
    HWND listview_ = nullptr;
    HWND add_btn_ = nullptr;
    HWND del_btn_ = nullptr;
};
