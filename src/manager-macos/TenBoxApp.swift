import SwiftUI
import AppKit
import Combine
import Sparkle
import IOKit.pwr_mgt

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
                .frame(minWidth: 1020, minHeight: 600)
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
                Divider()
                Button("LLM Proxy...") {
                    appDelegate.appState.showLlmProxySheet = true
                }
                .keyboardShortcut("l", modifiers: [.command, .shift])
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
    @Published var showLlmProxySheet = false
    @Published var startVmError: String?
    @Published var portForwardError: String?
    @Published var llmMappings: [LlmModelMapping] = []

    let llmProxy = LlmProxyService()
    private static let kLlmGuestIp = "10.0.2.3"
    private static let kLlmGuestPort: UInt16 = 80

    private var bridge = TenBoxBridgeWrapper()
    let clipboardHandler = ClipboardHandler()
    private var activeSessions: [String: VmSession] = [:]
    private var sessionCancellables: [String: AnyCancellable] = [:]
    private var stateObserver: NSObjectProtocol?
    private var pendingVmStartId: String?
    private var sleepAssertionID: IOPMAssertionID = IOPMAssertionID(0)

    init() {
        refreshVmList()
        loadLlmMappings()
        startLlmProxyIfNeeded()
        setupClipboard()
        stateObserver = NotificationCenter.default.addObserver(
            forName: NSNotification.Name("TenBoxVmStateChanged"),
            object: nil, queue: .main
        ) { [weak self] note in
            guard let self = self else { return }
            self.refreshVmList()
            self.updateSleepAssertion()
            if let vmId = note.object as? String {
                let newState = self.vms.first(where: { $0.id == vmId })?.state ?? .stopped
                if newState == .rebooting {
                    self.removeSession(for: vmId)
                } else if newState == .stopped || newState == .crashed {
                    self.activeSessions[vmId]?.disconnect()
                } else if newState == .running {
                    let session = self.getOrCreateSession(for: vmId)
                    session.consoleText = ""
                    session.connectIfNeeded()
                }
            }
        }
    }

    private func setupClipboard() {
        clipboardHandler.onHostClipboardChanged = { [weak self] data, mimeType in
            guard let self = self else { return }
            let dataType = VmSession.mimeToDataType(mimeType)
            guard dataType != 0 else { return }
            for session in self.activeSessions.values {
                guard session.connected else { continue }
                session.ipcClient.sendClipboardGrab(types: [dataType])
                session.ipcClient.sendClipboardData(dataType: dataType, payload: data)
            }
        }
        clipboardHandler.startMonitoring()
    }

    deinit {
        clipboardHandler.stopMonitoring()
        releaseSleepAssertion()
        if let obs = stateObserver {
            NotificationCenter.default.removeObserver(obs)
        }
    }

    // MARK: - Sleep prevention

    private func updateSleepAssertion() {
        let hasRunningVm = vms.contains { $0.state == .running || $0.state == .rebooting }
        if hasRunningVm {
            acquireSleepAssertion()
        } else {
            releaseSleepAssertion()
        }
    }

    private func acquireSleepAssertion() {
        guard sleepAssertionID == IOPMAssertionID(0) else { return }
        let reason = "TenBox VM is running" as CFString
        let ret = IOPMAssertionCreateWithName(
            kIOPMAssertPreventUserIdleSystemSleep as CFString,
            IOPMAssertionLevel(kIOPMAssertionLevelOn),
            reason,
            &sleepAssertionID
        )
        if ret != kIOReturnSuccess {
            sleepAssertionID = IOPMAssertionID(0)
        }
    }

    private func releaseSleepAssertion() {
        guard sleepAssertionID != IOPMAssertionID(0) else { return }
        IOPMAssertionRelease(sleepAssertionID)
        sleepAssertionID = IOPMAssertionID(0)
    }

    func getOrCreateSession(for vmId: String) -> VmSession {
        if let existing = activeSessions[vmId] {
            return existing
        }
        let session = VmSession(vmId: vmId, clipboardHandler: clipboardHandler)
        if let vm = vms.first(where: { $0.id == vmId }) {
            session.displayScale = vm.displayScale
        }
        session.onRuntimeRunning = { [weak self] in
            self?.sendNetworkUpdateIfRunning(vmId: vmId)
        }
        session.ipcClient.onPortForwardError = { [weak self] failedPorts in
            guard let self = self, !failedPorts.isEmpty else { return }
            let vm = self.vms.first(where: { $0.id == vmId })
            let mappings = failedPorts.map { hostPort -> String in
                if let hp = UInt16(hostPort),
                   let pf = vm?.portForwards.first(where: { $0.hostPort == hp }) {
                    return "\(hp) → \(pf.guestPort)"
                }
                return hostPort
            }
            let list = mappings.map { "  • \($0)" }.joined(separator: "\n")
            self.portForwardError = "The following port forward(s) failed to bind:\n\(list)\n\nThe host port(s) may already be in use."
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

    func editVm(id: String, name: String, memoryMb: Int, cpuCount: Int, netEnabled: Bool, debugMode: Bool) {
        bridge.editVm(id: id, name: name, memoryMb: memoryMb, cpuCount: cpuCount, netEnabled: netEnabled, debugMode: debugMode)
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
            let session = getOrCreateSession(for: id)
            session.consoleText = ""
            session.connectIfNeeded()
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
        sendNetworkUpdateIfRunning(vmId: vmId)
    }

    func removePortForward(hostPort: UInt16, fromVm vmId: String) {
        _ = bridge.removePortForward(hostPort: hostPort, fromVm: vmId)
        refreshVmList()
        sendNetworkUpdateIfRunning(vmId: vmId)
    }

    func addGuestForward(_ gf: GuestForward, toVm vmId: String) {
        _ = bridge.addGuestForward(gf, toVm: vmId)
        refreshVmList()
        sendNetworkUpdateIfRunning(vmId: vmId)
    }

    func removeGuestForward(guestIp: String, guestPort: UInt16, fromVm vmId: String) {
        _ = bridge.removeGuestForward(guestIp: guestIp, guestPort: guestPort, fromVm: vmId)
        refreshVmList()
        sendNetworkUpdateIfRunning(vmId: vmId)
    }

    // MARK: - LLM Proxy settings

    private var settingsPath: String {
        let paths = NSSearchPathForDirectoriesInDomains(.applicationSupportDirectory, .userDomainMask, true)
        let dir = (paths.first ?? NSHomeDirectory() + "/Library/Application Support") + "/TenBox"
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        return dir + "/settings.json"
    }

    func loadLlmMappings() {
        guard let data = FileManager.default.contents(atPath: settingsPath),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let llmProxy = json["llm_proxy"] as? [String: Any],
              let mappingsArray = llmProxy["mappings"] as? [[String: Any]] else {
            llmMappings = []
            return
        }
        llmMappings = mappingsArray.compactMap { item in
            guard let alias = item["alias"] as? String, !alias.isEmpty else { return nil }
            return LlmModelMapping(
                alias: alias,
                targetUrl: item["target_url"] as? String ?? "",
                apiKey: item["api_key"] as? String ?? "",
                model: item["model"] as? String ?? "",
                apiType: .openaiCompletions
            )
        }
    }

    private func saveLlmMappings() {
        var json: [String: Any] = [:]
        if let data = FileManager.default.contents(atPath: settingsPath),
           let existing = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            json = existing
        }
        let mappingsArray: [[String: Any]] = llmMappings.map { m in
            [
                "alias": m.alias,
                "target_url": m.targetUrl,
                "api_key": m.apiKey,
                "model": m.model,
                "api_type": "openai_completions",
            ]
        }
        json["llm_proxy"] = ["mappings": mappingsArray]
        if let data = try? JSONSerialization.data(withJSONObject: json, options: .prettyPrinted) {
            try? data.write(to: URL(fileURLWithPath: settingsPath))
        }
    }

    func addLlmMapping(_ mapping: LlmModelMapping) {
        guard !llmMappings.contains(where: { $0.alias == mapping.alias }) else { return }
        llmMappings.append(mapping)
        saveLlmMappings()
        syncLlmProxy()
    }

    func removeLlmMapping(alias: String) {
        llmMappings.removeAll { $0.alias == alias }
        saveLlmMappings()
        syncLlmProxy()
    }

    func updateLlmMapping(originalAlias: String, mapping: LlmModelMapping) {
        if let idx = llmMappings.firstIndex(where: { $0.alias == originalAlias }) {
            llmMappings[idx] = mapping
        }
        saveLlmMappings()
        syncLlmProxy()
    }

    private func startLlmProxyIfNeeded() {
        guard !llmMappings.isEmpty else { return }
        llmProxy.updateMappings(llmMappings)
        _ = llmProxy.start()
    }

    private func syncLlmProxy() {
        llmProxy.updateMappings(llmMappings)
        if llmMappings.isEmpty {
            llmProxy.stop()
        } else if llmProxy.listeningPort == 0 {
            _ = llmProxy.start()
        }
        for vm in vms where vm.state == .running {
            sendNetworkUpdateIfRunning(vmId: vm.id)
        }
    }

    private func sendSharedFoldersUpdateIfRunning(vmId: String) {
        guard let session = activeSessions[vmId], session.ipcClient.isConnected,
              let vm = vms.first(where: { $0.id == vmId }) else { return }
        let entries = vm.sharedFolders.map { f in
            "\(f.tag)|\(f.hostPath)|\(f.readonly ? "1" : "0")"
        }
        session.ipcClient.sendSharedFoldersUpdate(entries: entries)
    }

    func sendNetworkUpdateIfRunning(vmId: String) {
        guard let session = activeSessions[vmId], session.ipcClient.isConnected,
              let vm = vms.first(where: { $0.id == vmId }) else { return }
        let hostfwdEntries = vm.portForwards.map { pf in
            "tcp:\(pf.effectiveHostIp):\(pf.hostPort)-\(pf.effectiveGuestIp):\(pf.guestPort)"
        }
        var guestfwdEntries = vm.guestForwards.map { gf in
            "guestfwd:\(gf.guestIp):\(gf.guestPort)-\(gf.effectiveHostAddr):\(gf.hostPort)"
        }
        let proxyPort = llmProxy.listeningPort
        if proxyPort > 0 {
            guestfwdEntries.append(
                "guestfwd:\(Self.kLlmGuestIp):\(Self.kLlmGuestPort)-127.0.0.1:\(proxyPort)")
        }
        session.ipcClient.sendNetworkUpdate(
            hostfwdEntries: hostfwdEntries, guestfwdEntries: guestfwdEntries,
            netEnabled: vm.netEnabled)
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
        appState.llmProxy.stop()
        bridge.stopAllVms()
    }
}
