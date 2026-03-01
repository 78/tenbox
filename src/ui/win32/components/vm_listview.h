#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

#include <functional>
#include <vector>

struct VmRecord;

// ListView-based VM list with drag-and-drop reordering support.
class VmListView {
public:
    static constexpr UINT kControlId = 2003;
    static constexpr int kItemHeight = 80;

    using DragDropCallback = std::function<void(int, int)>;

    VmListView() = default;
    ~VmListView() = default;

    void Create(HWND parent, HINSTANCE hinst, HFONT ui_font);
    HWND handle() const { return hwnd_; }

    // Resize the single column to fill the client area (call after MoveWindow).
    void UpdateColumnWidth();

    void Populate(const std::vector<VmRecord>& records, int selected_index);

    // Handle WM_NOTIFY from the ListView. Returns true if handled.
    bool HandleNotify(NMHDR* nmhdr, const std::vector<VmRecord>& records,
                      HFONT ui_font, LRESULT* result);

    // Forward WM_MOUSEMOVE during drag. Returns true if in drag mode.
    bool HandleDragMove(LPARAM lp);

    // Forward WM_LBUTTONUP to finish drag. Returns true if drag was active.
    bool HandleDragEnd(LPARAM lp);

    void SetDragDropCallback(DragDropCallback cb) { drag_drop_cb_ = std::move(cb); }

private:
    HWND hwnd_ = nullptr;
    HIMAGELIST drag_image_ = nullptr;
    int drag_index_ = -1;
    int drop_marker_ = -1;
    bool dragging_ = false;
    DragDropCallback drag_drop_cb_;

    void DrawItem(HDC hdc, const RECT& rc, const VmRecord& rec,
                  bool selected, HFONT font);
    void DrawDropMarker(int target_index);
    void ClearDropMarker();
};
