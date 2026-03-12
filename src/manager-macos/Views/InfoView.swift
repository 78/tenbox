import SwiftUI

struct InfoView: View {
    let vm: VmInfo

    private var vmDirectory: String {
        (vm.diskPath as NSString).deletingLastPathComponent
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                GroupBox("General") {
                    VStack(alignment: .leading, spacing: 8) {
                        InfoRow(label: "Name", value: vm.name)
                        InfoRow(label: "State", value: vm.state.displayName)
                        InfoRow(label: "CPUs", value: "\(vm.cpuCount)")
                        InfoRow(label: "Memory", value: "\(vm.memoryMb) MB")
                        HStack(spacing: 16) {
                            Text("Directory")
                                .foregroundStyle(.secondary)
                                .frame(width: 70, alignment: .trailing)
                            HStack(spacing: 6) {
                                Text(vmDirectory)
                                    .lineLimit(1)
                                    .truncationMode(.middle)
                                    .frame(maxWidth: 360, alignment: .leading)
                                    .help(vmDirectory)
                                Button {
                                    NSWorkspace.shared.selectFile(nil, inFileViewerRootedAtPath: vmDirectory)
                                } label: {
                                    Image(systemName: "folder")
                                        .font(.caption)
                                }
                                .buttonStyle(.borderless)
                                .help("Open in Finder")
                            }
                        }
                    }
                    .padding(8)
                }
            }
            .padding()
        }
    }
}

private struct InfoRow: View {
    let label: String
    let value: String

    var body: some View {
        HStack(spacing: 16) {
            Text(label)
                .foregroundStyle(.secondary)
                .frame(width: 70, alignment: .trailing)
            Text(value)
        }
    }
}

struct AddSharedFolderSheet: View {
    let vmId: String
    @EnvironmentObject var appState: AppState
    @Environment(\.dismiss) private var dismiss

    @State private var tag = ""
    @State private var hostPath = ""
    @State private var readonly = false
    @State private var bookmarkData: Data?

    var body: some View {
        VStack(spacing: 0) {
            Text("Add Shared Folder")
                .font(.title3)
                .fontWeight(.semibold)
                .padding()

            Form {
                TextField("Tag", text: $tag)
                    .disableAutocorrection(true)
                HStack {
                    TextField("Host Path", text: $hostPath)
                    Button("Browse...") { browseFolder() }
                }
                Toggle("Read Only", isOn: $readonly)
            }
            .padding(.horizontal)

            HStack {
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Spacer()
                Button("Add") { addFolder() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(tag.isEmpty || hostPath.isEmpty)
            }
            .padding()
        }
        .frame(width: 420, height: 280)
    }

    private func browseFolder() {
        let panel = NSOpenPanel()
        panel.title = "Select Shared Folder"
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url {
            hostPath = url.path
            if tag.isEmpty {
                tag = url.lastPathComponent
            }
            bookmarkData = try? url.bookmarkData(
                options: .withSecurityScope,
                includingResourceValuesForKeys: nil,
                relativeTo: nil
            )
        }
    }

    private func addFolder() {
        let folder = SharedFolder(tag: tag, hostPath: hostPath, readonly: readonly, bookmark: bookmarkData)
        appState.addSharedFolder(folder, toVm: vmId)
        dismiss()
    }
}

struct AddPortForwardSheet: View {
    let vmId: String
    @EnvironmentObject var appState: AppState
    @Environment(\.dismiss) private var dismiss

    @State private var hostPortText = ""
    @State private var guestPortText = ""

    private var hostPort: UInt16? { UInt16(hostPortText) }
    private var guestPort: UInt16? { UInt16(guestPortText) }
    private var isValid: Bool {
        guard let hp = hostPort, let gp = guestPort else { return false }
        return hp >= 1 && gp >= 1
    }

