#include "manager/ui/create_vm_dialog.h"
#include "manager/ui/dlg_builder.h"
#include "manager/i18n.h"
#include "manager/vm_forms.h"
#include "manager/app_settings.h"
#include "manager/image_source.h"
#include "manager/http_download.h"
#include "version.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>

#include <atomic>
#include <functional>
#include <iterator>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static int g_host_memory_gb = 0;
static int g_host_cpus = 0;

// Process-lifetime caches
static std::mutex g_cache_mutex;
static std::vector<image_source::ImageEntry> g_cached_online_images;
static bool g_online_images_loaded = false;
static int g_online_images_source_index = -1;

enum {
    IDC_SOURCE_LABEL = 100,
    IDC_SOURCE_COMBO = 101,
    IDC_BTN_LOAD = 102,
    IDC_IMAGE_LIST = 103,
    IDC_STATUS_TEXT = 105,
    IDC_PROGRESS = 106,
    IDC_PROGRESS_TEXT = 107,
    IDC_NAME_LABEL = 108,
    IDC_NAME_EDIT = 109,
    IDC_MEMORY_LABEL = 110,
    IDC_MEMORY_SLIDER = 111,
    IDC_MEMORY_VALUE = 119,
    IDC_CPU_LABEL = 112,
    IDC_CPU_SLIDER = 113,
    IDC_CPU_VALUE = 120,
    IDC_DEBUG_CHECK = 114,
    IDC_BTN_BACK = 115,
    IDC_BTN_NEXT = 116,
    IDC_BTN_DELETE_CACHE = 118,
    IDC_BTN_LOCAL_IMAGE = 121,
};

enum class Page {
    kSelectImage,
    kDownloading,
    kConfirm
};

enum {
    WM_FETCH_COMPLETE = WM_USER + 101,
    WM_DOWNLOAD_PROGRESS = WM_USER + 102,
    WM_DOWNLOAD_COMPLETE = WM_USER + 103,
};

struct DialogData {
    ManagerService* mgr;
    HWND dlg;
    Page current_page;
    bool created;
    bool closed;
    std::string error;

    std::vector<image_source::ImageSource> sources;
    int selected_source_index;

    std::vector<image_source::ImageEntry> cached_images;
    std::vector<image_source::ImageEntry> online_images;
    bool online_loaded;
    bool loading_online;
    std::thread online_images_thread;

    int selected_index;
    std::vector<std::wstring> list_descriptions;
    image_source::ImageEntry selected_image;
    bool is_local_image;
    std::string local_image_dir;

    std::atomic<bool> cancel_download;
    std::atomic<bool> download_running;
    std::thread download_thread;
    std::string download_error;
    int current_file_index;
    int total_files;
    std::string current_file_name;
    uint64_t current_downloaded;
    uint64_t current_total;
    DWORD last_progress_ui_tick;
    int last_progress_ui_file_index;
    int last_progress_ui_percent;

    DWORD speed_sample_tick;
    uint64_t speed_sample_bytes;
    double smooth_speed_bps;

    DialogData() : mgr(nullptr), dlg(nullptr), current_page(Page::kSelectImage),
                   created(false), closed(false),
                   selected_source_index(-1),
                   online_loaded(false), loading_online(false),
                   selected_index(-1), is_local_image(false),
                   cancel_download(false), download_running(false),
                   current_file_index(0), total_files(0),
                   current_downloaded(0), current_total(0),
                   last_progress_ui_tick(0), last_progress_ui_file_index(-1),
                   last_progress_ui_percent(-1),
                   speed_sample_tick(0), speed_sample_bytes(0), smooth_speed_bps(0) {}

    std::string ImagesDir() const {
        return settings::EffectiveImageCacheDir(mgr->app_settings(), mgr->data_dir());
    }
};

static std::string NextAgentName(const std::vector<VmRecord>& records, const std::string& image_id) {
    std::string prefix = image_id + "-";
    int max_n = 0;
    for (const auto& rec : records) {
        const auto& name = rec.spec.name;
        if (name.size() > prefix.size() && name.substr(0, prefix.size()) == prefix) {
            try { max_n = std::max(max_n, std::stoi(name.substr(prefix.size()))); }
            catch (...) {}
        }
    }
    return prefix + std::to_string(max_n + 1);
}

static std::string FormatSize(uint64_t bytes) {
    static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    char buf[64];
    if (unit == 0) {
        snprintf(buf, sizeof(buf), "%llu %s",
            static_cast<unsigned long long>(bytes), kUnits[unit]);
    } else {
        snprintf(buf, sizeof(buf), "%.1f %s", value, kUnits[unit]);
    }
    return std::string(buf);
}

