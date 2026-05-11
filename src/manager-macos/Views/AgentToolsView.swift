import SwiftUI
import AppKit
import UniformTypeIdentifiers

struct AgentToolsSheet: View {
    let vmId: String
    @ObservedObject private var session: VmSession
    @EnvironmentObject var appState: AppState
    @Environment(\.dismiss) private var dismiss

    @State private var selectedAgent: AgentKind = .hermes
    @State private var runningOperation: AgentToolOperation?
    @State private var operationResult: AgentOperationDisplay?
    @State private var pendingConfirmation: PendingAgentConfirmation?
    @State private var latestBackupText = "正在读取..."
    @State private var latestBackupPath: String?
    @State private var backupSchedule = AgentBackupSchedule()
    @State private var backupPackages: [AgentBackupPackage] = []
    @State private var selectedBackupId: String?
    @State private var showsAdvancedActions = false

    init(vmId: String, session: VmSession) {
        self.vmId = vmId
        self.session = session
    }

    private var vm: VmInfo? {
        appState.vms.first(where: { $0.id == vmId })
    }

    private var canRun: Bool {
        vm?.state == .running && session.guestAgentConnected && runningOperation == nil
    }

    private var confirmationPresented: Binding<Bool> {
        Binding(
            get: { pendingConfirmation != nil },
            set: { if !$0 { pendingConfirmation = nil } }
        )
    }

