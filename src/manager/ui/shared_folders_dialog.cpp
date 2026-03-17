#include "manager/ui/win32_dialogs.h"
#include "manager/ui/dlg_builder.h"
#include "manager/i18n.h"
#include "manager/manager_service.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

#include <shellapi.h>

#include <array>
#include <string>

enum SfDlgId {
    IDC_SF_LIST    = 300,
    IDC_SF_ADD     = 301,
    IDC_SF_REMOVE  = 302,
    IDC_SF_OPEN    = 303,
    IDC_SF_HINT    = 304,
};

struct SfDlgData {
    ManagerService* mgr;
    std::string vm_id;
    HWND listview;
};

static void SfUpdateButtons(HWND dlg, HWND listview) {
    BOOL has_sel = ListView_GetNextItem(listview, -1, LVNI_SELECTED) >= 0;
    EnableWindow(GetDlgItem(dlg, IDC_SF_REMOVE), has_sel);
    EnableWindow(GetDlgItem(dlg, IDC_SF_OPEN), has_sel);
}

static void SfRefreshList(SfDlgData* data) {
    HWND lv = data->listview;
    ListView_DeleteAllItems(lv);

    auto folders = data->mgr->GetSharedFolders(data->vm_id);
    for (size_t i = 0; i < folders.size(); ++i) {
        const auto& sf = folders[i];
        std::wstring tag_w = i18n::to_wide(sf.tag);
        std::wstring path_w = i18n::to_wide(sf.host_path);
        std::wstring mode_w = sf.readonly ? i18n::tr_w(i18n::S::kSfModeReadOnly) : i18n::tr_w(i18n::S::kSfModeReadWrite);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = tag_w.data();
        int idx = static_cast<int>(SendMessageW(lv, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)));
        LVITEMW setitem{};
        setitem.iSubItem = 1;
        setitem.pszText = path_w.data();
        SendMessageW(lv, LVM_SETITEMTEXTW, idx, reinterpret_cast<LPARAM>(&setitem));
        setitem.iSubItem = 2;
        setitem.pszText = mode_w.data();
        SendMessageW(lv, LVM_SETITEMTEXTW, idx, reinterpret_cast<LPARAM>(&setitem));
    }
}

