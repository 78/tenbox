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
        .frame(width: 460, height: 300)
    }

    private func exportProfile() {
        guard let vm = vm else { return }
        let panel = NSSavePanel()
        panel.title = "Export Agent Data"
        panel.nameFieldStringValue = "\(vm.name)-\(selectedAgent.rawValue)-profile.tar.gz"
        panel.allowedContentTypes = []
        presentPanel(panel) { response in
            guard response == .OK, let url = panel.url else { return }
            runOperation {
                appState.exportAgentProfile(vmId: vm.id, agent: selectedAgent, destinationURL: url, completion: $0)
            }
        }
    }

    private func importProfile() {
        guard let vm = vm else { return }
        let panel = NSOpenPanel()
        panel.title = "Import Agent Data"
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        presentPanel(panel) { response in
            guard response == .OK, let url = panel.url else { return }
            runOperation {
                appState.importAgentProfile(vmId: vm.id, agent: selectedAgent, sourceURL: url, completion: $0)
            }
        }
    }

    private func presentPanel(_ panel: NSSavePanel, completion: @escaping (NSApplication.ModalResponse) -> Void) {
        if let window = NSApplication.shared.keyWindow {
            panel.beginSheetModal(for: window, completionHandler: completion)
        } else {
            panel.begin(completionHandler: completion)
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
                    resultText = output.message
                case .failure(let error):
                    errorText = error.localizedDescription
                }
            }
        }
    }
}
