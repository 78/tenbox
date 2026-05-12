#include "manager/agent_tools_service.h"

#include "manager/app_settings.h"

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
    case SkillConflictStrategy::kOverwrite: return "技能覆盖 Hermes";
    case SkillConflictStrategy::kRename: return "技能重命名导入";
    case SkillConflictStrategy::kSkip:
    default: return "技能保留 Hermes";
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
        if (error) *error = "找不到 VM";
        return false;
    }
    if (vm->state != VmPowerState::kRunning) {
        if (error) *error = "VM 未运行";
        return false;
    }
    if (!vm->guest_agent_connected) {
        if (error) *error = "Guest Agent 未连接";
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
        failure_cb(Failure("创建临时目录失败", ec.message()));
        return;
    }

    const std::string tag = "tenbox-agent-ops-" + NormalizeTagSeed(settings::GenerateUuid());
    fs::path dir = fs::path(OperationBaseDirectory()) / (tag + "-" + NormalizeTagSeed(settings::GenerateUuid()));
    fs::create_directories(dir, ec);
    if (ec) {
        failure_cb(Failure("创建临时共享目录失败", ec.message()));
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
            failure_cb(Failure("挂载临时共享目录失败", error));
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
        failure_cb(Failure("创建备份目录失败", ec.message()));
        return;
    }
    const std::string tag = "tenbox-agent-backups-" + NormalizeTagSeed(settings::GenerateUuid());

    ShareLease lease;
    lease.folder = SharedFolder{tag, BackupBaseDirectory(vm_id), false};
    lease.vm_ids = {vm_id};

    std::string error;
    if (!manager_.AddRuntimeSharedFolder(vm_id, lease.folder, &error)) {
        failure_cb(Failure("挂载备份目录失败", error));
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
                cb(Failure(result.error.empty() ? "Guest Agent 命令执行失败" : result.error, output));
                return;
            }
            if (result.exit_code != 0) {
                cb(Failure(output.empty() ? "Agent 操作失败" : output, output));
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

std::string AgentToolsService::AgentDataRelativePath(AgentKind agent) {
    return agent == AgentKind::kHermes ? ".hermes" : ".openclaw";
}

std::string AgentToolsService::AgentExcludeArgs(AgentKind agent, const std::string& scope) {
    std::vector<std::string> patterns;
    if (agent == AgentKind::kHermes) {
        patterns = {
            ".hermes/logs", ".hermes/image_cache", ".hermes/audio_cache",
            ".hermes/cache", ".hermes/hermes-agent", ".hermes/bin",
            ".hermes/gateway.pid", ".hermes/gateway.lock"
        };
    } else {
        patterns = {
            ".openclaw/cache", ".openclaw/.cache", ".openclaw/workspace/.cache",
            ".openclaw/logs", ".openclaw/backup", ".openclaw/openclaw-backup*.tar.gz"
        };
    }
    std::ostringstream os;
    for (const auto& p : patterns) {
        os << " --exclude=" << ShellQuote(p);
    }
    if (scope != "migration") {
        os << " --exclude=" << ShellQuote(AgentDataRelativePath(agent) + "/tmp");
    }
    return os.str();
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
        "[ -d \"$share_dir\" ] || { echo \"共享文件夹未挂载：$share_dir\" >&2; exit 1; }\n"
        "[ -w \"$share_dir\" ] || { echo \"共享文件夹不可写：$share_dir\" >&2; exit 1; }\n" +
        body + "\n";
}

std::string AgentToolsService::ProfileExportCommand(AgentKind agent, const std::string& output_path,
                                                    const std::string& scope) {
    const std::string rel = AgentDataRelativePath(agent);
    const std::string out_dir = Dirname(output_path);
    const std::string work_dir = out_dir + "/.tenbox-profile-work";
    std::ostringstream os;
    os << "set -eu\n"
       << "home=\"${HOME:-/home/tenbox}\"\n"
       << "rel=" << ShellQuote(rel) << "\n"
       << "src=\"$home/$rel\"\n"
       << "out=" << ShellQuote(output_path) << "\n"
       << "work=" << ShellQuote(work_dir) << "\n"
       << "[ -d \"$src\" ] || { echo \"Agent 数据尚未初始化：$src\" >&2; exit 1; }\n"
       << "rm -rf \"$work\"\n"
       << "mkdir -p \"$work\"\n"
       << "cat > \"$work/manifest.json\" <<EOF\n"
       << "{\n"
       << "  \"format\": \"tenbox-agent-profile\",\n"
       << "  \"format_version\": 2,\n"
       << "  \"agent_type\": \"" << AgentRawValue(agent) << "\",\n"
       << "  \"export_scope\": \"" << scope << "\",\n"
       << "  \"archive\": \"files.tar.gz\"\n"
       << "}\n"
       << "EOF\n"
       << "tar_status=0\n"
       << "(cd \"$home\" && tar --warning=no-file-changed --ignore-failed-read"
       << AgentExcludeArgs(agent, scope)
       << " -czf \"$work/files.tar.gz\" \"$rel\") || tar_status=$?\n"
       << "[ \"$tar_status\" -le 1 ] || exit \"$tar_status\"\n"
       << "rm -f \"$out\"\n"
       << "tar -czf \"$out\" -C \"$work\" manifest.json files.tar.gz\n"
       << "rm -rf \"$work\"\n"
       << "echo \"$out\"\n";
    return os.str();
}

std::string AgentToolsService::ProfileImportCommand(AgentKind agent, const std::string& input_path) {
    const std::string rel = AgentDataRelativePath(agent);
    std::ostringstream os;
    os << "set -eu\n"
       << "home=\"${HOME:-/home/tenbox}\"\n"
       << "input=" << ShellQuote(input_path) << "\n"
       << "rel=" << ShellQuote(rel) << "\n"
       << "target=\"$home/$rel\"\n"
       << "[ -f \"$input\" ] || { echo \"找不到导入包：$input\" >&2; exit 1; }\n"
       << "input_dir=\"$(dirname \"$input\")\"\n"
       << "tmp_parent=\"${HOME:-/home/tenbox}/.tenbox-tmp\"\n"
       << "mkdir -p \"$tmp_parent\"\n"
       << "work=\"$(mktemp -d \"$tmp_parent/profile-import.XXXXXX\")\"\n"
       << "trap 'rm -rf \"$work\"' EXIT\n"
       << "tar --touch --no-same-owner -xzf \"$input\" -C \"$work\"\n"
       << "[ -f \"$work/manifest.json\" ] || { echo \"导入包缺少 manifest.json\" >&2; exit 1; }\n"
       << "[ -f \"$work/files.tar.gz\" ] || { echo \"导入包缺少 files.tar.gz\" >&2; exit 1; }\n"
       << "pkg_agent=\"\"\n"
       << "if command -v python3 >/dev/null 2>&1; then\n"
       << "  pkg_agent=\"$(python3 - \"$work/manifest.json\" <<'PY'\n"
       << "import json, sys\n"
       << "with open(sys.argv[1], 'r', encoding='utf-8') as f:\n"
       << "    print(json.load(f).get('agent_type', ''))\n"
       << "PY\n"
       << "  )\" || pkg_agent=\"\"\n"
       << "fi\n"
       << "if [ -z \"$pkg_agent\" ]; then pkg_agent=\"$(awk -F\\\" '/agent_type/ {print $4; exit}' \"$work/manifest.json\")\"; fi\n"
       << "[ \"$pkg_agent\" = \"" << AgentRawValue(agent) << "\" ] || { echo \"导入包属于 $pkg_agent，不是 "
       << AgentRawValue(agent) << "\" >&2; exit 1; }\n"
       << "if ! tar -tzf \"$work/files.tar.gz\" \"$rel\" >/dev/null 2>&1; then echo \"导入包缺少 $rel 目录\" >&2; exit 1; fi\n"
       << "backup=\"\"\n"
       << "if [ -e \"$target\" ]; then\n"
       << "  backup=\"$input_dir/pre-import-" << AgentRawValue(agent) << "-$(date -u +%Y%m%d%H%M%S).tar.gz\"\n"
       << "  backup_status=0\n"
       << "  (cd \"$home\" && tar --warning=no-file-changed --ignore-failed-read -czf \"$backup\" \"$rel\") || backup_status=$?\n"
       << "  if [ \"$backup_status\" -gt 1 ]; then rm -f \"$backup\"; echo \"创建导入前备份失败\" >&2; exit \"$backup_status\"; fi\n"
       << "fi\n"
       << "mkdir -p \"$target\"\n"
       << "tar -tzf \"$work/files.tar.gz\" | awk -v rel=\"$rel/\" 'index($0, rel) == 1 { rest=substr($0, length(rel)+1); split(rest, a, \"/\"); if (a[1] != \"\") print a[1] }' | sort -u | while IFS= read -r item; do [ -n \"$item\" ] || continue; rm -rf \"$target/$item\"; done\n"
       << "if ! tar --touch --no-same-owner -xzf \"$work/files.tar.gz\" -C \"$home\"; then\n"
       << "  rm -rf \"$target\"\n"
       << "  if [ -n \"$backup\" ] && [ -f \"$backup\" ]; then tar --touch --no-same-owner -xzf \"$backup\" -C \"$home\"; fi\n"
       << "  echo \"恢复 Agent 数据失败\" >&2; exit 1\n"
       << "fi\n"
       << "chmod 700 \"$target\" 2>/dev/null || true\n"
       << "svc=\"$(" << ServiceResolverCommand(agent) << ")\"\n"
       << "if [ -n \"$svc\" ]; then XDG_RUNTIME_DIR=\"${XDG_RUNTIME_DIR:-/run/user/$(id -u)}\" systemctl --user restart \"$svc\" >/dev/null 2>&1 || true; fi\n"
       << "if [ -n \"$backup\" ]; then echo \"$backup\"; else echo \"已导入\"; fi\n";
    return os.str();
}

std::string AgentToolsService::ServiceResolverCommand(AgentKind agent) {
    if (agent == AgentKind::kHermes) {
        return "systemctl --user list-unit-files --no-legend 2>/dev/null | awk '{print $1}' | grep -E '^(hermes|hermes-gateway)\\.service$' | head -n 1";
    }
    return "systemctl --user list-unit-files --no-legend 2>/dev/null | awk '{print $1}' | grep -E '^(openclaw|openclaw-gateway)\\.service$' | head -n 1";
}

std::string AgentToolsService::HermesCommandResolver() {
    return "if command -v hermes >/dev/null 2>&1; then command -v hermes; elif [ -x \"$HOME/.local/bin/hermes\" ]; then echo \"$HOME/.local/bin/hermes\"; elif [ -x \"$HOME/.hermes/bin/hermes\" ]; then echo \"$HOME/.hermes/bin/hermes\"; else find \"$HOME/.hermes\" -path '*/bin/hermes' -type f -perm -111 2>/dev/null | head -n 1; fi";
}

std::string AgentToolsService::OpenClawCommandResolver() {
    return "if command -v openclaw >/dev/null 2>&1; then command -v openclaw; elif [ -x \"$HOME/.npm-global/bin/openclaw\" ]; then echo \"$HOME/.npm-global/bin/openclaw\"; else find \"$HOME\" -path '*/bin/openclaw' -type f -perm -111 2>/dev/null | head -n 1; fi";
}

std::string AgentToolsService::HealthStatusCommand(AgentKind agent) {
    const std::string port = agent == AgentKind::kOpenClaw ? "18789" : "";
    std::ostringstream os;
    os << "set -u\n"
       << "svc=\"$(" << ServiceResolverCommand(agent) << ")\"\n"
       << "agent=" << ShellQuote(AgentRawValue(agent)) << "\n"
       << "port=" << ShellQuote(port) << "\n"
       << "if [ -n \"$svc\" ] && XDG_RUNTIME_DIR=\"${XDG_RUNTIME_DIR:-/run/user/$(id -u)}\" systemctl --user is-active --quiet \"$svc\" 2>/dev/null; then service_state=ok; else service_state=error; fi\n"
       << "if [ -z \"$port\" ]; then port_state=skipped; elif nc -z 127.0.0.1 \"$port\" >/dev/null 2>&1; then port_state=ok; else port_state=error; fi\n"
       << "if curl -fsS --max-time 5 http://10.0.2.3/v1/models >/dev/null 2>&1; then model_state=ok; else model_state=error; fi\n"
       << "if command -v chromium >/dev/null 2>&1 || command -v chromium-browser >/dev/null 2>&1; then browser_state=ok; else browser_state=error; fi\n"
       << "free_kb=\"$(df -Pk \"$HOME\" 2>/dev/null | awk 'NR==2 {print $4}')\"\n"
       << "if [ \"${free_kb:-0}\" -gt 1048576 ]; then disk_state=ok; else disk_state=space_low; fi\n"
       << "state=ok; message=\"Agent 正常\"\n"
       << "if [ \"$disk_state\" = space_low ]; then state=error; message=\"磁盘空间不足\"; fi\n"
       << "if [ \"$service_state\" = error ]; then state=error; message=\"Agent 服务未运行\"; fi\n"
       << "if [ \"$port_state\" = error ]; then state=error; message=\"Agent 网关不可用\"; fi\n"
       << "if [ \"$model_state\" = error ]; then state=error; message=\"模型代理不可用\"; fi\n"
       << "if [ \"$browser_state\" = error ]; then state=error; message=\"浏览器不可用\"; fi\n"
       << "printf '{\"agent_type\":\"%s\",\"state\":\"%s\",\"message\":\"%s\",\"checks\":{\"agent_service\":\"%s\",\"gateway_port\":\"%s\",\"llm_proxy\":\"%s\",\"browser\":\"%s\",\"disk\":\"%s\"}}\\n' \"$agent\" \"$state\" \"$message\" \"$service_state\" \"$port_state\" \"$model_state\" \"$browser_state\" \"$disk_state\"\n";
    return os.str();
}

std::string AgentToolsService::RestartCommand(AgentKind agent) {
    std::ostringstream os;
    os << "set -eu\n"
       << "svc=\"$(" << ServiceResolverCommand(agent) << ")\"\n"
       << "[ -n \"$svc\" ] || { echo \"Agent 服务未安装\" >&2; exit 1; }\n"
       << "XDG_RUNTIME_DIR=\"${XDG_RUNTIME_DIR:-/run/user/$(id -u)}\" systemctl --user restart \"$svc\"\n";
    if (agent == AgentKind::kOpenClaw) {
        os << "i=0\nwhile [ \"$i\" -lt 60 ]; do nc -z 127.0.0.1 18789 >/dev/null 2>&1 && break; i=$((i + 1)); sleep 1; done\n";
    }
    os << HealthStatusCommand(agent);
    return os.str();
}

std::string AgentToolsService::ResetConfigCommand(AgentKind agent) {
    if (agent == AgentKind::kOpenClaw) {
        std::ostringstream os;
        os << "set -eu\n"
           << "openclaw_cmd=\"$(" << OpenClawCommandResolver() << ")\"\n"
           << "[ -n \"$openclaw_cmd\" ] || { echo \"缺少 OpenClaw 命令\" >&2; exit 1; }\n"
           << "tenbox_provider='{\"baseUrl\":\"http://10.0.2.3/v1\",\"apiKey\":\"tenbox\",\"api\":\"openai-completions\",\"models\":[{\"id\":\"default\",\"name\":\"Default (TenBox Proxy)\",\"reasoning\":false,\"input\":[\"text\",\"image\"],\"contextWindow\":200000,\"maxTokens\":65536,\"cost\":{\"input\":0,\"output\":0,\"cacheRead\":0,\"cacheWrite\":0}}]}'\n"
           << "\"$openclaw_cmd\" config set models.providers.tenbox \"$tenbox_provider\" --strict-json --merge >/dev/null 2>&1 || \"$openclaw_cmd\" config set models.providers.tenbox \"$tenbox_provider\" >/dev/null\n"
           << "\"$openclaw_cmd\" config set models.mode merge >/dev/null\n"
           << "\"$openclaw_cmd\" config set agents.defaults.model.primary tenbox/default >/dev/null\n"
           << "\"$openclaw_cmd\" config set agents.defaults.compaction.mode safeguard >/dev/null\n"
           << "\"$openclaw_cmd\" config set agents.defaults.workspace \"$HOME/.openclaw/workspace\" >/dev/null\n"
           << "\"$openclaw_cmd\" config set agents.defaults.models.tenbox/default '{\"alias\":\"TenBox Proxy\"}' --strict-json --merge >/dev/null 2>&1 || \"$openclaw_cmd\" config set agents.defaults.models.tenbox/default '{\"alias\":\"TenBox Proxy\"}' >/dev/null\n"
           << HealthStatusCommand(agent);
        return os.str();
    }

    std::ostringstream os;
    os << "set -eu\n"
       << "home=\"${HOME:-/home/tenbox}\"\n"
       << "hermes_cmd=\"$(" << HermesCommandResolver() << ")\"\n"
       << "if [ -n \"$hermes_cmd\" ]; then\n"
       << "  \"$hermes_cmd\" config set model.default default >/dev/null\n"
       << "  \"$hermes_cmd\" config set model.provider custom >/dev/null\n"
       << "  \"$hermes_cmd\" config set model.base_url http://10.0.2.3/v1 >/dev/null\n"
       << "  \"$hermes_cmd\" config set terminal.backend local >/dev/null\n"
       << "else\n"
       << "  mkdir -p \"$home/.hermes\"\n"
       << "  cfg=\"$home/.hermes/config.yaml\"\n"
       << "  env_file=\"$home/.hermes/.env\"\n"
       << "  cat > \"$cfg\" <<'EOF'\n"
       << "model:\n"
       << "  default: \"default\"\n"
       << "  provider: \"custom\"\n"
       << "  base_url: \"http://10.0.2.3/v1\"\n"
       << "\n"
       << "terminal:\n"
       << "  backend: local\n"
       << "EOF\n"
       << "  touch \"$env_file\"\n"
       << "fi\n"
       << "env_file=\"$home/.hermes/.env\"\n"
       << "mkdir -p \"$(dirname \"$env_file\")\"\n"
       << "touch \"$env_file\"\n"
       << "set_env_value() { key=\"$1\"; value=\"$2\"; if grep -q \"^$key=\" \"$env_file\"; then sed -i \"s|^$key=.*|$key=$value|\" \"$env_file\"; else printf '%s=%s\\n' \"$key\" \"$value\" >> \"$env_file\"; fi; }\n"
       << "set_env_value OPENAI_BASE_URL http://10.0.2.3/v1\n"
       << "set_env_value OPENAI_API_KEY tenbox\n"
       << "set_env_value AGENT_BROWSER_HEADED true\n"
       << "set_env_value AGENT_BROWSER_EXECUTABLE_PATH /usr/bin/chromium\n"
       << "svc=\"$(" << ServiceResolverCommand(agent) << ")\"\n"
       << "if [ -n \"$svc\" ]; then XDG_RUNTIME_DIR=\"${XDG_RUNTIME_DIR:-/run/user/$(id -u)}\" systemctl --user restart \"$svc\" >/dev/null 2>&1 || true; fi\n"
       << HealthStatusCommand(agent);
    return os.str();
}

std::string AgentToolsService::DiagnosticsCommand(AgentKind agent, const std::string& output_dir) {
    std::ostringstream os;
    os << "set -eu\n"
       << "out=" << ShellQuote(output_dir) << "/tenbox-agent-diagnostics-" << AgentRawValue(agent) << "-$(date -u +%Y%m%d%H%M%S).tar.gz\n"
       << "tmp=" << ShellQuote(output_dir) << "/.tenbox-diagnostics-work\n"
       << "rm -rf \"$tmp\"\n"
       << "mkdir -p \"$tmp\"\n"
       << "(" << HealthStatusCommand(agent) << ") > \"$tmp/health.json\" 2>&1 || true\n"
       << "svc=\"$(" << ServiceResolverCommand(agent) << ")\"\n"
       << "if [ -n \"$svc\" ]; then\n"
       << "  XDG_RUNTIME_DIR=\"${XDG_RUNTIME_DIR:-/run/user/$(id -u)}\" systemctl --user status \"$svc\" --no-pager > \"$tmp/service.txt\" 2>&1 || true\n"
       << "  journalctl --user -u \"$svc\" -n 200 --no-pager > \"$tmp/journal.txt\" 2>&1 || true\n"
       << "else\n"
       << "  echo \"Agent 服务未安装\" > \"$tmp/service.txt\"\n"
       << "  echo \"Agent 服务未安装\" > \"$tmp/journal.txt\"\n"
       << "fi\n"
       << "df -h > \"$tmp/disk.txt\" 2>&1 || true\n"
       << "sed -Ei 's/(sk-[A-Za-z0-9_-]{8})[A-Za-z0-9_-]+/\\1***/g; s/(authorization:[[:space:]]*bearer[[:space:]]+)[^[:space:]]+/\\1***/Ig; s/((api[_-]?key|token|secret|password)[=: ]+)[^ ]+/\\1***/Ig' \"$tmp\"/*.txt \"$tmp\"/*.json 2>/dev/null || true\n"
       << "tar -czf \"$out\" -C \"$tmp\" .\n"
       << "rm -rf \"$tmp\"\n"
       << "echo \"$out\"\n";
    return os.str();
}

std::string AgentToolsService::OpenClawMigrationSourceExportCommand(const std::string& output_path) {
    std::ostringstream os;
    os << "set -eu\n"
       << "home=\"${HOME:-/home/tenbox}\"\n"
       << "src=\"$home/.openclaw\"\n"
       << "out=" << ShellQuote(output_path) << "\n"
       << "[ -d \"$src\" ] || { echo \"OpenClaw 数据尚未初始化：$src\" >&2; exit 1; }\n"
       << "rm -f \"$out\"\n"
       << "tar_status=0\n"
       << "(cd \"$home\" && tar --warning=no-file-changed --ignore-failed-read"
       << AgentExcludeArgs(AgentKind::kOpenClaw, "migration")
       << " -czf \"$out\" \".openclaw\") || tar_status=$?\n"
       << "[ \"$tar_status\" -le 1 ] || exit \"$tar_status\"\n"
       << "echo \"$out\"\n";
    return os.str();
}

std::string AgentToolsService::OpenClawMigrationFlags(const MigrationOptions& options, bool include_yes) {
    std::ostringstream os;
    os << "--preset full --migrate-secrets --overwrite --skill-conflict "
       << ShellQuote(SkillConflictRawValue(options.skill_conflict));
    if (!options.workspace_target.empty()) {
        os << " --workspace-target " << ShellQuote(options.workspace_target);
    }
    if (include_yes) os << " --yes";
    return os.str();
}

std::string AgentToolsService::OpenClawToHermesDryRunCommand(const std::string& input_path,
                                                            const std::string& report_path,
                                                            const MigrationOptions& options) {
    std::ostringstream os;
    os << "set -eu\n"
       << "hermes_cmd=\"$(" << HermesCommandResolver() << ")\"\n"
       << "[ -n \"$hermes_cmd\" ] || { echo \"目标 VM 缺少 Hermes 命令\" >&2; exit 1; }\n"
       << "input=" << ShellQuote(input_path) << "\n"
       << "report=" << ShellQuote(report_path) << "\n"
       << "tmp_parent=\"${HOME:-/home/tenbox}/.tenbox-tmp\"\n"
       << "mkdir -p \"$tmp_parent\"\n"
       << "work=\"$(mktemp -d \"$tmp_parent/openclaw-to-hermes.XXXXXX\")\"\n"
       << "trap 'rm -rf \"$work\"' EXIT\n"
       << "source_dir=\"$work/source\"\n"
       << "[ -f \"$input\" ] || { echo \"找不到 OpenClaw 迁移包：$input\" >&2; exit 1; }\n"
       << "mkdir -p \"$source_dir\"\n"
       << "tar --touch --no-same-owner -xzf \"$input\" -C \"$source_dir\"\n"
       << "[ -d \"$source_dir/.openclaw\" ] || { echo \"迁移包缺少 .openclaw 目录\" >&2; exit 1; }\n"
       << "dry_log=\"$work/dry-run.txt\"\n"
       << "dry_status=0\n"
       << "\"$hermes_cmd\" claw migrate --dry-run --source \"$source_dir/.openclaw\" "
       << OpenClawMigrationFlags(options, false) << " > \"$dry_log\" 2>&1 || dry_status=$?\n"
       << "{ echo \"===== OpenClaw -> Hermes dry-run $(date -u +%Y-%m-%dT%H:%M:%SZ) =====\"; cat \"$dry_log\"; echo; } >> \"$report\"\n"
       << "tail -n 80 \"$dry_log\"\n"
       << "[ \"$dry_status\" -eq 0 ] || exit \"$dry_status\"\n";
    return os.str();
}

std::string AgentToolsService::HermesOpenClawChannelConfigCommand() {
    return R"SH(
if command -v python3 >/dev/null 2>&1; then
  python3 - "$source_dir/.openclaw/openclaw.json" "${HOME:-/home/tenbox}/.hermes/.env" <<'PY' >> "$report" 2>&1
import json
import sys
from pathlib import Path

source = Path(sys.argv[1])
env_file = Path(sys.argv[2])
if not source.exists():
    print("OpenClaw channel 配置未迁移：找不到 openclaw.json")
    raise SystemExit(0)

try:
    data = json.loads(source.read_text(encoding="utf-8"))
except Exception as exc:
    print(f"OpenClaw channel 配置未迁移：openclaw.json 解析失败：{exc}")
    raise SystemExit(0)

channels = data.get("channels") or {}
updates = {}
notes = []

feishu = channels.get("feishu") or {}
if feishu.get("enabled"):
    app_id = feishu.get("appId") or feishu.get("app_id")
    app_secret = feishu.get("appSecret") or feishu.get("app_secret")
    if app_id:
        updates["FEISHU_APP_ID"] = str(app_id)
    if app_secret:
        updates["FEISHU_APP_SECRET"] = str(app_secret)
    if feishu.get("domain"):
        updates["FEISHU_DOMAIN"] = str(feishu["domain"])
    if feishu.get("connectionMode") or feishu.get("connection_mode"):
        updates["FEISHU_CONNECTION_MODE"] = str(feishu.get("connectionMode") or feishu.get("connection_mode"))
    if feishu.get("groupPolicy") or feishu.get("group_policy"):
        updates["FEISHU_GROUP_POLICY"] = str(feishu.get("groupPolicy") or feishu.get("group_policy"))
    allowed = feishu.get("allowFrom") or feishu.get("allowedUsers") or feishu.get("allowed_users")
    if isinstance(allowed, list) and allowed:
        updates["FEISHU_ALLOWED_USERS"] = ",".join(str(item) for item in allowed)
    notes.append("Feishu")

wecom = channels.get("wecom") or {}
if wecom.get("enabled"):
    bot_id = wecom.get("botId") or wecom.get("bot_id")
    secret = wecom.get("secret")
    if bot_id:
        updates["WECOM_BOT_ID"] = str(bot_id)
    if secret:
        updates["WECOM_SECRET"] = str(secret)
    if wecom.get("dmPolicy") or wecom.get("dm_policy"):
        updates["WECOM_DM_POLICY"] = str(wecom.get("dmPolicy") or wecom.get("dm_policy"))
    if wecom.get("groupPolicy") or wecom.get("group_policy"):
        updates["WECOM_GROUP_POLICY"] = str(wecom.get("groupPolicy") or wecom.get("group_policy"))
    allowed = wecom.get("allowFrom") or wecom.get("allow_from") or wecom.get("allowedUsers") or wecom.get("allowed_users")
    if isinstance(allowed, list) and allowed:
        updates["WECOM_ALLOWED_USERS"] = ",".join(str(item) for item in allowed)
    notes.append("WeCom")

if updates:
    env_file.parent.mkdir(parents=True, exist_ok=True)
    lines = env_file.read_text(encoding="utf-8").splitlines() if env_file.exists() else []
    seen = set()
    patched = []
    for line in lines:
        key = line.split("=", 1)[0] if "=" in line and not line.startswith("#") else None
        if key in updates:
            patched.append(f"{key}={updates[key]}")
            seen.add(key)
        else:
            patched.append(line)
    for key, value in updates.items():
        if key not in seen:
            patched.append(f"{key}={value}")
    env_file.write_text("\n".join(patched) + "\n", encoding="utf-8")

if notes:
    print("已迁移 OpenClaw channel 配置：" + "、".join(notes))
    print("提示：插件安装态、pairing/device 运行态未自动复制；如 Hermes channel adapter 版本不兼容，仍需手动检查。")
else:
    print("未发现可迁移的 Feishu/WeCom channel 配置")
PY
  if grep -q '^FEISHU_APP_ID=' "${HOME:-/home/tenbox}/.hermes/.env" 2>/dev/null; then
    "$hermes_cmd" config set platforms.feishu.enabled true >/dev/null 2>&1 || true
  fi
  if grep -q '^WECOM_BOT_ID=' "${HOME:-/home/tenbox}/.hermes/.env" 2>/dev/null; then
    "$hermes_cmd" config set platforms.wecom.enabled true >/dev/null 2>&1 || true
    wecom_dm_policy="$(grep '^WECOM_DM_POLICY=' "${HOME:-/home/tenbox}/.hermes/.env" 2>/dev/null | tail -n 1 | cut -d= -f2- || true)"
    if [ -n "$wecom_dm_policy" ]; then
      "$hermes_cmd" config set platforms.wecom.extra.dm_policy "$wecom_dm_policy" >/dev/null 2>&1 || true
    fi
  fi
fi
)SH";
}