static std::string FormatSpeed(double bytes_per_sec) {
    if (bytes_per_sec < 1.0) return "";
    static const char* kUnits[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int unit = 0;
    while (bytes_per_sec >= 1024.0 && unit < 3) {
        bytes_per_sec /= 1024.0;
        ++unit;
    }
    char buf[64];
    if (unit == 0) {
        snprintf(buf, sizeof(buf), "%.0f %s", bytes_per_sec, kUnits[unit]);
    } else {
        snprintf(buf, sizeof(buf), "%.1f %s", bytes_per_sec, kUnits[unit]);
    }
    return std::string(buf);
}

static std::string FormatEta(double seconds) {
    if (seconds < 0 || seconds > 359999) return "";
    int s = static_cast<int>(seconds + 0.5);
    char buf[64];
    if (s < 60) {
        snprintf(buf, sizeof(buf), "%ds", s);
    } else if (s < 3600) {
        snprintf(buf, sizeof(buf), "%dm%02ds", s / 60, s % 60);
    } else {
        snprintf(buf, sizeof(buf), "%dh%02dm", s / 3600, (s % 3600) / 60);
    }
    return std::string(buf);
}

static bool TryGetSelectedCachedImage(const DialogData* data, image_source::ImageEntry* image) {
    if (!data || data->selected_index < 0) return false;
    int cached_count = static_cast<int>(data->cached_images.size());
    if (data->selected_index >= cached_count) return false;
    if (image) *image = data->cached_images[data->selected_index];
    return true;
}

static void ShowPage(DialogData* data, Page page);
static void RefreshImageList(DialogData* data);
static void FetchSources(DialogData* data);
static void FetchOnlineImages(DialogData* data);
static void StartDownload(DialogData* data);

static void SetControlsVisible(HWND dlg, const int* ids, int count, bool visible) {
    for (int i = 0; i < count; ++i) {
        HWND ctrl = GetDlgItem(dlg, ids[i]);
        if (ctrl) ShowWindow(ctrl, visible ? SW_SHOW : SW_HIDE);
    }
}

static void ShowPage(DialogData* data, Page page) {
    HWND dlg = data->dlg;
    data->current_page = page;

    static const int select_ctrls[] = {IDC_SOURCE_LABEL, IDC_SOURCE_COMBO, IDC_BTN_LOAD,
                                       IDC_IMAGE_LIST, IDC_STATUS_TEXT,
                                       IDC_BTN_DELETE_CACHE};
    static const int download_ctrls[] = {IDC_PROGRESS, IDC_PROGRESS_TEXT};
    static const int confirm_ctrls[] = {IDC_NAME_LABEL, IDC_NAME_EDIT, IDC_MEMORY_LABEL,
                                        IDC_MEMORY_SLIDER, IDC_MEMORY_VALUE,
                                        IDC_CPU_LABEL, IDC_CPU_SLIDER, IDC_CPU_VALUE,
                                        IDC_DEBUG_CHECK};

    SetControlsVisible(dlg, select_ctrls, 6, false);
    SetControlsVisible(dlg, download_ctrls, 2, false);
    SetControlsVisible(dlg, confirm_ctrls, 9, false);

    HWND btn_back = GetDlgItem(dlg, IDC_BTN_BACK);
    HWND btn_next = GetDlgItem(dlg, IDC_BTN_NEXT);
    HWND btn_local = GetDlgItem(dlg, IDC_BTN_LOCAL_IMAGE);

    switch (page) {
    case Page::kSelectImage:
        SetControlsVisible(dlg, select_ctrls, 5, true);
        ShowWindow(GetDlgItem(dlg, IDC_BTN_DELETE_CACHE), SW_SHOW);
        EnableWindow(GetDlgItem(dlg, IDC_BTN_DELETE_CACHE), TryGetSelectedCachedImage(data, nullptr));
        SetWindowTextW(btn_next, i18n::tr_w(i18n::S::kImgBtnNext).c_str());
        EnableWindow(btn_next, data->selected_index >= 0);
        ShowWindow(btn_next, SW_SHOW);
        ShowWindow(btn_local, SW_SHOW);
        ShowWindow(btn_back, SW_HIDE);
        RefreshImageList(data);
        break;

    case Page::kDownloading:
        SetControlsVisible(dlg, download_ctrls, 2, true);
        ShowWindow(btn_next, SW_HIDE);
        ShowWindow(btn_local, SW_HIDE);
        ShowWindow(btn_back, SW_HIDE);
        SendMessage(GetDlgItem(dlg, IDC_PROGRESS), PBM_SETPOS, 0, 0);
        SetDlgItemTextW(dlg, IDC_PROGRESS_TEXT, i18n::tr_w(i18n::S::kImgDownloading).c_str());
        break;

    case Page::kConfirm: {
        SetControlsVisible(dlg, confirm_ctrls, 9, true);
        SetWindowTextW(btn_next, i18n::tr_w(i18n::S::kDlgBtnCreate).c_str());
        EnableWindow(btn_next, TRUE);
        ShowWindow(btn_next, SW_SHOW);
        ShowWindow(btn_local, SW_HIDE);
        ShowWindow(btn_back, SW_SHOW);
        SetWindowTextW(btn_back, i18n::tr_w(i18n::S::kImgBtnBack).c_str());

        auto records = data->mgr->ListVms();
        SetDlgItemTextW(dlg, IDC_NAME_EDIT, i18n::to_wide(NextAgentName(records, data->selected_image.id)).c_str());

        int max_mem = g_host_memory_gb > 0 ? g_host_memory_gb : 16;
        InitSlider(dlg, IDC_MEMORY_SLIDER, IDC_MEMORY_VALUE, 1, max_mem, kDefaultMemoryGb, true);

        int max_cpus = g_host_cpus > 0 ? g_host_cpus : 4;
        InitSlider(dlg, IDC_CPU_SLIDER, IDC_CPU_VALUE, 1, max_cpus, kDefaultVcpus, false);

        CheckDlgButton(dlg, IDC_DEBUG_CHECK, BST_UNCHECKED);
        break;
    }
    }
}

static constexpr int kGroupIdCached = 0;
static constexpr int kGroupIdOnline = 1;

static void RefreshImageList(DialogData* data) {
    HWND list = GetDlgItem(data->dlg, IDC_IMAGE_LIST);
    ListView_DeleteAllItems(list);
    ListView_RemoveAllGroups(list);
    data->list_descriptions.clear();

    LVGROUP grp{};
    grp.cbSize = sizeof(grp);
    grp.mask = LVGF_HEADER | LVGF_GROUPID;

    std::wstring cached_header = i18n::tr_w(i18n::S::kImgGroupCached);
    grp.pszHeader = cached_header.data();
    grp.iGroupId = kGroupIdCached;
    ListView_InsertGroup(list, -1, &grp);

    std::wstring online_header = i18n::tr_w(i18n::S::kImgGroupOnline);
    grp.pszHeader = online_header.data();
    grp.iGroupId = kGroupIdOnline;
    ListView_InsertGroup(list, -1, &grp);

    int index = 0;

    for (const auto& img : data->cached_images) {
        std::string text = img.display_name;
        uint64_t total = img.TotalSize();
        if (total > 0)
            text += " (" + FormatSize(total) + ")";
        std::wstring wtext = i18n::to_wide(text);

        LVITEMW lvi{};
        lvi.mask = LVIF_TEXT | LVIF_GROUPID | LVIF_PARAM;
        lvi.iItem = index;
        lvi.iSubItem = 0;
        lvi.pszText = wtext.data();
        lvi.iGroupId = kGroupIdCached;
        lvi.lParam = 0;
        SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&lvi));

        data->list_descriptions.push_back(i18n::to_wide(
            img.description.empty() ? i18n::tr(i18n::S::kImgNoDescription) : img.description));
        ++index;
    }

    if (data->online_loaded) {
        for (const auto& img : data->online_images) {
            bool is_cached = false;
            for (const auto& cached : data->cached_images) {
                if (cached.id == img.id && cached.version == img.version) {
                    is_cached = true;
                    break;
                }
            }
            if (!is_cached) {
                std::string text = img.display_name;
                uint64_t total = img.TotalSize();
                if (total > 0)
                    text += " (" + FormatSize(total) + ")";
                std::wstring wtext = i18n::to_wide(text);

                LVITEMW lvi{};
                lvi.mask = LVIF_TEXT | LVIF_GROUPID | LVIF_PARAM;
                lvi.iItem = index;
                lvi.iSubItem = 0;
                lvi.pszText = wtext.data();
                lvi.iGroupId = kGroupIdOnline;
                lvi.lParam = 1;
                SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&lvi));

                data->list_descriptions.push_back(i18n::to_wide(
                    img.description.empty() ? i18n::tr(i18n::S::kImgNoDescription) : img.description));
                ++index;
            }
        }
    }

    if (data->selected_index >= 0 && data->selected_index < index) {
        ListView_SetItemState(list, data->selected_index,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, data->selected_index, FALSE);
    }

    HWND status = GetDlgItem(data->dlg, IDC_STATUS_TEXT);
    if (data->loading_online) {
        SetWindowTextW(status, i18n::tr_w(i18n::S::kImgLoadingOnline).c_str());
    } else if (!data->download_error.empty()) {
        SetWindowTextW(status, i18n::to_wide(data->download_error).c_str());
    } else {
        SetWindowTextW(status, L"");
    }
}

