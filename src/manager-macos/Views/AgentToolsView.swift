import SwiftUI
import AppKit

struct AgentToolsSheet: View {
    let vmId: String
    @EnvironmentObject var appState: AppState
    @Environment(\.dismiss) private var dismiss

    @State private var selectedAgent: AgentKind = .hermes
    @State private var isRunningOperation = false
    @State private var resultText = ""
    @State private var errorText = ""

    private var vm: VmInfo? {
        appState.vms.first(where: { $0.id == vmId })
    }

    private var canRun: Bool {
        vm?.state == .running && !isRunningOperation
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                Text("Agent Data")
                    .font(.title3)
                    .fontWeight(.semibold)
                Spacer()
                Button("Done") { dismiss() }
                    .keyboardShortcut(.cancelAction)
            }

            Picker("Agent", selection: $selectedAgent) {
                ForEach(AgentKind.allCases) { agent in
                    Text(agent.displayName).tag(agent)
                }
            }
            .pickerStyle(.segmented)

            HStack(spacing: 10) {
                Button {
                    exportProfile()
                } label: {
                    Label("Export", systemImage: "square.and.arrow.up")
                }
                .disabled(!canRun)

                Button {
                    importProfile()
                } label: {
                    Label("Import", systemImage: "square.and.arrow.down")
                }
                .disabled(!canRun)
            }

            Divider()

            Text("Backups")
                .font(.headline)

            HStack(spacing: 10) {
                Button {
                    showBackupStatus()
                } label: {
                    Label("Status", systemImage: "checklist")
                }
                .disabled(!canRun)

                Button {
                    snapshotBackup()
                } label: {
                    Label("Back Up Now", systemImage: "clock.arrow.circlepath")
                }
                .disabled(!canRun)

                Button {
                    restoreLatestBackup()
                } label: {
                    Label("Restore Latest", systemImage: "arrow.uturn.backward")
                }
                .disabled(!canRun)
            }

            Divider()

            Text("Health")
                .font(.headline)

            HStack(spacing: 10) {
                Button {
                    checkHealth()
                } label: {
                    Label("Check", systemImage: "stethoscope")
                }
                .disabled(!canRun)

                Button {
                    restartAgent()
                } label: {
                    Label("Restart", systemImage: "arrow.clockwise")
                }
                .disabled(!canRun)

                Button {
                    testModel()
                } label: {
                    Label("Test Model", systemImage: "bolt.horizontal")
                }
                .disabled(!canRun)
            }

            HStack(spacing: 10) {
                Button {
                    resetConfig()
                } label: {
                    Label("Reset Config", systemImage: "slider.horizontal.2.square")
                }
                .disabled(!canRun)

                Button {
                    exportDiagnostics()
                } label: {
                    Label("Diagnostics", systemImage: "doc.zipper")
                }
                .disabled(!canRun)
            }

            if isRunningOperation {
                ProgressView()
                    .controlSize(.small)
            }

            if let vm = vm, vm.state != .running {
                Text("Start the VM before using Agent data tools.")
                    .foregroundStyle(.secondary)
            }

            if !resultText.isEmpty {
                Text(resultText)
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
            }

            if !errorText.isEmpty {
                Text(errorText)
                    .foregroundStyle(.red)
                    .textSelection(.enabled)
            }

            Spacer(minLength: 0)
        }
        .padding()
        .frame(width: 560, height: 520)
    }

    private func exportProfile() {
        guard let vm = vm else { return }
        let panel = NSSavePanel()
        panel.title = "Export Agent Data"
        panel.nameFieldStringValue = "\(vm.name)-\(selectedAgent.rawValue)-profile.tar.gz"
        panel.allowedContentTypes = []
        guard panel.runModal() == .OK, let url = panel.url else { return }
        runOperation {
            appState.exportAgentProfile(vmId: vm.id, agent: selectedAgent, destinationURL: url, completion: $0)
        }
    }

    private func importProfile() {
        guard let vm = vm else { return }
        let panel = NSOpenPanel()
        panel.title = "Import Agent Data"
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        runOperation {
            appState.importAgentProfile(vmId: vm.id, agent: selectedAgent, sourceURL: url, completion: $0)
        }
    }

    private func showBackupStatus() {
        guard let vm = vm else { return }
        runOperation {
            appState.agentBackupStatus(vmId: vm.id, agent: selectedAgent, completion: $0)
        }
    }

    private func snapshotBackup() {
        guard let vm = vm else { return }
        runOperation {
            appState.snapshotAgentBackup(vmId: vm.id, agent: selectedAgent, completion: $0)
        }
    }

    private func restoreLatestBackup() {
        guard let vm = vm else { return }
        runOperation {
            appState.restoreLatestAgentBackup(vmId: vm.id, agent: selectedAgent, completion: $0)
        }
    }

    private func checkHealth() {
        guard let vm = vm else { return }
        runOperation {
            appState.agentHealthStatus(vmId: vm.id, agent: selectedAgent, completion: $0)
        }
    }

    private func restartAgent() {
        guard let vm = vm else { return }
        runOperation {
            appState.restartAgent(vmId: vm.id, agent: selectedAgent, completion: $0)
        }
    }

    private func testModel() {
        guard let vm = vm else { return }
        runOperation {
            appState.testAgentModel(vmId: vm.id, agent: selectedAgent, completion: $0)
        }
    }

    private func resetConfig() {
        guard let vm = vm else { return }
        runOperation {
            appState.resetAgentConfig(vmId: vm.id, agent: selectedAgent, completion: $0)
        }
    }

    private func exportDiagnostics() {
        guard let vm = vm else { return }
        runOperation {
            appState.exportAgentDiagnostics(vmId: vm.id, agent: selectedAgent, completion: $0)
        }
    }

    private func runOperation(_ operation: (@escaping (Result<AgentToolResult, Error>) -> Void) -> Void) {
        resultText = ""
        errorText = ""
        isRunningOperation = true
        operation { result in
            DispatchQueue.main.async {
                isRunningOperation = false
                switch result {
                case .success(let output):
                    resultText = output.output.isEmpty ? output.message : "\(output.message)\n\(output.output)"
                case .failure(let error):
                    errorText = error.localizedDescription
                }
            }
        }
    }
}
