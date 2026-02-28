#include "ui/common/i18n.h"
#include "version.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace i18n {

static Lang g_current_lang = Lang::kEnglish;

// English strings; order must match enum S
static const char* kStringsEn[] = {
    "TenBox Manager",                    // kAppTitle
    "Manager",                           // kMenuManager
    "VM",                                // kMenuVm
    "New VM\tCtrl+N",                    // kMenuNewVm
    "Exit\tAlt+F4",                      // kMenuExit
    "Edit...",                           // kMenuEdit
    "Delete",                            // kMenuDelete
    "Start",                             // kMenuStart
    "Stop",                              // kMenuStop
    "Reboot",                            // kMenuReboot
    "Shutdown",                          // kMenuShutdown
    "New VM",                            // kToolbarNewVm
    "Edit",                              // kToolbarEdit
    "Delete",                            // kToolbarDelete
    "Start",                             // kToolbarStart
    "Stop",                              // kToolbarStop
    "Reboot",                            // kToolbarReboot
    "Shutdown",                          // kToolbarShutdown
    "Shared Folders",                    // kToolbarSharedFolders
    "Basic Info",                        // kTabInfo
    "Console",                           // kTabConsole
    "Screen",                            // kTabDisplay
    "Shared Folders",                    // kTabSharedFolders
    "ID:",                               // kLabelId
    "Location:",                         // kLabelLocation
    "Kernel:",                           // kLabelKernel
    "Disk:",                             // kLabelDisk
    "Memory:",                           // kLabelMemory
    "vCPUs:",                            // kLabelVcpus
    "NAT:",                              // kLabelNat
    "Running",                           // kStateRunning
    "Stopped",                           // kStateStopped
    "Starting",                          // kStateStarting
    "Stopping",                          // kStateStopping
    "Crashed",                           // kStateCrashed
    "%u VM(s) loaded",                   // kStatusVmsLoaded
    "Starting %s...",                    // kStatusStarting
    "%s started",                        // kStatusStarted
    "%s stopped",                       // kStatusStopped
    "%s rebooted",                       // kStatusRebooted
    "%s shutting down...",              // kStatusShuttingDown
    "VM deleted",                        // kStatusVmDeleted
    "%s updated",                        // kStatusVmUpdated
    "Error: ",                           // kStatusErrorPrefix
    "%u vCPU, %u MB RAM",                // kDetailVcpuRam
    "Create New VM",                     // kDlgCreateVm
    "Edit VM",                           // kDlgEditVm
    "Edit - ",                           // kDlgEditTitlePrefix
    "Name:",                             // kDlgLabelName
    "Kernel:",                           // kDlgLabelKernel
    "Initrd:",                           // kDlgLabelInitrd
    "Disk:",                             // kDlgLabelDisk
    "Memory:",                           // kDlgLabelMemory
    "vCPUs:",                            // kDlgLabelVcpus
    "Location:",                         // kDlgLabelLocation
    "Enable NAT networking",             // kDlgEnableNat
    "Create",                            // kDlgBtnCreate
    "Save",                              // kDlgBtnSave
    "Cancel",                            // kDlgBtnCancel
    "Close",                             // kDlgBtnClose
    "Shared Folders",                    // kDlgSharedFolders
    "Delete VM",                         // kConfirmDeleteTitle
    "Are you sure you want to delete '%s'?\nThis will remove all VM files permanently.",  // kConfirmDeleteMsg
    "Confirm Exit",                      // kConfirmExitTitle
    "%u VM(s) still running. Are you sure you want to exit?\n\nAll running VMs will be forcibly terminated.",  // kConfirmExitMsg
    "Force Stop VM",                     // kConfirmForceStopTitle
    "Are you sure you want to force stop '%s'?\n\nThis will immediately terminate the VM without graceful shutdown. Any unsaved data may be lost.",  // kConfirmForceStopMsg
    "Error",                             // kError
    "Validation Error",                  // kValidationError
    "Send",                              // kSend
    "Type command and press Enter...",  // kConsolePlaceholder
    "Enabled",                           // kNatEnabled
    "Disabled",                          // kNatDisabled
    "(none)",                            // kNone
    "CPU / Memory changes require VM to be stopped",  // kCpuMemoryChangeWarning
    "Full input capture (system keys) | Press Right Alt to release",  // kDisplayHintCaptured
    "Click to capture system keys",  // kDisplayHintNormal
    "Help",                                  // kMenuHelp
    "Website",                               // kMenuWebsite
    "Check for Updates",                     // kMenuCheckUpdate
    "About...",                              // kMenuAbout
    "About " TENBOX_PRODUCT_NAME,            // kAboutTitle
    TENBOX_PRODUCT_NAME " Manager\nVersion " TENBOX_VERSION_STR "\n\nA lightweight virtual machine manager for Windows.\n\n" TENBOX_COPYRIGHT,  // kAboutText
    "Tag",                                   // kSfColTag
    "Host Path",                             // kSfColHostPath
    "Mode",                                  // kSfColMode
    "Add...",                                // kSfBtnAdd
    "Remove",                                // kSfBtnRemove
    "Read Only",                             // kSfModeReadOnly
    "Read/Write",                            // kSfModeReadWrite
    "Select folder to share",                // kSfBrowseTitle
    "Please select a shared folder to remove.",  // kSfNoSelection
    "Confirm Remove",                        // kSfConfirmRemoveTitle
    "Remove shared folder '%s'?",            // kSfConfirmRemoveMsg
};