static INT_PTR CALLBACK SfDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<SfDlgData*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<SfDlgData*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(data));
        CenterDialogToParent(dlg);

        RECT rc;
        GetClientRect(dlg, &rc);
        RECT du = {0, 0, 48, 14};
        MapDialogRect(dlg, &du);
        int btn_w = du.right, btn_h = du.bottom;
        int gap = btn_h / 2, btn_gap = btn_h / 4;
        int list_w = rc.right - btn_w - gap * 3;

        RECT hdu = {0, 0, 4, 24};
        MapDialogRect(dlg, &hdu);
        int hint_h = hdu.bottom;
        int list_h = rc.bottom - gap * 3 - hint_h;

        HWND lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            gap, gap, list_w, list_h,
            dlg, reinterpret_cast<HMENU>(IDC_SF_LIST),
            GetModuleHandle(nullptr), nullptr);
        ListView_SetExtendedListViewStyle(lv,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        std::wstring col_tag = i18n::tr_w(i18n::S::kSfColTag);
        std::wstring col_path = i18n::tr_w(i18n::S::kSfColHostPath);
        std::wstring col_mode = i18n::tr_w(i18n::S::kSfColMode);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 110;
        col.pszText = col_tag.data();
        SendMessageW(lv, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&col));
        col.cx = list_w - 110 - 90 - 4;
        if (col.cx < 80) col.cx = 80;
        col.pszText = col_path.data();
        SendMessageW(lv, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&col));
        col.cx = 90;
        col.pszText = col_mode.data();
        SendMessageW(lv, LVM_INSERTCOLUMNW, 2, reinterpret_cast<LPARAM>(&col));

        data->listview = lv;

        int btn_x = gap + list_w + gap;
        MoveWindow(GetDlgItem(dlg, IDC_SF_ADD),    btn_x, gap,                          btn_w, btn_h, FALSE);
        MoveWindow(GetDlgItem(dlg, IDC_SF_REMOVE), btn_x, gap + btn_h + btn_gap,        btn_w, btn_h, FALSE);
        MoveWindow(GetDlgItem(dlg, IDC_SF_OPEN),   btn_x, gap + (btn_h + btn_gap) * 2,  btn_w, btn_h, FALSE);

        std::wstring hint = i18n::tr_w(i18n::S::kSfHint);
        HWND hint_ctrl = CreateWindowExW(0, L"STATIC", hint.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            gap, gap + list_h + gap, rc.right - gap * 2, hint_h,
            dlg, reinterpret_cast<HMENU>(IDC_SF_HINT),
            GetModuleHandle(nullptr), nullptr);
        SendMessageW(hint_ctrl, WM_SETFONT,
            SendMessageW(dlg, WM_GETFONT, 0, 0), TRUE);

        SfRefreshList(data);
        SfUpdateButtons(dlg, lv);
        return TRUE;
    }

    case WM_NOTIFY: {
        auto* nmhdr = reinterpret_cast<NMHDR*>(lp);
        if (nmhdr->idFrom == IDC_SF_LIST && nmhdr->code == LVN_ITEMCHANGED) {
            SfUpdateButtons(dlg, data->listview);
        }
        return FALSE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SF_ADD: {
            auto path_str = BrowseForFolder(dlg, i18n::tr(i18n::S::kSfBrowseTitle), nullptr);
            if (!path_str.empty()) {
                size_t last_sep = path_str.find_last_of("\\/");
                std::string tag = (last_sep != std::string::npos)
                    ? path_str.substr(last_sep + 1) : "share";
                if (tag.empty()) tag = "share";

                SharedFolder sf;
                sf.tag = tag;
                sf.host_path = path_str;
                sf.readonly = false;

                std::string error;
                if (data->mgr->AddSharedFolder(data->vm_id, sf, &error)) {
                    SfRefreshList(data);
                    SfUpdateButtons(dlg, data->listview);
                } else {
                    MessageBoxW(dlg, i18n::to_wide(error).c_str(),
                        i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONERROR);
                }
            }
            return TRUE;
        }
        case IDC_SF_OPEN: {
            int sel = ListView_GetNextItem(data->listview, -1, LVNI_SELECTED);
            if (sel >= 0) {
                wchar_t path_buf[MAX_PATH]{};
                LVITEMW lvi{};
                lvi.iSubItem = 1;
                lvi.pszText = path_buf;
                lvi.cchTextMax = static_cast<int>(std::size(path_buf));
                SendMessageW(data->listview, LVM_GETITEMTEXTW, sel, reinterpret_cast<LPARAM>(&lvi));
                ShellExecuteW(dlg, L"open", path_buf, nullptr, nullptr, SW_SHOWNORMAL);
            }
            return TRUE;
        }
        case IDC_SF_REMOVE: {
            int sel = ListView_GetNextItem(data->listview, -1, LVNI_SELECTED);
            if (sel < 0) {
                MessageBoxW(dlg, i18n::tr_w(i18n::S::kSfNoSelection).c_str(),
                    i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            wchar_t tag_buf[64]{};
            LVITEMW lvi_get{};
            lvi_get.iSubItem = 0;
            lvi_get.pszText = tag_buf;
            lvi_get.cchTextMax = static_cast<int>(std::size(tag_buf));
            SendMessageW(data->listview, LVM_GETITEMTEXTW, sel, reinterpret_cast<LPARAM>(&lvi_get));
            std::string tag_str = i18n::wide_to_utf8(tag_buf);
            std::string prompt = i18n::fmt(i18n::S::kSfConfirmRemoveMsg, tag_str.c_str());
            if (MessageBoxW(dlg, i18n::to_wide(prompt).c_str(),
                    i18n::tr_w(i18n::S::kSfConfirmRemoveTitle).c_str(),
                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
                std::string error;
                if (data->mgr->RemoveSharedFolder(data->vm_id, tag_str, &error)) {
                    SfRefreshList(data);
                    SfUpdateButtons(dlg, data->listview);
                } else {
                    MessageBoxW(dlg, i18n::to_wide(error).c_str(),
                        i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONERROR);
                }
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

void ShowSharedFoldersDialog(HWND parent, ManagerService& mgr, const std::string& vm_id) {
    using S = i18n::S;
    DlgBuilder b;
    int W = 380, H = 200;
    b.Begin(i18n::tr(S::kDlgSharedFolders), 0, 0, W, H,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT);

    int btn_h = 14, btn_w = 50;
    b.AddButton(IDC_SF_ADD,    i18n::tr(S::kSfBtnAdd),    0, 0, btn_w, btn_h);
    b.AddButton(IDC_SF_REMOVE, i18n::tr(S::kSfBtnRemove), 0, 0, btn_w, btn_h);
    b.AddButton(IDC_SF_OPEN,   i18n::tr(S::kSfBtnOpen),   0, 0, btn_w, btn_h);

    SfDlgData data{&mgr, vm_id, nullptr};
    DialogBoxIndirectParamW(GetModuleHandle(nullptr), b.Build(), parent,
        SfDlgProc, reinterpret_cast<LPARAM>(&data));
}
