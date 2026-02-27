#include "ui/win32/win32_ui_shell.h"
#include "ui/win32/win32_dialogs.h"
#include "ui/win32/win32_display_panel.h"
#include "manager/app_settings.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' " \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
    "language='*'\"")

#include <algorithm>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
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
};

// WM_APP range for cross-thread invoke
static constexpr UINT WM_INVOKE = WM_APP + 100;

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
    switch (s) {
    case VmPowerState::kRunning:  return "Running";
    case VmPowerState::kStarting: return "Starting";
    case VmPowerState::kStopping: return "Stopping";
    case VmPowerState::kCrashed:  return "Crashed";
    default:                      return "Stopped";
    }
}

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
    HMENU menu_bar  = nullptr;

    bool display_available = false;

    static constexpr int kDetailRows = 7;
    HWND detail_labels[kDetailRows] = {};
    HWND detail_values[kDetailRows] = {};

    HFONT ui_font     = nullptr;
    HFONT mono_font   = nullptr;

    std::unique_ptr<DisplayPanel> display_panel;

    std::vector<VmRecord> records;
    int selected_index = -1;
    std::string console_text;

    enum AnsiState { kNormal, kEsc, kCsi };
    AnsiState ansi_state = kNormal;

    void ResetConsole() {
        console_text.clear();
        ansi_state = kNormal;
    }

    std::string AppendConsoleData(const std::string& raw) {
        std::string added;
        for (unsigned char ch : raw) {
            switch (ansi_state) {
            case kNormal:
                if (ch == 0x1b) { ansi_state = kEsc; break; }
                if (ch == '\n') { console_text += "\r\n"; added += "\r\n"; break; }
                if (ch == '\t') { console_text.push_back('\t'); added.push_back('\t'); break; }
                if (ch >= 0x20 && ch <= 0x7e) { console_text.push_back(ch); added.push_back(ch); break; }
                break;
            case kEsc:
                ansi_state = (ch == '[') ? kCsi : kNormal;
                break;
            case kCsi:
                if (ch >= 0x40 && ch <= 0x7e) ansi_state = kNormal;
                break;
            }
        }
        if (console_text.size() > kMaxConsoleLen) {
            console_text.erase(0, console_text.size() - kMaxConsoleLen);
        }
        return added;
    }
};

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
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    return RegisterClassExA(&wc);
}

// ── Menu building ──

static HMENU BuildMenuBar() {
    HMENU bar = CreateMenu();

    HMENU file_menu = CreatePopupMenu();
    AppendMenuA(file_menu, MF_STRING, IDM_NEW_VM, "New VM\tCtrl+N");
    AppendMenuA(file_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(file_menu, MF_STRING, IDM_EXIT, "Exit\tAlt+F4");
    AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file_menu), "Manager");

    HMENU vm_menu = CreatePopupMenu();
    AppendMenuA(vm_menu, MF_STRING, IDM_EDIT,     "Edit...");
    AppendMenuA(vm_menu, MF_STRING, IDM_DELETE,   "Delete");
    AppendMenuA(vm_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(vm_menu, MF_STRING, IDM_START,    "Start");
    AppendMenuA(vm_menu, MF_STRING, IDM_STOP,     "Stop");
    AppendMenuA(vm_menu, MF_STRING, IDM_REBOOT,   "Reboot");
    AppendMenuA(vm_menu, MF_STRING, IDM_SHUTDOWN,  "Shutdown");
    AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(vm_menu), "VM");

    return bar;
}

// ── Toolbar building ──

