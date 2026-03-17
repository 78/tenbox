#pragma once

#include <cstdio>
#include <string>
#include <utility>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace i18n {

// Convert UTF-8 to UTF-16 for Wide API calls
inline std::wstring to_wide(const char* utf8) {
    if (!utf8 || !*utf8) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring result(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, result.data(), len);
    return result;
}

inline std::wstring to_wide(const std::string& utf8) {
    return to_wide(utf8.c_str());
}

// Convert UTF-16 to UTF-8 for results from Wide API calls
inline std::string wide_to_utf8(const wchar_t* wide) {
    if (!wide || !*wide) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string result(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), len, nullptr, nullptr);
    return result;
}

inline std::string wide_to_utf8(const std::wstring& wide) {
    return wide_to_utf8(wide.c_str());
}

enum class Lang { kEnglish, kChineseSimplified };

enum class S {
    // Window titles
    kAppTitle,

    // Menus
    kMenuManager,
    kMenuVm,
    kMenuNewVm,
    kMenuExit,
    kMenuEdit,
    kMenuDelete,
    kMenuStart,
    kMenuStop,
    kMenuReboot,
    kMenuShutdown,

    // Toolbar
    kToolbarNewVm,
    kToolbarEdit,
    kToolbarDelete,
    kToolbarStart,
    kToolbarStop,
    kToolbarReboot,
    kToolbarShutdown,
    kToolbarSharedFolders,

    // Tabs
    kTabInfo,
    kTabConsole,
    kTabDisplay,
    kTabSharedFolders,

    // Detail panel labels
    kLabelId,
    kLabelLocation,
    kLabelCreatedTime,
    kLabelLastBootTime,
    kLabelMemory,
    kLabelVcpus,
    kLabelNat,
    kLabelDebug,

    // VM states
    kStateRunning,
    kStateStopped,
    kStateStarting,
    kStateStopping,
    kStateCrashed,

    // Status messages (format strings: use with snprintf)
    kStatusVmsLoaded,
    kStatusStarting,
    kStatusStarted,
    kStatusStopped,
    kStatusRebooted,
    kStatusShuttingDown,
    kStatusVmDeleted,
    kStatusVmUpdated,
    kStatusErrorPrefix,

    // List item detail format
    kDetailVcpuRam,

    // Dialogs
    kDlgCreateVm,
    kDlgEditVm,
    kDlgEditTitlePrefix,
    kDlgLabelName,
    kDlgLabelKernel,
    kDlgLabelInitrd,
    kDlgLabelDisk,
    kDlgLabelMemory,
    kDlgLabelVcpus,
    kDlgLabelLocation,
    kDlgEnableNat,
    kDlgDebugMode,
    kDlgBtnCreate,
    kDlgBtnSave,
    kDlgBtnCancel,
    kDlgBtnBrowse,
    kDlgBtnClose,
    kDlgSharedFolders,

    // Confirmations
    kConfirmDeleteTitle,
    kConfirmDeleteMsg,
    kConfirmExitTitle,
    kConfirmExitMsg,
    kConfirmForceStopTitle,
    kConfirmForceStopMsg,

    // Errors & misc
    kError,
    kValidationError,
    kSend,
    kConsolePlaceholder,
    kNatEnabled,
    kNatDisabled,
    kDebugEnabled,
    kDebugDisabled,
    kNone,
    kCpuMemoryChangeWarning,

    // Display panel hints
    kDisplayHintCaptured,
    kDisplayHintNormal,

    // View menu
    kMenuView,
    kMenuViewToolbar,

    // Help menu
    kMenuHelp,
    kMenuWebsite,
    kMenuCheckUpdate,
    kMenuAbout,
    kAboutTitle,
    kAboutText,

    // Hypervisor check
    kHvCheckTitle,
    kHvCheckMessage,
    kHvNoDllMessage,
    kHvBtnAutoEnable,
    kHvBtnManualOpen,
    kHvBtnIgnore,
    kHvEnableSuccessTitle,
    kHvEnableSuccessMsg,
    kHvEnableFailTitle,
    kHvEnableFailMsg,

    // Shared folders tab
    kSfColTag,
    kSfColHostPath,
    kSfColMode,
    kSfBtnAdd,
    kSfBtnRemove,
    kSfBtnOpen,
    kSfModeReadOnly,
    kSfModeReadWrite,
    kSfBrowseTitle,
    kSfNoSelection,
    kSfConfirmRemoveTitle,
    kSfConfirmRemoveMsg,
    kSfHint,

