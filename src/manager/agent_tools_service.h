#pragma once

#include "manager/manager_service.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace agent_tools {

enum class AgentKind {
    kHermes,
    kOpenClaw,
};

enum class SkillConflictStrategy {
    kSkip,
    kOverwrite,
    kRename,
};

struct MigrationOptions {
    SkillConflictStrategy skill_conflict = SkillConflictStrategy::kSkip;
    std::string workspace_target = "/home/tenbox/.hermes/workspace/openclaw-migrated";
};

struct ToolResult {
    bool ok = false;
    std::string message;
    std::string output;
};

struct BackupPackage {
    std::string path;
    std::string filename;
    uint64_t size = 0;
    std::chrono::system_clock::time_point modified_at{};
};

struct BackupSchedule {
    bool enabled = false;
    int hour = 3;
    int minute = 0;
    int keep_count = 7;
    std::string last_run_date;
    std::string last_attempt_at;
    std::string last_attempt_status;
    std::string last_attempt_message;
};

using ToolCallback = std::function<void(ToolResult result)>;
using ProgressCallback = std::function<void(const std::string& step,
                                            const std::string& message,
                                            const std::string& detail)>;

const char* AgentRawValue(AgentKind agent);
const char* AgentDisplayName(AgentKind agent);
std::string SkillConflictRawValue(SkillConflictStrategy strategy);
std::string SkillConflictDisplayName(SkillConflictStrategy strategy);

class AgentToolsService {
public:
    AgentToolsService(ManagerService& manager, std::string data_dir);

    std::vector<BackupPackage> ListBackups(const std::string& vm_id, AgentKind agent) const;
    void RotateBackups(const std::string& vm_id, AgentKind agent, int keep_count);

    void ExportProfile(const std::string& vm_id, AgentKind agent,
                       const std::string& destination_path, ToolCallback cb);
    void ImportProfile(const std::string& vm_id, AgentKind agent,
                       const std::string& source_path, ToolCallback cb);
    void SnapshotBackup(const std::string& vm_id, AgentKind agent,
                        int keep_count, ToolCallback cb);
    void RestoreBackup(const std::string& vm_id, AgentKind agent,
                       const std::string& package_path, ToolCallback cb);
    void HealthStatus(const std::string& vm_id, AgentKind agent, ToolCallback cb);
    void RestartAgent(const std::string& vm_id, AgentKind agent,
                      int keep_count, ToolCallback cb);
    void ResetAgentConfig(const std::string& vm_id, AgentKind agent,
                          int keep_count, ToolCallback cb);
    void ExportDiagnostics(const std::string& vm_id, AgentKind agent, ToolCallback cb);
    void MigrateOpenClawToHermes(const std::string& source_vm_id,
                                 const std::string& target_vm_id,
                                 const MigrationOptions& options,
                                 int keep_count,
                                 ProgressCallback progress,
                                 ToolCallback cb);

private:
    struct ShareLease {
        SharedFolder folder;
        std::vector<std::string> vm_ids;
        std::string cleanup_dir;
    };

    using ShareCallback = std::function<void(ShareLease lease)>;

    bool IsRunnable(const std::string& vm_id, std::string* error) const;
    void WithOperationShare(const std::vector<std::string>& vm_ids,
                            ShareCallback cb,
                            ToolCallback failure_cb);
    void WithBackupShare(const std::string& vm_id,
                         ShareCallback cb,
                         ToolCallback failure_cb);
    void CleanupShare(const ShareLease& lease);
    void RunCommand(const std::string& vm_id, const std::string& command,
                    uint32_t timeout_ms, ToolCallback cb);
    void RunHealthCommand(const std::string& vm_id, AgentKind agent,
                          const std::string& command,
                          const std::string& success_message,
                          ToolCallback cb);
    void RunRepairCommand(const std::string& vm_id, AgentKind agent,
                          const std::vector<std::string>& repair_args,
                          const std::string& success_message,
                          int keep_count,
                          ToolCallback cb);

    std::string OperationBaseDirectory() const;
    std::string BackupBaseDirectory(const std::string& vm_id) const;
    std::string BackupPackageDirectory(const std::string& vm_id, AgentKind agent) const;
    std::string NewBackupPackagePath(const std::string& vm_id, AgentKind agent) const;
    std::string NewMigrationReportPath(const std::string& vm_id) const;

    bool PrepareAgentToolScript(const ShareLease& lease, std::string* error) const;

    static std::string ShellQuote(const std::string& value);
    static std::string PathFilename(const std::string& path);
    static std::string Dirname(const std::string& path);
    static std::string Timestamp();
    static std::string WithSharedFolderReady(const std::string& tag, const std::string& body);
    static std::string ScriptInvocation(const std::string& tag, const std::vector<std::string>& args);
    static std::string ScriptCommand(const std::string& tag, const std::vector<std::string>& args);

    ManagerService& manager_;
    std::string data_dir_;
};

}  // namespace agent_tools
