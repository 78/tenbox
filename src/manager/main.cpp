#include "manager/manager_service.h"
#include "manager/app_settings.h"
#include "manager/llm_proxy.h"
#include "manager/i18n.h"
#include "version.h"

#include "manager/ui/win32_ui_shell.h"
using UiShell = Win32UiShell;

#include <winsparkle.h>

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <ctime>
#include <io.h>

static FILE* g_log_file = nullptr;

FILE* GetManagerLogFile() { return g_log_file; }

static void InitLogFile() {
    wchar_t path[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path)))
        return;
    
    std::filesystem::path log_dir = path;
    log_dir /= L"TenBox";
    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    
    std::filesystem::path log_path = log_dir / L"manager.log";
    _wfopen_s(&g_log_file, log_path.c_str(), L"a");
    if (g_log_file) {
        setvbuf(g_log_file, nullptr, _IOLBF, BUFSIZ);

        // Write startup marker
        time_t now = time(nullptr);
        struct tm local_tm;
        localtime_s(&local_tm, &now);
        char time_buf[64];
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &local_tm);
        fprintf(g_log_file, "\n=== TenBox Manager started at %s ===\n", time_buf);

        // Redirect stderr to log file (best-effort; may be no-op on GUI apps).
        _dup2(_fileno(g_log_file), _fileno(stderr));
    }
}

static void CloseLogFile() {
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = nullptr;
    }
}

static std::string ResolveDefaultRuntimeExePath() {
    wchar_t self[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return "tenbox-vm-runtime.exe";
    }
    std::string path = i18n::wide_to_utf8(self);
    size_t sep = path.find_last_of("\\/");
    if (sep == std::string::npos) {
        return "tenbox-vm-runtime.exe";
    }
    path.resize(sep + 1);
    path += "tenbox-vm-runtime.exe";
    return path;
}

static void PrintUsage(const char* prog, const char* default_runtime) {
    fprintf(stderr,
        "TenBox manager v" TENBOX_VERSION "\n"
        "Usage: %s [--runtime-exe <path>]\n"
        "  --runtime-exe is optional. Default: %s\n",
        prog, default_runtime);
}

static constexpr const wchar_t* kMutexName = L"TenBoxManager_SingleInstance";
static constexpr const wchar_t* kWndClass = L"TenBoxManagerWin32";

static bool ActivateExistingInstance() {
    HWND hwnd = FindWindowW(kWndClass, nullptr);
    if (hwnd) {
        if (IsIconic(hwnd))
            ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        return true;
    }
    return false;
}

enum class HvStatus { kAvailable, kNoDll, kNotEnabled };

static HvStatus CheckHypervisorStatus() {
    HMODULE hMod = LoadLibraryW(L"WinHvPlatform.dll");
    if (!hMod) return HvStatus::kNoDll;

    // WHV_CAPABILITY_CODE 0x0000 = WHvCapabilityCodeHypervisorPresent
    using GetCapFn = HRESULT(WINAPI*)(int, void*, UINT32, UINT32*);
    auto pGetCap = reinterpret_cast<GetCapFn>(
        GetProcAddress(hMod, "WHvGetCapability"));
    if (!pGetCap) {
        FreeLibrary(hMod);
        return HvStatus::kNoDll;
    }

    struct { BOOL HypervisorPresent; } cap{};
    UINT32 written = 0;
    HRESULT hr = pGetCap(0, &cap, sizeof(cap), &written);
    FreeLibrary(hMod);
    if (SUCCEEDED(hr) && cap.HypervisorPresent)
        return HvStatus::kAvailable;
    return HvStatus::kNotEnabled;
}

static bool EnableHypervisorPlatform() {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = L"dism.exe";
    sei.lpParameters =
        L"/online /enable-feature "
        L"/featurename:HypervisorPlatform "
        L"/featurename:VirtualMachinePlatform "
        L"/all /norestart";
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei))
        return false;

    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(sei.hProcess, &exitCode);
        CloseHandle(sei.hProcess);
        // 3010 = ERROR_SUCCESS_REBOOT_REQUIRED
        if (exitCode != 0 && exitCode != 3010)
            return false;

        // Ensure the hypervisor is set to launch at boot.
        // Some software (e.g. older VMware/VirtualBox) may have set it to "off".
        SHELLEXECUTEINFOW bcd{};
        bcd.cbSize = sizeof(bcd);
        bcd.fMask = SEE_MASK_NOCLOSEPROCESS;
        bcd.lpVerb = L"runas";
        bcd.lpFile = L"bcdedit.exe";
        bcd.lpParameters = L"/set hypervisorlaunchtype auto";
        bcd.nShow = SW_HIDE;
        if (ShellExecuteExW(&bcd) && bcd.hProcess) {
            WaitForSingleObject(bcd.hProcess, INFINITE);
            CloseHandle(bcd.hProcess);
        }

        return true;
    }
    return false;
}

