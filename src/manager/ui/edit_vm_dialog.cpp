#include "manager/ui/win32_dialogs.h"
#include "manager/ui/dlg_builder.h"
#include "manager/vm_forms.h"
#include "manager/i18n.h"
#include "manager/manager_service.h"

#include <commctrl.h>
#include <string>
#include <functional>

enum EditDlgId {
    IDC_ED_NAME_LABEL  = 200,
    IDC_ED_NAME        = 201,
    IDC_ED_MEM_LABEL   = 202,
    IDC_ED_MEM_SLIDER  = 203,
    IDC_ED_MEM_VALUE   = 204,
    IDC_ED_CPU_LABEL   = 205,
    IDC_ED_CPU_SLIDER  = 206,
    IDC_ED_CPU_VALUE   = 207,
    IDC_ED_DEBUG       = 208,
    IDC_ED_WARN        = 209,
    IDC_ED_OK          = IDOK,
};

struct EditDlgData {
    ManagerService* mgr;
    VmRecord rec;
    bool saved;
    std::string error;
};

static LRESULT CALLBACK EditDlgSubclassProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp,
                                            UINT_PTR, DWORD_PTR ref) {
    auto* data = reinterpret_cast<EditDlgData*>(ref);

    switch (msg) {
    case WM_HSCROLL:
        if (HandleSliderScroll(dlg, lp,
                IDC_ED_MEM_SLIDER, IDC_ED_MEM_VALUE,
                IDC_ED_CPU_SLIDER, IDC_ED_CPU_VALUE))
            return 0;
        break;

    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            int mem_gb = static_cast<int>(SendMessage(
                GetDlgItem(dlg, IDC_ED_MEM_SLIDER), TBM_GETPOS, 0, 0));
            int cpu_count = static_cast<int>(SendMessage(
                GetDlgItem(dlg, IDC_ED_CPU_SLIDER), TBM_GETPOS, 0, 0));
            if (mem_gb < 1) mem_gb = kDefaultMemoryGb;
            if (cpu_count < 1) cpu_count = kDefaultVcpus;

            bool running = data->rec.state == VmPowerState::kRunning ||
                           data->rec.state == VmPowerState::kStarting;

            VmEditForm form;
            form.vm_id              = data->rec.spec.vm_id;
            form.name               = GetDlgText(dlg, IDC_ED_NAME);
            form.memory_mb          = mem_gb * 1024;
            form.cpu_count          = cpu_count;
            form.debug_mode         = IsDlgButtonChecked(dlg, IDC_ED_DEBUG) == BST_CHECKED;
            form.apply_on_next_boot = running;

            auto patch = BuildVmPatch(form, data->rec.spec);
            std::string error;
            if (data->mgr->EditVm(data->rec.spec.vm_id, patch, &error)) {
                data->saved = true;
                EnableWindow(GetWindow(dlg, GW_OWNER), TRUE);
                DestroyWindow(dlg);
            } else {
                MessageBoxW(dlg, i18n::to_wide(error).c_str(),
                    i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        break;

    case WM_CLOSE:
        EnableWindow(GetWindow(dlg, GW_OWNER), TRUE);
        DestroyWindow(dlg);
        return 0;

    case WM_NCDESTROY:
        RemoveWindowSubclass(dlg, EditDlgSubclassProc, 1);
        break;
    }
    return DefSubclassProc(dlg, msg, wp, lp);
}

static const wchar_t* kEditDlgClassName = L"TenBoxEditVmDlg";
static bool g_edit_class_registered = false;

static void RegisterEditDialogClass() {
    if (g_edit_class_registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance      = GetModuleHandle(nullptr);
    wc.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kEditDlgClassName;
    RegisterClassExW(&wc);
    g_edit_class_registered = true;
}

bool ShowEditVmDialog(HWND parent, ManagerService& mgr,
                      const VmRecord& rec, std::string* error) {
    using S = i18n::S;
    RegisterEditDialogClass();

    EditDlgData data{&mgr, rec, false, ""};

    RECT parent_rect;
    GetWindowRect(parent, &parent_rect);
    int pw = parent_rect.right - parent_rect.left;
    int ph = parent_rect.bottom - parent_rect.top;

    UINT dpi = 96;
#if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0A00
    dpi = GetDpiForWindow(parent);
#else
    HDC hdc = GetDC(parent);
    if (hdc) {
        dpi = static_cast<UINT>(GetDeviceCaps(hdc, LOGPIXELSX));
        ReleaseDC(parent, hdc);
    }
#endif
    if (dpi == 0) dpi = 96;
    auto scale_px = [dpi](int px) -> int { return MulDiv(px, static_cast<int>(dpi), 96); };

    int dlg_w = scale_px(400), dlg_h = scale_px(230);
    int x = parent_rect.left + (pw - dlg_w) / 2;
    int y = parent_rect.top + (ph - dlg_h) / 2;

    std::string title_str = std::string(i18n::tr(S::kDlgEditTitlePrefix)) + rec.spec.name;
    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kEditDlgClassName, i18n::to_wide(title_str).c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, dlg_w, dlg_h,
        parent, nullptr, GetModuleHandle(nullptr), nullptr);

    if (!dlg) {
        if (error) *error = "Failed to create dialog";
        return false;
    }

    SetWindowSubclass(dlg, EditDlgSubclassProc, 1, reinterpret_cast<DWORD_PTR>(&data));

    RECT rc;
    GetClientRect(dlg, &rc);
    int w = rc.right, h = rc.bottom;
    int margin = scale_px(16);
    int btn_w = scale_px(96);
    int btn_h = scale_px(30);

    auto layout = CalcSliderRowLayout(w, margin, btn_h, scale_px);
    int edit_w = w - layout.edit_x - margin;
    int edit_h = scale_px(26);
    int ctrl_y = margin;

    CreateWindowExW(0, L"STATIC", i18n::tr_w(S::kDlgLabelName).c_str(),
        WS_CHILD | SS_LEFT, margin, ctrl_y + layout.label_y_off, layout.label_w, scale_px(20),
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_ED_NAME_LABEL)),
        nullptr, nullptr);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | ES_AUTOHSCROLL, layout.edit_x, ctrl_y, edit_w, edit_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_ED_NAME)),
        nullptr, nullptr);
    ctrl_y += layout.form_row_h;

    CreateSliderRow(dlg, layout, margin, ctrl_y,
        IDC_ED_MEM_LABEL, i18n::tr_w(S::kDlgLabelMemory).c_str(),
        IDC_ED_MEM_SLIDER, IDC_ED_MEM_VALUE, scale_px);
    ctrl_y += layout.form_row_h;

    CreateSliderRow(dlg, layout, margin, ctrl_y,
        IDC_ED_CPU_LABEL, i18n::tr_w(S::kDlgLabelVcpus).c_str(),
        IDC_ED_CPU_SLIDER, IDC_ED_CPU_VALUE, scale_px);
    ctrl_y += layout.form_row_h;

    CreateWindowExW(0, L"BUTTON", i18n::tr_w(S::kDlgDebugMode).c_str(),
        WS_CHILD | BS_AUTOCHECKBOX, layout.edit_x, ctrl_y + scale_px(4), edit_w, scale_px(22),
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_ED_DEBUG)),
        nullptr, nullptr);
    ctrl_y += layout.form_row_h;

    CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | SS_LEFT, margin, ctrl_y, w - 2 * margin, scale_px(20),
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_ED_WARN)),
        nullptr, nullptr);

    int ok_y = h - margin - btn_h;
    CreateWindowExW(0, L"BUTTON", i18n::tr_w(S::kDlgBtnSave).c_str(),
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, w - margin - btn_w, ok_y, btn_w, btn_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDOK)),
        nullptr, nullptr);

    HFONT ui_font = nullptr;
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfHeight = -MulDiv(10, static_cast<int>(dpi), 72);
        ui_font = CreateFontIndirectW(&lf);
    }
    if (!ui_font) ui_font = reinterpret_cast<HFONT>(SendMessage(parent, WM_GETFONT, 0, 0));
    if (!ui_font) ui_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    EnumChildWindows(dlg, [](HWND child, LPARAM f) -> BOOL {
        SendMessage(child, WM_SETFONT, f, TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(ui_font));

    SetDlgItemTextW(dlg, IDC_ED_NAME, i18n::to_wide(rec.spec.name).c_str());

    int host_mem_gb = GetHostMemoryGb();
    if (host_mem_gb < 1) host_mem_gb = 16;
    int cur_mem_gb = static_cast<int>(rec.spec.memory_mb / 1024);
    if (cur_mem_gb < 1) cur_mem_gb = 1;
    if (cur_mem_gb > host_mem_gb) cur_mem_gb = host_mem_gb;
    InitSlider(dlg, IDC_ED_MEM_SLIDER, IDC_ED_MEM_VALUE, 1, host_mem_gb, cur_mem_gb, true);

    int host_cpus = GetHostLogicalCpus();
    int cur_cpus = static_cast<int>(rec.spec.cpu_count);
    if (cur_cpus < 1) cur_cpus = 1;
    if (cur_cpus > host_cpus) cur_cpus = host_cpus;
    InitSlider(dlg, IDC_ED_CPU_SLIDER, IDC_ED_CPU_VALUE, 1, host_cpus, cur_cpus, false);

    CheckDlgButton(dlg, IDC_ED_DEBUG, rec.spec.debug_mode ? BST_CHECKED : BST_UNCHECKED);

    bool running = rec.state == VmPowerState::kRunning ||
                   rec.state == VmPowerState::kStarting;
    EnableWindow(GetDlgItem(dlg, IDC_ED_MEM_SLIDER), !running);
    EnableWindow(GetDlgItem(dlg, IDC_ED_CPU_SLIDER), !running);
    EnableWindow(GetDlgItem(dlg, IDC_ED_DEBUG), !running);
    if (running)
        SetDlgItemTextW(dlg, IDC_ED_WARN, i18n::tr_w(S::kCpuMemoryChangeWarning).c_str());

    ShowWindow(GetDlgItem(dlg, IDC_ED_WARN), SW_SHOW);
    EnumChildWindows(dlg, [](HWND child, LPARAM) -> BOOL {
        if (!(GetWindowLong(child, GWL_STYLE) & WS_VISIBLE))
            ShowWindow(child, SW_SHOW);
        return TRUE;
    }, 0);

    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    MSG msg;
    while (IsWindow(dlg) && GetMessage(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessage(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    if (ui_font && ui_font != reinterpret_cast<HFONT>(SendMessage(parent, WM_GETFONT, 0, 0))
        && ui_font != static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT))) {
        DeleteObject(ui_font);
    }

    EnableWindow(parent, TRUE);

    if (error) *error = data.error;
    return data.saved;
}