static void PopulateSourceCombo(DialogData* data) {
    HWND combo = GetDlgItem(data->dlg, IDC_SOURCE_COMBO);
    SendMessage(combo, CB_RESETCONTENT, 0, 0);

    const auto& last = data->mgr->app_settings().last_selected_source;
    int default_sel = 0;
    for (int i = 0; i < static_cast<int>(data->sources.size()); ++i) {
        SendMessageW(combo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(i18n::to_wide(data->sources[i].name).c_str()));
        if (!last.empty() && data->sources[i].name == last)
            default_sel = i;
    }
    if (!data->sources.empty()) {
        SendMessage(combo, CB_SETCURSEL, default_sel, 0);
        data->selected_source_index = default_sel;
    }

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    if (g_online_images_loaded && g_online_images_source_index == data->selected_source_index) {
        data->online_images = g_cached_online_images;
        data->online_loaded = true;
    }
}

static void FetchSources(DialogData* data) {
    data->sources = settings::EffectiveSources(data->mgr->app_settings());
    data->download_error.clear();
    PopulateSourceCombo(data);
    RefreshImageList(data);
    if (!data->online_loaded) {
        FetchOnlineImages(data);
    }
}

static void FetchOnlineImages(DialogData* data) {
    if (data->loading_online) return;
    if (data->selected_source_index < 0) return;

    if (data->selected_source_index >= static_cast<int>(data->sources.size())) return;
    std::string url = data->sources[data->selected_source_index].url;

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        if (g_online_images_loaded && g_online_images_source_index == data->selected_source_index) {
            data->online_images = g_cached_online_images;
            data->online_loaded = true;
            RefreshImageList(data);
            return;
        }
    }

    data->loading_online = true;
    data->online_loaded = false;
    data->online_images.clear();
    data->download_error.clear();
    data->selected_index = -1;
    EnableWindow(GetDlgItem(data->dlg, IDC_BTN_LOAD), FALSE);
    RefreshImageList(data);

    HWND dlg = data->dlg;
    int source_idx = data->selected_source_index;
    if (data->online_images_thread.joinable())
        data->online_images_thread.join();
    data->online_images_thread = std::thread([data, dlg, url, source_idx]() {
        auto images_result = http::FetchString(url);
        if (data->closed) return;
        if (!images_result.success) {
            data->download_error = images_result.error;
            if (!data->closed) PostMessage(dlg, WM_FETCH_COMPLETE, 0, 0);
            return;
        }

        auto images = image_source::ParseImages(images_result.data);
        auto filtered = image_source::FilterImages(images, TENBOX_VERSION_STR);

        {
            std::lock_guard<std::mutex> lock(g_cache_mutex);
            g_cached_online_images = filtered;
            g_online_images_loaded = true;
            g_online_images_source_index = source_idx;
        }

        if (data->closed) return;
        data->online_images = std::move(filtered);
        PostMessage(dlg, WM_FETCH_COMPLETE, 1, 0);
    });
}

static void StartDownload(DialogData* data) {
    ShowPage(data, Page::kDownloading);

    if (data->download_thread.joinable())
        data->download_thread.join();

    data->cancel_download = false;
    data->download_running = true;
    data->download_error.clear();
    data->current_file_index = 0;
    data->total_files = static_cast<int>(data->selected_image.files.size());
    data->current_file_name.clear();
    data->last_progress_ui_tick = 0;
    data->last_progress_ui_file_index = -1;
    data->last_progress_ui_percent = -1;
    data->speed_sample_tick = GetTickCount();
    data->speed_sample_bytes = 0;
    data->smooth_speed_bps = 0;

    HWND dlg = data->dlg;
    data->download_thread = std::thread([data, dlg]() {
        std::string cache_dir = image_source::ImageCacheDir(
            data->ImagesDir(), data->selected_image);

        std::error_code ec;
        fs::create_directories(cache_dir, ec);

        bool success = true;
        for (size_t i = 0; i < data->selected_image.files.size(); ++i) {
            if (data->cancel_download) {
                data->download_error = "Cancelled";
                success = false;
                break;
            }

            const auto& file = data->selected_image.files[i];
            std::string dest = (fs::path(cache_dir) / file.name).string();

            data->current_file_index = static_cast<int>(i);
            data->current_file_name = file.name;
            data->current_downloaded = 0;
            data->current_total = 0;
            if (!data->cancel_download)
                PostMessage(dlg, WM_DOWNLOAD_PROGRESS, 0, 0);

            auto result = http::DownloadFile(
                file.url, dest, file.sha256,
                [data, dlg](uint64_t downloaded, uint64_t total) {
                    data->current_downloaded = downloaded;
                    data->current_total = total;
                    if (!data->cancel_download)
                        PostMessage(dlg, WM_DOWNLOAD_PROGRESS, 0, 0);
                },
                &data->cancel_download);

            if (!result.success) {
                data->download_error = file.name + ": " + result.error;
                success = false;
                break;
            }
        }

        if (success)
            image_source::SaveImageMeta(cache_dir, data->selected_image);

        data->download_running = false;
        if (!data->closed)
            PostMessage(dlg, WM_DOWNLOAD_COMPLETE, success ? 1 : 0, 0);
    });
}

