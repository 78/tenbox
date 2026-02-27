#include "ui/win32/win32_display_panel.h"
#include "ui/win32/vk_to_evdev.h"
#include "ui/common/i18n.h"
#include <windowsx.h>
#include <algorithm>
#include <cstring>

static const char* kDisplayPanelClass = "TenBoxDisplayPanel";
static bool g_class_registered = false;

static constexpr int kHintBarHeight = 20;

// Global pointer so the low-level keyboard hook callback can reach the active panel.
// Only one DisplayPanel captures input at a time.
static DisplayPanel* g_captured_panel = nullptr;

DisplayPanel::DisplayPanel() = default;

DisplayPanel::~DisplayPanel() {
    SetCaptured(false);
    if (hwnd_) DestroyWindow(hwnd_);
    if (custom_cursor_) DestroyCursor(custom_cursor_);
}

static void RegisterPanelClass(HINSTANCE hinst) {
    if (g_class_registered) return;
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = DisplayPanel::WndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kDisplayPanelClass;
    RegisterClassExA(&wc);
    g_class_registered = true;
}

bool DisplayPanel::Create(HWND parent, HINSTANCE hinst, int x, int y, int w, int h) {
    RegisterPanelClass(hinst);
    hwnd_ = CreateWindowExA(
        WS_EX_CLIENTEDGE,
        kDisplayPanelClass,
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        x, y, w, h,
        parent, nullptr, hinst, this);
    return hwnd_ != nullptr;
}

void DisplayPanel::SetKeyCallback(KeyEventCallback cb) {
    key_cb_ = std::move(cb);
}

void DisplayPanel::SetPointerCallback(PointerEventCallback cb) {
    pointer_cb_ = std::move(cb);
}

void DisplayPanel::SetWheelCallback(WheelEventCallback cb) {
    wheel_cb_ = std::move(cb);
}

