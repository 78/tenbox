import Foundation

enum AgentKind: String, CaseIterable, Identifiable {
    case hermes
    case openclaw

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .hermes: return "Hermes"
        case .openclaw: return "OpenClaw"
        }
    }
}

struct ConsoleCommandResult {
    let exitCode: Int32
    let output: String
}

struct AgentToolResult {
    let message: String
    let output: String
}

struct ConsoleCommandError: LocalizedError {
    let errorDescription: String?

    init(_ message: String) {
        self.errorDescription = message
    }
}

final class AgentToolsService {
    private let fileManager = FileManager.default

    func exportProfile(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                       destinationURL: URL,
                       completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        withOperationShare(vmId: vm.id, appState: appState) { share, cleanup in
            let packageName = destinationURL.lastPathComponent.isEmpty
                ? "\(vm.name)-\(agent.rawValue)-profile.tar.gz"
                : destinationURL.lastPathComponent
            let guestPackage = "/mnt/shared/\(share.tag)/\(packageName)"
            let command = Self.withSharedFolderReady(
                tag: share.tag,
                body: Self.profileExportCommand(agent: agent, outputPath: guestPackage)
            )

            session.runGuestAgentCommand(command, timeout: 420) { result in
                switch result {
                case .success(let commandResult):
                    guard commandResult.exitCode == 0 else {
                        cleanup()
                        completion(.failure(Self.makeError(commandResult.output.isEmpty ? "Agent data export failed" : commandResult.output)))
                        return
                    }
                    let hostPackage = URL(fileURLWithPath: share.hostPath).appendingPathComponent(packageName)
                    do {
                        if self.fileManager.fileExists(atPath: destinationURL.path) {
                            try self.fileManager.removeItem(at: destinationURL)
                        }
                        try self.fileManager.copyItem(at: hostPackage, to: destinationURL)
                        cleanup()
                        completion(.success(AgentToolResult(
                            message: "已导出到 \(destinationURL.path)",
                            output: commandResult.output
                        )))
                    } catch {
                        cleanup()
                        completion(.failure(error))
                    }
                case .failure(let error):
                    cleanup()
                    completion(.failure(error))
                }
            }
        } failure: { error in
            completion(.failure(error))
        }
    }

    func importProfile(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                       sourceURL: URL,
                       completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        withOperationShare(vmId: vm.id, appState: appState) { share, cleanup in
            let packageName = "tenbox-agent-profile-import.tar.gz"
            let hostPackage = URL(fileURLWithPath: share.hostPath).appendingPathComponent(packageName)
            do {
                if self.fileManager.fileExists(atPath: hostPackage.path) {
                    try self.fileManager.removeItem(at: hostPackage)
                }
                try self.fileManager.copyItem(at: sourceURL, to: hostPackage)
            } catch {
                cleanup()
                completion(.failure(error))
                return
            }

            let guestPackage = "/mnt/shared/\(share.tag)/\(packageName)"
            let command = Self.withSharedFolderReady(
                tag: share.tag,
                body: Self.profileImportCommand(agent: agent, inputPath: guestPackage)
            )
            session.runGuestAgentCommand(command, timeout: 420) { result in
                cleanup()
                switch result {
                case .success(let commandResult):
                    guard commandResult.exitCode == 0 else {
                        completion(.failure(Self.makeError(commandResult.output.isEmpty ? "Agent data import failed" : commandResult.output)))
                        return
                    }
                    completion(.success(AgentToolResult(
                        message: "已导入 \(agent.displayName) 数据",
                        output: commandResult.output
                    )))
                case .failure(let error):
                    completion(.failure(error))
                }
            }
        } failure: { error in
            completion(.failure(error))
        }
    }

    func backupStatus(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                      completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        do {
            let latest = try latestBackupPackage(vmId: vm.id, agent: agent)
            if let latest {
                completion(.success(AgentToolResult(
                    message: "Agent 数据已保护",
                    output: "最近备份：\(latest.path)"
                )))
            } else {
                completion(.success(AgentToolResult(
                    message: "还没有备份",
                    output: "点击 Back Up Now 创建第一份备份。"
                )))
            }
        } catch {
            completion(.failure(error))
        }
    }