std::string AgentToolsService::HermesTenBoxModelConfigCommand() {
    return R"SH(
if [ -n "$hermes_cmd" ]; then
  "$hermes_cmd" config set model.default default >/dev/null
  "$hermes_cmd" config set model.provider custom >/dev/null
  "$hermes_cmd" config set model.base_url http://10.0.2.3/v1 >/dev/null
  "$hermes_cmd" config set terminal.backend local >/dev/null
  "$hermes_cmd" config set auxiliary.compression.provider custom >/dev/null 2>&1 || true
  "$hermes_cmd" config set auxiliary.compression.model default >/dev/null 2>&1 || true
  "$hermes_cmd" config set auxiliary.compression.base_url http://10.0.2.3/v1 >/dev/null 2>&1 || true
  "$hermes_cmd" config set auxiliary.vision.provider custom >/dev/null 2>&1 || true
  "$hermes_cmd" config set auxiliary.vision.model default >/dev/null 2>&1 || true
  "$hermes_cmd" config set auxiliary.vision.base_url http://10.0.2.3/v1 >/dev/null 2>&1 || true
  "$hermes_cmd" config set auxiliary.session_search.provider custom >/dev/null 2>&1 || true
  "$hermes_cmd" config set auxiliary.session_search.model default >/dev/null 2>&1 || true
  "$hermes_cmd" config set auxiliary.session_search.base_url http://10.0.2.3/v1 >/dev/null 2>&1 || true
  env_file="${HOME:-/home/tenbox}/.hermes/.env"
  mkdir -p "$(dirname "$env_file")"
  touch "$env_file"
  set_env_value() {
    key="$1"
    value="$2"
    if grep -q "^$key=" "$env_file"; then
      sed -i "s|^$key=.*|$key=$value|" "$env_file"
    else
      printf '%s=%s\n' "$key" "$value" >> "$env_file"
    fi
  }
  set_env_value OPENAI_BASE_URL http://10.0.2.3/v1
  set_env_value OPENAI_API_KEY tenbox
  echo "已恢复 TenBox 模型代理配置" >> "$report"
fi
)SH";
}

