import SwiftUI
import AppKit
import Combine
import Sparkle
import IOKit.pwr_mgt

let kTenBoxVersion: String = {
    Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "unknown"
}()
let kTenBoxCopyright = "Copyright \u{00A9} 2026 terrence@tenclass.com"
let kTenBoxWebsiteURL = URL(string: "https://tenbox.ai/")!

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
                .frame(minWidth: 800, idealWidth: 1020, minHeight: 480, idealHeight: 600)
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
            CommandMenu("VM") {
                VmCommandMenuContent(appState: appDelegate.appState)
            }
            CommandGroup(replacing: .toolbar) { }
            CommandGroup(replacing: .sidebar) { }
            CommandGroup(replacing: .help) {
                Button("TenBox Website...") {
                    NSWorkspace.shared.open(kTenBoxWebsiteURL)
                }
                Button("Help Documentation...") {
                    NSWorkspace.shared.open(URL(string: "https://my.feishu.cn/wiki/Q96KwUH1Di3cAik2W7kcQsWKncb")!)
                }
            }
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
    @Published var showDeleteConfirm = false
    @Published var showForceStopConfirm = false
    @Published var showSharedFoldersSheet = false
    @Published var showPortForwardsSheet = false
    @Published var showAgentToolsSheet = false
    @Published var startVmError: String?
    @Published var hostForwardError: String?
    @Published var llmMappings: [LlmModelMapping] = []
    @Published var llmLoggingEnabled = false
    @Published var agentBackupSchedules: [String: AgentBackupSchedule] = [:]

    let llmProxy = LlmProxyService()
    private let agentTools = AgentToolsService()
    private static let kLlmGuestIp = "10.0.2.3"
    private static let kLlmGuestPort: UInt16 = 80

    private var bridge = TenBoxBridgeWrapper()
    let clipboardHandler = ClipboardHandler()
    private var activeSessions: [String: VmSession] = [:]
    private var runtimeSharedFolders: [String: [SharedFolder]] = [:]
    private var sessionCancellables: [String: AnyCancellable] = [:]
    private var stateObserver: NSObjectProtocol?
    private var workspaceWakeObserver: NSObjectProtocol?
    private var agentBackupTimer: Timer?
    private var scheduledBackupsRunning: Set<String> = []
    private var pendingVmStartId: String?
    private var sleepAssertionID: IOPMAssertionID = IOPMAssertionID(0)

    init() {
        bridge.configStore.purgeAgentToolSharedFolders()
        refreshVmList()
        NSLog("[TenBoxApp] Loaded %d VM(s):", vms.count)
        for vm in vms {
            NSLog("[TenBoxApp]   - [%@] \"%@\"", vm.id, vm.name)
        }
        loadLlmMappings()
        startLlmProxyIfNeeded()
        loadAgentBackupSchedules()
        startAgentBackupScheduler()
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
                if newState == .rebooting || newState == .stopped || newState == .crashed {
                    self.removeSession(for: vmId)
                } else if newState == .running {
                    let session = self.getOrCreateSession(for: vmId)
                    session.consoleText = ""
                    session.connectIfNeeded()
                }
            }
        }
        workspaceWakeObserver = NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didWakeNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            self?.syncGuestTimeAfterHostWake()
        }
    }

    /// After host sleep/resume, push wall time to guests via qemu-ga (runtime sync-time).
    private func syncGuestTimeAfterHostWake() {
        for (vmId, session) in activeSessions {
            guard session.connected, session.ipcClient.isConnected else { continue }
            guard session.guestAgentConnected else { continue }
            guard let vm = vms.first(where: { $0.id == vmId }),
                  vm.state == .running else { continue }
            session.ipcClient.sendSyncTime()
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
        agentBackupTimer?.invalidate()
        releaseSleepAssertion()
        if let obs = stateObserver {
            NotificationCenter.default.removeObserver(obs)
        }
        if let obs = workspaceWakeObserver {
            NSWorkspace.shared.notificationCenter.removeObserver(obs)
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
        session.ipcClient.onHostForwardError = { [weak self] failedPorts in
            guard let self = self, !failedPorts.isEmpty else { return }
            let vm = self.vms.first(where: { $0.id == vmId })
            let mappings = failedPorts.map { hostPort -> String in
                if let hp = UInt16(hostPort),
                   let pf = vm?.hostForwards.first(where: { $0.hostPort == hp }) {
                    return "\(hp) → \(pf.guestPort)"
                }
                return hostPort
            }
            let list = mappings.map { "  • \($0)" }.joined(separator: "\n")
            self.hostForwardError = "The following host forward(s) failed to bind:\n\(list)\n\nThe host port(s) may already be in use."
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

    func cloneVm(id: String) {
        if let newId = bridge.cloneVm(id: id) {
            refreshVmList()
            selectedVmId = newId
        }
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

    func addRuntimeSharedFolder(_ folder: SharedFolder, toVm vmId: String) {
        runtimeSharedFolders[vmId, default: []].removeAll { $0.tag == folder.tag }
        runtimeSharedFolders[vmId, default: []].append(folder)
        sendSharedFoldersUpdateIfRunning(vmId: vmId)
    }

    func removeRuntimeSharedFolder(tag: String, fromVm vmId: String) {
        runtimeSharedFolders[vmId]?.removeAll { $0.tag == tag }
        if runtimeSharedFolders[vmId]?.isEmpty == true {
            runtimeSharedFolders.removeValue(forKey: vmId)
        }
        sendSharedFoldersUpdateIfRunning(vmId: vmId)
    }

    func addHostForward(_ pf: HostForward, toVm vmId: String) {
        _ = bridge.addHostForward(pf, toVm: vmId)
        refreshVmList()
        sendNetworkUpdateIfRunning(vmId: vmId)
    }

    func removeHostForward(hostPort: UInt16, fromVm vmId: String) {
        _ = bridge.removeHostForward(hostPort: hostPort, fromVm: vmId)
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

    func exportAgentProfile(vmId: String, agent: AgentKind, destinationURL: URL,
                            completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        guard let vm = vms.first(where: { $0.id == vmId }) else {
            completion(.failure(ConsoleCommandError("找不到 VM")))
            return
        }
        let session = getOrCreateSession(for: vmId)
        agentTools.exportProfile(vm: vm, session: session, appState: self, agent: agent,
                                 destinationURL: destinationURL, completion: completion)
    }

    func importAgentProfile(vmId: String, agent: AgentKind, sourceURL: URL,
                            completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        guard let vm = vms.first(where: { $0.id == vmId }) else {
            completion(.failure(ConsoleCommandError("找不到 VM")))
            return
        }
        let session = getOrCreateSession(for: vmId)
        agentTools.importProfile(vm: vm, session: session, appState: self, agent: agent,
                                 sourceURL: sourceURL, completion: completion)
    }

    func agentBackupStatus(vmId: String, agent: AgentKind,
                           completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        guard let vm = vms.first(where: { $0.id == vmId }) else {
            completion(.failure(ConsoleCommandError("找不到 VM")))
            return
        }
        let session = getOrCreateSession(for: vmId)
        agentTools.backupStatus(vm: vm, session: session, appState: self, agent: agent,
                                completion: completion)
    }

    func listAgentBackups(vmId: String, agent: AgentKind) -> [AgentBackupPackage] {
        (try? agentTools.listBackupPackages(vmId: vmId, agent: agent)) ?? []
    }

    func snapshotAgentBackup(vmId: String, agent: AgentKind,
                             completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        guard let vm = vms.first(where: { $0.id == vmId }) else {
            completion(.failure(ConsoleCommandError("找不到 VM")))
            return
        }
        let session = getOrCreateSession(for: vmId)
        agentTools.snapshotBackup(vm: vm, session: session, appState: self, agent: agent,
                                  keepCount: agentBackupSchedule(vmId: vmId, agent: agent).keepCount,
                                  completion: completion)
    }

    func restoreAgentBackup(vmId: String, agent: AgentKind, packageURL: URL,
                            completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        guard let vm = vms.first(where: { $0.id == vmId }) else {
            completion(.failure(ConsoleCommandError("找不到 VM")))
            return
        }
        let session = getOrCreateSession(for: vmId)
        agentTools.restoreBackup(vm: vm, session: session, appState: self, agent: agent,
                                 packageURL: packageURL, completion: completion)
    }

    func agentHealthStatus(vmId: String, agent: AgentKind,
                           completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        guard let vm = vms.first(where: { $0.id == vmId }) else {
            completion(.failure(ConsoleCommandError("找不到 VM")))
            return
        }
        let session = getOrCreateSession(for: vmId)
        agentTools.healthStatus(vm: vm, session: session, appState: self, agent: agent,
                                completion: completion)
    }

    func restartAgent(vmId: String, agent: AgentKind,
                      completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        guard let vm = vms.first(where: { $0.id == vmId }) else {
            completion(.failure(ConsoleCommandError("找不到 VM")))
            return
        }
        let session = getOrCreateSession(for: vmId)
        agentTools.restartAgent(vm: vm, session: session, appState: self, agent: agent,
                                keepCount: agentBackupSchedule(vmId: vmId, agent: agent).keepCount,
                                completion: completion)
    }

    func testAgentModel(vmId: String, agent: AgentKind,
                        completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        guard let vm = vms.first(where: { $0.id == vmId }) else {
            completion(.failure(ConsoleCommandError("找不到 VM")))
            return
        }
        let session = getOrCreateSession(for: vmId)
        agentTools.testModel(vm: vm, session: session, appState: self, agent: agent,
                             completion: completion)
    }

    func resetAgentConfig(vmId: String, agent: AgentKind,
                          completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        guard let vm = vms.first(where: { $0.id == vmId }) else {
            completion(.failure(ConsoleCommandError("找不到 VM")))
            return
        }
        let session = getOrCreateSession(for: vmId)
        agentTools.resetAgentConfig(vm: vm, session: session, appState: self, agent: agent,
                                    keepCount: agentBackupSchedule(vmId: vmId, agent: agent).keepCount,
                                    completion: completion)
    }

    func exportAgentDiagnostics(vmId: String, agent: AgentKind,
                                completion: @escaping (Result<AgentToolResult, Error>) -> Void) {
        guard let vm = vms.first(where: { $0.id == vmId }) else {
            completion(.failure(ConsoleCommandError("找不到 VM")))
            return
        }
        let session = getOrCreateSession(for: vmId)
        agentTools.exportDiagnostics(vm: vm, session: session, appState: self, agent: agent,
                                     completion: completion)
    }

    func agentBackupSchedule(vmId: String, agent: AgentKind) -> AgentBackupSchedule {
        agentBackupSchedules[Self.agentBackupScheduleKey(vmId: vmId, agent: agent)] ?? AgentBackupSchedule()
    }

    func setAgentBackupSchedule(_ schedule: AgentBackupSchedule, vmId: String, agent: AgentKind) {
        let previous = agentBackupSchedule(vmId: vmId, agent: agent)
        var normalized = AgentBackupSchedule(
            enabled: schedule.enabled,
            hour: schedule.hour,
            minute: schedule.minute,
            keepCount: schedule.keepCount,
            lastRunDate: schedule.lastRunDate,
            lastAttemptAt: schedule.lastAttemptAt,
            lastAttemptStatus: schedule.lastAttemptStatus,
            lastAttemptMessage: schedule.lastAttemptMessage
        )
        let now = Date()
        let calendar = Calendar.current
        let nowMinutes = calendar.component(.hour, from: now) * 60 + calendar.component(.minute, from: now)
        let scheduledMinutes = normalized.hour * 60 + normalized.minute
        if !previous.enabled && normalized.enabled && nowMinutes >= scheduledMinutes {
            normalized.lastRunDate = Self.agentBackupDateKey(now)
        }
        agentBackupSchedules[Self.agentBackupScheduleKey(vmId: vmId, agent: agent)] = normalized
        saveAgentBackupSchedules()
        agentTools.rotateBackups(vmId: vmId, agent: agent, keep: normalized.keepCount)
    }

    private static func agentBackupScheduleKey(vmId: String, agent: AgentKind) -> String {
        "\(vmId)|\(agent.rawValue)"
    }

    private func loadAgentBackupSchedules() {
        guard let agentBackups = readSettingsJSON()["agent_backups"] as? [String: Any],
              let schedules = agentBackups["schedules"] as? [String: Any] else {
            agentBackupSchedules = [:]
            return
        }
        var loaded: [String: AgentBackupSchedule] = [:]
        for (key, value) in schedules {
            guard let dict = value as? [String: Any] else { continue }
            loaded[key] = AgentBackupSchedule(
                enabled: dict["enabled"] as? Bool ?? false,
                hour: dict["hour"] as? Int ?? AgentBackupSchedule.defaultHour,
                minute: dict["minute"] as? Int ?? AgentBackupSchedule.defaultMinute,
                keepCount: dict["keep_count"] as? Int ?? AgentBackupSchedule.defaultKeepCount,
                lastRunDate: dict["last_run_date"] as? String,
                lastAttemptAt: dict["last_attempt_at"] as? String,
                lastAttemptStatus: dict["last_attempt_status"] as? String,
                lastAttemptMessage: dict["last_attempt_message"] as? String
            )
        }
        agentBackupSchedules = loaded
    }

    private func saveAgentBackupSchedules() {
        var json = readSettingsJSON()
        let schedules: [String: [String: Any]] = agentBackupSchedules.mapValues { schedule in
            var value: [String: Any] = [
                "enabled": schedule.enabled,
                "hour": schedule.hour,
                "minute": schedule.minute,
                "keep_count": schedule.keepCount,
            ]
            if let lastRunDate = schedule.lastRunDate {
                value["last_run_date"] = lastRunDate
            }
            if let lastAttemptAt = schedule.lastAttemptAt {
                value["last_attempt_at"] = lastAttemptAt
            }
            if let lastAttemptStatus = schedule.lastAttemptStatus {
                value["last_attempt_status"] = lastAttemptStatus
            }
            if let lastAttemptMessage = schedule.lastAttemptMessage {
                value["last_attempt_message"] = lastAttemptMessage
            }
            return value
        }
        json["agent_backups"] = ["schedules": schedules] as [String: Any]
        writeSettingsJSON(json)
    }

    private func startAgentBackupScheduler() {
        agentBackupTimer?.invalidate()
        agentBackupTimer = Timer.scheduledTimer(withTimeInterval: 60, repeats: true) { [weak self] _ in
            self?.runDueAgentBackups()
        }
        runDueAgentBackups()
    }

    private func runDueAgentBackups(now: Date = Date()) {
        let calendar = Calendar.current
        let today = Self.agentBackupDateKey(now)
        let nowMinutes = calendar.component(.hour, from: now) * 60 + calendar.component(.minute, from: now)

        for (key, schedule) in agentBackupSchedules {
            guard schedule.enabled, schedule.lastRunDate != today else { continue }
            let scheduledMinutes = schedule.hour * 60 + schedule.minute
            guard nowMinutes >= scheduledMinutes else { continue }
            guard !scheduledBackupsRunning.contains(key) else { continue }

            let parts = key.split(separator: "|", maxSplits: 1).map(String.init)
            guard parts.count == 2,
                  let agent = AgentKind(rawValue: parts[1]) else {
                continue
            }
            guard let vm = vms.first(where: { $0.id == parts[0] }) else { continue }
            guard vm.state == .running else {
                updateAgentBackupAttempt(key: key, base: schedule, status: "failed", message: "VM 未运行", at: now, lastRunDate: today)
                continue
            }

            let session = getOrCreateSession(for: vm.id)
            if !session.connected || !session.ipcClient.isConnected {
                session.connectIfNeeded()
                updateAgentBackupAttempt(key: key, base: schedule, status: "failed", message: "执行通道未连接", at: now, lastRunDate: today)
                continue
            }
            guard session.guestAgentConnected else {
                updateAgentBackupAttempt(key: key, base: schedule, status: "failed", message: "执行通道未连接", at: now, lastRunDate: today)
                continue
            }

            scheduledBackupsRunning.insert(key)
            agentTools.snapshotBackup(vm: vm, session: session, appState: self, agent: agent, keepCount: schedule.keepCount) { [weak self] result in
                DispatchQueue.main.async {
                    guard let self = self else { return }
                    self.scheduledBackupsRunning.remove(key)
                    switch result {
                    case .success:
                        self.updateAgentBackupAttempt(key: key, base: schedule, status: "success", message: "成功", at: now, lastRunDate: today)
                        NSLog("[AgentBackup] Scheduled backup completed: %@ %@", vm.id, agent.rawValue)
                    case .failure(let error):
                        self.updateAgentBackupAttempt(key: key, base: schedule, status: "failed", message: error.localizedDescription, at: now, lastRunDate: today)
                        NSLog("[AgentBackup] Scheduled backup failed: %@ %@ %@", vm.id, agent.rawValue, error.localizedDescription)
                    }
                }
            }
        }
    }

    private func updateAgentBackupAttempt(key: String,
                                          base: AgentBackupSchedule,
                                          status: String,
                                          message: String,
                                          at date: Date,
                                          lastRunDate: String? = nil) {
        var updated = agentBackupSchedules[key] ?? base
        updated.lastAttemptAt = Self.agentBackupAttemptTimeText(date)
        updated.lastAttemptStatus = status
        updated.lastAttemptMessage = message
        if let lastRunDate {
            updated.lastRunDate = lastRunDate
        }
        agentBackupSchedules[key] = updated
        saveAgentBackupSchedules()
    }

    private static func agentBackupDateKey(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.calendar = Calendar(identifier: .gregorian)
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd"
        return formatter.string(from: date)
    }

    private static func agentBackupAttemptTimeText(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.calendar = Calendar(identifier: .gregorian)
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "MM-dd HH:mm"
        return formatter.string(from: date)
    }

    // MARK: - LLM Proxy settings

    private var settingsPath: String {
        let paths = NSSearchPathForDirectoriesInDomains(.applicationSupportDirectory, .userDomainMask, true)
        let dir = (paths.first ?? NSHomeDirectory() + "/Library/Application Support") + "/TenBox"
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        return dir + "/settings.json"
    }

    private func readSettingsJSON() -> [String: Any] {
        guard let data = FileManager.default.contents(atPath: settingsPath),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return [:]
        }
        return json
    }

    private func writeSettingsJSON(_ json: [String: Any]) {
        if let data = try? JSONSerialization.data(withJSONObject: json, options: .prettyPrinted) {
            try? data.write(to: URL(fileURLWithPath: settingsPath))
        }
    }

    func loadLlmMappings() {
        guard let llmProxy = readSettingsJSON()["llm_proxy"] as? [String: Any],
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
        llmLoggingEnabled = llmProxy["enable_logging"] as? Bool ?? false
    }

    private func saveLlmMappings() {
        var json = readSettingsJSON()
        let mappingsArray: [[String: Any]] = llmMappings.map { m in
            [
                "alias": m.alias,
                "target_url": m.targetUrl,
                "api_key": m.apiKey,
                "model": m.model,
                "api_type": "openai_completions",
            ]
        }
        json["llm_proxy"] = [
            "mappings": mappingsArray,
            "enable_logging": llmLoggingEnabled,
        ] as [String: Any]
        writeSettingsJSON(json)
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

    func setLlmLogging(enabled: Bool) {
        llmLoggingEnabled = enabled
        saveLlmMappings()
        llmProxy.setLogging(enabled: enabled)
    }

    private func startLlmProxyIfNeeded() {
        guard !llmMappings.isEmpty else { return }
        llmProxy.updateMappings(llmMappings)
        _ = llmProxy.start()
        if llmLoggingEnabled {
            llmProxy.setLogging(enabled: true)
        }
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
        let folders = vm.sharedFolders + (runtimeSharedFolders[vmId] ?? [])
        let entries = folders.map { f in
            "\(f.tag)|\(f.hostPath)|\(f.readonly ? "1" : "0")"
        }
        session.ipcClient.sendSharedFoldersUpdate(entries: entries)
    }

    func sendNetworkUpdateIfRunning(vmId: String) {
        guard let session = activeSessions[vmId], session.ipcClient.isConnected,
              let vm = vms.first(where: { $0.id == vmId }) else { return }
        let hostfwdEntries = vm.hostForwards.map { pf in
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

private struct VmCommandMenuContent: View {
    @ObservedObject var appState: AppState

    private var selectedVm: VmInfo? {
        guard let vmId = appState.selectedVmId else { return nil }
        return appState.vms.first { $0.id == vmId }
    }

    var body: some View {
        let vm = selectedVm
        let isRunning = vm?.state == .running
        let isStopped = vm?.state == .stopped || vm?.state == .crashed

        Button("Start") {
            if let vm = vm { appState.requestStartVm(id: vm.id) }
        }
        .keyboardShortcut("r")
        .disabled(vm == nil || !isStopped)

        Button("Force Stop...") {
            appState.showForceStopConfirm = true
        }
        .disabled(vm == nil || !isRunning)

        Button("Reboot") {
            if let vm = vm { appState.rebootVm(id: vm.id) }
        }
        .disabled(vm == nil || !isRunning)

        Button("Shutdown") {
            if let vm = vm { appState.shutdownVm(id: vm.id) }
        }
        .disabled(vm == nil || !isRunning)

        Divider()

        Button(vm.map { $0.displayScale == 2 ? "Display 1x" : "Display 2x" } ?? "Display Scale") {
            if let vm = vm {
                appState.setDisplayScale(vm.displayScale == 1 ? 2 : 1, forVm: vm.id)
            }
        }
        .disabled(vm == nil || !isRunning)

        Divider()

        Button("Edit...") {
            appState.showEditVmDialog = true
        }
        .keyboardShortcut("e")
        .disabled(vm == nil || isRunning)

        Button("Clone") {
            if let vm = vm { appState.cloneVm(id: vm.id) }
        }
        .disabled(vm == nil || isRunning)

        Button("Delete...") {
            appState.showDeleteConfirm = true
        }
        .keyboardShortcut(.delete, modifiers: .command)
        .disabled(vm == nil || isRunning)

        Divider()

        Button("Shared Folders...") {
            appState.showSharedFoldersSheet = true
        }
        .disabled(vm == nil)

        Button("Port Forwards...") {
            appState.showPortForwardsSheet = true
        }
        .disabled(vm == nil)

        Button("Agent急救箱...") {
            appState.showAgentToolsSheet = true
        }
        .disabled(vm == nil || !isRunning)
    }
}
