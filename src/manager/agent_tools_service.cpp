#include "manager/agent_tools_service.h"

#include "manager/app_settings.h"
#include "manager/i18n.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace agent_tools {
namespace fs = std::filesystem;

namespace {

ToolResult Failure(std::string message, std::string output = {}) {
    ToolResult r;
    r.ok = false;
    r.message = std::move(message);
    r.output = std::move(output);
    return r;
}

ToolResult Success(std::string message, std::string output = {}) {
    ToolResult r;
    r.ok = true;
    r.message = std::move(message);
    r.output = std::move(output);
    return r;
}

std::string Text(const char* en, const char* zh) {
    return i18n::GetCurrentLanguage() == i18n::Lang::kChineseSimplified ? zh : en;
}

std::string NormalizeTagSeed(std::string value) {
    value.erase(std::remove(value.begin(), value.end(), '-'), value.end());
    if (value.size() > 8) value.resize(8);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value.empty() ? "00000000" : value;
}

}  // namespace

const char* AgentRawValue(AgentKind agent) {
    return agent == AgentKind::kHermes ? "hermes" : "openclaw";
}

const char* AgentDisplayName(AgentKind agent) {
    return agent == AgentKind::kHermes ? "Hermes" : "OpenClaw";
}

std::string SkillConflictRawValue(SkillConflictStrategy strategy) {
    switch (strategy) {
    case SkillConflictStrategy::kOverwrite: return "overwrite";
    case SkillConflictStrategy::kRename: return "rename";
    case SkillConflictStrategy::kSkip:
    default: return "skip";
    }
}

std::string SkillConflictDisplayName(SkillConflictStrategy strategy) {
    switch (strategy) {
    case SkillConflictStrategy::kOverwrite: return Text("Overwrite Hermes skills", "技能覆盖 Hermes");
    case SkillConflictStrategy::kRename: return Text("Rename imported skills", "技能重命名导入");
    case SkillConflictStrategy::kSkip:
    default: return Text("Keep Hermes skills", "技能保留 Hermes");
    }
}

AgentToolsService::AgentToolsService(ManagerService& manager, std::string data_dir)
    : manager_(manager), data_dir_(std::move(data_dir)) {}

std::string AgentToolsService::Timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d-%H%M%S");
    return os.str();
}

