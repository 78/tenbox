import SwiftUI
import MetalKit
import AppKit

class VmSession: ObservableObject {
    let vmId: String
    let ipcClient = IpcClientWrapper()
    let renderer: MetalDisplayRenderer? = MetalDisplayRenderer.create()
    let audioPlayer = CoreAudioPlayer()

    @Published var consoleText = ""
    @Published var guestAgentConnected = false
    @Published var runtimeState = ""
    @Published var connected = false
    @Published var displaySize: CGSize = .zero
    @Published var displayInitialized = false
    var lastSentDisplayW: UInt32 = 0
    var lastSentDisplayH: UInt32 = 0
    var lastResizeFromVmTime: CFTimeInterval = 0
    var displayViewSize: CGSize = .zero
    @Published var activeTab = 0
    var displayScale: Int = 1
    var onRuntimeRunning: (() -> Void)?

    private let bridge = TenBoxBridgeWrapper()
    private weak var clipboardHandler: ClipboardHandler?
    private var connecting = false
    private static let maxConsoleSize = 64 * 1024
    private var pendingConsoleCommands: [String: PendingConsoleCommand] = [:]
    private var nextGuestExecRequestId: UInt64 = 1
    private var pendingGuestExecCommands: [UInt64: PendingGuestExecCommand] = [:]

    private struct PendingConsoleCommand {
        let beginMarker: String
        let endPrefix: String
        let completion: (Result<ConsoleCommandResult, Error>) -> Void
        let beginTimeoutWorkItem: DispatchWorkItem
        let timeoutWorkItem: DispatchWorkItem
    }

    private struct PendingGuestExecCommand {
        let completion: (Result<ConsoleCommandResult, Error>) -> Void
        let timeoutWorkItem: DispatchWorkItem
    }

    init(vmId: String, clipboardHandler: ClipboardHandler) {
        self.vmId = vmId
        self.clipboardHandler = clipboardHandler
        setupCallbacks()
    }

    private func setupCallbacks() {
        ipcClient.onConsole = { [weak self] text in
            guard let self = self else { return }
            self.appendConsoleText(Self.filterAnsi(text))
        }
        ipcClient.onRuntimeState = { [weak self] state in
            self?.runtimeState = state
            if state == "running" {
                self?.onRuntimeRunning?()
            }
        }
        ipcClient.onGuestAgentState = { [weak self] conn in
            self?.guestAgentConnected = conn
        }
        ipcClient.onGuestExecResult = { [weak self] requestId, ok, exitCode, stdoutText, stderrText, error in
            self?.finishGuestExecCommand(
                requestId: requestId,
                ok: ok,
                exitCode: exitCode,
                stdoutText: stdoutText,
                stderrText: stderrText,
                error: error
            )
        }

        ipcClient.onFrame = { [weak self] pixelBytes, pixelLength, w, h, stride, resW, resH, dirtyX, dirtyY in
            guard let self = self, let renderer = self.renderer else { return }
            renderer.blitDirtyRect(
                pixels: pixelBytes,
                dirtyX: Int(dirtyX),
                dirtyY: Int(dirtyY),
                dirtyWidth: Int(w),
                dirtyHeight: Int(h),
                srcStride: Int(stride),
                resourceWidth: Int(resW),
                resourceHeight: Int(resH)
            )
        }

        ipcClient.onAudio = { [weak self] pcm, rate, channels in
            guard let self = self else { return }
            self.audioPlayer.enqueuePcmData(pcm, sampleRate: rate, channels: UInt32(channels))
        }

        ipcClient.onDisplayState = { [weak self] active, w, h in
            guard let self = self else { return }
            if active && w > 0 && h > 0 {
                let newSize = CGSize(width: CGFloat(w), height: CGFloat(h))
                DispatchQueue.main.async {
                    let wasInitialized = self.displayInitialized
                    self.displayInitialized = true
                    self.lastSentDisplayW = (w + 7) & ~7
                    self.lastSentDisplayH = h
                    self.lastResizeFromVmTime = CACurrentMediaTime()
                    if !wasInitialized {
                        self.activeTab = 2
                    }
                    if self.displaySize != newSize {
                        self.displaySize = newSize
                    }
                }
            } else if !active {
                // Guest OS blanked the monitor: drop the cached framebuffer
                // so the view shows black instead of freezing on the last
                // captured frame. Keep displaySize / displayInitialized /
                // lastSentDisplayW/H untouched so re-activation with the same
                // resolution doesn't cause any window or tab reshuffling.
                self.renderer?.clear()
            }
        }

        ipcClient.onDisconnect = { [weak self] in
            guard let self = self else { return }
            self.audioPlayer.stop()
            self.connected = false
            self.connecting = false
            self.displayInitialized = false
            self.failPendingGuestExecCommands(ConsoleCommandError("VM runtime disconnected"))
        }

        setupClipboardCallbacks()
    }