    // Port forwards dialog
    kDlgPortForwards,
    kPfColHostPort,
    kPfColGuestPort,
    kPfBtnAdd,
    kPfBtnRemove,
    kPfBtnOpen,
    kPfNoSelection,
    kPfConfirmRemoveTitle,
    kPfConfirmRemoveMsg,
    kPfAddTitle,
    kPfLabelHostIp,
    kPfLabelHostPort,
    kPfLabelGuestIp,
    kPfLabelGuestPort,
    kPfInvalidPort,
    kPfDuplicatePort,
    kToolbarPortForwards,
    kToolbarDpiZoom,
    kToolbarLlmProxy,
    kMenuPortForwards,
    kMenuClone,
    kStatusVmCloned,
    kStatusVmCloning,
    kPfLabelLan,
    kPfBindErrorTitle,
    kPfBindErrorMsg,

    // Guest forward (in port forward dialog)
    kGfTitle,
    kGfColGuestAddr,
    kGfColHostAddr,
    kGfBtnAdd,
    kGfBtnRemove,
    kGfAddTitle,
    kGfLabelGuestIp,
    kGfLabelGuestPort,
    kGfLabelHostAddr,
    kGfLabelHostPort,
    kGfInvalidParams,
    kGfDuplicateEntry,

    // Online image dialog
    kImgFetchOnline,
    kImgLoadingOnline,
    kImgGroupCached,
    kImgGroupOnline,
    kImgDownloading,
    kImgDownloadingFile,
    kImgBtnNext,
    kImgBtnBack,
    kImgBtnLoad,
    kImgBtnRefresh,
    kImgBtnDeleteCache,
    kImgSource,
    kImgNoDescription,
    kImgConfirmDeleteCacheTitle,
    kImgConfirmDeleteCacheMsg,
    kImgCacheDeleted,
    kImgCacheDeleteFailed,
    kImgEta,
    kImgBtnLocalImage,
    kImgLocalNoFiles,

    // Auto-update
    kUpdateAvailableTitle,
    kUpdateAvailableMsg,
    kUpdateNow,
    kUpdateSkip,
    kUpdateDownloading,
    kUpdateDownloadProgress,
    kUpdateDownloadFailed,
    kUpdateInstalling,
    kUpdateLatest,

    // Settings dialog
    kMenuSettings,
    kDlgSettings,
    kSettingsVmStorageDir,
    kSettingsImageCacheDir,
    kSettingsDefault,
    kSettingsBrowse,
    kSettingsReset,
    kSettingsCacheSize,
    kSettingsClearCache,
    kSettingsCacheCleared,
    kSettingsConfirmClearTitle,
    kSettingsConfirmClearMsg,
    kSettingsOldCacheTitle,
    kSettingsOldCacheMsg,
    kSettingsMigrate,
    kSettingsDelete,

    // StartVm errors
    kErrHvNotEnabled,
    kErrVmNotFound,
    kErrLaunchRuntimeFailed,
    kErrVmDisappearedDuringStart,
    kErrIpcConnectionFailed,

    // LLM Proxy dialog
    kMenuLlmProxy,
    kDlgLlmProxy,
    kLlmColAlias,
    kLlmColApiType,
    kLlmColTargetUrl,
    kLlmColModel,
    kLlmBtnAdd,
    kLlmBtnEdit,
    kLlmBtnRemove,
    kLlmAddTitle,
    kLlmEditTitle,
    kLlmLabelAlias,
    kLlmLabelApiType,
    kLlmLabelTargetUrl,
    kLlmLabelApiKey,
    kLlmLabelModel,
    kLlmApiTypeOpenAiCompletions,
    kLlmNoSelection,
    kLlmConfirmRemoveTitle,
    kLlmConfirmRemoveMsg,
    kLlmDuplicateAlias,
    kLlmHint,

    kCount  // Must be last
};

void InitLanguage();
Lang GetCurrentLanguage();
void SetLanguage(Lang lang);
const char* tr(S id);

// Return translated string as wide for Win32 API calls
inline std::wstring tr_w(S id) {
    return to_wide(tr(id));
}

// Format a translated string with arguments (wraps snprintf)
template<typename... Args>
std::string fmt(S id, Args&&... args) {
    char buf[512];
    snprintf(buf, sizeof(buf), tr(id), std::forward<Args>(args)...);
    return buf;
}

}  // namespace i18n
