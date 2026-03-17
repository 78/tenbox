#include "manager/ui/port_forward_dialog.h"
#include "manager/manager_service.h"
#include "manager/i18n.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

#include <shellapi.h>

#include <array>
#include <cstdio>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace {

class DlgBuilder {
public:
    void Begin(const char* title, int x, int y, int cx, int cy, DWORD style) {
        buf_.clear();
        Align(4);
        DLGTEMPLATE dt{};
        dt.style = style | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT | DS_MODALFRAME;
        dt.x = static_cast<short>(x);
        dt.y = static_cast<short>(y);
        dt.cx = static_cast<short>(cx);
        dt.cy = static_cast<short>(cy);
        Append(&dt, sizeof(dt));
        AppendWord(0);
        AppendWord(0);
        AppendWideStr(title);
        AppendWord(9);
        AppendWideStr("Segoe UI");
        count_offset_ = offsetof(DLGTEMPLATE, cdit);
    }

    void AddStatic(int id, const char* text, int x, int y, int cx, int cy) {
        AddItem(id, 0x0082, text, x, y, cx, cy,
            WS_CHILD | WS_VISIBLE | SS_LEFT);
    }

    void AddEdit(int id, int x, int y, int cx, int cy, DWORD extra = 0) {
        AddItem(id, 0x0081, "", x, y, cx, cy,
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | extra);
    }

    void AddButton(int id, const char* text, int x, int y, int cx, int cy, DWORD style = 0) {
        AddItem(id, 0x0080, text, x, y, cx, cy,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | style);
    }

    void AddCheckBox(int id, const char* text, int x, int y, int cx, int cy) {
        AddItem(id, 0x0080, text, x, y, cx, cy,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX);
    }

    void AddDefButton(int id, const char* text, int x, int y, int cx, int cy) {
        AddButton(id, text, x, y, cx, cy, BS_DEFPUSHBUTTON);
    }

    LPCDLGTEMPLATE Build() {
        auto* dt = reinterpret_cast<DLGTEMPLATE*>(buf_.data());
        dt->cdit = static_cast<WORD>(item_count_);
        return reinterpret_cast<LPCDLGTEMPLATE>(buf_.data());
    }

private:
    std::vector<BYTE> buf_;
    int item_count_ = 0;
    size_t count_offset_ = 0;

    void Append(const void* data, size_t len) {
        auto* p = static_cast<const BYTE*>(data);
        buf_.insert(buf_.end(), p, p + len);
    }

    void AppendWord(WORD w) { Append(&w, 2); }

    void AppendWideStr(const char* s) {
        if (!s || !*s) {
            AppendWord(0);
            return;
        }
        int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        if (len <= 0) {
            AppendWord(0);
            return;
        }
        std::vector<wchar_t> wstr(len);
        MultiByteToWideChar(CP_UTF8, 0, s, -1, wstr.data(), len);
        for (int i = 0; i < len; ++i) {
            AppendWord(static_cast<WORD>(wstr[i]));
        }
    }

    void Align(size_t a) {
        while (buf_.size() % a) buf_.push_back(0);
    }

    void AddItem(int id, WORD cls, const char* text,
                 int x, int y, int cx, int cy, DWORD style) {
        Align(4);
        DLGITEMTEMPLATE dit{};
        dit.style = style;
        dit.x  = static_cast<short>(x);
        dit.y  = static_cast<short>(y);
        dit.cx = static_cast<short>(cx);
        dit.cy = static_cast<short>(cy);
        dit.id = static_cast<WORD>(id);
        Append(&dit, sizeof(dit));
        AppendWord(0xFFFF);
        AppendWord(cls);
        AppendWideStr(text);
        AppendWord(0);
        ++item_count_;
    }
};

