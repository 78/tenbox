#include "ui/win32/win32_ui_shell.h"
#include "ui/win32/win32_dialogs.h"
#include "ui/win32/win32_display_panel.h"
#include "ui/win32/components/info_tab.h"
#include "ui/win32/components/console_tab.h"
#include "ui/win32/components/vm_listview.h"
#include "ui/common/i18n.h"
#include "manager/app_settings.h"
#include "manager/resource.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>

#pragma comment(lib, "comctl32.lib")

#include "common/ports.h"
#include "platform/windows/audio/wasapi_audio_player.h"

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
    IDM_NEW_VM        = 1001,
    IDM_EXIT          = 1002,
    IDM_START         = 1010,
    IDM_STOP          = 1011,
    IDM_REBOOT        = 1012,
    IDM_SHUTDOWN      = 1013,
    IDM_EDIT          = 1014,
    IDM_DELETE        = 1015,
    IDM_SHARED_FOLDERS = 1016,
    IDM_VIEW_TOOLBAR   = 1017,
    IDM_WEBSITE        = 1020,
    IDM_CHECK_UPDATE  = 1021,
    IDM_ABOUT         = 1022,
};

// ── Control IDs ──

enum CtlId : UINT {
    IDC_TOOLBAR     = 2001,
    IDC_STATUSBAR   = 2002,
    IDC_TAB         = 2007,
};

// WM_APP range for cross-thread invoke
static constexpr UINT WM_INVOKE = WM_APP + 100;

// Track if we initiated the clipboard change (to avoid echo)
static bool g_clipboard_from_vm = false;

static constexpr int kDefaultLeftPaneWidth = 280;

// ── Forward declarations for dialog helpers (win32_dialogs.cpp) ──

extern bool ShowCreateVmDialog(HWND parent, ManagerService& mgr, std::string* error);
extern bool ShowEditVmDialog(HWND parent, ManagerService& mgr,
                             const VmRecord& rec, std::string* error);
extern void ShowSharedFoldersDialog(HWND parent, ManagerService& mgr,
                                    const std::string& vm_id);

// ── Static singleton HWND (needed for InvokeOnUiThread) ──

static HWND g_main_hwnd = nullptr;
static std::mutex g_invoke_mutex;
static std::deque<std::function<void()>> g_invoke_queue;

// ── Helpers ──

static bool IsVmRunning(VmPowerState s) {
    return s == VmPowerState::kRunning || s == VmPowerState::kStarting
        || s == VmPowerState::kStopping;
}

// ── Per-VM UI state cache ──

struct VmUiState {
    int current_tab = 0;

    ConsoleTab::TextState console_state;

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
    HWND tab        = nullptr;
    HMENU menu_bar  = nullptr;

    bool display_available = false;

    uint32_t last_sent_display_w = 0;
    uint32_t last_sent_display_h = 0;

    uint32_t pending_display_w = 0;
    uint32_t pending_display_h = 0;
    UINT_PTR resize_timer_id = 0;
    static constexpr UINT kResizeTimerId = 9001;
    static constexpr UINT kResizeDebounceMs = 500;

    HFONT ui_font     = nullptr;
    HFONT mono_font   = nullptr;

    // Components
    VmListView vm_listview;
    InfoTab info_tab;
    ConsoleTab console_tab;
    std::unique_ptr<DisplayPanel> display_panel;

    std::vector<VmRecord> records;
    int selected_index = -1;

    // Splitter between left pane (VM list) and right pane (tabs)
    int left_pane_width = kDefaultLeftPaneWidth;
    bool splitter_dragging = false;
    int splitter_drag_start_x = 0;
    int splitter_drag_start_width = 0;
    static constexpr int kSplitterWidth    = 4;
    static constexpr int kMinLeftPaneWidth = 180;
    static constexpr int kMaxLeftPaneWidth = 500;

    std::unordered_map<std::string, VmUiState> vm_ui_states;
    std::unordered_map<std::string, std::unique_ptr<WasapiAudioPlayer>> audio_players;

