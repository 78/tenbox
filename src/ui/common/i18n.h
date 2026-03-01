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
    kLabelKernel,
    kLabelDisk,
    kLabelMemory,
    kLabelVcpus,
    kLabelNat,

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

    // Shared folders tab
    kSfColTag,
    kSfColHostPath,
    kSfColMode,
    kSfBtnAdd,
    kSfBtnRemove,
    kSfModeReadOnly,
    kSfModeReadWrite,
    kSfBrowseTitle,
    kSfNoSelection,
    kSfConfirmRemoveTitle,
    kSfConfirmRemoveMsg,

    kCount  // Must be last
};

void InitLanguage();
Lang GetCurrentLanguage();
void SetLanguage(Lang lang);
const char* tr(S id);

// Format a translated string with arguments (wraps snprintf)
template<typename... Args>
std::string fmt(S id, Args&&... args) {
    char buf[512];
    snprintf(buf, sizeof(buf), tr(id), std::forward<Args>(args)...);
    return buf;
}

}  // namespace i18n
