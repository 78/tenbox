#include "ui/win32/win32_ui_shell.h"
#include "ui/win32/win32_dialogs.h"
#include "ui/win32/win32_display_panel.h"
#include "ui/common/i18n.h"
#include "manager/app_settings.h"
#include "manager/resource.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <shlobj.h>

#pragma comment(lib, "comctl32.lib")

#include "common/ports.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ── Menu / toolbar command IDs ──

enum CmdId : UINT {
    IDM_NEW_VM      = 1001,
    IDM_EXIT        = 1002,
    IDM_START       = 1010,
    IDM_STOP        = 1011,
    IDM_REBOOT      = 1012,
    IDM_SHUTDOWN    = 1013,
    IDM_EDIT        = 1014,
    IDM_DELETE      = 1015,
};

// ── Control IDs ──

enum CtlId : UINT {
    IDC_TOOLBAR     = 2001,
    IDC_STATUSBAR   = 2002,
    IDC_LISTVIEW    = 2003,
    IDC_CONSOLE     = 2004,
    IDC_CONSOLE_IN  = 2005,
    IDC_SEND_BTN    = 2006,
    IDC_TAB         = 2007,
    IDC_SF_LISTVIEW = 2008,
    IDC_SF_ADD_BTN  = 2009,
    IDC_SF_DEL_BTN  = 2010,
};

// WM_APP range for cross-thread invoke
static constexpr UINT WM_INVOKE = WM_APP + 100;

// Track if we initiated the clipboard change (to avoid echo)
static bool g_clipboard_from_vm = false;

static constexpr int kLeftPaneWidth = 260;
static constexpr size_t kMaxConsoleLen = 32 * 1024;
static constexpr size_t kConsoleTrimAt = 24 * 1024;  // trim control when exceeding this

// ── Forward declarations for dialog helpers (win32_dialogs.cpp) ──

extern bool ShowCreateVmDialog(HWND parent, ManagerService& mgr, std::string* error);
extern bool ShowEditVmDialog(HWND parent, ManagerService& mgr,
                             const VmRecord& rec, std::string* error);

// ── Static singleton HWND (needed for InvokeOnUiThread) ──

static HWND g_main_hwnd = nullptr;
static std::mutex g_invoke_mutex;
static std::deque<std::function<void()>> g_invoke_queue;

// ── Helpers ──

static bool IsVmRunning(VmPowerState s) {
    return s == VmPowerState::kRunning || s == VmPowerState::kStarting
        || s == VmPowerState::kStopping;
}

static const char* StateText(VmPowerState s) {
    using S = i18n::S;
    switch (s) {
    case VmPowerState::kRunning:  return i18n::tr(S::kStateRunning);
    case VmPowerState::kStarting: return i18n::tr(S::kStateStarting);
    case VmPowerState::kStopping: return i18n::tr(S::kStateStopping);
    case VmPowerState::kCrashed:  return i18n::tr(S::kStateCrashed);
    default:                      return i18n::tr(S::kStateStopped);
    }
}

// ── Per-VM UI state cache ──

enum class AnsiState { kNormal, kEsc, kCsi };

struct VmUiState {
    int current_tab = 0;  // kTabInfo / kTabConsole / kTabDisplay (set after constants)

    std::string console_text;
    AnsiState ansi_state = AnsiState::kNormal;

    uint32_t fb_width = 0;
    uint32_t fb_height = 0;
    std::vector<uint8_t> framebuffer;

    CursorInfo cursor;
    std::vector<uint8_t> cursor_pixels;
};

// ── PIMPL ──

struct Win32UiShell::Impl {
    HWND hwnd       = nullptr;
    HWND toolbar    = nullptr;
    HWND statusbar  = nullptr;
    HWND listview   = nullptr;
    HWND console    = nullptr;
    HWND console_in = nullptr;
    HWND send_btn   = nullptr;
    HWND tab        = nullptr;
    HWND sf_list    = nullptr;
    HWND sf_add_btn = nullptr;
    HWND sf_del_btn = nullptr;
    HMENU menu_bar  = nullptr;

    bool display_available = false;

    // Track last sent display size to avoid redundant messages
    uint32_t last_sent_display_w = 0;
    uint32_t last_sent_display_h = 0;

    // Pending display size for debounced resize (send after resize stops)
    uint32_t pending_display_w = 0;
    uint32_t pending_display_h = 0;
    UINT_PTR resize_timer_id = 0;
    static constexpr UINT kResizeTimerId = 9001;
    static constexpr UINT kResizeDebounceMs = 500;

    static constexpr int kDetailRows = 7;
    HWND detail_labels[kDetailRows] = {};
    HWND detail_values[kDetailRows] = {};

    HFONT ui_font     = nullptr;
    HFONT mono_font   = nullptr;

    std::unique_ptr<DisplayPanel> display_panel;

    std::vector<VmRecord> records;
    int selected_index = -1;

    std::unordered_map<std::string, VmUiState> vm_ui_states;

    VmUiState& GetVmUiState(const std::string& vm_id) {
        return vm_ui_states[vm_id];
    }

    void ResetConsoleForVm(const std::string& vm_id) {
        auto it = vm_ui_states.find(vm_id);
        if (it != vm_ui_states.end()) {
            it->second.console_text.clear();
            it->second.ansi_state = AnsiState::kNormal;
        }
    }

    std::string AppendConsoleDataToState(VmUiState& state, const std::string& raw) {
        std::string added;
        for (unsigned char ch : raw) {
            switch (state.ansi_state) {
            case AnsiState::kNormal:
                if (ch == 0x1b) { state.ansi_state = AnsiState::kEsc; break; }
                if (ch == '\n') { state.console_text += "\r\n"; added += "\r\n"; break; }
                if (ch == '\t') { state.console_text.push_back('\t'); added.push_back('\t'); break; }
                if (ch >= 0x20 && ch <= 0x7e) { state.console_text.push_back(ch); added.push_back(ch); break; }
                break;
            case AnsiState::kEsc:
                state.ansi_state = (ch == '[') ? AnsiState::kCsi : AnsiState::kNormal;
                break;
            case AnsiState::kCsi:
                if (ch >= 0x40 && ch <= 0x7e) state.ansi_state = AnsiState::kNormal;
                break;
            }
        }
        if (state.console_text.size() > kMaxConsoleLen) {
            state.console_text.erase(0, state.console_text.size() - kMaxConsoleLen);
        }
        return added;
    }
};

// Blit incoming DisplayFrame into VmUiState framebuffer (for caching).
static void BlitFrameToState(VmUiState& state, const DisplayFrame& frame) {
    uint32_t rw = frame.resource_width;
    uint32_t rh = frame.resource_height;
    if (rw == 0) rw = frame.width;
    if (rh == 0) rh = frame.height;

    if (state.fb_width != rw || state.fb_height != rh) {
        state.fb_width = rw;
        state.fb_height = rh;
        state.framebuffer.resize(static_cast<size_t>(rw) * rh * 4, 0);
    }

    uint32_t dx = frame.dirty_x;
    uint32_t dy = frame.dirty_y;
    uint32_t dw = frame.width;
    uint32_t dh = frame.height;
    uint32_t src_stride = dw * 4;
    uint32_t dst_stride = state.fb_width * 4;

    for (uint32_t row = 0; row < dh; ++row) {
        uint32_t src_off = row * src_stride;
        uint32_t dst_off = (dy + row) * dst_stride + dx * 4;
        if (src_off + src_stride > frame.pixels.size()) break;
        if (dst_off + dw * 4 > state.framebuffer.size()) break;
        std::memcpy(state.framebuffer.data() + dst_off,
                    frame.pixels.data() + src_off, dw * 4);
    }
}

// ── Window class registration ──

static const char* kWndClass = "TenBoxManagerWin32";
static LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);

