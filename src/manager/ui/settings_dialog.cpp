#include "manager/ui/settings_dialog.h"
#include "manager/ui/dlg_builder.h"
#include "manager/i18n.h"
#include "manager/app_settings.h"
#include "manager/image_source.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cinttypes>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

enum SettingsDlgId {
    IDC_VM_DIR_LABEL     = 400,
    IDC_VM_DIR_EDIT      = 401,
    IDC_VM_DIR_BROWSE    = 402,
    IDC_VM_DIR_RESET     = 403,
    IDC_CACHE_DIR_LABEL  = 410,
    IDC_CACHE_DIR_EDIT   = 411,
    IDC_CACHE_DIR_BROWSE = 412,
    IDC_CACHE_DIR_RESET  = 413,
    IDC_CACHE_SIZE_LABEL = 420,
    IDC_CACHE_CLEAR      = 421,
};

struct SettingsDlgData {
    ManagerService* mgr;
    bool changed;
};

static std::string FormatSize(uint64_t bytes) {
    char buf[64];
    if (bytes >= uint64_t(1) << 30)
        snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    else if (bytes >= uint64_t(1) << 20)
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
    else
        snprintf(buf, sizeof(buf), "%" PRIu64 " B", bytes);
    return buf;
}

struct CacheStats {
    uint64_t total_bytes = 0;
    int image_count = 0;
};

static CacheStats ComputeCacheStats(const std::string& images_dir) {
    CacheStats stats;
    if (images_dir.empty()) return stats;
    std::error_code ec;
    if (!fs::exists(images_dir, ec) || !fs::is_directory(images_dir, ec))
        return stats;
    for (const auto& entry : fs::directory_iterator(images_dir, ec)) {
        if (!entry.is_directory(ec)) continue;
        bool has_meta = fs::exists(entry.path() / "image.json", ec);
        if (!has_meta) continue;
        ++stats.image_count;
        for (const auto& f : fs::recursive_directory_iterator(entry, ec)) {
            if (f.is_regular_file(ec))
                stats.total_bytes += f.file_size(ec);
        }
    }
    return stats;
}

static std::string EffectiveImagesDir(ManagerService* mgr) {
    return settings::EffectiveImageCacheDir(mgr->app_settings(), mgr->data_dir());
}

static std::string EffectiveVmDir(ManagerService* mgr) {
    return settings::EffectiveVmStorageDir(mgr->app_settings());
}

static void SaveAndMark(SettingsDlgData* data) {
    data->mgr->SaveAppSettings();
    data->changed = true;
}

static void UpdateCacheSizeLabel(HWND dlg, ManagerService* mgr) {
    auto stats = ComputeCacheStats(EffectiveImagesDir(mgr));
    auto size_str = FormatSize(stats.total_bytes);
    auto label = i18n::fmt(i18n::S::kSettingsCacheSize, size_str.c_str(), stats.image_count);
    SetDlgItemTextW(dlg, IDC_CACHE_SIZE_LABEL, i18n::to_wide(label).c_str());
}

static void UpdateEditDisplays(HWND dlg, ManagerService* mgr) {
    SetDlgItemTextW(dlg, IDC_VM_DIR_EDIT,
                    i18n::to_wide(EffectiveVmDir(mgr)).c_str());
    SetDlgItemTextW(dlg, IDC_CACHE_DIR_EDIT,
                    i18n::to_wide(EffectiveImagesDir(mgr)).c_str());
}

