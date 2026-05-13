import Foundation

private enum AgentLocale {
    static var isChinese: Bool {
        Locale.preferredLanguages.first?.lowercased().hasPrefix("zh") == true
    }
}

func AgentText(_ english: String, _ chinese: String) -> String {
    AgentLocale.isChinese ? chinese : english
}

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

enum AgentSkillConflictStrategy: String, CaseIterable, Identifiable {
    case skip
    case overwrite
    case rename

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .skip: return AgentText("Keep Hermes skills", "技能保留 Hermes")
        case .overwrite: return AgentText("Overwrite Hermes skills", "技能覆盖 Hermes")
        case .rename: return AgentText("Rename imported skills", "技能重命名导入")
        }
    }

    var help: String {
        switch self {
        case .skip: return AgentText("Keep existing Hermes skills when names conflict; target-level config conflicts follow Hermes migration rules.", "遇到同名技能时保留目标 Hermes 版本；目标级配置冲突会按 Hermes 迁移规则覆盖")
        case .overwrite: return AgentText("Overwrite conflicting Hermes skills with OpenClaw versions; target-level config conflicts follow Hermes migration rules.", "遇到同名技能时使用 OpenClaw 版本覆盖；目标级配置冲突会按 Hermes 迁移规则覆盖")
        case .rename: return AgentText("Import conflicting OpenClaw skills under renamed copies; target-level config conflicts follow Hermes migration rules.", "遇到同名技能时将 OpenClaw 版本重命名导入；目标级配置冲突会按 Hermes 迁移规则覆盖")
        }
    }
}

struct AgentMigrationOptions: Equatable {
    var skillConflictStrategy: AgentSkillConflictStrategy = .skip
    var workspaceTarget: String = "/home/tenbox/.hermes/workspace/openclaw-migrated"
}

enum AgentMigrationStep: String {
    case backup
    case exportSource
    case dryRun
    case migrate
    case restart
    case health
    case complete

    var title: String {
        switch self {
        case .backup: return AgentText("Back up Hermes", "备份 Hermes")
        case .exportSource: return AgentText("Export OpenClaw", "导出 OpenClaw")
        case .dryRun: return AgentText("Check migration plan", "检查迁移计划")
        case .migrate: return AgentText("Run migration", "执行迁移")
        case .restart: return AgentText("Restart Hermes", "重启 Hermes")
        case .health: return AgentText("Health check", "健康检查")
        case .complete: return AgentText("Complete", "完成")
        }
    }
}

struct AgentMigrationProgress: Identifiable, Equatable {
    let id = UUID()
    let step: AgentMigrationStep
    let message: String
    let detail: String?
    let date: Date

    init(step: AgentMigrationStep, message: String, detail: String? = nil, date: Date = Date()) {
        self.step = step
        self.message = message
        self.detail = detail
        self.date = date
    }
}

struct AgentBackupPackage: Identifiable, Equatable {
    let url: URL
    let modifiedAt: Date
    let sizeBytes: Int64

    var id: String { url.path }
    var filename: String { url.lastPathComponent }
}

struct AgentBackupSchedule: Codable, Equatable {
    static let defaultHour = 3
    static let defaultMinute = 0
    static let defaultKeepCount = 7

    var enabled: Bool
    var hour: Int
    var minute: Int
    var keepCount: Int
    var lastRunDate: String?
    var lastAttemptAt: String?
    var lastAttemptStatus: String?
    var lastAttemptMessage: String?

    init(enabled: Bool = false,
         hour: Int = Self.defaultHour,
         minute: Int = Self.defaultMinute,
         keepCount: Int = Self.defaultKeepCount,
         lastRunDate: String? = nil,
         lastAttemptAt: String? = nil,
         lastAttemptStatus: String? = nil,
         lastAttemptMessage: String? = nil) {
        self.enabled = enabled
        self.hour = min(max(hour, 0), 23)
        self.minute = min(max(minute, 0), 59)
        self.keepCount = min(max(keepCount, 1), 99)
        self.lastRunDate = lastRunDate
        self.lastAttemptAt = lastAttemptAt
        self.lastAttemptStatus = lastAttemptStatus
        self.lastAttemptMessage = lastAttemptMessage
    }