// Returns true if the application should continue, false if it should exit.
static bool CheckHypervisorAndPrompt() {
    HvStatus status = CheckHypervisorStatus();
    if (status == HvStatus::kAvailable)
        return true;

    if (status == HvStatus::kNoDll) {
        MessageBoxW(nullptr,
            i18n::tr_w(i18n::S::kHvNoDllMessage).c_str(),
            i18n::tr_w(i18n::S::kHvCheckTitle).c_str(),
            MB_OK | MB_ICONERROR);
        return true;
    }

    const auto title = i18n::tr_w(i18n::S::kHvCheckTitle);
    const auto message = i18n::tr_w(i18n::S::kHvCheckMessage);
    const auto btnAuto = i18n::tr_w(i18n::S::kHvBtnAutoEnable);
    const auto btnManual = i18n::tr_w(i18n::S::kHvBtnManualOpen);
    const auto btnIgnore = i18n::tr_w(i18n::S::kHvBtnIgnore);

    TASKDIALOG_BUTTON buttons[] = {
        { 1001, btnAuto.c_str() },
        { 1002, btnManual.c_str() },
        { 1003, btnIgnore.c_str() },
    };

    TASKDIALOGCONFIG tdc{};
    tdc.cbSize = sizeof(tdc);
    tdc.dwFlags = TDF_USE_COMMAND_LINKS;
    tdc.pszWindowTitle = title.c_str();
    tdc.pszMainIcon = TD_WARNING_ICON;
    tdc.pszContent = message.c_str();
    tdc.cButtons = _countof(buttons);
    tdc.pButtons = buttons;
    tdc.nDefaultButton = 1001;

    int clicked = 0;
    HRESULT hr = TaskDialogIndirect(&tdc, &clicked, nullptr, nullptr);
    if (FAILED(hr))
        return true;

    if (clicked == 1001) {
        if (EnableHypervisorPlatform()) {
            int answer = MessageBoxW(nullptr,
                i18n::tr_w(i18n::S::kHvEnableSuccessMsg).c_str(),
                i18n::tr_w(i18n::S::kHvEnableSuccessTitle).c_str(),
                MB_YESNO | MB_ICONINFORMATION);
            if (answer == IDYES) {
                // InitiateSystemShutdownEx with EWX_REBOOT flag
                HANDLE hToken;
                if (OpenProcessToken(GetCurrentProcess(),
                        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
                    TOKEN_PRIVILEGES tp{};
                    LookupPrivilegeValueA(nullptr, "SeShutdownPrivilege",
                                          &tp.Privileges[0].Luid);
                    tp.PrivilegeCount = 1;
                    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                    AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr);
                    CloseHandle(hToken);
                }
                ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, SHTDN_REASON_MINOR_OTHER);
            }
            return false;
        } else {
            MessageBoxW(nullptr,
                i18n::tr_w(i18n::S::kHvEnableFailMsg).c_str(),
                i18n::tr_w(i18n::S::kHvEnableFailTitle).c_str(),
                MB_OK | MB_ICONERROR);
        }
    } else if (clicked == 1002) {
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.lpVerb = L"open";
        sei.lpFile = L"optionalfeatures.exe";
        sei.nShow = SW_SHOWNORMAL;
        ShellExecuteExW(&sei);
        return false;
    }
    return true;
}