static void CenterDialogToParent(HWND dlg) {
    HWND parent = GetParent(dlg);
    if (!parent) parent = GetWindow(dlg, GW_OWNER);
    if (!parent) return;
    RECT pr, dr;
    GetWindowRect(parent, &pr);
    GetWindowRect(dlg, &dr);
    int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    int x = pr.left + ((pr.right - pr.left) - dw) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - dh) / 2;
    SetWindowPos(dlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

enum PfDlgId {
    IDC_PF_LIST   = 300,
    IDC_PF_ADD    = 301,
    IDC_PF_REMOVE = 302,
    IDC_PF_OPEN   = 303,
    IDC_GF_LIST   = 311,
    IDC_GF_ADD    = 312,
    IDC_GF_REMOVE = 313,
};

struct PfDlgData {
    ManagerService* mgr;
    std::string vm_id;
    HWND listview;
    HWND gf_listview;
};

static void PfUpdateButtons(HWND dlg, HWND listview) {
    BOOL has_sel = ListView_GetNextItem(listview, -1, LVNI_SELECTED) >= 0;
    EnableWindow(GetDlgItem(dlg, IDC_PF_REMOVE), has_sel);
    EnableWindow(GetDlgItem(dlg, IDC_PF_OPEN), has_sel);
}

static void PfRefreshList(PfDlgData* data) {
    HWND lv = data->listview;
    ListView_DeleteAllItems(lv);

    auto forwards = data->mgr->GetPortForwards(data->vm_id);
    for (size_t i = 0; i < forwards.size(); ++i) {
        const auto& pf = forwards[i];
        std::string host_part = pf.EffectiveHostIp() + ":" + std::to_string(pf.host_port);
        std::string guest_part = (pf.guest_ip.empty() ? std::string("10.0.2.15") : pf.guest_ip) +
                                 ":" + std::to_string(pf.guest_port);
        std::wstring text = i18n::to_wide(host_part) + L"  \u2192  " + i18n::to_wide(guest_part);
        wchar_t buf[200];
        wcsncpy_s(buf, text.c_str(), _TRUNCATE);

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = buf;
        SendMessageW(lv, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
    }
}

static void GfRefreshList(PfDlgData* data) {
    HWND lv = data->gf_listview;
    ListView_DeleteAllItems(lv);

    auto forwards = data->mgr->GetGuestForwards(data->vm_id);
    for (size_t i = 0; i < forwards.size(); ++i) {
        const auto& gf = forwards[i];
        std::string guest = GuestForward::Ip4ToString(gf.guest_ip) + ":" + std::to_string(gf.guest_port);
        std::string host = gf.EffectiveHostAddr() + ":" + std::to_string(gf.host_port);
        std::wstring text = i18n::to_wide(guest) + L"  \u2192  " + i18n::to_wide(host);

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = text.data();
        SendMessageW(lv, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
    }
}

static void GfUpdateButtons(HWND dlg, HWND listview) {
    BOOL has_sel = ListView_GetNextItem(listview, -1, LVNI_SELECTED) >= 0;
    EnableWindow(GetDlgItem(dlg, IDC_GF_REMOVE), has_sel);
}

enum AddGfDlgId {
    IDC_AGF_GUEST_IP   = 500,
    IDC_AGF_GUEST_PORT = 501,
    IDC_AGF_HOST_ADDR  = 502,
    IDC_AGF_HOST_PORT  = 503,
};

struct AddGfDlgData {
    ManagerService* mgr;
    std::string vm_id;
    bool added;
};

static INT_PTR CALLBACK AddGfDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<AddGfDlgData*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG:
        data = reinterpret_cast<AddGfDlgData*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(data));
        CenterDialogToParent(dlg);
        SetDlgItemTextW(dlg, IDC_AGF_GUEST_IP, L"10.0.2.2");
        SetDlgItemTextW(dlg, IDC_AGF_HOST_ADDR, L"127.0.0.1");
        SetFocus(GetDlgItem(dlg, IDC_AGF_GUEST_PORT));
        return FALSE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            wchar_t gip_buf[64], gport_buf[16], haddr_buf[64], hport_buf[16];
            GetDlgItemTextW(dlg, IDC_AGF_GUEST_IP, gip_buf, 64);
            GetDlgItemTextW(dlg, IDC_AGF_GUEST_PORT, gport_buf, 16);
            GetDlgItemTextW(dlg, IDC_AGF_HOST_ADDR, haddr_buf, 64);
            GetDlgItemTextW(dlg, IDC_AGF_HOST_PORT, hport_buf, 16);

            std::string gip_str = i18n::wide_to_utf8(gip_buf);
            int gport = _wtoi(gport_buf);
            std::string haddr_str = i18n::wide_to_utf8(haddr_buf);
            int hport = _wtoi(hport_buf);

            uint32_t guest_ip = 0;
            if (!GuestForward::Ip4FromString(gip_str, guest_ip) ||
                gport <= 0 || gport > 65535 || hport <= 0 || hport > 65535 ||
                haddr_str.empty()) {
                MessageBoxW(dlg, i18n::tr_w(i18n::S::kGfInvalidParams).c_str(),
                    i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONWARNING);
                return TRUE;
            }

            GuestForward gf;
            gf.guest_ip = guest_ip;
            gf.guest_port = static_cast<uint16_t>(gport);
            gf.host_addr = haddr_str;
            gf.host_port = static_cast<uint16_t>(hport);

            std::string error;
            if (data->mgr->AddGuestForward(data->vm_id, gf, &error)) {
                data->added = true;
                EndDialog(dlg, IDOK);
            } else {
                MessageBoxW(dlg, i18n::to_wide(error).c_str(),
                    i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONERROR);
            }
            return TRUE;
        }
        }
        break;

    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