static LRESULT CALLBACK DlgSubclassProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp,
                                        UINT_PTR, DWORD_PTR ref) {
    DialogData* data = reinterpret_cast<DialogData*>(ref);

    switch (msg) {
    case WM_SETCURSOR:
        if (data->loading_online) {
            SetCursor(LoadCursor(nullptr, IDC_APPSTARTING));
            return TRUE;
        }
        break;

    case WM_FETCH_COMPLETE:
        data->loading_online = false;
        data->online_loaded = (wp == 1);
        RefreshImageList(data);
        EnableWindow(GetDlgItem(dlg, IDC_BTN_LOAD), TRUE);
        return 0;

    case WM_DOWNLOAD_PROGRESS: {
        int file_progress = 0;
        if (data->current_total > 0) {
            file_progress = static_cast<int>(data->current_downloaded * 100 / data->current_total);
        }

        DWORD now = GetTickCount();
        bool file_changed = (data->last_progress_ui_file_index != data->current_file_index);
        bool percent_changed = (data->last_progress_ui_percent != file_progress);
        bool done = (file_progress >= 100);
        DWORD elapsed = now - data->last_progress_ui_tick;

        // Throttle repaint frequency to reduce progress bar/text flicker.
        if (!file_changed && !done) {
            if (!percent_changed && elapsed < 150) return 0;
            if (percent_changed && elapsed < 90) return 0;
        }

        if (file_changed) {
            data->speed_sample_tick = now;
            data->speed_sample_bytes = 0;
            data->smooth_speed_bps = 0;
        }

        data->last_progress_ui_tick = now;
        data->last_progress_ui_file_index = data->current_file_index;
        data->last_progress_ui_percent = file_progress;
        SendMessage(GetDlgItem(dlg, IDC_PROGRESS), PBM_SETPOS, file_progress, 0);

        DWORD speed_elapsed = now - data->speed_sample_tick;
        if (speed_elapsed >= 1000 && data->current_downloaded > data->speed_sample_bytes) {
            double instant = static_cast<double>(data->current_downloaded - data->speed_sample_bytes)
                             / (speed_elapsed / 1000.0);
            constexpr double kAlpha = 0.3;
            data->smooth_speed_bps = (data->smooth_speed_bps < 1.0)
                ? instant
                : data->smooth_speed_bps * (1.0 - kAlpha) + instant * kAlpha;
            data->speed_sample_tick = now;
            data->speed_sample_bytes = data->current_downloaded;
        }

        char buf[512];
        snprintf(buf, sizeof(buf), i18n::tr(i18n::S::kImgDownloadingFile),
                 data->current_file_index + 1, data->total_files, data->current_file_name.c_str());
        std::string text = buf;
        text += "\n" + std::to_string(file_progress) + "%";
        if (data->current_total > 0) {
            text += "  " + FormatSize(data->current_downloaded) + " / " + FormatSize(data->current_total);
        } else if (data->current_downloaded > 0) {
            text += "  " + FormatSize(data->current_downloaded);
        }
        if (data->smooth_speed_bps > 0) {
            text += "  " + FormatSpeed(data->smooth_speed_bps);
            if (data->current_total > data->current_downloaded) {
                double remaining = static_cast<double>(data->current_total - data->current_downloaded)
                                   / data->smooth_speed_bps;
                std::string eta = FormatEta(remaining);
                if (!eta.empty()) text += "  " + i18n::fmt(i18n::S::kImgEta, eta.c_str());
            }
        }
        SetDlgItemTextW(dlg, IDC_PROGRESS_TEXT, i18n::to_wide(text).c_str());
        return 0;
    }

    case WM_DOWNLOAD_COMPLETE:
        if (wp == 1) {
            data->cached_images = image_source::GetCachedImages(data->ImagesDir());
            ShowPage(data, Page::kConfirm);
        } else {
            if (!data->download_error.empty() && data->download_error != "Cancelled") {
                MessageBoxW(dlg, i18n::to_wide(data->download_error).c_str(), i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONERROR);
            }
            ShowPage(data, Page::kSelectImage);
        }
        return 0;

    case WM_NOTIFY: {
        NMHDR* nmhdr = reinterpret_cast<NMHDR*>(lp);
        if (nmhdr->idFrom == IDC_IMAGE_LIST) {
            if (nmhdr->code == LVN_ITEMCHANGED) {
                NMLISTVIEW* nmlv = reinterpret_cast<NMLISTVIEW*>(lp);
                if (!(nmlv->uChanged & LVIF_STATE)) return 0;

                HWND list = GetDlgItem(dlg, IDC_IMAGE_LIST);
                bool now_selected = (nmlv->uNewState & LVIS_SELECTED) != 0;
                bool was_selected = (nmlv->uOldState & LVIS_SELECTED) != 0;

                if (now_selected && !was_selected) {
                    int old_sel = data->selected_index;
                    LPARAM item_data = nmlv->lParam;
                    if (item_data == 0 || item_data == 1) {
                        data->selected_index = nmlv->iItem;
                    } else {
                        data->selected_index = -1;
                    }
                    if (old_sel >= 0 && old_sel != nmlv->iItem) {
                        RECT old_rc{};
                        ListView_GetItemRect(list, old_sel, &old_rc, LVIR_BOUNDS);
                        InvalidateRect(list, &old_rc, TRUE);
                    }
                    EnableWindow(GetDlgItem(dlg, IDC_BTN_NEXT), data->selected_index >= 0);
                    EnableWindow(GetDlgItem(dlg, IDC_BTN_DELETE_CACHE), item_data == 0);
                } else if (!now_selected && was_selected) {
                    if (nmlv->iItem == data->selected_index) {
                        data->selected_index = -1;
                        EnableWindow(GetDlgItem(dlg, IDC_BTN_NEXT), FALSE);
                        EnableWindow(GetDlgItem(dlg, IDC_BTN_DELETE_CACHE), FALSE);
                    }
                }
                return 0;
            } else if (nmhdr->code == NM_DBLCLK) {
                NMITEMACTIVATE* nmia = reinterpret_cast<NMITEMACTIVATE*>(lp);
                if (nmia->iItem >= 0) {
                    LVITEMW lvi{};
                    lvi.mask = LVIF_PARAM;
                    lvi.iItem = nmia->iItem;
                    ListView_GetItem(GetDlgItem(dlg, IDC_IMAGE_LIST), &lvi);
                    if (lvi.lParam == 0 || lvi.lParam == 1) {
                        data->selected_index = nmia->iItem;
                        SendMessage(dlg, WM_COMMAND, IDC_BTN_NEXT, 0);
                    }
                }
                return 0;
            } else if (nmhdr->code == NM_CUSTOMDRAW) {
                NMLVCUSTOMDRAW* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
                switch (cd->nmcd.dwDrawStage) {
                case CDDS_PREPAINT:
                    return CDRF_NOTIFYITEMDRAW;
                case CDDS_ITEMPREPAINT: {
                    int idx = static_cast<int>(cd->nmcd.dwItemSpec);
                    if (idx < 0 || idx >= static_cast<int>(data->list_descriptions.size()))
                        return CDRF_DODEFAULT;

                    HDC hdc = cd->nmcd.hdc;
                    RECT rc = cd->nmcd.rc;
                    bool selected = (idx == data->selected_index);
                    bool focused = (GetFocus() == GetDlgItem(dlg, IDC_IMAGE_LIST));

                    COLORREF bg_color, name_color, desc_color;
                    if (selected && focused) {
                        bg_color = GetSysColor(COLOR_HIGHLIGHT);
                        name_color = GetSysColor(COLOR_HIGHLIGHTTEXT);
                        desc_color = GetSysColor(COLOR_HIGHLIGHTTEXT);
                    } else {
                        bg_color = GetSysColor(COLOR_WINDOW);
                        name_color = GetSysColor(COLOR_WINDOWTEXT);
                        desc_color = GetSysColor(COLOR_GRAYTEXT);
                    }

                    HBRUSH bg_brush = CreateSolidBrush(bg_color);
                    FillRect(hdc, &rc, bg_brush);
                    DeleteObject(bg_brush);

                    HFONT base_font = reinterpret_cast<HFONT>(
                        SendMessage(GetDlgItem(dlg, IDC_IMAGE_LIST), WM_GETFONT, 0, 0));

                    wchar_t name_buf[512]{};
                    LVITEMW lvi_tmp{};
                    lvi_tmp.mask = LVIF_TEXT;
                    lvi_tmp.iItem = idx;
                    lvi_tmp.iSubItem = 0;
                    lvi_tmp.pszText = name_buf;
                    lvi_tmp.cchTextMax = static_cast<int>(std::size(name_buf));
                    SendMessageW(GetDlgItem(dlg, IDC_IMAGE_LIST), LVM_GETITEMTEXTW,
                                 static_cast<WPARAM>(idx), reinterpret_cast<LPARAM>(&lvi_tmp));

                    int item_h = rc.bottom - rc.top;
                    int pad_x = 8;
                    int pad_y = 4;
                    int mid_gap = 2;

                    RECT name_rc = rc;
                    name_rc.left += pad_x;
                    name_rc.right -= pad_x;
                    name_rc.top = rc.top + pad_y;
                    name_rc.bottom = rc.top + (item_h - mid_gap) / 2;

                    HFONT old_font = reinterpret_cast<HFONT>(SelectObject(hdc, base_font));
                    int old_bk = SetBkMode(hdc, TRANSPARENT);
                    COLORREF old_color = SetTextColor(hdc, name_color);
                    DrawTextW(hdc, name_buf, -1, &name_rc,
                              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_BOTTOM);

                    LOGFONTW lf{};
                    GetObjectW(base_font, sizeof(lf), &lf);
                    lf.lfHeight = MulDiv(lf.lfHeight, 85, 100);
                    HFONT small_font = CreateFontIndirectW(&lf);
                    SelectObject(hdc, small_font);

                    RECT desc_rc = rc;
                    desc_rc.left += pad_x;
                    desc_rc.right -= pad_x;
                    desc_rc.top = rc.top + (item_h + mid_gap) / 2;

                    SetTextColor(hdc, desc_color);
                    DrawTextW(hdc, data->list_descriptions[idx].c_str(), -1, &desc_rc,
                              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

                    SetTextColor(hdc, old_color);
                    SetBkMode(hdc, old_bk);
                    SelectObject(hdc, old_font);
                    DeleteObject(small_font);

                    if (selected && focused) {
                        DrawFocusRect(hdc, &rc);
                    }

                    return CDRF_SKIPDEFAULT;
                }
                }
                return CDRF_DODEFAULT;
            } else if (nmhdr->code == NM_SETFOCUS || nmhdr->code == NM_KILLFOCUS) {
                if (data->selected_index >= 0) {
                    HWND list = GetDlgItem(dlg, IDC_IMAGE_LIST);
                    RECT item_rc{};
                    ListView_GetItemRect(list, data->selected_index, &item_rc, LVIR_BOUNDS);
                    InvalidateRect(list, &item_rc, TRUE);
                }
                return 0;
            }
        }
        break;
    }

    case WM_HSCROLL:
        if (HandleSliderScroll(dlg, lp,
                IDC_MEMORY_SLIDER, IDC_MEMORY_VALUE,
                IDC_CPU_SLIDER, IDC_CPU_VALUE))
            return 0;
        break;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SOURCE_COMBO:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = static_cast<int>(SendMessage(GetDlgItem(dlg, IDC_SOURCE_COMBO), CB_GETCURSEL, 0, 0));
                if (sel != CB_ERR && sel != data->selected_source_index) {
                    data->selected_source_index = sel;
                    if (sel >= 0 && sel < static_cast<int>(data->sources.size())) {
                        data->mgr->app_settings().last_selected_source = data->sources[sel].name;
                        data->mgr->SaveAppSettings();
                    }
                    data->online_loaded = false;
                    data->online_images.clear();
                    data->selected_index = -1;
                    {
                        std::lock_guard<std::mutex> lock(g_cache_mutex);
                        if (g_online_images_source_index != sel) {
                            g_online_images_loaded = false;
                            g_cached_online_images.clear();
                        }
                    }
                    RefreshImageList(data);
                    EnableWindow(GetDlgItem(dlg, IDC_BTN_NEXT), FALSE);
                    EnableWindow(GetDlgItem(dlg, IDC_BTN_DELETE_CACHE), FALSE);
                    FetchOnlineImages(data);
                }
            }
            return 0;

        case IDC_BTN_LOAD:
            if (data->selected_source_index >= 0) {
                {
                    std::lock_guard<std::mutex> lock(g_cache_mutex);
                    g_online_images_loaded = false;
                    g_cached_online_images.clear();
                }
                data->online_loaded = false;
                data->online_images.clear();
                data->selected_index = -1;
                data->download_error.clear();
                EnableWindow(GetDlgItem(dlg, IDC_BTN_NEXT), FALSE);
                EnableWindow(GetDlgItem(dlg, IDC_BTN_DELETE_CACHE), FALSE);
                FetchOnlineImages(data);
            }
            return 0;

        case IDC_BTN_DELETE_CACHE: {
            image_source::ImageEntry cached_img;
            if (!TryGetSelectedCachedImage(data, &cached_img)) return 0;

            char confirm_buf[512];
            snprintf(confirm_buf, sizeof(confirm_buf),
                i18n::tr(i18n::S::kImgConfirmDeleteCacheMsg), cached_img.display_name.c_str());
            int ans = MessageBoxW(dlg, i18n::to_wide(confirm_buf).c_str(),
                i18n::tr_w(i18n::S::kImgConfirmDeleteCacheTitle).c_str(),
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (ans != IDYES) return 0;

            std::string cache_dir = image_source::ImageCacheDir(data->ImagesDir(), cached_img);
            std::error_code ec;
            fs::remove_all(cache_dir, ec);
            if (ec) {
                std::string err = i18n::fmt(i18n::S::kImgCacheDeleteFailed, ec.message().c_str());
                MessageBoxW(dlg, i18n::to_wide(err).c_str(), i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONERROR);
                return 0;
            }

            data->cached_images = image_source::GetCachedImages(data->ImagesDir());
            data->selected_index = -1;
            RefreshImageList(data);
            EnableWindow(GetDlgItem(dlg, IDC_BTN_NEXT), FALSE);
            EnableWindow(GetDlgItem(dlg, IDC_BTN_DELETE_CACHE), FALSE);
            SetWindowTextW(GetDlgItem(dlg, IDC_STATUS_TEXT), i18n::tr_w(i18n::S::kImgCacheDeleted).c_str());
            return 0;
        }

        case IDC_BTN_LOCAL_IMAGE: {
            std::string folder = BrowseForFolder(dlg, i18n::tr(i18n::S::kImgBtnLocalImage), nullptr);
            if (folder.empty()) return 0;

            fs::path dir_path(folder);
            std::string kernel, initrd, disk;
            for (const auto& entry : fs::directory_iterator(dir_path)) {
                if (!entry.is_regular_file()) continue;
                std::string name = entry.path().filename().string();
                if (name == "vmlinuz" || name.find("vmlinuz") == 0) {
                    kernel = name;
                } else if (name.find("initrd") == 0 || name.find("initramfs") == 0 ||
                           (name.size() > 3 && name.substr(name.size() - 3) == ".gz" && kernel.empty() == false)) {
                    if (initrd.empty()) initrd = name;
                } else if (name.size() > 6 && name.substr(name.size() - 6) == ".qcow2") {
                    disk = name;
                }
            }
            // Also pick up *.gz files that look like initrd even without a kernel match
            if (initrd.empty()) {
                for (const auto& entry : fs::directory_iterator(dir_path)) {
                    if (!entry.is_regular_file()) continue;
                    std::string name = entry.path().filename().string();
                    if (name.size() > 3 && name.substr(name.size() - 3) == ".gz") {
                        initrd = name;
                        break;
                    }
                }
            }

            if (disk.empty() && kernel.empty()) {
                MessageBoxW(dlg, i18n::tr_w(i18n::S::kImgLocalNoFiles).c_str(),
                    i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONWARNING);
                return 0;
            }

            image_source::ImageEntry local_img;
            local_img.id = dir_path.filename().string();
            local_img.display_name = local_img.id;
            if (!kernel.empty())
                local_img.files.push_back({kernel, {}, {}});
            if (!initrd.empty())
                local_img.files.push_back({initrd, {}, {}});
            if (!disk.empty())
                local_img.files.push_back({disk, {}, {}});

            data->selected_image = local_img;
            data->is_local_image = true;
            data->local_image_dir = folder;
            ShowPage(data, Page::kConfirm);
            return 0;
        }

        case IDC_BTN_BACK:
            if (data->current_page == Page::kConfirm) {
                data->is_local_image = false;
                data->local_image_dir.clear();
                ShowPage(data, Page::kSelectImage);
            }
            return 0;

        case IDC_BTN_NEXT:
            if (data->current_page == Page::kSelectImage) {
                if (data->selected_index >= 0) {
                    int cached_count = static_cast<int>(data->cached_images.size());
                    if (data->selected_index < cached_count) {
                        data->selected_image = data->cached_images[data->selected_index];
                        ShowPage(data, Page::kConfirm);
                    } else {
                        int online_idx = data->selected_index - cached_count;
                        int actual_idx = 0;
                        for (const auto& img : data->online_images) {
                            bool is_cached = false;
                            for (const auto& cached : data->cached_images) {
                                if (cached.id == img.id && cached.version == img.version) {
                                    is_cached = true;
                                    break;
                                }
                            }
                            if (!is_cached) {
                                if (actual_idx == online_idx) {
                                    data->selected_image = img;
                                    if (image_source::IsImageCached(data->ImagesDir(), img)) {
                                        ShowPage(data, Page::kConfirm);
                                    } else {
                                        StartDownload(data);
                                    }
                                    break;
                                }
                                ++actual_idx;
                            }
                        }
                    }
                }
            } else if (data->current_page == Page::kDownloading) {
                data->cancel_download = true;
            } else if (data->current_page == Page::kConfirm) {
                wchar_t name_buf[256]{};
                GetDlgItemTextW(dlg, IDC_NAME_EDIT, name_buf, static_cast<int>(std::size(name_buf)));
                std::string req_name = i18n::wide_to_utf8(name_buf);

                int mem_gb = static_cast<int>(SendMessage(GetDlgItem(dlg, IDC_MEMORY_SLIDER), TBM_GETPOS, 0, 0));
                int cpu_count = static_cast<int>(SendMessage(GetDlgItem(dlg, IDC_CPU_SLIDER), TBM_GETPOS, 0, 0));
                if (mem_gb < 1) mem_gb = kDefaultMemoryGb;
                if (cpu_count < 1) cpu_count = kDefaultVcpus;

                std::string cache_dir = data->is_local_image
                    ? data->local_image_dir
                    : image_source::ImageCacheDir(data->ImagesDir(), data->selected_image);

                VmCreateRequest req;
                req.name = req_name;
                req.storage_dir = settings::EffectiveVmStorageDir(data->mgr->app_settings());
                req.memory_mb = mem_gb * 1024;
                req.cpu_count = cpu_count;
                req.debug_mode = IsDlgButtonChecked(dlg, IDC_DEBUG_CHECK) == BST_CHECKED;

                for (const auto& file : data->selected_image.files) {
                    std::string path = (fs::path(cache_dir) / file.name).string();
                    if (file.name == "vmlinuz" || file.name.find("vmlinuz") == 0) {
                        req.source_kernel = path;
                    } else if (file.name == "initrd.gz" || file.name.find("initrd") == 0 ||
                               file.name.find("initramfs") == 0) {
                        req.source_initrd = path;
                    } else if (file.name.find(".qcow2") != std::string::npos ||
                               file.name.find("rootfs") != std::string::npos) {
                        req.source_disk = path;
                    }
                }

                auto v = ValidateCreateRequest(req);
                if (!v.ok) {
                    MessageBoxW(dlg, i18n::to_wide(v.message).c_str(), i18n::tr_w(i18n::S::kValidationError).c_str(), MB_OK | MB_ICONWARNING);
                    return 0;
                }

                std::string error;
                if (data->mgr->CreateVm(req, &error)) {
                    data->created = true;
                    data->closed = true;
                    EnableWindow(GetWindow(dlg, GW_OWNER), TRUE);
                    DestroyWindow(dlg);
                } else {
                    MessageBoxW(dlg, i18n::to_wide(error).c_str(), i18n::tr_w(i18n::S::kError).c_str(), MB_OK | MB_ICONERROR);
                }
            }
            return 0;

        }
        break;

    case WM_CLOSE:
        if (data->current_page == Page::kDownloading) {
            data->cancel_download = true;
        }
        data->closed = true;
        EnableWindow(GetWindow(dlg, GW_OWNER), TRUE);
        DestroyWindow(dlg);
        return 0;

    case WM_NCDESTROY:
        RemoveWindowSubclass(dlg, DlgSubclassProc, 1);
        break;
    }
    return DefSubclassProc(dlg, msg, wp, lp);
}