std::string AgentToolsService::PathFilename(const std::string& path) {
    auto u8 = fs::path(path).filename().u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::string AgentToolsService::Dirname(const std::string& path) {
    auto u8 = fs::path(path).parent_path().u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::string AgentToolsService::OperationBaseDirectory() const {
    return (fs::path(data_dir_) / "AgentOperations").string();
}

std::string AgentToolsService::BackupBaseDirectory(const std::string& vm_id) const {
    return (fs::path(data_dir_) / "AgentBackups" / vm_id).string();
}

std::string AgentToolsService::BackupPackageDirectory(const std::string& vm_id, AgentKind agent) const {
    return (fs::path(BackupBaseDirectory(vm_id)) / AgentRawValue(agent)).string();
}

std::string AgentToolsService::NewBackupPackagePath(const std::string& vm_id, AgentKind agent) const {
    return (fs::path(BackupPackageDirectory(vm_id, agent)) /
            ("agent-data-" + Timestamp() + ".tar.gz")).string();
}

std::string AgentToolsService::NewMigrationReportPath(const std::string& vm_id) const {
    return (fs::path(BackupPackageDirectory(vm_id, AgentKind::kHermes)) /
            ("openclaw-migration-" + Timestamp() + ".txt")).string();
}

bool AgentToolsService::IsRunnable(const std::string& vm_id, std::string* error) const {
    auto vm = manager_.GetVm(vm_id);
    if (!vm) {
        if (error) *error = Text("VM was not found", "找不到 VM");
        return false;
    }
    if (vm->state != VmPowerState::kRunning) {
        if (error) *error = Text("VM is not running", "VM 未运行");
        return false;
    }
    if (!vm->guest_agent_connected) {
        if (error) *error = Text("Guest Agent is not connected", "Guest Agent 未连接");
        return false;
    }
    return true;
}

void AgentToolsService::WithOperationShare(const std::vector<std::string>& vm_ids,
                                           ShareCallback cb,
                                           ToolCallback failure_cb) {
    std::error_code ec;
    fs::create_directories(OperationBaseDirectory(), ec);
    if (ec) {
        failure_cb(Failure(Text("Failed to create temporary directory", "创建临时目录失败"), ec.message()));
        return;
    }

    const std::string tag = "tenbox-agent-ops-" + NormalizeTagSeed(settings::GenerateUuid());
    fs::path dir = fs::path(OperationBaseDirectory()) / (tag + "-" + NormalizeTagSeed(settings::GenerateUuid()));
    fs::create_directories(dir, ec);
    if (ec) {
        failure_cb(Failure(Text("Failed to create temporary shared directory", "创建临时共享目录失败"), ec.message()));
        return;
    }

    ShareLease lease;
    lease.folder = SharedFolder{tag, dir.string(), false};
    lease.vm_ids = vm_ids;
    lease.cleanup_dir = dir.string();

    for (const auto& vm_id : vm_ids) {
        std::string error;
        if (!manager_.AddRuntimeSharedFolder(vm_id, lease.folder, &error)) {
            CleanupShare(lease);
            failure_cb(Failure(Text("Failed to mount temporary shared directory", "挂载临时共享目录失败"), error));
            return;
        }
    }
    cb(std::move(lease));
}

void AgentToolsService::WithBackupShare(const std::string& vm_id,
                                        ShareCallback cb,
                                        ToolCallback failure_cb) {
    std::error_code ec;
    fs::create_directories(BackupBaseDirectory(vm_id), ec);
    if (ec) {
        failure_cb(Failure(Text("Failed to create backup directory", "创建备份目录失败"), ec.message()));
        return;
    }
    const std::string tag = "tenbox-agent-backups-" + NormalizeTagSeed(settings::GenerateUuid());

    ShareLease lease;
    lease.folder = SharedFolder{tag, BackupBaseDirectory(vm_id), false};
    lease.vm_ids = {vm_id};

    std::string error;
    if (!manager_.AddRuntimeSharedFolder(vm_id, lease.folder, &error)) {
        failure_cb(Failure(Text("Failed to mount backup directory", "挂载备份目录失败"), error));
        return;
    }
    cb(std::move(lease));
}

void AgentToolsService::CleanupShare(const ShareLease& lease) {
    for (const auto& vm_id : lease.vm_ids) {
        std::string ignored;
        manager_.RemoveRuntimeSharedFolder(vm_id, lease.folder.tag, &ignored);
    }
    if (!lease.cleanup_dir.empty()) {
        std::error_code ec;
        fs::remove_all(lease.cleanup_dir, ec);
    }
}

void AgentToolsService::RunCommand(const std::string& vm_id, const std::string& command,
                                   uint32_t timeout_ms, ToolCallback cb) {
    std::string error;
    if (!IsRunnable(vm_id, &error)) {
        cb(Failure(error));
        return;
    }
    manager_.RunGuestAgentCommand(vm_id, command, timeout_ms,
        [cb = std::move(cb)](ManagerService::GuestExecResult result) mutable {
            const std::string output = result.CombinedOutput();
            if (!result.ok) {
                std::string message = result.error.empty() ? Text("Guest Agent command failed", "Guest Agent 命令执行失败") : result.error;
                if (message.find("/bin/sh") != std::string::npos || message.find("No such file") != std::string::npos) {
                    message = Text("Agent tools require a Linux guest OS.", "Agent 工具箱需要 Linux Guest OS。");
                }
                cb(Failure(message, output));
                return;
            }
            if (result.exit_code != 0) {
                std::string message = output.empty() ? Text("Agent operation failed", "Agent 操作失败") : output;
                if (message.find("Agent tools require a Linux guest OS") != std::string::npos) {
                    message = Text("Agent tools require a Linux guest OS.", "Agent 工具箱需要 Linux Guest OS。");
                }
                cb(Failure(message, output));
                return;
            }
            cb(Success("ok", output));
        });
}

std::vector<BackupPackage> AgentToolsService::ListBackups(const std::string& vm_id, AgentKind agent) const {
    std::vector<BackupPackage> result;
    const fs::path dir = BackupPackageDirectory(vm_id, agent);
    std::error_code ec;
    if (!fs::exists(dir, ec)) return result;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec || !entry.is_regular_file()) continue;
        const auto path = entry.path();
        const auto name_u8 = path.filename().u8string();
        const std::string name(reinterpret_cast<const char*>(name_u8.data()), name_u8.size());
        if (name.rfind("agent-data-", 0) != 0 || path.extension() != ".gz") continue;
        BackupPackage pkg;
        pkg.path = path.string();
        pkg.filename = name;
        pkg.size = static_cast<uint64_t>(entry.file_size(ec));
        auto ft = entry.last_write_time(ec);
        if (!ec) {
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            pkg.modified_at = sctp;
        }
        result.push_back(std::move(pkg));
    }
    std::sort(result.begin(), result.end(),
              [](const BackupPackage& a, const BackupPackage& b) {
                  return a.modified_at > b.modified_at;
              });
    return result;
}

