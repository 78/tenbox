#include "ui/win32/create_vm_dialog.h"
#include "ui/common/i18n.h"
#include "ui/common/vm_forms.h"
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
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static constexpr int kMemoryOptionsMb[] = {1024, 2048, 4096, 8192, 16384};
static const char* kMemoryLabels[] = {"1 GB", "2 GB", "4 GB", "8 GB", "16 GB"};
static constexpr int kCpuOptions[] = {1, 2, 4, 8, 16};
static const char* kCpuLabels[] = {"1", "2", "4", "8", "16"};
static constexpr int kNumOptions = 5;

static const char* kSourcesUrl = "https://tenbox.ai/api/sources.json";

// Process-lifetime caches
static std::mutex g_cache_mutex;
static std::vector<image_source::ImageSource> g_cached_sources;
static bool g_sources_loaded = false;
static std::vector<image_source::ImageEntry> g_cached_online_images;
static bool g_online_images_loaded = false;
static int g_online_images_source_index = -1;

enum {
    IDC_SOURCE_LABEL = 100,
    IDC_SOURCE_COMBO = 101,
    IDC_BTN_LOAD = 102,
    IDC_IMAGE_LIST = 103,
    IDC_DESC_TEXT = 104,
    IDC_STATUS_TEXT = 105,
    IDC_PROGRESS = 106,
    IDC_PROGRESS_TEXT = 107,
    IDC_NAME_LABEL = 108,
    IDC_NAME_EDIT = 109,
    IDC_MEMORY_LABEL = 110,
    IDC_MEMORY_COMBO = 111,
    IDC_CPU_LABEL = 112,
    IDC_CPU_COMBO = 113,
    IDC_NAT_CHECK = 114,
    IDC_BTN_BACK = 115,
    IDC_BTN_NEXT = 116,
    IDC_BTN_RETRY = 117,
    IDC_BTN_DELETE_CACHE = 118,
};

enum class Page {
    kSelectImage,
    kDownloading,
    kConfirm
};

enum {
    WM_SOURCES_COMPLETE = WM_USER + 100,
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

    bool sources_loaded;
    bool loading_sources;
    int selected_source_index;

    std::vector<image_source::ImageEntry> cached_images;
    std::vector<image_source::ImageEntry> online_images;
    bool online_loaded;
    bool loading_online;

    int selected_index;
    image_source::ImageEntry selected_image;

    std::atomic<bool> cancel_download;
    std::string download_error;
    int current_file_index;
    int total_files;
    std::string current_file_name;
    uint64_t current_downloaded;
    uint64_t current_total;
    DWORD last_progress_ui_tick;
    int last_progress_ui_file_index;
    int last_progress_ui_percent;

    DialogData() : mgr(nullptr), dlg(nullptr), current_page(Page::kSelectImage),
                   created(false), closed(false),
                   sources_loaded(false), loading_sources(false), selected_source_index(-1),
                   online_loaded(false), loading_online(false),
                   selected_index(-1), cancel_download(false),
                   current_file_index(0), total_files(0),
                   current_downloaded(0), current_total(0),
                   last_progress_ui_tick(0), last_progress_ui_file_index(-1),
                   last_progress_ui_percent(-1) {}
};

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

static bool TryGetSelectedCachedImage(const DialogData* data, image_source::ImageEntry* image) {
    if (!data || data->selected_index < 0) return false;
    int cached_count = static_cast<int>(data->cached_images.size());
    if (data->selected_index >= cached_count) return false;
    if (image) *image = data->cached_images[data->selected_index];
    return true;
}