    private func setupClipboardCallbacks() {
        ipcClient.onClipboardData = { [weak self] dataType, payload in
            guard let self = self else { return }
            if let mime = Self.dataTypeToMime(dataType) {
                self.clipboardHandler?.setGuestClipboard(data: payload, mimeType: mime)
            }
        }

        ipcClient.onClipboardGrab = { [weak ipcClient] types in
            guard let client = ipcClient else { return }
            for t in types {
                client.sendClipboardRequest(dataType: t)
            }
        }

        ipcClient.onClipboardRequest = { [weak self] dataType in
            guard let self = self else { return }
            let pasteboard = NSPasteboard.general
            var data: Data?
            switch dataType {
            case 1:
                if let str = pasteboard.string(forType: .string) {
                    data = str.data(using: .utf8)
                }
            case 2:
                data = pasteboard.data(forType: .png)
            case 3:
                data = pasteboard.data(forType: .init("com.microsoft.bmp"))
            default:
                break
            }
            if let data = data {
                self.ipcClient.sendClipboardData(dataType: dataType, payload: data)
            }
        }
    }

    static func mimeToDataType(_ mime: String) -> UInt32 {
        switch mime {
        case "text/plain", "text/plain;charset=utf-8", "UTF8_STRING": return 1
        case "image/png": return 2
        case "image/bmp": return 3
        default: return 0
        }
    }

    static func dataTypeToMime(_ dataType: UInt32) -> String? {
        switch dataType {
        case 1: return "text/plain;charset=utf-8"
        case 2: return "image/png"
        case 3: return "image/bmp"
        default: return nil
        }
    }

