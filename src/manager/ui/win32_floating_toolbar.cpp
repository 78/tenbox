#include "manager/ui/win32_floating_toolbar.h"
#include "manager/ui/win32_display_panel.h"  // WM_FS_* defines
#include "manager/i18n.h"
#include "../resource.h"
#include <commctrl.h>
#include <windowsx.h>
#include <climits>

namespace {
constexpr const wchar_t* kWndClass = L"TenBoxFloatingToolbar";

constexpr int kBarHeight = 62;
constexpr int kBtnH = 38;
constexpr int kPad = 10;
constexpr int kGripW = 56;
constexpr int kVmBtnMinW = 280;
constexpr int kIconBtnW = 40;
constexpr int kRightPadding = 14;
constexpr UINT_PTR kHideTimerId = 1;
constexpr DWORD kAutoHideMs = 1500;

enum BtnId { kBtnDrag = 300, kBtnVm = 301, kBtnDpi = 302, kBtnPin = 303, kBtnExit = 304 };
}

HWND FloatingToolbar::Create(HINSTANCE hinst, HWND fullscreen_hwnd) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kWndClass, L"",
        WS_POPUP | WS_VISIBLE,
        100, 0, 500, kBarHeight,
        fullscreen_hwnd, nullptr, hinst, nullptr);
    if (!hwnd) return nullptr;

    auto* st = new ToolbarState();
    st->fullscreen_parent = fullscreen_hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

    // UI font — large size
    LOGFONTW lf{};
    HFONT stock = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    GetObjectW(stock, sizeof(lf), &lf);
    lf.lfHeight = -28;  // 21pt
    lf.lfWeight = FW_NORMAL;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    HFONT ui_font = CreateFontIndirectW(&lf);
    st->ui_font = ui_font;

    // Drag handle — TenBox icon
    st->drag_handle = CreateWindowExW(0, WC_STATICW, L"",
        WS_CHILD | WS_VISIBLE | SS_ICON | SS_CENTERIMAGE | SS_NOTIFY,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kBtnDrag), hinst, nullptr);
    {
        HICON icon = static_cast<HICON>(LoadImageW(hinst, MAKEINTRESOURCEW(IDI_APP_ICON),
            IMAGE_ICON, 54, 54, LR_SHARED));
        SendMessageW(st->drag_handle, STM_SETICON, reinterpret_cast<WPARAM>(icon), 0);
    }

    // VM dropdown button
    st->btn_vm = CreateWindowExW(0, WC_BUTTONW, L"",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_CENTER | BS_FLAT,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kBtnVm), hinst, nullptr);
    SendMessage(st->btn_vm, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font), FALSE);

    // Zoom button (colored BMP)
    st->btn_dpi = CreateWindowExW(0, WC_BUTTONW, L"",
        WS_CHILD | WS_VISIBLE | BS_BITMAP,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kBtnDpi), hinst, nullptr);
    SendMessageW(st->btn_dpi, BM_SETIMAGE, IMAGE_BITMAP,
        reinterpret_cast<LPARAM>(LoadBitmapW(hinst, MAKEINTRESOURCEW(IDB_FS_ZOOM))));

    // Pin button (colored BMP, default pinned)
    st->btn_pin = CreateWindowExW(0, WC_BUTTONW, L"",
        WS_CHILD | WS_VISIBLE | BS_BITMAP,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kBtnPin), hinst, nullptr);
    SendMessageW(st->btn_pin, BM_SETIMAGE, IMAGE_BITMAP,
        reinterpret_cast<LPARAM>(LoadBitmapW(hinst, MAKEINTRESOURCEW(IDB_FS_PIN))));

    // Exit button (colored BMP)
    st->btn_exit = CreateWindowExW(0, WC_BUTTONW, L"",
        WS_CHILD | WS_VISIBLE | BS_BITMAP,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(kBtnExit), hinst, nullptr);
    SendMessageW(st->btn_exit, BM_SETIMAGE, IMAGE_BITMAP,
        reinterpret_cast<LPARAM>(LoadBitmapW(hinst, MAKEINTRESOURCEW(IDB_FS_EXIT))));

    LayoutButtons(hwnd);
    {
        RECT pr;
        GetWindowRect(fullscreen_hwnd, &pr);
        st->free_pos.x = (pr.left + pr.right - st->tb_width) / 2;
        st->free_pos.y = pr.top + 15;
    }
    UpdatePosition(hwnd);

    // Create tooltip control
    HWND tt = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hwnd, nullptr, hinst, nullptr);
    SetWindowPos(tt, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    st->tooltip = tt;

    auto AddTool = [&](HWND btn, const wchar_t* text) {
        TOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
        ti.hwnd = hwnd;
        ti.uId = reinterpret_cast<UINT_PTR>(btn);
        ti.lpszText = const_cast<wchar_t*>(text);
        SendMessageW(tt, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    };

    AddTool(st->btn_dpi,  L"\U0001F50D 缩放 (DPI Zoom)");
    AddTool(st->btn_pin,  L"\U0001F4CC 取消固定");
    AddTool(st->btn_exit, L"✕ 退出全屏 (或长按 ESC 2 秒)");
    AddTool(st->btn_vm,   L"切换虚拟机");

    AddTool(st->drag_handle, L"可拖拽至窗口边缘以吸附");

    return hwnd;
}

void FloatingToolbar::RestoreState(HWND hwnd, const settings::FullscreenToolbarState& state) {
    auto* st = GetState(hwnd);
    if (!st) return;
    st->snap = static_cast<SnapEdge>(state.snap_edge);
    st->offset = state.offset;
    st->pinned = state.pinned;
    if (st->pinned) {
        KillAutoHideTimer(hwnd);
        SendMessageW(st->btn_pin, BM_SETIMAGE, IMAGE_BITMAP,
            reinterpret_cast<LPARAM>(LoadBitmapW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDB_FS_PIN))));
    }
    UpdatePosition(hwnd);
}

