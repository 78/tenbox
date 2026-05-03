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

struct AddHostForwardSheet: View {
    let vmId: String
    var existing: HostForward? = nil
    @EnvironmentObject var appState: AppState
    @Environment(\.dismiss) private var dismiss
    @FocusState private var focusedField: HostForwardField?

    @State private var nameText = ""
    @State private var hostIpText = "127.0.0.1"
    @State private var hostPortText = ""
    @State private var guestIpText = "10.0.2.15"
    @State private var guestPortText = ""

    private enum HostForwardField { case name, hostIp, hostPort, guestIp, guestPort }

    private var hostPort: UInt16? { UInt16(hostPortText) }
    private var guestPort: UInt16? { UInt16(guestPortText) }
    private var isValid: Bool {
        guard let hp = hostPort, let gp = guestPort else { return false }
        return hp >= 1 && gp >= 1 && !hostIpText.isEmpty
    }
    private var isEditing: Bool { existing != nil }

    var body: some View {
        VStack(spacing: 0) {
            Text(isEditing ? "Edit Host → Guest" : "Host → Guest")
                .font(.title3)
                .fontWeight(.semibold)
                .padding()

            Form {
                TextField("Name (optional)", text: $nameText)
                    .disableAutocorrection(true)
                    .focused($focusedField, equals: .name)
                TextField("Host IP", text: $hostIpText)
                    .disableAutocorrection(true)
                    .focused($focusedField, equals: .hostIp)
                TextField("Host Port", text: $hostPortText)
                    .disableAutocorrection(true)
                    .focused($focusedField, equals: .hostPort)
                TextField("Guest IP", text: .constant(guestIpText))
                    .disableAutocorrection(true)
                    .disabled(true)
                    .foregroundStyle(.secondary)
                TextField("Guest Port", text: $guestPortText)
                    .disableAutocorrection(true)
                    .focused($focusedField, equals: .guestPort)
            }
            .padding(.horizontal)
            .onAppear {
                if let pf = existing {
                    nameText = pf.name
                    hostIpText = pf.effectiveHostIp
                    hostPortText = String(pf.hostPort)
                    guestPortText = String(pf.guestPort)
                }
                focusedField = .name
            }

            HStack {
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Spacer()
                Button(isEditing ? "Save" : "Add") { addHostForward() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(!isValid)
            }
            .padding()
        }
        .frame(width: 340, height: 300)
    }

    private func addHostForward() {
        guard let hp = hostPort, let gp = guestPort else { return }
        if let old = existing {
            appState.removeHostForward(hostPort: old.hostPort, fromVm: vmId)
        }
        let pf = HostForward(name: nameText, hostPort: hp, guestPort: gp, hostIp: hostIpText, guestIp: guestIpText)
        appState.addHostForward(pf, toVm: vmId)
        dismiss()
    }
}

struct AddGuestForwardSheet: View {
    let vmId: String
    var existing: GuestForward? = nil
    @EnvironmentObject var appState: AppState
    @Environment(\.dismiss) private var dismiss
    @FocusState private var focusedField: GuestForwardField?

    @State private var nameText = ""
    @State private var guestIpText = "10.0.2.2"
    @State private var guestPortText = ""
    @State private var hostAddrText = "127.0.0.1"
    @State private var hostPortText = ""

    private enum GuestForwardField { case name, guestIp, guestPort, hostAddr, hostPort }

    private var guestPort: UInt16? { UInt16(guestPortText) }
    private var hostPort: UInt16? { UInt16(hostPortText) }
    private var isValid: Bool {
        guard let gp = guestPort, let hp = hostPort else { return false }
        return gp >= 1 && hp >= 1 && !guestIpText.isEmpty && !hostAddrText.isEmpty
    }
    private var isEditing: Bool { existing != nil }