    var body: some View {
        VStack(spacing: 0) {
            header

            Divider()

            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    statusPanel

                    Picker("Agent", selection: $selectedAgent) {
                        ForEach(AgentKind.allCases) { agent in
                            Text(agent.displayName).tag(agent)
                        }
                    }
                    .pickerStyle(.segmented)

                    triagePanel

                    if let runningOperation {
                        HStack(spacing: 8) {
                            ProgressView()
                                .controlSize(.small)
                            Text(runningOperation.runningText(agent: selectedAgent))
                                .foregroundStyle(.secondary)
                        }
                    }

                    if let operationResult {
                        AgentOperationResultView(result: operationResult)
                        if let report = operationResult.healthReport, report.state != "ok" {
                            repairSuggestionPanel(report: report)
                        }
                    }

                    advancedActionsPanel

                    schedulePanel

                    backupPickerPanel
                }
                .padding()
            }
        }
        .frame(width: 640, height: 600)
        .onAppear {
            loadSchedule()
            refreshBackupList()
            refreshBackupSummary()
        }
        .onChange(of: selectedAgent, perform: { _ in
            operationResult = nil
            selectedBackupId = nil
            loadSchedule()
            refreshBackupList()
            refreshBackupSummary()
        })
        .alert(pendingConfirmation?.title ?? "", isPresented: confirmationPresented) {
            Button("取消", role: .cancel) {
                pendingConfirmation = nil
            }
            if let pendingConfirmation {
                Button(pendingConfirmation.confirmTitle, role: .destructive) {
                    confirmPendingAction(pendingConfirmation)
                }
            }
        } message: {
            Text(pendingConfirmation?.message ?? "")
        }
    }

    private var header: some View {
        HStack(spacing: 12) {
            Text("Agent急救箱")
                .font(.title3)
                .fontWeight(.semibold)

            Spacer()

            Button("完成") { dismiss() }
                .keyboardShortcut(.cancelAction)
        }
        .padding()
    }

    private var statusPanel: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 8) {
                StatusPill(
                    title: "虚拟机",
                    value: vmStateText,
                    systemImage: "desktopcomputer",
                    tone: vm?.state == .running ? .ok : .muted
                )
                StatusPill(
                    title: "执行通道",
                    value: session.guestAgentConnected ? "已连接" : "未连接",
                    systemImage: "checkmark.seal",
                    tone: session.guestAgentConnected ? .ok : .warning
                )
                StatusPill(
                    title: "最近备份",
                    value: latestBackupText,
                    systemImage: "clock.arrow.circlepath",
                    tone: latestBackupPath == nil ? .muted : .ok
                )
            }

            if vm?.state != .running {
                Text("请先启动 VM，再使用 Agent 数据工具。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else if !session.guestAgentConnected {
                Text("执行通道连接后才能执行导入、备份和健康检查。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(12)
        .background(.quaternary.opacity(0.7))
        .clipShape(RoundedRectangle(cornerRadius: 8))
    }

    private var triagePanel: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("急救")
                .font(.headline)

            Button {
                checkHealth()
            } label: {
                Label("一键诊断", systemImage: "stethoscope")
                    .frame(maxWidth: .infinity, alignment: .center)
            }
            .controlSize(.large)
            .disabled(!canRun)
            .help("检查 Agent 服务、模型代理、浏览器和磁盘状态")

            HStack(spacing: 10) {
                Button {
                    snapshotBackup()
                } label: {
                    Label("立即备份", systemImage: "clock.arrow.circlepath")
                        .frame(maxWidth: .infinity)
                }
                .disabled(!canRun)

                Button {
                    exportProfile()
                } label: {
                    Label("导出迁移包", systemImage: "square.and.arrow.up")
                        .frame(maxWidth: .infinity)
                }
                .disabled(!canRun)
            }

            Text("建议先点“一键诊断”。只有需要迁移或人工处理时，再导入、重置配置或导出诊断包。")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(12)
        .background(.quaternary.opacity(0.45))
        .clipShape(RoundedRectangle(cornerRadius: 8))
    }

    private var schedulePanel: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("定时备份")
                    .font(.headline)
                Spacer()
                Toggle("启用", isOn: scheduleEnabledBinding)
                    .toggleStyle(.checkbox)
            }

            HStack(spacing: 12) {
                Picker("时间", selection: scheduleHourBinding) {
                    ForEach(0..<24, id: \.self) { hour in
                        Text(String(format: "%02d", hour)).tag(hour)
                    }
                }
                .frame(width: 112)

                Picker("分钟", selection: scheduleMinuteBinding) {
                    ForEach(0..<60, id: \.self) { minute in
                        Text(String(format: "%02d", minute)).tag(minute)
                    }
                }
                .frame(width: 112)

                Stepper(value: scheduleKeepCountBinding, in: 1...99) {
                    Text("最多保留 \(backupSchedule.keepCount) 条")
                }

                Spacer()
            }

            Text(scheduleDescription)
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(12)
        .background(.quaternary.opacity(0.45))
        .clipShape(RoundedRectangle(cornerRadius: 8))
    }

    private var scheduleDescription: String {
        guard backupSchedule.enabled else {
            return "定时备份未启用。默认时间为 03:00，只有 VM 运行且执行通道已连接时才会执行。"
        }
        return "每天 \(backupSchedule.timeText) 自动备份；\(nextBackupText)。"
    }

    private var nextBackupText: String {
        let calendar = Calendar.current
        let now = Date()
        var components = calendar.dateComponents([.year, .month, .day], from: now)
        components.hour = backupSchedule.hour
        components.minute = backupSchedule.minute
        components.second = 0
        var next = calendar.date(from: components) ?? now
        if next <= now {
            next = calendar.date(byAdding: .day, value: 1, to: next) ?? next
        }
        let formatter = DateFormatter()
        formatter.dateFormat = calendar.isDateInToday(next) ? "今天 HH:mm" : "明天 HH:mm"
        return "下次预计 \(formatter.string(from: next))"
    }

    private var backupPickerPanel: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("备份列表")
                    .font(.headline)
                Spacer()
                Button {
                    refreshBackupList()
                    refreshBackupSummary()
                } label: {
                    Label("刷新", systemImage: "arrow.clockwise")
                }
                .buttonStyle(.borderless)
            }

            if backupPackages.isEmpty {
                Text("还没有备份。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.vertical, 4)
            } else {
                VStack(spacing: 6) {
                    ForEach(backupPackages) { package in
                        BackupPackageRow(
                            package: package,
                            isSelected: selectedBackupId == package.id
                        ) {
                            selectedBackupId = package.id
                        }
                    }
                }

                HStack {
                    Button {
                        guard let package = selectedBackupPackage else { return }
                        pendingConfirmation = .restoreBackup(package)
                    } label: {
                        Label("恢复选中备份", systemImage: "arrow.uturn.backward")
                    }
                    .disabled(!canRun || selectedBackupPackage == nil)

                    Spacer()
                }
            }
        }
        .padding(12)
        .background(.quaternary.opacity(0.45))
        .clipShape(RoundedRectangle(cornerRadius: 8))
    }

    private var selectedBackupPackage: AgentBackupPackage? {
        guard let selectedBackupId else { return nil }
        return backupPackages.first { $0.id == selectedBackupId }
    }

    private var advancedActionsPanel: some View {
        DisclosureGroup(isExpanded: $showsAdvancedActions) {
            VStack(alignment: .leading, spacing: 10) {
                operationSection(
                    title: "高级操作",
                    operations: [.importProfile, .restartAgent, .resetConfig, .diagnostics]
                )
                Text("这些操作会改动配置、覆盖数据或生成排障包，建议在诊断结果提示后再使用。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .padding(.top, 8)
        } label: {
            Text("高级操作")
                .font(.headline)
        }
        .padding(12)
        .background(.quaternary.opacity(0.45))
        .clipShape(RoundedRectangle(cornerRadius: 8))
    }

    private func repairSuggestionPanel(report: HealthReport) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("建议修复")
                .font(.headline)

            LazyVGrid(columns: [
                GridItem(.flexible(), spacing: 10),
                GridItem(.flexible(), spacing: 10)
            ], spacing: 10) {
                if report.isError("agent_service") || report.isError("gateway_port") {
                    Button {
                        restartAgent()
                    } label: {
                        Label("重启服务", systemImage: "arrow.clockwise")
                            .frame(maxWidth: .infinity)
                    }
                    .disabled(!canRun)
                }

                if report.isError("llm_proxy") {
                    Button {
                        openLlmProxySettings()
                    } label: {
                        Label("检查 LLM Proxy", systemImage: "key.viewfinder")
                            .frame(maxWidth: .infinity)
                    }
                    .disabled(runningOperation != nil)

                    Button {
                        pendingConfirmation = .resetConfig
                    } label: {
                        Label("重置模型配置", systemImage: "slider.horizontal.2.square")
                            .frame(maxWidth: .infinity)
                    }
                    .disabled(!canRun)
                }

                Button {
                    exportDiagnostics()
                } label: {
                    Label("导出诊断包", systemImage: "doc.zipper")
                        .frame(maxWidth: .infinity)
                }
                .disabled(!canRun)
            }
        }
        .padding(12)
        .background(Color.orange.opacity(0.08))
        .clipShape(RoundedRectangle(cornerRadius: 8))
    }

    private var scheduleEnabledBinding: Binding<Bool> {
        Binding(
            get: { backupSchedule.enabled },
            set: { backupSchedule.enabled = $0; saveSchedule() }
        )
    }

    private var scheduleHourBinding: Binding<Int> {
        Binding(
            get: { backupSchedule.hour },
            set: { backupSchedule.hour = $0; saveSchedule() }
        )
    }

    private var scheduleMinuteBinding: Binding<Int> {
        Binding(
            get: { backupSchedule.minute },
            set: { backupSchedule.minute = $0; saveSchedule() }
        )
    }

    private var scheduleKeepCountBinding: Binding<Int> {
        Binding(
            get: { backupSchedule.keepCount },
            set: {
                backupSchedule.keepCount = $0
                saveSchedule()
                refreshBackupList()
                refreshBackupSummary()
            }
        )
    }

    private var vmStateText: String {
        switch vm?.state {
        case .running: return "运行中"
        case .starting: return "启动中"
        case .rebooting: return "重启中"
        case .crashed: return "异常退出"
        case .stopped: return "已停止"
        case .none: return "未知"
        }
    }

    private func operationSection(title: String, operations: [AgentToolOperation]) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title)
                .font(.headline)

            LazyVGrid(columns: [
                GridItem(.flexible(), spacing: 10),
                GridItem(.flexible(), spacing: 10),
                GridItem(.flexible(), spacing: 10)
            ], spacing: 10) {
                ForEach(operations) { operation in
                    Button {
                        run(operation)
                    } label: {
                        Label(operation.title, systemImage: operation.systemImage)
                            .frame(maxWidth: .infinity, alignment: .center)
                    }
                    .disabled(!canRun)
                    .help(operation.help)
                }
            }
        }
    }

    private func run(_ operation: AgentToolOperation) {
        switch operation {
        case .exportProfile:
            exportProfile()
        case .importProfile:
            importProfile()
        case .snapshotBackup:
            snapshotBackup()
        case .restoreBackup:
            if let package = selectedBackupPackage {
                pendingConfirmation = .restoreBackup(package)
            }
        case .healthCheck:
            checkHealth()
        case .restartAgent:
            restartAgent()
        case .resetConfig:
            pendingConfirmation = .resetConfig
        case .diagnostics:
            exportDiagnostics()
        }
    }

    private func exportProfile() {
        guard let vm = vm else { return }
        let panel = NSSavePanel()
        panel.title = "导出 Agent 数据"
        panel.nameFieldStringValue = "\(vm.name)-\(selectedAgent.rawValue)-profile.tar.gz"
        applyGzipTypeLimit(to: panel)
        presentPanel(panel) { response in
            guard response == .OK, let url = panel.url else { return }
            let destinationURL = Self.normalizedPackageURL(url)
            runOperation(.exportProfile, revealPath: destinationURL.path) {
                appState.exportAgentProfile(vmId: vm.id, agent: selectedAgent, destinationURL: destinationURL, completion: $0)
            }
        }
    }

    private func importProfile() {
        let panel = NSOpenPanel()
        panel.title = "导入 Agent 数据"
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        applyGzipTypeLimit(to: panel)
        presentPanel(panel) { response in
            guard response == .OK, let url = panel.url else { return }
            guard Self.isAgentPackageURL(url) else {
                operationResult = AgentOperationDisplay(
                    isSuccess: false,
                    title: "导入失败",
                    summary: "请选择 .tar.gz 或 .tgz 文件",
                    details: url.path,
                    revealPath: nil,
                    healthReport: nil
                )
                return
            }
            pendingConfirmation = .importProfile(url)
        }
    }

    private func presentPanel(_ panel: NSSavePanel, completion: @escaping (NSApplication.ModalResponse) -> Void) {
        if let window = NSApplication.shared.keyWindow {
            panel.beginSheetModal(for: window, completionHandler: completion)
        } else {
            panel.begin(completionHandler: completion)
        }
    }

    private func applyGzipTypeLimit(to panel: NSSavePanel) {
        if let gzipType = UTType(filenameExtension: "gz") {
            panel.allowedContentTypes = [gzipType]
        }
    }

    private func confirmPendingAction(_ pending: PendingAgentConfirmation) {
        pendingConfirmation = nil
        switch pending {
        case .importProfile(let url):
            guard let vm = vm else { return }
            runOperation(.importProfile) {
                appState.importAgentProfile(vmId: vm.id, agent: selectedAgent, sourceURL: url, completion: $0)
            }
        case .restoreBackup(let package):
            restoreBackup(package)
        case .resetConfig:
            resetConfig()
        }
    }

    private func snapshotBackup() {
        guard let vm = vm else { return }
        runOperation(.snapshotBackup) {
            appState.snapshotAgentBackup(vmId: vm.id, agent: selectedAgent, completion: $0)
        }
    }

    private func restoreBackup(_ package: AgentBackupPackage) {
        guard let vm = vm else { return }
        runOperation(.restoreBackup) {
            appState.restoreAgentBackup(vmId: vm.id, agent: selectedAgent, packageURL: package.url, completion: $0)
        }
    }

    private func checkHealth() {
        guard let vm = vm else { return }
        runOperation(.healthCheck) {
            appState.agentHealthStatus(vmId: vm.id, agent: selectedAgent, completion: $0)
        }
    }

    private func restartAgent() {
        guard let vm = vm else { return }
        runOperation(.restartAgent) {
            appState.restartAgent(vmId: vm.id, agent: selectedAgent, completion: $0)
        }
    }

    private func openLlmProxySettings() {
        dismiss()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) {
            appState.showLlmProxySheet = true
        }
    }

    private func resetConfig() {
        guard let vm = vm else { return }
        runOperation(.resetConfig) {
            appState.resetAgentConfig(vmId: vm.id, agent: selectedAgent, completion: $0)
        }
    }

    private func exportDiagnostics() {
        guard let vm = vm else { return }
        runOperation(.diagnostics) {
            appState.exportAgentDiagnostics(vmId: vm.id, agent: selectedAgent, completion: $0)
        }
    }

    private func refreshBackupSummary() {
        guard let vm = vm else {
            latestBackupText = "未知"
            latestBackupPath = nil
            return
        }
        appState.agentBackupStatus(vmId: vm.id, agent: selectedAgent) { result in
            DispatchQueue.main.async {
                switch result {
                case .success(let status):
                    latestBackupPath = Self.extractBackupPath(from: status.output)
                    if let latestBackupPath {
                        latestBackupText = Self.compactBackupText(path: latestBackupPath)
                    } else {
                        latestBackupText = "暂无"
                    }
                case .failure:
                    latestBackupText = "读取失败"
                    latestBackupPath = nil
                }
            }
        }
    }

    private func refreshBackupList() {
        guard let vm = vm else {
            backupPackages = []
            selectedBackupId = nil
            return
        }
        backupPackages = appState.listAgentBackups(vmId: vm.id, agent: selectedAgent)
        if let selectedBackupId, backupPackages.contains(where: { $0.id == selectedBackupId }) {
            return
        }
        selectedBackupId = backupPackages.first?.id
    }

    private func loadSchedule() {
        backupSchedule = appState.agentBackupSchedule(vmId: vmId, agent: selectedAgent)
    }

    private func saveSchedule() {
        appState.setAgentBackupSchedule(backupSchedule, vmId: vmId, agent: selectedAgent)
    }

    private func runOperation(_ operation: AgentToolOperation,
                              revealPath: String? = nil,
                              _ action: (@escaping (Result<AgentToolResult, Error>) -> Void) -> Void) {
        operationResult = nil
        runningOperation = operation
        action { result in
            DispatchQueue.main.async {
                runningOperation = nil
                switch result {
                case .success(let output):
                    operationResult = Self.makeSuccessDisplay(
                        operation: operation,
                        result: output,
                        revealPath: revealPath
                    )
                    refreshBackupSummary()
                    refreshBackupList()
                case .failure(let error):
                    operationResult = Self.makeFailureDisplay(operation: operation, error: error)
                    refreshBackupSummary()
                    refreshBackupList()
                }
            }
        }
    }

    private static func makeSuccessDisplay(operation: AgentToolOperation,
                                           result: AgentToolResult,
                                           revealPath: String?) -> AgentOperationDisplay {
        let raw = result.output.trimmingCharacters(in: .whitespacesAndNewlines)
        let detectedPath = revealPath ?? operation.revealPath(from: result)
        let health = operation.showsHealth ? HealthReport.parse(from: raw) : nil
        let summary = result.message.trimmingCharacters(in: .whitespacesAndNewlines)
        return AgentOperationDisplay(
            isSuccess: true,
            title: operation.successTitle,
            summary: summary.isEmpty ? "操作已完成" : summary,
            details: raw,
            revealPath: detectedPath,
            healthReport: health
        )
    }

    private static func makeFailureDisplay(operation: AgentToolOperation, error: Error) -> AgentOperationDisplay {
        let raw = error.localizedDescription.trimmingCharacters(in: .whitespacesAndNewlines)
        return AgentOperationDisplay(
            isSuccess: false,
            title: operation.failureTitle,
            summary: friendlyErrorMessage(raw),
            details: raw,
            revealPath: nil,
            healthReport: nil
        )
    }

    private static func extractBackupPath(from output: String) -> String? {
        let prefix = "最近备份："
        for line in output.split(whereSeparator: { $0.isNewline }) {
            let text = String(line).trimmingCharacters(in: .whitespaces)
            if text.hasPrefix(prefix) {
                return String(text.dropFirst(prefix.count))
            }
        }
        return nil
    }

    private static func isAgentPackageURL(_ url: URL) -> Bool {
        let name = url.lastPathComponent.lowercased()
        return name.hasSuffix(".tar.gz") || name.hasSuffix(".tgz")
    }

    private static func normalizedPackageURL(_ url: URL) -> URL {
        if isAgentPackageURL(url) {
            return url
        }
        if url.lastPathComponent.lowercased().hasSuffix(".gz") {
            return url.deletingPathExtension().appendingPathExtension("tar.gz")
        }
        return url.appendingPathExtension("tar.gz")
    }

    private static func compactBackupText(path: String) -> String {
        let url = URL(fileURLWithPath: path)
        if let attrs = try? FileManager.default.attributesOfItem(atPath: path),
           let date = attrs[.modificationDate] as? Date {
            let formatter = DateFormatter()
            formatter.dateFormat = "MM-dd HH:mm"
            return formatter.string(from: date)
        }
        return url.lastPathComponent
    }

    private static func friendlyErrorMessage(_ raw: String) -> String {
        if raw.isEmpty { return "操作失败" }
        let checks: [(String, String)] = [
            ("VM not found", "找不到 VM"),
            ("VM runtime is not connected", "VM 运行时未连接"),
            ("Guest agent is not connected", "Guest Agent 未连接"),
            ("Command timed out", "操作超时"),
            ("Failed to send guest agent command", "发送 Guest Agent 命令失败"),
            ("Agent data is not initialized", "Agent 数据尚未初始化"),
            ("No backup package found", "没有找到可恢复的备份"),
            ("package not found", "找不到导入包"),
            ("manifest.json missing", "导入包缺少 manifest.json"),
            ("files.tar.gz missing", "导入包缺少 files.tar.gz"),
            ("Model proxy is unavailable", "模型代理不可用"),
            ("Browser is unavailable", "浏览器不可用"),
            ("Disk space is low", "磁盘空间不足"),
            ("Agent service is not running", "Agent 服务未运行"),
            ("Agent gateway is unavailable", "Agent 网关不可用")
        ]
        for (needle, message) in checks where raw.contains(needle) {
            return message
        }
        return raw
    }
}