void DisplayPanel::UpdateFrame(const DisplayFrame& frame) {
    std::lock_guard<std::mutex> lock(fb_mutex_);

    uint32_t rw = frame.resource_width;
    uint32_t rh = frame.resource_height;
    if (rw == 0) rw = frame.width;
    if (rh == 0) rh = frame.height;

    // Resize framebuffer if resource dimensions changed
    if (fb_width_ != rw || fb_height_ != rh) {
        fb_width_ = rw;
        fb_height_ = rh;
        framebuffer_.resize(static_cast<size_t>(rw) * rh * 4, 0);
    }

    // Blit dirty rectangle into framebuffer
    uint32_t dx = frame.dirty_x;
    uint32_t dy = frame.dirty_y;
    uint32_t dw = frame.width;
    uint32_t dh = frame.height;
    uint32_t src_stride = dw * 4;
    uint32_t dst_stride = fb_width_ * 4;

    for (uint32_t row = 0; row < dh; ++row) {
        uint32_t src_off = row * src_stride;
        uint32_t dst_off = (dy + row) * dst_stride + dx * 4;
        if (src_off + src_stride > frame.pixels.size()) break;
        if (dst_off + dw * 4 > framebuffer_.size()) break;
        std::memcpy(framebuffer_.data() + dst_off,
                    frame.pixels.data() + src_off, dw * 4);
    }

    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void DisplayPanel::RestoreFramebuffer(uint32_t w, uint32_t h, const std::vector<uint8_t>& pixels) {
    std::lock_guard<std::mutex> lock(fb_mutex_);
    fb_width_ = w;
    fb_height_ = h;
    framebuffer_ = pixels;
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void DisplayPanel::Clear() {
    {
        std::lock_guard<std::mutex> lock(fb_mutex_);
        fb_width_ = 0;
        fb_height_ = 0;
        framebuffer_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(cursor_mutex_);
        if (custom_cursor_) {
            DestroyCursor(custom_cursor_);
            custom_cursor_ = nullptr;
        }
    }
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void DisplayPanel::UpdateCursor(const CursorInfo& cursor) {
    if (!cursor.image_updated || cursor.width == 0 || cursor.height == 0) {
        return;
    }

    HCURSOR new_cursor = nullptr;
    if (cursor.visible && !cursor.pixels.empty()) {
        uint32_t w = cursor.width;
        uint32_t h = cursor.height;

        BITMAPV5HEADER bi{};
        bi.bV5Size = sizeof(BITMAPV5HEADER);
        bi.bV5Width = static_cast<LONG>(w);
        bi.bV5Height = -static_cast<LONG>(h);  // Top-down
        bi.bV5Planes = 1;
        bi.bV5BitCount = 32;
        bi.bV5Compression = BI_BITFIELDS;
        bi.bV5RedMask = 0x00FF0000;
        bi.bV5GreenMask = 0x0000FF00;
        bi.bV5BlueMask = 0x000000FF;
        bi.bV5AlphaMask = 0xFF000000;

        HDC hdc = GetDC(nullptr);
        void* bits = nullptr;
        HBITMAP color_bmp = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi),
            DIB_RGB_COLORS, &bits, nullptr, 0);
        if (color_bmp && bits) {
            std::memcpy(bits, cursor.pixels.data(),
                (std::min)(cursor.pixels.size(), static_cast<size_t>(w * h * 4)));

            HBITMAP mask_bmp = CreateBitmap(static_cast<int>(w), static_cast<int>(h), 1, 1, nullptr);

            ICONINFO ii{};
            ii.fIcon = FALSE;
            ii.xHotspot = cursor.hot_x;
            ii.yHotspot = cursor.hot_y;
            ii.hbmMask = mask_bmp;
            ii.hbmColor = color_bmp;
            new_cursor = CreateIconIndirect(&ii);

            DeleteObject(mask_bmp);
            DeleteObject(color_bmp);
        }
        ReleaseDC(nullptr, hdc);
    }

    {
        std::lock_guard<std::mutex> lock(cursor_mutex_);
        if (custom_cursor_) {
            DestroyCursor(custom_cursor_);
        }
        custom_cursor_ = new_cursor;
    }

    if (hwnd_ && new_cursor) {
        SetCursor(new_cursor);
    }
}

void DisplayPanel::RestoreCursor(const CursorInfo& cursor, const std::vector<uint8_t>& pixels) {
    if (cursor.width == 0 || cursor.height == 0 || pixels.empty()) {
        return;
    }

    HCURSOR new_cursor = nullptr;
    if (cursor.visible) {
        uint32_t w = cursor.width;
        uint32_t h = cursor.height;

        BITMAPV5HEADER bi{};
        bi.bV5Size = sizeof(BITMAPV5HEADER);
        bi.bV5Width = static_cast<LONG>(w);
        bi.bV5Height = -static_cast<LONG>(h);
        bi.bV5Planes = 1;
        bi.bV5BitCount = 32;
        bi.bV5Compression = BI_BITFIELDS;
        bi.bV5RedMask = 0x00FF0000;
        bi.bV5GreenMask = 0x0000FF00;
        bi.bV5BlueMask = 0x000000FF;
        bi.bV5AlphaMask = 0xFF000000;

        HDC hdc = GetDC(nullptr);
        void* bits = nullptr;
        HBITMAP color_bmp = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi),
            DIB_RGB_COLORS, &bits, nullptr, 0);
        if (color_bmp && bits) {
            std::memcpy(bits, pixels.data(),
                (std::min)(pixels.size(), static_cast<size_t>(w * h * 4)));

            HBITMAP mask_bmp = CreateBitmap(static_cast<int>(w), static_cast<int>(h), 1, 1, nullptr);

            ICONINFO ii{};
            ii.fIcon = FALSE;
            ii.xHotspot = cursor.hot_x;
            ii.yHotspot = cursor.hot_y;
            ii.hbmMask = mask_bmp;
            ii.hbmColor = color_bmp;
            new_cursor = CreateIconIndirect(&ii);

            DeleteObject(mask_bmp);
            DeleteObject(color_bmp);
        }
        ReleaseDC(nullptr, hdc);
    }

    {
        std::lock_guard<std::mutex> lock(cursor_mutex_);
        if (custom_cursor_) {
            DestroyCursor(custom_cursor_);
        }
        custom_cursor_ = new_cursor;
    }

    if (hwnd_ && new_cursor) {
        SetCursor(new_cursor);
    }
}

void DisplayPanel::SetBounds(int x, int y, int w, int h) {
    if (hwnd_) MoveWindow(hwnd_, x, y, w, h, TRUE);
}

void DisplayPanel::SetVisible(bool visible) {
    if (hwnd_) ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
}

void DisplayPanel::CalcDisplayRect(int cw, int ch, RECT* out) const {
    if (fb_width_ == 0 || fb_height_ == 0 || cw <= 0 || ch <= 0) {
        *out = {0, 0, cw, ch};
        return;
    }
    double scale_x = static_cast<double>(cw) / fb_width_;
    double scale_y = static_cast<double>(ch) / fb_height_;
    double scale = (std::min)(scale_x, scale_y);
    int dw = static_cast<int>(fb_width_ * scale);
    int dh = static_cast<int>(fb_height_ * scale);
    int dx = (cw - dw) / 2;
    int dy = (ch - dh) / 2;
    *out = {dx, dy, dx + dw, dy + dh};
}

