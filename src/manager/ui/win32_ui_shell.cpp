#include "manager/ui/win32_ui_shell.h"
#include "manager/ui/win32_dialogs.h"
#include "manager/ui/create_vm_dialog.h"
#include "manager/ui/settings_dialog.h"
#include "manager/ui/llm_proxy_dialog.h"
#include "manager/ui/win32_display_panel.h"
#include "manager/ui/info_tab.h"
#include "manager/ui/console_tab.h"
#include "manager/ui/vm_listview.h"
#include "manager/i18n.h"
#include "manager/app_settings.h"
#include "manager/resource.h"
#include "version.h"

#include <winsparkle.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>

#pragma comment(lib, "comctl32.lib")

#include "common/ports.h"
#include "manager/audio/wasapi_audio_player.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
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
    IDM_PORT_FORWARDS  = 1023,
    IDM_CLONE          = 1024,
    IDM_VIEW_TOOLBAR           = 1017,
    IDM_WEBSITE        = 1020,
    IDM_CHECK_UPDATE  = 1021,
    IDM_ABOUT         = 1022,
    IDM_SETTINGS      = 1025,
    IDM_DPI_ZOOM      = 1026,
    IDM_LLM_PROXY     = 1027,
    IDM_HELP_DOC      = 1028,
};

// ── Control IDs ──

enum CtlId : UINT {
    IDC_TOOLBAR     = 2001,
    IDC_STATUSBAR   = 2002,
    IDC_TAB         = 2007,
    IDC_SPRING_SEP  = 2010,
};

// WM_APP range for cross-thread invoke
static constexpr UINT WM_INVOKE = WM_APP + 100;

// Suppress host→guest clipboard echo after we write VM data to the clipboard.
// Uses a timestamp instead of a simple bool because EmptyClipboard() +
// SetClipboardData() can trigger multiple WM_CLIPBOARDUPDATE notifications.
static ULONGLONG g_clipboard_suppress_until = 0;
static constexpr ULONGLONG kClipboardSuppressMs = 500;

static int ScaleDpi(int px, UINT dpi) { return MulDiv(px, static_cast<int>(dpi), 96); }

static UINT GetWindowDpi(HWND hwnd) {
    UINT dpi = GetDpiForWindow(hwnd);
    if (dpi == 0) {
        HDC hdc = GetDC(hwnd);
        dpi = static_cast<UINT>(GetDeviceCaps(hdc, LOGPIXELSX));
        ReleaseDC(hwnd, hdc);
    }
    return dpi ? dpi : 96;
}

static constexpr int kDefaultLeftPaneWidth96 = 200;

// ── Forward declarations for dialog helpers ──

extern bool ShowEditVmDialog(HWND parent, ManagerService& mgr,
                             const VmRecord& rec, std::string* error);
extern void ShowSharedFoldersDialog(HWND parent, ManagerService& mgr,
                                    const std::string& vm_id);