static bool ShowAddGuestForwardDialog(HWND parent, ManagerService& mgr, const std::string& vm_id) {
    using S = i18n::S;
    DlgBuilder b;
    int W = 220, H = 120;
    b.Begin(i18n::tr(S::kGfTitle), 0, 0, W, H,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT);

    int lx = 8, lw = 70, ex = 82, ew = 80, y = 10, rh = 14, sp = 18;

    b.AddStatic(0, i18n::tr(S::kGfLabelGuestIp), lx, y, lw, rh);
    b.AddEdit(IDC_AGF_GUEST_IP, ex, y - 2, ew, rh);
    y += sp;

    b.AddStatic(0, i18n::tr(S::kGfLabelGuestPort), lx, y, lw, rh);
    b.AddEdit(IDC_AGF_GUEST_PORT, ex, y - 2, ew, rh, ES_NUMBER);
    y += sp;

    b.AddStatic(0, i18n::tr(S::kGfLabelHostAddr), lx, y, lw, rh);
    b.AddEdit(IDC_AGF_HOST_ADDR, ex, y - 2, ew, rh);
    y += sp;

    b.AddStatic(0, i18n::tr(S::kGfLabelHostPort), lx, y, lw, rh);
    b.AddEdit(IDC_AGF_HOST_PORT, ex, y - 2, ew, rh, ES_NUMBER);
    y += sp + 4;

    int btn_w = 50, btn_h = 14;
    int btn_x = W - btn_w - 8;
    b.AddDefButton(IDOK, i18n::tr(S::kDlgBtnSave), btn_x, y, btn_w, btn_h);

    AddGfDlgData data{&mgr, vm_id, false};
    DialogBoxIndirectParamW(GetModuleHandle(nullptr), b.Build(), parent,
        AddGfDlgProc, reinterpret_cast<LPARAM>(&data));
    return data.added;
}

enum AddPfDlgId {
    IDC_APF_HOST_IP    = 400,
    IDC_APF_HOST_PORT  = 401,
    IDC_APF_GUEST_IP   = 402,
    IDC_APF_GUEST_PORT = 403,
};

struct AddPfDlgData {
    ManagerService* mgr;
    std::string vm_id;
    bool added;
};