private enum AgentToolOperation: String, CaseIterable, Identifiable {
    case exportProfile
    case importProfile
    case snapshotBackup
    case restoreBackup
    case healthCheck
    case restartAgent
    case resetConfig
    case diagnostics

    var id: String { rawValue }

    var title: String {
        switch self {
        case .exportProfile: return "导出"
        case .importProfile: return "导入"
        case .snapshotBackup: return "立即备份"
        case .restoreBackup: return "恢复备份"
        case .healthCheck: return "一键诊断"
        case .restartAgent: return "重启服务"
        case .resetConfig: return "重置配置"
        case .diagnostics: return "导出诊断"
        }
    }

    var systemImage: String {
        switch self {
        case .exportProfile: return "square.and.arrow.up"
        case .importProfile: return "square.and.arrow.down"
        case .snapshotBackup: return "clock.arrow.circlepath"
        case .restoreBackup: return "arrow.uturn.backward"
        case .healthCheck: return "stethoscope"
        case .restartAgent: return "arrow.clockwise"
        case .resetConfig: return "slider.horizontal.2.square"
        case .diagnostics: return "doc.zipper"
        }
    }

    var help: String {
        switch self {
        case .exportProfile: return "导出当前 Agent 数据"
        case .importProfile: return "从归档包导入 Agent 数据"
        case .snapshotBackup: return "创建一份主机侧备份"
        case .restoreBackup: return "用选中的备份恢复 Agent 数据"
        case .healthCheck: return "检查 Agent 运行状态"
        case .restartAgent: return "重启 Agent 服务"
        case .resetConfig: return "重置 Agent 模型配置"
        case .diagnostics: return "导出诊断包"
        }
    }

