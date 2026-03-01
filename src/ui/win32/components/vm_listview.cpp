#include "ui/win32/components/vm_listview.h"
#include "manager/manager_service.h"
#include "ui/common/i18n.h"

#include <windowsx.h>

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

// ── Create ──

void VmListView::Create(HWND parent, HINSTANCE hinst, HFONT ui_font) {
    hwnd_ = CreateWindowExA(
        WS_EX_CLIENTEDGE, WC_LISTVIEWA, nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
        LVS_OWNERDATA | LVS_NOCOLUMNHEADER,
        0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kControlId)), hinst, nullptr);

    ListView_SetExtendedListViewStyle(hwnd_,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    // Disable the built-in tooltip / infotip that draws garbled text on hover.
    HWND tip = ListView_GetToolTips(hwnd_);
    if (tip) {
        DestroyWindow(tip);
        ListView_SetToolTips(hwnd_, nullptr);
    }

    // Force row height to kItemHeight via a small-state ImageList trick.
    HIMAGELIST hil = ImageList_Create(1, kItemHeight, ILC_COLOR32, 1, 0);
    ListView_SetImageList(hwnd_, hil, LVSIL_SMALL);

    LVCOLUMNA col{};
    col.mask = LVCF_WIDTH;
    col.cx = 0;
    SendMessageA(hwnd_, LVM_INSERTCOLUMNA, 0, reinterpret_cast<LPARAM>(&col));

    SendMessage(hwnd_, WM_SETFONT,
        reinterpret_cast<WPARAM>(ui_font), FALSE);
}

// ── UpdateColumnWidth ──

void VmListView::UpdateColumnWidth() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    ListView_SetColumnWidth(hwnd_, 0, rc.right - rc.left);
}

// ── Populate ──