static const wchar_t* kDialogClassName = L"TenBoxCreateVmDlg";
static bool g_class_registered = false;

static void RegisterDialogClass() {
    if (g_class_registered) return;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kDialogClassName;

    RegisterClassExW(&wc);
    g_class_registered = true;
}

bool ShowCreateVmDialog2(HWND parent, ManagerService& mgr, std::string* error) {
    RegisterDialogClass();

    DialogData data;
    data.mgr = &mgr;

    RECT parent_rect;
    GetWindowRect(parent, &parent_rect);
    int pw = parent_rect.right - parent_rect.left;
    int ph = parent_rect.bottom - parent_rect.top;

    UINT parent_dpi = 96;
#if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0A00
    parent_dpi = GetDpiForWindow(parent);
#else
    HDC parent_hdc = GetDC(parent);
    if (parent_hdc) {
        parent_dpi = static_cast<UINT>(GetDeviceCaps(parent_hdc, LOGPIXELSX));
        ReleaseDC(parent, parent_hdc);
    }
#endif
    if (parent_dpi == 0) parent_dpi = 96;
    auto scale_parent_px = [parent_dpi](int px) { return MulDiv(px, static_cast<int>(parent_dpi), 96); };

    int dlg_w = scale_parent_px(520), dlg_h = scale_parent_px(460);
    int x = parent_rect.left + (pw - dlg_w) / 2;
    int y = parent_rect.top + (ph - dlg_h) / 2;

    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kDialogClassName, i18n::tr_w(i18n::S::kDlgCreateVm).c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, dlg_w, dlg_h,
        parent, nullptr, GetModuleHandle(nullptr), nullptr);

    if (!dlg) {
        if (error) *error = "Failed to create dialog";
        return false;
    }

    data.dlg = dlg;
    SetWindowSubclass(dlg, DlgSubclassProc, 1, reinterpret_cast<DWORD_PTR>(&data));

    data.cached_images = image_source::GetCachedImages(data.ImagesDir());

    RECT rc;
    GetClientRect(dlg, &rc);
    int w = rc.right, h = rc.bottom;
    UINT dpi = 96;
