#include "manager/ui/agent_tools_dialog.h"

#include "manager/agent_tools_service.h"
#include "manager/app_settings.h"
#include "manager/i18n.h"
#include "manager/ui/dlg_builder.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using agent_tools::AgentKind;
using agent_tools::MigrationOptions;
using agent_tools::SkillConflictStrategy;

enum Id {
    IDC_AGENT_KIND = 2101,
    IDC_HEALTH,
    IDC_BACKUP,
    IDC_RESTORE,
    IDC_EXPORT,
    IDC_IMPORT,
    IDC_RESTART,
    IDC_RESET,
    IDC_DIAG,
    IDC_SOURCE_VM,
    IDC_STRATEGY,
    IDC_WORKSPACE,
    IDC_MIGRATE,
    IDC_SCHEDULE_ENABLED,
    IDC_SCHEDULE_TIME,
    IDC_SCHEDULE_KEEP,
    IDC_SCHEDULE_SAVE,
    IDC_OPEN_BACKUPS,
    IDC_OUTPUT
};

constexpr UINT WM_AGENT_RESULT = WM_APP + 71;
constexpr UINT WM_AGENT_PROGRESS = WM_APP + 72;

struct VmChoice {
    std::string id;
    std::string name;
};

struct PostedResult {
    agent_tools::ToolResult result;
};

struct PostedProgress {
    std::string step;
    std::string message;
    std::string detail;
};

std::string Text(const char* en, const char* zh) {
    return i18n::GetCurrentLanguage() == i18n::Lang::kChineseSimplified ? zh : en;
}

struct DialogData {
    ManagerService& manager;
    agent_tools::AgentToolsService tools;
    std::string vm_id;
    std::vector<VmChoice> source_vms;
    bool busy = false;

    DialogData(ManagerService& mgr, std::string id)
        : manager(mgr), tools(mgr, mgr.data_dir()), vm_id(std::move(id)) {}
};

std::string ScheduleKey(const std::string& vm_id, AgentKind agent) {
    return vm_id + "|" + agent_tools::AgentRawValue(agent);
}

AgentKind SelectedAgent(HWND dlg) {
    int idx = static_cast<int>(SendDlgItemMessageW(dlg, IDC_AGENT_KIND, CB_GETCURSEL, 0, 0));
    return idx == 1 ? AgentKind::kOpenClaw : AgentKind::kHermes;
}

void AppendOutput(HWND dlg, const std::string& text) {
    HWND out = GetDlgItem(dlg, IDC_OUTPUT);
    int len = GetWindowTextLengthW(out);
    std::wstring w = i18n::to_wide(text + "\r\n");
    SendMessageW(out, EM_SETSEL, len, len);
    SendMessageW(out, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(w.c_str()));
}

void SetBusy(HWND dlg, DialogData* data, bool busy) {
    data->busy = busy;
    for (int id : {IDC_HEALTH, IDC_BACKUP, IDC_RESTORE, IDC_EXPORT, IDC_IMPORT,
                   IDC_RESTART, IDC_RESET, IDC_DIAG, IDC_MIGRATE, IDC_SCHEDULE_SAVE}) {
        EnableWindow(GetDlgItem(dlg, id), busy ? FALSE : TRUE);
    }
}

std::string SaveFileDialog(HWND dlg, const std::string& filename) {
    wchar_t file_buf[MAX_PATH]{};
    MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, file_buf, MAX_PATH);
    static constexpr wchar_t filter[] = L"Agent Profile (*.tar.gz)\0*.tar.gz\0All Files\0*.*\0\0";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dlg;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file_buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    return GetSaveFileNameW(&ofn) ? i18n::wide_to_utf8(file_buf) : std::string{};
}

std::string OpenProfileDialog(HWND dlg) {
    return BrowseForFile(dlg, "Agent Profile (*.tar.gz)\0*.tar.gz\0All Files\0*.*\0\0", "");
}

int ScheduleKeep(HWND dlg) {
    wchar_t buf[16]{};
    GetDlgItemTextW(dlg, IDC_SCHEDULE_KEEP, buf, 16);
    int v = _wtoi(buf);
    return std::clamp(v, 1, 99);
}

void LoadSchedule(HWND dlg, DialogData* data) {
    auto agent = SelectedAgent(dlg);
    auto key = ScheduleKey(data->vm_id, agent);
    settings::AgentBackupSchedule schedule;
    auto it = data->manager.app_settings().agent_backup_schedules.find(key);
    if (it != data->manager.app_settings().agent_backup_schedules.end()) {
        schedule = it->second;
    }
    CheckDlgButton(dlg, IDC_SCHEDULE_ENABLED, schedule.enabled ? BST_CHECKED : BST_UNCHECKED);
    wchar_t time_buf[16]{};
    swprintf_s(time_buf, L"%02d:%02d", schedule.hour, schedule.minute);
    SetDlgItemTextW(dlg, IDC_SCHEDULE_TIME, time_buf);
    SetDlgItemTextW(dlg, IDC_SCHEDULE_KEEP, i18n::to_wide(std::to_string(schedule.keep_count)).c_str());
}