static void ShowPage(DialogData* data, Page page);
static void RefreshImageList(DialogData* data);
static void UpdateDescriptionText(DialogData* data);
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
                                       IDC_IMAGE_LIST, IDC_DESC_TEXT, IDC_STATUS_TEXT,
                                       IDC_BTN_RETRY, IDC_BTN_DELETE_CACHE};
    static const int download_ctrls[] = {IDC_PROGRESS, IDC_PROGRESS_TEXT};
    static const int confirm_ctrls[] = {IDC_NAME_LABEL, IDC_NAME_EDIT, IDC_MEMORY_LABEL,
                                        IDC_MEMORY_COMBO, IDC_CPU_LABEL, IDC_CPU_COMBO,
                                        IDC_NAT_CHECK};

    SetControlsVisible(dlg, select_ctrls, 8, false);
    SetControlsVisible(dlg, download_ctrls, 2, false);
    SetControlsVisible(dlg, confirm_ctrls, 7, false);

    HWND btn_back = GetDlgItem(dlg, IDC_BTN_BACK);
    HWND btn_next = GetDlgItem(dlg, IDC_BTN_NEXT);

    switch (page) {
    case Page::kSelectImage:
        SetControlsVisible(dlg, select_ctrls, 6, true);
        ShowWindow(GetDlgItem(dlg, IDC_BTN_RETRY),
            (!data->sources_loaded && !data->loading_sources && !data->download_error.empty()) ||
            (!data->online_loaded && !data->loading_online && data->sources_loaded && !data->download_error.empty())
            ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(dlg, IDC_BTN_DELETE_CACHE), SW_SHOW);
        EnableWindow(GetDlgItem(dlg, IDC_BTN_DELETE_CACHE), TryGetSelectedCachedImage(data, nullptr));
        SetWindowTextA(btn_next, i18n::tr(i18n::S::kImgBtnNext));
        EnableWindow(btn_next, data->selected_index >= 0);
        ShowWindow(btn_next, SW_SHOW);
        ShowWindow(btn_back, SW_HIDE);
        RefreshImageList(data);
        UpdateDescriptionText(data);
        break;

    case Page::kDownloading:
        SetControlsVisible(dlg, download_ctrls, 2, true);
        ShowWindow(btn_next, SW_HIDE);
        ShowWindow(btn_back, SW_HIDE);
        SendMessage(GetDlgItem(dlg, IDC_PROGRESS), PBM_SETPOS, 0, 0);
        SetDlgItemTextA(dlg, IDC_PROGRESS_TEXT, i18n::tr(i18n::S::kImgDownloading));
        break;

    case Page::kConfirm: {
        SetControlsVisible(dlg, confirm_ctrls, 7, true);
        SetWindowTextA(btn_next, i18n::tr(i18n::S::kDlgBtnCreate));
        EnableWindow(btn_next, TRUE);
        ShowWindow(btn_next, SW_SHOW);
        ShowWindow(btn_back, SW_SHOW);
        SetWindowTextA(btn_back, i18n::tr(i18n::S::kImgBtnBack));

        auto records = data->mgr->ListVms();
        SetDlgItemTextA(dlg, IDC_NAME_EDIT, NextAgentName(records).c_str());

        HWND mem_cb = GetDlgItem(dlg, IDC_MEMORY_COMBO);
        SendMessage(mem_cb, CB_RESETCONTENT, 0, 0);
        for (int i = 0; i < kNumOptions; ++i)
            SendMessageA(mem_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kMemoryLabels[i]));
        SendMessage(mem_cb, CB_SETCURSEL, 2, 0);

        HWND cpu_cb = GetDlgItem(dlg, IDC_CPU_COMBO);
        SendMessage(cpu_cb, CB_RESETCONTENT, 0, 0);
        for (int i = 0; i < kNumOptions; ++i)
            SendMessageA(cpu_cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kCpuLabels[i]));
        SendMessage(cpu_cb, CB_SETCURSEL, 2, 0);

        CheckDlgButton(dlg, IDC_NAT_CHECK, BST_CHECKED);
        break;
    }
    }
}