    var successTitle: String {
        switch self {
        case .exportProfile: return "导出完成"
        case .importProfile: return "导入完成"
        case .snapshotBackup: return "备份完成"
        case .restoreBackup: return "恢复完成"
        case .healthCheck: return "诊断完成"
        case .restartAgent: return "重启完成"
        case .resetConfig: return "配置已重置"
        case .diagnostics: return "诊断包已导出"
        }
    }

    var failureTitle: String {
        switch self {
        case .exportProfile: return "导出失败"
        case .importProfile: return "导入失败"
        case .snapshotBackup: return "备份失败"
        case .restoreBackup: return "恢复失败"
        case .healthCheck: return "诊断失败"
        case .restartAgent: return "重启失败"
        case .resetConfig: return "重置失败"
        case .diagnostics: return "诊断导出失败"
        }
    }

    var showsHealth: Bool {
        switch self {
        case .healthCheck, .restartAgent, .resetConfig: return true
        default: return false
        }
    }

    func runningText(agent: AgentKind) -> String {
        switch self {
        case .exportProfile: return "正在导出 \(agent.displayName) 数据..."
        case .importProfile: return "正在导入 \(agent.displayName) 数据..."
        case .snapshotBackup: return "正在备份 \(agent.displayName) 数据..."
        case .restoreBackup: return "正在恢复 \(agent.displayName) 备份..."
        case .healthCheck: return "正在诊断 \(agent.displayName) 状态..."
        case .restartAgent: return "正在重启 \(agent.displayName) 服务..."
        case .resetConfig: return "正在重置 \(agent.displayName) 配置..."
        case .diagnostics: return "正在导出 \(agent.displayName) 诊断包..."
        }
    }

