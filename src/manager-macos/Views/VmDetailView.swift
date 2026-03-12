import SwiftUI
import MetalKit

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

    private let bridge = TenBoxBridgeWrapper()
    private var connecting = false
    private static let maxConsoleSize = 32 * 1024

    init(vmId: String) {
        self.vmId = vmId
        setupCallbacks()
    }

    private func setupCallbacks() {
        ipcClient.onConsole = { [weak self] text in
            guard let self = self else { return }
            self.appendConsoleText(Self.filterAnsi(text))
        }
        ipcClient.onRuntimeState = { [weak self] state in
            self?.runtimeState = state
        }
        ipcClient.onGuestAgentState = { [weak self] conn in
            self?.guestAgentConnected = conn
        }

        ipcClient.onFrame = { [weak self] pixels, w, h, stride, resW, resH, dirtyX, dirtyY in
            guard let self = self, let renderer = self.renderer else { return }
            pixels.withUnsafeBytes { ptr in
                renderer.blitDirtyRect(
                    pixels: ptr.baseAddress!,
                    dirtyX: Int(dirtyX),
                    dirtyY: Int(dirtyY),
                    dirtyWidth: Int(w),
                    dirtyHeight: Int(h),
                    srcStride: Int(stride),
                    resourceWidth: Int(resW),
                    resourceHeight: Int(resH)
                )
            }
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
            }
        }

        ipcClient.onDisconnect = { [weak self] in
            guard let self = self else { return }
            self.audioPlayer.stop()
            self.connected = false
            self.connecting = false
            self.displayInitialized = false
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

    private func appendConsoleText(_ text: String) {
        consoleText.append(text)
        if consoleText.count > Self.maxConsoleSize {
            let excess = consoleText.count - Self.maxConsoleSize * 3 / 4
            consoleText.removeFirst(excess)
        }
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