std::string AgentToolsService::OpenClawToHermesMigrationCommand(const std::string& input_path,
                                                               const std::string& report_path,
                                                               const MigrationOptions& options) {
    std::ostringstream os;
    os << "set -eu\n"
       << "hermes_cmd=\"$(" << HermesCommandResolver() << ")\"\n"
       << "[ -n \"$hermes_cmd\" ] || { echo \"目标 VM 缺少 Hermes 命令\" >&2; exit 1; }\n"
       << "input=" << ShellQuote(input_path) << "\n"
       << "report=" << ShellQuote(report_path) << "\n"
       << "tmp_parent=\"${HOME:-/home/tenbox}/.tenbox-tmp\"\n"
       << "mkdir -p \"$tmp_parent\"\n"
       << "work=\"$(mktemp -d \"$tmp_parent/openclaw-to-hermes.XXXXXX\")\"\n"
       << "trap 'rm -rf \"$work\"' EXIT\n"
       << "source_dir=\"$work/source\"\n"
       << "[ -f \"$input\" ] || { echo \"找不到 OpenClaw 迁移包：$input\" >&2; exit 1; }\n"
       << "mkdir -p \"$source_dir\"\n"
       << "tar --touch --no-same-owner -xzf \"$input\" -C \"$source_dir\"\n"
       << "[ -d \"$source_dir/.openclaw\" ] || { echo \"迁移包缺少 .openclaw 目录\" >&2; exit 1; }\n"
       << "migrate_log=\"$work/migrate.txt\"\n"
       << "migrate_status=0\n"
       << "\"$hermes_cmd\" claw migrate --source \"$source_dir/.openclaw\" "
       << OpenClawMigrationFlags(options, true) << " > \"$migrate_log\" 2>&1 || migrate_status=$?\n"
       << "if grep -q \"Refusing to apply\" \"$migrate_log\"; then migrate_status=1; fi\n"
       << "{ echo \"===== OpenClaw -> Hermes migrate $(date -u +%Y-%m-%dT%H:%M:%SZ) =====\"; cat \"$migrate_log\"; echo; } >> \"$report\"\n"
       << "tail -n 80 \"$migrate_log\"\n"
       << "[ \"$migrate_status\" -eq 0 ] || exit \"$migrate_status\"\n"
       << HermesOpenClawChannelConfigCommand() << "\n"
       << HermesTenBoxModelConfigCommand() << "\n"
       << "svc=\"$(" << ServiceResolverCommand(AgentKind::kHermes) << ")\"\n"
       << "if [ -n \"$svc\" ]; then XDG_RUNTIME_DIR=\"${XDG_RUNTIME_DIR:-/run/user/$(id -u)}\" systemctl --user restart \"$svc\" >/dev/null 2>&1 || true; echo \"重启服务：$svc\" >> \"$report\"; fi\n"
       << "health_log=\"$(mktemp)\"\n"
       << "(" << HealthStatusCommand(AgentKind::kHermes) << ") > \"$health_log\" 2>&1 || true\n"
       << "cat \"$health_log\"\n"
       << "{ echo \"===== Hermes health =====\"; cat \"$health_log\"; echo; } >> \"$report\"\n"
       << "rm -f \"$health_log\"\n";
    return os.str();
}