void AgentToolsService::RotateBackups(const std::string& vm_id, AgentKind agent, int keep_count) {
    auto packages = ListBackups(vm_id, agent);
    if (keep_count < 1) keep_count = 1;
    for (size_t i = static_cast<size_t>(keep_count); i < packages.size(); ++i) {
        std::error_code ec;
        fs::remove(packages[i].path, ec);
    }
}

std::string AgentToolsService::ShellQuote(const std::string& value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') out += "'\\''";
        else out.push_back(ch);
    }
    out += "'";
    return out;
}

std::string AgentToolsService::WithSharedFolderReady(const std::string& tag, const std::string& body) {
    const std::string path = "/mnt/shared/" + tag;
    return
        "set -eu\n"
        "share_dir=" + ShellQuote(path) + "\n"
        "i=0\n"
        "while [ \"$i\" -lt 100 ]; do\n"
        "  if [ -d \"$share_dir\" ] && [ -w \"$share_dir\" ]; then break; fi\n"
        "  i=$((i + 1)); sleep 0.2\n"
        "done\n"
        "[ -d \"$share_dir\" ] || { echo \"Shared folder is not mounted: $share_dir\" >&2; exit 1; }\n"
        "[ -w \"$share_dir\" ] || { echo \"Shared folder is not writable: $share_dir\" >&2; exit 1; }\n" +
        body + "\n";
}

bool AgentToolsService::PrepareAgentToolScript(const ShareLease& lease, std::string* error) const {
    std::vector<fs::path> candidates;
#ifdef _WIN32
    wchar_t module_path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        fs::path module_dir = fs::path(module_path).parent_path();
        candidates.push_back(module_dir / "AgentTools" / "agent_tools.sh");
        candidates.push_back(module_dir.parent_path().parent_path() / "src" / "agent_tools" / "guest" / "agent_tools.sh");
    }
#endif
    candidates.push_back(fs::current_path() / "src" / "agent_tools" / "guest" / "agent_tools.sh");

    fs::path source;
    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec)) {
            source = candidate;
            break;
        }
    }
    if (source.empty()) {
        if (error) *error = "Agent tools script was not found.";
        return false;
    }

    fs::path destination = fs::path(lease.folder.host_path) / "agent_tools.sh";
    fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = "Failed to copy Agent tools script: " + ec.message();
        return false;
    }
    return true;
}

std::string AgentToolsService::ScriptInvocation(const std::string& tag, const std::vector<std::string>& args) {
    const std::string script = "/mnt/shared/" + tag + "/agent_tools.sh";
    std::ostringstream os;
    os << "script=" << ShellQuote(script) << "\n"
       << "[ -f \"$script\" ] || { echo \"Agent tools script is missing: $script\" >&2; exit 1; }\n"
       << "chmod +x \"$script\" 2>/dev/null || true\n"
       << "/bin/sh \"$script\"";
    for (const auto& arg : args) {
        os << " " << ShellQuote(arg);
    }
    os << "\n";
    return os.str();
}

std::string AgentToolsService::ScriptCommand(const std::string& tag, const std::vector<std::string>& args) {
    return WithSharedFolderReady(tag, ScriptInvocation(tag, args));
}