void SaveSchedule(HWND dlg, DialogData* data) {
    wchar_t time_buf[32]{};
    GetDlgItemTextW(dlg, IDC_SCHEDULE_TIME, time_buf, 32);
    int hour = 3, minute = 0;
    swscanf_s(time_buf, L"%d:%d", &hour, &minute);
    settings::AgentBackupSchedule schedule;
    schedule.enabled = IsDlgButtonChecked(dlg, IDC_SCHEDULE_ENABLED) == BST_CHECKED;
    schedule.hour = std::clamp(hour, 0, 23);
    schedule.minute = std::clamp(minute, 0, 59);
    schedule.keep_count = ScheduleKeep(dlg);
    data->manager.app_settings().agent_backup_schedules[ScheduleKey(data->vm_id, SelectedAgent(dlg))] = schedule;
    data->manager.SaveAppSettings();
    data->tools.RotateBackups(data->vm_id, SelectedAgent(dlg), schedule.keep_count);
    AppendOutput(dlg, Text("Scheduled backup settings saved", "定时备份设置已保存"));
}

void RefreshSources(HWND dlg, DialogData* data) {
    data->source_vms.clear();
    HWND combo = GetDlgItem(dlg, IDC_SOURCE_VM);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const auto& rec : data->manager.ListVms()) {
        if (rec.spec.vm_id == data->vm_id) continue;
        if (rec.state != VmPowerState::kRunning || !rec.guest_agent_connected) continue;
        data->source_vms.push_back({rec.spec.vm_id, rec.spec.name});
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::to_wide(rec.spec.name).c_str()));
    }
    if (!data->source_vms.empty()) SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

void StartOp(HWND dlg, DialogData* data, const std::string& label,
             std::function<void(agent_tools::ToolCallback)> run) {
    if (data->busy) return;
    SetBusy(dlg, data, true);
    AppendOutput(dlg, Text("Start: ", "开始：") + label);
    run([dlg](agent_tools::ToolResult result) {
        PostMessageW(dlg, WM_AGENT_RESULT, 0, reinterpret_cast<LPARAM>(new PostedResult{std::move(result)}));
    });
}

void OpenBackups(HWND dlg, DialogData* data) {
    std::filesystem::path dir = std::filesystem::path(data->manager.data_dir()) /
        "AgentBackups" / data->vm_id / agent_tools::AgentRawValue(SelectedAgent(dlg));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ShellExecuteW(dlg, L"open", i18n::to_wide(dir.string()).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void InitDialog(HWND dlg, DialogData* data) {
    CenterDialogToParent(dlg);
    HWND agent = GetDlgItem(dlg, IDC_AGENT_KIND);
    SendMessageW(agent, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Hermes"));
    SendMessageW(agent, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"OpenClaw"));
    SendMessageW(agent, CB_SETCURSEL, 0, 0);

    HWND strategy = GetDlgItem(dlg, IDC_STRATEGY);
    SendMessageW(strategy, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::to_wide(Text("Keep Hermes skills", "技能保留 Hermes")).c_str()));
    SendMessageW(strategy, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::to_wide(Text("Overwrite Hermes skills", "技能覆盖 Hermes")).c_str()));
    SendMessageW(strategy, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::to_wide(Text("Rename imported skills", "技能重命名导入")).c_str()));
    SendMessageW(strategy, CB_SETCURSEL, 0, 0);
    SetDlgItemTextW(dlg, IDC_WORKSPACE, L"/home/tenbox/.hermes/workspace/openclaw-migrated");
    SendDlgItemMessageW(dlg, IDC_OUTPUT, EM_SETLIMITTEXT, 1024 * 1024, 0);
    RefreshSources(dlg, data);
    LoadSchedule(dlg, data);
}