extern void ShowPortForwardsDialog(HWND parent, ManagerService& mgr,
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
    UINT  dpi         = 96;

    int Dpi(int px96) const { return ScaleDpi(px96, dpi); }

    void RecreateMonoFont() {
        if (mono_font) DeleteObject(mono_font);
        mono_font = CreateFontW(-MulDiv(10, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Consolas");
    }

    void RecreateUiFont() {
        if (ui_font) DeleteObject(ui_font);
        NONCLIENTMETRICSW ncm{sizeof(ncm)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
        ui_font = CreateFontIndirectW(&ncm.lfMessageFont);
    }

    // Components
    VmListView vm_listview;
    InfoTab info_tab;
    ConsoleTab console_tab;
    std::unique_ptr<DisplayPanel> display_panel;

    std::vector<VmRecord> records;
    int selected_index = -1;

    // Splitter between left pane (VM list) and right pane (tabs)
    int left_pane_width = kDefaultLeftPaneWidth96;
    bool splitter_dragging = false;
    int splitter_drag_start_x = 0;
    int splitter_drag_start_width = 0;

    int SplitterWidth()    const { return Dpi(4); }
    int MinLeftPaneWidth() const { return Dpi(180); }
    int MaxLeftPaneWidth() const { return Dpi(500); }

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
        if (src_off + src_stride > frame.data_size()) break;
        if (dst_off + dw * 4 > state.framebuffer.size()) break;
        std::memcpy(state.framebuffer.data() + dst_off,
                    frame.data() + src_off, dw * 4);
    }
}

// ── Window class registration ──

static const wchar_t* kWndClass = L"TenBoxManagerWin32";
static LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);

static ATOM RegisterMainClass(HINSTANCE hinst) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWndClass;
    wc.hIcon         = static_cast<HICON>(LoadImageW(hinst, MAKEINTRESOURCEW(IDI_APP_ICON),
                           IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    wc.hIconSm       = static_cast<HICON>(LoadImageW(hinst, MAKEINTRESOURCEW(IDI_APP_ICON),
                           IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                           GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    return RegisterClassExW(&wc);
}

// ── Menu building ──

static HMENU BuildMenuBar(bool show_toolbar) {
    using S = i18n::S;
    HMENU bar = CreateMenu();

    HMENU file_menu = CreatePopupMenu();
    AppendMenuW(file_menu, MF_STRING, IDM_NEW_VM, i18n::tr_w(S::kMenuNewVm).c_str());
    AppendMenuW(file_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file_menu, MF_STRING, IDM_SETTINGS, i18n::tr_w(S::kMenuSettings).c_str());
    AppendMenuW(file_menu, MF_STRING, IDM_LLM_PROXY, i18n::tr_w(S::kMenuLlmProxy).c_str());
    AppendMenuW(file_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file_menu, MF_STRING, IDM_EXIT, i18n::tr_w(S::kMenuExit).c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file_menu), i18n::tr_w(S::kMenuManager).c_str());

    HMENU vm_menu = CreatePopupMenu();
    AppendMenuW(vm_menu, MF_STRING, IDM_EDIT,     i18n::tr_w(S::kMenuEdit).c_str());
    AppendMenuW(vm_menu, MF_STRING, IDM_CLONE,    i18n::tr_w(S::kMenuClone).c_str());
    AppendMenuW(vm_menu, MF_STRING, IDM_DELETE,   i18n::tr_w(S::kMenuDelete).c_str());
    AppendMenuW(vm_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(vm_menu, MF_STRING, IDM_START,    i18n::tr_w(S::kMenuStart).c_str());
    AppendMenuW(vm_menu, MF_STRING, IDM_STOP,     i18n::tr_w(S::kMenuStop).c_str());
    AppendMenuW(vm_menu, MF_STRING, IDM_REBOOT,   i18n::tr_w(S::kMenuReboot).c_str());
    AppendMenuW(vm_menu, MF_STRING, IDM_SHUTDOWN, i18n::tr_w(S::kMenuShutdown).c_str());
    AppendMenuW(vm_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(vm_menu, MF_STRING, IDM_SHARED_FOLDERS, i18n::tr_w(S::kToolbarSharedFolders).c_str());
    AppendMenuW(vm_menu, MF_STRING, IDM_PORT_FORWARDS, i18n::tr_w(S::kMenuPortForwards).c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(vm_menu), i18n::tr_w(S::kMenuVm).c_str());

    HMENU view_menu = CreatePopupMenu();
    AppendMenuW(view_menu, MF_STRING | (show_toolbar ? MF_CHECKED : MF_UNCHECKED),
               IDM_VIEW_TOOLBAR, i18n::tr_w(S::kMenuViewToolbar).c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view_menu), i18n::tr_w(S::kMenuView).c_str());

    HMENU help_menu = CreatePopupMenu();
    AppendMenuW(help_menu, MF_STRING, IDM_WEBSITE,      i18n::tr_w(S::kMenuWebsite).c_str());
    AppendMenuW(help_menu, MF_STRING, IDM_HELP_DOC,     i18n::tr_w(S::kMenuHelpDoc).c_str());
    AppendMenuW(help_menu, MF_STRING, IDM_CHECK_UPDATE,  i18n::tr_w(S::kMenuCheckUpdate).c_str());
    AppendMenuW(help_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(help_menu, MF_STRING, IDM_ABOUT,        i18n::tr_w(S::kMenuAbout).c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help_menu), i18n::tr_w(S::kMenuHelp).c_str());

    return bar;
}

// ── Toolbar building ──

static constexpr int kToolbarIconSize96 = 32;

static HIMAGELIST CreateToolbarImageList(HINSTANCE hinst, int icon_size) {
    const UINT bmp_ids[] = {
        IDB_TOOLBAR_NEW_VM,
        IDB_TOOLBAR_EDIT,
        IDB_TOOLBAR_DELETE,
        IDB_TOOLBAR_START,
        IDB_TOOLBAR_STOP,
        IDB_TOOLBAR_REBOOT,
        IDB_TOOLBAR_SHUTDOWN,
        IDB_TOOLBAR_SHARED_FOLDERS,
        IDB_TOOLBAR_PORT_FORWARDS,
        IDB_TOOLBAR_DPI_ZOOM,
        IDB_TOOLBAR_LLM_PROXY,
    };

    HIMAGELIST hil = ImageList_Create(
        icon_size, icon_size,
        ILC_COLOR32 | ILC_MASK, 11, 0);
    if (!hil) return nullptr;

    for (UINT id : bmp_ids) {
        HBITMAP hbm = static_cast<HBITMAP>(LoadImageW(
            hinst, MAKEINTRESOURCEW(id), IMAGE_BITMAP,
            icon_size, icon_size, LR_CREATEDIBSECTION));
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

static HWND CreateToolbar(HWND parent, HINSTANCE hinst, UINT dpi) {
    HWND tb = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | CCS_TOP,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_TOOLBAR), hinst, nullptr);

    SendMessage(tb, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);

    int icon_size = ScaleDpi(kToolbarIconSize96, dpi);
    HIMAGELIST hil = CreateToolbarImageList(hinst, icon_size);
    bool has_icons = (hil != nullptr);
    if (has_icons) {
        SendMessage(tb, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(hil));
        SendMessage(tb, TB_SETPADDING, 0, MAKELPARAM(ScaleDpi(14, dpi), ScaleDpi(8, dpi)));
    } else {
        SendMessage(tb, TB_SETPADDING, 0, MAKELPARAM(ScaleDpi(10, dpi), ScaleDpi(4, dpi)));
    }

    using S = i18n::S;
    struct ToolbarItem { UINT id; const char* text; int icon_idx; BYTE extra_style; };
    const ToolbarItem items[] = {
        {IDM_NEW_VM,        i18n::tr(S::kToolbarNewVm),          0, 0},
        {IDM_EDIT,          i18n::tr(S::kToolbarEdit),           1, 0},
        {IDM_DELETE,        i18n::tr(S::kToolbarDelete),         2, 0},
        {0,                 nullptr,                             -1, 0},
        {IDM_START,         i18n::tr(S::kToolbarStart),          3, 0},
        {IDM_STOP,          i18n::tr(S::kToolbarStop),           4, 0},
        {IDM_REBOOT,        i18n::tr(S::kToolbarReboot),         5, 0},
        {IDM_SHUTDOWN,      i18n::tr(S::kToolbarShutdown),       6, 0},
        {0,                 nullptr,                             -1, 0},
        {IDM_SHARED_FOLDERS,i18n::tr(S::kToolbarSharedFolders),  7, 0},
        {IDM_PORT_FORWARDS, i18n::tr(S::kToolbarPortForwards),   8, 0},
        {0,                 nullptr,                             -1, 0},
        {IDM_DPI_ZOOM,      i18n::tr(S::kToolbarDpiZoom),        9, BTNS_CHECK},
        {IDC_SPRING_SEP,    nullptr,                             -1, 0},
        {IDM_LLM_PROXY,     i18n::tr(S::kToolbarLlmProxy),     10, 0},
    };

    std::vector<std::wstring> wtexts;
    wtexts.reserve(std::size(items));
    for (const auto& item : items) {
        wtexts.push_back(item.text ? i18n::to_wide(item.text) : std::wstring());
    }
    for (size_t i = 0; i < std::size(items); i++) {
        const auto& item = items[i];
        TBBUTTON btn{};
        if (item.id == 0 || item.id == IDC_SPRING_SEP) {
            btn.fsStyle = BTNS_SEP;
            btn.idCommand = item.id;
        } else {
            btn.iBitmap   = has_icons ? item.icon_idx : I_IMAGENONE;
            btn.idCommand = item.id;
            btn.fsState   = TBSTATE_ENABLED;
            btn.fsStyle   = BTNS_BUTTON | BTNS_AUTOSIZE | item.extra_style;
            btn.iString   = reinterpret_cast<INT_PTR>(wtexts[i].c_str());
        }
        SendMessageW(tb, TB_ADDBUTTONSW, 1, reinterpret_cast<LPARAM>(&btn));
    }

    SendMessage(tb, TB_AUTOSIZE, 0, 0);
    return tb;
}

// ── Spring separator ──
// Adjusts the spring separator width so that buttons after it are right-aligned.

static void UpdateToolbarSpring(HWND tb) {
    int spring_idx = static_cast<int>(SendMessage(tb, TB_COMMANDTOINDEX, IDC_SPRING_SEP, 0));
    if (spring_idx < 0) return;

    int count = static_cast<int>(SendMessage(tb, TB_BUTTONCOUNT, 0, 0));

    // First, set spring to minimal width so TB_AUTOSIZE computes natural layout
    TBBUTTONINFOW info{};
    info.cbSize = sizeof(info);
    info.dwMask = TBIF_SIZE;
    info.cx = 0;
    SendMessageW(tb, TB_SETBUTTONINFOW, IDC_SPRING_SEP, reinterpret_cast<LPARAM>(&info));
    SendMessage(tb, TB_AUTOSIZE, 0, 0);

    // Measure width of all buttons after the spring
    int right_width = 0;
    for (int i = spring_idx + 1; i < count; ++i) {
        RECT r{};
        SendMessage(tb, TB_GETITEMRECT, i, reinterpret_cast<LPARAM>(&r));
        right_width += r.right - r.left;
    }

    // Get toolbar client width
    RECT rc;
    GetClientRect(tb, &rc);
    int tb_width = rc.right;

    // Get the left edge of the spring separator (= total width of all buttons before it)
    RECT spring_rc{};
    SendMessage(tb, TB_GETITEMRECT, spring_idx, reinterpret_cast<LPARAM>(&spring_rc));
    int left_width = spring_rc.left;

    int spring_width = tb_width - left_width - right_width;
    if (spring_width < 0) spring_width = 0;

    info.cx = static_cast<WORD>(spring_width);
    SendMessageW(tb, TB_SETBUTTONINFOW, IDC_SPRING_SEP, reinterpret_cast<LPARAM>(&info));
}

// ── Layout helper ──

using Impl = Win32UiShell::Impl;

// ── Toolbar badge drawing ──

static int GetBadgeCount(Impl* p, UINT cmd_id) {
    if (p->selected_index < 0 ||
        p->selected_index >= static_cast<int>(p->records.size()))
        return 0;
    const auto& spec = p->records[p->selected_index].spec;
    if (cmd_id == IDM_SHARED_FOLDERS)
        return static_cast<int>(spec.shared_folders.size());
    if (cmd_id == IDM_PORT_FORWARDS)
        return static_cast<int>(spec.port_forwards.size() + spec.guest_forwards.size());
    return 0;
}

static void DrawToolbarBadge(HDC hdc, const RECT& btn_rect, int count, UINT dpi) {
    if (count <= 0) return;

    wchar_t text[8];
    swprintf_s(text, L"%d", count);

    int radius = ScaleDpi(8, dpi);
    int cx = btn_rect.right - ScaleDpi(12, dpi);
    int cy = btn_rect.top + ScaleDpi(10, dpi);

    HBRUSH brush = CreateSolidBrush(RGB(120, 120, 120));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 120, 120));
    HBRUSH old_brush = static_cast<HBRUSH>(SelectObject(hdc, brush));
    HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, pen));

    Ellipse(hdc, cx - radius, cy - radius, cx + radius, cy + radius);

    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);

    HFONT badge_font = CreateFontW(-MulDiv(8, static_cast<int>(dpi), 72), 0, 0, 0, FW_BOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT old_font = static_cast<HFONT>(SelectObject(hdc, badge_font));
    SetTextColor(hdc, RGB(255, 255, 255));
    SetBkMode(hdc, TRANSPARENT);

    RECT text_rect = { cx - radius, cy - radius, cx + radius, cy + radius };
    DrawTextW(hdc, text, -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, old_font);
    DeleteObject(badge_font);
}