    func snapshotBackup(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                        completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        do {
            let package = try backupPackageURL(vmId: vm.id, agent: agent)
            withBackupShare(vmId: vm.id, appState: appState) { share, cleanup in
                let guestPackage = "/mnt/shared/\(share.tag)/\(agent.rawValue)/\(package.lastPathComponent)"
                let command = Self.withSharedFolderReady(
                    tag: share.tag,
                    body: "mkdir -p \(Self.shellQuote("/mnt/shared/\(share.tag)/\(agent.rawValue)"))\n" +
                        Self.profileExportCommand(agent: agent, outputPath: guestPackage)
                )
                session.runGuestAgentCommand(command, timeout: 420) { result in
                    cleanup()
                    switch result {
                    case .success(let commandResult):
                        guard commandResult.exitCode == 0 else {
                            completion(.failure(Self.makeError(commandResult.output.isEmpty ? "Agent backup failed" : commandResult.output)))
                            return
                        }
                        self.rotateBackups(vmId: vm.id, agent: agent, keep: 5)
                        completion(.success(AgentToolResult(
                            message: "已创建 Agent 数据备份",
                            output: package.path
                        )))
                    case .failure(let error):
                        completion(.failure(error))
                    }
                }
            } failure: { error in
                completion(.failure(error))
            }
        } catch {
            completion(.failure(error))
        }
    }

    func restoreLatestBackup(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                             completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        do {
            guard let latest = try latestBackupPackage(vmId: vm.id, agent: agent) else {
                completion(.failure(Self.makeError("No backup package found")))
                return
            }
            withBackupShare(vmId: vm.id, appState: appState) { share, cleanup in
                let guestPackage = "/mnt/shared/\(share.tag)/\(agent.rawValue)/\(latest.lastPathComponent)"
                let command = Self.withSharedFolderReady(
                    tag: share.tag,
                    body: Self.profileImportCommand(agent: agent, inputPath: guestPackage)
                )
                session.runGuestAgentCommand(command, timeout: 420) { result in
                    cleanup()
                    switch result {
                    case .success(let commandResult):
                        guard commandResult.exitCode == 0 else {
                            completion(.failure(Self.makeError(commandResult.output.isEmpty ? "Agent backup restore failed" : commandResult.output)))
                            return
                        }
                        completion(.success(AgentToolResult(
                            message: "已从最近备份恢复 Agent 数据",
                            output: latest.path
                        )))
                    case .failure(let error):
                        completion(.failure(error))
                    }
                }
            } failure: { error in
                completion(.failure(error))
            }
        } catch {
            completion(.failure(error))
        }
    }

    func healthStatus(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                      completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        runHealthCommand(vm: vm, session: session, appState: appState, agent: agent,
                         command: Self.healthStatusCommand(agent: agent),
                         successMessage: "健康状态已更新",
                         completion: completion)
    }

    func restartAgent(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                      completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        runRepairCommand(vm: vm, session: session, appState: appState, agent: agent,
                         repairCommand: Self.restartCommand(agent: agent),
                         successMessage: "已重新启动 Agent",
                         completion: completion)
    }

    func testModel(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                   completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        runHealthCommand(vm: vm, session: session, appState: appState, agent: agent,
                         command: Self.testModelCommand(agent: agent),
                         successMessage: "模型连接已测试",
                         completion: completion)
    }

    func resetAgentConfig(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                          completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        runRepairCommand(vm: vm, session: session, appState: appState, agent: agent,
                         repairCommand: Self.resetConfigCommand(agent: agent),
                         successMessage: "已重置 Agent 配置",
                         completion: completion)
    }

    func exportDiagnostics(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                           completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        withBackupShare(vmId: vm.id, appState: appState) { share, cleanup in
            let guestDir = "/mnt/shared/\(share.tag)"
            let command = Self.withSharedFolderReady(
                tag: share.tag,
                body: Self.diagnosticsCommand(agent: agent, outputDir: guestDir)
            )
            session.runGuestAgentCommand(command, timeout: 180) { result in
                cleanup()
                switch result {
                case .success(let commandResult):
                    guard commandResult.exitCode == 0 else {
                        completion(.failure(Self.makeError(commandResult.output.isEmpty ? "Agent diagnostics failed" : commandResult.output)))
                        return
                    }
                    completion(.success(AgentToolResult(
                        message: "已导出诊断包",
                        output: commandResult.output
                    )))
                case .failure(let error):
                    completion(.failure(error))
                }
            }
        } failure: { error in
            completion(.failure(error))
        }
    }