void AgentToolsService::ExportProfile(const std::string& vm_id, AgentKind agent,
                                      const std::string& destination_path, ToolCallback cb) {
    ToolCallback failure_cb = cb;
    WithOperationShare({vm_id}, [this, vm_id, agent, destination_path, cb = std::move(cb)](ShareLease lease) mutable {
        const std::string package_name = PathFilename(destination_path).empty()
            ? std::string(AgentRawValue(agent)) + "-profile.tar.gz"
            : PathFilename(destination_path);
        const std::string guest_package = "/mnt/shared/" + lease.folder.tag + "/" + package_name;
        const std::string command = WithSharedFolderReady(
            lease.folder.tag,
            ProfileExportCommand(agent, guest_package, "migration"));
        RunCommand(vm_id, command, 420000, [this, lease, destination_path, guest_package, cb = std::move(cb)](ToolResult result) mutable {
            CleanupShare(lease);
            if (!result.ok) {
                cb(result);
                return;
            }
            std::error_code ec;
            fs::copy_file(fs::path(lease.folder.host_path) / PathFilename(guest_package),
                          destination_path, fs::copy_options::overwrite_existing, ec);
            if (ec) cb(Failure("复制导出包失败", ec.message()));
            else cb(Success("已导出 Agent 数据", destination_path));
        });
    }, std::move(failure_cb));
}