static INT_PTR CALLBACK AddPfDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<AddPfDlgData*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG:
        data = reinterpret_cast<AddPfDlgData*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(data));
        CenterDialogToParent(dlg);
        SetDlgItemTextW(dlg, IDC_APF_HOST_IP, L"127.0.0.1");
        SetDlgItemTextW(dlg, IDC_APF_GUEST_IP, L"10.0.2.15");
        EnableWindow(GetDlgItem(dlg, IDC_APF_GUEST_IP), FALSE);
        SetFocus(GetDlgItem(dlg, IDC_APF_HOST_PORT));
        return FALSE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            wchar_t hip_buf[64], hport_buf[16], gip_buf[64], gport_buf[16];
            GetDlgItemTextW(dlg, IDC_APF_HOST_IP, hip_buf, 64);
            GetDlgItemTextW(dlg, IDC_APF_HOST_PORT, hport_buf, 16);
            GetDlgItemTextW(dlg, IDC_APF_GUEST_IP, gip_buf, 64);
            GetDlgItemTextW(dlg, IDC_APF_GUEST_PORT, gport_buf, 16);

            std::string hip_str = i18n::wide_to_utf8(hip_buf);
            int host_port = _wtoi(hport_buf);
            std::string gip_str = i18n::wide_to_utf8(gip_buf);
            int guest_port = _wtoi(gport_buf);

            if (host_port <= 0 || host_port > 65535 || guest_port <= 0 || guest_port > 65535 ||
                hip_str.empty()) {
                MessageBoxW(dlg, i18n::tr_w(i18n::S::kPfInvalidPort).c_str(),
                    i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONWARNING);
                return TRUE;
            }

            PortForward pf;
            pf.host_ip = hip_str;
            pf.host_port = static_cast<uint16_t>(host_port);
            pf.guest_ip = gip_str;
            pf.guest_port = static_cast<uint16_t>(guest_port);

            std::string error;
            if (data->mgr->AddPortForward(data->vm_id, pf, &error)) {
                data->added = true;
                EndDialog(dlg, IDOK);
            } else {
                MessageBoxW(dlg, i18n::to_wide(error).c_str(),
                    i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONERROR);
            }
            return TRUE;
        }
        }
        break;

    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

static bool ShowAddPortForwardDialog(HWND parent, ManagerService& mgr, const std::string& vm_id) {
    using S = i18n::S;
    DlgBuilder b;
    int W = 220, H = 120;
    b.Begin(i18n::tr(S::kPfColHostPort), 0, 0, W, H,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT);

    int lx = 8, lw = 70, ex = 82, ew = 80, y = 10, rh = 14, sp = 18;

    b.AddStatic(0, i18n::tr(S::kPfLabelHostIp), lx, y, lw, rh);
    b.AddEdit(IDC_APF_HOST_IP, ex, y - 2, ew, rh);
    y += sp;

    b.AddStatic(0, i18n::tr(S::kPfLabelHostPort), lx, y, lw, rh);
    b.AddEdit(IDC_APF_HOST_PORT, ex, y - 2, ew, rh, ES_NUMBER);
    y += sp;

    b.AddStatic(0, i18n::tr(S::kPfLabelGuestIp), lx, y, lw, rh);
    b.AddEdit(IDC_APF_GUEST_IP, ex, y - 2, ew, rh);
    y += sp;

    b.AddStatic(0, i18n::tr(S::kPfLabelGuestPort), lx, y, lw, rh);
    b.AddEdit(IDC_APF_GUEST_PORT, ex, y - 2, ew, rh, ES_NUMBER);
    y += sp + 4;

    int btn_w = 50, btn_h = 14;
    int btn_x = W - btn_w - 8;
    b.AddDefButton(IDOK, i18n::tr(S::kDlgBtnSave), btn_x, y, btn_w, btn_h);

    AddPfDlgData data{&mgr, vm_id, false};
    DialogBoxIndirectParamW(GetModuleHandle(nullptr), b.Build(), parent,
        AddPfDlgProc, reinterpret_cast<LPARAM>(&data));
    return data.added;
}

