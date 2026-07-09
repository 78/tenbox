#include "manager/ui/win32_fullscreen_window.h"
#include "manager/ui/win32_display_panel.h"
#include "manager/ui/win32_floating_toolbar.h"
#include "manager/ui/win32_fullscreen_osd.h"
#include "manager/i18n.h"
#include <windowsx.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

namespace {
constexpr const wchar_t* kWndClass = L"TenBoxFullscreenWindow";
}

FullscreenWindow::FullscreenWindow() = default;

FullscreenWindow::~FullscreenWindow() {
    if (hwnd_ && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
}

bool FullscreenWindow::Create(HINSTANCE hinst, HMONITOR monitor,
                               std::unique_ptr<DisplayPanel> display_panel,
                               const std::string& current_vm_id,
                               ManagerService& manager) {
    display_panel_ = std::move(display_panel);
    current_vm_id_ = current_vm_id;
    manager_ = &manager;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);
    const RECT& mr = mi.rcMonitor;
    int w = mr.right - mr.left;
    int h = mr.bottom - mr.top;

    // Create hidden first so DWM never renders a default border
    hwnd_ = CreateWindowExW(0, kWndClass, L"",
        WS_POPUP | WS_THICKFRAME,
        mr.left, mr.top, w, h,
        nullptr, nullptr, hinst, this);

    if (!hwnd_) return false;

    MARGINS m{ 0, 0, 0, 1 };
    DwmExtendFrameIntoClientArea(hwnd_, &m);
    COLORREF border_color = RGB(0, 0, 0);
    DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border_color, sizeof(border_color));
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
        SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE |
        SWP_FRAMECHANGED | SWP_NOSIZE | SWP_NOMOVE);

    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    display_panel_->Reparent(hwnd_);
    SetWindowPos(display_panel_->Handle(), nullptr, 0, 0, w, h, SWP_NOZORDER);
    display_panel_->SetFullscreenMode(true);
    display_panel_->SetHintOffsetY(60);
    display_panel_->SetVisible(true);

    // Notify VM of new display size
    {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        uint32_t disp_w = static_cast<uint32_t>(rc.right) & ~7u;
        uint32_t disp_h = static_cast<uint32_t>(rc.bottom);
        if (disp_w > 0 && disp_h > 0) {
            manager.SetDisplaySize(current_vm_id_, disp_w, disp_h);
        }
    }

    toolbar_hwnd_ = FloatingToolbar::Create(hinst, hwnd_);
    FloatingToolbar::SetExitCallback(toolbar_hwnd_, [this]() { Exit(); });

    osd_hwnd_ = FullscreenOsd::Create(hinst, hwnd_);

    SetTimer(hwnd_, 1, 200, nullptr);

    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);

    return true;
}

std::unique_ptr<DisplayPanel> FullscreenWindow::ReleaseDisplayPanel() {
    if (display_panel_) {
        display_panel_->SetFullscreenMode(false);
        if (hwnd_) display_panel_->Reparent(nullptr);
    }
    return std::move(display_panel_);
}

void FullscreenWindow::Exit() {
    if (exiting_) return;
    exiting_ = true;
    if (exit_cb_) exit_cb_();
}