settings::FullscreenToolbarState FloatingToolbar::SaveState(HWND hwnd) {
    settings::FullscreenToolbarState s;
    auto* st = GetState(hwnd);
    if (!st) return s;
    s.snap_edge = static_cast<int>(st->snap);
    s.offset = st->offset;
    s.pinned = st->pinned;
    return s;
}

void FloatingToolbar::SetExitCallback(HWND hwnd, ExitCallback cb) { auto* st = GetState(hwnd); if (st) st->exit_cb = std::move(cb); }
void FloatingToolbar::SetPinCallback(HWND hwnd, PinCallback cb) { auto* st = GetState(hwnd); if (st) st->pin_cb = std::move(cb); }
void FloatingToolbar::SetSwitchCallback(HWND hwnd, SwitchCallback cb) { auto* st = GetState(hwnd); if (st) st->switch_cb = std::move(cb); }
void FloatingToolbar::SetDpiZoomCallback(HWND hwnd, DpiZoomCallback cb) { auto* st = GetState(hwnd); if (st) st->dpi_zoom_cb = std::move(cb); }

void FloatingToolbar::SetVmInfo(HWND hwnd, const std::string& current_id, const std::string& name,
                                 uint32_t width, uint32_t height,
                                 const std::vector<std::string>& running_ids,
                                 const std::vector<std::string>& running_names) {
    auto* st = GetState(hwnd);
    if (!st) return;
    st->current_vm_id = current_id;
    st->running_vm_ids = running_ids;
    st->running_vm_names = running_names;
    wchar_t buf[256];
    swprintf_s(buf, L"  %hs  (%u\x00d7%u) \x25be", name.c_str(), width, height);
    SetWindowTextW(st->btn_vm, buf);

    // Measure text to size toolbar
    HDC hdc = GetDC(st->btn_vm);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(st->btn_vm, WM_GETFONT, 0, 0));
    HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
    SIZE sz{};
    GetTextExtentPoint32W(hdc, buf, static_cast<int>(wcslen(buf)), &sz);
    SelectObject(hdc, old);
    ReleaseDC(st->btn_vm, hdc);

    int vm_w = sz.cx + kPad * 2 + 10;
    if (vm_w < kVmBtnMinW) vm_w = kVmBtnMinW;
    int new_w = kPad + kGripW + 2 + vm_w + kPad + kIconBtnW * 3 + 6 + kRightPadding;
    if (new_w < 420) new_w = 420;
    st->tb_width = new_w;

    SetWindowPos(hwnd, nullptr, 0, 0, st->tb_width, kBarHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    LayoutButtons(hwnd);
    UpdatePosition(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);  // force full redraw, erase background
}