static HWND CreateToolbar(HWND parent, HINSTANCE hinst) {
    HWND tb = CreateWindowExA(0, TOOLBARCLASSNAMEA, nullptr,
        WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | CCS_TOP,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_TOOLBAR), hinst, nullptr);

    SendMessage(tb, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessage(tb, TB_SETPADDING, 0, MAKELPARAM(16, 6));

    static const struct { UINT id; const char* text; } items[] = {
        {IDM_NEW_VM,   "New VM"},
        {IDM_EDIT,     "Edit"},
        {IDM_DELETE,   "Delete"},
        {0,            nullptr},
        {IDM_START,    "Start"},
        {IDM_STOP,     "Stop"},
        {IDM_REBOOT,   "Reboot"},
        {IDM_SHUTDOWN, "Shutdown"},
    };

    for (const auto& item : items) {
        TBBUTTON btn{};
        if (item.id == 0) {
            btn.fsStyle = BTNS_SEP;
        } else {
            btn.iBitmap   = I_IMAGENONE;
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
    HWND lb = CreateWindowExA(0, "LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
        LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(IDC_LISTVIEW), hinst, nullptr);
    SendMessageA(lb, LB_SETITEMHEIGHT, 0, MAKELPARAM(kListItemHeight, 0));
    return lb;
}

// ── Layout helper ──

using Impl = Win32UiShell::Impl;

static constexpr int kTabInfo    = 0;
static constexpr int kTabConsole = 1;
static constexpr int kTabDisplay = 2;

static void ShowDetailControls(Impl* p, bool visible) {
    int cmd = visible ? SW_SHOW : SW_HIDE;
    for (int i = 0; i < Impl::kDetailRows; ++i) {
        ShowWindow(p->detail_labels[i], cmd);
        ShowWindow(p->detail_values[i], cmd);
    }
}

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

    // Hide everything first, then show the active page
    ShowDetailControls(p, false);
    ShowWindow(p->console, SW_HIDE);
    ShowWindow(p->console_in, SW_HIDE);
    ShowWindow(p->send_btn, SW_HIDE);
    if (p->display_panel) p->display_panel->SetVisible(false);

    if (cur_tab == kTabInfo) {
        ShowDetailControls(p, true);

        HDC hdc = GetDC(p->hwnd);
        HFONT old_font = static_cast<HFONT>(SelectObject(hdc, p->ui_font));
        TEXTMETRICA tm{};
        GetTextMetricsA(hdc, &tm);

        static const char* kLabels[] = {
            "ID:", "Location:", "Kernel:", "Disk:", "Memory:", "vCPUs:", "NAT:"
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
        }
    }
}

// ── Update detail panel from selected VM ──

static void UpdateDetailPanel(Impl* p) {
    static const char* labels[] = {
        "ID:", "Location:", "Kernel:", "Disk:", "Memory:", "vCPUs:", "NAT:"
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
        spec.disk_path.empty() ? "(none)" : spec.disk_path.c_str());
    SetWindowTextA(p->detail_values[4], mb_str.c_str());
    SetWindowTextA(p->detail_values[5], cpu_str.c_str());
    SetWindowTextA(p->detail_values[6], spec.nat_enabled ? "Enabled" : "Disabled");
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

static Win32UiShell* g_shell = nullptr;

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* shell = g_shell;
    if (!shell) return DefWindowProcA(hwnd, msg, wp, lp);
    auto* p = reinterpret_cast<Win32UiShell::Impl*>(
        GetWindowLongPtrA(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_SIZE:
        if (p) LayoutControls(p);
        return 0;

    case WM_COMMAND: {
        UINT cmd = LOWORD(wp);
        UINT code = HIWORD(wp);

        // Send button or Enter in console input
        if ((cmd == IDC_SEND_BTN && code == BN_CLICKED) ||
            (cmd == IDC_CONSOLE_IN && code == EN_CHANGE && false)) {
            // handled below via accelerator
        }

        // ListBox selection change
        if (cmd == IDC_LISTVIEW && code == LBN_SELCHANGE) {
            int sel = static_cast<int>(SendMessageA(p->listview, LB_GETCURSEL, 0, 0));
            if (sel != LB_ERR && sel != p->selected_index) {
                p->selected_index = sel;
                p->ResetConsole();
                SetWindowTextA(p->console, "");
                p->display_available = false;
                UpdateDetailPanel(p);
                UpdateCommandStates(p);

                bool running = IsVmRunning(p->records[sel].state);
                SendMessage(p->tab, TCM_SETCURSEL,
                    running ? kTabConsole : kTabInfo, 0);
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
                MessageBoxA(hwnd, error.c_str(), "Error", MB_OK | MB_ICONERROR);
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
            p->ResetConsole();
            SetWindowTextA(p->console, "");
            p->display_available = false;
            SendMessage(p->tab, TCM_SETCURSEL, kTabConsole, 0);
            LayoutControls(p);
            SendMessageA(p->statusbar, SB_SETTEXTA, 0,
                reinterpret_cast<LPARAM>(("Starting " + vm_id + "...").c_str()));
            std::string error;
            bool ok = shell->manager_.StartVm(vm_id, &error);
            shell->RefreshVmList();
            auto msg = ok ? (vm_id + " started") : ("Error: " + error);
            SendMessageA(p->statusbar, SB_SETTEXTA, 0,
                reinterpret_cast<LPARAM>(msg.c_str()));
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
            auto msg = ok ? (vm_id + " stopped") : ("Error: " + error);
            SendMessageA(p->statusbar, SB_SETTEXTA, 0,
                reinterpret_cast<LPARAM>(msg.c_str()));
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
            auto msg = ok ? (vm_id + " rebooted") : ("Error: " + error);
            SendMessageA(p->statusbar, SB_SETTEXTA, 0,
                reinterpret_cast<LPARAM>(msg.c_str()));
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
            auto msg = ok ? (vm_id + " shutting down...") : ("Error: " + error);
            SendMessageA(p->statusbar, SB_SETTEXTA, 0,
                reinterpret_cast<LPARAM>(msg.c_str()));
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
                SendMessageA(p->statusbar, SB_SETTEXTA, 0,
                    reinterpret_cast<LPARAM>((vm_name + " updated").c_str()));
            } else if (!error.empty()) {
                MessageBoxA(hwnd, error.c_str(), "Error", MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        case IDM_DELETE: {
            if (p->selected_index < 0 ||
                p->selected_index >= static_cast<int>(p->records.size()))
                break;
            std::string vm_id   = p->records[p->selected_index].spec.vm_id;
            std::string vm_name = p->records[p->selected_index].spec.name;
            std::string prompt = "Are you sure you want to delete '" +
                vm_name + "'?\nThis will remove all VM files permanently.";
            if (MessageBoxA(hwnd, prompt.c_str(), "Delete VM",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
                std::string error;
                if (shell->manager_.DeleteVm(vm_id, &error)) {
                    shell->RefreshVmList();
                    SendMessageA(p->statusbar, SB_SETTEXTA, 0,
                        reinterpret_cast<LPARAM>("VM deleted"));
                } else {
                    MessageBoxA(hwnd, error.c_str(), "Error", MB_OK | MB_ICONERROR);
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
            COLORREF card_bg = selected ? GetSysColor(COLOR_HIGHLIGHT)
                                        : RGB(248, 248, 248);
            COLORREF fg  = selected ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                                    : GetSysColor(COLOR_WINDOWTEXT);
            COLORREF dim = selected ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                                    : GetSysColor(COLOR_GRAYTEXT);

            // Fill the whole row with window background first, then draw the card inset
            HBRUSH wnd_br = GetSysColorBrush(COLOR_WINDOW);
            FillRect(dis->hDC, &dis->rcItem, wnd_br);

            RECT card = dis->rcItem;
            card.left   += 4;
            card.right  -= 4;
            card.top    += 3;
            card.bottom -= 3;

            HBRUSH card_br = CreateSolidBrush(card_bg);
            HPEN border_pen = CreatePen(PS_SOLID, 1,
                selected ? GetSysColor(COLOR_HIGHLIGHT) : RGB(232, 232, 232));
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
            TextOutA(dis->hDC, x, y, rec.spec.name.c_str(),
                     static_cast<int>(rec.spec.name.size()));

            // State tag to the right of the name
            SIZE name_sz{};
            GetTextExtentPoint32A(dis->hDC, rec.spec.name.c_str(),
                static_cast<int>(rec.spec.name.size()), &name_sz);

            const char* state_text = StateText(rec.state);
            COLORREF state_color;
            if (rec.state == VmPowerState::kRunning)
                state_color = selected ? fg : RGB(0, 128, 0);
            else if (rec.state == VmPowerState::kCrashed)
                state_color = selected ? fg : RGB(200, 0, 0);
            else
                state_color = dim;
            SetTextColor(dis->hDC, state_color);
            TextOutA(dis->hDC, x + name_sz.cx + 12, y, state_text,
                     static_cast<int>(strlen(state_text)));

            // Line 2: vCPU / Memory
            y += line_h + 2;
            SelectObject(dis->hDC, normal);
            SetTextColor(dis->hDC, dim);
            std::string detail = std::to_string(rec.spec.cpu_count) + " vCPU, " +
                                 std::to_string(rec.spec.memory_mb) + " MB RAM";
            TextOutA(dis->hDC, x, y, detail.c_str(),
                     static_cast<int>(detail.size()));

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

    case WM_CLOSE:
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

    impl_->menu_bar = BuildMenuBar();

    impl_->hwnd = CreateWindowExA(0, kWndClass, "TenBox Manager",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x, y, w, h,
        nullptr, impl_->menu_bar, hinst, nullptr);

    g_main_hwnd = impl_->hwnd;
    SetWindowLongPtrA(impl_->hwnd, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(impl_.get()));

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
    {
        TCITEMA ti{};
        ti.mask = TCIF_TEXT;
        ti.pszText = const_cast<char*>("Info");
        SendMessageA(impl_->tab, TCM_INSERTITEMA, kTabInfo,
            reinterpret_cast<LPARAM>(&ti));
        ti.pszText = const_cast<char*>("Console");
        SendMessageA(impl_->tab, TCM_INSERTITEMA, kTabConsole,
            reinterpret_cast<LPARAM>(&ti));
        ti.pszText = const_cast<char*>("Display");
        SendMessageA(impl_->tab, TCM_INSERTITEMA, kTabDisplay,
            reinterpret_cast<LPARAM>(&ti));
    }

    // Detail label/value statics for Info tab
    for (int i = 0; i < Impl::kDetailRows; ++i) {
        impl_->detail_labels[i] = CreateWindowExA(0, "STATIC", "",
            WS_CHILD | SS_RIGHT,
            0, 0, 0, 0, impl_->hwnd, nullptr, hinst, nullptr);
        impl_->detail_values[i] = CreateWindowExA(0, "STATIC", "",
            WS_CHILD | SS_LEFT | SS_PATHELLIPSIS,
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
    SendMessageA(impl_->console_in, EM_SETCUEBANNER, FALSE,
        reinterpret_cast<LPARAM>(L"Type command and press Enter\x2026"));

    // Send button
    impl_->send_btn = CreateWindowExA(0, "BUTTON", "Send",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, impl_->hwnd,
        reinterpret_cast<HMENU>(IDC_SEND_BTN), hinst, nullptr);
    SendMessage(impl_->send_btn, WM_SETFONT,
        reinterpret_cast<WPARAM>(impl_->ui_font), FALSE);

    // Subclass the console input to catch Enter key
    SetWindowSubclass(impl_->console_in, ConsoleInputSubclass, 0, 0);

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

    // Wire callbacks
    manager_.SetDisplayCallback(
        [this](const std::string& vm_id, const DisplayFrame& frame) {
            InvokeOnUiThread([this, vm_id, frame]() {
                if (impl_->selected_index < 0 ||
                    impl_->selected_index >= static_cast<int>(impl_->records.size()))
                    return;
                if (impl_->records[impl_->selected_index].spec.vm_id != vm_id)
                    return;
                impl_->display_panel->UpdateFrame(frame);
                if (!impl_->display_available) {
                    impl_->display_available = true;
                    SendMessage(impl_->tab, TCM_SETCURSEL, kTabDisplay, 0);
                    LayoutControls(impl_.get());
                }
            });
        });

    manager_.SetConsoleCallback([this](const std::string& vm_id,
                                       const std::string& data) {
        InvokeOnUiThread([this, vm_id, data]() {
            if (impl_->selected_index < 0 ||
                impl_->selected_index >= static_cast<int>(impl_->records.size()))
                return;
            if (impl_->records[impl_->selected_index].spec.vm_id != vm_id)
                return;

            std::string added = impl_->AppendConsoleData(data);
            if (!added.empty()) {
                // Trim the Edit control when it gets too large
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
            if (impl_->selected_index >= 0 &&
                impl_->selected_index < static_cast<int>(impl_->records.size()) &&
                impl_->records[impl_->selected_index].spec.vm_id == vm_id) {
                auto state = impl_->records[impl_->selected_index].state;
                if (state == VmPowerState::kStopped || state == VmPowerState::kCrashed) {
                    impl_->display_available = false;
                    SendMessage(impl_->tab, TCM_SETCURSEL, kTabInfo, 0);
                    LayoutControls(impl_.get());
                }
            }
        });
    });

    RefreshVmList();
    LayoutControls(impl_.get());
}

Win32UiShell::~Win32UiShell() {
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
        TranslateMessage(&msg);
        DispatchMessage(&msg);
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

    auto status = std::to_string(impl_->records.size()) + " VM(s) loaded";
    SendMessageA(impl_->statusbar, SB_SETTEXTA, 0,
        reinterpret_cast<LPARAM>(status.c_str()));
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
