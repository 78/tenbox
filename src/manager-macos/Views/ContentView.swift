import SwiftUI

struct ContentView: View {
    @EnvironmentObject var appState: AppState

    private var selectedVm: VmInfo? {
        guard let vmId = appState.selectedVmId else { return nil }
        return appState.vms.first(where: { $0.id == vmId })
    }

    var body: some View {
        NavigationView {
            VmListView()
            detailView
        }
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                Button(action: { appState.showCreateVmDialog = true }) {
                    Label("New VM", systemImage: "plus.rectangle")
                }
                .help("Create a new virtual machine")
            }

            ToolbarItemGroup(placement: .primaryAction) {
                if let vm = selectedVm {
                    Button(action: { appState.showEditVmDialog = true }) {
                        Label("Edit", systemImage: "pencil")
                    }
                    .disabled(vm.state == .running)
                    .help("Edit VM settings")

                    Button(action: { appState.cloneVm(id: vm.id) }) {
                        Label("Clone", systemImage: "doc.on.doc")
                    }
                    .disabled(vm.state == .running)
                    .help("Clone this VM")

                    Button(role: .destructive, action: {
                        appState.showDeleteConfirm = true
                    }) {
                        Label("Delete", systemImage: "trash")
                    }
                    .disabled(vm.state == .running)
                    .help("Delete this VM")

                    Divider()
                }
            }