INT_PTR CALLBACK Proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<DialogData*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG:
        data = reinterpret_cast<DialogData*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, reinterpret_cast<LONG_PTR>(data));
        InitDialog(dlg, data);
        return TRUE;

    case WM_AGENT_PROGRESS: {
        std::unique_ptr<PostedProgress> p(reinterpret_cast<PostedProgress*>(lp));
        AppendOutput(dlg, p->message + (p->detail.empty() ? "" : " - " + p->detail));
        return TRUE;
    }

    case WM_AGENT_RESULT: {
        std::unique_ptr<PostedResult> r(reinterpret_cast<PostedResult*>(lp));
        SetBusy(dlg, data, false);
        AppendOutput(dlg, std::string(r->result.ok ? Text("Done: ", "完成：") : Text("Failed: ", "失败：")) + r->result.message);
        if (!r->result.output.empty()) AppendOutput(dlg, r->result.output);
        RefreshSources(dlg, data);
        return TRUE;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == IDCANCEL) {
            if (data && data->busy) {
                AppendOutput(dlg, Text("Operation is running. Please wait for it to finish before closing.", "操作执行中，请等待完成后关闭。"));
                return TRUE;
            }
            EndDialog(dlg, 0);
            return TRUE;
        }
        if (id == IDC_AGENT_KIND && HIWORD(wp) == CBN_SELCHANGE) {
            LoadSchedule(dlg, data);
            return TRUE;
        }
        const AgentKind agent = SelectedAgent(dlg);
        const int keep = ScheduleKeep(dlg);
        switch (id) {
        case IDC_HEALTH:
            StartOp(dlg, data, Text("Run diagnosis", "一键诊断"), [=](auto cb) { data->tools.HealthStatus(data->vm_id, agent, cb); });
            return TRUE;
        case IDC_BACKUP:
            StartOp(dlg, data, Text("Back Up Now", "立即备份"), [=](auto cb) { data->tools.SnapshotBackup(data->vm_id, agent, keep, cb); });
            return TRUE;
        case IDC_RESTORE: {
            auto backups = data->tools.ListBackups(data->vm_id, agent);
            if (backups.empty()) {
                AppendOutput(dlg, Text("No restorable backup was found", "没有找到可恢复的备份"));
                return TRUE;
            }
            if (MessageBoxW(dlg, i18n::to_wide(Text("Restore will overwrite current Agent data. Restore latest backup?", "恢复会覆盖当前 Agent 数据，确认恢复最新备份？")).c_str(), i18n::to_wide(Text("Confirm restore", "确认恢复")).c_str(), MB_OKCANCEL | MB_ICONWARNING) == IDOK) {
                StartOp(dlg, data, Text("Restore Latest Backup", "恢复最新备份"), [=](auto cb) { data->tools.RestoreBackup(data->vm_id, agent, backups.front().path, cb); });
            }
            return TRUE;
        }
        case IDC_EXPORT: {
            std::string path = SaveFileDialog(dlg, std::string(agent_tools::AgentRawValue(agent)) + "-profile.tar.gz");
            if (!path.empty()) StartOp(dlg, data, Text("Export Migration Package", "导出迁移包"), [=](auto cb) { data->tools.ExportProfile(data->vm_id, agent, path, cb); });
            return TRUE;
        }
        case IDC_IMPORT: {
            std::string path = OpenProfileDialog(dlg);
            if (!path.empty() && MessageBoxW(dlg, i18n::to_wide(Text("Import will replace current Agent data. Continue?", "导入会替换当前 Agent 数据，确认继续？")).c_str(), i18n::to_wide(Text("Confirm import", "确认导入")).c_str(), MB_OKCANCEL | MB_ICONWARNING) == IDOK) {
                StartOp(dlg, data, Text("Import Migration Package", "导入迁移包"), [=](auto cb) { data->tools.ImportProfile(data->vm_id, agent, path, cb); });
            }
            return TRUE;
        }
        case IDC_RESTART:
            StartOp(dlg, data, Text("Restart", "重启服务"), [=](auto cb) { data->tools.RestartAgent(data->vm_id, agent, keep, cb); });
            return TRUE;
        case IDC_RESET:
            if (MessageBoxW(dlg, i18n::to_wide(Text("Reset will overwrite current Agent model configuration. Continue?", "重置会覆盖当前 Agent 模型配置，确认继续？")).c_str(), i18n::to_wide(Text("Confirm reset", "确认重置")).c_str(), MB_OKCANCEL | MB_ICONWARNING) == IDOK) {
                StartOp(dlg, data, Text("Reset Config", "重置配置"), [=](auto cb) { data->tools.ResetAgentConfig(data->vm_id, agent, keep, cb); });
            }
            return TRUE;
        case IDC_DIAG:
            StartOp(dlg, data, Text("Export Diagnostics", "导出诊断包"), [=](auto cb) { data->tools.ExportDiagnostics(data->vm_id, agent, cb); });
            return TRUE;
        case IDC_MIGRATE: {
            int sel = static_cast<int>(SendDlgItemMessageW(dlg, IDC_SOURCE_VM, CB_GETCURSEL, 0, 0));
            if (sel < 0 || sel >= static_cast<int>(data->source_vms.size())) {
                AppendOutput(dlg, Text("Select a running OpenClaw source VM first", "请先选择运行中的 OpenClaw 来源 VM"));
                return TRUE;
            }
            if (MessageBoxW(dlg, i18n::to_wide(Text("Migration will back up target Hermes, then run dry-run and apply. Continue?", "迁移会先备份目标 Hermes，再执行 dry-run 和正式迁移。确认继续？")).c_str(), i18n::to_wide(Text("Confirm migration", "确认迁移")).c_str(), MB_OKCANCEL | MB_ICONWARNING) != IDOK)
                return TRUE;
            wchar_t workspace[512]{};
            GetDlgItemTextW(dlg, IDC_WORKSPACE, workspace, 512);
            int st = static_cast<int>(SendDlgItemMessageW(dlg, IDC_STRATEGY, CB_GETCURSEL, 0, 0));
            MigrationOptions options;
            options.workspace_target = i18n::wide_to_utf8(workspace);
            options.skill_conflict = st == 1 ? SkillConflictStrategy::kOverwrite :
                                     st == 2 ? SkillConflictStrategy::kRename :
                                               SkillConflictStrategy::kSkip;
            std::string source_id = data->source_vms[sel].id;
            StartOp(dlg, data, Text("OpenClaw to Hermes Migration", "OpenClaw 到 Hermes 迁移"), [=](auto cb) {
                data->tools.MigrateOpenClawToHermes(source_id, data->vm_id, options, keep,
                    [dlg](const std::string& step, const std::string& message, const std::string& detail) {
                        PostMessageW(dlg, WM_AGENT_PROGRESS, 0, reinterpret_cast<LPARAM>(new PostedProgress{step, message, detail}));
                    },
                    cb);
            });
            return TRUE;
        }
        case IDC_SCHEDULE_SAVE:
            SaveSchedule(dlg, data);
            return TRUE;
        case IDC_OPEN_BACKUPS:
            OpenBackups(dlg, data);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

}  // namespace

