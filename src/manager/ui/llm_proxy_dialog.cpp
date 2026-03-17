#include "manager/ui/llm_proxy_dialog.h"
#include "manager/i18n.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

#include <array>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace {

// ── DlgBuilder (reusable in-memory dialog template builder) ──────────

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

    void AddDefButton(int id, const char* text, int x, int y, int cx, int cy) {
        AddButton(id, text, x, y, cx, cy, BS_DEFPUSHBUTTON);
    }

    void AddComboBox(int id, int x, int y, int cx, int cy) {
        AddItem(id, 0x0085, "", x, y, cx, cy,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL);
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
        if (!s || !*s) { AppendWord(0); return; }
        int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        if (len <= 0) { AppendWord(0); return; }
        std::vector<wchar_t> wstr(len);
        MultiByteToWideChar(CP_UTF8, 0, s, -1, wstr.data(), len);
        for (int i = 0; i < len; ++i) AppendWord(static_cast<WORD>(wstr[i]));
    }
    void Align(size_t a) { while (buf_.size() % a) buf_.push_back(0); }
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

// ── Control IDs ──────────────────────────────────────────────────────

enum LlmDlgId {
    IDC_LLM_LIST      = 501,
    IDC_LLM_ADD       = 502,
    IDC_LLM_EDIT      = 503,
    IDC_LLM_REMOVE    = 504,
    IDC_LLM_HINT      = 505,
};

enum LlmEditDlgId {
    IDC_LLE_ALIAS      = 600,
    IDC_LLE_API_TYPE   = 601,
    IDC_LLE_TARGET_URL = 602,
    IDC_LLE_API_KEY    = 603,
    IDC_LLE_MODEL      = 604,
};

// ── Edit mapping sub-dialog ──────────────────────────────────────────

struct EditMappingData {
    settings::LlmModelMapping mapping;
    std::set<std::string> existing_aliases;  // aliases already in use (excluding self for edit)
    bool accepted = false;
};

static INT_PTR CALLBACK EditMappingDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<EditMappingData*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<EditMappingData*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(data));
        CenterDialogToParent(dlg);

        SetDlgItemTextW(dlg, IDC_LLE_ALIAS, i18n::to_wide(data->mapping.alias).c_str());
        SetDlgItemTextW(dlg, IDC_LLE_TARGET_URL, i18n::to_wide(data->mapping.target_url).c_str());
        SetDlgItemTextW(dlg, IDC_LLE_API_KEY, i18n::to_wide(data->mapping.api_key).c_str());
        SetDlgItemTextW(dlg, IDC_LLE_MODEL, i18n::to_wide(data->mapping.model).c_str());

        SendDlgItemMessageW(dlg, IDC_LLE_ALIAS, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(L"e.g. default"));
        SendDlgItemMessageW(dlg, IDC_LLE_TARGET_URL, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(L"https://api.openai.com/v1"));
        SendDlgItemMessageW(dlg, IDC_LLE_API_KEY, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(L"sk-..."));
        SendDlgItemMessageW(dlg, IDC_LLE_MODEL, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(L"e.g. gpt-5.4"));

        HWND combo = GetDlgItem(dlg, IDC_LLE_API_TYPE);
        SendMessageW(combo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(i18n::tr_w(i18n::S::kLlmApiTypeOpenAiCompletions).c_str()));
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(data->mapping.api_type), 0);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            wchar_t buf[512];
            GetDlgItemTextW(dlg, IDC_LLE_ALIAS, buf, 512);
            data->mapping.alias = i18n::wide_to_utf8(buf);

            int sel = static_cast<int>(SendDlgItemMessageW(dlg, IDC_LLE_API_TYPE, CB_GETCURSEL, 0, 0));
            data->mapping.api_type = static_cast<settings::LlmApiType>(sel >= 0 ? sel : 0);

            GetDlgItemTextW(dlg, IDC_LLE_TARGET_URL, buf, 512);
            data->mapping.target_url = i18n::wide_to_utf8(buf);

            GetDlgItemTextW(dlg, IDC_LLE_API_KEY, buf, 512);
            data->mapping.api_key = i18n::wide_to_utf8(buf);

            GetDlgItemTextW(dlg, IDC_LLE_MODEL, buf, 512);
            data->mapping.model = i18n::wide_to_utf8(buf);

            if (data->mapping.alias.empty()) {
                MessageBoxW(dlg, L"Alias is required.", i18n::tr_w(i18n::S::kError).c_str(),
                    MB_OK | MB_ICONWARNING);
                return TRUE;
            }

            if (data->existing_aliases.count(data->mapping.alias)) {
                auto msg = i18n::fmt(i18n::S::kLlmDuplicateAlias, data->mapping.alias.c_str());
                MessageBoxW(dlg, i18n::to_wide(msg).c_str(),
                    i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONWARNING);
                return TRUE;
            }

            data->accepted = true;
            EndDialog(dlg, IDOK);
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