    func revealPath(from result: AgentToolResult) -> String? {
        let raw = result.output.trimmingCharacters(in: .whitespacesAndNewlines)
        switch self {
        case .snapshotBackup, .restoreBackup, .diagnostics:
            return raw.split(whereSeparator: { $0.isNewline }).map(String.init).last
        default:
            return nil
        }
    }
}

private enum PendingAgentConfirmation: Identifiable {
    case importProfile(URL)
    case restoreBackup(AgentBackupPackage)
    case resetConfig

    var id: String {
        switch self {
        case .importProfile(let url): return "import-\(url.path)"
        case .restoreBackup(let package): return "restore-\(package.id)"
        case .resetConfig: return "reset"
        }
    }

    var title: String {
        switch self {
        case .importProfile: return "确认导入 Agent 数据？"
        case .restoreBackup: return "确认恢复这个备份？"
        case .resetConfig: return "确认重置配置？"
        }
    }

    var message: String {
        switch self {
        case .importProfile(let url):
            return "导入会替换当前 Agent 数据。文件：\(url.lastPathComponent)"
        case .restoreBackup(let package):
            return "恢复会用选中的备份覆盖当前 Agent 数据。文件：\(package.filename)"
        case .resetConfig:
            return "重置会覆盖当前 Agent 模型配置。"
        }
    }