void AgentToolsService::ImportProfile(const std::string& vm_id, AgentKind agent,
                                      const std::string& source_path, ToolCallback cb) {
    ToolCallback failure_cb = cb;
    WithOperationShare({vm_id}, [this, vm_id, agent, source_path, cb = std::move(cb)](ShareLease lease) mutable {
        const std::string package_name = "tenbox-agent-profile-import.tar.gz";
        std::error_code ec;
        fs::copy_file(source_path, fs::path(lease.folder.host_path) / package_name,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            CleanupShare(lease);
            cb(Failure("复制导入包失败", ec.message()));
            return;
        }
        const std::string guest_package = "/mnt/shared/" + lease.folder.tag + "/" + package_name;
        const std::string command = WithSharedFolderReady(lease.folder.tag, ProfileImportCommand(agent, guest_package));
        RunCommand(vm_id, command, 420000, [this, lease, agent, cb = std::move(cb)](ToolResult result) mutable {
            CleanupShare(lease);
            if (!result.ok) cb(result);
            else cb(Success(std::string("已导入 ") + AgentDisplayName(agent) + " 数据", result.output));
        });
    }, std::move(failure_cb));
}

void AgentToolsService::SnapshotBackup(const std::string& vm_id, AgentKind agent,
                                       int keep_count, ToolCallback cb) {
    std::error_code ec;
    fs::create_directories(BackupPackageDirectory(vm_id, agent), ec);
    if (ec) {
        cb(Failure("创建备份目录失败", ec.message()));
        return;
    }
    const std::string package = NewBackupPackagePath(vm_id, agent);
    ToolCallback failure_cb = cb;
    WithBackupShare(vm_id, [this, vm_id, agent, keep_count, package, cb = std::move(cb)](ShareLease lease) mutable {
        const std::string guest_dir = "/mnt/shared/" + lease.folder.tag + "/" + AgentRawValue(agent);
        const std::string guest_package = guest_dir + "/" + PathFilename(package);
        const std::string command = WithSharedFolderReady(
            lease.folder.tag,
            "mkdir -p " + ShellQuote(guest_dir) + "\n" +
            ProfileExportCommand(agent, guest_package, "backup"));
        RunCommand(vm_id, command, 420000, [this, lease, vm_id, agent, keep_count, package, cb = std::move(cb)](ToolResult result) mutable {
            CleanupShare(lease);
            if (!result.ok) {
                cb(result);
                return;
            }
            RotateBackups(vm_id, agent, keep_count);
            cb(Success("已创建 Agent 数据备份", package));
        });
    }, std::move(failure_cb));
}