void AgentToolsService::ExportProfile(const std::string& vm_id, AgentKind agent,
                                      const std::string& destination_path, ToolCallback cb) {
    ToolCallback failure_cb = cb;
    WithOperationShare({vm_id}, [this, vm_id, agent, destination_path, cb = std::move(cb)](ShareLease lease) mutable {
        std::string script_error;
        if (!PrepareAgentToolScript(lease, &script_error)) {
            CleanupShare(lease);
            cb(Failure(script_error));
            return;
        }
        const std::string package_name = PathFilename(destination_path).empty()
            ? std::string(AgentRawValue(agent)) + "-profile.tar.gz"
            : PathFilename(destination_path);
        const std::string guest_package = "/mnt/shared/" + lease.folder.tag + "/" + package_name;
        const std::string command = ScriptCommand(lease.folder.tag,
            {"export-profile", AgentRawValue(agent), guest_package, "migration"});
        RunCommand(vm_id, command, 420000, [this, lease, destination_path, guest_package, cb = std::move(cb)](ToolResult result) mutable {
            CleanupShare(lease);
            if (!result.ok) {
                cb(result);
                return;
            }
            std::error_code ec;
            fs::copy_file(fs::path(lease.folder.host_path) / PathFilename(guest_package),
                          destination_path, fs::copy_options::overwrite_existing, ec);
            if (ec) cb(Failure(Text("Failed to copy exported package", "复制导出包失败"), ec.message()));
            else cb(Success(Text("Agent data exported", "已导出 Agent 数据"), destination_path));
        });
    }, std::move(failure_cb));
}

void AgentToolsService::ImportProfile(const std::string& vm_id, AgentKind agent,
                                      const std::string& source_path, ToolCallback cb) {
    ToolCallback failure_cb = cb;
    WithOperationShare({vm_id}, [this, vm_id, agent, source_path, cb = std::move(cb)](ShareLease lease) mutable {
        std::string script_error;
        if (!PrepareAgentToolScript(lease, &script_error)) {
            CleanupShare(lease);
            cb(Failure(script_error));
            return;
        }
        const std::string package_name = "tenbox-agent-profile-import.tar.gz";
        std::error_code ec;
        fs::copy_file(source_path, fs::path(lease.folder.host_path) / package_name,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            CleanupShare(lease);
            cb(Failure(Text("Failed to copy import package", "复制导入包失败"), ec.message()));
            return;
        }
        const std::string guest_package = "/mnt/shared/" + lease.folder.tag + "/" + package_name;
        const std::string command = ScriptCommand(lease.folder.tag,
            {"import-profile", AgentRawValue(agent), guest_package});
        RunCommand(vm_id, command, 420000, [this, lease, agent, cb = std::move(cb)](ToolResult result) mutable {
            CleanupShare(lease);
            if (!result.ok) cb(result);
            else cb(Success(Text("Agent data imported", "已导入 Agent 数据"), result.output));
        });
    }, std::move(failure_cb));
}

void AgentToolsService::SnapshotBackup(const std::string& vm_id, AgentKind agent,
                                       int keep_count, ToolCallback cb) {
    std::error_code ec;
    fs::create_directories(BackupPackageDirectory(vm_id, agent), ec);
    if (ec) {
        cb(Failure(Text("Failed to create backup directory", "创建备份目录失败"), ec.message()));
        return;
    }
    const std::string package = NewBackupPackagePath(vm_id, agent);
    ToolCallback failure_cb = cb;
    WithBackupShare(vm_id, [this, vm_id, agent, keep_count, package, cb = std::move(cb)](ShareLease lease) mutable {
        std::string script_error;
        if (!PrepareAgentToolScript(lease, &script_error)) {
            CleanupShare(lease);
            cb(Failure(script_error));
            return;
        }
        const std::string guest_dir = "/mnt/shared/" + lease.folder.tag + "/" + AgentRawValue(agent);
        const std::string guest_package = guest_dir + "/" + PathFilename(package);
        const std::string command = WithSharedFolderReady(
            lease.folder.tag,
            "mkdir -p " + ShellQuote(guest_dir) + "\n" +
            ScriptInvocation(lease.folder.tag, {"export-profile", AgentRawValue(agent), guest_package, "backup"}));
        RunCommand(vm_id, command, 420000, [this, lease, vm_id, agent, keep_count, package, cb = std::move(cb)](ToolResult result) mutable {
            CleanupShare(lease);
            if (!result.ok) {
                cb(result);
                return;
            }
            RotateBackups(vm_id, agent, keep_count);
            cb(Success(Text("Agent data backup created", "已创建 Agent 数据备份"), package));
        });
    }, std::move(failure_cb));
}