void DisplayPanel::OnPaint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd_, &ps);

    RECT rc;
    GetClientRect(hwnd_, &rc);
    int cw = rc.right;
    int ch = rc.bottom - kHintBarHeight;
    if (ch < 0) ch = 0;

    std::lock_guard<std::mutex> lock(fb_mutex_);
    if (fb_width_ > 0 && fb_height_ > 0 && !framebuffer_.empty()) {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(fb_width_);
        // Negative height = top-down DIB
        bmi.bmiHeader.biHeight = -static_cast<LONG>(fb_height_);
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        RECT dst;
        CalcDisplayRect(cw, ch, &dst);

        HBRUSH black = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        if (dst.left > 0) {
            RECT bar = {0, 0, dst.left, ch};
            FillRect(hdc, &bar, black);
        }
        if (dst.right < cw) {
            RECT bar = {dst.right, 0, cw, ch};
            FillRect(hdc, &bar, black);
        }
        if (dst.top > 0) {
            RECT bar = {dst.left, 0, dst.right, dst.top};
            FillRect(hdc, &bar, black);
        }
        if (dst.bottom < ch) {
            RECT bar = {dst.left, dst.bottom, dst.right, ch};
            FillRect(hdc, &bar, black);
        }

        SetStretchBltMode(hdc, HALFTONE);
        StretchDIBits(hdc,
            dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top,
            0, 0, static_cast<int>(fb_width_), static_cast<int>(fb_height_),
            framebuffer_.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
    } else {
        HBRUSH black = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        RECT fb_area = {0, 0, cw, ch};
        FillRect(hdc, &fb_area, black);
    }

    // Draw hint bar at bottom
    RECT hint_rc = {0, ch, rc.right, rc.bottom};
    COLORREF bg_color = captured_ ? RGB(0, 100, 180) : RGB(48, 48, 48);
    HBRUSH bg_brush = CreateSolidBrush(bg_color);
    FillRect(hdc, &hint_rc, bg_brush);
    DeleteObject(bg_brush);

    SetBkMode(hdc, TRANSPARENT);
    COLORREF text_color = captured_ ? RGB(255, 255, 255) : RGB(200, 200, 200);
    SetTextColor(hdc, text_color);
    const char* hint = captured_
        ? i18n::tr(i18n::S::kDisplayHintCaptured)
        : i18n::tr(i18n::S::kDisplayHintNormal);
    std::wstring hint_w = i18n::to_wide(hint);
    DrawTextW(hdc, hint_w.c_str(), -1, &hint_rc,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    EndPaint(hwnd_, &ps);
}

void DisplayPanel::HandleKey(UINT msg, WPARAM wp, LPARAM lp) {
    // When captured, low-level hook handles keys, so this is only called
    // in non-captured state. Allow normal key input even without capture.

    bool pressed = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
    uint32_t vk = static_cast<uint32_t>(wp);

    // Distinguish left/right modifier keys using scan code
    UINT scancode = (lp >> 16) & 0xFF;
    bool extended = (lp >> 24) & 1;

    // Use Right Alt to release capture (fallback if LL hook missed it)
    if (captured_ && pressed && (vk == VK_MENU || vk == VK_RMENU) && extended) {
        SetCaptured(false);
        return;
    }
    if (vk == VK_CONTROL) vk = extended ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU)    vk = extended ? VK_RMENU : VK_LMENU;
    if (vk == VK_SHIFT) {
        vk = (scancode == 0x36) ? VK_RSHIFT : VK_LSHIFT;
    }

    uint32_t evdev = VkToEvdev(vk);
    if (evdev && key_cb_) {
        key_cb_(evdev, pressed);
    }
}