// Simplified Chinese strings; order must match enum S
static const char* kStringsZhCN[] = {
    "TenBox 管理器",                     // kAppTitle
    "管理",                              // kMenuManager
    "虚拟机",                            // kMenuVm
    "新建虚拟机\tCtrl+N",                // kMenuNewVm
    "退出\tAlt+F4",                      // kMenuExit
    "编辑...",                           // kMenuEdit
    "删除",                              // kMenuDelete
    "启动",                              // kMenuStart
    "停止",                              // kMenuStop
    "重启",                              // kMenuReboot
    "关机",                              // kMenuShutdown
    "新建虚拟机",                        // kToolbarNewVm
    "编辑",                              // kToolbarEdit
    "删除",                              // kToolbarDelete
    "启动",                              // kToolbarStart
    "停止",                              // kToolbarStop
    "重启",                              // kToolbarReboot
    "关机",                              // kToolbarShutdown
    "共享文件夹",                        // kToolbarSharedFolders
    "基本信息",                          // kTabInfo
    "控制台",                            // kTabConsole
    "屏幕显示",                          // kTabDisplay
    "共享文件夹",                        // kTabSharedFolders
    "标识:",                             // kLabelId
    "位置:",                             // kLabelLocation
    "内核:",                             // kLabelKernel
    "磁盘:",                             // kLabelDisk
    "内存:",                             // kLabelMemory
    "vCPU:",                           // kLabelVcpus
    "网络:",                             // kLabelNat
    "运行中",                            // kStateRunning
    "已停止",                            // kStateStopped
    "启动中",                            // kStateStarting
    "停止中",                            // kStateStopping
    "崩溃",                              // kStateCrashed
    "已加载 %u 个虚拟机",                // kStatusVmsLoaded
    "正在启动 %s...",                    // kStatusStarting
    "%s 已启动",                         // kStatusStarted
    "%s 已停止",                         // kStatusStopped
    "%s 已重启",                         // kStatusRebooted
    "%s 正在关机...",                    // kStatusShuttingDown
    "虚拟机已删除",                      // kStatusVmDeleted
    "%s 已更新",                         // kStatusVmUpdated
    "错误: ",                            // kStatusErrorPrefix
    "%u vCPU，%u MB 内存",             // kDetailVcpuRam
    "新建虚拟机",                        // kDlgCreateVm
    "编辑虚拟机",                        // kDlgEditVm
    "编辑 - ",                           // kDlgEditTitlePrefix
    "名称:",                             // kDlgLabelName
    "内核:",                             // kDlgLabelKernel
    "初始化磁盘:",                       // kDlgLabelInitrd
    "磁盘:",                             // kDlgLabelDisk
    "内存:",                             // kDlgLabelMemory
    "vCPU:",                           // kDlgLabelVcpus
    "位置:",                             // kDlgLabelLocation
    "启用 NAT 网络",                     // kDlgEnableNat
    "创建",                              // kDlgBtnCreate
    "保存",                              // kDlgBtnSave
    "取消",                              // kDlgBtnCancel
    "关闭",                              // kDlgBtnClose
    "共享文件夹",                        // kDlgSharedFolders
    "删除虚拟机",                        // kConfirmDeleteTitle
    "确认删除 '%s' 吗？\n此操作将永久删除该虚拟机的所有文件。",  // kConfirmDeleteMsg
    "确认退出",                          // kConfirmExitTitle
    "仍有 %u 个虚拟机在运行。确认退出吗？\n\n所有正在运行的虚拟机将被强制终止。",  // kConfirmExitMsg
    "强制停止",                          // kConfirmForceStopTitle
    "确认强制停止 '%s' 吗？\n\n此操作将立即终止虚拟机而不进行正常关机，未保存的数据可能会丢失。",  // kConfirmForceStopMsg
    "错误",                              // kError
    "验证错误",                          // kValidationError
    "发送",                              // kSend
    "输入命令并按 Enter...",             // kConsolePlaceholder
    "已启用",                            // kNatEnabled
    "已禁用",                            // kNatDisabled
    "无",                                // kNone
    "更改 CPU/内存需要先停止虚拟机",     // kCpuMemoryChangeWarning
    "已捕获全部输入（含系统键）| 按右 Alt 释放",  // kDisplayHintCaptured
    "点击以捕获系统键",  // kDisplayHintNormal
    "帮助",                                  // kMenuHelp
    "官方网站",                              // kMenuWebsite
    "检查更新",                              // kMenuCheckUpdate
    "关于...",                               // kMenuAbout
    "关于 " TENBOX_PRODUCT_NAME,             // kAboutTitle
    TENBOX_PRODUCT_NAME " 管理器\n版本 " TENBOX_VERSION_STR "\n\n一款轻量级的 Windows 虚拟机管理器。\n\n" TENBOX_COPYRIGHT,  // kAboutText
    "标签",                                  // kSfColTag
    "主机路径",                              // kSfColHostPath
    "模式",                                  // kSfColMode
    "添加...",                               // kSfBtnAdd
    "移除",                                  // kSfBtnRemove
    "只读",                                  // kSfModeReadOnly
    "读写",                                  // kSfModeReadWrite
    "选择要共享的文件夹",                     // kSfBrowseTitle
    "请先选择要移除的共享文件夹。",           // kSfNoSelection
    "确认移除",                              // kSfConfirmRemoveTitle
    "确认移除共享文件夹 '%s'？",             // kSfConfirmRemoveMsg
};

void InitLanguage() {
    LANGID lang = GetUserDefaultUILanguage();
    WORD primary = PRIMARYLANGID(lang);
    WORD sub = SUBLANGID(lang);

    if (primary == LANG_CHINESE &&
        (sub == SUBLANG_CHINESE_SIMPLIFIED ||
         sub == SUBLANG_CHINESE_SINGAPORE)) {
        g_current_lang = Lang::kChineseSimplified;
    } else {
        g_current_lang = Lang::kEnglish;
    }
}

Lang GetCurrentLanguage() {
    return g_current_lang;
}

void SetLanguage(Lang lang) {
    g_current_lang = lang;
}

const char* tr(S id) {
    int idx = static_cast<int>(id);
    if (idx < 0 || idx >= static_cast<int>(S::kCount)) return "";
    return (g_current_lang == Lang::kChineseSimplified)
        ? kStringsZhCN[idx] : kStringsEn[idx];
}

}  // namespace i18n