void AgentToolsService::RestoreBackup(const std::string& vm_id, AgentKind agent,
                                      const std::string& package_path, ToolCallback cb) {
    ToolCallback failure_cb = cb;
    WithBackupShare(vm_id, [this, vm_id, agent, package_path, cb = std::move(cb)](ShareLease lease) mutable {
        std::string script_error;
        if (!PrepareAgentToolScript(lease, &script_error)) {
            CleanupShare(lease);
            cb(Failure(script_error));
            return;
        }
        const std::string guest_package = "/mnt/shared/" + lease.folder.tag + "/" +
            std::string(AgentRawValue(agent)) + "/" + PathFilename(package_path);
        const std::string command = ScriptCommand(lease.folder.tag,
            {"import-profile", AgentRawValue(agent), guest_package});
        RunCommand(vm_id, command, 420000, [this, lease, package_path, cb = std::move(cb)](ToolResult result) mutable {
            CleanupShare(lease);
            if (!result.ok) cb(result);
            else cb(Success(Text("Agent data backup restored", "已恢复 Agent 数据备份"), package_path));
        });
    }, std::move(failure_cb));
}

void AgentToolsService::RunHealthCommand(const std::string& vm_id, AgentKind agent,
                                         const std::string&,
                                         const std::string& success_message,
                                         ToolCallback cb) {
    ToolCallback failure_cb = cb;
    WithOperationShare({vm_id}, [this, vm_id, agent, success_message, cb = std::move(cb)](ShareLease lease) mutable {
        std::string script_error;
        if (!PrepareAgentToolScript(lease, &script_error)) {
            CleanupShare(lease);
            cb(Failure(script_error));
            return;
        }
        RunCommand(vm_id, ScriptCommand(lease.folder.tag, {"health", AgentRawValue(agent)}), 180000,
            [this, lease, success_message, cb = std::move(cb)](ToolResult result) mutable {
                CleanupShare(lease);
                if (!result.ok) cb(result);
                else cb(Success(success_message, result.output));
            });
    }, std::move(failure_cb));
}

void AgentToolsService::HealthStatus(const std::string& vm_id, AgentKind agent, ToolCallback cb) {
    RunHealthCommand(vm_id, agent, {}, Text("Health status refreshed", "健康状态已更新"), std::move(cb));
}

void AgentToolsService::RunRepairCommand(const std::string& vm_id, AgentKind agent,
                                         const std::vector<std::string>& repair_args,
                                         const std::string& success_message,
                                         int keep_count,
                                         ToolCallback cb) {
    std::error_code ec;
    fs::create_directories(BackupPackageDirectory(vm_id, agent), ec);
    if (ec) {
        cb(Failure(Text("Failed to create backup directory", "创建备份目录失败"), ec.message()));
        return;
    }
    const std::string package = NewBackupPackagePath(vm_id, agent);
    ToolCallback failure_cb = cb;
    WithBackupShare(vm_id, [this, vm_id, agent, repair_args, success_message, keep_count, package, cb = std::move(cb)](ShareLease lease) mutable {
        std::string script_error;
        if (!PrepareAgentToolScript(lease, &script_error)) {
            CleanupShare(lease);
            cb(Failure(script_error));
            return;
        }
        const std::string guest_dir = "/mnt/shared/" + lease.folder.tag + "/" + AgentRawValue(agent);
        const std::string guest_package = guest_dir + "/" + PathFilename(package);
        const std::string command = WithSharedFolderReady(
            lease.folder.tag,
            "mkdir -p " + ShellQuote(guest_dir) + "\n" +
            ScriptInvocation(lease.folder.tag, {"export-profile", AgentRawValue(agent), guest_package, "backup"}) +
            ScriptInvocation(lease.folder.tag, repair_args));
        RunCommand(vm_id, command, 420000,
            [this, lease, vm_id, agent, keep_count, package, success_message, cb = std::move(cb)](ToolResult result) mutable {
                CleanupShare(lease);
                if (!result.ok) {
                    cb(result);
                    return;
                }
                RotateBackups(vm_id, agent, keep_count);
                cb(Success(success_message,
                    Text("Pre-repair backup: ", "修复前备份：") + package + "\n" + result.output));
            });
    }, std::move(failure_cb));
}