            ToolbarItemGroup(placement: .primaryAction) {
                if let vm = selectedVm {
                    if vm.state == .stopped || vm.state == .crashed {
                        Button(action: { appState.requestStartVm(id: vm.id) }) {
                            Label("Start", systemImage: "play.fill")
                        }
                        .help("Start VM")
                    }

                    if vm.state == .running {
                        Button(action: { appState.showForceStopConfirm = true }) {
                            Label("Force Stop", systemImage: "stop.fill")
                        }
                        .help("Force stop VM immediately")

                        Button(action: { appState.rebootVm(id: vm.id) }) {
                            Label("Reboot", systemImage: "arrow.clockwise")
                        }
                        .help("Reboot VM")

                        Button(action: { appState.shutdownVm(id: vm.id) }) {
                            Label("Shutdown", systemImage: "power")
                        }
                        .help("Gracefully shut down VM")

                        Divider()

                        Button(action: {
                            appState.setDisplayScale(vm.displayScale == 1 ? 2 : 1, forVm: vm.id)
                        }) {
                            Label(vm.displayScale == 2 ? "Display 1x" : "Display 2x",
                                  systemImage: vm.displayScale == 2 ? "minus.magnifyingglass" : "plus.magnifyingglass")
                        }
                        .help(vm.displayScale == 2 ? "Switch to 1x display scale" : "Switch to 2x Retina display scale")
                    }

                    Divider()

                    Button(action: { appState.showSharedFoldersSheet = true }) {
                        ToolbarBadgeLabel(
                            title: "Shared Folders",
                            systemImage: "folder",
                            count: vm.sharedFolders.count
                        )
                    }
                    .help("Manage shared folders")

                    Button(action: { appState.showPortForwardsSheet = true }) {
                        ToolbarBadgeLabel(
                            title: "Port Forwards",
                            systemImage: "network.badge.shield.half.filled",
                            count: vm.portForwards.count + vm.guestForwards.count
                        )
                    }
                    .help("Manage port forwards")

                    Button(action: { appState.showLlmProxySheet = true }) {
                        ToolbarBadgeLabel(
                            title: "LLM Proxy",
                            systemImage: "key.viewfinder",
                            count: appState.llmMappings.count
                        )
                    }
                    .help("Manage LLM proxy settings")

                    Picker("", selection: appState.activeTabBinding(for: vm.id)) {
                        Image(systemName: "info.circle").tag(0)
                        Image(systemName: "terminal").tag(1)
                        Image(systemName: "display").tag(2)
                    }
                    .pickerStyle(.segmented)
                    .labelsHidden()
                    .frame(maxWidth: 180)
                }
            }
        }
        .sheet(isPresented: $appState.showCreateVmDialog) {
            CreateVmDialog()
        }
        .sheet(isPresented: $appState.showEditVmDialog) {
            if let vm = selectedVm {
                EditVmDialog(vm: vm)
            }
        }
        .sheet(isPresented: $appState.showSharedFoldersSheet) {
            if let vm = selectedVm {
                SharedFoldersSheet(vmId: vm.id)
            }
        }
        .sheet(isPresented: $appState.showPortForwardsSheet) {
            if let vm = selectedVm {
                PortForwardsSheet(vmId: vm.id)
            }
        }
        .sheet(isPresented: $appState.showLlmProxySheet) {
            LlmProxySheet()
        }
        .alert("Delete VM", isPresented: $appState.showDeleteConfirm) {
            Button("Cancel", role: .cancel) {}
            Button("Delete", role: .destructive) {
                if let vm = selectedVm {
                    appState.deleteVm(id: vm.id)
                }
            }
        } message: {
            Text("Are you sure you want to delete \"\(selectedVm?.name ?? "")\"? This action cannot be undone.")
        }
        .alert("Force Stop VM", isPresented: $appState.showForceStopConfirm) {
            Button("Cancel", role: .cancel) {}
            Button("Force Stop", role: .destructive) {
                if let vm = selectedVm {
                    appState.stopVm(id: vm.id)
                }
            }
        } message: {
            Text("Are you sure you want to force stop \"\(selectedVm?.name ?? "")\"? Unsaved data may be lost.")
        }
        .alert("Enable Full Keyboard Capture?", isPresented: $appState.showKeyboardCapturePermissionAlert) {
            Button("Request Permissions") {
                appState.requestKeyboardCapturePermissions()
            }
            .keyboardShortcut(.defaultAction)
            Button("Start Without It") {
                appState.startPendingVmWithoutPermissionPrompt()
            }
            Button("Cancel", role: .cancel) {
                appState.dismissKeyboardCapturePermissionPrompt()
            }
        } message: {
            keyboardCapturePermissionMessage
        }
        .alert("Start VM Failed", isPresented: showStartVmErrorBinding) {
            Button("OK", role: .cancel) {
                appState.startVmError = nil
            }
        } message: {
            Text(appState.startVmError ?? "")
        }
        .onChange(of: appState.portForwardError) { error in
            guard let msg = error else { return }
            appState.portForwardError = nil
            let alert = NSAlert()
            alert.alertStyle = .warning
            alert.messageText = "Port Forward Error"
            alert.informativeText = msg
            alert.addButton(withTitle: "OK")
            alert.runModal()
        }
    }

    private var showStartVmErrorBinding: Binding<Bool> {
        Binding(
            get: { appState.startVmError != nil },
            set: { if !$0 { appState.startVmError = nil } }
        )
    }

    @ViewBuilder
    private var detailView: some View {
        if let vm = selectedVm {
            VmDetailView(vm: vm, appState: appState)
                .id(vm.id)
                .navigationTitle(vm.name)
        } else {
            Text("Select a VM")
                .font(.title2)
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .navigationTitle("")
        }
    }

    private var keyboardCapturePermissionMessage: Text {
        return Text("Full keyboard capture needs ") +
            Text("Accessibility").bold() +
            Text(" permission.\n\nYou can continue without full capture, or request permissions now.")
    }
}

private struct ToolbarBadgeLabel: View {
    let title: String
    let systemImage: String
    let count: Int

    var body: some View {
        Label {
            Text(title)
        } icon: {
            ZStack(alignment: .topTrailing) {
                Image(systemName: systemImage)
                if count > 0 {
                    Text("\(count)")
                        .font(.system(size: 8, weight: .bold))
                        .foregroundStyle(.white)
                        .frame(minWidth: 12, minHeight: 12)
                        .background(.secondary, in: Circle())
                        .offset(x: 4, y: -4)
                }
            }
        }
    }
}
