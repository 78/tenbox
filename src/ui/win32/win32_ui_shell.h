#pragma once

#include "manager/manager_service.h"

#include <functional>
#include <memory>
#include <string>

class Win32UiShell {
public:
    Win32UiShell(ManagerService& manager);
    ~Win32UiShell();

    void Show();
    void Hide();
    void Run();
    void Quit();
    void RefreshVmList();

    static void InvokeOnUiThread(std::function<void()> fn);
    static void SetClipboardFromVm(bool value);

    struct Impl;
    ManagerService& manager_;

private:
    std::unique_ptr<Impl> impl_;
};
