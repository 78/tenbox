#include "ui/win32/win32_dialogs.h"
#include "ui/common/vm_forms.h"
#include "ui/common/i18n.h"
#include "manager/app_settings.h"
#include "manager/manager_service.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <shlobj.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// ── Shared helpers ──

static constexpr int kMemoryOptionsMb[] = {1024, 2048, 4096, 8192, 16384};
static const char* kMemoryLabels[]      = {"1 GB", "2 GB", "4 GB", "8 GB", "16 GB"};
static constexpr int kCpuOptions[]      = {1, 2, 4, 8, 16};
static const char* kCpuLabels[]         = {"1", "2", "4", "8", "16"};
static constexpr int kNumOptions        = 5;

static int MemoryMbToIndex(int mb) {
    for (int i = 0; i < kNumOptions; ++i)
        if (kMemoryOptionsMb[i] >= mb) return i;
    return kNumOptions - 1;
}

static int CpuCountToIndex(int count) {
    for (int i = 0; i < kNumOptions; ++i)
        if (kCpuOptions[i] >= count) return i;
    return kNumOptions - 1;
}

static std::string NextAgentName(const std::vector<VmRecord>& records) {
    int max_n = 0;
    for (const auto& rec : records) {
        const auto& name = rec.spec.name;
        if (name.size() > 6 && name.substr(0, 6) == "Agent_") {
            try { max_n = std::max(max_n, std::stoi(name.substr(6))); }
            catch (...) {}
        }
    }
    return "Agent_" + std::to_string(max_n + 1);
}

static std::string ExeDirectory() {
    char buf[MAX_PATH]{};
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    std::string dir(buf, len);
    auto sep = dir.find_last_of("\\/");
    return (sep != std::string::npos) ? dir.substr(0, sep + 1) : std::string{};
}

static std::string FindShareFile(const std::string& exe_dir, const char* filename) {
    namespace fs = std::filesystem;
    for (const char* prefix : {"share\\", "..\\share\\"}) {
        auto path = fs::path(exe_dir) / prefix / filename;
        if (fs::exists(path)) return path.string();
    }
    return {};
}

static std::string GetDlgText(HWND dlg, int id) {
    char buf[1024]{};
    GetDlgItemTextA(dlg, id, buf, sizeof(buf));
    return buf;
}

// ── In-memory dialog template builder ──
// Builds a DLGTEMPLATE + items in a flat buffer, properly aligned.

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
        AppendWord(0); // menu
        AppendWord(0); // class
        AppendWideStr(title);
        AppendWord(9); // font size
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

    void AddComboBox(int id, int x, int y, int cx, int cy) {
        AddItem(id, 0x0085, "", x, y, cx, cy,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL);
    }

    void AddCheckBox(int id, const char* text, int x, int y, int cx, int cy) {
        AddItem(id, 0x0080, text, x, y, cx, cy,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX);
    }

    void AddButton(int id, const char* text, int x, int y, int cx, int cy, DWORD style = 0) {
        AddItem(id, 0x0080, text, x, y, cx, cy,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | style);
    }

    void AddDefButton(int id, const char* text, int x, int y, int cx, int cy) {
        AddButton(id, text, x, y, cx, cy, BS_DEFPUSHBUTTON);
    }

    LPCDLGTEMPLATE Build() {
        // Patch item count
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
                 int x, int y, int cx, int cy, DWORD style)
    {
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
        AppendWord(0); // extra data
        ++item_count_;
    }
};

// ════════════════════════════════════════════════════════════
// Create VM Dialog
// ════════════════════════════════════════════════════════════

enum CreateDlgId {
    IDC_CR_NAME     = 100,
    IDC_CR_KERNEL   = 101,
    IDC_CR_INITRD   = 102,
    IDC_CR_DISK     = 103,
    IDC_CR_MEMORY   = 104,
    IDC_CR_CPUS     = 105,
    IDC_CR_NAT      = 106,
    IDC_CR_LOC_LBL  = 107,
    IDC_CR_OK       = IDOK,
    IDC_CR_CANCEL   = IDCANCEL,
};

struct CreateDlgData {
    ManagerService* mgr;
    bool created;
    std::string error;
};