    VmUiState& GetVmUiState(const std::string& vm_id) {
        return vm_ui_states[vm_id];
    }

    WasapiAudioPlayer& GetAudioPlayer(const std::string& vm_id) {
        auto& ptr = audio_players[vm_id];
        if (!ptr) ptr = std::make_unique<WasapiAudioPlayer>();
        return *ptr;
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

static HMENU BuildMenuBar(bool show_toolbar) {
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
    AppendMenuA(vm_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(vm_menu, MF_STRING, IDM_SHARED_FOLDERS, i18n::tr(S::kToolbarSharedFolders));
    AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(vm_menu), i18n::tr(S::kMenuVm));

    HMENU view_menu = CreatePopupMenu();
    AppendMenuA(view_menu, MF_STRING | (show_toolbar ? MF_CHECKED : MF_UNCHECKED),
               IDM_VIEW_TOOLBAR, i18n::tr(S::kMenuViewToolbar));
    AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view_menu), i18n::tr(S::kMenuView));

    HMENU help_menu = CreatePopupMenu();
    AppendMenuA(help_menu, MF_STRING, IDM_WEBSITE,      i18n::tr(S::kMenuWebsite));
    AppendMenuA(help_menu, MF_STRING, IDM_CHECK_UPDATE,  i18n::tr(S::kMenuCheckUpdate));
    AppendMenuA(help_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(help_menu, MF_STRING, IDM_ABOUT,        i18n::tr(S::kMenuAbout));
    AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help_menu), i18n::tr(S::kMenuHelp));

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
        IDB_TOOLBAR_SHARED_FOLDERS,
    };

    HIMAGELIST hil = ImageList_Create(
        kToolbarIconSize, kToolbarIconSize,
        ILC_COLOR32 | ILC_MASK, 8, 0);
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
        {IDM_NEW_VM,        i18n::tr(S::kToolbarNewVm),       0},
        {IDM_EDIT,          i18n::tr(S::kToolbarEdit),        1},
        {IDM_DELETE,        i18n::tr(S::kToolbarDelete),      2},
        {0,                 nullptr,                          -1},
        {IDM_START,         i18n::tr(S::kToolbarStart),       3},
        {IDM_STOP,          i18n::tr(S::kToolbarStop),        4},
        {IDM_REBOOT,        i18n::tr(S::kToolbarReboot),      5},
        {IDM_SHUTDOWN,      i18n::tr(S::kToolbarShutdown),    6},
        {0,                 nullptr,                          -1},
        {IDM_SHARED_FOLDERS,i18n::tr(S::kToolbarSharedFolders),  7},
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

// ── Layout helper ──

using Impl = Win32UiShell::Impl;

static constexpr int kTabInfo    = 0;
static constexpr int kTabConsole = 1;
static constexpr int kTabDisplay = 2;