    func connectIfNeeded() {
        guard !connected, !connecting else { return }
        connecting = true
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            guard self.bridge.waitForRuntimeConnection(vmId: self.vmId, timeout: 30) else {
                DispatchQueue.main.async { self.connecting = false }
                return
            }
            let fd = self.bridge.takeAcceptedFd(vmId: self.vmId)
            guard fd >= 0 else {
                DispatchQueue.main.async { self.connecting = false }
                return
            }

            DispatchQueue.main.async {
                if self.ipcClient.attach(fd: fd) {
                    self.connected = true
                    self.audioPlayer.start()
                    if !self.displayInitialized {
                        self.activeTab = 1
                    }
                }
                self.connecting = false
            }
        }
    }

    func resendDisplaySize() {
        guard connected, displayViewSize.width > 0, displayViewSize.height > 0 else { return }
        let backingScale = NSScreen.main?.backingScaleFactor ?? 2.0
        let effectiveScale = backingScale / CGFloat(displayScale)
        var w = UInt32(displayViewSize.width * effectiveScale)
        let h = UInt32(displayViewSize.height * effectiveScale)
        w = (w + 7) & ~7
        guard w > 0 && h > 0 else { return }
        lastSentDisplayW = w
        lastSentDisplayH = h
        print("[VmSession] resendDisplaySize \(w)x\(h) (displayScale=\(displayScale))")
        ipcClient.sendDisplaySetSize(width: w, height: h)
    }

    func disconnect() {
        audioPlayer.stop()
        ipcClient.disconnect()
        connected = false
        connecting = false
    }

    func sendConsoleInput(_ text: String) {
        ipcClient.sendConsoleInput(text)
    }

    func runGuestAgentCommand(_ command: String, timeout: TimeInterval = 120,
                              completion: @escaping (Result<ConsoleCommandResult, Error>) -> Void) {
        DispatchQueue.main.async {
            guard self.connected, self.ipcClient.isConnected else {
                completion(.failure(ConsoleCommandError("VM runtime is not connected")))
                return
            }
            guard self.guestAgentConnected else {
                completion(.failure(ConsoleCommandError("Guest agent is not connected")))
                return
            }

            let requestId = self.nextGuestExecRequestId
            self.nextGuestExecRequestId += 1
            let timeoutMs = UInt32(min(max(timeout * 1000, 1000), 600000))
            let timeoutWorkItem = DispatchWorkItem { [weak self] in
                guard let self = self else { return }
                if let pending = self.pendingGuestExecCommands.removeValue(forKey: requestId) {
                    pending.completion(.failure(ConsoleCommandError("Command timed out")))
                }
            }

            self.pendingGuestExecCommands[requestId] = PendingGuestExecCommand(
                completion: completion,
                timeoutWorkItem: timeoutWorkItem
            )
            DispatchQueue.main.asyncAfter(deadline: .now() + timeout, execute: timeoutWorkItem)

            if !self.ipcClient.sendGuestExec(command: command, user: "tenbox", requestId: requestId, timeoutMs: timeoutMs) {
                timeoutWorkItem.cancel()
                self.pendingGuestExecCommands.removeValue(forKey: requestId)
                completion(.failure(ConsoleCommandError("Failed to send guest agent command")))
            }
        }
    }

    func runShellCommand(_ command: String, timeout: TimeInterval = 120,
                         completion: @escaping (Result<ConsoleCommandResult, Error>) -> Void) {
        DispatchQueue.main.async {
            guard self.connected, self.ipcClient.isConnected else {
                completion(.failure(ConsoleCommandError("VM console is not connected")))
                return
            }

            let token = UUID().uuidString.replacingOccurrences(of: "-", with: "")
            let beginMarker = "__TENBOX_CMD_BEGIN_\(token)__"
            let endPrefix = "__TENBOX_CMD_END_\(token)__:"
            let quotedCommand = Self.shellQuote(command)
            let beginTimeoutWorkItem = DispatchWorkItem { [weak self] in
                guard let self = self else { return }
                guard let pending = self.pendingConsoleCommands[token] else { return }
                if self.consoleText.range(of: pending.beginMarker, options: .backwards) == nil {
                    pending.timeoutWorkItem.cancel()
                    self.pendingConsoleCommands.removeValue(forKey: token)
                    pending.completion(.failure(ConsoleCommandError("VM shell did not start the command")))
                }
            }
            let timeoutWorkItem = DispatchWorkItem { [weak self] in
                guard let self = self else { return }
                if let pending = self.pendingConsoleCommands.removeValue(forKey: token) {
                    pending.beginTimeoutWorkItem.cancel()
                    pending.completion(.failure(ConsoleCommandError("Command timed out")))
                }
            }

            self.pendingConsoleCommands[token] = PendingConsoleCommand(
                beginMarker: beginMarker,
                endPrefix: endPrefix,
                completion: completion,
                beginTimeoutWorkItem: beginTimeoutWorkItem,
                timeoutWorkItem: timeoutWorkItem
            )

            DispatchQueue.main.asyncAfter(deadline: .now() + 12, execute: beginTimeoutWorkItem)
            DispatchQueue.main.asyncAfter(deadline: .now() + timeout, execute: timeoutWorkItem)
            let quotedToken = Self.shellQuote(token)
            let wrapped = "stty -echo 2>/dev/null; __tenbox_token=\(quotedToken); __tenbox_begin=\"__TENBOX_CMD_BEGIN_${__tenbox_token}__\"; __tenbox_end=\"__TENBOX_CMD_END_${__tenbox_token}__:\"; printf '\\n%s\\n' \"$__tenbox_begin\"; /bin/sh -lc \(quotedCommand); rc=$?; printf '\\n%s%s\\n' \"$__tenbox_end\" \"$rc\"; stty echo 2>/dev/null\n"
            self.sendConsoleInput(wrapped)
        }
    }

    private func finishGuestExecCommand(requestId: UInt64, ok: Bool, exitCode: Int32,
                                        stdoutText: String, stderrText: String,
                                        error: String?) {
        guard let pending = pendingGuestExecCommands.removeValue(forKey: requestId) else {
            return
        }
        pending.timeoutWorkItem.cancel()

        let output: String
        if !stdoutText.isEmpty && !stderrText.isEmpty {
            output = stdoutText + "\n" + stderrText
        } else {
            output = stdoutText + stderrText
        }

        if ok {
            pending.completion(.success(ConsoleCommandResult(exitCode: exitCode, output: output)))
        } else {
            let message = error ?? (output.isEmpty ? "Guest agent command failed" : output)
            pending.completion(.failure(ConsoleCommandError(message)))
        }
    }

    private func failPendingGuestExecCommands(_ error: Error) {
        let pending = pendingGuestExecCommands
        pendingGuestExecCommands.removeAll()
        for (_, command) in pending {
            command.timeoutWorkItem.cancel()
            command.completion(.failure(error))
        }
    }

    private func appendConsoleText(_ text: String) {
        consoleText.append(text)
        if consoleText.count > Self.maxConsoleSize {
            let excess = consoleText.count - Self.maxConsoleSize * 3 / 4
            consoleText.removeFirst(excess)
        }
        checkPendingConsoleCommands()
    }

    private func checkPendingConsoleCommands() {
        for token in Array(pendingConsoleCommands.keys) {
            guard let pending = pendingConsoleCommands[token],
                  let endRange = consoleText.range(of: pending.endPrefix, options: .backwards) else {
                continue
            }
            let afterEnd = consoleText[endRange.upperBound...]
            guard let lineEnd = afterEnd.firstIndex(where: { $0 == "\n" }) else { continue }
            let exitText = afterEnd[..<lineEnd].trimmingCharacters(in: .whitespacesAndNewlines)
            guard let exitCode = Int32(exitText) else { continue }

            let beforeEnd = consoleText[..<endRange.lowerBound]
            let output: String
            if let beginRange = beforeEnd.range(of: pending.beginMarker, options: .backwards) {
                output = String(beforeEnd[beginRange.upperBound...])
                    .trimmingCharacters(in: .whitespacesAndNewlines)
            } else {
                output = ""
            }

            pending.timeoutWorkItem.cancel()
            pending.beginTimeoutWorkItem.cancel()
            pendingConsoleCommands.removeValue(forKey: token)
            pending.completion(.success(ConsoleCommandResult(exitCode: exitCode, output: output)))
        }
    }

    private static func shellQuote(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    static func filterAnsi(_ input: String) -> String {
        var result = ""
        result.reserveCapacity(input.count)
        var i = input.startIndex
        while i < input.endIndex {
            let c = input[i]
            if c == "\u{1B}" {
                let next = input.index(after: i)
                if next < input.endIndex && input[next] == "[" {
                    var j = input.index(after: next)
                    while j < input.endIndex {
                        let ch = input[j]
                        if (ch >= "A" && ch <= "Z") || (ch >= "a" && ch <= "z") {
                            j = input.index(after: j)
                            break
                        }
                        j = input.index(after: j)
                    }
                    i = j
                    continue
                }
            }
            if c == "\r" {
                i = input.index(after: i)
                continue
            }
            result.append(c)
            i = input.index(after: i)
        }
        return result
    }
}

