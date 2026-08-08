#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "manager/manager_service.h"

class DisplayPanel;
struct CursorInfo;

// Borderless fullscreen window that hosts a reparented DisplayPanel.
class FullscreenWindow {
public:
    using ExitCallback = std::function<void()>;
    using SwitchVmCallback = std::function<void(const std::string& vm_id, bool forward)>;

    FullscreenWindow();
    ~FullscreenWindow();

    // Create the fullscreen window on the specified monitor.
    bool Create(HINSTANCE hinst, HMONITOR monitor,
                std::unique_ptr<DisplayPanel> display_panel,
                const std::string& current_vm_id,
                ManagerService& manager);

    void Exit();
    HWND Handle() const { return hwnd_; }

    void SetExitCallback(ExitCallback cb);
    void SetSwitchVmCallback(SwitchVmCallback cb);

    void SwitchToVm(const std::string& vm_id);
    void SetCurrentVmId(const std::string& id) { current_vm_id_ = id; }

    // Release ownership of the DisplayPanel (during exit fullscreen).
    std::unique_ptr<DisplayPanel> ReleaseDisplayPanel();

    HWND GetToolbarHwnd() const { return toolbar_hwnd_; }

    // Adopt a different VM's cached framebuffer and cursor after switching.
    void AdoptVmState(const std::vector<uint8_t>& framebuffer,
                      uint32_t fb_width, uint32_t fb_height,
                      const CursorInfo& cursor, const std::vector<uint8_t>& cursor_pixels);

    // Toggle DPI zoom and apply to DisplayPanel.
    void ToggleDpiZoom(float dpi_factor);

    // Show transient OSD overlay with VM name.
    void ShowOsd(const std::wstring& text);

    std::vector<std::string> GetRunningVmIds() const;
    void RefreshVmList();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    void OnKeyDown(WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    HWND toolbar_hwnd_ = nullptr;
    HWND osd_hwnd_ = nullptr;
    std::unique_ptr<DisplayPanel> display_panel_;
    ManagerService* manager_ = nullptr;
    std::string current_vm_id_;
    bool exiting_ = false;

    ExitCallback exit_cb_;
    SwitchVmCallback switch_vm_cb_;
};