static INT_PTR CALLBACK PfDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<PfDlgData*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<PfDlgData*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(data));
        CenterDialogToParent(dlg);

        RECT rc;
        GetClientRect(dlg, &rc);
        RECT du = {0, 0, 48, 14};
        MapDialogRect(dlg, &du);
        int btn_w = du.right, btn_h = du.bottom;
        int gap = btn_h / 2, btn_gap = btn_h / 4;
        int list_w = rc.right - btn_w - gap * 3;

        int total_h = rc.bottom - gap * 3;
        int pf_list_h = total_h / 2;
        int gf_list_h = total_h - pf_list_h;

        // Port forwards listview (Host -> Guest)
        HWND lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            gap, gap, list_w, pf_list_h,
            dlg, reinterpret_cast<HMENU>(IDC_PF_LIST),
            GetModuleHandle(nullptr), nullptr);
        ListView_SetExtendedListViewStyle(lv,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        std::wstring pf_col_title = i18n::tr_w(i18n::S::kPfColHostPort);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = list_w - 4;
        col.pszText = pf_col_title.data();
        SendMessageW(lv, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&col));

        data->listview = lv;

        int btn_x = gap + list_w + gap;
        MoveWindow(GetDlgItem(dlg, IDC_PF_ADD), btn_x, gap, btn_w, btn_h, FALSE);
        MoveWindow(GetDlgItem(dlg, IDC_PF_REMOVE), btn_x, gap + btn_h + btn_gap, btn_w, btn_h, FALSE);
        MoveWindow(GetDlgItem(dlg, IDC_PF_OPEN), btn_x, gap + (btn_h + btn_gap) * 2, btn_w, btn_h, FALSE);

        // Guest forwards listview (Guest -> Host)
        int gf_top = gap + pf_list_h + gap;

        HWND gf_lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            gap, gf_top, list_w, gf_list_h,
            dlg, reinterpret_cast<HMENU>(IDC_GF_LIST),
            GetModuleHandle(nullptr), nullptr);
        ListView_SetExtendedListViewStyle(gf_lv,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        std::wstring gf_col_title = i18n::tr_w(i18n::S::kGfTitle);
        LVCOLUMNW gf_col{};
        gf_col.mask = LVCF_TEXT | LVCF_WIDTH;
        gf_col.cx = list_w - 4;
        gf_col.pszText = gf_col_title.data();
        SendMessageW(gf_lv, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&gf_col));

        data->gf_listview = gf_lv;

        MoveWindow(GetDlgItem(dlg, IDC_GF_ADD), btn_x, gf_top, btn_w, btn_h, FALSE);
        MoveWindow(GetDlgItem(dlg, IDC_GF_REMOVE), btn_x, gf_top + btn_h + btn_gap, btn_w, btn_h, FALSE);

        PfRefreshList(data);
        PfUpdateButtons(dlg, lv);
        GfRefreshList(data);
        GfUpdateButtons(dlg, gf_lv);
        return TRUE;
    }

    case WM_NOTIFY: {
        auto* nmhdr = reinterpret_cast<NMHDR*>(lp);
        if (nmhdr->idFrom == IDC_PF_LIST && nmhdr->code == LVN_ITEMCHANGED) {
            PfUpdateButtons(dlg, data->listview);
        }
        if (nmhdr->idFrom == IDC_GF_LIST && nmhdr->code == LVN_ITEMCHANGED) {
            GfUpdateButtons(dlg, data->gf_listview);
        }
        return FALSE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PF_ADD:
            if (ShowAddPortForwardDialog(dlg, *data->mgr, data->vm_id)) {
                PfRefreshList(data);
                PfUpdateButtons(dlg, data->listview);
            }
            return TRUE;

        case IDC_PF_OPEN: {
            int sel = ListView_GetNextItem(data->listview, -1, LVNI_SELECTED);
            if (sel >= 0) {
                auto forwards = data->mgr->GetPortForwards(data->vm_id);
                if (sel < static_cast<int>(forwards.size())) {
                    std::string url_str = "http://" + forwards[sel].EffectiveHostIp() +
                                         ":" + std::to_string(forwards[sel].host_port) +
                                         "/#token=tenbox";
                    std::wstring url = i18n::to_wide(url_str);
                    ShellExecuteW(dlg, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            return TRUE;
        }

        case IDC_PF_REMOVE: {
            int sel = ListView_GetNextItem(data->listview, -1, LVNI_SELECTED);
            if (sel < 0) {
                MessageBoxW(dlg, i18n::tr_w(i18n::S::kPfNoSelection).c_str(),
                    i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONWARNING);
                return TRUE;
            }

            auto forwards = data->mgr->GetPortForwards(data->vm_id);
            if (sel >= static_cast<int>(forwards.size()))
                return TRUE;

            uint16_t host_port = forwards[sel].host_port;
            uint16_t guest_port = forwards[sel].guest_port;

            std::string prompt = i18n::fmt(i18n::S::kPfConfirmRemoveMsg, host_port, guest_port);

            if (MessageBoxW(dlg, i18n::to_wide(prompt).c_str(),
                    i18n::tr_w(i18n::S::kPfConfirmRemoveTitle).c_str(),
                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
                std::string error;
                if (data->mgr->RemovePortForward(data->vm_id, host_port, &error)) {
                    PfRefreshList(data);
                    PfUpdateButtons(dlg, data->listview);
                } else {
                    MessageBoxW(dlg, i18n::to_wide(error).c_str(),
                        i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONERROR);
                }
            }
            return TRUE;
        }

        case IDC_GF_ADD:
            if (ShowAddGuestForwardDialog(dlg, *data->mgr, data->vm_id)) {
                GfRefreshList(data);
                GfUpdateButtons(dlg, data->gf_listview);
            }
            return TRUE;

        case IDC_GF_REMOVE: {
            int sel = ListView_GetNextItem(data->gf_listview, -1, LVNI_SELECTED);
            if (sel < 0) return TRUE;

            auto forwards = data->mgr->GetGuestForwards(data->vm_id);
            if (sel >= static_cast<int>(forwards.size())) return TRUE;

            std::string error;
            if (data->mgr->RemoveGuestForward(data->vm_id,
                    forwards[sel].guest_ip, forwards[sel].guest_port, &error)) {
                GfRefreshList(data);
                GfUpdateButtons(dlg, data->gf_listview);
            } else {
                MessageBoxW(dlg, i18n::to_wide(error).c_str(),
                    i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONERROR);
            }
            return TRUE;
        }
        }
        break;

    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

}  // namespace

void ShowPortForwardsDialog(HWND parent, ManagerService& mgr, const std::string& vm_id) {
    using S = i18n::S;
    DlgBuilder b;
    int W = 300, H = 210;
    b.Begin(i18n::tr(S::kDlgPortForwards), 0, 0, W, H,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT);

    int btn_h = 14, btn_w = 56;
    b.AddButton(IDC_PF_ADD, i18n::tr(S::kPfBtnAdd), 0, 0, btn_w, btn_h);
    b.AddButton(IDC_PF_REMOVE, i18n::tr(S::kPfBtnRemove), 0, 0, btn_w, btn_h);
    b.AddButton(IDC_PF_OPEN, i18n::tr(S::kPfBtnOpen), 0, 0, btn_w, btn_h);
    b.AddButton(IDC_GF_ADD, i18n::tr(S::kGfBtnAdd), 0, 0, btn_w, btn_h);
    b.AddButton(IDC_GF_REMOVE, i18n::tr(S::kGfBtnRemove), 0, 0, btn_w, btn_h);

    PfDlgData data{&mgr, vm_id, nullptr, nullptr};
    DialogBoxIndirectParamW(GetModuleHandle(nullptr), b.Build(), parent,
        PfDlgProc, reinterpret_cast<LPARAM>(&data));
}
