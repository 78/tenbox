import SwiftUI

struct InfoView: View {
    let vm: VmInfo
    @EnvironmentObject var appState: AppState
    @State private var showAddSheet = false
    @State private var showAddPortForwardSheet = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                GroupBox("General") {
                    Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 8) {
                        GridRow {
                            Text("Name").foregroundStyle(.secondary)
                            Text(vm.name)
                        }
                        GridRow {
                            Text("State").foregroundStyle(.secondary)
                            Text(vm.state.displayName)
                        }
                        GridRow {
                            Text("CPUs").foregroundStyle(.secondary)
                            Text("\(vm.cpuCount)")
                        }
                        GridRow {
                            Text("Memory").foregroundStyle(.secondary)
                            Text("\(vm.memoryMb) MB")
                        }
                    }
                    .padding(8)
                }

                GroupBox("Disk") {
                    Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 8) {
                        GridRow {
                            Text("Image").foregroundStyle(.secondary)
                            Text(vm.diskPath)
                                .lineLimit(1)
                                .truncationMode(.middle)
                        }
                        GridRow {
                            Text("Kernel").foregroundStyle(.secondary)
                            Text(vm.kernelPath)
                                .lineLimit(1)
                                .truncationMode(.middle)
                        }
                    }
                    .padding(8)
                }

                GroupBox {
                    VStack(alignment: .leading, spacing: 8) {
                        if vm.sharedFolders.isEmpty {
                            Text("No shared folders")
                                .foregroundStyle(.secondary)
                                .frame(maxWidth: .infinity, alignment: .center)
                                .padding(.vertical, 4)
                        } else {
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
                                        appState.removeSharedFolder(tag: folder.tag, fromVm: vm.id)
                                    } label: {
                                        Image(systemName: "minus.circle")
                                    }
                                    .buttonStyle(.borderless)
                                }
                                if folder.id != vm.sharedFolders.last?.id {
                                    Divider()
                                }
                            }
                        }
                    }
                    .padding(8)
                } label: {
                    HStack {
                        Text("Shared Folders")
                        Spacer()
                        Button {
                            showAddSheet = true
                        } label: {
                            Image(systemName: "plus")
                        }
                        .buttonStyle(.borderless)
                    }
                }
                GroupBox {
                    VStack(alignment: .leading, spacing: 8) {
                        if vm.portForwards.isEmpty {
                            Text("No port forwards")
                                .foregroundStyle(.secondary)
                                .frame(maxWidth: .infinity, alignment: .center)
                                .padding(.vertical, 4)
                        } else {
                            ForEach(vm.portForwards) { pf in
                                HStack(spacing: 8) {
                                    Image(systemName: "network")
                                        .foregroundStyle(.secondary)
                                    Text("127.0.0.1:\(pf.hostPort)")
                                        .fontWeight(.medium)
                                    Image(systemName: "arrow.right")
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                    Text("guest:\(pf.guestPort)")
                                        .foregroundStyle(.secondary)
                                    Spacer()
                                    Button(role: .destructive) {
                                        appState.removePortForward(hostPort: pf.hostPort, fromVm: vm.id)
                                    } label: {
                                        Image(systemName: "minus.circle")
                                    }
                                    .buttonStyle(.borderless)
                                }
                                if pf.id != vm.portForwards.last?.id {
                                    Divider()
                                }
                            }
                        }
                    }
                    .padding(8)
                } label: {
                    HStack {
                        Text("Port Forwards")
                        Spacer()
                        Button {
                            showAddPortForwardSheet = true
                        } label: {
                            Image(systemName: "plus")
                        }
                        .buttonStyle(.borderless)
                    }
                }
            }
            .padding()
        }
        .sheet(isPresented: $showAddSheet) {
            AddSharedFolderSheet(vmId: vm.id)
        }
        .sheet(isPresented: $showAddPortForwardSheet) {
            AddPortForwardSheet(vmId: vm.id)
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
            .formStyle(.grouped)
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
            .formStyle(.grouped)
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