static INT_PTR CALLBACK CreateDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<CreateDlgData*>(GetWindowLongPtrA(dlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<CreateDlgData*>(lp);
        SetWindowLongPtrA(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(data));

        auto records = data->mgr->ListVms();
        SetDlgItemTextA(dlg, IDC_CR_NAME, NextAgentName(records).c_str());

        std::string dir = ExeDirectory();
        SetDlgItemTextA(dlg, IDC_CR_KERNEL, FindShareFile(dir, "vmlinuz").c_str());
        SetDlgItemTextA(dlg, IDC_CR_INITRD, FindShareFile(dir, "initramfs.cpio.gz").c_str());
        SetDlgItemTextA(dlg, IDC_CR_DISK, FindShareFile(dir, "rootfs.qcow2").c_str());

        HWND mem_cb = GetDlgItem(dlg, IDC_CR_MEMORY);
        for (int i = 0; i < kNumOptions; ++i)
            SendMessageA(mem_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kMemoryLabels[i]));
        SendMessage(mem_cb, CB_SETCURSEL, 2, 0);

        HWND cpu_cb = GetDlgItem(dlg, IDC_CR_CPUS);
        for (int i = 0; i < kNumOptions; ++i)
            SendMessageA(cpu_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kCpuLabels[i]));
        SendMessage(cpu_cb, CB_SETCURSEL, 2, 0);

        CheckDlgButton(dlg, IDC_CR_NAT, BST_CHECKED);

        auto vm_storage = settings::DefaultVmStorageDir();
        SetDlgItemTextA(dlg, IDC_CR_LOC_LBL, vm_storage.c_str());

        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            int mem_idx = static_cast<int>(SendMessage(GetDlgItem(dlg, IDC_CR_MEMORY), CB_GETCURSEL, 0, 0));
            int cpu_idx = static_cast<int>(SendMessage(GetDlgItem(dlg, IDC_CR_CPUS), CB_GETCURSEL, 0, 0));

            VmCreateRequest req;
            req.name          = GetDlgText(dlg, IDC_CR_NAME);
            req.source_kernel = GetDlgText(dlg, IDC_CR_KERNEL);
            req.source_initrd = GetDlgText(dlg, IDC_CR_INITRD);
            req.source_disk   = GetDlgText(dlg, IDC_CR_DISK);
            req.memory_mb     = (mem_idx >= 0 && mem_idx < kNumOptions)
                                    ? kMemoryOptionsMb[mem_idx] : 4096;
            req.cpu_count     = (cpu_idx >= 0 && cpu_idx < kNumOptions)
                                    ? kCpuOptions[cpu_idx] : 4;
            req.nat_enabled   = IsDlgButtonChecked(dlg, IDC_CR_NAT) == BST_CHECKED;

            auto v = ValidateCreateRequest(req);
            if (!v.ok) {
                MessageBoxA(dlg, v.message.c_str(), i18n::tr(i18n::S::kValidationError), MB_OK | MB_ICONWARNING);
                return TRUE;
            }

            std::string error;
            if (data->mgr->CreateVm(req, &error)) {
                data->created = true;
                EndDialog(dlg, IDOK);
            } else {
                MessageBoxA(dlg, error.c_str(), i18n::tr(i18n::S::kError), MB_OK | MB_ICONERROR);
            }
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

bool ShowCreateVmDialog(HWND parent, ManagerService& mgr, std::string* error) {
    using S = i18n::S;
    DlgBuilder b;
    int W = 260, H = 210;
    b.Begin(i18n::tr(S::kDlgCreateVm), 0, 0, W, H,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER);

    int lx = 8, lw = 40, ex = 52, ew = W - 60, y = 8, rh = 14, sp = 18;

    b.AddStatic(0,          i18n::tr(S::kDlgLabelName),   lx, y, lw, rh);
    b.AddEdit(IDC_CR_NAME,              ex, y-2, ew, rh); y += sp;
    b.AddStatic(0,          i18n::tr(S::kDlgLabelKernel), lx, y, lw, rh);
    b.AddEdit(IDC_CR_KERNEL,            ex, y-2, ew, rh); y += sp;
    b.AddStatic(0,          i18n::tr(S::kDlgLabelInitrd), lx, y, lw, rh);
    b.AddEdit(IDC_CR_INITRD,            ex, y-2, ew, rh); y += sp;
    b.AddStatic(0,          i18n::tr(S::kDlgLabelDisk),   lx, y, lw, rh);
    b.AddEdit(IDC_CR_DISK,              ex, y-2, ew, rh); y += sp;
    b.AddStatic(0,          i18n::tr(S::kDlgLabelMemory), lx, y, lw, rh);
    b.AddComboBox(IDC_CR_MEMORY,        ex, y-2, ew, 100); y += sp;
    b.AddStatic(0,          i18n::tr(S::kDlgLabelVcpus),  lx, y, lw, rh);
    b.AddComboBox(IDC_CR_CPUS,          ex, y-2, ew, 100); y += sp;
    b.AddCheckBox(IDC_CR_NAT, i18n::tr(S::kDlgEnableNat), ex, y, ew, rh); y += sp;
    b.AddStatic(0,          i18n::tr(S::kDlgLabelLocation), lx, y, lw, rh);
    b.AddStatic(IDC_CR_LOC_LBL, "",     ex, y, ew, rh); y += sp + 4;

    b.AddButton(IDCANCEL, i18n::tr(S::kDlgBtnCancel), W - 110, y, 48, 14);
    b.AddDefButton(IDOK,  i18n::tr(S::kDlgBtnCreate), W - 56, y, 48, 14);

    CreateDlgData data{&mgr, false, ""};
    DialogBoxIndirectParamA(GetModuleHandle(nullptr), b.Build(), parent,
        CreateDlgProc, reinterpret_cast<LPARAM>(&data));

    if (error) *error = data.error;
    return data.created;
}