struct VmDetailView: View {
    private static let minimumDisplayViewSize = CGSize(width: 320, height: 200)
    // Chrome = everything between the NSWindow frame and the DisplayView
    // (title bar + sidebar + tab bar + padding). Updated whenever
    // GeometryReader in DisplayView reports a fresh, trustworthy size.
    static var chromeExtraW: CGFloat = 207
    static var chromeExtraH: CGFloat = 84

    let vm: VmInfo
    @EnvironmentObject var appState: AppState
    @ObservedObject private var session: VmSession

    init(vm: VmInfo, appState: AppState) {
        self.vm = vm
        self.session = appState.getOrCreateSession(for: vm.id)
    }

    var body: some View {
        VStack(spacing: 0) {
            switch session.activeTab {
            case 0:  InfoView(vm: vm)
            case 1:  ConsoleView(session: session)
            default: DisplayView(session: session)
            }
        }
        .padding(.horizontal)
        .onAppear {
            if vm.state == .running {
                session.connectIfNeeded()
            }
            if session.displayInitialized,
               session.displaySize.width > 0, session.displaySize.height > 0 {
                Self.resizeWindowToFitDisplay(session.displaySize, session: session)
            }
        }
        .onChange(of: vm.state, perform: { [oldState = vm.state] newState in
            if newState == .running && oldState != .running {
                session.connectIfNeeded()
            } else if newState == .stopped || newState == .crashed {
                session.activeTab = 0
            }
        })
        .onChange(of: session.displaySize, perform: { newSize in
            if newSize.width > 0 && newSize.height > 0 {
                Self.resizeWindowToFitDisplay(newSize, session: session)
            }
        })
    }