void FloatingToolbar::SetDpiZoomState(HWND hwnd, bool enabled) {
    auto* st = GetState(hwnd);
    if (!st) return;
    HBITMAP hbm = LoadBitmapW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(enabled ? IDB_FS_ZOOM_ACTIVE : IDB_FS_ZOOM));
    SendMessageW(st->btn_dpi, BM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(hbm));
}

void FloatingToolbar::OnFullscreenDeactivated(HWND hwnd) {
    ShowWindow(hwnd, SW_HIDE);
}

void FloatingToolbar::OnFullscreenActivated(HWND hwnd) {
    ShowBar(hwnd);
}

FloatingToolbar::ToolbarState* FloatingToolbar::GetState(HWND hwnd) {
    return reinterpret_cast<ToolbarState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void FloatingToolbar::StartAutoHideTimer(HWND hwnd) {
    auto* st = GetState(hwnd);
    if (!st) return;
    // Pinned + free-floating: no auto-hide at all
    if (st->pinned && !st->snapped) return;
    KillAutoHideTimer(hwnd);
    st->hide_timer_id = SetTimer(hwnd, kHideTimerId, kAutoHideMs, nullptr);
}

void FloatingToolbar::KillAutoHideTimer(HWND hwnd) {
    auto* st = GetState(hwnd);
    if (!st) return;
    if (st->hide_timer_id) { KillTimer(hwnd, st->hide_timer_id); st->hide_timer_id = 0; }
}

void FloatingToolbar::ShowBar(HWND hwnd) {
    auto* st = GetState(hwnd);
    if (!st) return;
    st->tab_mode = false;
    // Restore full size
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, st->tb_width, st->tb_height,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd, nullptr, TRUE);
    // Show all children
    ShowWindow(st->drag_handle, SW_SHOW);
    ShowWindow(st->btn_vm, SW_SHOW);
    ShowWindow(st->btn_dpi, SW_SHOW);
    ShowWindow(st->btn_pin, SW_SHOW);
    ShowWindow(st->btn_exit, SW_SHOW);
    LayoutButtons(hwnd);
    UpdatePosition(hwnd);
    StartAutoHideTimer(hwnd);
}