static bool ShowEditMappingDialog(HWND parent, const char* title,
                                  settings::LlmModelMapping& mapping,
                                  std::set<std::string> existing_aliases) {
    using S = i18n::S;
    DlgBuilder b;
    int W = 260, H = 136;
    b.Begin(title, 0, 0, W, H,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT);

    int lx = 8, lw = 58, ex = 70, ew = 180, y = 10, rh = 14, sp = 18;

    b.AddStatic(0, i18n::tr(S::kLlmLabelAlias), lx, y, lw, rh);
    b.AddEdit(IDC_LLE_ALIAS, ex, y - 2, ew, rh);
    y += sp;

    b.AddStatic(0, i18n::tr(S::kLlmLabelApiType), lx, y, lw, rh);
    b.AddComboBox(IDC_LLE_API_TYPE, ex, y - 2, ew, 60);
    y += sp;

    b.AddStatic(0, i18n::tr(S::kLlmLabelTargetUrl), lx, y, lw, rh);
    b.AddEdit(IDC_LLE_TARGET_URL, ex, y - 2, ew, rh);
    y += sp;

    b.AddStatic(0, i18n::tr(S::kLlmLabelApiKey), lx, y, lw, rh);
    b.AddEdit(IDC_LLE_API_KEY, ex, y - 2, ew, rh);
    y += sp;

    b.AddStatic(0, i18n::tr(S::kLlmLabelModel), lx, y, lw, rh);
    b.AddEdit(IDC_LLE_MODEL, ex, y - 2, ew, rh);
    y += sp + 6;

    int btn_w = 50, btn_h = 14;
    b.AddDefButton(IDOK, i18n::tr(S::kDlgBtnSave), W - btn_w - 8, y, btn_w, btn_h);

    EditMappingData data{mapping, std::move(existing_aliases), false};
    DialogBoxIndirectParamW(GetModuleHandle(nullptr), b.Build(), parent,
        EditMappingDlgProc, reinterpret_cast<LPARAM>(&data));
    if (data.accepted) {
        mapping = data.mapping;
        return true;
    }
    return false;
}

// ── Main dialog ──────────────────────────────────────────────────────

struct LlmDlgData {
    settings::LlmProxySettings settings;
    LlmProxySettingsCallback on_change;
    HWND listview;
};

static void LlmRefreshList(LlmDlgData* data) {
    HWND lv = data->listview;
    ListView_DeleteAllItems(lv);

    for (size_t i = 0; i < data->settings.mappings.size(); ++i) {
        const auto& m = data->settings.mappings[i];
        std::wstring alias_w = i18n::to_wide(m.alias);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = alias_w.data();
        int idx = static_cast<int>(SendMessageW(lv, LVM_INSERTITEMW, 0,
            reinterpret_cast<LPARAM>(&item)));

        auto SetSubItem = [&](int col, const std::wstring& text) {
            std::wstring t = text;
            LVITEMW si{};
            si.mask = LVIF_TEXT;
            si.iItem = idx;
            si.iSubItem = col;
            si.pszText = t.data();
            SendMessageW(lv, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&si));
        };

        SetSubItem(1, i18n::to_wide(m.target_url));
        SetSubItem(2, i18n::to_wide(m.model));
    }
}

static void LlmUpdateButtons(HWND dlg, HWND listview) {
    BOOL has_sel = ListView_GetNextItem(listview, -1, LVNI_SELECTED) >= 0;
    EnableWindow(GetDlgItem(dlg, IDC_LLM_EDIT), has_sel);
    EnableWindow(GetDlgItem(dlg, IDC_LLM_REMOVE), has_sel);
}

static std::set<std::string> CollectAliases(const std::vector<settings::LlmModelMapping>& mappings,
                                            int exclude_index = -1) {
    std::set<std::string> aliases;
    for (int i = 0; i < static_cast<int>(mappings.size()); ++i) {
        if (i != exclude_index)
            aliases.insert(mappings[i].alias);
    }
    return aliases;
}

static void LlmNotifyChange(LlmDlgData* data) {
    if (data->on_change)
        data->on_change(data->settings);
}

