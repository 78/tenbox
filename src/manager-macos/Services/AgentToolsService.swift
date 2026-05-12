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

enum AgentSkillConflictStrategy: String, CaseIterable, Identifiable {
    case skip
    case overwrite
    case rename

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .skip: return "技能保留 Hermes"
        case .overwrite: return "技能覆盖 Hermes"
        case .rename: return "技能重命名导入"
        }
    }

    var help: String {
        switch self {
        case .skip: return "遇到同名技能时保留目标 Hermes 版本；目标级配置冲突会按 Hermes 迁移规则覆盖"
        case .overwrite: return "遇到同名技能时使用 OpenClaw 版本覆盖；目标级配置冲突会按 Hermes 迁移规则覆盖"
        case .rename: return "遇到同名技能时将 OpenClaw 版本重命名导入；目标级配置冲突会按 Hermes 迁移规则覆盖"
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
        case .backup: return "备份 Hermes"
        case .exportSource: return "导出 OpenClaw"
        case .dryRun: return "检查迁移计划"
        case .migrate: return "执行迁移"
        case .restart: return "重启 Hermes"
        case .health: return "健康检查"
        case .complete: return "完成"
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

private enum AgentProfileExportScope: String {
    case migration
    case backup
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
            let packageName = destinationURL.lastPathComponent.isEmpty
                ? "\(vm.name)-\(agent.rawValue)-profile.tar.gz"
                : destinationURL.lastPathComponent
            let guestPackage = "/mnt/shared/\(share.tag)/\(packageName)"
            let command = Self.withSharedFolderReady(
                tag: share.tag,
                body: Self.profileExportCommand(agent: agent, outputPath: guestPackage, scope: .migration)
            )

            session.runGuestAgentCommand(command, timeout: 420) { result in
                switch result {
                case .success(let commandResult):
                    guard commandResult.exitCode == 0 else {
                        cleanup()
                        completion(.failure(Self.makeError(commandResult.output.isEmpty ? "Agent 数据导出失败" : commandResult.output)))
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
                        completion(.failure(Self.makeError(commandResult.output.isEmpty ? "Agent 数据导入失败" : commandResult.output)))
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
                    output: "点击“立即备份”创建第一份备份。"
                )))
            }
        } catch {
            completion(.failure(error))
        }
    }

    func snapshotBackup(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                        keepCount: Int = AgentBackupSchedule.defaultKeepCount,
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
                            completion(.failure(Self.makeError(commandResult.output.isEmpty ? "Agent 备份失败" : commandResult.output)))
                            return
                        }
                        self.rotateBackups(vmId: vm.id, agent: agent, keep: keepCount)
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

    func restoreBackup(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                       packageURL: URL,
                       completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        withBackupShare(vmId: vm.id, appState: appState) { share, cleanup in
            let guestPackage = "/mnt/shared/\(share.tag)/\(agent.rawValue)/\(packageURL.lastPathComponent)"
            let command = Self.withSharedFolderReady(
                tag: share.tag,
                body: Self.profileImportCommand(agent: agent, inputPath: guestPackage)
            )
            session.runGuestAgentCommand(command, timeout: 420) { result in
                cleanup()
                switch result {
                case .success(let commandResult):
                    guard commandResult.exitCode == 0 else {
                        completion(.failure(Self.makeError(commandResult.output.isEmpty ? "Agent 备份恢复失败" : commandResult.output)))
                        return
                    }
                    completion(.success(AgentToolResult(
                        message: "已恢复 Agent 数据备份",
                        output: packageURL.path
                    )))
                case .failure(let error):
                    completion(.failure(error))
                }
            }
        } failure: { error in
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
                      keepCount: Int = AgentBackupSchedule.defaultKeepCount,
                      completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        runRepairCommand(vm: vm, session: session, appState: appState, agent: agent,
                         repairCommand: Self.restartCommand(agent: agent),
                         successMessage: "已重新启动 Agent",
                         keepCount: keepCount,
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
                          keepCount: Int = AgentBackupSchedule.defaultKeepCount,
                          completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        runRepairCommand(vm: vm, session: session, appState: appState, agent: agent,
                         repairCommand: Self.resetConfigCommand(agent: agent),
                         successMessage: "已重置 Agent 配置",
                         keepCount: keepCount,
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
                        completion(.failure(Self.makeError(commandResult.output.isEmpty ? "Agent 诊断导出失败" : commandResult.output)))
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

    func migrateOpenClawToHermes(sourceVm: VmInfo, sourceSession: VmSession,
                                 targetVm: VmInfo, targetSession: VmSession,
                                 appState: AppState,
                                 options: AgentMigrationOptions = AgentMigrationOptions(),
                                 keepCount: Int = AgentBackupSchedule.defaultKeepCount,
                                 progress: @escaping (AgentMigrationProgress) -> Void = { _ in },
                                 completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        let emit: (AgentMigrationStep, String, String?) -> Void = { step, message, detail in
            DispatchQueue.main.async {
                progress(AgentMigrationProgress(step: step, message: message, detail: detail))
            }
        }

        do {
            let backupPackage = try backupPackageURL(vmId: targetVm.id, agent: .hermes)
            let reportURL = try migrationReportURL(vmId: targetVm.id, agent: .hermes)
            withBackupShare(vmId: targetVm.id, appState: appState) { backupShare, backupCleanup in
                withOperationShare(vmIds: [sourceVm.id, targetVm.id], appState: appState) { share, cleanup in
                    let cleanupAll = {
                        cleanup()
                        backupCleanup()
                    }
                    let guestBackup = "/mnt/shared/\(backupShare.tag)/hermes/\(backupPackage.lastPathComponent)"
                    let guestReport = "/mnt/shared/\(backupShare.tag)/hermes/\(reportURL.lastPathComponent)"
                    let backupCommand = Self.withSharedFolderReady(
                        tag: backupShare.tag,
                        body: "mkdir -p \(Self.shellQuote("/mnt/shared/\(backupShare.tag)/hermes"))\n" +
                            Self.profileExportCommand(agent: .hermes, outputPath: guestBackup, scope: .backup)
                    )

                    emit(.backup, "正在创建目标 Hermes 迁移前备份", backupPackage.lastPathComponent)
                    targetSession.runGuestAgentCommand(backupCommand, timeout: 420) { backupResult in
                        switch backupResult {
                        case .success(let backupCommandResult):
                            guard backupCommandResult.exitCode == 0 else {
                                cleanupAll()
                                completion(.failure(Self.makeError(backupCommandResult.output.isEmpty ? "Hermes 迁移前备份失败" : backupCommandResult.output)))
                                return
                            }

                            let archivePath = "/mnt/shared/\(share.tag)/openclaw-source.tar.gz"
                            let exportCommand = Self.withSharedFolderReady(
                                tag: share.tag,
                                body: Self.openClawMigrationSourceExportCommand(outputPath: archivePath)
                            )
                            emit(.exportSource, "正在从来源 VM 导出 OpenClaw 用户数据", sourceVm.name)
                            sourceSession.runGuestAgentCommand(exportCommand, timeout: 420) { sourceResult in
                                switch sourceResult {
                                case .success(let sourceCommandResult):
                                    guard sourceCommandResult.exitCode == 0 else {
                                        cleanupAll()
                                        completion(.failure(Self.makeError(sourceCommandResult.output.isEmpty ? "OpenClaw 数据导出失败" : sourceCommandResult.output)))
                                        return
                                    }

                                    let dryRunCommand = Self.withSharedFolderReady(
                                        tag: share.tag,
                                        body: Self.openClawToHermesDryRunCommand(
                                            inputPath: archivePath,
                                            reportPath: guestReport,
                                            options: options
                                        )
                                    )
                                    emit(.dryRun, "正在生成官方 dry-run 迁移计划", "冲突策略：\(options.skillConflictStrategy.displayName)")
                                    targetSession.runGuestAgentCommand(dryRunCommand, timeout: 420) { dryRunResult in
                                        switch dryRunResult {
                                        case .success(let dryRunCommandResult):
                                            guard dryRunCommandResult.exitCode == 0 else {
                                                cleanupAll()
                                                completion(.failure(Self.makeError(dryRunCommandResult.output.isEmpty ? "OpenClaw 到 Hermes 迁移预检失败" : dryRunCommandResult.output)))
                                                return
                                            }
                                            emit(.migrate, "dry-run 已通过，正在执行正式迁移", "完整计划已写入 \(reportURL.lastPathComponent)")
                                            let migrateCommand = Self.withSharedFolderReady(
                                                tag: share.tag,
                                                body: Self.openClawToHermesMigrationCommand(
                                                    inputPath: archivePath,
                                                    reportPath: guestReport,
                                                    options: options
                                                )
                                            )
                                            targetSession.runGuestAgentCommand(migrateCommand, timeout: 600) { targetResult in
                                                cleanupAll()
                                                switch targetResult {
                                                case .success(let targetCommandResult):
                                                    guard targetCommandResult.exitCode == 0 else {
                                                        completion(.failure(Self.makeError(targetCommandResult.output.isEmpty ? "OpenClaw 到 Hermes 迁移失败" : targetCommandResult.output)))
                                                        return
                                                    }
                                                    self.rotateBackups(vmId: targetVm.id, agent: .hermes, keep: keepCount)
                                                    emit(.complete, "迁移完成，报告已保存", reportURL.lastPathComponent)
                                                    completion(.success(AgentToolResult(
                                                        message: "已完成 OpenClaw 到 Hermes 迁移",
                                                        output: """
                                                        迁移前备份：\(backupPackage.path)
                                                        迁移报告：\(reportURL.path)
                                                        来源 VM：\(sourceVm.name)
                                                        目标 VM：\(targetVm.name)
                                                        冲突策略：\(options.skillConflictStrategy.displayName)
                                                        Workspace 目标：\(options.workspaceTarget.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? "默认" : options.workspaceTarget)
                                                        完整 dry-run / migrate 输出见迁移报告。

                                                        [health]
                                                        \(Self.compactMigrationOutput(targetCommandResult.output) ?? "迁移命令已完成")
                                                        """
                                                    )))
                                                case .failure(let error):
                                                    completion(.failure(error))
                                                }
                                            }
                                        case .failure(let error):
                                            cleanupAll()
                                            completion(.failure(error))
                                        }
                                    }
                                case .failure(let error):
                                    cleanupAll()
                                    completion(.failure(error))
                                }
                            }
                        case .failure(let error):
                            cleanupAll()
                            completion(.failure(error))
                        }
                    }
                } failure: { error in
                    backupCleanup()
                    completion(.failure(error))
                }
            } failure: { error in
                completion(.failure(error))
            }
        } catch {
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
                    completion(.failure(Self.makeError(commandResult.output.isEmpty ? "Agent 健康检查失败" : commandResult.output)))
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
                                  keepCount: Int = AgentBackupSchedule.defaultKeepCount,
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
                            completion(.failure(Self.makeError(commandResult.output.isEmpty ? "Agent 修复操作失败" : commandResult.output)))
                            return
                        }
                        self.rotateBackups(vmId: vm.id, agent: agent, keep: keepCount)
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

    private static func profileExportCommand(agent: AgentKind, outputPath: String,
                                             scope: AgentProfileExportScope = .backup) -> String {
        let relPath = agentDataRelativePath(agent)
        let excludes = agentExcludeArgs(agent, scope: scope)
        let outDir = (outputPath as NSString).deletingLastPathComponent
        let workDir = "\(outDir)/.tenbox-profile-work"
        return """
        set -eu
        home="${HOME:-/home/tenbox}"
        rel=\(shellQuote(relPath))
        src="$home/$rel"
        out=\(shellQuote(outputPath))
        work=\(shellQuote(workDir))
        [ -d "$src" ] || { echo "Agent 数据尚未初始化：$src" >&2; exit 1; }
        rm -rf "$work"
        mkdir -p "$work"
        cat > "$work/manifest.json" <<EOF
        {
          "format": "tenbox-agent-profile",
          "format_version": 2,
          "agent_type": "\(agent.rawValue)",
          "export_scope": "\(scope.rawValue)",
          "archive": "files.tar.gz"
        }
        EOF
        tar_status=0
        (cd "$home" && tar --warning=no-file-changed --ignore-failed-read \(excludes) -czf "$work/files.tar.gz" "$rel") || tar_status=$?
        [ "$tar_status" -le 1 ] || exit "$tar_status"
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
        [ -d "$share_dir" ] || { echo "共享文件夹未挂载：$share_dir" >&2; exit 1; }
        [ -w "$share_dir" ] || { echo "共享文件夹不可写：$share_dir" >&2; exit 1; }
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
        [ -f "$input" ] || { echo "找不到导入包：$input" >&2; exit 1; }
        input_dir="$(dirname "$input")"
        work="$input_dir/.tenbox-profile-import-$(date -u +%Y%m%d%H%M%S)-$$"
        rm -rf "$work"
        mkdir -p "$work"
        trap 'rm -rf "$work"' EXIT
        tar --touch --no-same-owner -xzf "$input" -C "$work"
        [ -f "$work/manifest.json" ] || { echo "导入包缺少 manifest.json" >&2; exit 1; }
        [ -f "$work/files.tar.gz" ] || { echo "导入包缺少 files.tar.gz" >&2; exit 1; }
        pkg_agent=""
        if command -v python3 >/dev/null 2>&1; then
          pkg_agent="$(python3 - "$work/manifest.json" <<'PY'
        import json
        import sys
        with open(sys.argv[1], "r", encoding="utf-8") as f:
            print(json.load(f).get("agent_type", ""))
        PY
          )" || pkg_agent=""
        fi
        if [ -z "$pkg_agent" ]; then
          pkg_agent="$(awk -F\\" '/agent_type/ {print $4; exit}' "$work/manifest.json")"
        fi
        [ "$pkg_agent" = "\(agent.rawValue)" ] || { echo "导入包属于 $pkg_agent，不是 \(agent.rawValue)" >&2; exit 1; }
        tar -tzf "$work/files.tar.gz" > "$work/files.list"
        if ! awk -v rel="$rel" '
        BEGIN { prefix = rel "/"; found = 0; bad = 0 }
        { name = $0; if (name == rel || name == prefix) { found = 1; next }
          if (index(name, prefix) == 1) { found = 1 } else { bad = 1 }
          if (name ~ /^\\// || name ~ /(^|\\/)\\.\\.(\\/|$)/) { bad = 1 }
          if (bad) exit 1 }
        END { if (!found) exit 2; exit 0 }
        ' "$work/files.list"; then
          echo "导入包包含非法路径或缺少 $rel 目录" >&2
          exit 1
        fi
        backup=""
        if [ -e "$target" ]; then
          backup="$input_dir/pre-import-\(agent.rawValue)-$(date -u +%Y%m%d%H%M%S).tar.gz"
          backup_status=0
          (cd "$home" && tar --warning=no-file-changed --ignore-failed-read -czf "$backup" "$rel") || backup_status=$?
          if [ "$backup_status" -gt 1 ]; then
            rm -f "$backup"
            echo "创建导入前备份失败" >&2
            exit "$backup_status"
          fi
        fi
        mkdir -p "$target"
        awk -v rel="$rel/" 'index($0, rel) == 1 { rest=substr($0, length(rel)+1); split(rest, a, "/"); if (a[1] != "") print a[1] }' "$work/files.list" | sort -u | while IFS= read -r item; do
          [ -n "$item" ] || continue
          rm -rf "$target/$item"
        done
        if ! tar --touch --no-same-owner -xzf "$work/files.tar.gz" -C "$home"; then
          rm -rf "$target"
          if [ -n "$backup" ] && [ -f "$backup" ]; then tar --touch --no-same-owner -xzf "$backup" -C "$home"; fi
          echo "恢复 Agent 数据失败" >&2
          exit 1
        fi
        chmod 700 "$target" 2>/dev/null || true
        svc="$(\(serviceResolverCommand(agent: agent)))"
        if [ -n "$svc" ]; then
          XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" systemctl --user restart "$svc" >/dev/null 2>&1 || true
        fi
        if [ -n "$backup" ]; then echo "$backup"; else echo "已导入"; fi
        """
    }

    private static func healthStatusCommand(agent: AgentKind) -> String {
        let gatewayPort = agent == .openclaw ? "18789" : ""
        return """
        set -u
        svc="$(\(serviceResolverCommand(agent: agent)))"
        agent=\(shellQuote(agent.rawValue))
        port=\(shellQuote(gatewayPort))
        if XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" systemctl --user is-active --quiet "$svc" 2>/dev/null; then service_state=ok; else service_state=error; fi
        if [ -z "$port" ]; then port_state=skipped; elif nc -z 127.0.0.1 "$port" >/dev/null 2>&1; then port_state=ok; else port_state=error; fi
        if curl -fsS --max-time 5 http://10.0.2.3/v1/models >/dev/null 2>&1; then model_state=ok; else model_state=error; fi
        if command -v chromium >/dev/null 2>&1 || command -v chromium-browser >/dev/null 2>&1; then browser_state=ok; else browser_state=error; fi
        free_kb="$(df -Pk "$HOME" 2>/dev/null | awk 'NR==2 {print $4}')"
        if [ "${free_kb:-0}" -gt 1048576 ]; then disk_state=ok; else disk_state=space_low; fi
        state=ok
        message="Agent 正常"
        if [ "$disk_state" = space_low ]; then state=error; message="磁盘空间不足"; fi
        if [ "$service_state" = error ]; then state=error; message="Agent 服务未运行"; fi
        if [ "$port_state" = error ]; then state=error; message="Agent 网关不可用"; fi
        if [ "$model_state" = error ]; then state=error; message="模型代理不可用"; fi
        if [ "$browser_state" = error ]; then state=error; message="浏览器不可用"; fi
        printf '{"agent_type":"%s","state":"%s","message":"%s","checks":{"agent_service":"%s","gateway_port":"%s","llm_proxy":"%s","browser":"%s","disk":"%s"}}\\n' "$agent" "$state" "$message" "$service_state" "$port_state" "$model_state" "$browser_state" "$disk_state"
        """
    }

    private static func restartCommand(agent: AgentKind) -> String {
        let waitForGateway: String
        switch agent {
        case .hermes:
            waitForGateway = ""
        case .openclaw:
            waitForGateway = """
            i=0
            while [ "$i" -lt 60 ]; do
              if nc -z 127.0.0.1 18789 >/dev/null 2>&1; then
                break
              fi
              i=$((i + 1))
              sleep 1
            done
            """
        }
        return """
        svc="$(\(serviceResolverCommand(agent: agent)))"
        [ -n "$svc" ] || { echo "Agent 服务未安装" >&2; exit 1; }
        XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" systemctl --user restart "$svc"
        \(waitForGateway)
        \(healthStatusCommand(agent: agent))
        """
    }

    private static func testModelCommand(agent: AgentKind) -> String {
        """
        set -eu
        if curl -fsS --max-time 5 http://10.0.2.3/v1/models >/dev/null 2>&1; then
          printf '{"agent_type":"%s","state":"ok","message":"模型代理可用"}\\n' \(shellQuote(agent.rawValue))
        else
          printf '{"agent_type":"%s","state":"error","message":"模型代理不可用"}\\n' \(shellQuote(agent.rawValue))
          exit 1
        fi
        """
    }

    private static func resetConfigCommand(agent: AgentKind) -> String {
        switch agent {
        case .hermes:
            return """
            set -eu
            home="${HOME:-/home/tenbox}"
            hermes_cmd="$(\(hermesCommandResolver()))"
            if [ -n "$hermes_cmd" ]; then
              "$hermes_cmd" config set model.default default >/dev/null
              "$hermes_cmd" config set model.provider custom >/dev/null
              "$hermes_cmd" config set model.base_url http://10.0.2.3/v1 >/dev/null
              "$hermes_cmd" config set terminal.backend local >/dev/null
            else
              mkdir -p "$home/.hermes"
              cfg="$home/.hermes/config.yaml"
              env_file="$home/.hermes/.env"
              if command -v python3 >/dev/null 2>&1; then
                python3 - "$cfg" "$env_file" <<'PY'
            from pathlib import Path
            import sys

            cfg = Path(sys.argv[1])
            env = Path(sys.argv[2])
            lines = cfg.read_text(encoding="utf-8").splitlines() if cfg.exists() else []
            out = []
            i = 0
            while i < len(lines):
                line = lines[i]
                if line.startswith("model:") or line.startswith("terminal:"):
                    i += 1
                    while i < len(lines) and (lines[i].startswith(" ") or not lines[i].strip()):
                        i += 1
                    continue
                out.append(line)
                i += 1
            block = [
                'model:',
                '  default: "default"',
                '  provider: "custom"',
                '  base_url: "http://10.0.2.3/v1"',
                '',
                'terminal:',
                '  backend: local',
                ''
            ]
            cfg.write_text("\\n".join(block + [line for line in out if line.strip()]) + "\\n", encoding="utf-8")

            env_lines = env.read_text(encoding="utf-8").splitlines() if env.exists() else []
            values = {
                "OPENAI_BASE_URL": "http://10.0.2.3/v1",
                "OPENAI_API_KEY": "tenbox",
                "AGENT_BROWSER_HEADED": "true",
                "AGENT_BROWSER_EXECUTABLE_PATH": "/usr/bin/chromium",
            }
            seen = set()
            patched = []
            for line in env_lines:
                key = line.split("=", 1)[0] if "=" in line and not line.startswith("#") else None
                if key in values:
                    patched.append(f"{key}={values[key]}")
                    seen.add(key)
                else:
                    patched.append(line)
            for key, value in values.items():
                if key not in seen:
                    patched.append(f"{key}={value}")
            env.write_text("\\n".join(patched) + "\\n", encoding="utf-8")
            PY
              else
                cat > "$cfg" <<'EOF'
            model:
              default: "default"
              provider: "custom"
              base_url: "http://10.0.2.3/v1"

            terminal:
              backend: local
            EOF
                {
                  echo "OPENAI_BASE_URL=http://10.0.2.3/v1"
                  echo "OPENAI_API_KEY=tenbox"
                  echo "AGENT_BROWSER_HEADED=true"
                  echo "AGENT_BROWSER_EXECUTABLE_PATH=/usr/bin/chromium"
                } >> "$env_file"
              fi
            fi
            svc="$(\(serviceResolverCommand(agent: agent)))"
            if [ -n "$svc" ]; then
              XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" systemctl --user restart "$svc" >/dev/null 2>&1 || true
            fi
            \(healthStatusCommand(agent: agent))
            """
        case .openclaw:
            return """
            set -eu
            openclaw_cmd="$(\(openClawCommandResolver()))"
            [ -n "$openclaw_cmd" ] || { echo "缺少 OpenClaw 命令" >&2; exit 1; }
            tenbox_provider='{"baseUrl":"http://10.0.2.3/v1","apiKey":"tenbox","api":"openai-completions","models":[{"id":"default","name":"Default (TenBox Proxy)","reasoning":false,"input":["text","image"],"contextWindow":200000,"maxTokens":65536,"cost":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0}}]}'
            "$openclaw_cmd" config set models.providers.tenbox "$tenbox_provider" --strict-json --merge >/dev/null 2>&1 || "$openclaw_cmd" config set models.providers.tenbox "$tenbox_provider" >/dev/null
            "$openclaw_cmd" config set models.mode merge >/dev/null
            "$openclaw_cmd" config set agents.defaults.model.primary tenbox/default >/dev/null
            "$openclaw_cmd" config set agents.defaults.compaction.mode safeguard >/dev/null
            "$openclaw_cmd" config set agents.defaults.workspace "$HOME/.openclaw/workspace" >/dev/null
            "$openclaw_cmd" config set agents.defaults.models.tenbox/default '{"alias":"TenBox Proxy"}' --strict-json --merge >/dev/null 2>&1 || "$openclaw_cmd" config set agents.defaults.models.tenbox/default '{"alias":"TenBox Proxy"}' >/dev/null
            \(healthStatusCommand(agent: agent))
            """
        }
    }

    private static func diagnosticsCommand(agent: AgentKind, outputDir: String) -> String {
        return """
        set -eu
        out=\(shellQuote(outputDir))/tenbox-agent-diagnostics-\(agent.rawValue)-$(date -u +%Y%m%d%H%M%S).tar.gz
        tmp=\(shellQuote(outputDir))/.tenbox-diagnostics-work
        rm -rf "$tmp"
        mkdir -p "$tmp"
        \(healthStatusCommand(agent: agent)) > "$tmp/health.json" 2>&1 || true
        svc="$(\(serviceResolverCommand(agent: agent)))"
        if [ -n "$svc" ]; then
          XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" systemctl --user status "$svc" --no-pager > "$tmp/service.txt" 2>&1 || true
          journalctl --user -u "$svc" -n 200 --no-pager > "$tmp/journal.txt" 2>&1 || true
        else
          echo "Agent 服务未安装" > "$tmp/service.txt"
          echo "Agent 服务未安装" > "$tmp/journal.txt"
        fi
        df -h > "$tmp/disk.txt" 2>&1 || true
        sed -Ei 's/(sk-[A-Za-z0-9_-]{8})[A-Za-z0-9_-]+/\\1***/g; s/(authorization:[[:space:]]*bearer[[:space:]]+)[^[:space:]]+/\\1***/Ig; s/((api[_-]?key|token|secret|password)[=: ]+)[^ ]+/\\1***/Ig' "$tmp"/*.txt "$tmp"/*.json 2>/dev/null || true
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

    private static func openClawMigrationSourceExportCommand(outputPath: String) -> String {
        let outDir = (outputPath as NSString).deletingLastPathComponent
        let workDir = "\(outDir)/.tenbox-openclaw-migrate-source"
        let excludes = agentExcludeArgs(.openclaw, scope: .migration)
        return """
        set -eu
        home="${HOME:-/home/tenbox}"
        src="$home/.openclaw"
        out=\(shellQuote(outputPath))
        work=\(shellQuote(workDir))
        [ -d "$src" ] || { echo "OpenClaw 数据尚未初始化：$src" >&2; exit 1; }
        rm -rf "$work" "$out"
        mkdir -p "$work"
        tar_status=0
        (cd "$home" && tar --warning=no-file-changed --ignore-failed-read \(excludes) -czf "$out" ".openclaw") || tar_status=$?
        [ "$tar_status" -le 1 ] || exit "$tar_status"
        rm -rf "$work"
        echo "$out"
        """
    }

    private static func openClawToHermesDryRunCommand(inputPath: String,
                                                      reportPath: String,
                                                      options: AgentMigrationOptions) -> String {
        let flags = openClawMigrationFlags(options: options, includeYes: false)
        return """
        set -eu
        hermes_cmd="$(\(hermesCommandResolver()))"
        [ -n "$hermes_cmd" ] || { echo "目标 VM 缺少 Hermes 命令" >&2; exit 1; }
        input=\(shellQuote(inputPath))
        report=\(shellQuote(reportPath))
        tmp_parent="${HOME:-/home/tenbox}/.tenbox-tmp"
        mkdir -p "$tmp_parent"
        chmod 700 "$tmp_parent" 2>/dev/null || true
        work="$(mktemp -d "$tmp_parent/openclaw-to-hermes.XXXXXX")"
        trap 'rm -rf "$work"' EXIT
        source_dir="$work/source"
        [ -f "$input" ] || { echo "找不到 OpenClaw 迁移包：$input" >&2; exit 1; }
        mkdir -p "$source_dir"
        tar --touch --no-same-owner -xzf "$input" -C "$source_dir"
        [ -d "$source_dir/.openclaw" ] || { echo "迁移包缺少 .openclaw 目录" >&2; exit 1; }
        dry_log="$work/dry-run.txt"
        dry_status=0
        "$hermes_cmd" claw migrate --dry-run --source "$source_dir/.openclaw" \(flags) > "$dry_log" 2>&1 || dry_status=$?
        {
          echo "===== OpenClaw -> Hermes dry-run $(date -u +%Y-%m-%dT%H:%M:%SZ) ====="
          cat "$dry_log"
          echo
        } >> "$report"
        \(limitedLogCommand(logVariable: "dry_log"))
        [ "$dry_status" -eq 0 ] || exit "$dry_status"
        """
    }

    private static func openClawToHermesMigrationCommand(inputPath: String,
                                                        reportPath: String,
                                                        options: AgentMigrationOptions) -> String {
        let flags = openClawMigrationFlags(options: options, includeYes: true)
        return """
        set -eu
        hermes_cmd="$(\(hermesCommandResolver()))"
        [ -n "$hermes_cmd" ] || { echo "目标 VM 缺少 Hermes 命令" >&2; exit 1; }
        input=\(shellQuote(inputPath))
        report=\(shellQuote(reportPath))
        tmp_parent="${HOME:-/home/tenbox}/.tenbox-tmp"
        mkdir -p "$tmp_parent"
        chmod 700 "$tmp_parent" 2>/dev/null || true
        work="$(mktemp -d "$tmp_parent/openclaw-to-hermes.XXXXXX")"
        trap 'rm -rf "$work"' EXIT
        source_dir="$work/source"
        [ -f "$input" ] || { echo "找不到 OpenClaw 迁移包：$input" >&2; exit 1; }
        mkdir -p "$source_dir"
        tar --touch --no-same-owner -xzf "$input" -C "$source_dir"
        [ -d "$source_dir/.openclaw" ] || { echo "迁移包缺少 .openclaw 目录" >&2; exit 1; }
        migrate_log="$work/migrate.txt"
        migrate_status=0
        "$hermes_cmd" claw migrate --source "$source_dir/.openclaw" \(flags) > "$migrate_log" 2>&1 || migrate_status=$?
        if grep -q "Refusing to apply" "$migrate_log"; then
          migrate_status=1
        fi
        {
          echo "===== OpenClaw -> Hermes migrate $(date -u +%Y-%m-%dT%H:%M:%SZ) ====="
          cat "$migrate_log"
          echo
        } >> "$report"
        \(limitedLogCommand(logVariable: "migrate_log"))
        [ "$migrate_status" -eq 0 ] || exit "$migrate_status"
        \(hermesOpenClawChannelConfigCommand())
        \(hermesTenBoxModelConfigCommand())
        svc="$(\(serviceResolverCommand(agent: .hermes)))"
        if [ -n "$svc" ]; then
          XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" systemctl --user restart "$svc" >/dev/null 2>&1 || true
          echo "重启服务：$svc" >> "$report"
        fi
        health_log="$(mktemp)"
        (
        \(healthStatusCommand(agent: .hermes))
        ) > "$health_log" 2>&1 || true
        cat "$health_log"
        {
          echo "===== Hermes health ====="
          cat "$health_log"
          echo
        } >> "$report"
        rm -f "$health_log"
        """
    }

    private static func hermesOpenClawChannelConfigCommand() -> String {
        """
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
            env_file.write_text("\\n".join(patched) + "\\n", encoding="utf-8")

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
        """
    }

    private static func hermesTenBoxModelConfigCommand() -> String {
        """
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
              printf '%s=%s\\n' "$key" "$value" >> "$env_file"
            fi
          }
          set_env_value OPENAI_BASE_URL http://10.0.2.3/v1
          set_env_value OPENAI_API_KEY tenbox
          echo "已恢复 TenBox 模型代理配置" >> "$report"
        fi
        """
    }

    private static func openClawMigrationFlags(options: AgentMigrationOptions, includeYes: Bool) -> String {
        var flags = [
            "--preset", "full",
            "--migrate-secrets",
            "--overwrite",
            "--skill-conflict", options.skillConflictStrategy.rawValue
        ].map(shellQuote).joined(separator: " ")

        let workspaceTarget = options.workspaceTarget.trimmingCharacters(in: .whitespacesAndNewlines)
        if !workspaceTarget.isEmpty {
            flags += " --workspace-target \(shellQuote(workspaceTarget))"
        }
        if includeYes {
            flags += " --yes"
        }
        return flags
    }

    private static func limitedLogCommand(logVariable: String) -> String {
        """
        line_count="$(wc -l < "$\(logVariable)" | tr -d ' ')"
        if [ "${line_count:-0}" -gt 160 ]; then
          sed -n '1,80p' "$\(logVariable)"
          echo "... 输出已截断，完整内容见迁移报告 ..."
          tail -n 80 "$\(logVariable)"
        else
          cat "$\(logVariable)"
        fi
        """
    }

    private static func compactMigrationOutput(_ output: String) -> String? {
        let lines = output
            .split(whereSeparator: { $0.isNewline })
            .map { String($0).trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
        guard !lines.isEmpty else { return nil }
        return lines.prefix(8).joined(separator: "\n")
    }

    private static func agentExcludeArgs(_ agent: AgentKind, scope: AgentProfileExportScope) -> String {
        switch agent {
        case .hermes:
            let excludes = [
                "--exclude", ".hermes/logs",
                "--exclude", ".hermes/cache",
                "--exclude", ".hermes/image_cache",
                "--exclude", ".hermes/audio_cache",
                "--exclude", ".hermes/hermes-agent",
                "--exclude", ".hermes/bin",
                "--exclude", ".hermes/gateway.pid",
                "--exclude", ".hermes/gateway.lock",
            ]
            return excludes.map(shellQuote).joined(separator: " ")
        case .openclaw:
            let excludes = [
                "--exclude", ".openclaw/cache",
                "--exclude", ".openclaw/.cache",
                "--exclude", ".openclaw/workspace/.cache",
                "--exclude", ".openclaw/logs",
                "--exclude", ".openclaw/backup",
                "--exclude", ".openclaw/openclaw-backup*.tar.gz",
            ]
            return excludes.map(shellQuote).joined(separator: " ")
        }
    }

    private static func serviceName(_ agent: AgentKind) -> String {
        switch agent {
        case .hermes: return "hermes-gateway.service"
        case .openclaw: return "openclaw-gateway.service"
        }
    }

    private static func serviceResolverCommand(agent: AgentKind) -> String {
        let pattern: String
        switch agent {
        case .hermes:
            pattern = "hermes-gateway*.service"
        case .openclaw:
            pattern = "openclaw-gateway*.service"
        }
        let preferred = serviceName(agent)
        return """
        { preferred=\(shellQuote(preferred)); pattern=\(shellQuote(pattern)); if XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" systemctl --user status "$preferred" >/dev/null 2>&1; then printf '%s' "$preferred"; else XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" systemctl --user list-units --all "$pattern" --no-legend --plain 2>/dev/null | awk 'NR==1 {if ($1=="●") print $2; else print $1; exit}'; fi; }
        """
    }

    private static func hermesCommandResolver() -> String {
        """
        { hermes_cmd="$(command -v hermes 2>/dev/null || true)"; if [ -z "$hermes_cmd" ]; then svc="$(\(serviceResolverCommand(agent: .hermes)))"; if [ -n "$svc" ]; then exec_path="$(XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" systemctl --user show "$svc" -p ExecStart --value 2>/dev/null | sed -n 's/.*path=\\([^ ;]*\\).*/\\1/p' | head -n 1)"; if [ -n "$exec_path" ]; then exec_dir="$(dirname "$exec_path")"; for candidate in "$exec_dir/hermes" "$exec_dir/../bin/hermes" "$exec_dir/../../bin/hermes"; do [ -x "$candidate" ] && hermes_cmd="$candidate" && break; done; fi; fi; fi; if [ -z "$hermes_cmd" ]; then for candidate in "$HOME/.hermes/hermes-agent/.venv/bin/hermes" "$HOME/.hermes/hermes-agent/venv/bin/hermes" "$HOME/.local/bin/hermes"; do [ -x "$candidate" ] && hermes_cmd="$candidate" && break; done; fi; printf '%s' "$hermes_cmd"; }
        """
    }

    private static func openClawCommandResolver() -> String {
        """
        { openclaw_cmd="$(command -v openclaw 2>/dev/null || true)"; if [ -z "$openclaw_cmd" ]; then for candidate in "$HOME/.npm-global/bin/openclaw" "$HOME/.local/bin/openclaw"; do [ -x "$candidate" ] && openclaw_cmd="$candidate" && break; done; fi; printf '%s' "$openclaw_cmd"; }
        """
    }

    private static func shellQuote(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    private static func makeError(_ message: String) -> Error {
        ConsoleCommandError(message)
    }
}