#if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0A00
    dpi = GetDpiForWindow(dlg);
#else
    HDC hdc = GetDC(dlg);
    if (hdc) {
        dpi = static_cast<UINT>(GetDeviceCaps(hdc, LOGPIXELSX));
        ReleaseDC(dlg, hdc);
    }
#endif
    if (dpi == 0) dpi = 96;
    auto scale_px = [dpi](int px) { return MulDiv(px, static_cast<int>(dpi), 96); };

    int margin = scale_px(16);
    int btn_w = scale_px(96);
    int btn_h = scale_px(30);
    int load_btn_w = scale_px(72);
    int source_label_w = scale_px(84);
    int row_h = (btn_h + scale_px(4) > scale_px(30)) ? (btn_h + scale_px(4)) : scale_px(30);
    int list_top = margin + row_h + scale_px(8);
    int bottom_btns_y = h - margin - btn_h;
    int status_y = bottom_btns_y - scale_px(28);
    int list_h = status_y - list_top - scale_px(6);
    if (list_h < scale_px(120)) list_h = scale_px(120);

    int top_ctrl_y = margin + (row_h - btn_h) / 2;
    CreateWindowExW(0, L"STATIC", i18n::tr_w(i18n::S::kImgSource).c_str(),
        WS_CHILD | SS_LEFT | SS_CENTERIMAGE, margin, margin, source_label_w, row_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_SOURCE_LABEL)), nullptr, nullptr);

    int combo_x = margin + source_label_w + scale_px(6);
    int combo_w = w - combo_x - margin - load_btn_w - scale_px(10);
    CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        combo_x, top_ctrl_y, combo_w, 200,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_SOURCE_COMBO)), nullptr, nullptr);

    CreateWindowExW(0, L"BUTTON", i18n::tr_w(i18n::S::kImgBtnRefresh).c_str(),
        WS_CHILD | BS_PUSHBUTTON,
        w - margin - load_btn_w, top_ctrl_y, load_btn_w, btn_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_BTN_LOAD)), nullptr, nullptr);

    HWND list_view = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VSCROLL | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
        LVS_NOSORTHEADER | LVS_NOCOLUMNHEADER,
        margin, list_top, w - 2 * margin, list_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_IMAGE_LIST)), nullptr, nullptr);

    ListView_SetExtendedListViewStyle(list_view,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_EnableGroupView(list_view, TRUE);

    {
        int lv_w = w - 2 * margin - GetSystemMetrics(SM_CXVSCROLL) - 4;
        LVCOLUMNW col{};
        col.mask = LVCF_WIDTH;
        col.cx = lv_w;
        ListView_InsertColumn(list_view, 0, &col);
    }

    {
        int item_h = scale_px(46);
        HIMAGELIST hil = ImageList_Create(1, item_h, ILC_COLOR, 1, 0);
        ListView_SetImageList(list_view, hil, LVSIL_SMALL);
    }

    CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | SS_LEFT,
        margin, status_y, w - 2 * margin, scale_px(22),
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_STATUS_TEXT)), nullptr, nullptr);

    CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | PBS_SMOOTH,
        margin, margin + scale_px(58), w - 2 * margin, scale_px(24),
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_PROGRESS)), nullptr, nullptr);
    SendMessage(GetDlgItem(dlg, IDC_PROGRESS), PBM_SETRANGE, 0, MAKELPARAM(0, 100));

    CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | SS_LEFT,
        margin, margin + scale_px(92), w - 2 * margin, scale_px(48),
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_PROGRESS_TEXT)), nullptr, nullptr);

    auto layout = CalcSliderRowLayout(w, margin, btn_h, scale_px);
    int edit_w = w - layout.edit_x - margin;
    int edit_h = scale_px(26);
    int ctrl_y = margin;
    CreateWindowExW(0, L"STATIC", i18n::tr_w(i18n::S::kDlgLabelName).c_str(),
        WS_CHILD | SS_LEFT, margin, ctrl_y + layout.label_y_off, layout.label_w, scale_px(20),
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_NAME_LABEL)), nullptr, nullptr);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | ES_AUTOHSCROLL, layout.edit_x, ctrl_y, edit_w, edit_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_NAME_EDIT)), nullptr, nullptr);
    ctrl_y += layout.form_row_h;

    CreateSliderRow(dlg, layout, margin, ctrl_y,
        IDC_MEMORY_LABEL, i18n::tr_w(i18n::S::kDlgLabelMemory).c_str(),
        IDC_MEMORY_SLIDER, IDC_MEMORY_VALUE, scale_px);
    ctrl_y += layout.form_row_h;

    CreateSliderRow(dlg, layout, margin, ctrl_y,
        IDC_CPU_LABEL, i18n::tr_w(i18n::S::kDlgLabelVcpus).c_str(),
        IDC_CPU_SLIDER, IDC_CPU_VALUE, scale_px);
    ctrl_y += layout.form_row_h;

    CreateWindowExW(0, L"BUTTON", i18n::tr_w(i18n::S::kDlgDebugMode).c_str(),
        WS_CHILD | BS_AUTOCHECKBOX, layout.edit_x, ctrl_y + scale_px(4), edit_w, scale_px(22),
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_DEBUG_CHECK)), nullptr, nullptr);

    int btn_gap = scale_px(10);
    int secondary_btn_w = scale_px(130);
    int back_x = w - margin - 3 * btn_w - 2 * btn_gap;
    int delete_max_w = back_x - margin - btn_gap;
    if (secondary_btn_w > delete_max_w) secondary_btn_w = delete_max_w;
    if (secondary_btn_w < scale_px(90)) secondary_btn_w = scale_px(90);
    int delete_x = margin;

    CreateWindowExW(0, L"BUTTON", i18n::tr_w(i18n::S::kImgBtnDeleteCache).c_str(),
        WS_CHILD | BS_PUSHBUTTON, delete_x, bottom_btns_y, secondary_btn_w, btn_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_BTN_DELETE_CACHE)), nullptr, nullptr);
    CreateWindowExW(0, L"BUTTON", i18n::tr_w(i18n::S::kImgBtnBack).c_str(),
        WS_CHILD | BS_PUSHBUTTON, w - margin - 3 * btn_w - 2 * btn_gap, bottom_btns_y, btn_w, btn_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_BTN_BACK)), nullptr, nullptr);

    CreateWindowExW(0, L"BUTTON", i18n::tr_w(i18n::S::kImgBtnLocalImage).c_str(),
        WS_CHILD | BS_PUSHBUTTON, w - margin - 2 * btn_w - btn_gap, bottom_btns_y, btn_w, btn_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_BTN_LOCAL_IMAGE)), nullptr, nullptr);

    CreateWindowExW(0, L"BUTTON", i18n::tr_w(i18n::S::kImgBtnNext).c_str(),
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, w - margin - btn_w, bottom_btns_y, btn_w, btn_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_BTN_NEXT)), nullptr, nullptr);

    HFONT ui_font = nullptr;
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        LOGFONTW lf = ncm.lfMessageFont;
        // Use a slightly larger message font for this custom dialog.
        lf.lfHeight = -MulDiv(10, static_cast<int>(dpi), 72);
        ui_font = CreateFontIndirectW(&lf);
    }
    if (!ui_font) {
        ui_font = reinterpret_cast<HFONT>(SendMessage(parent, WM_GETFONT, 0, 0));
    }
    if (!ui_font) ui_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    EnumChildWindows(dlg, [](HWND child, LPARAM f) -> BOOL {
        SendMessage(child, WM_SETFONT, f, TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(ui_font));

    auto set_combo_heights = [&](int id) {
        HWND combo = GetDlgItem(dlg, id);
        if (!combo) return;
        int combo_sel_h = btn_h - scale_px(4);
        if (combo_sel_h < scale_px(24)) combo_sel_h = scale_px(24);
        int combo_item_h = combo_sel_h - scale_px(4);
        if (combo_item_h < scale_px(20)) combo_item_h = scale_px(20);
        SendMessage(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), static_cast<LPARAM>(combo_sel_h));
        SendMessage(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(0), static_cast<LPARAM>(combo_item_h));
    };
    set_combo_heights(IDC_SOURCE_COMBO);

    g_host_memory_gb = GetHostMemoryGb();
    if (g_host_memory_gb < 1) g_host_memory_gb = 16;
    g_host_cpus = GetHostLogicalCpus();

    ShowPage(&data, Page::kSelectImage);
    FetchSources(&data);

    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    MSG msg;
    while (!data.closed && GetMessage(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessage(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    data.cancel_download = true;
    if (data.download_thread.joinable())
        data.download_thread.join();
    if (data.online_images_thread.joinable())
        data.online_images_thread.join();

    // Delete only if we created a private font object.
    if (ui_font && ui_font != reinterpret_cast<HFONT>(SendMessage(parent, WM_GETFONT, 0, 0))
        && ui_font != static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT))) {
        DeleteObject(ui_font);
    }

    EnableWindow(parent, TRUE);

    if (error) *error = data.error;
    return data.created;
}