// ════════════════════════════════════════════════════════════
// Edit VM Dialog
// ════════════════════════════════════════════════════════════

enum EditDlgId {
    IDC_ED_NAME     = 200,
    IDC_ED_MEMORY   = 201,
    IDC_ED_CPUS     = 202,
    IDC_ED_NAT      = 203,
    IDC_ED_WARN     = 204,
    IDC_ED_OK       = IDOK,
    IDC_ED_CANCEL   = IDCANCEL,
};

struct EditDlgData {
    ManagerService* mgr;
    VmRecord rec;
    bool saved;
    std::string error;
};

static INT_PTR CALLBACK EditDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<EditDlgData*>(GetWindowLongPtrA(dlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<EditDlgData*>(lp);
        SetWindowLongPtrA(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(data));

        std::string title = std::string(i18n::tr(i18n::S::kDlgEditTitlePrefix)) + data->rec.spec.name;
        SetWindowTextA(dlg, title.c_str());

        SetDlgItemTextA(dlg, IDC_ED_NAME, data->rec.spec.name.c_str());

        HWND mem_cb = GetDlgItem(dlg, IDC_ED_MEMORY);
        for (int i = 0; i < kNumOptions; ++i)
            SendMessageA(mem_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kMemoryLabels[i]));
        SendMessage(mem_cb, CB_SETCURSEL,
            MemoryMbToIndex(static_cast<int>(data->rec.spec.memory_mb)), 0);

        HWND cpu_cb = GetDlgItem(dlg, IDC_ED_CPUS);
        for (int i = 0; i < kNumOptions; ++i)
            SendMessageA(cpu_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kCpuLabels[i]));
        SendMessage(cpu_cb, CB_SETCURSEL,
            CpuCountToIndex(static_cast<int>(data->rec.spec.cpu_count)), 0);

        CheckDlgButton(dlg, IDC_ED_NAT, data->rec.spec.nat_enabled ? BST_CHECKED : BST_UNCHECKED);

        bool running = data->rec.state == VmPowerState::kRunning ||
                       data->rec.state == VmPowerState::kStarting;
        EnableWindow(mem_cb, !running);
        EnableWindow(cpu_cb, !running);

        if (running) {
            SetDlgItemTextA(dlg, IDC_ED_WARN, i18n::tr(i18n::S::kCpuMemoryChangeWarning));
        }

        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            int mem_idx = static_cast<int>(SendMessage(GetDlgItem(dlg, IDC_ED_MEMORY), CB_GETCURSEL, 0, 0));
            int cpu_idx = static_cast<int>(SendMessage(GetDlgItem(dlg, IDC_ED_CPUS), CB_GETCURSEL, 0, 0));

            bool running = data->rec.state == VmPowerState::kRunning ||
                           data->rec.state == VmPowerState::kStarting;

            VmEditForm form;
            form.vm_id             = data->rec.spec.vm_id;
            form.name              = GetDlgText(dlg, IDC_ED_NAME);
            form.memory_mb         = (mem_idx >= 0 && mem_idx < kNumOptions)
                                         ? kMemoryOptionsMb[mem_idx] : 4096;
            form.cpu_count         = (cpu_idx >= 0 && cpu_idx < kNumOptions)
                                         ? kCpuOptions[cpu_idx] : 4;
            form.nat_enabled       = IsDlgButtonChecked(dlg, IDC_ED_NAT) == BST_CHECKED;
            form.apply_on_next_boot = running;

            auto patch = BuildVmPatch(form, data->rec.spec);
            std::string error;
            if (data->mgr->EditVm(data->rec.spec.vm_id, patch, &error)) {
                data->saved = true;
                EndDialog(dlg, IDOK);
            } else {
                MessageBoxA(dlg, error.c_str(), i18n::tr(i18n::S::kError), MB_OK | MB_ICONERROR);
            }
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