    var body: some View {
        VStack(spacing: 0) {
            Text("Add Port Forward")
                .font(.title3)
                .fontWeight(.semibold)
                .padding()

            Form {
                TextField("Host Port", text: $hostPortText)
                    .disableAutocorrection(true)
                TextField("Guest Port", text: $guestPortText)
                    .disableAutocorrection(true)
            }
            .padding(.horizontal)

            HStack {
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Spacer()
                Button("Add") { addPortForward() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(!isValid)
            }
            .padding()
        }
        .frame(width: 340, height: 220)
    }

    private func addPortForward() {
        guard let hp = hostPort, let gp = guestPort else { return }
        let pf = PortForward(hostPort: hp, guestPort: gp)
        appState.addPortForward(pf, toVm: vmId)
        dismiss()
    }
}

struct SharedFoldersSheet: View {
    let vmId: String
    @EnvironmentObject var appState: AppState
    @Environment(\.dismiss) private var dismiss
    @State private var showAddSheet = false

    private var vm: VmInfo? {
        appState.vms.first(where: { $0.id == vmId })
    }

    var body: some View {
        VStack(spacing: 0) {
            Text("Shared Folders")
                .font(.title3)
                .fontWeight(.semibold)
                .padding()

            if let vm = vm {
                if vm.sharedFolders.isEmpty {
                    Text("No shared folders")
                        .foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else {
                    List {
                        ForEach(vm.sharedFolders) { folder in
                            HStack(spacing: 8) {
                                Image(systemName: "folder")
                                    .foregroundStyle(.secondary)
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(folder.tag)
                                        .fontWeight(.medium)
                                    Text(folder.hostPath)
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                        .lineLimit(1)
                                        .truncationMode(.middle)
                                }
                                Spacer()
                                if folder.readonly {
                                    Text("RO")
                                        .font(.caption2)
                                        .padding(.horizontal, 6)
                                        .padding(.vertical, 2)
                                        .background(.quaternary)
                                        .clipShape(RoundedRectangle(cornerRadius: 4))
                                }
                                Button(role: .destructive) {
                                    appState.removeSharedFolder(tag: folder.tag, fromVm: vmId)
                                } label: {
                                    Image(systemName: "minus.circle")
                                }
                                .buttonStyle(.borderless)
                            }
                        }
                    }
                }
            }

            HStack {
                Button("Done") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Spacer()
                Button {
                    showAddSheet = true
                } label: {
                    Label("Add", systemImage: "plus")
                }
                .keyboardShortcut(.defaultAction)
            }
            .padding()
        }
        .frame(width: 480, height: 400)
        .sheet(isPresented: $showAddSheet) {
            AddSharedFolderSheet(vmId: vmId)
        }
    }
}

struct PortForwardsSheet: View {
    let vmId: String
    @EnvironmentObject var appState: AppState
    @Environment(\.dismiss) private var dismiss
    @State private var showAddSheet = false

    private var vm: VmInfo? {
        appState.vms.first(where: { $0.id == vmId })
    }

    var body: some View {
        VStack(spacing: 0) {
            Text("Port Forwards")
                .font(.title3)
                .fontWeight(.semibold)
                .padding()

            if let vm = vm {
                if vm.portForwards.isEmpty {
                    Text("No port forwards")
                        .foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else {
                    List {
                        ForEach(vm.portForwards) { pf in
                            HStack(spacing: 8) {
                                Image(systemName: "network")
                                    .foregroundStyle(.secondary)
                                Text(verbatim: "127.0.0.1:\(pf.hostPort)")
                                    .fontWeight(.medium)
                                Image(systemName: "arrow.right")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                                Text(verbatim: "guest:\(pf.guestPort)")
                                    .foregroundStyle(.secondary)
                                Spacer()
                                Button(role: .destructive) {
                                    appState.removePortForward(hostPort: pf.hostPort, fromVm: vmId)
                                } label: {
                                    Image(systemName: "minus.circle")
                                }
                                .buttonStyle(.borderless)
                            }
                        }
                    }
                }
            }

            HStack {
                Button("Done") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Spacer()
                Button {
                    showAddSheet = true
                } label: {
                    Label("Add", systemImage: "plus")
                }
                .keyboardShortcut(.defaultAction)
            }
            .padding()
        }
        .frame(width: 420, height: 360)
        .sheet(isPresented: $showAddSheet) {
            AddPortForwardSheet(vmId: vmId)
        }
    }
}