static void RefreshImageList(DialogData* data) {
    HWND list = GetDlgItem(data->dlg, IDC_IMAGE_LIST);
    SendMessage(list, LB_RESETCONTENT, 0, 0);

    int index = 0;

    for (const auto& img : data->cached_images) {
        std::string text = img.display_name + " " + i18n::tr(i18n::S::kImgCached);
        SendMessageA(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        SendMessage(list, LB_SETITEMDATA, index++, 0);
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
                SendMessageA(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(img.display_name.c_str()));
                SendMessage(list, LB_SETITEMDATA, index++, 1);
            }
        }
    }

    if (data->selected_index >= 0 && data->selected_index < index) {
        SendMessage(list, LB_SETCURSEL, data->selected_index, 0);
    }

    HWND status = GetDlgItem(data->dlg, IDC_STATUS_TEXT);
    if (data->loading_sources) {
        SetWindowTextA(status, i18n::tr(i18n::S::kImgLoadingOnline));
    } else if (data->loading_online) {
        SetWindowTextA(status, i18n::tr(i18n::S::kImgLoadingOnline));
    } else if (!data->download_error.empty()) {
        SetWindowTextA(status, data->download_error.c_str());
    } else {
        SetWindowTextA(status, "");
    }
}

static void UpdateDescriptionText(DialogData* data) {
    HWND desc_text = GetDlgItem(data->dlg, IDC_DESC_TEXT);
    if (data->selected_index < 0) {
        SetWindowTextA(desc_text, "");
        return;
    }

    int cached_count = static_cast<int>(data->cached_images.size());
    std::string description;

    if (data->selected_index < cached_count) {
        const auto& img = data->cached_images[data->selected_index];
        description = img.description.empty() ? i18n::tr(i18n::S::kImgNoDescription) : img.description;
    } else if (data->online_loaded) {
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
                    description = img.description.empty() ? i18n::tr(i18n::S::kImgNoDescription) : img.description;
                    break;
                }
                ++actual_idx;
            }
        }
    }

    SetWindowTextA(desc_text, description.c_str());
}

static void PopulateSourceCombo(DialogData* data) {
    HWND combo = GetDlgItem(data->dlg, IDC_SOURCE_COMBO);
    SendMessage(combo, CB_RESETCONTENT, 0, 0);

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    if (g_sources_loaded) {
        for (const auto& src : g_cached_sources) {
            SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(src.name.c_str()));
        }
        if (!g_cached_sources.empty()) {
            SendMessage(combo, CB_SETCURSEL, 0, 0);
            data->selected_source_index = 0;
        }
        data->sources_loaded = true;

        if (g_online_images_loaded && g_online_images_source_index == data->selected_source_index) {
            data->online_images = g_cached_online_images;
            data->online_loaded = true;
        }
    }
}

static void FetchSources(DialogData* data) {
    if (data->loading_sources) return;

    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        if (g_sources_loaded) {
            data->sources_loaded = true;
        }
    }
    if (data->sources_loaded) {
        PopulateSourceCombo(data);
        RefreshImageList(data);
        return;
    }

    data->loading_sources = true;
    data->download_error.clear();
    RefreshImageList(data);

    HWND dlg = data->dlg;
    std::thread([data, dlg]() {
        auto sources_result = http::FetchString(kSourcesUrl);
        if (!sources_result.success) {
            data->download_error = sources_result.error;
            PostMessage(dlg, WM_SOURCES_COMPLETE, 0, 0);
            return;
        }

        auto sources = image_source::ParseSources(sources_result.data);
        if (sources.empty()) {
            data->download_error = "No sources available";
            PostMessage(dlg, WM_SOURCES_COMPLETE, 0, 0);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_cache_mutex);
            g_cached_sources = sources;
            g_sources_loaded = true;
        }
        PostMessage(dlg, WM_SOURCES_COMPLETE, 1, 0);
    }).detach();
}