static INT_PTR CALLBACK LlmDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<LlmDlgData*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<LlmDlgData*>(lp);
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
            dlg, reinterpret_cast<HMENU>(static_cast<uintptr_t>(IDC_LLM_LIST)),
            GetModuleHandle(nullptr), nullptr);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        auto AddCol = [&](int idx, i18n::S str_id, int width) {
            std::wstring text = i18n::tr_w(str_id);
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.cx = width;
            col.pszText = text.data();
            SendMessageW(lv, LVM_INSERTCOLUMNW, idx, reinterpret_cast<LPARAM>(&col));
        };
        int col_w = list_w / 3;
        AddCol(0, i18n::S::kLlmColAlias, col_w - 20);
        AddCol(1, i18n::S::kLlmColTargetUrl, col_w + 30);
        AddCol(2, i18n::S::kLlmColModel, col_w - 14);

        data->listview = lv;

        int btn_x = gap + list_w + gap;
        MoveWindow(GetDlgItem(dlg, IDC_LLM_ADD), btn_x, gap, btn_w, btn_h, FALSE);
        MoveWindow(GetDlgItem(dlg, IDC_LLM_EDIT), btn_x, gap + btn_h + btn_gap, btn_w, btn_h, FALSE);
        MoveWindow(GetDlgItem(dlg, IDC_LLM_REMOVE), btn_x, gap + (btn_h + btn_gap) * 2, btn_w, btn_h, FALSE);

        std::wstring hint = i18n::tr_w(i18n::S::kLlmHint);
        HWND hint_ctrl = CreateWindowExW(0, L"STATIC", hint.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            gap, gap + list_h + gap, rc.right - gap * 2, hint_h,
            dlg, reinterpret_cast<HMENU>(IDC_LLM_HINT),
            GetModuleHandle(nullptr), nullptr);
        SendMessageW(hint_ctrl, WM_SETFONT,
            SendMessageW(dlg, WM_GETFONT, 0, 0), TRUE);

        LlmRefreshList(data);
        LlmUpdateButtons(dlg, lv);
        return TRUE;
    }

    case WM_NOTIFY: {
        auto* nmhdr = reinterpret_cast<NMHDR*>(lp);
        if (nmhdr->idFrom == IDC_LLM_LIST && nmhdr->code == LVN_ITEMCHANGED) {
            LlmUpdateButtons(dlg, data->listview);
        }
        if (nmhdr->idFrom == IDC_LLM_LIST && nmhdr->code == NM_DBLCLK) {
            int sel = ListView_GetNextItem(data->listview, -1, LVNI_SELECTED);
            if (sel >= 0 && sel < static_cast<int>(data->settings.mappings.size())) {
                auto& m = data->settings.mappings[sel];
                if (ShowEditMappingDialog(dlg, i18n::tr(i18n::S::kLlmEditTitle), m,
                        CollectAliases(data->settings.mappings, sel))) {
                    LlmRefreshList(data);
                    LlmNotifyChange(data);
                }
            }
        }
        return FALSE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_LLM_ADD: {
            settings::LlmModelMapping m;
            m.api_type = settings::LlmApiType::kOpenAiCompletions;
            if (ShowEditMappingDialog(dlg, i18n::tr(i18n::S::kLlmAddTitle), m,
                    CollectAliases(data->settings.mappings))) {
                data->settings.mappings.push_back(std::move(m));
                LlmRefreshList(data);
                LlmUpdateButtons(dlg, data->listview);
                LlmNotifyChange(data);
            }
            return TRUE;
        }

        case IDC_LLM_EDIT: {
            int sel = ListView_GetNextItem(data->listview, -1, LVNI_SELECTED);
            if (sel < 0 || sel >= static_cast<int>(data->settings.mappings.size())) {
                MessageBoxW(dlg, i18n::tr_w(i18n::S::kLlmNoSelection).c_str(),
                    i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            auto& m = data->settings.mappings[sel];
            if (ShowEditMappingDialog(dlg, i18n::tr(i18n::S::kLlmEditTitle), m,
                    CollectAliases(data->settings.mappings, sel))) {
                LlmRefreshList(data);
                LlmNotifyChange(data);
            }
            return TRUE;
        }

        case IDC_LLM_REMOVE: {
            int sel = ListView_GetNextItem(data->listview, -1, LVNI_SELECTED);
            if (sel < 0 || sel >= static_cast<int>(data->settings.mappings.size())) {
                MessageBoxW(dlg, i18n::tr_w(i18n::S::kLlmNoSelection).c_str(),
                    i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            const auto& m = data->settings.mappings[sel];
            std::string prompt = i18n::fmt(i18n::S::kLlmConfirmRemoveMsg, m.alias.c_str());
            if (MessageBoxW(dlg, i18n::to_wide(prompt).c_str(),
                    i18n::tr_w(i18n::S::kLlmConfirmRemoveTitle).c_str(),
                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
                data->settings.mappings.erase(
                    data->settings.mappings.begin() + sel);
                LlmRefreshList(data);
                LlmUpdateButtons(dlg, data->listview);
                LlmNotifyChange(data);
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

void ShowLlmProxyDialog(HWND parent,
                         settings::LlmProxySettings current_settings,
                         LlmProxySettingsCallback on_change) {
    using S = i18n::S;
    DlgBuilder b;
    int W = 380, H = 200;
    b.Begin(i18n::tr(S::kDlgLlmProxy), 0, 0, W, H,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT);

    int btn_h = 14, btn_w = 48;
    b.AddButton(IDC_LLM_ADD, i18n::tr(S::kLlmBtnAdd), 0, 0, btn_w, btn_h);
    b.AddButton(IDC_LLM_EDIT, i18n::tr(S::kLlmBtnEdit), 0, 0, btn_w, btn_h);
    b.AddButton(IDC_LLM_REMOVE, i18n::tr(S::kLlmBtnRemove), 0, 0, btn_w, btn_h);

    LlmDlgData data{std::move(current_settings), std::move(on_change), nullptr};
    DialogBoxIndirectParamW(GetModuleHandle(nullptr), b.Build(), parent,
        LlmDlgProc, reinterpret_cast<LPARAM>(&data));
}