void AgentToolsService::RestartAgent(const std::string& vm_id, AgentKind agent,
                                     int keep_count, ToolCallback cb) {
    RunRepairCommand(vm_id, agent, {"restart", AgentRawValue(agent)},
                     Text("Agent restarted", "已重新启动 Agent"), keep_count, std::move(cb));
}

void AgentToolsService::ResetAgentConfig(const std::string& vm_id, AgentKind agent,
                                         int keep_count, ToolCallback cb) {
    RunRepairCommand(vm_id, agent, {"reset-config", AgentRawValue(agent)},
                     Text("Agent configuration reset", "已重置 Agent 配置"), keep_count, std::move(cb));
}

void AgentToolsService::ExportDiagnostics(const std::string& vm_id, AgentKind agent, ToolCallback cb) {
    ToolCallback failure_cb = cb;
    WithBackupShare(vm_id, [this, vm_id, agent, cb = std::move(cb)](ShareLease lease) mutable {
        std::string script_error;
        if (!PrepareAgentToolScript(lease, &script_error)) {
            CleanupShare(lease);
            cb(Failure(script_error));
            return;
        }
        const std::string guest_dir = "/mnt/shared/" + lease.folder.tag;
        const std::string command = ScriptCommand(lease.folder.tag,
            {"diagnostics", AgentRawValue(agent), guest_dir});
        RunCommand(vm_id, command, 180000, [this, lease, cb = std::move(cb)](ToolResult result) mutable {
            CleanupShare(lease);
            if (!result.ok) cb(result);
            else cb(Success(Text("Diagnostics exported", "已导出诊断包"), result.output));
        });
    }, std::move(failure_cb));
}