bool ShowEditVmDialog(HWND parent, ManagerService& mgr,
                      const VmRecord& rec, std::string* error) {
    using S = i18n::S;
    DlgBuilder b;
    int W = 220, H = 148;
    b.Begin(i18n::tr(S::kDlgEditVm), 0, 0, W, H,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER);

    int lx = 8, lw = 40, ex = 52, ew = W - 60, y = 8, rh = 14, sp = 18;

    b.AddStatic(0,         i18n::tr(S::kDlgLabelName),   lx, y, lw, rh);
    b.AddEdit(IDC_ED_NAME,             ex, y-2, ew, rh); y += sp;
    b.AddStatic(0,         i18n::tr(S::kDlgLabelMemory), lx, y, lw, rh);
    b.AddComboBox(IDC_ED_MEMORY,       ex, y-2, ew, 100); y += sp;
    b.AddStatic(0,         i18n::tr(S::kDlgLabelVcpus),  lx, y, lw, rh);
    b.AddComboBox(IDC_ED_CPUS,         ex, y-2, ew, 100); y += sp;
    b.AddCheckBox(IDC_ED_NAT, i18n::tr(S::kDlgEnableNat), ex, y, ew, rh); y += sp;
    b.AddStatic(IDC_ED_WARN, "",       lx, y, W - 16, rh); y += sp + 4;

    b.AddButton(IDCANCEL, i18n::tr(S::kDlgBtnCancel), W - 110, y, 48, 14);
    b.AddDefButton(IDOK,  i18n::tr(S::kDlgBtnSave),  W - 56, y, 48, 14);

    EditDlgData data{&mgr, rec, false, ""};
    DialogBoxIndirectParamA(GetModuleHandle(nullptr), b.Build(), parent,
        EditDlgProc, reinterpret_cast<LPARAM>(&data));

    if (error) *error = data.error;
    return data.saved;
}

// ════════════════════════════════════════════════════════════
// Shared Folders Dialog
// ════════════════════════════════════════════════════════════

enum SfDlgId {
    IDC_SF_LIST    = 300,
    IDC_SF_ADD     = 301,
    IDC_SF_REMOVE  = 302,
    IDC_SF_CLOSE   = IDCANCEL,
};

struct SfDlgData {
    ManagerService* mgr;
    std::string vm_id;
    HWND listview;
};

static void SfRefreshList(SfDlgData* data) {
    HWND lv = data->listview;
    ListView_DeleteAllItems(lv);

    auto folders = data->mgr->GetSharedFolders(data->vm_id);
    for (size_t i = 0; i < folders.size(); ++i) {
        const auto& sf = folders[i];
        LVITEMA item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<char*>(sf.tag.c_str());
        int idx = ListView_InsertItem(lv, &item);
        ListView_SetItemText(lv, idx, 1, const_cast<char*>(sf.host_path.c_str()));
        ListView_SetItemText(lv, idx, 2,
            const_cast<char*>(sf.readonly
                ? i18n::tr(i18n::S::kSfModeReadOnly)
                : i18n::tr(i18n::S::kSfModeReadWrite)));
    }
}

