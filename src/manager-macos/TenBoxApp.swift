import SwiftUI
import AppKit

let kTenBoxVersion = "0.2.6"
let kTenBoxCopyright = "Copyright \u{00A9} 2026 terrence@tenclass.com"

@main
struct TenBoxApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

    init() {
        NSApplication.shared.setActivationPolicy(.regular)
        NSApplication.shared.activate(ignoringOtherApps: true)
        NSApplication.shared.applicationIconImage = Self.makeAppIcon()
    }

    private static func makeAppIcon() -> NSImage? {
        guard let url = Bundle.module.url(forResource: "icon", withExtension: "png"),
              let image = NSImage(contentsOf: url) else {
            return nil
        }
        image.size = NSSize(width: 256, height: 256)
        return image
    }

    private static func showAboutPanel() {
        let options: [NSApplication.AboutPanelOptionKey: Any] = [
            .applicationName: "TenBox",
            .applicationVersion: kTenBoxVersion,
            .version: "",
            .credits: NSAttributedString(
                string: "A lightweight virtual machine manager for macOS.\n\n\(kTenBoxCopyright)",
                attributes: [
                    .font: NSFont.systemFont(ofSize: 11),
                    .foregroundColor: NSColor.secondaryLabelColor,
                    .paragraphStyle: {
                        let ps = NSMutableParagraphStyle()
                        ps.alignment = .center
                        return ps
                    }()
                ]
            ),
        ]
        NSApplication.shared.orderFrontStandardAboutPanel(options: options)
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(appDelegate.appState)
                .frame(minWidth: 640, minHeight: 320)
        }
        .commands {
            CommandGroup(replacing: .appInfo) {
                Button("About TenBox") {
                    Self.showAboutPanel()
                }
            }
            CommandGroup(replacing: .newItem) {
                Button("New VM...") {
                    appDelegate.appState.showCreateVmDialog = true
                }
                .keyboardShortcut("n")
            }
            CommandGroup(replacing: .toolbar) { }
            CommandGroup(replacing: .sidebar) { }
        }
    }
}

class AppState: ObservableObject {
    @Published var vms: [VmInfo] = []
    @Published var selectedVmId: String?
    @Published var showCreateVmDialog = false
    @Published var showEditVmDialog = false

    private var bridge = TenBoxBridgeWrapper()
    private var activeSessions: [String: VmSession] = [:]
    private var stateObserver: NSObjectProtocol?

    init() {
        refreshVmList()
        stateObserver = NotificationCenter.default.addObserver(
            forName: NSNotification.Name("TenBoxVmStateChanged"),
            object: nil, queue: .main
        ) { [weak self] note in
            guard let self = self else { return }
            self.refreshVmList()
            if let vmId = note.object as? String {
                let newState = self.vms.first(where: { $0.id == vmId })?.state ?? .stopped
                if newState == .stopped || newState == .crashed || newState == .rebooting {
                    self.removeSession(for: vmId)
                } else if newState == .running {
                    let session = self.getOrCreateSession(for: vmId)
                    session.connectIfNeeded()
                }
            }
        }
    }

    deinit {
        if let obs = stateObserver {
            NotificationCenter.default.removeObserver(obs)
        }
    }

    func getOrCreateSession(for vmId: String) -> VmSession {
        if let existing = activeSessions[vmId] {
            return existing
        }
        let session = VmSession(vmId: vmId)
        activeSessions[vmId] = session
        return session
    }

    func removeSession(for vmId: String) {
        if let session = activeSessions[vmId] {
            session.disconnect()
        }
        activeSessions.removeValue(forKey: vmId)
    }

    func refreshVmList() {
        vms = bridge.getVmList()
    }

    func createVm(config: VmCreateConfig) {
        bridge.createVm(config: config)
        refreshVmList()
    }

    func editVm(id: String, name: String, memoryMb: Int, cpuCount: Int, netEnabled: Bool) {
        bridge.editVm(id: id, name: name, memoryMb: memoryMb, cpuCount: cpuCount, netEnabled: netEnabled)
        refreshVmList()
    }

    func deleteVm(id: String) {
        removeSession(for: id)
        bridge.deleteVm(id: id)
        refreshVmList()
    }

    func startVm(id: String) {
        bridge.startVm(id: id)
        refreshVmList()
        if let session = activeSessions[id] {
            session.connectIfNeeded()
        }
    }

    func stopVm(id: String) {
        if let session = activeSessions[id] {
            if session.ipcClient.isConnected {
                session.ipcClient.sendControl("stop")
            }
        }
        removeSession(for: id)
        bridge.stopVm(id: id)
        refreshVmList()
    }

    func rebootVm(id: String) {
        if let session = activeSessions[id], session.ipcClient.isConnected {
            session.ipcClient.sendControl("reboot")
        } else {
            bridge.rebootVm(id: id)
        }
    }

    func shutdownVm(id: String) {
        if let session = activeSessions[id], session.ipcClient.isConnected {
            session.ipcClient.sendControl("shutdown")
        } else {
            bridge.shutdownVm(id: id)
        }
    }

    func addSharedFolder(_ folder: SharedFolder, toVm vmId: String) {
        _ = bridge.addSharedFolder(folder, toVm: vmId)
        refreshVmList()
        sendSharedFoldersUpdateIfRunning(vmId: vmId)
    }

    func removeSharedFolder(tag: String, fromVm vmId: String) {
        _ = bridge.removeSharedFolder(tag: tag, fromVm: vmId)
        refreshVmList()
        sendSharedFoldersUpdateIfRunning(vmId: vmId)
    }

    func addPortForward(_ pf: PortForward, toVm vmId: String) {
        _ = bridge.addPortForward(pf, toVm: vmId)
        refreshVmList()
        sendPortForwardsUpdateIfRunning(vmId: vmId)
    }

    func removePortForward(hostPort: UInt16, fromVm vmId: String) {
        _ = bridge.removePortForward(hostPort: hostPort, fromVm: vmId)
        refreshVmList()
        sendPortForwardsUpdateIfRunning(vmId: vmId)
    }

    private func sendSharedFoldersUpdateIfRunning(vmId: String) {
        guard let session = activeSessions[vmId], session.ipcClient.isConnected,
              let vm = vms.first(where: { $0.id == vmId }) else { return }
        let entries = vm.sharedFolders.map { f in
            "\(f.tag)|\(f.hostPath)|\(f.readonly ? "1" : "0")"
        }
        session.ipcClient.sendSharedFoldersUpdate(entries: entries)
    }

    private func sendPortForwardsUpdateIfRunning(vmId: String) {
        guard let session = activeSessions[vmId], session.ipcClient.isConnected,
              let vm = vms.first(where: { $0.id == vmId }) else { return }
        let entries = vm.portForwards.map { pf in
            "\(pf.hostPort):\(pf.guestPort)"
        }
        session.ipcClient.sendPortForwardsUpdate(entries: entries, netEnabled: vm.netEnabled)
    }
}

class AppDelegate: NSObject, NSApplicationDelegate {
    let appState = AppState()
    private let bridge = TenBoxBridgeWrapper()

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSWindow.allowsAutomaticWindowTabbing = false
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        return false
    }

    func applicationWillTerminate(_ notification: Notification) {
        bridge.stopAllVms()
    }
}