    private static func resizeWindowToFitDisplay(_ guestSize: CGSize, session: VmSession) {
        guard let window = NSApplication.shared.keyWindow else { return }
        guard let screen = window.screen ?? NSScreen.main else { return }

        let backingScale = screen.backingScaleFactor
        let effectiveScale = backingScale / CGFloat(session.displayScale)
        let pointW = guestSize.width / effectiveScale
        let pointH = guestSize.height / effectiveScale

        let extraW = Self.chromeExtraW
        let extraH = Self.chromeExtraH

        let desiredW = pointW + extraW
        let desiredH = pointH + extraH

        let minDisplayW = min(pointW, Self.minimumDisplayViewSize.width)
        let minDisplayH = min(pointH, Self.minimumDisplayViewSize.height)
        let minFrameW = extraW + minDisplayW
        let minFrameH = extraH + minDisplayH

        window.minSize = NSSize(width: minFrameW, height: minFrameH)

        print("[resizeWindow] guest=\(guestSize.width)x\(guestSize.height) backingScale=\(backingScale) displayScale=\(session.displayScale) pointSize=\(pointW)x\(pointH)")
        print("[resizeWindow] chrome=\(extraW)x\(extraH) desired=\(desiredW)x\(desiredH)")

        let maxFrame = screen.visibleFrame
        let finalW = max(minFrameW, min(desiredW, maxFrame.width))
        let finalH = max(minFrameH, min(desiredH, maxFrame.height))

        let oldFrame = window.frame
        let topY = oldFrame.maxY
        var newX = oldFrame.minX
        var newY = topY - finalH

        newX = max(maxFrame.minX, min(newX, maxFrame.maxX - finalW))
        newY = max(maxFrame.minY, min(newY, maxFrame.maxY - finalH))

        let newFrame = NSRect(x: newX, y: newY, width: finalW, height: finalH)
        print("[resizeWindow] final=\(finalW)x\(finalH)")

        // Suppress the reverse notify: the window resize driven by guest
        // display size should not bounce back as a host→guest set_size.
        session.lastResizeFromVmTime = CACurrentMediaTime()
        window.setFrame(newFrame, display: true, animate: false)
    }
}