static int RunManagerApp(int argc, char* argv[]) {
    InitLogFile();
    
    HANDLE hMutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        ActivateExistingInstance();
        CloseLogFile();
        return 0;
    }

    std::string runtime_exe = ResolveDefaultRuntimeExePath();

    for (int i = 1; i < argc; ++i) {
        auto Arg = [&](const char* flag) { return std::strcmp(argv[i], flag) == 0; };
        auto NextArg = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            return nullptr;
        };
        if (Arg("--runtime-exe")) {
            auto v = NextArg(); if (!v) return 1;
            runtime_exe = v;
        } else if (Arg("--help") || Arg("-h")) {
            PrintUsage(argv[0], runtime_exe.c_str());
            return 0;
        }
    }

    std::wstring runtime_exe_w = i18n::to_wide(runtime_exe);
    DWORD attrs = GetFileAttributesW(runtime_exe_w.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        fprintf(stderr, "runtime executable not found: %s\n", runtime_exe.c_str());
        PrintUsage(argv[0], ResolveDefaultRuntimeExePath().c_str());
        return 1;
    }

    i18n::InitLanguage();
    if (!CheckHypervisorAndPrompt()) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    std::string data_dir = settings::GetDataDir();

    ManagerService manager(runtime_exe, data_dir);
    manager.set_hypervisor_available(CheckHypervisorStatus() == HvStatus::kAvailable);

    // Start LLM proxy service if enabled
    static constexpr uint32_t kLlmGuestFwdIp = 0x0A000203; // 10.0.2.3
    static constexpr uint16_t kLlmGuestFwdPort = 80;

    llm_proxy::LlmProxyService llm_proxy(manager.app_settings().llm_proxy);

    auto LogProxy = [](const char* fmt, ...) {
        if (FILE* f = GetManagerLogFile()) {
            va_list ap;
            va_start(ap, fmt);
            fprintf(f, "[llm-proxy] ");
            vfprintf(f, fmt, ap);
            fprintf(f, "\r\n");
            fflush(f);
            va_end(ap);
        }
    };

    auto StartProxyIfNeeded = [&]() {
        if (llm_proxy.port() == 0 && !manager.app_settings().llm_proxy.mappings.empty()) {
            LogProxy("Starting proxy (%zu mappings)...",
                     manager.app_settings().llm_proxy.mappings.size());
            if (llm_proxy.Start()) {
                GuestForward gf;
                gf.guest_ip = kLlmGuestFwdIp;
                gf.guest_port = kLlmGuestFwdPort;
                gf.host_port = llm_proxy.port();
                LogProxy("Proxy started on port %u, setting guestfwd 10.0.2.3:80 -> 127.0.0.1:%u",
                         llm_proxy.port(), llm_proxy.port());
                manager.UpdateGlobalGuestForwards({gf});
            } else {
                LogProxy("Proxy failed to start");
            }
        } else {
            LogProxy("Proxy not needed (port=%u, mappings=%zu)",
                     llm_proxy.port(), manager.app_settings().llm_proxy.mappings.size());
        }
    };

    StartProxyIfNeeded();

    manager.SetLlmProxyChangedCallback(
        [&](const settings::LlmProxySettings& settings) {
            llm_proxy.UpdateSettings(settings);
            if (settings.mappings.empty()) {
                llm_proxy.Stop();
                manager.UpdateGlobalGuestForwards({});
            } else {
                StartProxyIfNeeded();
            }
        });

    // Set up clipboard callbacks for VM <-> Host clipboard sharing
    manager.SetClipboardGrabCallback([&](const std::string& vm_id, const std::vector<uint32_t>& types) {
        for (uint32_t type : types) {
            if (type == 1) {  // VD_AGENT_CLIPBOARD_UTF8_TEXT
                manager.SendClipboardRequest(vm_id, type);
                break;
            }
        }
    });

    manager.SetClipboardDataCallback([&](const std::string& vm_id, uint32_t type,
                                         const std::vector<uint8_t>& data) {
        if (type == 1 && !data.empty()) {  // VD_AGENT_CLIPBOARD_UTF8_TEXT
            UiShell::SetClipboardFromVm(true);
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                int wlen = MultiByteToWideChar(CP_UTF8, 0,
                    reinterpret_cast<const char*>(data.data()),
                    static_cast<int>(data.size()), nullptr, 0);
                if (wlen > 0) {
                    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (wlen + 1) * sizeof(wchar_t));
                    if (hMem) {
                        wchar_t* pMem = static_cast<wchar_t*>(GlobalLock(hMem));
                        if (pMem) {
                            MultiByteToWideChar(CP_UTF8, 0,
                                reinterpret_cast<const char*>(data.data()),
                                static_cast<int>(data.size()), pMem, wlen);
                            pMem[wlen] = L'\0';
                            GlobalUnlock(hMem);
                            SetClipboardData(CF_UNICODETEXT, hMem);
                        }
                    }
                }
                CloseClipboard();
            }
        }
    });

    manager.SetClipboardRequestCallback([&](const std::string& vm_id, uint32_t type) {
        if (type == 1) {  // VD_AGENT_CLIPBOARD_UTF8_TEXT
            if (OpenClipboard(nullptr)) {
                HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                if (hData) {
                    wchar_t* pData = static_cast<wchar_t*>(GlobalLock(hData));
                    if (pData) {
                        int utf8_len = WideCharToMultiByte(CP_UTF8, 0, pData, -1, nullptr, 0, nullptr, nullptr);
                        if (utf8_len > 0) {
                            std::vector<uint8_t> utf8_data(utf8_len);
                            WideCharToMultiByte(CP_UTF8, 0, pData, -1,
                                reinterpret_cast<char*>(utf8_data.data()), utf8_len, nullptr, nullptr);
                            if (!utf8_data.empty() && utf8_data.back() == 0) {
                                utf8_data.pop_back();
                            }
                            manager.SendClipboardData(vm_id, type, utf8_data.data(), utf8_data.size());
                        }
                        GlobalUnlock(hData);
                    }
                }
                CloseClipboard();
            }
        }
    });

    UiShell ui(manager);

    ui.Show();

    win_sparkle_set_appcast_url("https://tenbox.ai/api/appcast.xml");
    win_sparkle_set_automatic_check_for_updates(1);
    win_sparkle_set_update_check_interval(86400);
    win_sparkle_init();
    // win_sparkle_check_update_without_ui();

    ui.Run();

    win_sparkle_cleanup();
    manager.ShutdownAll();
    llm_proxy.Stop();
    if (hMutex) CloseHandle(hMutex);
    CloseLogFile();
    return 0;
}

int main(int argc, char* argv[]) {
    return RunManagerApp(argc, argv);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Use the standard GUI entry point so the binary layout matches
    // a typical Windows desktop application more closely.
    return RunManagerApp(__argc, __argv);
}