static ATOM RegisterMainClass(HINSTANCE hinst) {
    WNDCLASSEXA wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWndClass;
    wc.hIcon         = static_cast<HICON>(LoadImageA(hinst, MAKEINTRESOURCEA(IDI_APP_ICON),
                           IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    wc.hIconSm       = static_cast<HICON>(LoadImageA(hinst, MAKEINTRESOURCEA(IDI_APP_ICON),
                           IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                           GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    return RegisterClassExA(&wc);
}

// ── Menu building ──

static HMENU BuildMenuBar() {
    using S = i18n::S;
    HMENU bar = CreateMenu();

    HMENU file_menu = CreatePopupMenu();
    AppendMenuA(file_menu, MF_STRING, IDM_NEW_VM, i18n::tr(S::kMenuNewVm));
    AppendMenuA(file_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(file_menu, MF_STRING, IDM_EXIT, i18n::tr(S::kMenuExit));
    AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file_menu), i18n::tr(S::kMenuManager));

    HMENU vm_menu = CreatePopupMenu();
    AppendMenuA(vm_menu, MF_STRING, IDM_EDIT,     i18n::tr(S::kMenuEdit));
    AppendMenuA(vm_menu, MF_STRING, IDM_DELETE,   i18n::tr(S::kMenuDelete));
    AppendMenuA(vm_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(vm_menu, MF_STRING, IDM_START,    i18n::tr(S::kMenuStart));
    AppendMenuA(vm_menu, MF_STRING, IDM_STOP,     i18n::tr(S::kMenuStop));
    AppendMenuA(vm_menu, MF_STRING, IDM_REBOOT,   i18n::tr(S::kMenuReboot));
    AppendMenuA(vm_menu, MF_STRING, IDM_SHUTDOWN, i18n::tr(S::kMenuShutdown));
    AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(vm_menu), i18n::tr(S::kMenuVm));

    return bar;
}

// ── Toolbar building ──

static constexpr int kToolbarIconSize = 48;

static HIMAGELIST CreateToolbarImageList(HINSTANCE hinst) {
    const UINT bmp_ids[] = {
        IDB_TOOLBAR_NEW_VM,
        IDB_TOOLBAR_EDIT,
        IDB_TOOLBAR_DELETE,
        IDB_TOOLBAR_START,
        IDB_TOOLBAR_STOP,
        IDB_TOOLBAR_REBOOT,
        IDB_TOOLBAR_SHUTDOWN,
    };

    HIMAGELIST hil = ImageList_Create(
        kToolbarIconSize, kToolbarIconSize,
        ILC_COLOR32 | ILC_MASK, 7, 0);
    if (!hil) return nullptr;

    for (UINT id : bmp_ids) {
        HBITMAP hbm = static_cast<HBITMAP>(LoadImageA(
            hinst, MAKEINTRESOURCEA(id), IMAGE_BITMAP,
            kToolbarIconSize, kToolbarIconSize, LR_CREATEDIBSECTION));
        if (hbm) {
            ImageList_AddMasked(hil, hbm, RGB(255, 0, 255));
            DeleteObject(hbm);
        } else {
            ImageList_Destroy(hil);
            return nullptr;
        }
    }
    return hil;
}

static HWND CreateToolbar(HWND parent, HINSTANCE hinst) {
    HWND tb = CreateWindowExA(0, TOOLBARCLASSNAMEA, nullptr,
        WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | CCS_TOP,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_TOOLBAR), hinst, nullptr);

    SendMessage(tb, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);

    HIMAGELIST hil = CreateToolbarImageList(hinst);
    bool has_icons = (hil != nullptr);
    if (has_icons) {
        SendMessage(tb, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(hil));
        SendMessage(tb, TB_SETPADDING, 0, MAKELPARAM(20, 12));
    } else {
        SendMessage(tb, TB_SETPADDING, 0, MAKELPARAM(16, 6));
    }

    using S = i18n::S;
    const struct { UINT id; const char* text; int icon_idx; } items[] = {
        {IDM_NEW_VM,   i18n::tr(S::kToolbarNewVm),   0},
        {IDM_EDIT,     i18n::tr(S::kToolbarEdit),    1},
        {IDM_DELETE,   i18n::tr(S::kToolbarDelete),  2},
        {0,            nullptr,                      -1},
        {IDM_START,    i18n::tr(S::kToolbarStart),   3},
        {IDM_STOP,     i18n::tr(S::kToolbarStop),    4},
        {IDM_REBOOT,   i18n::tr(S::kToolbarReboot),  5},
        {IDM_SHUTDOWN, i18n::tr(S::kToolbarShutdown),6},
    };

    for (const auto& item : items) {
        TBBUTTON btn{};
        if (item.id == 0) {
            btn.fsStyle = BTNS_SEP;
        } else {
            btn.iBitmap   = has_icons ? item.icon_idx : I_IMAGENONE;
            btn.idCommand = item.id;
            btn.fsState   = TBSTATE_ENABLED;
            btn.fsStyle   = BTNS_BUTTON | BTNS_AUTOSIZE;
            btn.iString   = reinterpret_cast<INT_PTR>(item.text);
        }
        SendMessageA(tb, TB_ADDBUTTONSA, 1, reinterpret_cast<LPARAM>(&btn));
    }

    SendMessage(tb, TB_AUTOSIZE, 0, 0);
    return tb;
}

// ── ListBox setup (owner-draw, each VM shown as multi-line card) ──

static constexpr int kListItemHeight = 80;

static HWND CreateVmListBox(HWND parent, HINSTANCE hinst) {
    HWND lb = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_LISTVIEW), hinst, nullptr);
    SendMessageA(lb, LB_SETITEMHEIGHT, 0, MAKELPARAM(kListItemHeight, 0));
    return lb;
}

// ── Layout helper ──

using Impl = Win32UiShell::Impl;

static constexpr int kTabInfo     = 0;
static constexpr int kTabConsole  = 1;
static constexpr int kTabDisplay  = 2;
static constexpr int kTabSharedFs = 3;

static void ShowDetailControls(Impl* p, bool visible) {
    int cmd = visible ? SW_SHOW : SW_HIDE;
    for (int i = 0; i < Impl::kDetailRows; ++i) {
        ShowWindow(p->detail_labels[i], cmd);
        ShowWindow(p->detail_values[i], cmd);
    }
}

static void ShowSharedFsControls(Impl* p, bool visible) {
    int cmd = visible ? SW_SHOW : SW_HIDE;
    ShowWindow(p->sf_list, cmd);
    ShowWindow(p->sf_add_btn, cmd);
    ShowWindow(p->sf_del_btn, cmd);
}

static void RefreshSharedFsList(Impl* p, ManagerService& mgr) {
    if (!p->sf_list) return;
    
    ListView_DeleteAllItems(p->sf_list);
    
    if (p->selected_index < 0 || p->selected_index >= static_cast<int>(p->records.size())) {
        return;
    }
    
    const auto& vm = p->records[p->selected_index];
    auto folders = mgr.GetSharedFolders(vm.spec.vm_id);
    
    for (size_t i = 0; i < folders.size(); ++i) {
        const auto& sf = folders[i];
        
        LVITEMA item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.iSubItem = 0;
        item.pszText = const_cast<char*>(sf.tag.c_str());
        int idx = ListView_InsertItem(p->sf_list, &item);
        
        ListView_SetItemText(p->sf_list, idx, 1, const_cast<char*>(sf.host_path.c_str()));
        ListView_SetItemText(p->sf_list, idx, 2, const_cast<char*>(sf.readonly ? "Read Only" : "Read/Write"));
    }
}

static constexpr int kDisplayHintBarHeight = 20;