void VmListView::Populate(const std::vector<VmRecord>& records, int selected_index) {
    SendMessageA(hwnd_, WM_SETREDRAW, FALSE, 0);

    ListView_SetItemCountEx(hwnd_, static_cast<int>(records.size()),
        LVSICF_NOINVALIDATEALL);

    // Clear previous selection
    int count = ListView_GetItemCount(hwnd_);
    for (int i = 0; i < count; ++i) {
        ListView_SetItemState(hwnd_, i, 0, LVIS_SELECTED | LVIS_FOCUSED);
    }

    if (selected_index >= 0 && selected_index < static_cast<int>(records.size())) {
        ListView_SetItemState(hwnd_, selected_index,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(hwnd_, selected_index, FALSE);
    }

    SendMessageA(hwnd_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

// ── Custom draw helper ──

void VmListView::DrawItem(HDC hdc, const RECT& rc, const VmRecord& rec,
                          bool selected, HFONT font) {
    COLORREF card_bg = selected ? RGB(229, 241, 255) : RGB(248, 248, 248);
    COLORREF fg  = selected ? RGB(20, 20, 20) : GetSysColor(COLOR_WINDOWTEXT);
    COLORREF dim = selected ? RGB(80, 80, 80) : GetSysColor(COLOR_GRAYTEXT);

    HBRUSH wnd_br = GetSysColorBrush(COLOR_WINDOW);
    FillRect(hdc, &rc, wnd_br);

    RECT card = rc;
    card.left   += 4;
    card.right  -= 4;
    card.top    += 3;
    card.bottom -= 3;

    HBRUSH card_br = CreateSolidBrush(card_bg);
    HPEN border_pen = CreatePen(PS_SOLID, 1,
        selected ? RGB(100, 160, 230) : RGB(232, 232, 232));
    HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, border_pen));
    HBRUSH old_br = static_cast<HBRUSH>(SelectObject(hdc, card_br));
    RoundRect(hdc, card.left, card.top, card.right, card.bottom, 6, 6);
    SelectObject(hdc, old_br);
    SelectObject(hdc, old_pen);
    DeleteObject(card_br);
    DeleteObject(border_pen);

    SetBkMode(hdc, TRANSPARENT);

    HFONT old_font = static_cast<HFONT>(SelectObject(hdc, font));

    TEXTMETRICA tm{};
    GetTextMetricsA(hdc, &tm);
    int line_h = tm.tmHeight + tm.tmExternalLeading;

    int x = card.left + 12;
    int y = card.top + 8;

    SetTextColor(hdc, fg);
    auto name_w = i18n::to_wide(rec.spec.name);
    TextOutW(hdc, x, y, name_w.c_str(), static_cast<int>(name_w.size()));

    SIZE name_sz{};
    GetTextExtentPoint32W(hdc, name_w.c_str(),
        static_cast<int>(name_w.size()), &name_sz);

    auto state_w = i18n::to_wide(StateText(rec.state));
    COLORREF state_color;
    if (rec.state == VmPowerState::kRunning)
        state_color = RGB(0, 128, 0);
    else if (rec.state == VmPowerState::kCrashed)
        state_color = RGB(200, 0, 0);
    else
        state_color = dim;
    SetTextColor(hdc, state_color);
    TextOutW(hdc, x + name_sz.cx + 12, y, state_w.c_str(),
             static_cast<int>(state_w.size()));

    y += line_h + 2;
    SelectObject(hdc, font);
    SetTextColor(hdc, dim);
    auto detail = i18n::fmt(i18n::S::kDetailVcpuRam,
        static_cast<unsigned>(rec.spec.cpu_count), static_cast<unsigned>(rec.spec.memory_mb));
    auto detail_w = i18n::to_wide(detail);
    TextOutW(hdc, x, y, detail_w.c_str(), static_cast<int>(detail_w.size()));

    SelectObject(hdc, old_font);
}

// ── HandleNotify ──

bool VmListView::HandleNotify(NMHDR* nmhdr, const std::vector<VmRecord>& records,
                              HFONT ui_font, LRESULT* result) {
    if (nmhdr->idFrom != kControlId)
        return false;

    switch (nmhdr->code) {
    case LVN_GETDISPINFOA: {
        auto* di = reinterpret_cast<NMLVDISPINFOA*>(nmhdr);
        if (di->item.mask & LVIF_TEXT) {
            di->item.pszText[0] = '\0';
        }
        if (di->item.mask & LVIF_IMAGE) {
            di->item.iImage = -1;
        }
        *result = 0;
        return true;
    }

    case NM_CUSTOMDRAW: {
        auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(nmhdr);
        switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            *result = CDRF_NOTIFYITEMDRAW;
            return true;
        case CDDS_ITEMPREPAINT: {
            int idx = static_cast<int>(cd->nmcd.dwItemSpec);
            if (idx >= 0 && idx < static_cast<int>(records.size())) {
                bool sel = (ListView_GetItemState(hwnd_, idx, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                RECT rc = cd->nmcd.rc;
                rc.bottom = rc.top + kItemHeight;
                DrawItem(cd->nmcd.hdc, rc, records[idx], sel, ui_font);
            }
            *result = CDRF_SKIPDEFAULT;
            return true;
        }
        default:
            *result = CDRF_SKIPDEFAULT;
            return true;
        }
    }

    case LVN_GETINFOTIPA:
    case LVN_GETINFOTIPW:
        *result = 0;
        return true;

    case LVN_BEGINDRAG: {
        auto* nmlv = reinterpret_cast<NMLISTVIEW*>(nmhdr);
        drag_index_ = nmlv->iItem;
        if (drag_index_ < 0 || drag_index_ >= static_cast<int>(records.size()))
            break;

        RECT client_rc{};
        GetClientRect(hwnd_, &client_rc);
        int w = client_rc.right - client_rc.left;
        int h = kItemHeight;
        if (w <= 0) w = 252;

        RECT item_rc{};
        ListView_GetItemRect(hwnd_, drag_index_, &item_rc, LVIR_BOUNDS);

        HDC screen_dc = GetDC(hwnd_);
        HDC mem_dc = CreateCompatibleDC(screen_dc);
        HBITMAP bmp = CreateCompatibleBitmap(screen_dc, w, h);
        HBITMAP old_bmp = static_cast<HBITMAP>(SelectObject(mem_dc, bmp));

        RECT draw_rc = { 0, 0, w, h };
        DrawItem(mem_dc, draw_rc, records[drag_index_], true, ui_font);

        SelectObject(mem_dc, old_bmp);
        DeleteDC(mem_dc);
        ReleaseDC(hwnd_, screen_dc);

        drag_image_ = ImageList_Create(w, h, ILC_COLOR32 | ILC_MASK, 1, 0);
        ImageList_Add(drag_image_, bmp, nullptr);
        DeleteObject(bmp);

        POINT cursor;
        GetCursorPos(&cursor);
        ScreenToClient(hwnd_, &cursor);
        int hotspot_x = cursor.x - item_rc.left;
        int hotspot_y = cursor.y - item_rc.top;

        ImageList_BeginDrag(drag_image_, 0, hotspot_x, hotspot_y);
        ClientToScreen(hwnd_, &cursor);
        ImageList_DragEnter(GetDesktopWindow(), cursor.x, cursor.y);

        SetCapture(GetParent(hwnd_));
        dragging_ = true;
        *result = 0;
        return true;
    }

    default:
        break;
    }
    return false;
}

// ── Drop marker ──

void VmListView::DrawDropMarker(int target_index) {
    if (target_index == drop_marker_) return;
    ClearDropMarker();
    drop_marker_ = target_index;
    if (drop_marker_ < 0) return;

    RECT rc{};
    if (ListView_GetItemRect(hwnd_, drop_marker_, &rc, LVIR_BOUNDS)) {
        int y = (drop_marker_ > drag_index_) ? rc.top + kItemHeight - 1 : rc.top;

        ImageList_DragShowNolock(FALSE);
        HDC hdc = GetDC(hwnd_);
        RECT client_rc{};
        GetClientRect(hwnd_, &client_rc);
        HPEN pen = CreatePen(PS_SOLID, 3, RGB(60, 130, 220));
        HPEN old = static_cast<HPEN>(SelectObject(hdc, pen));
        MoveToEx(hdc, client_rc.left + 4, y, nullptr);
        LineTo(hdc, client_rc.right - 4, y);
        SelectObject(hdc, old);
        DeleteObject(pen);
        ReleaseDC(hwnd_, hdc);
        ImageList_DragShowNolock(TRUE);
    }
}

void VmListView::ClearDropMarker() {
    if (drop_marker_ < 0) return;

    RECT rc{};
    if (ListView_GetItemRect(hwnd_, drop_marker_, &rc, LVIR_BOUNDS)) {
        int y = (drop_marker_ > drag_index_) ? rc.top + kItemHeight - 1 : rc.top;
        RECT inv = { rc.left, y - 2, rc.right, y + 2 };

        ImageList_DragShowNolock(FALSE);
        InvalidateRect(hwnd_, &inv, TRUE);
        UpdateWindow(hwnd_);
        ImageList_DragShowNolock(TRUE);
    }
    drop_marker_ = -1;
}

// ── Drag move / end ──

bool VmListView::HandleDragMove(LPARAM lp) {
    if (!dragging_) return false;

    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    ClientToScreen(GetParent(hwnd_), &pt);
    if (drag_image_) {
        ImageList_DragMove(pt.x, pt.y);
    }

    POINT lv_pt = pt;
    ScreenToClient(hwnd_, &lv_pt);
    LVHITTESTINFO hti{};
    hti.pt = lv_pt;
    int target = ListView_HitTest(hwnd_, &hti);
    if (target >= 0 && target != drag_index_) {
        DrawDropMarker(target);
    } else {
        ClearDropMarker();
    }

    return true;
}

bool VmListView::HandleDragEnd(LPARAM lp) {
    if (!dragging_) return false;

    ClearDropMarker();

    if (drag_image_) {
        ImageList_DragLeave(GetDesktopWindow());
        ImageList_EndDrag();
        ImageList_Destroy(drag_image_);
        drag_image_ = nullptr;
    }
    ReleaseCapture();
    dragging_ = false;

    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    MapWindowPoints(GetParent(hwnd_), hwnd_, &pt, 1);

    LVHITTESTINFO hti{};
    hti.pt = pt;
    int drop_index = ListView_HitTest(hwnd_, &hti);

    if (drop_index >= 0 && drop_index != drag_index_ && drag_drop_cb_) {
        drag_drop_cb_(drag_index_, drop_index);
    }

    drag_index_ = -1;
    return true;
}