static bool MigrateCache(const std::string& src_dir, const std::string& dst_dir) {
    std::error_code ec;
    if (!fs::exists(src_dir, ec) || !fs::is_directory(src_dir, ec))
        return true;
    fs::create_directories(dst_dir, ec);
    if (ec) return false;

    for (const auto& entry : fs::directory_iterator(src_dir, ec)) {
        if (!entry.is_directory(ec)) continue;
        auto dest = fs::path(dst_dir) / entry.path().filename();
        if (fs::exists(dest, ec)) continue;
        fs::rename(entry.path(), dest, ec);
        if (ec) {
            // Cross-volume: fall back to copy + delete
            ec.clear();
            fs::copy(entry.path(), dest,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (!ec)
                fs::remove_all(entry.path(), ec);
        }
    }
    if (fs::is_empty(src_dir, ec))
        fs::remove(src_dir, ec);
    return true;
}

// Prompt user to migrate or delete old cache after a path change.
static void HandleOldCache(HWND dlg, const std::string& old_dir, const std::string& new_dir) {
    if (old_dir.empty() || new_dir.empty() || old_dir == new_dir)
        return;

    auto stats = ComputeCacheStats(old_dir);
    if (stats.image_count == 0)
        return;

    auto size_str = FormatSize(stats.total_bytes);
    auto msg = i18n::fmt(i18n::S::kSettingsOldCacheMsg, old_dir.c_str(), size_str.c_str());
    auto title_w = i18n::tr_w(i18n::S::kSettingsOldCacheTitle);

    // YES = Migrate, NO = Delete
    int result = MessageBoxW(dlg, i18n::to_wide(msg).c_str(),
                             title_w.c_str(),
                             MB_YESNO | MB_ICONQUESTION);
    if (result == IDYES) {
        MigrateCache(old_dir, new_dir);
    } else {
        std::error_code ec;
        fs::remove_all(old_dir, ec);
    }
}

static INT_PTR CALLBACK SettingsDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<SettingsDlgData*>(GetWindowLongPtrW(dlg, DWLP_USER));

    switch (msg) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<SettingsDlgData*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(data));
        CenterDialogToParent(dlg);
        UpdateEditDisplays(dlg, data->mgr);
        UpdateCacheSizeLabel(dlg, data->mgr);

        SendDlgItemMessageW(dlg, IDC_VM_DIR_EDIT, EM_SETREADONLY, TRUE, 0);
        SendDlgItemMessageW(dlg, IDC_CACHE_DIR_EDIT, EM_SETREADONLY, TRUE, 0);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_VM_DIR_BROWSE: {
            auto path = BrowseForFolder(dlg, i18n::tr(i18n::S::kSettingsVmStorageDir),
                                        EffectiveVmDir(data->mgr).c_str());
            if (!path.empty()) {
                data->mgr->app_settings().vm_storage_dir = path;
                SaveAndMark(data);
                UpdateEditDisplays(dlg, data->mgr);
            }
            return TRUE;
        }
        case IDC_VM_DIR_RESET:
            data->mgr->app_settings().vm_storage_dir.clear();
            SaveAndMark(data);
            UpdateEditDisplays(dlg, data->mgr);
            return TRUE;

        case IDC_CACHE_DIR_BROWSE: {
            std::string old_dir = EffectiveImagesDir(data->mgr);
            auto path = BrowseForFolder(dlg, i18n::tr(i18n::S::kSettingsImageCacheDir),
                                        old_dir.c_str());
            if (!path.empty() && path != old_dir) {
                data->mgr->app_settings().image_cache_dir = path;
                SaveAndMark(data);
                HandleOldCache(dlg, old_dir, path);
                UpdateEditDisplays(dlg, data->mgr);
                UpdateCacheSizeLabel(dlg, data->mgr);
            }
            return TRUE;
        }
        case IDC_CACHE_DIR_RESET: {
            std::string old_dir = EffectiveImagesDir(data->mgr);
            std::string default_dir = settings::DefaultImageCacheDir(data->mgr->data_dir());
            if (old_dir != default_dir) {
                data->mgr->app_settings().image_cache_dir.clear();
                SaveAndMark(data);
                HandleOldCache(dlg, old_dir, default_dir);
            }
            UpdateEditDisplays(dlg, data->mgr);
            UpdateCacheSizeLabel(dlg, data->mgr);
            return TRUE;
        }

        case IDC_CACHE_CLEAR: {
            auto dir = EffectiveImagesDir(data->mgr);
            auto stats = ComputeCacheStats(dir);
            if (stats.image_count == 0) {
                UpdateCacheSizeLabel(dlg, data->mgr);
                return TRUE;
            }
            auto size_str = FormatSize(stats.total_bytes);
            auto prompt = i18n::fmt(i18n::S::kSettingsConfirmClearMsg, size_str.c_str());
            if (MessageBoxW(dlg, i18n::to_wide(prompt).c_str(),
                            i18n::tr_w(i18n::S::kSettingsConfirmClearTitle).c_str(),
                            MB_YESNO | MB_ICONWARNING) == IDYES) {
                std::error_code ec;
                for (const auto& entry : fs::directory_iterator(dir, ec)) {
                    if (entry.is_directory(ec))
                        fs::remove_all(entry.path(), ec);
                }
                UpdateCacheSizeLabel(dlg, data->mgr);
                MessageBoxW(dlg, i18n::tr_w(i18n::S::kSettingsCacheCleared).c_str(),
                            i18n::tr_w(i18n::S::kSettingsConfirmClearTitle).c_str(),
                            MB_OK | MB_ICONINFORMATION);
            }
            return TRUE;
        }
        }
        break;

    case WM_CLOSE:
        EndDialog(dlg, 0);
        return TRUE;
    }
    return FALSE;
}