    var timeText: String {
        String(format: "%02d:%02d", hour, minute)
    }
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
            do {
                try self.prepareAgentToolScript(share: share)
                let packageName = destinationURL.lastPathComponent.isEmpty
                    ? "\(vm.name)-\(agent.rawValue)-profile.tar.gz"
                    : destinationURL.lastPathComponent
                let guestPackage = "/mnt/shared/\(share.tag)/\(packageName)"
                let command = Self.scriptCommand(tag: share.tag, args: ["export-profile", agent.rawValue, guestPackage, "migration"])
                session.runGuestAgentCommand(command, timeout: 420) { result in
                    switch result {
                    case .success(let commandResult):
                        guard commandResult.exitCode == 0 else {
                            cleanup()
                            completion(.failure(Self.makeToolError(commandResult, fallback: AgentText("Agent export failed", "Agent 数据导出失败"))))
                            return
                        }
                        let hostPackage = URL(fileURLWithPath: share.hostPath).appendingPathComponent(packageName)
                        do {
                            if self.fileManager.fileExists(atPath: destinationURL.path) { try self.fileManager.removeItem(at: destinationURL) }
                            try self.fileManager.copyItem(at: hostPackage, to: destinationURL)
                            cleanup()
                            completion(.success(AgentToolResult(message: AgentText("Exported to \(destinationURL.path)", "已导出到 \(destinationURL.path)"), output: commandResult.output)))
                        } catch {
                            cleanup()
                            completion(.failure(error))
                        }
                    case .failure(let error):
                        cleanup()
                        completion(.failure(Self.localizedGuestError(error)))
                    }
                }
            } catch {
                cleanup()
                completion(.failure(error))
            }
        } failure: { completion(.failure($0)) }
    }

    func importProfile(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                       sourceURL: URL,
                       completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        withOperationShare(vmId: vm.id, appState: appState) { share, cleanup in
            do {
                try self.prepareAgentToolScript(share: share)
                let packageName = "tenbox-agent-profile-import.tar.gz"
                let hostPackage = URL(fileURLWithPath: share.hostPath).appendingPathComponent(packageName)
                if self.fileManager.fileExists(atPath: hostPackage.path) { try self.fileManager.removeItem(at: hostPackage) }
                try self.fileManager.copyItem(at: sourceURL, to: hostPackage)
                let guestPackage = "/mnt/shared/\(share.tag)/\(packageName)"
                let command = Self.scriptCommand(tag: share.tag, args: ["import-profile", agent.rawValue, guestPackage])
                session.runGuestAgentCommand(command, timeout: 420) { result in
                    cleanup()
                    switch result {
                    case .success(let commandResult):
                        guard commandResult.exitCode == 0 else {
                            completion(.failure(Self.makeToolError(commandResult, fallback: AgentText("Agent import failed", "Agent 数据导入失败"))))
                            return
                        }
                        completion(.success(AgentToolResult(message: AgentText("Agent data imported", "已导入 Agent 数据"), output: commandResult.output)))
                    case .failure(let error):
                        completion(.failure(Self.localizedGuestError(error)))
                    }
                }
            } catch {
                cleanup()
                completion(.failure(error))
            }
        } failure: { completion(.failure($0)) }
    }

    func backupStatus(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                      completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        do {
            let latest = try latestBackupPackage(vmId: vm.id, agent: agent)
            completion(.success(AgentToolResult(
                message: latest == nil ? AgentText("No backups yet", "还没有备份") : AgentText("Agent data is protected", "Agent 数据已保护"),
                output: latest == nil ? AgentText("Create the first backup with Back Up Now.", "点击“立即备份”创建第一份备份。") : AgentText("Latest backup: \(latest!.path)", "最近备份：\(latest!.path)")
            )))
        } catch { completion(.failure(error)) }
    }

    func snapshotBackup(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                        keepCount: Int = AgentBackupSchedule.defaultKeepCount,
                        completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        do {
            let package = try backupPackageURL(vmId: vm.id, agent: agent)
            withBackupShare(vmId: vm.id, appState: appState) { share, cleanup in
                do {
                    try self.prepareAgentToolScript(share: share)
                    let guestDir = "/mnt/shared/\(share.tag)/\(agent.rawValue)"
                    let guestPackage = "\(guestDir)/\(package.lastPathComponent)"
                    let command = Self.withSharedFolderReady(tag: share.tag, body: "mkdir -p \(Self.shellQuote(guestDir))\n" + Self.scriptInvocation(tag: share.tag, args: ["export-profile", agent.rawValue, guestPackage, "backup"]))
                    session.runGuestAgentCommand(command, timeout: 420) { result in
                        cleanup()
                        switch result {
                        case .success(let commandResult):
                            guard commandResult.exitCode == 0 else {
                                completion(.failure(Self.makeToolError(commandResult, fallback: AgentText("Agent backup failed", "Agent 备份失败"))))
                                return
                            }
                            self.rotateBackups(vmId: vm.id, agent: agent, keep: keepCount)
                            completion(.success(AgentToolResult(message: AgentText("Agent data backup created", "已创建 Agent 数据备份"), output: package.path)))
                        case .failure(let error):
                            completion(.failure(Self.localizedGuestError(error)))
                        }
                    }
                } catch {
                    cleanup()
                    completion(.failure(error))
                }
            } failure: { completion(.failure($0)) }
        } catch { completion(.failure(error)) }
    }

    func restoreBackup(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                       packageURL: URL,
                       completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        withBackupShare(vmId: vm.id, appState: appState) { share, cleanup in
            do {
                try self.prepareAgentToolScript(share: share)
                let guestPackage = "/mnt/shared/\(share.tag)/\(agent.rawValue)/\(packageURL.lastPathComponent)"
                let command = Self.scriptCommand(tag: share.tag, args: ["import-profile", agent.rawValue, guestPackage])
                session.runGuestAgentCommand(command, timeout: 420) { result in
                    cleanup()
                    switch result {
                    case .success(let commandResult):
                        guard commandResult.exitCode == 0 else {
                            completion(.failure(Self.makeToolError(commandResult, fallback: AgentText("Agent backup restore failed", "Agent 备份恢复失败"))))
                            return
                        }
                        completion(.success(AgentToolResult(message: AgentText("Agent data backup restored", "已恢复 Agent 数据备份"), output: packageURL.path)))
                    case .failure(let error):
                        completion(.failure(Self.localizedGuestError(error)))
                    }
                }
            } catch {
                cleanup()
                completion(.failure(error))
            }
        } failure: { completion(.failure($0)) }
    }

    func healthStatus(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                      completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        runHealthCommand(vm: vm, session: session, appState: appState, agent: agent,
                         args: ["health", agent.rawValue],
                         failureFallback: AgentText("Agent health check failed", "Agent 健康检查失败"),
                         successMessage: AgentText("Health status refreshed", "健康状态已更新"),
                         completion: completion)
    }

    func restartAgent(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                      keepCount: Int = AgentBackupSchedule.defaultKeepCount,
                      completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        runRepairCommand(vm: vm, session: session, appState: appState, agent: agent,
                         repairArgs: ["restart", agent.rawValue],
                         successMessage: AgentText("Agent restarted", "已重新启动 Agent"),
                         keepCount: keepCount,
                         completion: completion)
    }

    func testModel(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                   completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        runHealthCommand(vm: vm, session: session, appState: appState, agent: agent,
                         args: ["test-model", agent.rawValue],
                         failureFallback: AgentText("Model connection test failed", "模型连接测试失败"),
                         successMessage: AgentText("Model connection tested", "模型连接已测试"),
                         completion: completion)
    }

    func resetAgentConfig(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                          keepCount: Int = AgentBackupSchedule.defaultKeepCount,
                          completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        runRepairCommand(vm: vm, session: session, appState: appState, agent: agent,
                         repairArgs: ["reset-config", agent.rawValue],
                         successMessage: AgentText("Agent configuration reset", "已重置 Agent 配置"),
                         keepCount: keepCount,
                         completion: completion)
    }

    func exportDiagnostics(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                           completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        withBackupShare(vmId: vm.id, appState: appState) { share, cleanup in
            do {
                try self.prepareAgentToolScript(share: share)
                let guestDir = "/mnt/shared/\(share.tag)"
                let command = Self.scriptCommand(tag: share.tag, args: ["diagnostics", agent.rawValue, guestDir])
                session.runGuestAgentCommand(command, timeout: 180) { result in
                    cleanup()
                    switch result {
                    case .success(let commandResult):
                        guard commandResult.exitCode == 0 else {
                            completion(.failure(Self.makeToolError(commandResult, fallback: AgentText("Agent diagnostics export failed", "Agent 诊断导出失败"))))
                            return
                        }
                        completion(.success(AgentToolResult(message: AgentText("Diagnostics exported", "已导出诊断包"), output: commandResult.output)))
                    case .failure(let error):
                        completion(.failure(Self.localizedGuestError(error)))
                    }
                }
            } catch {
                cleanup()
                completion(.failure(error))
            }
        } failure: { completion(.failure($0)) }
    }

    func migrateOpenClawToHermes(sourceVm: VmInfo, sourceSession: VmSession,
                                 targetVm: VmInfo, targetSession: VmSession,
                                 appState: AppState,
                                 options: AgentMigrationOptions = AgentMigrationOptions(),
                                 keepCount: Int = AgentBackupSchedule.defaultKeepCount,
                                 progress: @escaping (AgentMigrationProgress) -> Void = { _ in },
                                 completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        let emit: (AgentMigrationStep, String, String?) -> Void = { step, message, detail in
            DispatchQueue.main.async { progress(AgentMigrationProgress(step: step, message: message, detail: detail)) }
        }
        do {
            let backupPackage = try backupPackageURL(vmId: targetVm.id, agent: .hermes)
            let reportURL = try migrationReportURL(vmId: targetVm.id, agent: .hermes)
            withBackupShare(vmId: targetVm.id, appState: appState) { backupShare, backupCleanup in
                do { try self.prepareAgentToolScript(share: backupShare) } catch { backupCleanup(); completion(.failure(error)); return }
                withOperationShare(vmIds: [sourceVm.id, targetVm.id], appState: appState) { share, cleanup in
                    do { try self.prepareAgentToolScript(share: share) } catch { cleanup(); backupCleanup(); completion(.failure(error)); return }
                    let cleanupAll = { cleanup(); backupCleanup() }
                    let guestBackupDir = "/mnt/shared/\(backupShare.tag)/hermes"
                    let guestBackup = "\(guestBackupDir)/\(backupPackage.lastPathComponent)"
                    let guestReport = "\(guestBackupDir)/\(reportURL.lastPathComponent)"
                    let backupCommand = Self.withSharedFolderReady(tag: backupShare.tag, body: "mkdir -p \(Self.shellQuote(guestBackupDir))\n" + Self.scriptInvocation(tag: backupShare.tag, args: ["export-profile", AgentKind.hermes.rawValue, guestBackup, "backup"]))
                    emit(.backup, AgentText("Creating target Hermes pre-migration backup", "正在创建目标 Hermes 迁移前备份"), backupPackage.lastPathComponent)
                    targetSession.runGuestAgentCommand(backupCommand, timeout: 420) { backupResult in
                        switch backupResult {
                        case .success(let backupCommandResult):
                            guard backupCommandResult.exitCode == 0 else { cleanupAll(); completion(.failure(Self.makeToolError(backupCommandResult, fallback: AgentText("Hermes pre-migration backup failed", "Hermes 迁移前备份失败")))); return }
                            let archivePath = "/mnt/shared/\(share.tag)/openclaw-source.tar.gz"
                            let exportCommand = Self.scriptCommand(tag: share.tag, args: ["export-openclaw-source", archivePath])
                            emit(.exportSource, AgentText("Exporting OpenClaw user data from source VM", "正在从来源 VM 导出 OpenClaw 用户数据"), sourceVm.name)
                            sourceSession.runGuestAgentCommand(exportCommand, timeout: 420) { sourceResult in
                                switch sourceResult {
                                case .success(let sourceCommandResult):
                                    guard sourceCommandResult.exitCode == 0 else { cleanupAll(); completion(.failure(Self.makeToolError(sourceCommandResult, fallback: AgentText("OpenClaw data export failed", "OpenClaw 数据导出失败")))); return }
                                    let dryRunCommand = Self.scriptCommand(tag: share.tag, args: ["migrate-openclaw-dry-run", archivePath, guestReport, options.skillConflictStrategy.rawValue, options.workspaceTarget])
                                    emit(.dryRun, AgentText("Generating official dry-run migration plan", "正在生成官方 dry-run 迁移计划"), AgentText("Conflict strategy: \(options.skillConflictStrategy.displayName)", "冲突策略：\(options.skillConflictStrategy.displayName)"))
                                    targetSession.runGuestAgentCommand(dryRunCommand, timeout: 420) { dryRunResult in
                                        switch dryRunResult {
                                        case .success(let dryRunCommandResult):
                                            guard dryRunCommandResult.exitCode == 0 else { cleanupAll(); completion(.failure(Self.makeToolError(dryRunCommandResult, fallback: AgentText("OpenClaw to Hermes migration preflight failed", "OpenClaw 到 Hermes 迁移预检失败")))); return }
                                            emit(.migrate, AgentText("Dry run passed; applying migration", "dry-run 已通过，正在执行正式迁移"), reportURL.lastPathComponent)
                                            let migrateCommand = Self.scriptCommand(tag: share.tag, args: ["migrate-openclaw-apply", archivePath, guestReport, options.skillConflictStrategy.rawValue, options.workspaceTarget])
                                            targetSession.runGuestAgentCommand(migrateCommand, timeout: 600) { targetResult in
                                                cleanupAll()
                                                switch targetResult {
                                                case .success(let targetCommandResult):
                                                    guard targetCommandResult.exitCode == 0 else { completion(.failure(Self.makeToolError(targetCommandResult, fallback: AgentText("OpenClaw to Hermes migration failed", "OpenClaw 到 Hermes 迁移失败")))); return }
                                                    self.rotateBackups(vmId: targetVm.id, agent: .hermes, keep: keepCount)
                                                    emit(.complete, AgentText("Migration completed; report saved", "迁移完成，报告已保存"), reportURL.lastPathComponent)
                                                    completion(.success(AgentToolResult(message: AgentText("OpenClaw to Hermes migration completed", "已完成 OpenClaw 到 Hermes 迁移"), output: """
                                                    \(AgentText("Pre-migration backup: ", "迁移前备份："))\(backupPackage.path)
                                                    \(AgentText("Migration report: ", "迁移报告："))\(reportURL.path)
                                                    \(AgentText("Source VM: ", "来源 VM："))\(sourceVm.name)
                                                    \(AgentText("Target VM: ", "目标 VM："))\(targetVm.name)
                                                    \(AgentText("Conflict strategy: ", "冲突策略："))\(options.skillConflictStrategy.displayName)
                                                    \(AgentText("Workspace target: ", "Workspace 目标："))\(options.workspaceTarget.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? AgentText("Default", "默认") : options.workspaceTarget)

                                                    [health]
                                                    \(Self.compactMigrationOutput(targetCommandResult.output) ?? AgentText("Migration command completed", "迁移命令已完成"))
                                                    """)))
                                                case .failure(let error): completion(.failure(Self.localizedGuestError(error)))
                                                }
                                            }
                                        case .failure(let error): cleanupAll(); completion(.failure(Self.localizedGuestError(error)))
                                        }
                                    }
                                case .failure(let error): cleanupAll(); completion(.failure(Self.localizedGuestError(error)))
                                }
                            }
                        case .failure(let error): cleanupAll(); completion(.failure(Self.localizedGuestError(error)))
                        }
                    }
                } failure: { error in backupCleanup(); completion(.failure(error)) }
            } failure: { completion(.failure($0)) }
        } catch { completion(.failure(error)) }
    }

    private func runHealthCommand(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                                  args: [String], failureFallback: String, successMessage: String,
                                  completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        withOperationShare(vmId: vm.id, appState: appState) { share, cleanup in
            do {
                try self.prepareAgentToolScript(share: share)
                session.runGuestAgentCommand(Self.scriptCommand(tag: share.tag, args: args), timeout: 180) { result in
                    cleanup()
                    switch result {
                    case .success(let commandResult):
                        guard commandResult.exitCode == 0 else { completion(.failure(Self.makeToolError(commandResult, fallback: failureFallback))); return }
                        completion(.success(AgentToolResult(message: successMessage, output: commandResult.output)))
                    case .failure(let error): completion(.failure(Self.localizedGuestError(error)))
                    }
                }
            } catch { cleanup(); completion(.failure(error)) }
        } failure: { completion(.failure($0)) }
    }

    private func runRepairCommand(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                                  repairArgs: [String], successMessage: String,
                                  keepCount: Int = AgentBackupSchedule.defaultKeepCount,
                                  completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        do {
            let package = try backupPackageURL(vmId: vm.id, agent: agent)
            withBackupShare(vmId: vm.id, appState: appState) { share, cleanup in
                do {
                    try self.prepareAgentToolScript(share: share)
                    let guestDir = "/mnt/shared/\(share.tag)/\(agent.rawValue)"
                    let guestPackage = "\(guestDir)/\(package.lastPathComponent)"
                    let command = Self.withSharedFolderReady(tag: share.tag, body: "mkdir -p \(Self.shellQuote(guestDir))\n" + Self.scriptInvocation(tag: share.tag, args: ["export-profile", agent.rawValue, guestPackage, "backup"]) + Self.scriptInvocation(tag: share.tag, args: repairArgs))
                    session.runGuestAgentCommand(command, timeout: 420) { result in
                        cleanup()
                        switch result {
                        case .success(let commandResult):
                            guard commandResult.exitCode == 0 else { completion(.failure(Self.makeToolError(commandResult, fallback: AgentText("Agent repair operation failed", "Agent 修复操作失败")))); return }
                            self.rotateBackups(vmId: vm.id, agent: agent, keep: keepCount)
                            completion(.success(AgentToolResult(message: successMessage, output: "\(AgentText("Pre-repair backup: ", "修复前备份："))\(package.path)\n\(commandResult.output)")))
                        case .failure(let error): completion(.failure(Self.localizedGuestError(error)))
                        }
                    }
                } catch { cleanup(); completion(.failure(error)) }
            } failure: { completion(.failure($0)) }
        } catch { completion(.failure(error)) }
    }

    private func withOperationShare(vmId: String, appState: AppState,
                                    perform: (SharedFolder, @escaping () -> Void) -> Void,
                                    failure: (Error) -> Void) {
        withOperationShare(vmIds: [vmId], appState: appState, perform: perform, failure: failure)
    }

    private func withOperationShare(vmIds: [String], appState: AppState,
                                    perform: (SharedFolder, @escaping () -> Void) -> Void,
                                    failure: (Error) -> Void) {
        do {
            let base = try operationBaseDirectory()
            let tag = "tenbox-agent-ops-\(UUID().uuidString.prefix(8).lowercased())"
            let dirName = "\(vmIds.joined(separator: "-"))-\(tag)"
            let dir = base.appendingPathComponent(dirName, isDirectory: true)
            try fileManager.createDirectory(at: dir, withIntermediateDirectories: true)
            let share = SharedFolder(tag: tag, hostPath: dir.path, readonly: false)
            for vmId in vmIds {
                appState.addRuntimeSharedFolder(share, toVm: vmId)
            }

            let cleanup: () -> Void = { [weak appState, weak self] in
                DispatchQueue.main.async {
                    for vmId in vmIds {
                        appState?.removeRuntimeSharedFolder(tag: tag, fromVm: vmId)
                    }
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
        formatter.dateFormat = "yyyy-MM-dd-HHmmss"
        return try backupPackageDirectory(vmId: vmId, agent: agent)
            .appendingPathComponent("agent-data-\(formatter.string(from: Date())).tar.gz")
    }

    private func migrationReportURL(vmId: String, agent: AgentKind) throws -> URL {
        let formatter = DateFormatter()
        formatter.calendar = Calendar(identifier: .gregorian)
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd-HHmmss"
        return try backupPackageDirectory(vmId: vmId, agent: agent)
            .appendingPathComponent("openclaw-migration-\(formatter.string(from: Date())).txt")
    }

    func listBackupPackages(vmId: String, agent: AgentKind) throws -> [AgentBackupPackage] {
        let dir = try backupPackageDirectory(vmId: vmId, agent: agent)
        let items = (try? fileManager.contentsOfDirectory(
            at: dir,
            includingPropertiesForKeys: [.contentModificationDateKey, .fileSizeKey],
            options: [.skipsHiddenFiles]
        )) ?? []
        return items
            .filter { $0.pathExtension == "gz" && $0.lastPathComponent.hasPrefix("agent-data-") }
            .map { url in
                let values = try? url.resourceValues(forKeys: [.contentModificationDateKey, .fileSizeKey])
                return AgentBackupPackage(
                    url: url,
                    modifiedAt: values?.contentModificationDate ?? .distantPast,
                    sizeBytes: Int64(values?.fileSize ?? 0)
                )
            }
            .sorted { $0.modifiedAt > $1.modifiedAt }
    }

    private func latestBackupPackage(vmId: String, agent: AgentKind) throws -> URL? {
        try listBackupPackages(vmId: vmId, agent: agent).first?.url
    }

    func rotateBackups(vmId: String, agent: AgentKind, keep: Int) {
        guard let packages = try? listBackupPackages(vmId: vmId, agent: agent) else { return }
        for old in packages.dropFirst(keep) {
            try? fileManager.removeItem(at: old.url)
        }
    }

    private func prepareAgentToolScript(share: SharedFolder) throws {
        let source = try Self.agentToolScriptURL()
        let destination = URL(fileURLWithPath: share.hostPath).appendingPathComponent("agent_tools.sh")
        if fileManager.fileExists(atPath: destination.path) { try fileManager.removeItem(at: destination) }
        try fileManager.copyItem(at: source, to: destination)
    }

    private static func agentToolScriptURL() throws -> URL {
        let candidates: [URL?] = [
            Bundle.main.url(forResource: "agent_tools", withExtension: "sh", subdirectory: "AgentTools"),
            Bundle.module.url(forResource: "agent_tools", withExtension: "sh", subdirectory: "AgentTools"),
            URL(fileURLWithPath: #filePath)
                .deletingLastPathComponent()
                .deletingLastPathComponent()
                .deletingLastPathComponent()
                .appendingPathComponent("agent_tools/guest/agent_tools.sh")
        ]
        for candidate in candidates.compactMap({ $0 }) where FileManager.default.isReadableFile(atPath: candidate.path) {
            return candidate
        }
        throw ConsoleCommandError(AgentText("Agent tools script was not found.", "找不到 Agent 工具箱脚本。"))
    }

    private static func withSharedFolderReady(tag: String, body: String) -> String {
        let path = "/mnt/shared/\(tag)"
        return """
        set -eu
        share_dir=\(shellQuote(path))
        i=0
        while [ "$i" -lt 100 ]; do
          if [ -d "$share_dir" ] && [ -w "$share_dir" ]; then break; fi
          i=$((i + 1)); sleep 0.2
        done
        [ -d "$share_dir" ] || { echo "Shared folder is not mounted: $share_dir" >&2; exit 1; }
        [ -w "$share_dir" ] || { echo "Shared folder is not writable: $share_dir" >&2; exit 1; }
        \(body)
        """
    }

    private static func scriptInvocation(tag: String, args: [String]) -> String {
        let script = "/mnt/shared/\(tag)/agent_tools.sh"
        let quotedArgs = args.map(shellQuote).joined(separator: " ")
        return """
        script=\(shellQuote(script))
        [ -f "$script" ] || { echo "Agent tools script is missing: $script" >&2; exit 1; }
        chmod +x "$script" 2>/dev/null || true
        /bin/sh "$script" \(quotedArgs)
        """
    }

    private static func scriptCommand(tag: String, args: [String]) -> String {
        withSharedFolderReady(tag: tag, body: scriptInvocation(tag: tag, args: args))
    }

    private static func compactMigrationOutput(_ output: String) -> String? {
        let lines = output
            .split(whereSeparator: { $0.isNewline })
            .map { String($0).trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
        guard !lines.isEmpty else { return nil }
        return lines.prefix(8).joined(separator: "\n")
    }

    private static func shellQuote(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    private static func makeToolError(_ result: ConsoleCommandResult, fallback: String) -> Error {
        let message = result.output.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? fallback : result.output
        return ConsoleCommandError(localizedGuestMessage(message))
    }

    private static func localizedGuestError(_ error: Error) -> Error {
        ConsoleCommandError(localizedGuestMessage(error.localizedDescription))
    }

    private static func localizedGuestMessage(_ message: String) -> String {
        if message.contains("Agent tools require a Linux guest OS") || message.contains("/bin/sh") || message.contains("No such file") {
            return AgentText("Agent tools require a Linux guest OS.", "Agent 工具箱需要 Linux Guest OS。")
        }
        return message
    }
}