    var confirmTitle: String {
        switch self {
        case .importProfile: return "导入"
        case .restoreBackup: return "恢复"
        case .resetConfig: return "重置"
        }
    }
}

private struct AgentOperationDisplay: Identifiable {
    let id = UUID()
    let isSuccess: Bool
    let title: String
    let summary: String
    let details: String
    let revealPath: String?
    let healthReport: HealthReport?
}

private struct BackupPackageRow: View {
    let package: AgentBackupPackage
    let isSelected: Bool
    let select: () -> Void

    var body: some View {
        Button(action: select) {
            HStack(spacing: 8) {
                Image(systemName: isSelected ? "checkmark.circle.fill" : "circle")
                    .foregroundStyle(isSelected ? Color.accentColor : Color.secondary)
                VStack(alignment: .leading, spacing: 2) {
                    Text(package.filename)
                        .fontWeight(.medium)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    Text("\(Self.dateText(package.modifiedAt)) · \(Self.sizeText(package.sizeBytes))")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .padding(.horizontal, 8)
        .padding(.vertical, 6)
        .background(isSelected ? Color.accentColor.opacity(0.12) : Color.clear)
        .clipShape(RoundedRectangle(cornerRadius: 6))
    }

    private static func dateText(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd HH:mm:ss"
        return formatter.string(from: date)
    }

    private static func sizeText(_ bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
    }
}

private struct AgentOperationResultView: View {
    let result: AgentOperationDisplay
    @State private var showsDetails = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 8) {
                Image(systemName: result.isSuccess ? "checkmark.circle.fill" : "xmark.octagon.fill")
                    .foregroundStyle(result.isSuccess ? .green : .red)
                VStack(alignment: .leading, spacing: 2) {
                    Text(result.title)
                        .fontWeight(.semibold)
                    Text(result.summary)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .textSelection(.enabled)
                }
                Spacer()
            }

            if let report = result.healthReport {
                HealthReportView(report: report)
            }

            HStack(spacing: 12) {
                if let path = result.revealPath, !path.isEmpty {
                    Button {
                        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: path)])
                    } label: {
                        Label("在 Finder 中显示", systemImage: "folder")
                    }
                    .buttonStyle(.link)
                }