void FullscreenWindow::ToggleDpiZoom(float dpi_factor) {
    if (!display_panel_ || !hwnd_) return;
    display_panel_->SetDpiZoomFactor(dpi_factor);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    uint32_t pw = rc.right & ~7u;
    uint32_t ph = rc.bottom;
    // When zoomed in, guest renders at lower resolution, then DisplayPanel scales up
    uint32_t disp_w, disp_h;
    if (dpi_factor > 1.0f) {
        UINT dpi = GetDpiForWindow(hwnd_);
        disp_w = static_cast<uint32_t>(MulDiv(static_cast<int>(pw), 96, static_cast<int>(dpi))) & ~7u;
        disp_h = static_cast<uint32_t>(MulDiv(static_cast<int>(ph), 96, static_cast<int>(dpi)));
    } else {
        disp_w = pw;
        disp_h = ph;
    }
    if (disp_w > 0 && disp_h > 0 && manager_) {
        manager_->SetDisplaySize(current_vm_id_, disp_w, disp_h);
    }
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void FullscreenWindow::AdoptVmState(const std::vector<uint8_t>& framebuffer,
                                     uint32_t fb_width, uint32_t fb_height,
                                     const CursorInfo& cursor,
                                     const std::vector<uint8_t>& cursor_pixels) {
    if (display_panel_) {
        display_panel_->RestoreFramebuffer(fb_width, fb_height, framebuffer);
        display_panel_->RestoreCursor(cursor, cursor_pixels);
    }
}

void FullscreenWindow::ShowOsd(const std::wstring& text) {
    if (osd_hwnd_) {
        FullscreenOsd::Show(osd_hwnd_, text);
    }
}

void FullscreenWindow::SetExitCallback(ExitCallback cb) { exit_cb_ = std::move(cb); }
void FullscreenWindow::SetSwitchVmCallback(SwitchVmCallback cb) { switch_vm_cb_ = std::move(cb); }

void FullscreenWindow::SwitchToVm(const std::string& vm_id) {
    if (switch_vm_cb_) switch_vm_cb_(vm_id, false);
}

std::vector<std::string> FullscreenWindow::GetRunningVmIds() const {
    std::vector<std::string> ids;
    if (!manager_) return ids;
    auto vms = manager_->ListVms();
    for (const auto& vm : vms) {
        if (vm.state == VmPowerState::kRunning) {
            ids.push_back(vm.spec.vm_id);
        }
    }
    return ids;
}

void FullscreenWindow::RefreshVmList() {
    // Toolbar will pick up changes via FloatingToolbar methods
}

void FullscreenWindow::OnKeyDown(WPARAM wp, LPARAM lp) {
    if (display_panel_ && display_panel_->IsCaptured()) return;
}

LRESULT CALLBACK FullscreenWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<FullscreenWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE:
        return 0;

    case WM_NCCALCSIZE:
        if (wp) return 0;
        break;

    case WM_NCACTIVATE:
        // Suppress DWM white border on activation
        lp = -1;
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (self) self->OnKeyDown(wp, lp);
        return 0;

    case WM_TIMER:
        if (self && self->toolbar_hwnd_) {
            POINT pt;
            GetCursorPos(&pt);
            FloatingToolbar::CheckMouseNearEdge(self->toolbar_hwnd_, pt);
        }
        return 0;

    case WM_FS_SHOW_HINT:
        if (self && self->osd_hwnd_) {
            FullscreenOsd::Show(self->osd_hwnd_,
                i18n::tr_w(i18n::S::kFullscreenFindToolbar).c_str());
        }
        return 0;

    case WM_CLOSE:
        if (self) self->Exit();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            // Don't hide toolbar if it's the window gaining activation (e.g. user clicked it)
            if (self && self->toolbar_hwnd_ && reinterpret_cast<HWND>(lp) != self->toolbar_hwnd_) {
                FloatingToolbar::OnFullscreenDeactivated(self->toolbar_hwnd_);
            }
            return 0;
        }
        {
            LRESULT lr = DefWindowProcW(hwnd, msg, wp, lp);
            if (self && self->display_panel_) {
                SetFocus(self->display_panel_->Handle());
            }
            if (self && self->toolbar_hwnd_) {
                FloatingToolbar::OnFullscreenActivated(self->toolbar_hwnd_);
            }
            return lr;
        }

    case WM_DISPLAYCHANGE:
        // Re-anchor to current monitor on display config changes
        if (self && self->hwnd_) {
            HMONITOR mon = MonitorFromWindow(self->hwnd_, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) };
            if (GetMonitorInfoW(mon, &mi)) {
                const RECT& mr = mi.rcMonitor;
                SetWindowPos(self->hwnd_, nullptr,
                    mr.left, mr.top,
                    mr.right - mr.left,
                    mr.bottom - mr.top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