void AgentToolsService::RestoreBackup(const std::string& vm_id, AgentKind agent,
                                      const std::string& package_path, ToolCallback cb) {
    ToolCallback failure_cb = cb;
    WithBackupShare(vm_id, [this, vm_id, agent, package_path, cb = std::move(cb)](ShareLease lease) mutable {
        const std::string guest_package = "/mnt/shared/" + lease.folder.tag + "/" +
            std::string(AgentRawValue(agent)) + "/" + PathFilename(package_path);
        const std::string command = WithSharedFolderReady(lease.folder.tag, ProfileImportCommand(agent, guest_package));
        RunCommand(vm_id, command, 420000, [this, lease, package_path, cb = std::move(cb)](ToolResult result) mutable {
            CleanupShare(lease);
            if (!result.ok) cb(result);
            else cb(Success("已恢复 Agent 数据备份", package_path));
        });
    }, std::move(failure_cb));
}

void AgentToolsService::RunHealthCommand(const std::string& vm_id, AgentKind,
                                         const std::string& command,
                                         const std::string& success_message,
                                         ToolCallback cb) {
    RunCommand(vm_id, command, 180000, [success_message, cb = std::move(cb)](ToolResult result) mutable {
        if (!result.ok) cb(result);
        else cb(Success(success_message, result.output));
    });
}