// Resize window to fit VM display at 1:1 pixel ratio
static void ResizeWindowForDisplay(Impl* p, uint32_t vm_width, uint32_t vm_height) {
    if (!p->hwnd || vm_width == 0 || vm_height == 0) return;

    // Align width to 8 pixels (same as LayoutControls)
    vm_width = vm_width & ~7u;

    // If user is actively resizing (pending timer), don't let VM callback
    // override the user's intended size - this prevents the window from
    // jumping back to an old size while user is still dragging
    if (p->resize_timer_id != 0) {
        return;
    }

    // Pre-update last_sent values to prevent LayoutControls from triggering
    // a new SetDisplaySize request (avoids feedback loop)
    p->last_sent_display_w = vm_width;
    p->last_sent_display_h = vm_height;

    // Get current toolbar and statusbar heights
    RECT tbr, sbr;
    GetWindowRect(p->toolbar, &tbr);
    GetWindowRect(p->statusbar, &sbr);
    int tb_h = tbr.bottom - tbr.top;
    int sb_h = sbr.bottom - sbr.top;

    // Calculate tab control padding
    RECT tab_padding = {0, 0, 100, 100};
    SendMessage(p->tab, TCM_ADJUSTRECT, TRUE, reinterpret_cast<LPARAM>(&tab_padding));
    int tab_extra_w = tab_padding.right - tab_padding.left - 100;
    int tab_extra_h = tab_padding.bottom - tab_padding.top - 100;

    // Target client area size:
    // Width = left pane + gap + tab padding + display width
    // Height = toolbar + tab padding + display height + hint bar + statusbar
    int target_cw = kLeftPaneWidth + 2 + tab_extra_w + static_cast<int>(vm_width);
    int target_ch = tb_h + tab_extra_h + static_cast<int>(vm_height) + kDisplayHintBarHeight + sb_h;

    // Convert client size to window size
    RECT wr = {0, 0, target_cw, target_ch};
    DWORD style = static_cast<DWORD>(GetWindowLongPtr(p->hwnd, GWL_STYLE));
    DWORD ex_style = static_cast<DWORD>(GetWindowLongPtr(p->hwnd, GWL_EXSTYLE));
    AdjustWindowRectEx(&wr, style, TRUE, ex_style);

    int new_w = wr.right - wr.left;
    int new_h = wr.bottom - wr.top;

    // Clamp to screen work area
    HMONITOR hmon = MonitorFromWindow(p->hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfo(hmon, &mi);
    int max_w = mi.rcWork.right - mi.rcWork.left;
    int max_h = mi.rcWork.bottom - mi.rcWork.top;
    if (new_w > max_w) new_w = max_w;
    if (new_h > max_h) new_h = max_h;

    // Get current position and adjust if needed to stay on screen
    RECT cur_wr;
    GetWindowRect(p->hwnd, &cur_wr);
    int x = cur_wr.left;
    int y = cur_wr.top;
    if (x + new_w > mi.rcWork.right) x = mi.rcWork.right - new_w;
    if (y + new_h > mi.rcWork.bottom) y = mi.rcWork.bottom - new_h;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;

    SetWindowPos(p->hwnd, nullptr, x, y, new_w, new_h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// Singleton shell pointer for use in static functions
static Win32UiShell* g_shell = nullptr;

static void LayoutControls(Impl* p) {
    if (!p->hwnd) return;

    RECT rc;
    GetClientRect(p->hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;

    SendMessage(p->toolbar, TB_AUTOSIZE, 0, 0);
    RECT tbr;
    GetWindowRect(p->toolbar, &tbr);
    int tb_h = tbr.bottom - tbr.top;

    SendMessage(p->statusbar, WM_SIZE, 0, 0);
    RECT sbr;
    GetWindowRect(p->statusbar, &sbr);
    int sb_h = sbr.bottom - sbr.top;

    int content_top = tb_h;
    int content_h   = ch - tb_h - sb_h;
    if (content_h < 0) content_h = 0;

    MoveWindow(p->listview, 0, content_top, kLeftPaneWidth, content_h, TRUE);

    int rx = kLeftPaneWidth + 2;
    int rw = cw - rx;
    if (rw < 0) rw = 0;

    // Hide all right-pane controls first
    ShowDetailControls(p, false);
    ShowWindow(p->console, SW_HIDE);
    ShowWindow(p->console_in, SW_HIDE);
    ShowWindow(p->send_btn, SW_HIDE);
    ShowSharedFsControls(p, false);
    if (p->display_panel) p->display_panel->SetVisible(false);

    // Hide tab control when no VM is selected
    bool has_selection = p->selected_index >= 0 &&
                         p->selected_index < static_cast<int>(p->records.size());
    if (!has_selection) {
        ShowWindow(p->tab, SW_HIDE);
        return;
    }

    ShowWindow(p->tab, SW_SHOW);
    MoveWindow(p->tab, rx, content_top, rw, content_h, TRUE);

    RECT page_rc = {0, 0, rw, content_h};
    SendMessage(p->tab, TCM_ADJUSTRECT, FALSE, reinterpret_cast<LPARAM>(&page_rc));
    int px = rx + page_rc.left;
    int py = content_top + page_rc.top;
    int pw = page_rc.right - page_rc.left;
    int ph = page_rc.bottom - page_rc.top;
    if (pw < 0) pw = 0;
    if (ph < 0) ph = 0;

    int cur_tab = static_cast<int>(SendMessage(p->tab, TCM_GETCURSEL, 0, 0));

    if (cur_tab == kTabInfo) {
        ShowDetailControls(p, true);

        HDC hdc = GetDC(p->hwnd);
        HFONT old_font = static_cast<HFONT>(SelectObject(hdc, p->ui_font));
        TEXTMETRICA tm{};
        GetTextMetricsA(hdc, &tm);

        const char* kLabels[] = {
            i18n::tr(i18n::S::kLabelId), i18n::tr(i18n::S::kLabelLocation),
            i18n::tr(i18n::S::kLabelKernel), i18n::tr(i18n::S::kLabelDisk),
            i18n::tr(i18n::S::kLabelMemory), i18n::tr(i18n::S::kLabelVcpus),
            i18n::tr(i18n::S::kLabelNat)
        };
        int label_w = 0;
        for (const char* lbl : kLabels) {
            SIZE sz{};
            GetTextExtentPoint32A(hdc, lbl, static_cast<int>(strlen(lbl)), &sz);
            if (sz.cx > label_w) label_w = sz.cx;
        }
        label_w += 12;
        SelectObject(hdc, old_font);
        ReleaseDC(p->hwnd, hdc);

        int row_h = tm.tmHeight + tm.tmExternalLeading + 2;
        int row_gap = row_h + 6;
        int val_x = px + 8 + label_w + 8;
        int val_w = pw - (label_w + 24);
        if (val_w < 40) val_w = 40;

        int dy = py + 8;
        for (int i = 0; i < Impl::kDetailRows; ++i) {
            MoveWindow(p->detail_labels[i], px + 8, dy, label_w, row_h, TRUE);
            MoveWindow(p->detail_values[i], val_x, dy, val_w, row_h, TRUE);
            dy += row_gap;
        }
    } else if (cur_tab == kTabConsole) {
        ShowWindow(p->console, SW_SHOW);
        ShowWindow(p->console_in, SW_SHOW);
        ShowWindow(p->send_btn, SW_SHOW);

        int input_h = 24;
        int send_w = 60;
        int gap = 4;
        int console_h = ph - input_h - gap;
        if (console_h < 20) console_h = 20;

        MoveWindow(p->console, px, py, pw, console_h, TRUE);
        int input_y = py + console_h + gap;
        MoveWindow(p->console_in, px, input_y, pw - send_w - gap, input_h, TRUE);
        MoveWindow(p->send_btn, px + pw - send_w, input_y, send_w, input_h, TRUE);
    } else if (cur_tab == kTabDisplay) {
        if (p->display_panel) {
            p->display_panel->SetVisible(true);
            p->display_panel->SetBounds(px, py, pw, ph);

            // Compute display area size (subtract hint bar height)
            // Align width to 8 pixels for GPU/DRM compatibility
            uint32_t disp_w = pw > 0 ? (static_cast<uint32_t>(pw) & ~7u) : 0;
            uint32_t disp_h = ph > kDisplayHintBarHeight
                ? static_cast<uint32_t>(ph - kDisplayHintBarHeight) : 0;

            // Debounce: schedule resize notification after user stops resizing
            if (p->display_available && disp_w > 0 && disp_h > 0 &&
                (disp_w != p->last_sent_display_w || disp_h != p->last_sent_display_h)) {
                p->pending_display_w = disp_w;
                p->pending_display_h = disp_h;
                // Reset timer on each resize event (debounce)
                if (p->resize_timer_id) {
                    KillTimer(p->hwnd, Impl::kResizeTimerId);
                }
                p->resize_timer_id = SetTimer(p->hwnd, Impl::kResizeTimerId,
                    Impl::kResizeDebounceMs, nullptr);
            }
        }
    } else if (cur_tab == kTabSharedFs) {
        ShowSharedFsControls(p, true);

        int btn_h = 26;
        int btn_w = 80;
        int gap = 4;
        int list_h = ph - btn_h - gap;
        if (list_h < 50) list_h = 50;

        MoveWindow(p->sf_list, px, py, pw, list_h, TRUE);
        int btn_y = py + list_h + gap;
        MoveWindow(p->sf_add_btn, px, btn_y, btn_w, btn_h, TRUE);
        MoveWindow(p->sf_del_btn, px + btn_w + gap, btn_y, btn_w, btn_h, TRUE);
    }
}

// ── Update detail panel from selected VM ──

static void UpdateDetailPanel(Impl* p) {
    using S = i18n::S;
    const char* labels[] = {
        i18n::tr(S::kLabelId), i18n::tr(S::kLabelLocation),
        i18n::tr(S::kLabelKernel), i18n::tr(S::kLabelDisk),
        i18n::tr(S::kLabelMemory), i18n::tr(S::kLabelVcpus),
        i18n::tr(S::kLabelNat)
    };
    for (int i = 0; i < Impl::kDetailRows; ++i)
        SetWindowTextA(p->detail_labels[i], labels[i]);

    if (p->selected_index < 0 ||
        p->selected_index >= static_cast<int>(p->records.size())) {
        for (int i = 0; i < Impl::kDetailRows; ++i)
            SetWindowTextA(p->detail_values[i], "");
        return;
    }

    const auto& spec = p->records[p->selected_index].spec;
    auto mb_str = std::to_string(spec.memory_mb) + " MB";
    auto cpu_str = std::to_string(spec.cpu_count);

    SetWindowTextA(p->detail_values[0], spec.vm_id.c_str());
    SetWindowTextA(p->detail_values[1], spec.vm_dir.c_str());
    SetWindowTextA(p->detail_values[2], spec.kernel_path.c_str());
    SetWindowTextA(p->detail_values[3],
        spec.disk_path.empty() ? i18n::tr(S::kNone) : spec.disk_path.c_str());
    SetWindowTextA(p->detail_values[4], mb_str.c_str());
    SetWindowTextA(p->detail_values[5], cpu_str.c_str());
    SetWindowTextA(p->detail_values[6], spec.nat_enabled ? i18n::tr(S::kNatEnabled) : i18n::tr(S::kNatDisabled));
}

// ── Update toolbar/menu enable state ──

static void UpdateCommandStates(Win32UiShell::Impl* p) {
    bool has_sel = p->selected_index >= 0 &&
                   p->selected_index < static_cast<int>(p->records.size());
    bool running = has_sel && IsVmRunning(p->records[p->selected_index].state);
    bool stopping = has_sel && p->records[p->selected_index].state == VmPowerState::kStopping;

    auto EnableCmd = [&](UINT id, bool en) {
        SendMessage(p->toolbar, TB_ENABLEBUTTON, id, MAKELONG(en ? TRUE : FALSE, 0));
        HMENU vm_menu = GetSubMenu(p->menu_bar, 1);
        if (vm_menu) {
            EnableMenuItem(vm_menu, id, en ? MF_ENABLED : MF_GRAYED);
        }
    };

    EnableCmd(IDM_START,    has_sel && !running);
    EnableCmd(IDM_STOP,     has_sel && running);
    EnableCmd(IDM_REBOOT,   has_sel && running && !stopping);
    EnableCmd(IDM_SHUTDOWN, has_sel && running && !stopping);
    EnableCmd(IDM_EDIT,     has_sel);
    EnableCmd(IDM_DELETE,   has_sel && !running);

    EnableWindow(p->console_in, running);
    EnableWindow(p->send_btn, running);
}

// ── Populate ListBox from cached records ──

static void PopulateListBox(Win32UiShell::Impl* p) {
    SendMessageA(p->listview, WM_SETREDRAW, FALSE, 0);
    SendMessageA(p->listview, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < static_cast<int>(p->records.size()); ++i) {
        SendMessageA(p->listview, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(p->records[i].spec.name.c_str()));
    }
    if (p->selected_index >= 0 &&
        p->selected_index < static_cast<int>(p->records.size())) {
        SendMessageA(p->listview, LB_SETCURSEL, p->selected_index, 0);
    }
    SendMessageA(p->listview, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(p->listview, nullptr, TRUE);
}

// ── WndProc ──

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* shell = g_shell;
    if (!shell) return DefWindowProcA(hwnd, msg, wp, lp);
    auto* p = reinterpret_cast<Win32UiShell::Impl*>(
        GetWindowLongPtrA(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_SIZE:
        if (p) LayoutControls(p);
        return 0;

    case WM_TIMER:
        if (p && wp == Impl::kResizeTimerId) {
            KillTimer(hwnd, Impl::kResizeTimerId);
            p->resize_timer_id = 0;
            // Send the debounced display size to VM
            if (p->pending_display_w > 0 && p->pending_display_h > 0 &&
                (p->pending_display_w != p->last_sent_display_w ||
                 p->pending_display_h != p->last_sent_display_h)) {
                p->last_sent_display_w = p->pending_display_w;
                p->last_sent_display_h = p->pending_display_h;
                if (p->selected_index >= 0 &&
                    p->selected_index < static_cast<int>(p->records.size())) {
                    const auto& vm_id = p->records[p->selected_index].spec.vm_id;
                    shell->manager_.SetDisplaySize(vm_id, p->pending_display_w, p->pending_display_h);
                }
            }
        }
        return 0;

    case WM_COMMAND: {
        UINT cmd = LOWORD(wp);
        UINT code = HIWORD(wp);

        // Send button or Enter in console input
        if ((cmd == IDC_SEND_BTN && code == BN_CLICKED) ||
            (cmd == IDC_CONSOLE_IN && code == EN_CHANGE && false)) {
            // handled below via accelerator
        }

        // ListBox selection change: save current VM state, restore selected VM state
        if (cmd == IDC_LISTVIEW && code == LBN_SELCHANGE) {
            int sel = static_cast<int>(SendMessageA(p->listview, LB_GETCURSEL, 0, 0));
            if (sel != LB_ERR && sel != p->selected_index) {
                // Save current tab for the VM we are leaving
                if (p->selected_index >= 0 &&
                    p->selected_index < static_cast<int>(p->records.size())) {
                    int cur_tab = static_cast<int>(SendMessage(p->tab, TCM_GETCURSEL, 0, 0));
                    const std::string& old_vm_id = p->records[p->selected_index].spec.vm_id;
                    p->GetVmUiState(old_vm_id).current_tab = cur_tab;
                }

                p->selected_index = sel;
                const std::string& new_vm_id = p->records[sel].spec.vm_id;
                VmUiState& new_state = p->GetVmUiState(new_vm_id);

                UpdateDetailPanel(p);
                UpdateCommandStates(p);

                SendMessage(p->tab, TCM_SETCURSEL, new_state.current_tab, 0);

                SetWindowTextA(p->console, new_state.console_text.c_str());

                p->display_available = (new_state.fb_width > 0 && new_state.fb_height > 0);
                if (p->display_available && !new_state.framebuffer.empty()) {
                    p->display_panel->RestoreFramebuffer(
                        new_state.fb_width, new_state.fb_height, new_state.framebuffer);
                    if (!new_state.cursor_pixels.empty()) {
                        p->display_panel->RestoreCursor(new_state.cursor, new_state.cursor_pixels);
                    }
                } else {
                    p->display_panel->Clear();
                }

                // Refresh shared folders list if that tab is visible
                if (new_state.current_tab == kTabSharedFs) {
                    RefreshSharedFsList(p, shell->manager_);
                }

                LayoutControls(p);
            }
            return 0;
        }

        if (cmd == IDC_SEND_BTN && code == BN_CLICKED) {
            char buf[1024]{};
            GetWindowTextA(p->console_in, buf, sizeof(buf));
            std::string input(buf);
            if (!input.empty() && p->selected_index >= 0 &&
                p->selected_index < static_cast<int>(p->records.size())) {
                std::string vm_id = p->records[p->selected_index].spec.vm_id;
                std::string to_send = input + "\n";
                if (shell->manager_.SendConsoleInput(vm_id, to_send)) {
                    SetWindowTextA(p->console_in, "");
                }
            }
            return 0;
        }

        switch (cmd) {
        case IDM_NEW_VM: {
            std::string error;
            if (ShowCreateVmDialog(hwnd, shell->manager_, &error)) {
                shell->RefreshVmList();
            } else if (!error.empty()) {
                MessageBoxA(hwnd, error.c_str(), i18n::tr(i18n::S::kError), MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        case IDM_EXIT:
            SendMessage(hwnd, WM_CLOSE, 0, 0);
            return 0;
        case IDM_START: {
            if (p->selected_index < 0 ||
                p->selected_index >= static_cast<int>(p->records.size()))
                break;
            std::string vm_id = p->records[p->selected_index].spec.vm_id;
            p->ResetConsoleForVm(vm_id);
            VmUiState& state = p->GetVmUiState(vm_id);
            state.current_tab = kTabConsole;
            SetWindowTextA(p->console, "");
            p->display_available = false;
            SendMessage(p->tab, TCM_SETCURSEL, kTabConsole, 0);
            LayoutControls(p);
            auto status = i18n::fmt(i18n::S::kStatusStarting, vm_id.c_str());
            SendMessageA(p->statusbar, SB_SETTEXTA, 0, reinterpret_cast<LPARAM>(status.c_str()));
            std::string error;
            bool ok = shell->manager_.StartVm(vm_id, &error);
            shell->RefreshVmList();
            status = ok ? i18n::fmt(i18n::S::kStatusStarted, vm_id.c_str())
                        : (std::string(i18n::tr(i18n::S::kStatusErrorPrefix)) + error);
            SendMessageA(p->statusbar, SB_SETTEXTA, 0, reinterpret_cast<LPARAM>(status.c_str()));
            return 0;
        }
        case IDM_STOP: {
            if (p->selected_index < 0 ||
                p->selected_index >= static_cast<int>(p->records.size()))
                break;
            std::string vm_id = p->records[p->selected_index].spec.vm_id;
            std::string error;
            bool ok = shell->manager_.StopVm(vm_id, &error);
            shell->RefreshVmList();
            auto status = ok ? i18n::fmt(i18n::S::kStatusStopped, vm_id.c_str())
                             : (std::string(i18n::tr(i18n::S::kStatusErrorPrefix)) + error);
            SendMessageA(p->statusbar, SB_SETTEXTA, 0, reinterpret_cast<LPARAM>(status.c_str()));
            return 0;
        }
        case IDM_REBOOT: {
            if (p->selected_index < 0 ||
                p->selected_index >= static_cast<int>(p->records.size()))
                break;
            std::string vm_id = p->records[p->selected_index].spec.vm_id;
            std::string error;
            bool ok = shell->manager_.RebootVm(vm_id, &error);
            shell->RefreshVmList();
            auto status = ok ? i18n::fmt(i18n::S::kStatusRebooted, vm_id.c_str())
                             : (std::string(i18n::tr(i18n::S::kStatusErrorPrefix)) + error);
            SendMessageA(p->statusbar, SB_SETTEXTA, 0, reinterpret_cast<LPARAM>(status.c_str()));
            return 0;
        }
        case IDM_SHUTDOWN: {
            if (p->selected_index < 0 ||
                p->selected_index >= static_cast<int>(p->records.size()))
                break;
            std::string vm_id = p->records[p->selected_index].spec.vm_id;
            std::string error;
            bool ok = shell->manager_.ShutdownVm(vm_id, &error);
            shell->RefreshVmList();
            auto status = ok ? i18n::fmt(i18n::S::kStatusShuttingDown, vm_id.c_str())
                             : (std::string(i18n::tr(i18n::S::kStatusErrorPrefix)) + error);
            SendMessageA(p->statusbar, SB_SETTEXTA, 0, reinterpret_cast<LPARAM>(status.c_str()));
            return 0;
        }
        case IDM_EDIT: {
            if (p->selected_index < 0 ||
                p->selected_index >= static_cast<int>(p->records.size()))
                break;
            VmRecord rec = p->records[p->selected_index];
            std::string vm_name = rec.spec.name;
            std::string error;
            if (ShowEditVmDialog(hwnd, shell->manager_, rec, &error)) {
                shell->RefreshVmList();
                auto status = i18n::fmt(i18n::S::kStatusVmUpdated, vm_name.c_str());
                SendMessageA(p->statusbar, SB_SETTEXTA, 0, reinterpret_cast<LPARAM>(status.c_str()));
            } else if (!error.empty()) {
                MessageBoxA(hwnd, error.c_str(), i18n::tr(i18n::S::kError), MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        case IDM_DELETE: {
            if (p->selected_index < 0 ||
                p->selected_index >= static_cast<int>(p->records.size()))
                break;
            std::string vm_id   = p->records[p->selected_index].spec.vm_id;
            std::string vm_name = p->records[p->selected_index].spec.name;
            auto prompt = i18n::fmt(i18n::S::kConfirmDeleteMsg, vm_name.c_str());
            if (MessageBoxA(hwnd, prompt.c_str(), i18n::tr(i18n::S::kConfirmDeleteTitle),
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
                std::string error;
                if (shell->manager_.DeleteVm(vm_id, &error)) {
                    p->vm_ui_states.erase(vm_id);
                    shell->RefreshVmList();
                    if (p->records.empty()) {
                        LayoutControls(p);
                    } else {
                        // Simulate click on current selection to restore VM UI state
                        int new_sel = p->selected_index;
                        p->selected_index = -1;
                        SendMessageA(p->listview, LB_SETCURSEL, new_sel, 0);
                        SendMessageA(hwnd, WM_COMMAND,
                            MAKEWPARAM(IDC_LISTVIEW, LBN_SELCHANGE),
                            reinterpret_cast<LPARAM>(p->listview));
                    }
                    SendMessageA(p->statusbar, SB_SETTEXTA, 0,
                        reinterpret_cast<LPARAM>(i18n::tr(i18n::S::kStatusVmDeleted)));
                } else {
                    MessageBoxA(hwnd, error.c_str(), i18n::tr(i18n::S::kError), MB_OK | MB_ICONERROR);
                }
            }
            return 0;
        }
        case IDC_SF_ADD_BTN: {
            if (code != BN_CLICKED) break;
            if (p->selected_index < 0 ||
                p->selected_index >= static_cast<int>(p->records.size()))
                break;
            
            // Simple dialog to add shared folder
            char tag_buf[64] = {};
            char path_buf[MAX_PATH] = {};
            
            // Use folder browser dialog
            BROWSEINFOA bi = {};
            bi.hwndOwner = hwnd;
            bi.lpszTitle = "Select folder to share";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            
            LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
            if (pidl) {
                SHGetPathFromIDListA(pidl, path_buf);
                CoTaskMemFree(pidl);
                
                // Generate a default tag from folder name
                std::string path_str(path_buf);
                size_t last_sep = path_str.find_last_of("\\/");
                std::string default_tag = (last_sep != std::string::npos) 
                    ? path_str.substr(last_sep + 1) : "share";
                
                // Ask for tag name
                std::string prompt = "Enter a tag name for this shared folder:";
                strncpy_s(tag_buf, default_tag.c_str(), sizeof(tag_buf) - 1);
                
                // Simple input dialog using a static variable and MessageBox
                // For a better UX, this should be a custom dialog
                if (default_tag.empty()) {
                    strcpy_s(tag_buf, "share");
                }
                
                // Add the shared folder
                SharedFolder sf;
                sf.tag = default_tag;
                sf.host_path = path_buf;
                sf.readonly = false;
                
                std::string error;
                std::string vm_id = p->records[p->selected_index].spec.vm_id;
                if (shell->manager_.AddSharedFolder(vm_id, sf, &error)) {
                    RefreshSharedFsList(p, shell->manager_);
                } else {
                    MessageBoxA(hwnd, error.c_str(), i18n::tr(i18n::S::kError), MB_OK | MB_ICONERROR);
                }
            }
            return 0;
        }
        case IDC_SF_DEL_BTN: {
            if (code != BN_CLICKED) break;
            if (p->selected_index < 0 ||
                p->selected_index >= static_cast<int>(p->records.size()))
                break;
            
            int sel = ListView_GetNextItem(p->sf_list, -1, LVNI_SELECTED);
            if (sel < 0) {
                MessageBoxA(hwnd, "Please select a shared folder to remove.",
                           i18n::tr(i18n::S::kError), MB_OK | MB_ICONWARNING);
                return 0;
            }
            
            char tag_buf[64] = {};
            ListView_GetItemText(p->sf_list, sel, 0, tag_buf, sizeof(tag_buf));
            
            std::string prompt = "Remove shared folder '" + std::string(tag_buf) + "'?";
            if (MessageBoxA(hwnd, prompt.c_str(), "Confirm Remove",
                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
                std::string error;
                std::string vm_id = p->records[p->selected_index].spec.vm_id;
                if (shell->manager_.RemoveSharedFolder(vm_id, tag_buf, &error)) {
                    RefreshSharedFsList(p, shell->manager_);
                } else {
                    MessageBoxA(hwnd, error.c_str(), i18n::tr(i18n::S::kError), MB_OK | MB_ICONERROR);
                }
            }
            return 0;
        }
        } // switch cmd
        break;
    }

    case WM_NOTIFY: {
        auto* nmhdr = reinterpret_cast<NMHDR*>(lp);
        if (nmhdr->idFrom == IDC_TAB && nmhdr->code == TCN_SELCHANGE) {
            int cur_tab = static_cast<int>(SendMessage(p->tab, TCM_GETCURSEL, 0, 0));
            if (p->selected_index >= 0 &&
                p->selected_index < static_cast<int>(p->records.size())) {
                p->GetVmUiState(p->records[p->selected_index].spec.vm_id).current_tab = cur_tab;
            }
            if (cur_tab == kTabSharedFs) {
                RefreshSharedFsList(p, shell->manager_);
            }
            LayoutControls(p);
        }
        break;
    }

    case WM_MEASUREITEM: {
        auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (mis->CtlID == IDC_LISTVIEW) {
            mis->itemHeight = kListItemHeight;
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);

        if (dis->CtlID == IDC_LISTVIEW && dis->itemID != static_cast<UINT>(-1)) {
            int idx = static_cast<int>(dis->itemID);
            if (idx < 0 || idx >= static_cast<int>(p->records.size()))
                break;
            const auto& rec = p->records[idx];

            bool selected = (dis->itemState & ODS_SELECTED) != 0;
            COLORREF card_bg = selected ? RGB(229, 241, 255)
                                        : RGB(248, 248, 248);
            COLORREF fg  = selected ? RGB(20, 20, 20)
                                    : GetSysColor(COLOR_WINDOWTEXT);
            COLORREF dim = selected ? RGB(80, 80, 80)
                                    : GetSysColor(COLOR_GRAYTEXT);

            HBRUSH wnd_br = GetSysColorBrush(COLOR_WINDOW);
            FillRect(dis->hDC, &dis->rcItem, wnd_br);

            RECT card = dis->rcItem;
            card.left   += 4;
            card.right  -= 4;
            card.top    += 3;
            card.bottom -= 3;

            HBRUSH card_br = CreateSolidBrush(card_bg);
            HPEN border_pen = CreatePen(PS_SOLID, 1,
                selected ? RGB(100, 160, 230) : RGB(232, 232, 232));
            HPEN old_pen = static_cast<HPEN>(SelectObject(dis->hDC, border_pen));
            HBRUSH old_br = static_cast<HBRUSH>(SelectObject(dis->hDC, card_br));
            RoundRect(dis->hDC, card.left, card.top, card.right, card.bottom, 6, 6);
            SelectObject(dis->hDC, old_br);
            SelectObject(dis->hDC, old_pen);
            DeleteObject(card_br);
            DeleteObject(border_pen);

            SetBkMode(dis->hDC, TRANSPARENT);

            HFONT bold = p->ui_font;
            HFONT normal = p->ui_font;
            HFONT old_font = static_cast<HFONT>(SelectObject(dis->hDC, bold));

            TEXTMETRICA tm{};
            GetTextMetricsA(dis->hDC, &tm);
            int line_h = tm.tmHeight + tm.tmExternalLeading;

            int x = card.left + 12;
            int y = card.top + 8;

            // Line 1: VM name (bold/normal)
            SetTextColor(dis->hDC, fg);
            auto name_w = i18n::to_wide(rec.spec.name);
            TextOutW(dis->hDC, x, y, name_w.c_str(), static_cast<int>(name_w.size()));

            // State tag to the right of the name
            SIZE name_sz{};
            GetTextExtentPoint32W(dis->hDC, name_w.c_str(),
                static_cast<int>(name_w.size()), &name_sz);

            auto state_w = i18n::to_wide(StateText(rec.state));
            COLORREF state_color;
            if (rec.state == VmPowerState::kRunning)
                state_color = RGB(0, 128, 0);
            else if (rec.state == VmPowerState::kCrashed)
                state_color = RGB(200, 0, 0);
            else
                state_color = dim;
            SetTextColor(dis->hDC, state_color);
            TextOutW(dis->hDC, x + name_sz.cx + 12, y, state_w.c_str(),
                     static_cast<int>(state_w.size()));

            // Line 2: vCPU / Memory
            y += line_h + 2;
            SelectObject(dis->hDC, normal);
            SetTextColor(dis->hDC, dim);
            auto detail = i18n::fmt(i18n::S::kDetailVcpuRam,
                static_cast<unsigned>(rec.spec.cpu_count), static_cast<unsigned>(rec.spec.memory_mb));
            auto detail_w = i18n::to_wide(detail);
            TextOutW(dis->hDC, x, y, detail_w.c_str(), static_cast<int>(detail_w.size()));

            SelectObject(dis->hDC, old_font);

            return TRUE;
        }
        break;
    }

    case WM_INVOKE: {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lock(g_invoke_mutex);
            if (!g_invoke_queue.empty()) {
                fn = std::move(g_invoke_queue.front());
                g_invoke_queue.pop_front();
            }
        }
        if (fn) fn();
        return 0;
    }

    case WM_CLIPBOARDUPDATE:
        // Host clipboard changed - notify running VMs
        if (!g_clipboard_from_vm) {
            auto vms = shell->manager_.ListVms();
            for (const auto& vm : vms) {
                if (vm.state == VmPowerState::kRunning) {
                    std::vector<uint32_t> types = {1};  // VD_AGENT_CLIPBOARD_UTF8_TEXT
                    shell->manager_.SendClipboardGrab(vm.spec.vm_id, types);
                }
            }
        } else {
            g_clipboard_from_vm = false;
        }
        return 0;

    case WM_CLOSE:
        // Check if any VM is still running
        {
            auto vms = shell->manager_.ListVms();
            int running_count = 0;
            for (const auto& vm : vms) {
                if (vm.state == VmPowerState::kStarting ||
                    vm.state == VmPowerState::kRunning ||
                    vm.state == VmPowerState::kStopping) {
                    ++running_count;
                }
            }
            if (running_count > 0) {
                auto exit_msg = i18n::fmt(i18n::S::kConfirmExitMsg, static_cast<unsigned>(running_count));
                int result = MessageBoxA(hwnd, exit_msg.c_str(), i18n::tr(i18n::S::kConfirmExitTitle),
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
                if (result != IDYES) {
                    return 0;
                }
            }
        }
        // Save geometry
        {
            RECT wr;
            GetWindowRect(hwnd, &wr);
            auto& geo = shell->manager_.app_settings().window;
            geo.x      = wr.left;
            geo.y      = wr.top;
            geo.width  = wr.right - wr.left;
            geo.height = wr.bottom - wr.top;
            shell->manager_.SaveAppSettings();
        }
        PostQuitMessage(0);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

// Subclass proc to catch Enter in console input
static LRESULT CALLBACK ConsoleInputSubclass(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR /*id*/, DWORD_PTR /*ref_data*/)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessage(GetParent(hwnd), WM_COMMAND,
            MAKEWPARAM(IDC_SEND_BTN, BN_CLICKED), 0);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ── Lifetime ──

Win32UiShell::Win32UiShell(ManagerService& manager)
    : manager_(manager),
      impl_(std::make_unique<Impl>())
{
    g_shell = this;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX icc{sizeof(icc),
        ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    HINSTANCE hinst = GetModuleHandle(nullptr);
    RegisterMainClass(hinst);

    // Fonts
    NONCLIENTMETRICSA ncm{sizeof(ncm)};
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    impl_->ui_font = CreateFontIndirectA(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -13;
    lstrcpyA(ncm.lfMessageFont.lfFaceName, "Consolas");
    impl_->mono_font = CreateFontIndirectA(&ncm.lfMessageFont);

    // Restore geometry
    const auto& geo = manager_.app_settings().window;
    int x = (geo.x >= 0) ? geo.x : CW_USEDEFAULT;
    int y = (geo.y >= 0) ? geo.y : CW_USEDEFAULT;
    int w = (geo.width > 0) ? geo.width : 1024;
    int h = (geo.height > 0) ? geo.height : 680;

    i18n::InitLanguage();
    impl_->menu_bar = BuildMenuBar();

    impl_->hwnd = CreateWindowExA(0, kWndClass, i18n::tr(i18n::S::kAppTitle),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x, y, w, h,
        nullptr, impl_->menu_bar, hinst, nullptr);

    g_main_hwnd = impl_->hwnd;
    SetWindowLongPtrA(impl_->hwnd, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(impl_.get()));

    // Register for clipboard change notifications
    AddClipboardFormatListener(impl_->hwnd);

    impl_->toolbar   = CreateToolbar(impl_->hwnd, hinst);
    impl_->statusbar = CreateWindowExA(0, STATUSCLASSNAMEA, nullptr,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, impl_->hwnd,
        reinterpret_cast<HMENU>(IDC_STATUSBAR), hinst, nullptr);

    impl_->listview  = CreateVmListBox(impl_->hwnd, hinst);

    // Tab control (Console / Display)
    impl_->tab = CreateWindowExA(0, WC_TABCONTROLA, nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 0, 0, impl_->hwnd,
        reinterpret_cast<HMENU>(IDC_TAB), hinst, nullptr);
    SendMessage(impl_->tab, WM_SETFONT,
        reinterpret_cast<WPARAM>(impl_->ui_font), FALSE);
    SendMessage(impl_->tab, TCM_SETPADDING, 0, MAKELPARAM(24, 8));
    {
        TCITEMA ti{};
        ti.mask = TCIF_TEXT;
        ti.pszText = const_cast<char*>(i18n::tr(i18n::S::kTabInfo));
        SendMessageA(impl_->tab, TCM_INSERTITEMA, kTabInfo,
            reinterpret_cast<LPARAM>(&ti));
        ti.pszText = const_cast<char*>(i18n::tr(i18n::S::kTabConsole));
        SendMessageA(impl_->tab, TCM_INSERTITEMA, kTabConsole,
            reinterpret_cast<LPARAM>(&ti));
        ti.pszText = const_cast<char*>(i18n::tr(i18n::S::kTabDisplay));
        SendMessageA(impl_->tab, TCM_INSERTITEMA, kTabDisplay,
            reinterpret_cast<LPARAM>(&ti));
        ti.pszText = const_cast<char*>("Shared Folders");
        SendMessageA(impl_->tab, TCM_INSERTITEMA, kTabSharedFs,
            reinterpret_cast<LPARAM>(&ti));
    }

    // Detail label/value statics for Info tab
    for (int i = 0; i < Impl::kDetailRows; ++i) {
        impl_->detail_labels[i] = CreateWindowExA(0, "STATIC", "",
            WS_CHILD | SS_RIGHT,
            0, 0, 0, 0, impl_->hwnd, nullptr, hinst, nullptr);
        impl_->detail_values[i] = CreateWindowExA(0, "EDIT", "",
            WS_CHILD | ES_READONLY | ES_AUTOHSCROLL,
            0, 0, 0, 0, impl_->hwnd, nullptr, hinst, nullptr);
        SendMessage(impl_->detail_labels[i], WM_SETFONT,
            reinterpret_cast<WPARAM>(impl_->ui_font), FALSE);
        SendMessage(impl_->detail_values[i], WM_SETFONT,
            reinterpret_cast<WPARAM>(impl_->ui_font), FALSE);
    }

    // Console output
    impl_->console = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 0, 0, impl_->hwnd,
        reinterpret_cast<HMENU>(IDC_CONSOLE), hinst, nullptr);
    SendMessage(impl_->console, EM_SETLIMITTEXT,
        static_cast<WPARAM>(kMaxConsoleLen * 2), 0);
    SendMessage(impl_->console, WM_SETFONT,
        reinterpret_cast<WPARAM>(impl_->mono_font), FALSE);

    // Console input
    impl_->console_in = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 0, 0, impl_->hwnd,
        reinterpret_cast<HMENU>(IDC_CONSOLE_IN), hinst, nullptr);
    SendMessage(impl_->console_in, WM_SETFONT,
        reinterpret_cast<WPARAM>(impl_->mono_font), FALSE);
    auto placeholder_w = i18n::to_wide(i18n::tr(i18n::S::kConsolePlaceholder));
    SendMessageW(impl_->console_in, EM_SETCUEBANNER, FALSE,
        reinterpret_cast<LPARAM>(placeholder_w.c_str()));

    // Send button
    impl_->send_btn = CreateWindowExA(0, "BUTTON", i18n::tr(i18n::S::kSend),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, impl_->hwnd,
        reinterpret_cast<HMENU>(IDC_SEND_BTN), hinst, nullptr);
    SendMessage(impl_->send_btn, WM_SETFONT,
        reinterpret_cast<WPARAM>(impl_->ui_font), FALSE);

    // Subclass the console input to catch Enter key
    SetWindowSubclass(impl_->console_in, ConsoleInputSubclass, 0, 0);

    // Shared folders ListView
    impl_->sf_list = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
        WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, impl_->hwnd,
        reinterpret_cast<HMENU>(IDC_SF_LISTVIEW), hinst, nullptr);
    ListView_SetExtendedListViewStyle(impl_->sf_list, 
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    SendMessage(impl_->sf_list, WM_SETFONT,
        reinterpret_cast<WPARAM>(impl_->ui_font), FALSE);
    {
        LVCOLUMNA col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 200;
        col.pszText = const_cast<char*>("Tag");
        ListView_InsertColumn(impl_->sf_list, 0, &col);
        col.cx = 500;
        col.pszText = const_cast<char*>("Host Path");
        ListView_InsertColumn(impl_->sf_list, 1, &col);
        col.cx = 160;
        col.pszText = const_cast<char*>("Mode");
        ListView_InsertColumn(impl_->sf_list, 2, &col);
    }

    // Shared folders buttons
    impl_->sf_add_btn = CreateWindowExA(0, "BUTTON", "Add...",
        WS_CHILD | BS_PUSHBUTTON,
        0, 0, 0, 0, impl_->hwnd,
        reinterpret_cast<HMENU>(IDC_SF_ADD_BTN), hinst, nullptr);
    impl_->sf_del_btn = CreateWindowExA(0, "BUTTON", "Remove",
        WS_CHILD | BS_PUSHBUTTON,
        0, 0, 0, 0, impl_->hwnd,
        reinterpret_cast<HMENU>(IDC_SF_DEL_BTN), hinst, nullptr);
    SendMessage(impl_->sf_add_btn, WM_SETFONT,
        reinterpret_cast<WPARAM>(impl_->ui_font), FALSE);
    SendMessage(impl_->sf_del_btn, WM_SETFONT,
        reinterpret_cast<WPARAM>(impl_->ui_font), FALSE);

    // Apply font to toolbar
    SendMessage(impl_->toolbar, WM_SETFONT,
        reinterpret_cast<WPARAM>(impl_->ui_font), FALSE);
    SendMessage(impl_->listview, WM_SETFONT,
        reinterpret_cast<WPARAM>(impl_->ui_font), FALSE);

    // Display panel
    impl_->display_panel = std::make_unique<DisplayPanel>();
    impl_->display_panel->Create(impl_->hwnd, hinst, 0, 0, 400, 300);
    impl_->display_panel->SetKeyCallback(
        [this](uint32_t evdev_code, bool pressed) {
            if (impl_->selected_index < 0 ||
                impl_->selected_index >= static_cast<int>(impl_->records.size()))
                return;
            const auto& vm_id = impl_->records[impl_->selected_index].spec.vm_id;
            manager_.SendKeyEvent(vm_id, evdev_code, pressed);
        });
    impl_->display_panel->SetPointerCallback(
        [this](int32_t x, int32_t y, uint32_t buttons) {
            if (impl_->selected_index < 0 ||
                impl_->selected_index >= static_cast<int>(impl_->records.size()))
                return;
            const auto& vm_id = impl_->records[impl_->selected_index].spec.vm_id;
            manager_.SendPointerEvent(vm_id, x, y, buttons);
        });
    impl_->display_panel->SetWheelCallback(
        [this](int32_t delta) {
            if (impl_->selected_index < 0 ||
                impl_->selected_index >= static_cast<int>(impl_->records.size()))
                return;
            const auto& vm_id = impl_->records[impl_->selected_index].spec.vm_id;
            manager_.SendWheelEvent(vm_id, delta);
        });

    // Wire callbacks: always cache to VM state; update UI only for current VM
    manager_.SetDisplayCallback(
        [this](const std::string& vm_id, const DisplayFrame& frame) {
            InvokeOnUiThread([this, vm_id, frame]() {
                VmUiState& state = impl_->GetVmUiState(vm_id);
                BlitFrameToState(state, frame);

                bool is_current = (impl_->selected_index >= 0 &&
                    impl_->selected_index < static_cast<int>(impl_->records.size()) &&
                    impl_->records[impl_->selected_index].spec.vm_id == vm_id);
                if (is_current) {
                    impl_->display_panel->UpdateFrame(frame);
                }
            });
        });
    manager_.SetCursorCallback(
        [this](const std::string& vm_id, const CursorInfo& cursor) {
            InvokeOnUiThread([this, vm_id, cursor]() {
                VmUiState& state = impl_->GetVmUiState(vm_id);
                state.cursor = cursor;
                if (cursor.image_updated && !cursor.pixels.empty()) {
                    state.cursor_pixels = cursor.pixels;
                }

                bool is_current = (impl_->selected_index >= 0 &&
                    impl_->selected_index < static_cast<int>(impl_->records.size()) &&
                    impl_->records[impl_->selected_index].spec.vm_id == vm_id);
                if (is_current) {
                    impl_->display_panel->UpdateCursor(cursor);
                }
            });
        });

    manager_.SetDisplayStateCallback(
        [this](const std::string& vm_id, bool active, uint32_t width, uint32_t height) {
            InvokeOnUiThread([this, vm_id, active, width, height]() {
                bool is_current = (impl_->selected_index >= 0 &&
                    impl_->selected_index < static_cast<int>(impl_->records.size()) &&
                    impl_->records[impl_->selected_index].spec.vm_id == vm_id);
                if (!is_current) return;

                VmUiState& state = impl_->GetVmUiState(vm_id);
                if (active) {
                    impl_->display_available = true;
                    state.current_tab = kTabDisplay;
                    SendMessage(impl_->tab, TCM_SETCURSEL, kTabDisplay, 0);
                    // Resize window to fit VM display at 1:1 pixel ratio
                    ResizeWindowForDisplay(impl_.get(), width, height);
                } else {
                    impl_->display_available = false;
                    state.current_tab = kTabConsole;
                    SendMessage(impl_->tab, TCM_SETCURSEL, kTabConsole, 0);
                }
                LayoutControls(impl_.get());
            });
        });

    manager_.SetConsoleCallback([this](const std::string& vm_id,
                                       const std::string& data) {
        InvokeOnUiThread([this, vm_id, data]() {
            VmUiState& state = impl_->GetVmUiState(vm_id);
            std::string added = impl_->AppendConsoleDataToState(state, data);

            bool is_current = (impl_->selected_index >= 0 &&
                impl_->selected_index < static_cast<int>(impl_->records.size()) &&
                impl_->records[impl_->selected_index].spec.vm_id == vm_id);
            if (is_current && !added.empty()) {
                int ctl_len = GetWindowTextLengthA(impl_->console);
                if (ctl_len > static_cast<int>(kConsoleTrimAt)) {
                    int cut = ctl_len / 2;
                    SendMessageA(impl_->console, EM_SETSEL, 0, cut);
                    SendMessageA(impl_->console, EM_REPLACESEL, FALSE,
                        reinterpret_cast<LPARAM>(""));
                    ctl_len = GetWindowTextLengthA(impl_->console);
                }
                SendMessageA(impl_->console, EM_SETSEL, ctl_len, ctl_len);
                SendMessageA(impl_->console, EM_REPLACESEL, FALSE,
                    reinterpret_cast<LPARAM>(added.c_str()));
            }
        });
    });

    manager_.SetStateChangeCallback([this](const std::string& vm_id) {
        InvokeOnUiThread([this, vm_id]() {
            RefreshVmList();

            auto vm_opt = manager_.GetVm(vm_id);
            bool is_stopped = !vm_opt || vm_opt->state == VmPowerState::kStopped ||
                              vm_opt->state == VmPowerState::kCrashed;

            if (is_stopped) {
                VmUiState& ui_state = impl_->GetVmUiState(vm_id);
                ui_state.current_tab = kTabInfo;
                ui_state.fb_width = 0;
                ui_state.fb_height = 0;
                ui_state.framebuffer.clear();
                ui_state.cursor_pixels.clear();
            }

            bool is_current = (impl_->selected_index >= 0 &&
                impl_->selected_index < static_cast<int>(impl_->records.size()) &&
                impl_->records[impl_->selected_index].spec.vm_id == vm_id);
            if (is_current && is_stopped) {
                impl_->display_available = false;
                SendMessage(impl_->tab, TCM_SETCURSEL, kTabInfo, 0);
                LayoutControls(impl_.get());
            }
        });
    });

    RefreshVmList();
    LayoutControls(impl_.get());
}

Win32UiShell::~Win32UiShell() {
    if (impl_->hwnd) {
        RemoveClipboardFormatListener(impl_->hwnd);
    }
    if (impl_->ui_font) DeleteObject(impl_->ui_font);
    if (impl_->mono_font) DeleteObject(impl_->mono_font);
    g_shell = nullptr;
    g_main_hwnd = nullptr;
}

void Win32UiShell::Show() {
    ShowWindow(impl_->hwnd, SW_SHOW);
    UpdateWindow(impl_->hwnd);
}

void Win32UiShell::Hide() {
    ShowWindow(impl_->hwnd, SW_HIDE);
}

void Win32UiShell::Run() {
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        // Intercept keyboard messages and forward to DisplayPanel when Display tab is visible.
        // This handles the case where focus is on listbox or other controls.
        bool forwarded = false;
        if (msg.message == WM_KEYDOWN || msg.message == WM_KEYUP ||
            msg.message == WM_SYSKEYDOWN || msg.message == WM_SYSKEYUP) {
            int cur_tab = static_cast<int>(SendMessage(impl_->tab, TCM_GETCURSEL, 0, 0));
            if (cur_tab == kTabDisplay && impl_->display_available && impl_->display_panel) {
                HWND panel_hwnd = impl_->display_panel->Handle();
                // Don't intercept if focus is already on panel or on console input
                if (msg.hwnd != panel_hwnd && msg.hwnd != impl_->console_in) {
                    SendMessage(panel_hwnd, msg.message, msg.wParam, msg.lParam);
                    forwarded = true;
                }
            }
        }
        if (!forwarded) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
}

void Win32UiShell::Quit() {
    // Save geometry
    if (impl_->hwnd) {
        RECT wr;
        GetWindowRect(impl_->hwnd, &wr);
        auto& geo = manager_.app_settings().window;
        geo.x      = wr.left;
        geo.y      = wr.top;
        geo.width  = wr.right - wr.left;
        geo.height = wr.bottom - wr.top;
        manager_.SaveAppSettings();
    }
    PostQuitMessage(0);
}

void Win32UiShell::RefreshVmList() {
    impl_->records = manager_.ListVms();

    if (impl_->selected_index >= static_cast<int>(impl_->records.size())) {
        impl_->selected_index = static_cast<int>(impl_->records.size()) - 1;
    }

    PopulateListBox(impl_.get());
    UpdateDetailPanel(impl_.get());
    UpdateCommandStates(impl_.get());

    auto status = i18n::fmt(i18n::S::kStatusVmsLoaded, static_cast<unsigned>(impl_->records.size()));
    SendMessageA(impl_->statusbar, SB_SETTEXTA, 0, reinterpret_cast<LPARAM>(status.c_str()));
}

void Win32UiShell::InvokeOnUiThread(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(g_invoke_mutex);
        g_invoke_queue.push_back(std::move(fn));
    }
    if (g_main_hwnd) {
        PostMessage(g_main_hwnd, WM_INVOKE, 0, 0);
    }
}

void Win32UiShell::SetClipboardFromVm(bool value) {
    g_clipboard_from_vm = value;
}
