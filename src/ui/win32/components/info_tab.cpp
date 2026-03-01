#include "ui/win32/components/info_tab.h"
#include "common/vm_model.h"
#include "ui/common/i18n.h"

#include <cstring>
#include <ctime>
#include <string>

void InfoTab::Create(HWND parent, HINSTANCE hinst, HFONT ui_font) {
    for (int i = 0; i < kDetailRows; ++i) {
        labels_[i] = CreateWindowExA(0, "STATIC", "",
            WS_CHILD | SS_RIGHT,
            0, 0, 0, 0, parent, nullptr, hinst, nullptr);
        values_[i] = CreateWindowExA(0, "EDIT", "",
            WS_CHILD | ES_READONLY | ES_AUTOHSCROLL,
            0, 0, 0, 0, parent, nullptr, hinst, nullptr);
        SendMessage(labels_[i], WM_SETFONT,
            reinterpret_cast<WPARAM>(ui_font), FALSE);
        SendMessage(values_[i], WM_SETFONT,
            reinterpret_cast<WPARAM>(ui_font), FALSE);
    }
}

void InfoTab::Show(bool visible) {
    int cmd = visible ? SW_SHOW : SW_HIDE;
    for (int i = 0; i < kDetailRows; ++i) {
        ShowWindow(labels_[i], cmd);
        ShowWindow(values_[i], cmd);
    }
}

void InfoTab::Layout(HWND hwnd, HFONT ui_font, int px, int py, int pw, int ph) {
    (void)ph;

    HDC hdc = GetDC(hwnd);
    HFONT old_font = static_cast<HFONT>(SelectObject(hdc, ui_font));
    TEXTMETRICA tm{};
    GetTextMetricsA(hdc, &tm);

    const char* kLabels[] = {
        i18n::tr(i18n::S::kLabelId), i18n::tr(i18n::S::kLabelLocation),
        i18n::tr(i18n::S::kLabelMemory), i18n::tr(i18n::S::kLabelVcpus),
        i18n::tr(i18n::S::kLabelNat),
        i18n::tr(i18n::S::kLabelCreatedTime), i18n::tr(i18n::S::kLabelLastBootTime)
    };
    int label_w = 0;
    for (const char* lbl : kLabels) {
        SIZE sz{};
        GetTextExtentPoint32A(hdc, lbl, static_cast<int>(strlen(lbl)), &sz);
        if (sz.cx > label_w) label_w = sz.cx;
    }
    label_w += 12;
    SelectObject(hdc, old_font);
    ReleaseDC(hwnd, hdc);

    int row_h = tm.tmHeight + tm.tmExternalLeading + 2;
    int row_gap = row_h + 6;
    int val_x = px + 8 + label_w + 8;
    int val_w = pw - (label_w + 24);
    if (val_w < 40) val_w = 40;

    int dy = py + 8;
    for (int i = 0; i < kDetailRows; ++i) {
        MoveWindow(labels_[i], px + 8, dy, label_w, row_h, TRUE);
        MoveWindow(values_[i], val_x, dy, val_w, row_h, TRUE);
        dy += row_gap;
    }
}

namespace {

std::string FormatTimestamp(int64_t ts) {
    if (ts <= 0) return "";
    time_t t = static_cast<time_t>(ts);
    struct tm tm_buf {};
#ifdef _WIN32
    if (localtime_s(&tm_buf, &t) != 0) return "";
#else
    if (!localtime_r(&t, &tm_buf)) return "";
#endif
    char buf[32];
    size_t n = strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_buf);
    return n > 0 ? std::string(buf, n) : "";
}

}  // namespace

void InfoTab::Update(const VmSpec* spec) {
    using S = i18n::S;
    const char* label_texts[] = {
        i18n::tr(S::kLabelId), i18n::tr(S::kLabelLocation),
        i18n::tr(S::kLabelMemory), i18n::tr(S::kLabelVcpus),
        i18n::tr(S::kLabelNat),
        i18n::tr(S::kLabelCreatedTime), i18n::tr(S::kLabelLastBootTime)
    };
    for (int i = 0; i < kDetailRows; ++i)
        SetWindowTextA(labels_[i], label_texts[i]);

    if (!spec) {
        for (int i = 0; i < kDetailRows; ++i)
            SetWindowTextA(values_[i], "");
        return;
    }

    auto mb_str = std::to_string(spec->memory_mb) + " MB";
    auto cpu_str = std::to_string(spec->cpu_count);

    SetWindowTextA(values_[0], spec->vm_id.c_str());
    SetWindowTextA(values_[1], spec->vm_dir.c_str());
    SetWindowTextA(values_[2], mb_str.c_str());
    SetWindowTextA(values_[3], cpu_str.c_str());
    SetWindowTextA(values_[4], spec->nat_enabled ? i18n::tr(S::kNatEnabled) : i18n::tr(S::kNatDisabled));
    SetWindowTextA(values_[5], FormatTimestamp(spec->creation_time).c_str());
    SetWindowTextA(values_[6], FormatTimestamp(spec->last_boot_time).c_str());
}
