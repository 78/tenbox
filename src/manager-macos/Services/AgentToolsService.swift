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
            let packageName = "tenbox-agent-profile.tar.zst"
            let guestDir = "/mnt/shared/\(share.tag)"
            let guestPackage = "\(guestDir)/\(packageName)"
            let command = "TENBOX_SHARED_DIR=\(Self.shellQuote(guestDir)) tenbox-agent-profile export --agent \(agent.rawValue) --output \(Self.shellQuote(guestPackage))"

            session.runShellCommand(command, timeout: 300) { result in
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
            let packageName = "tenbox-agent-profile-import.tar.zst"
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

            let guestDir = "/mnt/shared/\(share.tag)"
            let guestPackage = "\(guestDir)/\(packageName)"
            let command = "TENBOX_SHARED_DIR=\(Self.shellQuote(guestDir)) tenbox-agent-profile import --agent \(agent.rawValue) --input \(Self.shellQuote(guestPackage))"

            session.runShellCommand(command, timeout: 300) { result in
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
        runBackupCommand(vm: vm, session: session, appState: appState, agent: agent,
                         command: "status", successMessage: "备份状态已更新",
                         completion: completion)
    }

    func snapshotBackup(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                        completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        runBackupCommand(vm: vm, session: session, appState: appState, agent: agent,
                         command: "snapshot", successMessage: "已创建 Agent 数据备份",
                         completion: completion)
    }

    func restoreLatestBackup(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                             completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        runBackupCommand(vm: vm, session: session, appState: appState, agent: agent,
                         command: "restore", successMessage: "已从最近备份恢复 Agent 数据",
                         completion: completion)
    }

    private func runBackupCommand(vm: VmInfo, session: VmSession, appState: AppState, agent: AgentKind,
                                  command: String, successMessage: String,
                                  completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        withBackupShare(vmId: vm.id, appState: appState) { share, cleanup in
            let guestDir = "/mnt/shared/\(share.tag)"
            let shellCommand = "TENBOX_SHARED_DIR=\(Self.shellQuote(guestDir)) TENBOX_VM_ID=\(Self.shellQuote(vm.id)) tenbox-agent-backup \(command) --agent \(agent.rawValue) --vm-id \(Self.shellQuote(vm.id))"
            session.runShellCommand(shellCommand, timeout: 300) { result in
                cleanup()
                switch result {
                case .success(let commandResult):
                    guard commandResult.exitCode == 0 else {
                        completion(.failure(Self.makeError(commandResult.output.isEmpty ? "Agent backup command failed" : commandResult.output)))
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
        } failure: { error in
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
            appState.addSharedFolder(share, toVm: vmId)

            let cleanup: () -> Void = { [weak appState, weak self] in
                DispatchQueue.main.async {
                    appState?.removeSharedFolder(tag: tag, fromVm: vmId)
                    try? self?.fileManager.removeItem(at: dir)
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

    private func withBackupShare(vmId: String, appState: AppState,
                                 perform: (SharedFolder, @escaping () -> Void) -> Void,
                                 failure: (Error) -> Void) {
        do {
            let dir = try backupDirectory(vmId: vmId)
            let tag = "tenbox-agent-backups-\(UUID().uuidString.prefix(8).lowercased())"
            let share = SharedFolder(tag: tag, hostPath: dir.path, readonly: false)
            appState.addSharedFolder(share, toVm: vmId)
            let cleanup: () -> Void = { [weak appState] in
                DispatchQueue.main.async {
                    appState?.removeSharedFolder(tag: tag, fromVm: vmId)
                }
            }
            perform(share, cleanup)
        } catch {
            failure(error)
        }
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

    private static func shellQuote(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    private static func makeError(_ message: String) -> Error {
        ConsoleCommandError(message)
    }
}