                if !result.details.isEmpty {
                    Button {
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(result.details, forType: .string)
                    } label: {
                        Label("复制详情", systemImage: "doc.on.doc")
                    }
                    .buttonStyle(.link)
                }

                Spacer()
            }

            if !result.details.isEmpty {
                DisclosureGroup(isExpanded: $showsDetails) {
                    ScrollView {
                        Text(result.details)
                            .font(.system(.caption, design: .monospaced))
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(.top, 4)
                    }
                    .frame(maxHeight: 140)
                } label: {
                    Text("详情")
                        .font(.caption)
                }
            }
        }
        .padding(12)
        .background(result.isSuccess ? Color.green.opacity(0.08) : Color.red.opacity(0.08))
        .clipShape(RoundedRectangle(cornerRadius: 8))
    }
}

private struct StatusPill: View {
    enum Tone {
        case ok
        case warning
        case muted
    }

    let title: String
    let value: String
    let systemImage: String
    let tone: Tone

    private var color: Color {
        switch tone {
        case .ok: return .green
        case .warning: return .orange
        case .muted: return .secondary
        }
    }

    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: systemImage)
                .foregroundStyle(color)
            VStack(alignment: .leading, spacing: 1) {
                Text(title)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                Text(value)
                    .font(.caption)
                    .fontWeight(.medium)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 6)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.background.opacity(0.6))
        .clipShape(RoundedRectangle(cornerRadius: 6))
    }
}