void ShowAgentToolsDialog(HWND parent, ManagerService& mgr, const std::string& vm_id) {
    DlgBuilder b;
    b.Begin(Text("Agent Toolbox", "Agent 急救箱").c_str(), 0, 0, 610, 430, WS_CAPTION | WS_SYSMENU);
    b.AddStatic(-1, "Agent:", 12, 12, 45, 12);
    b.AddComboBox(IDC_AGENT_KIND, 60, 10, 110, 80);
    b.AddButton(IDC_HEALTH, Text("Run diagnosis", "一键诊断").c_str(), 185, 9, 72, 16);
    b.AddButton(IDC_BACKUP, Text("Back Up Now", "立即备份").c_str(), 262, 9, 72, 16);
    b.AddButton(IDC_RESTORE, Text("Restore Latest", "恢复最新").c_str(), 339, 9, 72, 16);
    b.AddButton(IDC_OPEN_BACKUPS, Text("Open Backups", "打开备份").c_str(), 416, 9, 72, 16);

    b.AddButton(IDC_EXPORT, Text("Export", "导出包").c_str(), 12, 38, 66, 16);
    b.AddButton(IDC_IMPORT, Text("Import", "导入包").c_str(), 84, 38, 66, 16);
    b.AddButton(IDC_RESTART, Text("Restart", "重启服务").c_str(), 156, 38, 72, 16);
    b.AddButton(IDC_RESET, Text("Reset Config", "重置配置").c_str(), 234, 38, 72, 16);
    b.AddButton(IDC_DIAG, Text("Diagnostics", "导出诊断").c_str(), 312, 38, 72, 16);

    b.AddCheckBox(IDC_SCHEDULE_ENABLED, Text("Scheduled", "定时备份").c_str(), 400, 39, 70, 14);
    b.AddEdit(IDC_SCHEDULE_TIME, 472, 38, 45, 15);
    b.AddStatic(-1, Text("Keep", "保留").c_str(), 522, 40, 24, 12);
    b.AddEdit(IDC_SCHEDULE_KEEP, 548, 38, 24, 15);
    b.AddButton(IDC_SCHEDULE_SAVE, Text("Save", "保存").c_str(), 576, 38, 28, 16);

    b.AddStatic(-1, Text("Migrate OpenClaw to this Hermes:", "OpenClaw 迁移到当前 Hermes:").c_str(), 12, 72, 150, 12);
    b.AddComboBox(IDC_SOURCE_VM, 165, 69, 120, 100);
    b.AddComboBox(IDC_STRATEGY, 292, 69, 120, 100);
    b.AddEdit(IDC_WORKSPACE, 418, 69, 115, 15);
    b.AddButton(IDC_MIGRATE, Text("Migrate", "自动迁移").c_str(), 540, 68, 58, 17);

    b.AddEdit(IDC_OUTPUT, 12, 98, 586, 300, ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL);
    b.AddButton(IDCANCEL, Text("Close", "关闭").c_str(), 540, 405, 58, 17);

    DialogData data(mgr, vm_id);
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), b.Build(), parent, Proc, reinterpret_cast<LPARAM>(&data));
}