bool ShowSettingsDialog(HWND parent, ManagerService& mgr) {
    using S = i18n::S;
    DlgBuilder b;

    int W = 340, H = 170;
    b.Begin(i18n::tr(S::kDlgSettings), 0, 0, W, H,
            WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT);

    int x = 8, y = 8;
    int edit_w = W - 16 - 50 - 40 - 8;
    int btn_w = 42, reset_w = 36, btn_h = 14, row_h = 16;

    // VM Storage Directory
    b.AddStatic(IDC_VM_DIR_LABEL, i18n::tr(S::kSettingsVmStorageDir), x, y, W - 16, 10);
    y += 12;
    b.AddEdit(IDC_VM_DIR_EDIT, x, y, edit_w, 13);
    b.AddButton(IDC_VM_DIR_BROWSE, i18n::tr(S::kSettingsBrowse),
                x + edit_w + 3, y, btn_w, btn_h);
    b.AddButton(IDC_VM_DIR_RESET, i18n::tr(S::kSettingsReset),
                x + edit_w + 3 + btn_w + 3, y, reset_w, btn_h);
    y += row_h + 6;

    // Image Cache Directory
    b.AddStatic(IDC_CACHE_DIR_LABEL, i18n::tr(S::kSettingsImageCacheDir), x, y, W - 16, 10);
    y += 12;
    b.AddEdit(IDC_CACHE_DIR_EDIT, x, y, edit_w, 13);
    b.AddButton(IDC_CACHE_DIR_BROWSE, i18n::tr(S::kSettingsBrowse),
                x + edit_w + 3, y, btn_w, btn_h);
    b.AddButton(IDC_CACHE_DIR_RESET, i18n::tr(S::kSettingsReset),
                x + edit_w + 3 + btn_w + 3, y, reset_w, btn_h);
    y += row_h + 6;

    // Cache size label + Clear Cache button, left-aligned together
    int clear_btn_w = 56;
    b.AddStatic(IDC_CACHE_SIZE_LABEL, "", x, y + 2, 160, 10);
    b.AddButton(IDC_CACHE_CLEAR, i18n::tr(S::kSettingsClearCache),
                x + 162, y, clear_btn_w, btn_h);

    SettingsDlgData data{};
    data.mgr = &mgr;
    data.changed = false;

    DialogBoxIndirectParamW(GetModuleHandle(nullptr), b.Build(), parent,
                            SettingsDlgProc, reinterpret_cast<LPARAM>(&data));
    return data.changed;
}
