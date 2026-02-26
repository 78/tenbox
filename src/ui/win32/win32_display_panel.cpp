#include "ui/win32/win32_display_panel.h"
#include "ui/win32/vk_to_evdev.h"
#include <windowsx.h>
#include <algorithm>
#include <cstring>

static const char* kDisplayPanelClass = "TenBoxDisplayPanel";
static bool g_class_registered = false;

static constexpr int kHintBarHeight = 20;

DisplayPanel::DisplayPanel() = default;

DisplayPanel::~DisplayPanel() {
    if (hwnd_) DestroyWindow(hwnd_);
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
    HBRUSH bg_brush = CreateSolidBrush(RGB(48, 48, 48));
    FillRect(hdc, &hint_rc, bg_brush);
    DeleteObject(bg_brush);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(200, 200, 200));
    const char* hint = captured_
        ? "Press Ctrl+Alt to release | Input captured"
        : "Click to capture keyboard & mouse";
    DrawTextA(hdc, hint, -1, &hint_rc,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    EndPaint(hwnd_, &ps);
}

void DisplayPanel::HandleKey(UINT msg, WPARAM wp, LPARAM lp) {
    if (!captured_) return;

    bool pressed = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
    uint32_t vk = static_cast<uint32_t>(wp);

    // Detect Ctrl+Alt release combo
    if (pressed && (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU)) {
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            captured_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }
    if (pressed && (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL)) {
        if (GetKeyState(VK_MENU) & 0x8000) {
            captured_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    // Distinguish left/right modifier keys using scan code
    UINT scancode = (lp >> 16) & 0xFF;
    bool extended = (lp >> 24) & 1;
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
    if (!captured_ && msg == WM_LBUTTONDOWN) {
        captured_ = true;
        SetFocus(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (!captured_) return;

    int mx = GET_X_LPARAM(lp);
    int my = GET_Y_LPARAM(lp);

    switch (msg) {
    case WM_LBUTTONDOWN:   mouse_buttons_ |= 1; break;
    case WM_LBUTTONUP:     mouse_buttons_ &= ~1u; break;
    case WM_RBUTTONDOWN:   mouse_buttons_ |= 2; break;
    case WM_RBUTTONUP:     mouse_buttons_ &= ~2u; break;
    case WM_MBUTTONDOWN:   mouse_buttons_ |= 4; break;
    case WM_MBUTTONUP:     mouse_buttons_ &= ~4u; break;
    default: break;
    }

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

    if (pointer_cb_) {
        pointer_cb_(abs_x, abs_y, mouse_buttons_);
    }
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

    case WM_KILLFOCUS:
        self->captured_ = false;
        self->mouse_buttons_ = 0;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}