    var body: some View {
        VStack(spacing: 0) {
            Text(isEditing ? "Edit Guest → Host" : "Guest → Host")
                .font(.title3)
                .fontWeight(.semibold)
                .padding()

            Form {
                TextField("Name (optional)", text: $nameText)
                    .disableAutocorrection(true)
                    .focused($focusedField, equals: .name)
                TextField("Guest IP", text: $guestIpText)
                    .disableAutocorrection(true)
                    .focused($focusedField, equals: .guestIp)
                TextField("Guest Port", text: $guestPortText)
                    .disableAutocorrection(true)
                    .focused($focusedField, equals: .guestPort)
                TextField("Host Address", text: $hostAddrText)
                    .disableAutocorrection(true)
                    .focused($focusedField, equals: .hostAddr)
                TextField("Host Port", text: $hostPortText)
                    .disableAutocorrection(true)
                    .focused($focusedField, equals: .hostPort)
            }
            .padding(.horizontal)
            .onAppear {
                if let gf = existing {
                    nameText = gf.name
                    guestIpText = gf.guestIp
                    guestPortText = String(gf.guestPort)
                    hostAddrText = gf.effectiveHostAddr
                    hostPortText = String(gf.hostPort)
                }
                focusedField = .name
            }

            HStack {
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Spacer()
                Button(isEditing ? "Save" : "Add") { addGuestForward() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(!isValid)
            }
            .padding()
        }
        .frame(width: 340, height: 300)
    }

    private func addGuestForward() {
        guard let gp = guestPort, let hp = hostPort else { return }
        if let old = existing {
            appState.removeGuestForward(guestIp: old.guestIp, guestPort: old.guestPort, fromVm: vmId)
        }
        let gf = GuestForward(name: nameText, guestIp: guestIpText, guestPort: gp, hostAddr: hostAddrText, hostPort: hp)
        appState.addGuestForward(gf, toVm: vmId)
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

            Text("Shared folders appear as desktop shortcuts in the VM, making it easy to exchange files between host and guest.")
                .font(.caption)
                .foregroundStyle(.secondary)
                .padding(.horizontal)
                .padding(.bottom, 4)

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
    @State private var showAddPfSheet = false
    @State private var showAddGfSheet = false
    @State private var editingPf: HostForward? = nil
    @State private var editingGf: GuestForward? = nil

    private var vm: VmInfo? {
        appState.vms.first(where: { $0.id == vmId })
    }

    var body: some View {
        VStack(spacing: 0) {
            if let vm = vm {
                // Host -> Guest section
                HStack {
                    Text("Host \u{2192} Guest")
                        .font(.headline)
                    Spacer()
                    Button {
                        showAddPfSheet = true
                    } label: {
                        Image(systemName: "plus")
                    }
                    .buttonStyle(.borderless)
                }
                .padding(.horizontal)
                .padding(.top)

                if vm.hostForwards.isEmpty {
                    Text("No port forwards")
                        .foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else {
                    List {
                        ForEach(vm.hostForwards) { pf in
                            HStack(spacing: 8) {
                                Image(systemName: "network")
                                    .foregroundStyle(.secondary)
                                VStack(alignment: .leading, spacing: 2) {
                                    if !pf.name.isEmpty {
                                        Text(pf.name)
                                            .fontWeight(.medium)
                                    }
                                    let guestDisplay = pf.guestIp.isEmpty ? "10.0.2.15" : pf.guestIp
                                    Text(verbatim: "\(pf.effectiveHostIp):\(pf.hostPort) \u{2192} \(guestDisplay):\(pf.guestPort)")
                                        .foregroundStyle(pf.name.isEmpty ? .primary : .secondary)
                                        .font(pf.name.isEmpty ? .body : .caption)
                                }
                                Spacer()
                                Button {
                                    editingPf = pf
                                } label: {
                                    Image(systemName: "pencil")
                                        .font(.caption)
                                }
                                .buttonStyle(.borderless)
                                .help("Edit")
                                Button(role: .destructive) {
                                    appState.removeHostForward(hostPort: pf.hostPort, fromVm: vmId)
                                } label: {
                                    Image(systemName: "minus.circle")
                                }
                                .buttonStyle(.borderless)
                            }
                        }
                    }
                }

                Divider()

                // Guest -> Host section
                HStack {
                    Text("Guest \u{2192} Host")
                        .font(.headline)
                    Spacer()
                    Button {
                        showAddGfSheet = true
                    } label: {
                        Image(systemName: "plus")
                    }
                    .buttonStyle(.borderless)
                }
                .padding(.horizontal)
                .padding(.top, 8)

                if vm.guestForwards.isEmpty {
                    Text("No guest forwards")
                        .foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else {
                    List {
                        ForEach(vm.guestForwards) { gf in
                            HStack(spacing: 8) {
                                Image(systemName: "network")
                                    .foregroundStyle(.secondary)
                                VStack(alignment: .leading, spacing: 2) {
                                    if !gf.name.isEmpty {
                                        Text(gf.name)
                                            .fontWeight(.medium)
                                    }
                                    Text(verbatim: "\(gf.guestIp):\(gf.guestPort) \u{2192} \(gf.effectiveHostAddr):\(gf.hostPort)")
                                        .foregroundStyle(gf.name.isEmpty ? .primary : .secondary)
                                        .font(gf.name.isEmpty ? .body : .caption)
                                }
                                Spacer()
                                Button {
                                    editingGf = gf
                                } label: {
                                    Image(systemName: "pencil")
                                        .font(.caption)
                                }
                                .buttonStyle(.borderless)
                                .help("Edit")
                                Button(role: .destructive) {
                                    appState.removeGuestForward(guestIp: gf.guestIp, guestPort: gf.guestPort, fromVm: vmId)
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
            }
            .padding()
        }
        .frame(width: 460, height: 480)
        .sheet(isPresented: $showAddPfSheet) {
            AddHostForwardSheet(vmId: vmId)
        }
        .sheet(item: $editingPf) { pf in
            AddHostForwardSheet(vmId: vmId, existing: pf)
        }
        .sheet(isPresented: $showAddGfSheet) {
            AddGuestForwardSheet(vmId: vmId)
        }
        .sheet(item: $editingGf) { gf in
            AddGuestForwardSheet(vmId: vmId, existing: gf)
        }
    }
}