void FloatingToolbar::HideBar(HWND hwnd) {
    auto* st = GetState(hwnd);
    if (!st || st->dragging) return;
    // Unpinned: always fully hide. Pinned: shrink to icon tab (only when snapped)
    if (!st->pinned) {
        ShowWindow(hwnd, SW_HIDE);
        return;
    }
    if (!st->snapped) return;  // pinned + free-floating: stay visible
    st->tab_mode = true;
    ShowWindow(st->btn_vm, SW_HIDE);
    ShowWindow(st->btn_dpi, SW_HIDE);
    ShowWindow(st->btn_pin, SW_HIDE);
    ShowWindow(st->btn_exit, SW_HIDE);
    ShowWindow(st->drag_handle, SW_SHOW);
    // Fixed square tab size to fit the icon
    const int kTabSize = kBarHeight;  // 56px square
    RECT pr;
    GetWindowRect(st->fullscreen_parent, &pr);
    int x, y;
    switch (st->snap) {
    case SnapEdge::Top:
        x = (st->offset >= 0) ? pr.left + st->offset : pr.left + (pr.right - pr.left - kTabSize) / 2;
        y = pr.top; break;
    case SnapEdge::Bottom:
        x = (st->offset >= 0) ? pr.left + st->offset : pr.left + (pr.right - pr.left - kTabSize) / 2;
        y = pr.bottom - kTabSize; break;
    case SnapEdge::Left:
        x = pr.left;
        y = (st->offset >= 0) ? pr.top + st->offset : pr.top + (pr.bottom - pr.top - kTabSize) / 2;
        break;
    case SnapEdge::Right:
        x = pr.right - kTabSize;
        y = (st->offset >= 0) ? pr.top + st->offset : pr.top + (pr.bottom - pr.top - kTabSize) / 2;
        break;
    }
    // Fill icon in tab area
    SetWindowPos(st->drag_handle, nullptr, 0, 0, kTabSize, kTabSize, SWP_NOZORDER);
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, kTabSize, kTabSize, SWP_NOACTIVATE);
    InvalidateRect(hwnd, nullptr, TRUE);
}

