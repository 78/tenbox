import SwiftUI
import AppKit
import Combine
import Sparkle

let kTenBoxVersion: String = {
    Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "unknown"
}()
let kTenBoxCopyright = "Copyright \u{00A9} 2026 terrence@tenclass.com"

final class CheckForUpdatesViewModel: ObservableObject {
    @Published var canCheckForUpdates = false

    init(updater: SPUUpdater) {
        updater.publisher(for: \.canCheckForUpdates)
            .assign(to: &$canCheckForUpdates)
    }
}

struct CheckForUpdatesView: View {
    @ObservedObject private var viewModel: CheckForUpdatesViewModel
    private let updater: SPUUpdater

    init(updater: SPUUpdater) {
        self.updater = updater
        self.viewModel = CheckForUpdatesViewModel(updater: updater)
    }

    var body: some View {
        Button("Check for Updates...", action: updater.checkForUpdates)
            .disabled(!viewModel.canCheckForUpdates)
    }
}

@main
struct TenBoxApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    private let updaterController: SPUStandardUpdaterController

    init() {
        updaterController = SPUStandardUpdaterController(
            startingUpdater: true,
            updaterDelegate: nil,
            userDriverDelegate: nil
        )
        NSApplication.shared.setActivationPolicy(.regular)
        NSApplication.shared.activate(ignoringOtherApps: true)
        NSApplication.shared.applicationIconImage = Self.makeAppIcon()
    }

    private static func makeAppIcon() -> NSImage? {
        if let image = NSImage(named: "AppIcon") {
            image.size = NSSize(width: 256, height: 256)
            return image
        }
        if let url = Bundle.main.url(forResource: "AppIcon", withExtension: "icns"),
           let image = NSImage(contentsOf: url) {
            image.size = NSSize(width: 256, height: 256)
            return image
        }
        // SwiftPM places .copy resources in Bundle.module
        if let url = Bundle.module.url(forResource: "icon", withExtension: "png"),
           let image = NSImage(contentsOf: url) {
            image.size = NSSize(width: 256, height: 256)
            return image
        }
        return nil
    }

    private static func showAboutPanel() {
        let options: [NSApplication.AboutPanelOptionKey: Any] = [
            .applicationName: "TenBox 本地龙虾",
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
                Divider()
                CheckForUpdatesView(updater: updaterController.updater)
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
    @Published var showKeyboardCapturePermissionAlert = false
    @Published var startVmError: String?

    private var bridge = TenBoxBridgeWrapper()
    private var activeSessions: [String: VmSession] = [:]
    private var sessionCancellables: [String: AnyCancellable] = [:]
    private var stateObserver: NSObjectProtocol?
    private var pendingVmStartId: String?

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
                if newState == .rebooting {
                    self.removeSession(for: vmId)
                } else if newState == .stopped || newState == .crashed {
                    // Disconnect but keep session so console output stays visible
                    self.activeSessions[vmId]?.disconnect()
                } else if newState == .running {
                    let session = self.getOrCreateSession(for: vmId)
                    // Clear stale console output from the previous run before reconnecting
                    session.consoleText = ""
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
        if let vm = vms.first(where: { $0.id == vmId }) {
            session.displayScale = vm.displayScale
        }
        sessionCancellables[vmId] = session.objectWillChange
            .receive(on: RunLoop.main)
            .sink { [weak self] _ in self?.objectWillChange.send() }
        activeSessions[vmId] = session
        return session
    }

    func activeTabBinding(for vmId: String) -> Binding<Int> {
        let session = getOrCreateSession(for: vmId)
        return Binding(
            get: { session.activeTab },
            set: { session.activeTab = $0 }
        )
    }

    func removeSession(for vmId: String) {
        if let session = activeSessions[vmId] {
            session.disconnect()
        }
        sessionCancellables.removeValue(forKey: vmId)
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

    func requestStartVm(id: String) {
        let permissions = KeyboardCaptureManager.currentPermissions()
        if permissions.accessibilityGranted {
            startVm(id: id)
            return
        }

        pendingVmStartId = id
        showKeyboardCapturePermissionAlert = true
    }

    func startVm(id: String) {
        let ok = bridge.startVm(id: id)
        refreshVmList()
        if ok {
            if let session = activeSessions[id] {
                session.connectIfNeeded()
            }
        } else {
            let vmName = vms.first(where: { $0.id == id })?.name ?? id
            startVmError = "Failed to start VM \"\(vmName)\". The runtime binary may be missing or the VM configuration is invalid."
        }
    }

    func startPendingVmWithoutPermissionPrompt() {
        showKeyboardCapturePermissionAlert = false
        guard let vmId = pendingVmStartId else { return }
        pendingVmStartId = nil
        startVm(id: vmId)
    }

    func requestKeyboardCapturePermissions() {
        showKeyboardCapturePermissionAlert = false
        pendingVmStartId = nil
        KeyboardCaptureManager.requestFullCapturePermissions()
    }

    func dismissKeyboardCapturePermissionPrompt() {
        showKeyboardCapturePermissionAlert = false
        pendingVmStartId = nil
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

    func setDisplayScale(_ scale: Int, forVm vmId: String) {
        let clamped = max(1, min(2, scale))
        _ = bridge.setDisplayScale(clamped, forVm: vmId)
        refreshVmList()
        if let session = activeSessions[vmId] {
            session.displayScale = clamped
            session.resendDisplaySize()
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