void AgentToolsService::MigrateOpenClawToHermes(const std::string& source_vm_id,
                                                const std::string& target_vm_id,
                                                const MigrationOptions& options,
                                                int keep_count,
                                                ProgressCallback progress,
                                                ToolCallback cb) {
    std::string source_error;
    if (!IsRunnable(source_vm_id, &source_error)) {
        cb(Failure(Text("OpenClaw source VM ", "OpenClaw 来源 VM ") + source_error));
        return;
    }
    std::string target_error;
    if (!IsRunnable(target_vm_id, &target_error)) {
        cb(Failure(Text("Hermes target VM ", "Hermes 目标 VM ") + target_error));
        return;
    }

    std::error_code ec;
    fs::create_directories(BackupPackageDirectory(target_vm_id, AgentKind::kHermes), ec);
    if (ec) {
        cb(Failure(Text("Failed to create migration directory", "创建迁移目录失败"), ec.message()));
        return;
    }
    const std::string backup_package = NewBackupPackagePath(target_vm_id, AgentKind::kHermes);
    const std::string report_path = NewMigrationReportPath(target_vm_id);

    ToolCallback backup_failure_cb = cb;
    WithBackupShare(target_vm_id,
        [this, source_vm_id, target_vm_id, options, keep_count, progress, cb = std::move(cb), backup_package, report_path](ShareLease backup_lease) mutable {
            std::string script_error;
            if (!PrepareAgentToolScript(backup_lease, &script_error)) {
                CleanupShare(backup_lease);
                cb(Failure(script_error));
                return;
            }
            ToolCallback op_failure_cb = cb;
            WithOperationShare({source_vm_id, target_vm_id},
                [this, source_vm_id, target_vm_id, options, keep_count, progress, cb = std::move(cb), backup_package, report_path, backup_lease](ShareLease op_lease) mutable {
                    auto cleanup_all = [this, backup_lease, op_lease]() {
                        CleanupShare(op_lease);
                        CleanupShare(backup_lease);
                    };
                    std::string script_error;
                    if (!PrepareAgentToolScript(op_lease, &script_error)) {
                        cleanup_all();
                        cb(Failure(script_error));
                        return;
                    }
                    const std::string guest_backup_dir = "/mnt/shared/" + backup_lease.folder.tag + "/hermes";
                    const std::string guest_backup = guest_backup_dir + "/" + PathFilename(backup_package);
                    const std::string guest_report = guest_backup_dir + "/" + PathFilename(report_path);
                    const std::string backup_command = WithSharedFolderReady(
                        backup_lease.folder.tag,
                        "mkdir -p " + ShellQuote(guest_backup_dir) + "\n" +
                        ScriptInvocation(backup_lease.folder.tag, {"export-profile", AgentRawValue(AgentKind::kHermes), guest_backup, "backup"}));
                    if (progress) progress("backup", Text("Creating target Hermes pre-migration backup", "正在创建目标 Hermes 迁移前备份"), PathFilename(backup_package));
                    RunCommand(target_vm_id, backup_command, 420000,
                        [this, source_vm_id, target_vm_id, options, keep_count, progress, cb = std::move(cb), backup_package, report_path, op_lease, backup_lease, cleanup_all, guest_report](ToolResult backup_result) mutable {
                            if (!backup_result.ok) {
                                cleanup_all();
                                cb(backup_result);
                                return;
                            }
                            const std::string archive_path = "/mnt/shared/" + op_lease.folder.tag + "/openclaw-source.tar.gz";
                            const std::string export_command = ScriptCommand(op_lease.folder.tag,
                                {"export-openclaw-source", archive_path});
                            if (progress) progress("exportSource", Text("Exporting OpenClaw user data from source VM", "正在从来源 VM 导出 OpenClaw 用户数据"), "");
                            RunCommand(source_vm_id, export_command, 420000,
                                [this, target_vm_id, options, keep_count, progress, cb = std::move(cb), backup_package, report_path, archive_path, op_lease, backup_lease, cleanup_all, guest_report](ToolResult export_result) mutable {
                                    if (!export_result.ok) {
                                        cleanup_all();
                                        cb(export_result);
                                        return;
                                    }
                                    const std::string dry_command = ScriptCommand(op_lease.folder.tag,
                                        {"migrate-openclaw-dry-run", archive_path, guest_report,
                                         SkillConflictRawValue(options.skill_conflict), options.workspace_target});
                                    if (progress) progress("dryRun", Text("Generating official dry-run migration plan", "正在生成官方 dry-run 迁移计划"), SkillConflictDisplayName(options.skill_conflict));
                                    RunCommand(target_vm_id, dry_command, 420000,
                                        [this, target_vm_id, options, keep_count, progress, cb = std::move(cb), backup_package, report_path, archive_path, op_lease, backup_lease, cleanup_all, guest_report](ToolResult dry_result) mutable {
                                            if (!dry_result.ok) {
                                                cleanup_all();
                                                cb(dry_result);
                                                return;
                                            }
                                            const std::string migrate_command = ScriptCommand(op_lease.folder.tag,
                                                {"migrate-openclaw-apply", archive_path, guest_report,
                                                 SkillConflictRawValue(options.skill_conflict), options.workspace_target});
                                            if (progress) progress("migrate", Text("Dry run passed; applying migration", "dry-run 已通过，正在执行正式迁移"), PathFilename(report_path));
                                            RunCommand(target_vm_id, migrate_command, 600000,
                                                [this, target_vm_id, keep_count, progress, cb = std::move(cb), backup_package, report_path, cleanup_all](ToolResult migrate_result) mutable {
                                                    cleanup_all();
                                                    if (!migrate_result.ok) {
                                                        cb(migrate_result);
                                                        return;
                                                    }
                                                    RotateBackups(target_vm_id, AgentKind::kHermes, keep_count);
                                                    if (progress) progress("complete", Text("Migration completed; report saved", "迁移完成，报告已保存"), PathFilename(report_path));
                                                    cb(Success(Text("OpenClaw to Hermes migration completed", "已完成 OpenClaw 到 Hermes 迁移"),
                                                        Text("Pre-migration backup: ", "迁移前备份：") + backup_package +
                                                        "\n" + Text("Migration report: ", "迁移报告：") + report_path + "\n" + migrate_result.output));
                                                });
                                        });
                                });
                        });
                },
                [this, backup_lease, cb = std::move(op_failure_cb)](ToolResult failure) mutable {
                    CleanupShare(backup_lease);
                    cb(std::move(failure));
                });
        },
        std::move(backup_failure_cb));
}

}  // namespace agent_tools
