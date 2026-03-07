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
    @Published var displayAspect: CGFloat = 16.0 / 9.0

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
            let newAspect = CGFloat(resW) / CGFloat(max(resH, 1))
            if abs(self.displayAspect - newAspect) > 0.01 {
                DispatchQueue.main.async { self.displayAspect = newAspect }
            }
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
            self.audioPlayer.sampleRate = Double(rate)
            self.audioPlayer.channelCount = UInt32(channels)
            self.audioPlayer.enqueuePcmData(pcm)
        }

        ipcClient.onDisplayState = { [weak self] active, w, h in
            guard let self = self else { return }
            if active && w > 0 && h > 0 {
                self.displayAspect = CGFloat(w) / CGFloat(max(h, 1))
            }
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
                }
                self.connecting = false
            }
        }
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
    let vm: VmInfo
    @EnvironmentObject var appState: AppState
    @StateObject private var session: VmSession

    @State private var selectedTab = 0

    init(vm: VmInfo) {
        self.vm = vm
        _session = StateObject(wrappedValue: VmSession(vmId: vm.id))
    }

    var body: some View {
        TabView(selection: $selectedTab) {
            InfoView(vm: vm)
                .tabItem { Label("Info", systemImage: "info.circle") }
                .tag(0)

            ConsoleView(session: session)
                .tabItem { Label("Console", systemImage: "terminal") }
                .tag(1)

            DisplayView(session: session)
                .tabItem { Label("Display", systemImage: "display") }
                .tag(2)
        }
        .padding()
        .onAppear {
            appState.registerSession(session, for: vm.id)
            if vm.state == .running {
                session.connectIfNeeded()
            }
        }
        .onDisappear {
            appState.unregisterSession(for: vm.id)
        }
        .onChange(of: vm.state) { _, newState in
            if newState == .running {
                session.connectIfNeeded()
            }
        }
    }
}