    private func runHealthCommand(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                                  command: String, successMessage: String,
                                  completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        session.runGuestAgentCommand(command, timeout: 180) { result in
            switch result {
            case .success(let commandResult):
                guard commandResult.exitCode == 0 else {
                    completion(.failure(Self.makeError(commandResult.output.isEmpty ? "Agent health command failed" : commandResult.output)))
                    return
                }
                completion(.success(AgentToolResult(
                    message: successMessage,
                    output: commandResult.output
                )))
            case .failure(let error):
                completion(.failure(error))
            }
        }
    }

    private func runRepairCommand(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                                  repairCommand: String, successMessage: String,
                                  completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        do {
            let package = try backupPackageURL(vmId: vm.id, agent: agent)
            withBackupShare(vmId: vm.id, appState: appState) { share, cleanup in
                let guestPackage = "/mnt/shared/\(share.tag)/\(agent.rawValue)/\(package.lastPathComponent)"
                let command = Self.withSharedFolderReady(
                    tag: share.tag,
                    body: "mkdir -p \(Self.shellQuote("/mnt/shared/\(share.tag)/\(agent.rawValue)"))\n" +
                        Self.profileExportCommand(agent: agent, outputPath: guestPackage) + "\n" +
                        repairCommand
                )
                session.runGuestAgentCommand(command, timeout: 420) { result in
                    cleanup()
                    switch result {
                    case .success(let commandResult):
                        guard commandResult.exitCode == 0 else {
                            completion(.failure(Self.makeError(commandResult.output.isEmpty ? "Agent repair failed" : commandResult.output)))
                            return
                        }
                        self.rotateBackups(vmId: vm.id, agent: agent, keep: 5)
                        completion(.success(AgentToolResult(
                            message: successMessage,
                            output: "修复前备份：\(package.path)\n\(commandResult.output)"
                        )))
                    case .failure(let error):
                        completion(.failure(error))
                    }
                }
            } failure: { error in
                completion(.failure(error))
            }
        } catch {
            completion(.failure(error))
        }
    }

    private func withOperationShare(vmId: String, appState: AppState,
                                    perform: (SharedFolder, @escaping () -> Void) -> Void,
                                    failure: (Error) -> Void) {
        do {
            let base = try operationBaseDirectory()
            let tag = "tenbox-agent-ops-\(UUID().uuidString.prefix(8).lowercased())"
            let dir = base.appendingPathComponent("\(vmId)-\(tag)", isDirectory: true)
            try fileManager.createDirectory(at: dir, withIntermediateDirectories: true)
            let share = SharedFolder(tag: tag, hostPath: dir.path, readonly: false)
            appState.addRuntimeSharedFolder(share, toVm: vmId)

            let cleanup: () -> Void = { [weak appState, weak self] in
                DispatchQueue.main.async {
                    appState?.removeRuntimeSharedFolder(tag: tag, fromVm: vmId)
                    try? self?.fileManager.removeItem(at: dir)
                }
            }
            perform(share, cleanup)
        } catch {
            failure(error)
        }
    }

    private func withBackupShare(vmId: String, appState: AppState,
                                 perform: (SharedFolder, @escaping () -> Void) -> Void,
                                 failure: (Error) -> Void) {
        do {
            let dir = try backupDirectory(vmId: vmId)
            let tag = "tenbox-agent-backups-\(UUID().uuidString.prefix(8).lowercased())"
            let share = SharedFolder(tag: tag, hostPath: dir.path, readonly: false)
            appState.addRuntimeSharedFolder(share, toVm: vmId)
            let cleanup: () -> Void = { [weak appState] in
                DispatchQueue.main.async {
                    appState?.removeRuntimeSharedFolder(tag: tag, fromVm: vmId)
                }
            }
            perform(share, cleanup)
        } catch {
            failure(error)
        }
    }

