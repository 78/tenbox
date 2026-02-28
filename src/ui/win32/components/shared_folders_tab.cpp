#include "ui/win32/components/shared_folders_tab.h"
#include "manager/manager_service.h"
#include "ui/common/i18n.h"

void SharedFoldersTab::Create(HWND parent, HINSTANCE hinst, HFONT ui_font) {
    // Create ListView for shared folders
    listview_ = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
        WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(kListViewId), hinst, nullptr);
    ListView_SetExtendedListViewStyle(listview_, 
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    SendMessage(listview_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font), FALSE);

    // Setup columns
    LVCOLUMNA col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 200;
    col.pszText = const_cast<char*>(i18n::tr(i18n::S::kSfColTag));
    ListView_InsertColumn(listview_, 0, &col);
    col.cx = 560;
    col.pszText = const_cast<char*>(i18n::tr(i18n::S::kSfColHostPath));
    ListView_InsertColumn(listview_, 1, &col);
    col.cx = 140;
    col.pszText = const_cast<char*>(i18n::tr(i18n::S::kSfColMode));
    ListView_InsertColumn(listview_, 2, &col);

    // Create buttons
    add_btn_ = CreateWindowExA(0, "BUTTON", i18n::tr(i18n::S::kSfBtnAdd),
        WS_CHILD | BS_PUSHBUTTON,
        0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(kAddButtonId), hinst, nullptr);
    del_btn_ = CreateWindowExA(0, "BUTTON", i18n::tr(i18n::S::kSfBtnRemove),
        WS_CHILD | BS_PUSHBUTTON,
        0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(kRemoveButtonId), hinst, nullptr);
    
    SendMessage(add_btn_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font), FALSE);
    SendMessage(del_btn_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font), FALSE);
}

void SharedFoldersTab::Show(bool visible) {
    int cmd = visible ? SW_SHOW : SW_HIDE;
    ShowWindow(listview_, cmd);
    ShowWindow(add_btn_, cmd);
    ShowWindow(del_btn_, cmd);
}

void SharedFoldersTab::Layout(int px, int py, int pw, int ph) {
    int btn_h = 26;
    int btn_w = 80;
    int gap = 4;
    int list_h = ph - btn_h - gap;
    if (list_h < 50) list_h = 50;

    MoveWindow(listview_, px, py, pw, list_h, TRUE);
    int btn_y = py + list_h + gap;
    MoveWindow(add_btn_, px, btn_y, btn_w, btn_h, TRUE);
    MoveWindow(del_btn_, px + btn_w + gap, btn_y, btn_w, btn_h, TRUE);
}

void SharedFoldersTab::Refresh(ManagerService& manager, const std::string& vm_id) {
    if (!listview_) return;
    
    ListView_DeleteAllItems(listview_);
    
    if (vm_id.empty()) return;
    
    auto folders = manager.GetSharedFolders(vm_id);
    
    for (size_t i = 0; i < folders.size(); ++i) {
        const auto& sf = folders[i];
        
        LVITEMA item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.iSubItem = 0;
        item.pszText = const_cast<char*>(sf.tag.c_str());
        int idx = ListView_InsertItem(listview_, &item);
        
        ListView_SetItemText(listview_, idx, 1, const_cast<char*>(sf.host_path.c_str()));
        ListView_SetItemText(listview_, idx, 2, 
            const_cast<char*>(sf.readonly ? i18n::tr(i18n::S::kSfModeReadOnly) : i18n::tr(i18n::S::kSfModeReadWrite)));
    }
}

bool SharedFoldersTab::HandleCommand(HWND hwnd, UINT cmd, UINT code, 
                                      ManagerService& manager,
                                      const std::string& vm_id) {
    if (cmd == kAddButtonId && code == BN_CLICKED) {
        // Use folder browser dialog
        BROWSEINFOA bi = {};
        bi.hwndOwner = hwnd;
        bi.lpszTitle = i18n::tr(i18n::S::kSfBrowseTitle);
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        
        LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
        if (pidl) {
            char path_buf[MAX_PATH] = {};
            SHGetPathFromIDListA(pidl, path_buf);
            CoTaskMemFree(pidl);
            
            // Generate a default tag from folder name
            std::string path_str(path_buf);
            size_t last_sep = path_str.find_last_of("\\/");
            std::string default_tag = (last_sep != std::string::npos) 
                ? path_str.substr(last_sep + 1) : "share";
            
            if (default_tag.empty()) {
                default_tag = "share";
            }
            
            // Add the shared folder
            SharedFolder sf;
            sf.tag = default_tag;
            sf.host_path = path_buf;
            sf.readonly = false;
            
            std::string error;
            if (manager.AddSharedFolder(vm_id, sf, &error)) {
                Refresh(manager, vm_id);
            } else {
                MessageBoxA(hwnd, error.c_str(), i18n::tr(i18n::S::kError), 
                           MB_OK | MB_ICONERROR);
            }
        }
        return true;
    }
    
    if (cmd == kRemoveButtonId && code == BN_CLICKED) {
        int sel = ListView_GetNextItem(listview_, -1, LVNI_SELECTED);
        if (sel < 0) {
            MessageBoxA(hwnd, i18n::tr(i18n::S::kSfNoSelection),
                       i18n::tr(i18n::S::kError), MB_OK | MB_ICONWARNING);
            return true;
        }
        
        char tag_buf[64] = {};
        ListView_GetItemText(listview_, sel, 0, tag_buf, sizeof(tag_buf));
        
        std::string prompt = i18n::fmt(i18n::S::kSfConfirmRemoveMsg, tag_buf);
        if (MessageBoxA(hwnd, prompt.c_str(), i18n::tr(i18n::S::kSfConfirmRemoveTitle),
                MB_YESNO | MB_ICONQUESTION) == IDYES) {
            std::string error;
            if (manager.RemoveSharedFolder(vm_id, tag_buf, &error)) {
                Refresh(manager, vm_id);
            } else {
                MessageBoxA(hwnd, error.c_str(), i18n::tr(i18n::S::kError), 
                           MB_OK | MB_ICONERROR);
            }
        }
        return true;
    }
    
    return false;
}