static void FetchOnlineImages(DialogData* data) {
    if (data->loading_online) return;
    if (data->selected_source_index < 0) return;

    std::string url;
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        if (data->selected_source_index >= static_cast<int>(g_cached_sources.size())) return;
        url = g_cached_sources[data->selected_source_index].url;

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
    RefreshImageList(data);

    HWND dlg = data->dlg;
    int source_idx = data->selected_source_index;
    std::thread([data, dlg, url, source_idx]() {
        auto images_result = http::FetchString(url);
        if (!images_result.success) {
            data->download_error = images_result.error;
            PostMessage(dlg, WM_FETCH_COMPLETE, 0, 0);
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

        data->online_images = std::move(filtered);
        PostMessage(dlg, WM_FETCH_COMPLETE, 1, 0);
    }).detach();
}

static void StartDownload(DialogData* data) {
    ShowPage(data, Page::kDownloading);

    data->cancel_download = false;
    data->download_error.clear();
    data->current_file_index = 0;
    data->total_files = static_cast<int>(data->selected_image.files.size());
    data->current_file_name.clear();
    data->last_progress_ui_tick = 0;
    data->last_progress_ui_file_index = -1;
    data->last_progress_ui_percent = -1;

    HWND dlg = data->dlg;
    std::thread([data, dlg]() {
        std::string cache_dir = image_source::ImageCacheDir(
            data->mgr->data_dir(), data->selected_image);

        std::error_code ec;
        fs::create_directories(cache_dir, ec);

        for (size_t i = 0; i < data->selected_image.files.size(); ++i) {
            if (data->cancel_download) {
                data->download_error = "Cancelled";
                PostMessage(dlg, WM_DOWNLOAD_COMPLETE, 0, 0);
                return;
            }

            const auto& file = data->selected_image.files[i];
            std::string dest = (fs::path(cache_dir) / file.name).string();

            data->current_file_index = static_cast<int>(i);
            data->current_file_name = file.name;
            data->current_downloaded = 0;
            data->current_total = 0;
            PostMessage(dlg, WM_DOWNLOAD_PROGRESS, 0, 0);

            auto result = http::DownloadFile(
                file.url, dest, file.sha256,
                [data, dlg](uint64_t downloaded, uint64_t total) {
                    data->current_downloaded = downloaded;
                    data->current_total = total;
                    PostMessage(dlg, WM_DOWNLOAD_PROGRESS, 0, 0);
                },
                &data->cancel_download);

            if (!result.success) {
                data->download_error = file.name + ": " + result.error;
                PostMessage(dlg, WM_DOWNLOAD_COMPLETE, 0, 0);
                return;
            }
        }

        image_source::SaveImageMeta(cache_dir, data->selected_image);
        PostMessage(dlg, WM_DOWNLOAD_COMPLETE, 1, 0);
    }).detach();
}

static LRESULT CALLBACK DlgSubclassProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp,
                                        UINT_PTR, DWORD_PTR ref) {
    DialogData* data = reinterpret_cast<DialogData*>(ref);

    switch (msg) {
    case WM_SOURCES_COMPLETE: {
        data->loading_sources = false;
        data->sources_loaded = (wp == 1);
        if (data->sources_loaded) {
            PopulateSourceCombo(data);
        }
        RefreshImageList(data);
        ShowWindow(GetDlgItem(dlg, IDC_BTN_RETRY),
            (!data->sources_loaded && !data->download_error.empty()) ? SW_SHOW : SW_HIDE);
        return 0;
    }

    case WM_FETCH_COMPLETE:
        data->loading_online = false;
        data->online_loaded = (wp == 1);
        RefreshImageList(data);
        ShowWindow(GetDlgItem(dlg, IDC_BTN_RETRY),
            (!data->online_loaded && !data->download_error.empty()) ? SW_SHOW : SW_HIDE);
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

        data->last_progress_ui_tick = now;
        data->last_progress_ui_file_index = data->current_file_index;
        data->last_progress_ui_percent = file_progress;
        SendMessage(GetDlgItem(dlg, IDC_PROGRESS), PBM_SETPOS, file_progress, 0);

        char buf[512];
        snprintf(buf, sizeof(buf), i18n::tr(i18n::S::kImgDownloadingFile),
                 data->current_file_index + 1, data->total_files, data->current_file_name.c_str());
        std::string text = buf;
        text += " - " + std::to_string(file_progress) + "%";
        if (data->current_total > 0) {
            text += " (" + FormatSize(data->current_downloaded) + " / " + FormatSize(data->current_total) + ")";
        } else if (data->current_downloaded > 0) {
            text += " (" + FormatSize(data->current_downloaded) + ")";
        }
        SetDlgItemTextA(dlg, IDC_PROGRESS_TEXT, text.c_str());
        return 0;
    }

    case WM_DOWNLOAD_COMPLETE:
        if (wp == 1) {
            data->cached_images = image_source::GetCachedImages(data->mgr->data_dir());
            ShowPage(data, Page::kConfirm);
        } else {
            if (!data->download_error.empty() && data->download_error != "Cancelled") {
                MessageBoxA(dlg, data->download_error.c_str(), i18n::tr(i18n::S::kError), MB_OK | MB_ICONERROR);
            }
            ShowPage(data, Page::kSelectImage);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SOURCE_COMBO:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = static_cast<int>(SendMessage(GetDlgItem(dlg, IDC_SOURCE_COMBO), CB_GETCURSEL, 0, 0));
                if (sel != CB_ERR && sel != data->selected_source_index) {
                    data->selected_source_index = sel;
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
                    UpdateDescriptionText(data);
                    EnableWindow(GetDlgItem(dlg, IDC_BTN_NEXT), FALSE);
                    EnableWindow(GetDlgItem(dlg, IDC_BTN_DELETE_CACHE), FALSE);
                }
            }
            return 0;

        case IDC_BTN_LOAD:
            if (!data->sources_loaded) {
                FetchSources(data);
            } else if (data->selected_source_index >= 0) {
                {
                    std::lock_guard<std::mutex> lock(g_cache_mutex);
                    g_online_images_loaded = false;
                    g_cached_online_images.clear();
                }
                data->online_loaded = false;
                data->online_images.clear();
                data->selected_index = -1;
                EnableWindow(GetDlgItem(dlg, IDC_BTN_NEXT), FALSE);
                EnableWindow(GetDlgItem(dlg, IDC_BTN_DELETE_CACHE), FALSE);
                FetchOnlineImages(data);
            }
            return 0;

        case IDC_IMAGE_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE) {
                HWND list = GetDlgItem(dlg, IDC_IMAGE_LIST);
                int sel = static_cast<int>(SendMessage(list, LB_GETCURSEL, 0, 0));
                if (sel != LB_ERR) {
                    LRESULT item_data = SendMessage(list, LB_GETITEMDATA, sel, 0);
                    if (item_data == 0 || item_data == 1) {
                        data->selected_index = sel;
                    } else {
                        SendMessage(list, LB_SETCURSEL, -1, 0);
                        data->selected_index = -1;
                    }
                    UpdateDescriptionText(data);
                    EnableWindow(GetDlgItem(dlg, IDC_BTN_NEXT), data->selected_index >= 0);
                    EnableWindow(GetDlgItem(dlg, IDC_BTN_DELETE_CACHE), item_data == 0);
                }
            } else if (HIWORD(wp) == LBN_DBLCLK) {
                HWND list = GetDlgItem(dlg, IDC_IMAGE_LIST);
                int sel = static_cast<int>(SendMessage(list, LB_GETCURSEL, 0, 0));
                if (sel != LB_ERR) {
                    LRESULT item_data = SendMessage(list, LB_GETITEMDATA, sel, 0);
                    if (item_data == 0 || item_data == 1) {
                        SendMessage(dlg, WM_COMMAND, IDC_BTN_NEXT, 0);
                    }
                }
            }
            return 0;

        case IDC_BTN_DELETE_CACHE: {
            image_source::ImageEntry cached_img;
            if (!TryGetSelectedCachedImage(data, &cached_img)) return 0;

            char confirm_buf[512];
            snprintf(confirm_buf, sizeof(confirm_buf),
                i18n::tr(i18n::S::kImgConfirmDeleteCacheMsg), cached_img.display_name.c_str());
            int ans = MessageBoxA(dlg, confirm_buf,
                i18n::tr(i18n::S::kImgConfirmDeleteCacheTitle),
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (ans != IDYES) return 0;

            std::string cache_dir = image_source::ImageCacheDir(data->mgr->data_dir(), cached_img);
            std::error_code ec;
            fs::remove_all(cache_dir, ec);
            if (ec) {
                std::string err = i18n::fmt(i18n::S::kImgCacheDeleteFailed, ec.message().c_str());
                MessageBoxA(dlg, err.c_str(), i18n::tr(i18n::S::kError), MB_OK | MB_ICONERROR);
                return 0;
            }

            data->cached_images = image_source::GetCachedImages(data->mgr->data_dir());
            data->selected_index = -1;
            RefreshImageList(data);
            UpdateDescriptionText(data);
            EnableWindow(GetDlgItem(dlg, IDC_BTN_NEXT), FALSE);
            EnableWindow(GetDlgItem(dlg, IDC_BTN_DELETE_CACHE), FALSE);
            SetWindowTextA(GetDlgItem(dlg, IDC_STATUS_TEXT), i18n::tr(i18n::S::kImgCacheDeleted));
            return 0;
        }

        case IDC_BTN_RETRY:
            data->download_error.clear();
            if (!data->sources_loaded) {
                data->loading_sources = false;
                FetchSources(data);
            } else {
                data->online_loaded = false;
                data->loading_online = false;
                FetchOnlineImages(data);
            }
            return 0;

        case IDC_BTN_BACK:
            if (data->current_page == Page::kConfirm) {
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
                                    if (image_source::IsImageCached(data->mgr->data_dir(), img)) {
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
                char name_buf[256]{};
                GetDlgItemTextA(dlg, IDC_NAME_EDIT, name_buf, sizeof(name_buf));

                int mem_idx = static_cast<int>(SendMessage(GetDlgItem(dlg, IDC_MEMORY_COMBO), CB_GETCURSEL, 0, 0));
                int cpu_idx = static_cast<int>(SendMessage(GetDlgItem(dlg, IDC_CPU_COMBO), CB_GETCURSEL, 0, 0));

                std::string cache_dir = image_source::ImageCacheDir(
                    data->mgr->data_dir(), data->selected_image);

                VmCreateRequest req;
                req.name = name_buf;
                req.memory_mb = (mem_idx >= 0 && mem_idx < kNumOptions) ? kMemoryOptionsMb[mem_idx] : 4096;
                req.cpu_count = (cpu_idx >= 0 && cpu_idx < kNumOptions) ? kCpuOptions[cpu_idx] : 4;
                req.nat_enabled = IsDlgButtonChecked(dlg, IDC_NAT_CHECK) == BST_CHECKED;

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
                    MessageBoxA(dlg, v.message.c_str(), i18n::tr(i18n::S::kValidationError), MB_OK | MB_ICONWARNING);
                    return 0;
                }

                std::string error;
                if (data->mgr->CreateVm(req, &error)) {
                    data->created = true;
                    data->closed = true;
                    DestroyWindow(dlg);
                } else {
                    MessageBoxA(dlg, error.c_str(), i18n::tr(i18n::S::kError), MB_OK | MB_ICONERROR);
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
        DestroyWindow(dlg);
        return 0;

    case WM_NCDESTROY:
        RemoveWindowSubclass(dlg, DlgSubclassProc, 1);
        break;
    }
    return DefSubclassProc(dlg, msg, wp, lp);
}

static const char* kDialogClassName = "TenBoxCreateVmDlg";
static bool g_class_registered = false;

static void RegisterDialogClass() {
    if (g_class_registered) return;

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kDialogClassName;

    RegisterClassExA(&wc);
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

    HWND dlg = CreateWindowExA(
        WS_EX_DLGMODALFRAME,
        kDialogClassName, i18n::tr(i18n::S::kDlgCreateVm),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, dlg_w, dlg_h,
        parent, nullptr, GetModuleHandle(nullptr), nullptr);

    if (!dlg) {
        if (error) *error = "Failed to create dialog";
        return false;
    }

    data.dlg = dlg;
    SetWindowSubclass(dlg, DlgSubclassProc, 1, reinterpret_cast<DWORD_PTR>(&data));

    data.cached_images = image_source::GetCachedImages(data.mgr->data_dir());

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
    int desc_h = scale_px(52);
    int list_top = margin + row_h + scale_px(8);
    int bottom_btns_y = h - margin - btn_h;
    int status_y = bottom_btns_y - scale_px(28);
    int list_h = status_y - list_top - desc_h - scale_px(10);
    if (list_h < scale_px(120)) list_h = scale_px(120);

    int top_ctrl_y = margin + (row_h - btn_h) / 2;
    CreateWindowExA(0, "STATIC", i18n::tr(i18n::S::kImgSource),
        WS_CHILD | SS_LEFT | SS_CENTERIMAGE, margin, margin, source_label_w, row_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_SOURCE_LABEL)), nullptr, nullptr);

    int combo_x = margin + source_label_w + scale_px(6);
    int combo_w = w - combo_x - margin - load_btn_w - scale_px(10);
    CreateWindowExA(0, "COMBOBOX", "",
        WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        combo_x, top_ctrl_y, combo_w, 200,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_SOURCE_COMBO)), nullptr, nullptr);

    CreateWindowExA(0, "BUTTON", i18n::tr(i18n::S::kImgBtnLoad),
        WS_CHILD | BS_PUSHBUTTON,
        w - margin - load_btn_w, top_ctrl_y, load_btn_w, btn_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_BTN_LOAD)), nullptr, nullptr);

    CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
        WS_CHILD | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        margin, list_top, w - 2 * margin, list_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_IMAGE_LIST)), nullptr, nullptr);

    CreateWindowExA(0, "STATIC", "",
        WS_CHILD | SS_LEFT,
        margin, list_top + list_h + scale_px(6), w - 2 * margin, desc_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_DESC_TEXT)), nullptr, nullptr);

    CreateWindowExA(0, "STATIC", "",
        WS_CHILD | SS_LEFT,
        margin, status_y, w - 2 * margin, scale_px(22),
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_STATUS_TEXT)), nullptr, nullptr);

    CreateWindowExA(0, PROGRESS_CLASSA, "",
        WS_CHILD | PBS_SMOOTH,
        margin, margin + scale_px(58), w - 2 * margin, scale_px(24),
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_PROGRESS)), nullptr, nullptr);
    SendMessage(GetDlgItem(dlg, IDC_PROGRESS), PBM_SETRANGE, 0, MAKELPARAM(0, 100));

    CreateWindowExA(0, "STATIC", "",
        WS_CHILD | SS_LEFT,
        margin, margin + scale_px(92), w - 2 * margin, scale_px(48),
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_PROGRESS_TEXT)), nullptr, nullptr);

    int label_w = scale_px(78), edit_x = margin + label_w + scale_px(8), edit_w = w - edit_x - margin;
    int form_row_h = (btn_h > scale_px(32)) ? btn_h : scale_px(32);
    int label_y_off = (form_row_h - scale_px(20)) / 2;
    int edit_h = scale_px(26);
    int combo_drop_h = scale_px(220);
    int ctrl_y = margin;
    CreateWindowExA(0, "STATIC", i18n::tr(i18n::S::kDlgLabelName),
        WS_CHILD | SS_LEFT, margin, ctrl_y + label_y_off, label_w, scale_px(20),
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_NAME_LABEL)), nullptr, nullptr);
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | ES_AUTOHSCROLL, edit_x, ctrl_y, edit_w, edit_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_NAME_EDIT)), nullptr, nullptr);
    ctrl_y += form_row_h;

    CreateWindowExA(0, "STATIC", i18n::tr(i18n::S::kDlgLabelMemory),
        WS_CHILD | SS_LEFT, margin, ctrl_y + label_y_off, label_w, scale_px(20),
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_MEMORY_LABEL)), nullptr, nullptr);
    CreateWindowExA(0, "COMBOBOX", "",
        WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, edit_x, ctrl_y, edit_w, combo_drop_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_MEMORY_COMBO)), nullptr, nullptr);
    ctrl_y += form_row_h;

    CreateWindowExA(0, "STATIC", i18n::tr(i18n::S::kDlgLabelVcpus),
        WS_CHILD | SS_LEFT, margin, ctrl_y + label_y_off, label_w, scale_px(20),
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_CPU_LABEL)), nullptr, nullptr);
    CreateWindowExA(0, "COMBOBOX", "",
        WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, edit_x, ctrl_y, edit_w, combo_drop_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_CPU_COMBO)), nullptr, nullptr);
    ctrl_y += form_row_h;

    CreateWindowExA(0, "BUTTON", i18n::tr(i18n::S::kDlgEnableNat),
        WS_CHILD | BS_AUTOCHECKBOX, edit_x, ctrl_y + scale_px(4), edit_w, scale_px(22),
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_NAT_CHECK)), nullptr, nullptr);

    int btn_gap = scale_px(10);
    int secondary_btn_w = scale_px(130);
    int back_x = w - margin - 2 * btn_w - btn_gap;
    int delete_max_w = back_x - margin - btn_gap;
    if (secondary_btn_w > delete_max_w) secondary_btn_w = delete_max_w;
    if (secondary_btn_w < scale_px(90)) secondary_btn_w = scale_px(90);
    int retry_x = w - margin - btn_w;
    int delete_x = margin;

    CreateWindowExA(0, "BUTTON", i18n::tr(i18n::S::kImgBtnDeleteCache),
        WS_CHILD | BS_PUSHBUTTON, delete_x, bottom_btns_y, secondary_btn_w, btn_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_BTN_DELETE_CACHE)), nullptr, nullptr);
    CreateWindowExA(0, "BUTTON", i18n::tr(i18n::S::kImgBtnRetry),
        WS_CHILD | BS_PUSHBUTTON, retry_x, status_y - scale_px(6) - btn_h, btn_w, btn_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_BTN_RETRY)), nullptr, nullptr);

    CreateWindowExA(0, "BUTTON", i18n::tr(i18n::S::kImgBtnBack),
        WS_CHILD | BS_PUSHBUTTON, w - margin - 2 * btn_w - btn_gap, bottom_btns_y, btn_w, btn_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_BTN_BACK)), nullptr, nullptr);

    CreateWindowExA(0, "BUTTON", i18n::tr(i18n::S::kImgBtnNext),
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, w - margin - btn_w, bottom_btns_y, btn_w, btn_h,
        dlg, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_BTN_NEXT)), nullptr, nullptr);

    HFONT ui_font = nullptr;
    NONCLIENTMETRICSA ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        LOGFONTA lf = ncm.lfMessageFont;
        // Use a slightly larger message font for this custom dialog.
        lf.lfHeight = -MulDiv(10, static_cast<int>(dpi), 72);
        ui_font = CreateFontIndirectA(&lf);
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
        // Keep combo slightly shorter than push buttons for visual balance.
        int combo_sel_h = btn_h - scale_px(4);
        if (combo_sel_h < scale_px(24)) combo_sel_h = scale_px(24);
        int combo_item_h = combo_sel_h - scale_px(4);
        if (combo_item_h < scale_px(20)) combo_item_h = scale_px(20);
        SendMessage(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), static_cast<LPARAM>(combo_sel_h));
        SendMessage(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(0), static_cast<LPARAM>(combo_item_h));
    };
    set_combo_heights(IDC_SOURCE_COMBO);
    set_combo_heights(IDC_MEMORY_COMBO);
    set_combo_heights(IDC_CPU_COMBO);

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

    // Delete only if we created a private font object.
    if (ui_font && ui_font != reinterpret_cast<HFONT>(SendMessage(parent, WM_GETFONT, 0, 0))
        && ui_font != static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT))) {
        DeleteObject(ui_font);
    }

    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    if (error) *error = data.error;
    return data.created;
}