void AgentToolsService::HealthStatus(const std::string& vm_id, AgentKind agent, ToolCallback cb) {
    RunHealthCommand(vm_id, agent, HealthStatusCommand(agent), "健康状态已更新", std::move(cb));
}

void AgentToolsService::RunRepairCommand(const std::string& vm_id, AgentKind agent,
                                         const std::string& repair_command,
                                         const std::string& success_message,
                                         int keep_count,
                                         ToolCallback cb) {
    std::error_code ec;
    fs::create_directories(BackupPackageDirectory(vm_id, agent), ec);
    if (ec) {
        cb(Failure("创建备份目录失败", ec.message()));
        return;
    }
    const std::string package = NewBackupPackagePath(vm_id, agent);
    ToolCallback failure_cb = cb;
    WithBackupShare(vm_id, [this, vm_id, agent, repair_command, success_message, keep_count, package, cb = std::move(cb)](ShareLease lease) mutable {
        const std::string guest_dir = "/mnt/shared/" + lease.folder.tag + "/" + AgentRawValue(agent);
        const std::string guest_package = guest_dir + "/" + PathFilename(package);
        const std::string command = WithSharedFolderReady(
            lease.folder.tag,
            "mkdir -p " + ShellQuote(guest_dir) + "\n" +
            ProfileExportCommand(agent, guest_package, "backup") + "\n" +
            repair_command);
        RunCommand(vm_id, command, 420000,
            [this, lease, vm_id, agent, keep_count, package, success_message, cb = std::move(cb)](ToolResult result) mutable {
                CleanupShare(lease);
                if (!result.ok) {
                    cb(result);
                    return;
                }
                RotateBackups(vm_id, agent, keep_count);
                cb(Success(success_message, "修复前备份：" + package + "\n" + result.output));
            });
    }, std::move(failure_cb));
}

void AgentToolsService::RestartAgent(const std::string& vm_id, AgentKind agent,
                                     int keep_count, ToolCallback cb) {
    RunRepairCommand(vm_id, agent, RestartCommand(agent), "已重新启动 Agent", keep_count, std::move(cb));
}

