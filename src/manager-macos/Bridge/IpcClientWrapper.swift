import Foundation
import TenBoxBridge

class IpcClientWrapper: ObservableObject {
    private let client = TBIpcClient()
    @Published var isConnected = false

    // Display: (pixelBytes, pixelLength, dirtyW, dirtyH, stride, resourceW, resourceH, dirtyX, dirtyY)
    var onFrame: ((UnsafeRawPointer, Int, UInt32, UInt32, UInt32, UInt32, UInt32, UInt32, UInt32) -> Void)?
    var onDisplayState: ((Bool, UInt32, UInt32) -> Void)?
    // Cursor: (visible, imageUpdated, width, height, hotX, hotY, pixels?)
    var onCursor: ((Bool, Bool, UInt32, UInt32, UInt32, UInt32, Data?) -> Void)?

    // Audio
    var onAudio: ((Data, UInt32, UInt16) -> Void)?

    // Console
    var onConsole: ((String) -> Void)?

    // Clipboard
    var onClipboardGrab: (([UInt32]) -> Void)?
    var onClipboardData: ((UInt32, Data) -> Void)?
    var onClipboardRequest: ((UInt32) -> Void)?

    // VM state
    var onRuntimeState: ((String) -> Void)?
    var onGuestAgentState: ((Bool) -> Void)?

    // Port forward errors (host ports that failed to bind)
    var onPortForwardError: (([String]) -> Void)?

    // Disconnect (called when the IPC recv loop exits, e.g. runtime crashed)
    var onDisconnect: (() -> Void)?

    func connect(vmId: String) -> Bool {
        let result = client.connect(toVm: vmId)
        if result {
            isConnected = true
            startReceiveLoop()
        }
        return result
    }

    func attach(fd: Int32) -> Bool {
        let result = client.attach(toFd: fd)
        if result {
            isConnected = true
            startReceiveLoop()
        }
        return result
    }

    private var disconnecting = false

    func disconnect() {
        guard !disconnecting else { return }
        disconnecting = true
        isConnected = false
        DispatchQueue.global(qos: .userInitiated).async { [client] in
            client.disconnect()
        }
    }

    // MARK: - Send

    func sendControl(_ command: String) {
        client.sendControlCommand(command)
    }

    func sendKey(code: UInt16, pressed: Bool) {
        client.sendKeyEvent(code, pressed: pressed)
    }

    func sendPointer(x: Int32, y: Int32, buttons: UInt32) {
        client.sendPointerAbsolute(x, y: y, buttons: buttons)
    }

    func sendScroll(delta: Int32) {
        client.sendScrollEvent(delta)
    }

    func sendConsoleInput(_ text: String) {
        client.sendConsoleInput(text)
    }

    func sendDisplaySetSize(width: UInt32, height: UInt32) {
        client.sendDisplaySetSizeWidth(width, height: height)
    }

    func sendClipboardGrab(types: [UInt32]) {
        client.sendClipboardGrab(types.map { NSNumber(value: $0) })
    }

    func sendClipboardData(dataType: UInt32, payload: Data) {
        client.sendClipboardData(dataType, payload: payload)
    }

    func sendClipboardRequest(dataType: UInt32) {
        client.sendClipboardRequest(dataType)
    }

    func sendClipboardRelease() {
        client.sendClipboardRelease()
    }

    func sendSharedFoldersUpdate(entries: [String]) {
        client.sendSharedFoldersUpdate(entries)
    }

    func sendNetworkUpdate(hostfwdEntries: [String], guestfwdEntries: [String], netEnabled: Bool) {
        client.sendNetworkUpdate(hostfwdEntries, guestfwdEntries: guestfwdEntries, netEnabled: netEnabled)
    }

    // MARK: - Receive

    private func startReceiveLoop() {
        client.portForwardErrorHandler = { [weak self] failedPorts in
            self?.onPortForwardError?(failedPorts)
        }
        client.startReceiveLoop(
            frameHandler: { [weak self] pixelBytes, pixelLength, w, h, stride, resW, resH, dirtyX, dirtyY in
                self?.onFrame?(pixelBytes, Int(pixelLength), w, h, stride, resW, resH, dirtyX, dirtyY)
            },
            cursorHandler: { [weak self] visible, imageUpdated, w, h, hotX, hotY, pixels in
                self?.onCursor?(visible, imageUpdated, w, h, hotX, hotY, pixels)
            },
            audioHandler: { [weak self] pcm, rate, channels in
                self?.onAudio?(pcm, rate, channels)
            },
            consoleHandler: { [weak self] text in
                self?.onConsole?(text)
            },
            clipboardGrabHandler: { [weak self] types in
                let uintTypes = types.map { $0.uint32Value }
                self?.onClipboardGrab?(uintTypes)
            },
            clipboardDataHandler: { [weak self] dataType, payload in
                self?.onClipboardData?(dataType, payload)
            },
            clipboardRequestHandler: { [weak self] dataType in
                self?.onClipboardRequest?(dataType)
            },
            runtimeStateHandler: { [weak self] state in
                self?.onRuntimeState?(state)
            },
            guestAgentStateHandler: { [weak self] connected in
                self?.onGuestAgentState?(connected)
            },
            displayStateHandler: { [weak self] active, w, h in
                self?.onDisplayState?(active, w, h)
            },
            disconnectHandler: { [weak self] in
                self?.isConnected = false
                self?.onDisconnect?()
            }
        )
    }

    deinit {
        disconnect()
    }
}