void DisplayPanel::HandleMouse(UINT msg, WPARAM wp, LPARAM lp) {
    // Handle focus and capture mode on left click
    if (msg == WM_LBUTTONDOWN) {
        if (GetFocus() != hwnd_) {
            SetFocus(hwnd_);
        }
        // Enter capture mode for keyboard system keys
        if (!captured_) {
            SetCaptured(true);
        }
    }

    // Always send mouse events when mouse is over the panel (no focus required)

    uint32_t old_buttons = mouse_buttons_;

    switch (msg) {
    case WM_LBUTTONDOWN:   mouse_buttons_ |= 1; break;
    case WM_LBUTTONUP:     mouse_buttons_ &= ~1u; break;
    case WM_RBUTTONDOWN:   mouse_buttons_ |= 2; break;
    case WM_RBUTTONUP:     mouse_buttons_ &= ~2u; break;
    case WM_MBUTTONDOWN:   mouse_buttons_ |= 4; break;
    case WM_MBUTTONUP:     mouse_buttons_ &= ~4u; break;
    default: break;
    }

    bool buttons_changed = (mouse_buttons_ != old_buttons);

    int mx = GET_X_LPARAM(lp);
    int my = GET_Y_LPARAM(lp);

    RECT rc;
    GetClientRect(hwnd_, &rc);
    int cw = rc.right;
    int ch = rc.bottom - kHintBarHeight;
    if (cw <= 0 || ch <= 0) return;

    RECT dst;
    CalcDisplayRect(cw, ch, &dst);
    int dw = dst.right - dst.left;
    int dh = dst.bottom - dst.top;
    if (dw <= 0 || dh <= 0) return;

    int32_t abs_x = static_cast<int32_t>(
        static_cast<int64_t>(mx - dst.left) * 32767 / dw);
    int32_t abs_y = static_cast<int32_t>(
        static_cast<int64_t>(my - dst.top) * 32767 / dh);
    abs_x = (std::max)(0, (std::min)(abs_x, 32767));
    abs_y = (std::max)(0, (std::min)(abs_y, 32767));

    // Skip if nothing actually changed (handles spurious WM_MOUSEMOVE
    // that Windows can generate after InvalidateRect).
    if (!buttons_changed && abs_x == last_abs_x_ && abs_y == last_abs_y_)
        return;

    // Throttle pure move events to avoid flooding the pipe.
    // Button state changes are always sent immediately.
    if (!buttons_changed) {
        DWORD now = GetTickCount();
        if (now - last_pointer_tick_ < kPointerMinIntervalMs)
            return;
        last_pointer_tick_ = now;
    }

    last_abs_x_ = abs_x;
    last_abs_y_ = abs_y;
    last_sent_buttons_ = mouse_buttons_;

    if (pointer_cb_) {
        pointer_cb_(abs_x, abs_y, mouse_buttons_);
    }
}

void DisplayPanel::SetCaptured(bool captured) {
    if (captured_ == captured) return;
    captured_ = captured;
    if (captured) {
        InstallKeyboardHook();
    } else {
        UninstallKeyboardHook();
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void DisplayPanel::InstallKeyboardHook() {
    if (kb_hook_) return;
    g_captured_panel = this;
    kb_hook_ = SetWindowsHookExW(
        WH_KEYBOARD_LL, LowLevelKeyboardProc,
        GetModuleHandleW(nullptr), 0);
}

void DisplayPanel::UninstallKeyboardHook() {
    if (kb_hook_) {
        UnhookWindowsHookEx(kb_hook_);
        kb_hook_ = nullptr;
    }
    if (g_captured_panel == this) {
        g_captured_panel = nullptr;
    }
}

LRESULT CALLBACK DisplayPanel::LowLevelKeyboardProc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode == HC_ACTION && g_captured_panel && g_captured_panel->captured_) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        uint32_t vk = kb->vkCode;
        bool pressed = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        bool extended = (kb->flags & LLKHF_EXTENDED) != 0;

        // Right Alt releases capture
        if (pressed && (vk == VK_MENU || vk == VK_RMENU) && extended) {
            g_captured_panel->SetCaptured(false);
            return 1;
        }

        // Distinguish left/right modifiers
        if (vk == VK_CONTROL) vk = extended ? VK_RCONTROL : VK_LCONTROL;
        if (vk == VK_MENU)    vk = extended ? VK_RMENU : VK_LMENU;
        if (vk == VK_SHIFT) {
            vk = (kb->scanCode == 0x36) ? VK_RSHIFT : VK_LSHIFT;
        }

        uint32_t evdev = VkToEvdev(vk);
        if (evdev && g_captured_panel->key_cb_) {
            g_captured_panel->key_cb_(evdev, pressed);
        }

        // Swallow the key so the host OS does not act on it
        return 1;
    }
    return CallNextHookEx(nullptr, nCode, wp, lp);
}

LRESULT CALLBACK DisplayPanel::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DisplayPanel* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTA*>(lp);
        self = reinterpret_cast<DisplayPanel*>(cs->lpCreateParams);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<DisplayPanel*>(
            GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    }

    if (!self) return DefWindowProcA(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_PAINT:
        self->OnPaint();
        return 0;

    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        self->HandleKey(msg, wp, lp);
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEMOVE:
        self->HandleMouse(msg, wp, lp);
        return 0;

    case WM_MOUSEWHEEL:
        if (self->wheel_cb_) {
            int16_t delta = GET_WHEEL_DELTA_WPARAM(wp);
            self->wheel_cb_(delta / WHEEL_DELTA);
        }
        return 0;

    case WM_KILLFOCUS:
        self->SetCaptured(false);
        self->mouse_buttons_ = 0;
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            std::lock_guard<std::mutex> lock(self->cursor_mutex_);
            if (self->custom_cursor_) {
                SetCursor(self->custom_cursor_);
                return TRUE;
            }
        }
        break;

    case WM_ERASEBKGND:
        return 1;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}