void FloatingToolbar::LayoutButtons(HWND hwnd) {
    auto* st = GetState(hwnd);
    if (!st) return;
    if (st->tb_width == 0) st->tb_width = 420;
    int y = (kBarHeight - kBtnH) / 2;

    // Icon buttons anchored to right edge (fixed position)
    int rx = st->tb_width - kRightPadding;
    rx -= kIconBtnW; MoveWindow(st->btn_exit, rx, y, kIconBtnW, kBtnH, FALSE);
    rx -= kIconBtnW + 2; MoveWindow(st->btn_pin, rx, y, kIconBtnW, kBtnH, FALSE);
    rx -= kIconBtnW + 2; MoveWindow(st->btn_dpi, rx, y, kIconBtnW, kBtnH, FALSE);

    // Drag handle at left — taller to fill more vertical space
    int lx = kPad;
    int grip_h = kBarHeight - 10;
    MoveWindow(st->drag_handle, lx, 5, kGripW, grip_h, FALSE);
    lx += kGripW + 2;

    // VM button fills space between drag handle and icon buttons
    int vm_w = rx - lx - kPad;
    if (vm_w < kVmBtnMinW) vm_w = kVmBtnMinW;
    MoveWindow(st->btn_vm, lx, y, vm_w, kBtnH, FALSE);

    st->tb_height = kBarHeight;
    SetWindowPos(hwnd, nullptr, 0, 0, st->tb_width, kBarHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void FloatingToolbar::UpdatePosition(HWND hwnd) {
    auto* st = GetState(hwnd);
    if (!st || !st->fullscreen_parent) return;
    RECT pr;
    GetWindowRect(st->fullscreen_parent, &pr);
    if (!st->snapped) {
        int x = st->free_pos.x, y = st->free_pos.y;
        if (x < pr.left) x = pr.left;
        if (y < pr.top) y = pr.top;
        if (x + st->tb_width > pr.right) x = pr.right - st->tb_width;
        if (y + st->tb_height > pr.bottom) y = pr.bottom - st->tb_height;
        SetWindowPos(hwnd, HWND_TOPMOST, x, y, st->tb_width, st->tb_height, SWP_NOACTIVATE);
        return;
    }
    int pw = pr.right - pr.left;
    int ph = pr.bottom - pr.top;
    int cx = pr.left + (pw - st->tb_width) / 2;
    int x, y;
    switch (st->snap) {
    case SnapEdge::Top:
        x = (st->offset >= 0) ? pr.left + st->offset : cx;
        y = pr.top + 2; break;
    case SnapEdge::Bottom:
        x = (st->offset >= 0) ? pr.left + st->offset : cx;
        y = pr.bottom - st->tb_height - 2; break;
    case SnapEdge::Left:
        x = pr.left + 2;
        y = (st->offset >= 0) ? pr.top + st->offset : pr.top + (ph - st->tb_height) / 2; break;
    case SnapEdge::Right:
        x = pr.right - st->tb_width - 2;
        y = (st->offset >= 0) ? pr.top + st->offset : pr.top + (ph - st->tb_height) / 2; break;
    }
    if (x < pr.left) x = pr.left;
    if (x + st->tb_width > pr.right) x = pr.right - st->tb_width;
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, st->tb_width, st->tb_height, SWP_NOACTIVATE);
}

void FloatingToolbar::SnapToNearestEdge(HWND hwnd) {
    auto* st = GetState(hwnd);
    if (!st || !st->fullscreen_parent) return;
    RECT pr, tr;
    GetWindowRect(st->fullscreen_parent, &pr);
    GetWindowRect(hwnd, &tr);
    // Measure from toolbar EDGES, not center
    int d_top = tr.top - pr.top;
    int d_bot = pr.bottom - tr.bottom;
    int d_left = tr.left - pr.left;
    int d_right = pr.right - tr.right;
    int cx = (tr.left + tr.right) / 2;
    int cy = (tr.top + tr.bottom) / 2;
    const int kSnapDistTop = 8;
    const int kSnapDistBot = 80;
    const int kSnapDistLeft = 80;
    const int kSnapDistRight = 80;

    int best_d = INT_MAX;
    SnapEdge best = SnapEdge::Top;
    int best_off = 0;
    if (d_top <= kSnapDistTop && d_top < best_d) {
        best_d = d_top; best = SnapEdge::Top; best_off = cx - pr.left;
    }
    if (d_bot <= kSnapDistBot && d_bot < best_d) {
        best_d = d_bot; best = SnapEdge::Bottom; best_off = cx - pr.left;
    }
    if (d_left <= kSnapDistLeft && d_left < best_d) {
        best_d = d_left; best = SnapEdge::Left; best_off = cy - pr.top;
    }
    if (d_right <= kSnapDistRight && d_right < best_d) {
        best_d = d_right; best = SnapEdge::Right; best_off = cy - pr.top;
    }
    if (best_d < INT_MAX) {
        st->snap = best;
        st->offset = best_off;
        st->snapped = true;
    } else {
        st->free_pos.x = tr.left;
        st->free_pos.y = tr.top;
        st->snapped = false;
    }
    UpdatePosition(hwnd);
    // Start shrink-to-tab after snapping — use timer with explicit ID
    if (st->snapped) {
        KillTimer(hwnd, kHideTimerId);
        st->hide_timer_id = SetTimer(hwnd, kHideTimerId, kAutoHideMs, nullptr);
    }
}

void FloatingToolbar::CheckMouseNearEdge(HWND hwnd, POINT cursor) {
    auto* st = GetState(hwnd);
    if (!st || !st->fullscreen_parent || st->dragging) return;
    RECT pr;
    GetWindowRect(st->fullscreen_parent, &pr);
    int screen_w = pr.right - pr.left;
    int screen_cx = (pr.left + pr.right) / 2;
    const int kSafeZoneHalfW = screen_w / 3;  // 2/3 of screen width

    // "Safe zone": top center 2/3 of screen — show at initial position
    if (cursor.y <= pr.top + 10 && abs(cursor.x - screen_cx) <= kSafeZoneHalfW) {
        int init_x = (pr.left + pr.right - st->tb_width) / 2;
        int init_y = pr.top + 15;
        // Already at initial position and visible — don't re-trigger
        if (!st->snapped && !st->tab_mode && IsWindowVisible(hwnd) &&
            st->free_pos.x == init_x && st->free_pos.y == init_y) {
            return;
        }
        st->snapped = false;
        st->free_pos.x = init_x;
        st->free_pos.y = init_y;
        st->tab_mode = false;
        UpdatePosition(hwnd);
        ShowBar(hwnd);
        return;
    }

    if (st->pinned) return;

    bool hit = false;
    switch (st->snap) {
    case SnapEdge::Top:    hit = (cursor.y <= pr.top + 10); break;
    case SnapEdge::Bottom: hit = (cursor.y >= pr.bottom - 10); break;
    case SnapEdge::Left:   hit = (cursor.x <= pr.left + 10); break;
    case SnapEdge::Right:  hit = (cursor.x >= pr.right - 10); break;
    }
    if (hit) ShowBar(hwnd);
}

LRESULT CALLBACK FloatingToolbar::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = GetState(hwnd);
    switch (msg) {
    case WM_CREATE:
        return 0;
    case WM_SIZE: {
        RECT rc; GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w > 0 && h > 0) {
            HRGN rgn = CreateRoundRectRgn(0, 0, w, h, 14, 14);
            SetWindowRgn(hwnd, rgn, TRUE);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        // Properly fill background to avoid ghost artifacts
        {
            RECT rc; GetClientRect(hwnd, &rc);
            HBRUSH white = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
            FillRect(reinterpret_cast<HDC>(wp), &rc, white);
        }
        return 1;  // non-zero = we handled it
    case WM_DESTROY: {
        if (st) {
            KillTimer(hwnd, kHideTimerId);
            if (st->ui_font) DeleteObject(st->ui_font);
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == kHideTimerId && st && !st->dragging) {
            POINT pt;
            GetCursorPos(&pt);
            RECT pr, tr;
            GetWindowRect(st->fullscreen_parent, &pr);
            GetWindowRect(hwnd, &tr);
            const int kEdgeDist = 10;
            bool over_toolbar = PtInRect(&tr, pt);
            bool near_edge = false;
            switch (st->snap) {
            case SnapEdge::Top:    near_edge = (pt.y <= pr.top + kEdgeDist); break;
            case SnapEdge::Bottom: near_edge = (pt.y >= pr.bottom - kEdgeDist); break;
            case SnapEdge::Left:   near_edge = (pt.x <= pr.left + kEdgeDist); break;
            case SnapEdge::Right:  near_edge = (pt.x >= pr.right - kEdgeDist); break;
            }
            if (over_toolbar || near_edge) {
                KillTimer(hwnd, kHideTimerId);
                st->hide_timer_id = SetTimer(hwnd, kHideTimerId, kAutoHideMs, nullptr);
            } else {
                KillTimer(hwnd, kHideTimerId);
                st->hide_timer_id = 0;
                HideBar(hwnd);
            }
        }
        return 0;
    case WM_MOUSEMOVE:
        if (st && st->tooltip) {
            MSG m{ hwnd, msg, wp, lp };
            SendMessageW(st->tooltip, TTM_RELAYEVENT, 0, reinterpret_cast<LPARAM>(&m));
        }
        // If in tab mode, expand on mouse touch
        if (st && st->tab_mode) {
            ShowBar(hwnd);
        }
        if (st && !st->pinned) { KillAutoHideTimer(hwnd); StartAutoHideTimer(hwnd); }
        if (st && st->dragging) {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(hwnd, &pt);
            int dx = pt.x - st->drag_start.x, dy = pt.y - st->drag_start.y;
            RECT rc; GetWindowRect(hwnd, &rc);
            SetWindowPos(hwnd, nullptr, rc.left + dx, rc.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            st->drag_start = pt;
        }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        HWND child = ChildWindowFromPoint(hwnd, pt);
        if (st && (child == hwnd || !child || child == st->drag_handle)) {
            st->dragging = true;
            ClientToScreen(hwnd, &pt);
            st->drag_start = pt;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (st && st->dragging) { st->dragging = false; ReleaseCapture(); SnapToNearestEdge(hwnd); }
        return 0;
    case WM_COMMAND: {
        if (st && !st->pinned) { KillAutoHideTimer(hwnd); StartAutoHideTimer(hwnd); }
        UINT id = LOWORD(wp);
        switch (id) {
        case kBtnDrag:
            if (st && !st->dragging) {
                st->dragging = true;
                GetCursorPos(&st->drag_start);
                SetCapture(hwnd);
            }
            return 0;
        case kBtnVm: {
            if (st->running_vm_ids.empty()) break;
            HMENU menu = CreatePopupMenu();
            for (size_t i = 0; i < st->running_vm_ids.size(); ++i) {
                UINT flags = MF_STRING;
                if (st->running_vm_ids[i] == st->current_vm_id) flags |= MF_CHECKED;
                std::wstring name = i18n::to_wide(st->running_vm_names[i]);
                AppendMenuW(menu, flags, static_cast<UINT>(1000 + i), name.c_str());
            }
            RECT rc; GetWindowRect(st->btn_vm, &rc);
            SetForegroundWindow(hwnd);
            UINT sel = TrackPopupMenuEx(menu,
                TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
                rc.left, rc.bottom, hwnd, nullptr);
            DestroyMenu(menu);
            if (sel >= 1000 && st->switch_cb) {
                size_t idx = sel - 1000;
                if (idx < st->running_vm_ids.size())
                    st->switch_cb(st->running_vm_ids[idx]);
            }
            return 0;
        }
        case kBtnDpi:
            if (st && st->dpi_zoom_cb) st->dpi_zoom_cb();
            return 0;
        case kBtnPin:
            if (st) {
                st->pinned = !st->pinned;
                {
                    HINSTANCE hi = GetModuleHandleW(nullptr);
                    HBITMAP hbm = LoadBitmapW(hi,
                        MAKEINTRESOURCEW(st->pinned ? IDB_FS_PIN : IDB_FS_UNPIN));
                    SendMessageW(st->btn_pin, BM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(hbm));
                }
                if (!st->pinned) StartAutoHideTimer(hwnd);
                else KillAutoHideTimer(hwnd);
                if (st->pin_cb) st->pin_cb(st->pinned);
                // Update tooltip + show OSD hint when unpinning
                if (st->tooltip) {
                    TOOLINFOW ti{ sizeof(ti) };
                    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
                    ti.hwnd = hwnd;
                    ti.uId = reinterpret_cast<UINT_PTR>(st->btn_pin);
                    ti.lpszText = const_cast<wchar_t*>(st->pinned
                        ? L"\U0001F4CC 取消固定"
                        : L"\U0001F4CD 固定");
                    SendMessageW(st->tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
                }
                if (!st->pinned && st->fullscreen_parent) {
                    PostMessageW(st->fullscreen_parent, WM_FS_SHOW_HINT, 0, 0);
                }
            }
            return 0;
        case kBtnExit:
            if (st && st->exit_cb) st->exit_cb();
            return 0;
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        // Double-buffer: draw to memory DC first, then blit to screen
        HDC mem_dc = CreateCompatibleDC(hdc);
        HBITMAP mem_bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ old_bmp = SelectObject(mem_dc, mem_bmp);
        // Background
        HBRUSH white = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        FillRect(mem_dc, &rc, white);
        // Border
        HPEN border = CreatePen(PS_SOLID, 1, RGB(180, 180, 180));
        HPEN old_pen = static_cast<HPEN>(SelectObject(mem_dc, border));
        HBRUSH old_br = static_cast<HBRUSH>(SelectObject(mem_dc, GetStockObject(NULL_BRUSH)));
        RoundRect(mem_dc, rc.left, rc.top, rc.right, rc.bottom, 14, 14);
        SelectObject(mem_dc, old_pen);
        SelectObject(mem_dc, old_br);
        DeleteObject(border);
        // Blit to screen
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem_dc, 0, 0, SRCCOPY);
        SelectObject(mem_dc, old_bmp);
        DeleteObject(mem_bmp);
        DeleteDC(mem_dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}