void AgentToolsService::ResetAgentConfig(const std::string& vm_id, AgentKind agent,
                                         int keep_count, ToolCallback cb) {
    RunRepairCommand(vm_id, agent, ResetConfigCommand(agent), "已重置 Agent 配置", keep_count, std::move(cb));
}

void AgentToolsService::ExportDiagnostics(const std::string& vm_id, AgentKind agent, ToolCallback cb) {
    ToolCallback failure_cb = cb;
    WithBackupShare(vm_id, [this, vm_id, agent, cb = std::move(cb)](ShareLease lease) mutable {
        const std::string guest_dir = "/mnt/shared/" + lease.folder.tag;
        const std::string command = WithSharedFolderReady(lease.folder.tag, DiagnosticsCommand(agent, guest_dir));
        RunCommand(vm_id, command, 180000, [this, lease, cb = std::move(cb)](ToolResult result) mutable {
            CleanupShare(lease);
            if (!result.ok) cb(result);
            else cb(Success("已导出诊断包", result.output));
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
        cb(Failure("OpenClaw 来源 VM " + source_error));
        return;
    }
    std::string target_error;
    if (!IsRunnable(target_vm_id, &target_error)) {
        cb(Failure("Hermes 目标 VM " + target_error));
        return;
    }

    std::error_code ec;
    fs::create_directories(BackupPackageDirectory(target_vm_id, AgentKind::kHermes), ec);
    if (ec) {
        cb(Failure("创建迁移目录失败", ec.message()));
        return;
    }
    const std::string backup_package = NewBackupPackagePath(target_vm_id, AgentKind::kHermes);
    const std::string report_path = NewMigrationReportPath(target_vm_id);

    ToolCallback backup_failure_cb = cb;
    WithBackupShare(target_vm_id,
        [this, source_vm_id, target_vm_id, options, keep_count, progress, cb = std::move(cb), backup_package, report_path](ShareLease backup_lease) mutable {
            ToolCallback op_failure_cb = cb;
            WithOperationShare({source_vm_id, target_vm_id},
                [this, source_vm_id, target_vm_id, options, keep_count, progress, cb = std::move(cb), backup_package, report_path, backup_lease](ShareLease op_lease) mutable {
                    auto cleanup_all = [this, backup_lease, op_lease]() {
                        CleanupShare(op_lease);
                        CleanupShare(backup_lease);
                    };
                    const std::string guest_backup_dir = "/mnt/shared/" + backup_lease.folder.tag + "/hermes";
                    const std::string guest_backup = guest_backup_dir + "/" + PathFilename(backup_package);
                    const std::string guest_report = guest_backup_dir + "/" + PathFilename(report_path);
                    const std::string backup_command = WithSharedFolderReady(
                        backup_lease.folder.tag,
                        "mkdir -p " + ShellQuote(guest_backup_dir) + "\n" +
                        ProfileExportCommand(AgentKind::kHermes, guest_backup, "backup"));
                    if (progress) progress("backup", "正在创建目标 Hermes 迁移前备份", PathFilename(backup_package));
                    RunCommand(target_vm_id, backup_command, 420000,
                        [this, source_vm_id, target_vm_id, options, keep_count, progress, cb = std::move(cb), backup_package, report_path, op_lease, backup_lease, cleanup_all](ToolResult backup_result) mutable {
                            if (!backup_result.ok) {
                                cleanup_all();
                                cb(backup_result);
                                return;
                            }
                            const std::string archive_path = "/mnt/shared/" + op_lease.folder.tag + "/openclaw-source.tar.gz";
                            const std::string export_command = WithSharedFolderReady(
                                op_lease.folder.tag,
                                OpenClawMigrationSourceExportCommand(archive_path));
                            if (progress) progress("exportSource", "正在从来源 VM 导出 OpenClaw 用户数据", "");
                            RunCommand(source_vm_id, export_command, 420000,
                                [this, target_vm_id, options, keep_count, progress, cb = std::move(cb), backup_package, report_path, archive_path, op_lease, backup_lease, cleanup_all](ToolResult export_result) mutable {
                                    if (!export_result.ok) {
                                        cleanup_all();
                                        cb(export_result);
                                        return;
                                    }
                                    const std::string dry_command = WithSharedFolderReady(
                                        op_lease.folder.tag,
                                        OpenClawToHermesDryRunCommand(archive_path, "/mnt/shared/" + backup_lease.folder.tag + "/hermes/" + PathFilename(report_path), options));
                                    if (progress) progress("dryRun", "正在生成官方 dry-run 迁移计划", SkillConflictDisplayName(options.skill_conflict));
                                    RunCommand(target_vm_id, dry_command, 420000,
                                        [this, target_vm_id, options, keep_count, progress, cb = std::move(cb), backup_package, report_path, archive_path, op_lease, backup_lease, cleanup_all](ToolResult dry_result) mutable {
                                            if (!dry_result.ok) {
                                                cleanup_all();
                                                cb(dry_result);
                                                return;
                                            }
                                            const std::string migrate_command = WithSharedFolderReady(
                                                op_lease.folder.tag,
                                                OpenClawToHermesMigrationCommand(archive_path, "/mnt/shared/" + backup_lease.folder.tag + "/hermes/" + PathFilename(report_path), options));
                                            if (progress) progress("migrate", "dry-run 已通过，正在执行正式迁移", PathFilename(report_path));
                                            RunCommand(target_vm_id, migrate_command, 600000,
                                                [this, target_vm_id, keep_count, progress, cb = std::move(cb), backup_package, report_path, cleanup_all](ToolResult migrate_result) mutable {
                                                    cleanup_all();
                                                    if (!migrate_result.ok) {
                                                        cb(migrate_result);
                                                        return;
                                                    }
                                                    RotateBackups(target_vm_id, AgentKind::kHermes, keep_count);
                                                    if (progress) progress("complete", "迁移完成，报告已保存", PathFilename(report_path));
                                                    cb(Success("已完成 OpenClaw 到 Hermes 迁移",
                                                        "迁移前备份：" + backup_package + "\n迁移报告：" + report_path + "\n" + migrate_result.output));
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
