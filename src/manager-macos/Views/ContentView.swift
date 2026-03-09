import SwiftUI

struct ContentView: View {
    @EnvironmentObject var appState: AppState
    @State private var columnVisibility = NavigationSplitViewVisibility.all
    @State private var showDeleteConfirm = false
    @State private var showForceStopConfirm = false

    var body: some View {
        NavigationSplitView(columnVisibility: $columnVisibility) {
            VmListView()
        } detail: {
            if let vmId = appState.selectedVmId,
               let vm = appState.vms.first(where: { $0.id == vmId }) {
                VmDetailView(vm: vm, appState: appState)
                    .id(vmId)
            } else {
                Text("Select a VM")
                    .font(.title2)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .onChange(of: columnVisibility, perform: { newValue in
            if newValue != .all {
                columnVisibility = .all
            }
        })
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                Button(action: { appState.showCreateVmDialog = true }) {
                    Label("New VM", systemImage: "plus.rectangle")
                }

                if let vmId = appState.selectedVmId,
                   let vm = appState.vms.first(where: { $0.id == vmId }) {
                    Button(action: { appState.showEditVmDialog = true }) {
                        Label("Edit", systemImage: "pencil")
                    }
                    .disabled(vm.state == .running)

                    Button(role: .destructive, action: {
                        showDeleteConfirm = true
                    }) {
                        Label("Delete", systemImage: "trash")
                    }
                    .disabled(vm.state == .running)

                    Divider()

                    if vm.state == .stopped || vm.state == .crashed {
                        Button(action: { appState.startVm(id: vmId) }) {
                            Label("Start", systemImage: "play.fill")
                        }
                    }

                    if vm.state == .running {
                        Button(action: { showForceStopConfirm = true }) {
                            Label("Force Stop", systemImage: "stop.fill")
                        }

                        Button(action: { appState.rebootVm(id: vmId) }) {
                            Label("Reboot", systemImage: "arrow.clockwise")
                        }

                        Button(action: { appState.shutdownVm(id: vmId) }) {
                            Label("Shutdown", systemImage: "power")
                        }

                        Divider()

                        Button(action: {
                            appState.setDisplayScale(vm.displayScale == 1 ? 2 : 1, forVm: vmId)
                        }) {
                            Label(vm.displayScale == 2 ? "Display 1x" : "Display 2x",
                                  systemImage: vm.displayScale == 2 ? "minus.magnifyingglass" : "plus.magnifyingglass")
                        }
                    }
                }
            }
        }
        .sheet(isPresented: $appState.showCreateVmDialog) {
            CreateVmDialog()
        }
        .sheet(isPresented: $appState.showEditVmDialog) {
            if let vmId = appState.selectedVmId,
               let vm = appState.vms.first(where: { $0.id == vmId }) {
                EditVmDialog(vm: vm)
            }
        }
        .alert("Delete VM", isPresented: $showDeleteConfirm) {
            Button("Cancel", role: .cancel) {}
            Button("Delete", role: .destructive) {
                if let vmId = appState.selectedVmId {
                    appState.deleteVm(id: vmId)
                }
            }
        } message: {
            if let vmId = appState.selectedVmId,
               let vm = appState.vms.first(where: { $0.id == vmId }) {
                Text("Are you sure you want to delete \"\(vm.name)\"? This action cannot be undone.")
            }
        }
        .alert("Force Stop VM", isPresented: $showForceStopConfirm) {
            Button("Cancel", role: .cancel) {}
            Button("Force Stop", role: .destructive) {
                if let vmId = appState.selectedVmId {
                    appState.stopVm(id: vmId)
                }
            }
        } message: {
            if let vmId = appState.selectedVmId,
               let vm = appState.vms.first(where: { $0.id == vmId }) {
                Text("Are you sure you want to force stop \"\(vm.name)\"? Unsaved data may be lost.")
            }
        }
    }
}