private struct HealthReport {
    let state: String
    let message: String
    let checks: [HealthCheckItem]

    func value(_ key: String) -> String {
        checks.first { $0.key == key }?.value ?? "unknown"
    }

    func isError(_ key: String) -> Bool {
        let state = value(key)
        return state == "error" || state == "space_low"
    }

    static func parse(from raw: String) -> HealthReport? {
        let jsonLine = raw
            .split(whereSeparator: { $0.isNewline })
            .map { String($0).trimmingCharacters(in: .whitespacesAndNewlines) }
            .first { $0.hasPrefix("{") && $0.hasSuffix("}") }
        guard let jsonLine,
              let data = jsonLine.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return nil
        }
        let checks = object["checks"] as? [String: Any] ?? [:]
        return HealthReport(
            state: object["state"] as? String ?? "unknown",
            message: translateMessage(object["message"] as? String ?? ""),
            checks: [
                HealthCheckItem(key: "agent_service", title: "Agent 服务", value: checks["agent_service"] as? String ?? "unknown"),
                HealthCheckItem(key: "gateway_port", title: "网关端口", value: checks["gateway_port"] as? String ?? "unknown"),
                HealthCheckItem(key: "llm_proxy", title: "模型代理", value: checks["llm_proxy"] as? String ?? "unknown"),
                HealthCheckItem(key: "browser", title: "浏览器", value: checks["browser"] as? String ?? "unknown"),
                HealthCheckItem(key: "disk", title: "磁盘空间", value: checks["disk"] as? String ?? "unknown")
            ]
        )
    }

    private static func translateMessage(_ message: String) -> String {
        switch message {
        case "Agent normal": return "Agent 正常"
        case "Disk space is low": return "磁盘空间不足"
        case "Agent service is not running": return "Agent 服务未运行"
        case "Agent gateway is unavailable": return "Agent 网关不可用"
        case "Model proxy is unavailable": return "模型代理不可用"
        case "Browser is unavailable": return "浏览器不可用"
        case "Model proxy is available": return "模型代理可用"
        default: return message.isEmpty ? "状态未知" : message
        }
    }
}

private struct HealthCheckItem: Identifiable {
    let id = UUID()
    let key: String
    let title: String
    let value: String

    var displayValue: String {
        switch value {
        case "ok": return "正常"
        case "error": return "异常"
        case "skipped": return "跳过"
        case "space_low": return "空间不足"
        default: return "未知"
        }
    }

    var color: Color {
        switch value {
        case "ok", "skipped": return .green
        case "space_low": return .orange
        case "error": return .red
        default: return .secondary
        }
    }

    var icon: String {
        switch value {
        case "ok", "skipped": return "checkmark.circle.fill"
        case "space_low": return "exclamationmark.triangle.fill"
        case "error": return "xmark.octagon.fill"
        default: return "questionmark.circle"
        }
    }
}

private struct HealthReportView: View {
    let report: HealthReport

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(report.message)
                .font(.caption)
                .foregroundStyle(report.state == "ok" ? Color.secondary : Color.red)

            LazyVGrid(columns: [
                GridItem(.flexible(), spacing: 8),
                GridItem(.flexible(), spacing: 8)
            ], spacing: 8) {
                ForEach(report.checks) { item in
                    HStack(spacing: 6) {
                        Image(systemName: item.icon)
                            .foregroundStyle(item.color)
                        Text(item.title)
                        Spacer()
                        Text(item.displayValue)
                            .foregroundStyle(.secondary)
                    }
                    .font(.caption)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 6)
                    .background(.background.opacity(0.65))
                    .clipShape(RoundedRectangle(cornerRadius: 6))
                }
            }
        }
    }
}