static INT_PTR CALLBACK SfDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<SfDlgData*>(GetWindowLongPtrA(dlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<SfDlgData*>(lp);
        SetWindowLongPtrA(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(data));

        RECT rc;
        GetClientRect(dlg, &rc);
        // Convert dialog units to pixels to match other dialogs (48x14 DU)
        RECT du = {0, 0, 48, 14};
        MapDialogRect(dlg, &du);
        int btn_w = du.right, btn_h = du.bottom;
        int gap = btn_h / 2, btn_gap = btn_h / 4;
        // ListView occupies left area; right column holds the buttons
        int list_w = rc.right - btn_w - gap * 3;
        int list_h = rc.bottom - gap * 2;

        HWND lv = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            gap, gap, list_w, list_h,
            dlg, reinterpret_cast<HMENU>(IDC_SF_LIST),
            GetModuleHandle(nullptr), nullptr);
        ListView_SetExtendedListViewStyle(lv,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        // Setup columns
        LVCOLUMNA col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 110;
        col.pszText = const_cast<char*>(i18n::tr(i18n::S::kSfColTag));
        ListView_InsertColumn(lv, 0, &col);
        col.cx = list_w - 110 - 90 - 4;
        if (col.cx < 80) col.cx = 80;
        col.pszText = const_cast<char*>(i18n::tr(i18n::S::kSfColHostPath));
        ListView_InsertColumn(lv, 1, &col);
        col.cx = 90;
        col.pszText = const_cast<char*>(i18n::tr(i18n::S::kSfColMode));
        ListView_InsertColumn(lv, 2, &col);

        data->listview = lv;

        // Buttons stacked on the right
        int btn_x = gap + list_w + gap;
        MoveWindow(GetDlgItem(dlg, IDC_SF_ADD),    btn_x, gap,                          btn_w, btn_h, FALSE);
        MoveWindow(GetDlgItem(dlg, IDC_SF_REMOVE), btn_x, gap + btn_h + btn_gap,        btn_w, btn_h, FALSE);
        MoveWindow(GetDlgItem(dlg, IDC_SF_CLOSE),  btn_x, rc.bottom - gap - btn_h,      btn_w, btn_h, FALSE);

        SfRefreshList(data);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SF_ADD: {
            BROWSEINFOA bi{};
            bi.hwndOwner = dlg;
            bi.lpszTitle = i18n::tr(i18n::S::kSfBrowseTitle);
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

            LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
            if (pidl) {
                char path_buf[MAX_PATH]{};
                SHGetPathFromIDListA(pidl, path_buf);
                CoTaskMemFree(pidl);

                std::string path_str(path_buf);
                size_t last_sep = path_str.find_last_of("\\/");
                std::string tag = (last_sep != std::string::npos)
                    ? path_str.substr(last_sep + 1) : "share";
                if (tag.empty()) tag = "share";

                SharedFolder sf;
                sf.tag = tag;
                sf.host_path = path_buf;
                sf.readonly = false;

                std::string error;
                if (data->mgr->AddSharedFolder(data->vm_id, sf, &error)) {
                    SfRefreshList(data);
                } else {
                    MessageBoxA(dlg, error.c_str(),
                        i18n::tr(i18n::S::kError), MB_OK | MB_ICONERROR);
                }
            }
            return TRUE;
        }
        case IDC_SF_REMOVE: {
            int sel = ListView_GetNextItem(data->listview, -1, LVNI_SELECTED);
            if (sel < 0) {
                MessageBoxA(dlg, i18n::tr(i18n::S::kSfNoSelection),
                    i18n::tr(i18n::S::kError), MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            char tag_buf[64]{};
            ListView_GetItemText(data->listview, sel, 0, tag_buf, sizeof(tag_buf));
            std::string prompt = i18n::fmt(i18n::S::kSfConfirmRemoveMsg, tag_buf);
            if (MessageBoxA(dlg, prompt.c_str(),
                    i18n::tr(i18n::S::kSfConfirmRemoveTitle),
                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
                std::string error;
                if (data->mgr->RemoveSharedFolder(data->vm_id, tag_buf, &error)) {
                    SfRefreshList(data);
                } else {
                    MessageBoxA(dlg, error.c_str(),
                        i18n::tr(i18n::S::kError), MB_OK | MB_ICONERROR);
                }
            }
            return TRUE;
        }
        case IDC_SF_CLOSE:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
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
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_CENTER | DS_SETFONT);

    // Placeholder buttons (positions will be set in WM_INITDIALOG)
    int btn_h = 14, btn_w = 50;
    b.AddButton(IDC_SF_ADD,    i18n::tr(S::kSfBtnAdd),    0, 0, btn_w, btn_h);
    b.AddButton(IDC_SF_REMOVE, i18n::tr(S::kSfBtnRemove), 0, 0, btn_w, btn_h);
    b.AddButton(IDC_SF_CLOSE,  i18n::tr(S::kDlgBtnClose), 0, 0, btn_w, btn_h);

    SfDlgData data{&mgr, vm_id, nullptr};
    DialogBoxIndirectParamA(GetModuleHandle(nullptr), b.Build(), parent,
        SfDlgProc, reinterpret_cast<LPARAM>(&data));
}
