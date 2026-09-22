#include "manager/ui/win32_fullscreen_osd.h"

namespace {
constexpr const wchar_t* kOsdClass = L"TenBoxFullscreenOsd";
constexpr UINT_PTR kOsdTimerId = 1;
constexpr DWORD kOsdDurationMs = 1500;
}

HWND FullscreenOsd::Create(HINSTANCE hinst, HWND parent) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    wc.lpszClassName = kOsdClass;
    RegisterClassExW(&wc);

    return CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kOsdClass, L"",
        WS_POPUP,
        0, 0, 600, 100,
        parent, nullptr, hinst, nullptr);
}

void FullscreenOsd::Show(HWND hwnd, const std::wstring& text) {
    if (!hwnd) return;
    SetWindowTextW(hwnd, text.c_str());

    HWND parent = GetParent(hwnd);
    RECT prc;
    GetClientRect(parent, &prc);
    int w = 600, h = 100;
    int x = (prc.right - prc.left - w) / 2;
    int y = (prc.bottom - prc.top - h) / 2;

    SetLayeredWindowAttributes(hwnd, 0, 200, LWA_ALPHA);
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    SetTimer(hwnd, kOsdTimerId, kOsdDurationMs, nullptr);
    InvalidateRect(hwnd, nullptr, TRUE);
}

LRESULT CALLBACK FullscreenOsd::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        return 0;

    case WM_TIMER:
        if (wp == kOsdTimerId) {
            KillTimer(hwnd, kOsdTimerId);
            ShowWindow(hwnd, SW_HIDE);
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HBRUSH bg = CreateSolidBrush(RGB(32, 32, 32));
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(70, 70, 70));
        HBRUSH old_br = static_cast<HBRUSH>(SelectObject(hdc, bg));
        HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, pen));
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 12, 12);
        SelectObject(hdc, old_br);
        SelectObject(hdc, old_pen);
        DeleteObject(bg);
        DeleteObject(pen);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        LOGFONTW lf{};
        HFONT stock = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        GetObjectW(stock, sizeof(lf), &lf);
        lf.lfHeight = -36;  // 27pt
        lf.lfWeight = FW_SEMIBOLD;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        HFONT font = CreateFontIndirectW(&lf);
        HFONT old_font = static_cast<HFONT>(SelectObject(hdc, font));

        std::wstring text(256, L'\0');
        GetWindowTextW(hwnd, &text[0], 256);
        while (!text.empty() && text.back() == L'\0') text.pop_back();
        DrawTextW(hdc, text.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, old_font);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}
