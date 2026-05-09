#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <functional>
#include <string>
#include <vector>
#include <cstdint>
#include "manager/app_settings.h"

// Topmost floating toolbar for fullscreen mode.
// Auto-hides after 3s when unpinned. Pinnable. Draggable; snaps to nearest screen edge.
class FloatingToolbar {
public:
    using ExitCallback = std::function<void()>;
    using PinCallback = std::function<void(bool pinned)>;
    using SwitchCallback = std::function<void(const std::string& vm_id)>;
    using DpiZoomCallback = std::function<void()>;

    static HWND Create(HINSTANCE hinst, HWND fullscreen_hwnd);

    static void RestoreState(HWND hwnd, const settings::FullscreenToolbarState& state);
    static settings::FullscreenToolbarState SaveState(HWND hwnd);

    static void SetExitCallback(HWND hwnd, ExitCallback cb);
    static void SetPinCallback(HWND hwnd, PinCallback cb);
    static void SetSwitchCallback(HWND hwnd, SwitchCallback cb);
    static void SetDpiZoomCallback(HWND hwnd, DpiZoomCallback cb);

    // Update displayed VM info and available VMs for dropdown.
    static void SetVmInfo(HWND hwnd, const std::string& current_id, const std::string& name,
                          uint32_t width, uint32_t height,
                          const std::vector<std::string>& running_ids,
                          const std::vector<std::string>& running_names);
    static void SetDpiZoomState(HWND hwnd, bool enabled);

    static void OnFullscreenDeactivated(HWND hwnd);
    static void CheckMouseNearEdge(HWND hwnd, POINT cursor_screen);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    enum class SnapEdge { Top, Bottom, Left, Right };

    struct ToolbarState {
        bool pinned = true;
        bool snapped = false;      // false = free-floating
        SnapEdge snap = SnapEdge::Top;
        int offset = -1;
        POINT free_pos{};          // position when free-floating
        bool tab_mode = false;     // shrunk to icon-only tab at edge
        ExitCallback exit_cb;
        PinCallback pin_cb;
        SwitchCallback switch_cb;
        DpiZoomCallback dpi_zoom_cb;
        HWND fullscreen_parent = nullptr;
        HFONT ui_font = nullptr;
        HWND drag_handle = nullptr;   // grip icon for dragging
        HWND btn_vm = nullptr;        // dropdown button showing VM name
        HWND btn_dpi = nullptr;
        HWND btn_pin = nullptr;
        HWND btn_exit = nullptr;
        HWND tooltip = nullptr;
        std::vector<std::string> running_vm_ids;
        std::vector<std::string> running_vm_names;
        std::string current_vm_id;
        POINT drag_start{};
        bool dragging = false;
        UINT_PTR hide_timer_id = 0;
        int tb_width = 0;
        int tb_height = 0;
    };

    static ToolbarState* GetState(HWND hwnd);
    static void StartAutoHideTimer(HWND hwnd);
    static void KillAutoHideTimer(HWND hwnd);
    static void ShowBar(HWND hwnd);
    static void HideBar(HWND hwnd);
    static void LayoutButtons(HWND hwnd);
    static void UpdatePosition(HWND hwnd);
    static void SnapToNearestEdge(HWND hwnd);
};