// Resize window to fit VM display at 1:1 pixel ratio
static void ResizeWindowForDisplay(Impl* p, uint32_t vm_width, uint32_t vm_height) {
    if (!p->hwnd || vm_width == 0 || vm_height == 0) return;

    vm_width = vm_width & ~7u;

    if (p->resize_timer_id != 0) return;

    p->last_sent_display_w = vm_width;
    p->last_sent_display_h = vm_height;

    int tb_h = 0;
    if (IsWindowVisible(p->toolbar)) {
        RECT tbr;
        GetWindowRect(p->toolbar, &tbr);
        tb_h = tbr.bottom - tbr.top;
    }
    RECT sbr;
    GetWindowRect(p->statusbar, &sbr);
    int sb_h = sbr.bottom - sbr.top;

    RECT tab_padding = {0, 0, 100, 100};
    SendMessage(p->tab, TCM_ADJUSTRECT, TRUE, reinterpret_cast<LPARAM>(&tab_padding));
    int tab_extra_w = tab_padding.right - tab_padding.left - 100;
    int tab_extra_h = tab_padding.bottom - tab_padding.top - 100;

    int target_cw = p->left_pane_width + Impl::kSplitterWidth + tab_extra_w + static_cast<int>(vm_width);
    int target_ch = tb_h + tab_extra_h + static_cast<int>(vm_height) + sb_h;

    RECT wr = {0, 0, target_cw, target_ch};
    DWORD style = static_cast<DWORD>(GetWindowLongPtr(p->hwnd, GWL_STYLE));
    DWORD ex_style = static_cast<DWORD>(GetWindowLongPtr(p->hwnd, GWL_EXSTYLE));
    AdjustWindowRectEx(&wr, style, TRUE, ex_style);

    int new_w = wr.right - wr.left;
    int new_h = wr.bottom - wr.top;

    HMONITOR hmon = MonitorFromWindow(p->hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfo(hmon, &mi);
    int max_w = mi.rcWork.right - mi.rcWork.left;
    int max_h = mi.rcWork.bottom - mi.rcWork.top;
    if (new_w > max_w) new_w = max_w;
    if (new_h > max_h) new_h = max_h;

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

    int tb_h = 0;
    if (IsWindowVisible(p->toolbar)) {
        SendMessage(p->toolbar, TB_AUTOSIZE, 0, 0);
        RECT tbr;
        GetWindowRect(p->toolbar, &tbr);
        tb_h = tbr.bottom - tbr.top;
    }

    SendMessage(p->statusbar, WM_SIZE, 0, 0);
    RECT sbr;
    GetWindowRect(p->statusbar, &sbr);
    int sb_h = sbr.bottom - sbr.top;

    int content_top = tb_h;
    int content_h   = ch - tb_h - sb_h;
    if (content_h < 0) content_h = 0;

    MoveWindow(p->vm_listview.handle(), 0, content_top, p->left_pane_width, content_h, TRUE);
    p->vm_listview.UpdateColumnWidth();

    int rx = p->left_pane_width + Impl::kSplitterWidth;
    int rw = cw - rx;
    if (rw < 0) rw = 0;

    // Hide all right-pane controls first
    p->info_tab.Show(false);
    p->console_tab.Show(false);
    if (p->display_panel) p->display_panel->SetVisible(false);

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
        p->info_tab.Show(true);
        p->info_tab.Layout(p->hwnd, p->ui_font, px, py, pw, ph);
    } else if (cur_tab == kTabConsole) {
        p->console_tab.Show(true);
        p->console_tab.Layout(px, py, pw, ph);
    } else if (cur_tab == kTabDisplay) {
        if (p->display_panel) {
            p->display_panel->SetVisible(true);
            p->display_panel->SetBounds(px, py, pw, ph);

            uint32_t disp_w = pw > 0 ? (static_cast<uint32_t>(pw) & ~7u) : 0;
            uint32_t disp_h = ph > 0 ? static_cast<uint32_t>(ph) : 0;

            if (p->display_available && disp_w > 0 && disp_h > 0 &&
                (disp_w != p->last_sent_display_w || disp_h != p->last_sent_display_h)) {
                p->pending_display_w = disp_w;
                p->pending_display_h = disp_h;
                if (p->resize_timer_id) {
                    KillTimer(p->hwnd, Impl::kResizeTimerId);
                }
                p->resize_timer_id = SetTimer(p->hwnd, Impl::kResizeTimerId,
                    Impl::kResizeDebounceMs, nullptr);
            }
        }
    }
}

// ── Update toolbar/menu enable state ──

