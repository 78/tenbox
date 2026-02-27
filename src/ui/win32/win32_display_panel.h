#pragma once

#include "common/ports.h"
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// A Win32 child window that renders a VM framebuffer via StretchDIBits.
// When focused, captures keyboard and mouse input and forwards them to the VM.
class DisplayPanel {
public:
    using KeyEventCallback = std::function<void(uint32_t evdev_code, bool pressed)>;
    using PointerEventCallback = std::function<void(int32_t x, int32_t y, uint32_t buttons)>;
    using WheelEventCallback = std::function<void(int32_t delta)>;

    DisplayPanel();
    ~DisplayPanel();

    // Create the child window. Call once.
    bool Create(HWND parent, HINSTANCE hinst, int x, int y, int w, int h);

    void SetKeyCallback(KeyEventCallback cb);
    void SetPointerCallback(PointerEventCallback cb);
    void SetWheelCallback(WheelEventCallback cb);

    // Update the internal framebuffer with a dirty-rect frame.
    // Thread-safe; triggers InvalidateRect.
    void UpdateFrame(const DisplayFrame& frame);

    // Restore framebuffer from cached state (e.g. when switching back to a VM).
    void RestoreFramebuffer(uint32_t w, uint32_t h, const std::vector<uint8_t>& pixels);

    // Clear the framebuffer and cursor (e.g. when switching to a VM with no display).
    void Clear();

    // Update the cursor image and/or position.
    // Thread-safe; triggers InvalidateRect.
    void UpdateCursor(const CursorInfo& cursor);

    // Restore cursor from cached state (e.g. when switching back to a VM).
    void RestoreCursor(const CursorInfo& cursor, const std::vector<uint8_t>& pixels);

    // Move/resize the window.
    void SetBounds(int x, int y, int w, int h);

    HWND Handle() const { return hwnd_; }
    void SetVisible(bool visible);

    // Whether keyboard/mouse capture is active (panel has focus).
    bool IsCaptured() const { return captured_; }

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

private:
    void OnPaint();
    void HandleKey(UINT msg, WPARAM wp, LPARAM lp);
    void HandleMouse(UINT msg, WPARAM wp, LPARAM lp);
    void CalcDisplayRect(int cw, int ch, RECT* out) const;

    void SetCaptured(bool captured);
    void InstallKeyboardHook();
    void UninstallKeyboardHook();
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    bool captured_ = false;
    uint32_t mouse_buttons_ = 0;
    HHOOK kb_hook_ = nullptr;
    DWORD last_pointer_tick_ = 0;
    int32_t last_abs_x_ = -1;
    int32_t last_abs_y_ = -1;
    uint32_t last_sent_buttons_ = 0;
    static constexpr DWORD kPointerMinIntervalMs = 20;

    // Host-side framebuffer (full resource size)
    std::mutex fb_mutex_;
    uint32_t fb_width_ = 0;
    uint32_t fb_height_ = 0;
    std::vector<uint8_t> framebuffer_;

    // Cursor state
    std::mutex cursor_mutex_;
    HCURSOR custom_cursor_ = nullptr;

    KeyEventCallback key_cb_;
    PointerEventCallback pointer_cb_;
    WheelEventCallback wheel_cb_;
};