static LRESULT HandleToolbarCustomDraw(Impl* p, LPNMTBCUSTOMDRAW cd) {
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        return CDRF_NOTIFYPOSTPAINT;
    case CDDS_ITEMPOSTPAINT: {
        UINT cmd = static_cast<UINT>(cd->nmcd.dwItemSpec);
        if (cmd == IDM_SHARED_FOLDERS || cmd == IDM_PORT_FORWARDS) {
            int count = GetBadgeCount(p, cmd);
            if (count > 0) {
                DrawToolbarBadge(cd->nmcd.hdc, cd->nmcd.rc, count, p->dpi);
            }
        }
        return CDRF_DODEFAULT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

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

    // When DPI-scaled, VM resolution is lower than panel size;
    // expand panel size to match the upscaled framebuffer display.
    int panel_w = static_cast<int>(vm_width);
    int panel_h = static_cast<int>(vm_height);
    bool scaled = (p->selected_index >= 0 &&
        p->selected_index < static_cast<int>(p->records.size()) &&
        p->records[p->selected_index].spec.dpi_scaled);
    if (scaled && p->dpi != 96) {
        panel_w = MulDiv(panel_w, static_cast<int>(p->dpi), 96);
        panel_h = MulDiv(panel_h, static_cast<int>(p->dpi), 96);
    }

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

    int target_cw = p->left_pane_width + p->SplitterWidth() + tab_extra_w + panel_w;
    int target_ch = tb_h + tab_extra_h + panel_h + sb_h;

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
        UpdateToolbarSpring(p->toolbar);
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

    int rx = p->left_pane_width + p->SplitterWidth();
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

            bool scaled = (p->selected_index >= 0 &&
                p->selected_index < static_cast<int>(p->records.size()) &&
                p->records[p->selected_index].spec.dpi_scaled);
            uint32_t disp_w, disp_h;
            if (scaled && p->dpi != 96) {
                disp_w = pw > 0 ? (static_cast<uint32_t>(MulDiv(pw, 96, p->dpi)) & ~7u) : 0;
                disp_h = ph > 0 ? static_cast<uint32_t>(MulDiv(ph, 96, p->dpi)) : 0;
            } else {
                disp_w = pw > 0 ? (static_cast<uint32_t>(pw) & ~7u) : 0;
                disp_h = ph > 0 ? static_cast<uint32_t>(ph) : 0;
            }

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
    EnableCmd(IDM_CLONE,          has_sel && !running);
    EnableCmd(IDM_DELETE,         has_sel && !running);
    EnableCmd(IDM_SHARED_FOLDERS, has_sel);
    EnableCmd(IDM_PORT_FORWARDS, has_sel);

    SendMessage(p->toolbar, TB_ENABLEBUTTON, IDM_DPI_ZOOM,
                MAKELONG((has_sel && p->dpi != 96) ? TRUE : FALSE, 0));
    bool dpi_scaled = has_sel && p->records[p->selected_index].spec.dpi_scaled;
    SendMessage(p->toolbar, TB_CHECKBUTTON, IDM_DPI_ZOOM,
                MAKELONG(dpi_scaled ? TRUE : FALSE, 0));

    InvalidateRect(p->toolbar, nullptr, FALSE);

    p->console_tab.SetEnabled(running);
}

// ── Splitter hit-test ──

static bool HitTestSplitter(Impl* p, int client_x) {
    return client_x >= p->left_pane_width &&
           client_x <  p->left_pane_width + p->SplitterWidth();
}

// ── WndProc ──

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* shell = g_shell;
    if (!shell) return DefWindowProcW(hwnd, msg, wp, lp);
    auto* p = reinterpret_cast<Impl*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

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
            wchar_t buf[1024]{};
            GetWindowTextW(p->console_tab.input_handle(), buf, static_cast<int>(std::size(buf)));
            std::string input = i18n::wide_to_utf8(buf);
            if (p->selected_index >= 0 &&
                p->selected_index < static_cast<int>(p->records.size())) {
                std::string vm_id = p->records[p->selected_index].spec.vm_id;
                std::string to_send = input + "\n";
                if (shell->manager_.SendConsoleInput(vm_id, to_send)) {
                    SetWindowTextW(p->console_tab.input_handle(), L"");
                }
            }
            return 0;
        }

        switch (cmd) {
        case IDM_NEW_VM: {
            std::string error;
            if (ShowCreateVmDialog2(hwnd, shell->manager_, &error)) {
                shell->RefreshVmList();
            } else if (!error.empty()) {
                MessageBoxW(hwnd, i18n::to_wide(error).c_str(), i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        case IDM_SETTINGS:
            ShowSettingsDialog(hwnd, shell->manager_);
            return 0;
        case IDM_LLM_PROXY:
            ShowLlmProxyDialog(hwnd, shell->manager_.app_settings().llm_proxy,
                [&shell](const settings::LlmProxySettings& settings) {
                    shell->manager_.app_settings().llm_proxy = settings;
                    shell->manager_.NotifyLlmProxyChanged();
                });
            return 0;
        case IDM_EXIT:
            SendMessage(hwnd, WM_CLOSE, 0, 0);
            return 0;
        case IDM_WEBSITE: {
            SHELLEXECUTEINFOW sei{sizeof(sei)};
            sei.hwnd = hwnd;
            sei.lpVerb = L"open";
            sei.lpFile = L"https://tenbox.ai/";
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
            return 0;
        }
        case IDM_HELP_DOC: {
            SHELLEXECUTEINFOW sei{sizeof(sei)};
            sei.hwnd = hwnd;
            sei.lpVerb = L"open";
            sei.lpFile = L"https://my.feishu.cn/wiki/Q96KwUH1Di3cAik2W7kcQsWKncb";
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
            return 0;
        }
        case IDM_CHECK_UPDATE:
            win_sparkle_check_update_with_ui();
            return 0;
        case IDM_ABOUT:
            MessageBoxW(hwnd, i18n::tr_w(i18n::S::kAboutText).c_str(),
                i18n::tr_w(i18n::S::kAboutTitle).c_str(), MB_OK | MB_ICONINFORMATION);
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
            SendMessageW(p->statusbar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(i18n::to_wide(status).c_str()));
            std::string error;
            bool ok = shell->manager_.StartVm(vm_id, &error);
            shell->RefreshVmList();
            if (ok) {
                status = i18n::fmt(i18n::S::kStatusStarted, vm_id.c_str());
                SendMessageW(p->statusbar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(i18n::to_wide(status).c_str()));
            } else {
                status = std::string(i18n::tr(i18n::S::kStatusErrorPrefix)) + error;
                SendMessageW(p->statusbar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(i18n::to_wide(status).c_str()));
                MessageBoxW(hwnd, i18n::to_wide(error).c_str(), i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        case IDM_STOP: {
            if (p->selected_index < 0 ||
                p->selected_index >= static_cast<int>(p->records.size()))
                break;
            std::string vm_id = p->records[p->selected_index].spec.vm_id;
            std::string vm_name = p->records[p->selected_index].spec.name;
            auto prompt = i18n::fmt(i18n::S::kConfirmForceStopMsg, vm_name.c_str());
            if (MessageBoxW(hwnd, i18n::to_wide(prompt).c_str(), i18n::tr_w(i18n::S::kConfirmForceStopTitle).c_str(),
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                return 0;
            }
            std::string error;
            bool ok = shell->manager_.StopVm(vm_id, &error);
            shell->RefreshVmList();
            auto status = ok ? i18n::fmt(i18n::S::kStatusStopped, vm_id.c_str())
                             : (std::string(i18n::tr(i18n::S::kStatusErrorPrefix)) + error);
            SendMessageW(p->statusbar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(i18n::to_wide(status).c_str()));
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
            SendMessageW(p->statusbar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(i18n::to_wide(status).c_str()));
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
            SendMessageW(p->statusbar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(i18n::to_wide(status).c_str()));
            return 0;
        }
        case IDM_SHARED_FOLDERS: {
            if (p->selected_index < 0 ||
                p->selected_index >= static_cast<int>(p->records.size()))
                break;
            const std::string& vm_id = p->records[p->selected_index].spec.vm_id;
            ShowSharedFoldersDialog(hwnd, shell->manager_, vm_id);
            shell->RefreshVmList();
            return 0;
        }
        case IDM_PORT_FORWARDS: {
            if (p->selected_index < 0 ||
                p->selected_index >= static_cast<int>(p->records.size()))
                break;
            const std::string& vm_id = p->records[p->selected_index].spec.vm_id;
            ShowPortForwardsDialog(hwnd, shell->manager_, vm_id);
            shell->RefreshVmList();
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
        case IDM_DPI_ZOOM: {
            if (p->selected_index < 0 ||
                p->selected_index >= static_cast<int>(p->records.size()))
                break;
            auto& spec = p->records[p->selected_index].spec;
            bool new_val = !spec.dpi_scaled;
            shell->manager_.SetVmDpiScaled(spec.vm_id, new_val);
            spec.dpi_scaled = new_val;

            float factor = new_val ? (static_cast<float>(p->dpi) / 96.0f) : 1.0f;
            if (p->display_panel) p->display_panel->SetDpiZoomFactor(factor);

            SendMessage(p->toolbar, TB_CHECKBUTTON, IDM_DPI_ZOOM,
                        MAKELONG(new_val ? TRUE : FALSE, 0));

            p->last_sent_display_w = 0;
            p->last_sent_display_h = 0;
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
                SendMessageW(p->statusbar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(i18n::to_wide(status).c_str()));
            } else if (!error.empty()) {
                MessageBoxW(hwnd, i18n::to_wide(error).c_str(), i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        case IDM_CLONE: {
            if (p->selected_index < 0 ||
                p->selected_index >= static_cast<int>(p->records.size()))
                break;
            std::string vm_id   = p->records[p->selected_index].spec.vm_id;
            std::string vm_name = p->records[p->selected_index].spec.name;
            auto status_msg = i18n::fmt(i18n::S::kStatusVmCloning, vm_name.c_str());
            SendMessageW(p->statusbar, SB_SETTEXTW, 0,
                reinterpret_cast<LPARAM>(i18n::to_wide(status_msg).c_str()));
            std::string error;
            if (shell->manager_.CloneVm(vm_id, &error)) {
                shell->RefreshVmList();
                auto done_msg = i18n::fmt(i18n::S::kStatusVmCloned, vm_name.c_str());
                SendMessageW(p->statusbar, SB_SETTEXTW, 0,
                    reinterpret_cast<LPARAM>(i18n::to_wide(done_msg).c_str()));
            } else {
                MessageBoxW(hwnd, i18n::to_wide(error).c_str(), i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONERROR);
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
            if (MessageBoxW(hwnd, i18n::to_wide(prompt).c_str(), i18n::tr_w(i18n::S::kConfirmDeleteTitle).c_str(),
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
                std::string error;
                if (shell->manager_.DeleteVm(vm_id, &error)) {
                    p->vm_ui_states.erase(vm_id);
                    p->selected_index = -1;
                    p->display_available = false;
                    shell->RefreshVmList();
                    LayoutControls(p);
                    SendMessageW(p->statusbar, SB_SETTEXTW, 0,
                        reinterpret_cast<LPARAM>(i18n::tr_w(i18n::S::kStatusVmDeleted).c_str()));
                } else {
                    MessageBoxW(hwnd, i18n::to_wide(error).c_str(), i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONERROR);
                }
            }
            return 0;
        }
        } // switch cmd
        break;
    }

    case WM_NOTIFY: {
        auto* nmhdr = reinterpret_cast<NMHDR*>(lp);

        if (nmhdr->idFrom == IDC_TOOLBAR && nmhdr->code == NM_CUSTOMDRAW) {
            return HandleToolbarCustomDraw(p,
                reinterpret_cast<LPNMTBCUSTOMDRAW>(lp));
        }

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

                    float factor = p->records[sel].spec.dpi_scaled
                        ? (static_cast<float>(p->dpi) / 96.0f) : 1.0f;
                    if (p->display_panel) p->display_panel->SetDpiZoomFactor(factor);

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
            if (new_w < p->MinLeftPaneWidth()) new_w = p->MinLeftPaneWidth();
            if (new_w > p->MaxLeftPaneWidth()) new_w = p->MaxLeftPaneWidth();
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
        if (GetTickCount64() >= g_clipboard_suppress_until) {
            auto vms = shell->manager_.ListVms();
            for (const auto& vm : vms) {
                if (vm.state == VmPowerState::kRunning) {
                    std::vector<uint32_t> types = {1};
                    shell->manager_.SendClipboardGrab(vm.spec.vm_id, types);
                }
            }
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
                int result = MessageBoxW(hwnd, i18n::to_wide(exit_msg).c_str(), i18n::tr_w(i18n::S::kConfirmExitTitle).c_str(),
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

    case WM_DPICHANGED: {
        if (!p) break;
        UINT new_dpi = HIWORD(wp);
        UINT old_dpi = p->dpi;
        p->dpi = new_dpi;

        p->left_pane_width = MulDiv(p->left_pane_width, static_cast<int>(new_dpi), static_cast<int>(old_dpi));

        p->RecreateUiFont();
        p->RecreateMonoFont();

        SendMessage(p->toolbar, WM_SETFONT, reinterpret_cast<WPARAM>(p->ui_font), FALSE);
        SendMessage(p->tab, WM_SETFONT, reinterpret_cast<WPARAM>(p->ui_font), FALSE);
        SendMessage(p->vm_listview.handle(), WM_SETFONT, reinterpret_cast<WPARAM>(p->ui_font), FALSE);
        p->vm_listview.UpdateRowHeight(p->dpi);
        p->console_tab.UpdateFonts(p->mono_font, p->ui_font);

        {
            HINSTANCE hinst = GetModuleHandle(nullptr);
            HWND old_tb = p->toolbar;
            bool was_visible = IsWindowVisible(old_tb) != FALSE;
            DestroyWindow(old_tb);
            p->toolbar = CreateToolbar(hwnd, hinst, new_dpi);
            SendMessage(p->toolbar, WM_SETFONT, reinterpret_cast<WPARAM>(p->ui_font), FALSE);
            if (!was_visible) ShowWindow(p->toolbar, SW_HIDE);
        }

        const RECT* suggested = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr,
            suggested->left, suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);

        if (p->selected_index >= 0 && p->selected_index < static_cast<int>(p->records.size())) {
            p->vm_listview.Populate(p->records, p->selected_index);
            if (p->display_panel && p->records[p->selected_index].spec.dpi_scaled) {
                p->display_panel->SetDpiZoomFactor(static_cast<float>(new_dpi) / 96.0f);
            }
        }
        p->last_sent_display_w = 0;
        p->last_sent_display_h = 0;
        UpdateCommandStates(p);
        LayoutControls(p);
        return 0;
    }

    case WM_POWERBROADCAST:
        // Host resumed from sleep: push wall time to guests via qemu-ga.
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            shell->manager_.SyncGuestTimeForRunningVms();
            return TRUE;
        }
        break;

    case WM_ACTIVATEAPP:
        if (!wp && p && p->display_panel) {
            p->display_panel->ReleaseAllModifiers();
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
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

    // Get initial DPI from primary monitor
    HDC screen_dc = GetDC(nullptr);
    impl_->dpi = static_cast<UINT>(GetDeviceCaps(screen_dc, LOGPIXELSX));
    ReleaseDC(nullptr, screen_dc);
    if (impl_->dpi == 0) impl_->dpi = 96;

    // Fonts
    impl_->RecreateUiFont();
    impl_->RecreateMonoFont();

    // Restore geometry
    const auto& geo = manager_.app_settings().window;
    int x = (geo.x >= 0) ? geo.x : CW_USEDEFAULT;
    int y = (geo.y >= 0) ? geo.y : CW_USEDEFAULT;
    int w = (geo.width > 0) ? geo.width : impl_->Dpi(1024);
    int h = (geo.height > 0) ? geo.height : impl_->Dpi(680);

    // Clamp the restored window rectangle into the nearest monitor's work area,
    // so the window stays visible when a previously used monitor is gone or the
    // display layout has changed.
    if (x != CW_USEDEFAULT && y != CW_USEDEFAULT) {
        RECT saved{ x, y, x + w, y + h };
        HMONITOR mon = MonitorFromRect(&saved, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (mon && GetMonitorInfoW(mon, &mi)) {
            const RECT& wa = mi.rcWork;
            const int wa_w = wa.right - wa.left;
            const int wa_h = wa.bottom - wa.top;
            if (w > wa_w) w = wa_w;
            if (h > wa_h) h = wa_h;
            if (x < wa.left)         x = wa.left;
            if (y < wa.top)          y = wa.top;
            if (x + w > wa.right)    x = wa.right  - w;
            if (y + h > wa.bottom)   y = wa.bottom - h;
        }
    }

    i18n::InitLanguage();
    impl_->menu_bar = BuildMenuBar(manager_.app_settings().show_toolbar);

    impl_->hwnd = CreateWindowExW(0, kWndClass, i18n::tr_w(i18n::S::kAppTitle).c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x, y, w, h,
        nullptr, impl_->menu_bar, hinst, nullptr);

    g_main_hwnd = impl_->hwnd;
    SetWindowLongPtrW(impl_->hwnd, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(impl_.get()));

    // Refresh DPI from the actual window (may differ from screen DC if on a secondary monitor)
    UINT actual_dpi = GetWindowDpi(impl_->hwnd);
    if (actual_dpi != impl_->dpi) {
        impl_->dpi = actual_dpi;
        impl_->RecreateUiFont();
        impl_->RecreateMonoFont();
    }
    impl_->left_pane_width = impl_->Dpi(kDefaultLeftPaneWidth96);

    AddClipboardFormatListener(impl_->hwnd);

    impl_->toolbar = CreateToolbar(impl_->hwnd, hinst, impl_->dpi);
    if (!manager_.app_settings().show_toolbar) {
        ShowWindow(impl_->toolbar, SW_HIDE);
    }
    impl_->statusbar = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, impl_->hwnd,
        reinterpret_cast<HMENU>(IDC_STATUSBAR), hinst, nullptr);

    // Components
    impl_->vm_listview.Create(impl_->hwnd, hinst, impl_->ui_font, impl_->dpi);
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
    impl_->tab = CreateWindowExW(0, WC_TABCONTROLW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 0, 0, impl_->hwnd,
        reinterpret_cast<HMENU>(IDC_TAB), hinst, nullptr);
    SendMessage(impl_->tab, WM_SETFONT,
        reinterpret_cast<WPARAM>(impl_->ui_font), FALSE);
    SendMessage(impl_->tab, TCM_SETPADDING, 0, MAKELPARAM(impl_->Dpi(16), impl_->Dpi(4)));
    {
        std::wstring wtab_info = i18n::tr_w(i18n::S::kTabInfo);
        std::wstring wtab_console = i18n::tr_w(i18n::S::kTabConsole);
        std::wstring wtab_display = i18n::tr_w(i18n::S::kTabDisplay);
        TCITEMW ti{};
        ti.mask = TCIF_TEXT;
        ti.pszText = wtab_info.data();
        SendMessageW(impl_->tab, TCM_INSERTITEMW, kTabInfo,
            reinterpret_cast<LPARAM>(&ti));
        ti.pszText = wtab_console.data();
        SendMessageW(impl_->tab, TCM_INSERTITEMW, kTabConsole,
            reinterpret_cast<LPARAM>(&ti));
        ti.pszText = wtab_display.data();
        SendMessageW(impl_->tab, TCM_INSERTITEMW, kTabDisplay,
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
                VmUiState& state = impl_->GetVmUiState(vm_id);
                if (active) {
                    state.current_tab = kTabDisplay;
                } else {
                    // Display deactivated (e.g. guest OS blanked the monitor).
                    // Drop cached framebuffer/cursor so we don't restore a
                    // stale frame when the user switches back to this VM.
                    state.fb_width = 0;
                    state.fb_height = 0;
                    state.framebuffer.clear();
                    state.cursor = CursorInfo{};
                    state.cursor_pixels.clear();
                }

                bool is_current = (impl_->selected_index >= 0 &&
                    impl_->selected_index < static_cast<int>(impl_->records.size()) &&
                    impl_->records[impl_->selected_index].spec.vm_id == vm_id);
                if (!is_current) return;

                if (active) {
                    impl_->display_available = true;
                    float factor = impl_->records[impl_->selected_index].spec.dpi_scaled
                        ? (static_cast<float>(impl_->dpi) / 96.0f) : 1.0f;
                    if (impl_->display_panel) impl_->display_panel->SetDpiZoomFactor(factor);
                    SendMessage(impl_->tab, TCM_SETCURSEL, kTabDisplay, 0);
                    ResizeWindowForDisplay(impl_.get(), width, height);
                } else {
                    impl_->display_available = false;
                    // Clear the on-screen framebuffer so the panel goes black
                    // instead of freezing on the last frame. Keep the window
                    // size and last_sent_display_w/h so re-activation with
                    // the same resolution doesn't cause any window reshuffle.
                    if (impl_->display_panel) impl_->display_panel->Clear();
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

            UpdateSleepPrevention();
        });
    });

    manager_.SetGuestAgentStateCallback(
        [this](const std::string& vm_id, bool connected) {
            (void)connected;
            InvokeOnUiThread([this, vm_id]() {
                RefreshVmList();
            });
        });

    manager_.SetPortForwardErrorCallback(
        [this](const std::string& vm_id, const std::vector<std::string>& failed_mappings) {
            (void)vm_id;
            InvokeOnUiThread([this, failed_mappings]() {
                std::string ports_str;
                for (const auto& mapping : failed_mappings) {
                    ports_str += "  - " + mapping + "\n";
                }
                std::string msg = i18n::fmt(i18n::S::kPfBindErrorMsg, ports_str.c_str());
                MessageBoxW(impl_->hwnd, i18n::to_wide(msg).c_str(),
                    i18n::tr_w(i18n::S::kPfBindErrorTitle).c_str(), MB_OK | MB_ICONWARNING);
            });
        });

    RefreshVmList();
    LayoutControls(impl_.get());
}

Win32UiShell::~Win32UiShell() {
    // Clear all callbacks so background threads won't invoke into a
    // partially-destroyed UI shell.
    manager_.SetConsoleCallback(nullptr);
    manager_.SetStateChangeCallback(nullptr);
    manager_.SetDisplayCallback(nullptr);
    manager_.SetCursorCallback(nullptr);
    manager_.SetDisplayStateCallback(nullptr);
    manager_.SetClipboardGrabCallback(nullptr);
    manager_.SetClipboardDataCallback(nullptr);
    manager_.SetClipboardRequestCallback(nullptr);
    manager_.SetAudioPcmCallback(nullptr);
    manager_.SetGuestAgentStateCallback(nullptr);
    manager_.SetPortForwardErrorCallback(nullptr);

    // Stop all VMs and join their read threads so no background thread
    // can still be executing a previously-copied callback closure that
    // captured our `this`.
    manager_.ShutdownAll();

    // Drain pending invoke queue to avoid calling stale closures.
    {
        std::lock_guard<std::mutex> lock(g_invoke_mutex);
        g_invoke_queue.clear();
    }

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
    if (sleep_prevented_) {
        SetThreadExecutionState(ES_CONTINUOUS);
        sleep_prevented_ = false;
    }
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
        impl_->selected_index = impl_->records.empty() ? -1 :
            static_cast<int>(impl_->records.size()) - 1;
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
    SendMessageW(impl_->statusbar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(i18n::to_wide(status).c_str()));
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
    if (value) {
        g_clipboard_suppress_until = GetTickCount64() + kClipboardSuppressMs;
    } else {
        g_clipboard_suppress_until = 0;
    }
}

void Win32UiShell::UpdateSleepPrevention() {
    bool has_running = false;
    for (const auto& rec : impl_->records) {
        if (rec.state == VmPowerState::kRunning ||
            rec.state == VmPowerState::kStarting) {
            has_running = true;
            break;
        }
    }

    if (has_running && !sleep_prevented_) {
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
        sleep_prevented_ = true;
    } else if (!has_running && sleep_prevented_) {
        SetThreadExecutionState(ES_CONTINUOUS);
        sleep_prevented_ = false;
    }
}