static void UpdateCommandStates(Impl* p) {
    bool has_sel = p->selected_index >= 0 &&
                   p->selected_index < static_cast<int>(p->records.size());
    bool running = has_sel && IsVmRunning(p->records[p->selected_index].state);
    bool stopping = has_sel && p->records[p->selected_index].state == VmPowerState::kStopping;
    bool ga_ok = has_sel && p->records[p->selected_index].guest_agent_connected;

    auto EnableCmd = [&](UINT id, bool en) {
        SendMessage(p->toolbar, TB_ENABLEBUTTON, id, MAKELONG(en ? TRUE : FALSE, 0));
        HMENU vm_menu = GetSubMenu(p->menu_bar, 1);
        if (vm_menu) {
            EnableMenuItem(vm_menu, id, en ? MF_ENABLED : MF_GRAYED);
        }
    };

    EnableCmd(IDM_START,          has_sel && !running);
    EnableCmd(IDM_STOP,           has_sel && running);
    EnableCmd(IDM_REBOOT,         has_sel && running && !stopping && ga_ok);
    EnableCmd(IDM_SHUTDOWN,       has_sel && running && !stopping && ga_ok);
    EnableCmd(IDM_EDIT,           has_sel);
    EnableCmd(IDM_DELETE,         has_sel && !running);
    EnableCmd(IDM_SHARED_FOLDERS, has_sel);

    p->console_tab.SetEnabled(running);
}

// ── Splitter hit-test ──

static bool HitTestSplitter(Impl* p, int client_x) {
    return client_x >= p->left_pane_width &&
           client_x <  p->left_pane_width + Impl::kSplitterWidth;
}

