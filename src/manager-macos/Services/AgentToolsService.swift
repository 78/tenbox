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

    private static func shellQuote(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    private static func makeError(_ message: String) -> Error {
        ConsoleCommandError(message)
    }
}
