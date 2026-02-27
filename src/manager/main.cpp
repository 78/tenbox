#include "manager/manager_service.h"
#include "manager/app_settings.h"
#include "platform/tray/windows/tray_app.h"
#include "version.h"

#include "ui/win32/win32_ui_shell.h"
using UiShell = Win32UiShell;

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static std::string ResolveDefaultRuntimeExePath() {
    char self[MAX_PATH]{};
    DWORD len = GetModuleFileNameA(nullptr, self, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return "tenbox-vm-runtime.exe";
    }
    std::string path(self, len);
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

int main(int argc, char* argv[]) {
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

    DWORD attrs = GetFileAttributesA(runtime_exe.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        fprintf(stderr, "runtime executable not found: %s\n", runtime_exe.c_str());
        PrintUsage(argv[0], ResolveDefaultRuntimeExePath().c_str());
        return 1;
    }

    std::string data_dir = settings::GetDataDir();

    ManagerService manager(runtime_exe, data_dir);

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

    WindowsTrayApp tray(
        "TenBox Manager",
        [&]() {
            UiShell::InvokeOnUiThread([&]() { ui.Show(); });
        },
        [&]() {
            UiShell::InvokeOnUiThread([&]() {
                manager.ShutdownAll();
                ui.Quit();
            });
        });

    std::thread tray_thread([&]() {
        if (tray.Init()) tray.Run();
    });

    ui.Show();
    ui.Run();

    tray.RequestExit();
    if (tray_thread.joinable()) tray_thread.join();

    manager.ShutdownAll();
    return 0;
}