// ── WndProc ──

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* shell = g_shell;
    if (!shell) return DefWindowProcA(hwnd, msg, wp, lp);
    auto* p = reinterpret_cast<Impl*>(
        GetWindowLongPtrA(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_SIZE:
        if (p) LayoutControls(p);
        return 0;

    case WM_SETCURSOR:
        if (p && reinterpret_cast<HWND>(wp) == hwnd &&
            LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (p->splitter_dragging || HitTestSplitter(p, pt.x)) {
                SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                return TRUE;
            }
        }
        break;

    case WM_TIMER:
        if (p && wp == Impl::kResizeTimerId) {
            KillTimer(hwnd, Impl::kResizeTimerId);
            p->resize_timer_id = 0;
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

        // Console send button
        if (cmd == ConsoleTab::kSendBtnId && code == BN_CLICKED) {
            char buf[1024]{};
            GetWindowTextA(p->console_tab.input_handle(), buf, sizeof(buf));
            std::string input(buf);
            if (!input.empty() && p->selected_index >= 0 &&
                p->selected_index < static_cast<int>(p->records.size())) {
                std::string vm_id = p->records[p->selected_index].spec.vm_id;
                std::string to_send = input + "\n";
                if (shell->manager_.SendConsoleInput(vm_id, to_send)) {
                    SetWindowTextA(p->console_tab.input_handle(), "");
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
        case IDM_WEBSITE:
            ShellExecuteA(hwnd, "open", "https://tenbox.ai/", nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        case IDM_CHECK_UPDATE:
            ShellExecuteA(hwnd, "open", "https://github.com/78/tenbox/releases", nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        case IDM_ABOUT:
            MessageBoxA(hwnd, i18n::tr(i18n::S::kAboutText),
                i18n::tr(i18n::S::kAboutTitle), MB_OK | MB_ICONINFORMATION);
            return 0;
        case IDM_START: {
            if (p->selected_index < 0 ||
                p->selected_index >= static_cast<int>(p->records.size()))
                break;
            std::string vm_id = p->records[p->selected_index].spec.vm_id;
            auto it = p->vm_ui_states.find(vm_id);
            if (it != p->vm_ui_states.end()) {
                ConsoleTab::ResetState(it->second.console_state);
            }
            VmUiState& state = p->GetVmUiState(vm_id);
            state.current_tab = kTabConsole;
            p->console_tab.SetText("");
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
            std::string vm_name = p->records[p->selected_index].spec.name;
            auto prompt = i18n::fmt(i18n::S::kConfirmForceStopMsg, vm_name.c_str());
            if (MessageBoxA(hwnd, prompt.c_str(), i18n::tr(i18n::S::kConfirmForceStopTitle),
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                return 0;
            }
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
        case IDM_SHARED_FOLDERS: {
            if (p->selected_index < 0 ||
                p->selected_index >= static_cast<int>(p->records.size()))
                break;
            const std::string& vm_id = p->records[p->selected_index].spec.vm_id;
            ShowSharedFoldersDialog(hwnd, shell->manager_, vm_id);
            return 0;
        }
        case IDM_VIEW_TOOLBAR: {
            auto& show = shell->manager_.app_settings().show_toolbar;
            show = !show;
            ShowWindow(p->toolbar, show ? SW_SHOW : SW_HIDE);
            HMENU view_menu = GetSubMenu(p->menu_bar, 2);
            if (view_menu) {
                CheckMenuItem(view_menu, IDM_VIEW_TOOLBAR,
                    MF_BYCOMMAND | (show ? MF_CHECKED : MF_UNCHECKED));
            }
            shell->manager_.SaveAppSettings();
            LayoutControls(p);
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
                        int new_sel = p->selected_index;
                        p->selected_index = -1;
                        ListView_SetItemState(p->vm_listview.handle(), new_sel,
                            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                    }
                    SendMessageA(p->statusbar, SB_SETTEXTA, 0,
                        reinterpret_cast<LPARAM>(i18n::tr(i18n::S::kStatusVmDeleted)));
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

        LRESULT lr = 0;
        if (p->vm_listview.HandleNotify(nmhdr, p->records, p->ui_font, &lr))
            return lr;

        if (nmhdr->idFrom == VmListView::kControlId &&
            nmhdr->code == LVN_ITEMCHANGED) {
            auto* nmlv = reinterpret_cast<NMLISTVIEW*>(nmhdr);
            if ((nmlv->uChanged & LVIF_STATE) &&
                (nmlv->uNewState & LVIS_SELECTED) &&
                !(nmlv->uOldState & LVIS_SELECTED)) {
                int sel = nmlv->iItem;
                if (sel >= 0 && sel < static_cast<int>(p->records.size()) &&
                    sel != p->selected_index) {
                    if (p->selected_index >= 0 &&
                        p->selected_index < static_cast<int>(p->records.size())) {
                        int cur_tab = static_cast<int>(SendMessage(p->tab, TCM_GETCURSEL, 0, 0));
                        const std::string& old_vm_id = p->records[p->selected_index].spec.vm_id;
                        p->GetVmUiState(old_vm_id).current_tab = cur_tab;
                    }

                    p->selected_index = sel;
                    const std::string& new_vm_id = p->records[sel].spec.vm_id;
                    VmUiState& new_state = p->GetVmUiState(new_vm_id);

                    p->last_sent_display_w = 0;
                    p->last_sent_display_h = 0;

                    p->info_tab.Update(&p->records[sel].spec);
                    UpdateCommandStates(p);

                    SendMessage(p->tab, TCM_SETCURSEL, new_state.current_tab, 0);

                    p->console_tab.SetText(new_state.console_state.text.c_str());

                    p->display_available = (new_state.fb_width > 0 && new_state.fb_height > 0);
                    if (p->display_available && !new_state.framebuffer.empty()) {
                        p->display_panel->RestoreFramebuffer(
                            new_state.fb_width, new_state.fb_height, new_state.framebuffer);
                        if (!new_state.cursor_pixels.empty()) {
                            p->display_panel->RestoreCursor(new_state.cursor, new_state.cursor_pixels);
                        }
                        if (new_state.current_tab == kTabDisplay) {
                            ResizeWindowForDisplay(p, new_state.fb_width, new_state.fb_height);
                        }
                    } else {
                        p->display_panel->Clear();
                    }

                    LayoutControls(p);
                }
            }
        }

        if (nmhdr->idFrom == IDC_TAB && nmhdr->code == TCN_SELCHANGE) {
            int cur_tab = static_cast<int>(SendMessage(p->tab, TCM_GETCURSEL, 0, 0));
            if (p->selected_index >= 0 &&
                p->selected_index < static_cast<int>(p->records.size())) {
                p->GetVmUiState(p->records[p->selected_index].spec.vm_id).current_tab = cur_tab;
            }
            LayoutControls(p);
        }
        break;
    }

    case WM_LBUTTONDOWN:
        if (p && HitTestSplitter(p, GET_X_LPARAM(lp))) {
            p->splitter_dragging = true;
            p->splitter_drag_start_x = GET_X_LPARAM(lp);
            p->splitter_drag_start_width = p->left_pane_width;
            SetCapture(hwnd);
            return 0;
        }
        break;

    case WM_MOUSEMOVE:
        if (p && p->splitter_dragging) {
            int dx = GET_X_LPARAM(lp) - p->splitter_drag_start_x;
            int new_w = p->splitter_drag_start_width + dx;
            if (new_w < Impl::kMinLeftPaneWidth) new_w = Impl::kMinLeftPaneWidth;
            if (new_w > Impl::kMaxLeftPaneWidth) new_w = Impl::kMaxLeftPaneWidth;
            if (new_w != p->left_pane_width) {
                p->left_pane_width = new_w;
                LayoutControls(p);
            }
            return 0;
        }
        if (p && p->vm_listview.HandleDragMove(lp)) return 0;
        break;

    case WM_LBUTTONUP:
        if (p && p->splitter_dragging) {
            p->splitter_dragging = false;
            ReleaseCapture();
            return 0;
        }
        if (p && p->vm_listview.HandleDragEnd(lp)) return 0;
        break;

    case WM_CONTEXTMENU: {
        if (p && reinterpret_cast<HWND>(wp) == p->vm_listview.handle() &&
            p->selected_index >= 0 && p->selected_index < static_cast<int>(p->records.size())) {
            int x = GET_X_LPARAM(lp);
            int y = GET_Y_LPARAM(lp);
            int idx = -1;
            if (x != -1 && y != -1) {
                LVHITTESTINFO hti{};
                hti.pt = {x, y};
                ScreenToClient(p->vm_listview.handle(), &hti.pt);
                int hit = ListView_HitTest(p->vm_listview.handle(), &hti);
                if (hit >= 0 && hit == p->selected_index) {
                    idx = p->selected_index;
                }
            } else {
                idx = p->selected_index;
            }
            if (idx >= 0) {
                HMENU vm_menu = GetSubMenu(p->menu_bar, 1);
                if (vm_menu) {
                    if (x == -1 && y == -1) {
                        RECT rc;
                        GetWindowRect(p->vm_listview.handle(), &rc);
                        x = (rc.left + rc.right) / 2;
                        y = (rc.top + rc.bottom) / 2;
                    }
                    TrackPopupMenuEx(vm_menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
                        x, y, hwnd, nullptr);
                }
            }
            return 0;
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
        if (!g_clipboard_from_vm) {
            auto vms = shell->manager_.ListVms();
            for (const auto& vm : vms) {
                if (vm.state == VmPowerState::kRunning) {
                    std::vector<uint32_t> types = {1};
                    shell->manager_.SendClipboardGrab(vm.spec.vm_id, types);
                }
            }
        } else {
            g_clipboard_from_vm = false;
        }
        return 0;

    case WM_CLOSE:
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
    impl_->menu_bar = BuildMenuBar(manager_.app_settings().show_toolbar);

    impl_->hwnd = CreateWindowExA(0, kWndClass, i18n::tr(i18n::S::kAppTitle),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x, y, w, h,
        nullptr, impl_->menu_bar, hinst, nullptr);

    g_main_hwnd = impl_->hwnd;
    SetWindowLongPtrA(impl_->hwnd, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(impl_.get()));

    AddClipboardFormatListener(impl_->hwnd);

    impl_->toolbar = CreateToolbar(impl_->hwnd, hinst);
    if (!manager_.app_settings().show_toolbar) {
        ShowWindow(impl_->toolbar, SW_HIDE);
    }
    impl_->statusbar = CreateWindowExA(0, STATUSCLASSNAMEA, nullptr,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, impl_->hwnd,
        reinterpret_cast<HMENU>(IDC_STATUSBAR), hinst, nullptr);

    // Components
    impl_->vm_listview.Create(impl_->hwnd, hinst, impl_->ui_font);
    impl_->vm_listview.SetDragDropCallback([this](int from, int to) {
        if (from == to) return;
        auto item = std::move(impl_->records[from]);
        impl_->records.erase(impl_->records.begin() + from);
        impl_->records.insert(impl_->records.begin() + to, std::move(item));
        impl_->selected_index = to;
        impl_->vm_listview.Populate(impl_->records, to);
        impl_->info_tab.Update(&impl_->records[to].spec);
        UpdateCommandStates(impl_.get());
        manager_.ReorderVm(from, to);
    });
    impl_->info_tab.Create(impl_->hwnd, hinst, impl_->ui_font);
    impl_->console_tab.Create(impl_->hwnd, hinst, impl_->mono_font, impl_->ui_font);

    // Tab control
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
    }

    // Apply font to toolbar
    SendMessage(impl_->toolbar, WM_SETFONT,
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

    // Wire callbacks
    manager_.SetDisplayCallback(
        [this](const std::string& vm_id, DisplayFrame frame) {
            InvokeOnUiThread([this, vm_id, frame = std::move(frame)]() {
                VmUiState& state = impl_->GetVmUiState(vm_id);
                BlitFrameToState(state, frame);

                bool is_current = (impl_->selected_index >= 0 &&
                    impl_->selected_index < static_cast<int>(impl_->records.size()) &&
                    impl_->records[impl_->selected_index].spec.vm_id == vm_id);
                if (is_current) {
                    impl_->display_panel->AdoptFramebuffer(
                        state.fb_width, state.fb_height,
                        state.framebuffer.data(), state.framebuffer.size());
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
            std::string added = ConsoleTab::FilterAndAppend(state.console_state, data);

            bool is_current = (impl_->selected_index >= 0 &&
                impl_->selected_index < static_cast<int>(impl_->records.size()) &&
                impl_->records[impl_->selected_index].spec.vm_id == vm_id);
            if (is_current && !added.empty()) {
                impl_->console_tab.AppendText(added);
            }
        });
    });

    manager_.SetAudioPcmCallback(
        [this](const std::string& vm_id, AudioChunk chunk) {
            if (chunk.pcm.empty()) return;
            WasapiAudioPlayer& player = impl_->GetAudioPlayer(vm_id);
            player.SubmitPcm(chunk.sample_rate, chunk.channels,
                             std::move(chunk.pcm));
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
                impl_->audio_players.erase(vm_id);
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

    manager_.SetGuestAgentStateCallback(
        [this](const std::string& vm_id, bool connected) {
            (void)connected;
            InvokeOnUiThread([this, vm_id]() {
                RefreshVmList();
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
        bool forwarded = false;
        if (msg.message == WM_KEYDOWN || msg.message == WM_KEYUP ||
            msg.message == WM_SYSKEYDOWN || msg.message == WM_SYSKEYUP) {
            int cur_tab = static_cast<int>(SendMessage(impl_->tab, TCM_GETCURSEL, 0, 0));
            if (cur_tab == kTabDisplay && impl_->display_available && impl_->display_panel) {
                HWND panel_hwnd = impl_->display_panel->Handle();
                if (msg.hwnd != panel_hwnd && msg.hwnd != impl_->console_tab.input_handle()) {
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

    impl_->vm_listview.Populate(impl_->records, impl_->selected_index);

    const VmSpec* spec = nullptr;
    if (impl_->selected_index >= 0 &&
        impl_->selected_index < static_cast<int>(impl_->records.size())) {
        spec = &impl_->records[impl_->selected_index].spec;
    }
    impl_->info_tab.Update(spec);
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