    private func operationBaseDirectory() throws -> URL {
        let appSupport = try fileManager.url(
            for: .applicationSupportDirectory,
            in: .userDomainMask,
            appropriateFor: nil,
            create: true
        )
        let dir = appSupport.appendingPathComponent("TenBox/AgentOperations", isDirectory: true)
        try fileManager.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    private func backupDirectory(vmId: String) throws -> URL {
        let appSupport = try fileManager.url(
            for: .applicationSupportDirectory,
            in: .userDomainMask,
            appropriateFor: nil,
            create: true
        )
        let dir = appSupport.appendingPathComponent("TenBox/AgentBackups/\(vmId)", isDirectory: true)
        try fileManager.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    private func backupPackageDirectory(vmId: String, agent: AgentKind) throws -> URL {
        let dir = try backupDirectory(vmId: vmId).appendingPathComponent(agent.rawValue, isDirectory: true)
        try fileManager.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    private func backupPackageURL(vmId: String, agent: AgentKind) throws -> URL {
        let formatter = DateFormatter()
        formatter.calendar = Calendar(identifier: .gregorian)
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(secondsFromGMT: 0)
        formatter.dateFormat = "yyyyMMddHHmmss"
        return try backupPackageDirectory(vmId: vmId, agent: agent)
            .appendingPathComponent("agent-data-\(formatter.string(from: Date())).tar.gz")
    }

    private func latestBackupPackage(vmId: String, agent: AgentKind) throws -> URL? {
        let dir = try backupPackageDirectory(vmId: vmId, agent: agent)
        let items = (try? fileManager.contentsOfDirectory(
            at: dir,
            includingPropertiesForKeys: [.contentModificationDateKey],
            options: [.skipsHiddenFiles]
        )) ?? []
        return items
            .filter { $0.pathExtension == "gz" && $0.lastPathComponent.hasPrefix("agent-data-") }
            .sorted { lhs, rhs in
                let lm = (try? lhs.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
                let rm = (try? rhs.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
                return lm > rm
            }
            .first
    }

    private func rotateBackups(vmId: String, agent: AgentKind, keep: Int) {
        guard let dir = try? backupPackageDirectory(vmId: vmId, agent: agent),
              let items = try? fileManager.contentsOfDirectory(
                at: dir,
                includingPropertiesForKeys: [.contentModificationDateKey],
                options: [.skipsHiddenFiles]
              ) else { return }
        let packages = items
            .filter { $0.pathExtension == "gz" && $0.lastPathComponent.hasPrefix("agent-data-") }
            .sorted { lhs, rhs in
                let lm = (try? lhs.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
                let rm = (try? rhs.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
                return lm > rm
            }
        for old in packages.dropFirst(keep) {
            try? fileManager.removeItem(at: old)
        }
    }

    private static func profileExportCommand(agent: AgentKind, outputPath: String) -> String {
        let relPath = agentDataRelativePath(agent)
        let excludes = agentExcludeArgs(agent)
        let outDir = (outputPath as NSString).deletingLastPathComponent
        let workDir = "\(outDir)/.tenbox-profile-work"
        return """
        set -eu
        home="${HOME:-/home/tenbox}"
        rel=\(shellQuote(relPath))
        src="$home/$rel"
        out=\(shellQuote(outputPath))
        work=\(shellQuote(workDir))
        [ -d "$src" ] || { echo "Agent data is not initialized: $src" >&2; exit 1; }
        rm -rf "$work"
        mkdir -p "$work"
        cat > "$work/manifest.json" <<EOF
        {
          "format": "tenbox-agent-profile",
          "format_version": 2,
          "agent_type": "\(agent.rawValue)",
          "archive": "files.tar.gz"
        }
        EOF
        (cd "$home" && tar \(excludes) -czf "$work/files.tar.gz" "$rel")
        rm -f "$out"
        tar -czf "$out" -C "$work" manifest.json files.tar.gz
        rm -rf "$work"
        echo "$out"
        """
    }

    private static func withSharedFolderReady(tag: String, body: String) -> String {
        let path = "/mnt/shared/\(tag)"
        return """
        set -eu
        share_dir=\(shellQuote(path))
        i=0
        while [ "$i" -lt 100 ]; do
          if [ -d "$share_dir" ] && [ -w "$share_dir" ]; then
            break
          fi
          i=$((i + 1))
          sleep 0.2
        done
        [ -d "$share_dir" ] || { echo "shared folder not mounted: $share_dir" >&2; exit 1; }
        [ -w "$share_dir" ] || { echo "shared folder is not writable: $share_dir" >&2; exit 1; }
        \(body)
        """
    }

    private static func profileImportCommand(agent: AgentKind, inputPath: String) -> String {
        let relPath = agentDataRelativePath(agent)
        return """
        set -eu
        home="${HOME:-/home/tenbox}"
        input=\(shellQuote(inputPath))
        rel=\(shellQuote(relPath))
        target="$home/$rel"
        work=\(shellQuote((inputPath as NSString).deletingLastPathComponent + "/.tenbox-profile-import"))
        [ -f "$input" ] || { echo "package not found: $input" >&2; exit 1; }
        rm -rf "$work"
        mkdir -p "$work"
        tar --touch -xzf "$input" -C "$work"
        [ -f "$work/manifest.json" ] || { echo "manifest.json missing" >&2; exit 1; }
        [ -f "$work/files.tar.gz" ] || { echo "files.tar.gz missing" >&2; exit 1; }
        pkg_agent="$(awk -F\\" '/agent_type/ {print $4; exit}' "$work/manifest.json")"
        [ "$pkg_agent" = "\(agent.rawValue)" ] || { echo "package is for $pkg_agent, not \(agent.rawValue)" >&2; exit 1; }
        backup=""
        if [ -e "$target" ]; then
          backup="$target.pre-import-$(date -u +%Y%m%d%H%M%S)"
          mv "$target" "$backup"
        fi
        if ! tar -xzf "$work/files.tar.gz" -C "$home"; then
          rm -rf "$target"
          if [ -n "$backup" ] && [ -d "$backup" ]; then mv "$backup" "$target"; fi
          echo "failed to restore Agent data" >&2
          exit 1
        fi
        chmod 700 "$target" 2>/dev/null || true
        rm -rf "$work"
        if [ -n "$backup" ]; then echo "$backup"; else echo "imported"; fi
        """
    }

    private static func healthStatusCommand(agent: AgentKind) -> String {
        let service = serviceName(agent)
        let gatewayPort = agent == .openclaw ? "18789" : ""
        return """
        set -u
        svc=\(shellQuote(service))
        agent=\(shellQuote(agent.rawValue))
        port=\(shellQuote(gatewayPort))
        if XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" systemctl --user is-active --quiet "$svc" 2>/dev/null; then service_state=ok; else service_state=error; fi
        if [ -z "$port" ]; then port_state=skipped; elif nc -z 127.0.0.1 "$port" >/dev/null 2>&1; then port_state=ok; else port_state=error; fi
        if curl -fsS --max-time 5 http://10.0.2.3/v1/models >/dev/null 2>&1; then model_state=ok; else model_state=error; fi
        if command -v chromium >/dev/null 2>&1 || command -v chromium-browser >/dev/null 2>&1; then browser_state=ok; else browser_state=error; fi
        free_kb="$(df -Pk "$HOME" 2>/dev/null | awk 'NR==2 {print $4}')"
        if [ "${free_kb:-0}" -gt 1048576 ]; then disk_state=ok; else disk_state=space_low; fi
        state=ok
        message="Agent normal"
        if [ "$disk_state" = space_low ]; then state=error; message="Disk space is low"; fi
        if [ "$service_state" = error ]; then state=error; message="Agent service is not running"; fi
        if [ "$port_state" = error ]; then state=error; message="Agent gateway is unavailable"; fi
        if [ "$model_state" = error ]; then state=error; message="Model proxy is unavailable"; fi
        if [ "$browser_state" = error ]; then state=error; message="Browser is unavailable"; fi
        printf '{"agent_type":"%s","state":"%s","message":"%s","checks":{"agent_service":"%s","gateway_port":"%s","llm_proxy":"%s","browser":"%s","disk":"%s"}}\\n' "$agent" "$state" "$message" "$service_state" "$port_state" "$model_state" "$browser_state" "$disk_state"
        """
    }

    private static func restartCommand(agent: AgentKind) -> String {
        """
        XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" systemctl --user restart \(shellQuote(serviceName(agent)))
        \(healthStatusCommand(agent: agent))
        """
    }

    private static func testModelCommand(agent: AgentKind) -> String {
        """
        set -eu
        if curl -fsS --max-time 5 http://10.0.2.3/v1/models >/dev/null 2>&1; then
          printf '{"agent_type":"%s","state":"ok","message":"Model proxy is available"}\\n' \(shellQuote(agent.rawValue))
        else
          printf '{"agent_type":"%s","state":"error","message":"Model proxy is unavailable"}\\n' \(shellQuote(agent.rawValue))
          exit 1
        fi
        """
    }

    private static func resetConfigCommand(agent: AgentKind) -> String {
        switch agent {
        case .hermes:
            return """
            set -eu
            mkdir -p "$HOME/.hermes"
            cat > "$HOME/.hermes/config.yaml" <<'EOF'
            model:
              default: "default"
              provider: "custom"
              base_url: "http://10.0.2.3/v1"

            terminal:
              backend: local

            approvals:
              mode: off
              timeout: 60

            display:
              streaming: true
            EOF
            \(healthStatusCommand(agent: agent))
            """
        case .openclaw:
            return """
            set -eu
            command -v openclaw >/dev/null 2>&1 || { echo "OpenClaw command is missing" >&2; exit 1; }
            openclaw config set models.providers.tenbox '{"baseUrl":"http://10.0.2.3/v1","apiKey":"tenbox","api":"openai-completions","models":[{"id":"default","name":"Default (TenBox Proxy)","reasoning":false,"input":["text","image"],"contextWindow":200000,"maxTokens":65536,"cost":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0}}]}' >/dev/null
            openclaw config set models.mode merge >/dev/null
            openclaw config set agents.defaults '{"model":{"primary":"tenbox/default"},"compaction":{"mode":"safeguard"},"workspace":"'"$HOME"'/.openclaw/workspace","models":{"tenbox/default":{}}}' >/dev/null
            \(healthStatusCommand(agent: agent))
            """
        }
    }

    private static func diagnosticsCommand(agent: AgentKind, outputDir: String) -> String {
        let service = serviceName(agent)
        return """
        set -eu
        out=\(shellQuote(outputDir))/tenbox-agent-diagnostics-\(agent.rawValue)-$(date -u +%Y%m%d%H%M%S).tar.gz
        tmp=\(shellQuote(outputDir))/.tenbox-diagnostics-work
        rm -rf "$tmp"
        mkdir -p "$tmp"
        \(healthStatusCommand(agent: agent)) > "$tmp/health.json" 2>&1 || true
        XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" systemctl --user status \(shellQuote(service)) --no-pager > "$tmp/service.txt" 2>&1 || true
        journalctl --user -u \(shellQuote(service)) -n 200 --no-pager > "$tmp/journal.txt" 2>&1 || true
        df -h > "$tmp/disk.txt" 2>&1 || true
        sed -Ei 's/(sk-[A-Za-z0-9_-]{8})[A-Za-z0-9_-]+/\\1***/g; s/(api[_-]?key[=: ]+)[^ ]+/\\1***/Ig' "$tmp"/*.txt "$tmp"/*.json 2>/dev/null || true
        tar -czf "$out" -C "$tmp" .
        rm -rf "$tmp"
        echo "$out"
        """
    }

    private static func agentDataRelativePath(_ agent: AgentKind) -> String {
        switch agent {
        case .hermes: return ".hermes"
        case .openclaw: return ".openclaw"
        }
    }

    private static func agentExcludeArgs(_ agent: AgentKind) -> String {
        switch agent {
        case .hermes:
            return [
                "--exclude", ".hermes/logs",
                "--exclude", ".hermes/image_cache",
                "--exclude", ".hermes/audio_cache",
                "--exclude", ".hermes/hermes-agent",
                "--exclude", ".hermes/bin",
                "--exclude", ".hermes/gateway.pid",
                "--exclude", ".hermes/gateway.lock",
            ].map(shellQuote).joined(separator: " ")
        case .openclaw:
            return [
                "--exclude", ".openclaw/cache",
                "--exclude", ".openclaw/.cache",
                "--exclude", ".openclaw/workspace/.cache",
            ].map(shellQuote).joined(separator: " ")
        }
    }

    private static func serviceName(_ agent: AgentKind) -> String {
        switch agent {
        case .hermes: return "hermes-gateway.service"
        case .openclaw: return "openclaw-gateway.service"
        }
    }

    private static func shellQuote(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    private static func makeError(_ message: String) -> Error {
        ConsoleCommandError(message)
    }
}
