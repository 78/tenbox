import Foundation
import TenBoxBridge

class IpcClientWrapper: ObservableObject {
    private let client = TBIpcClient()
    @Published var isConnected = false

    // Display: (pixels, dirtyW, dirtyH, stride, resourceW, resourceH, dirtyX, dirtyY)
    var onFrame: ((Data, UInt32, UInt32, UInt32, UInt32, UInt32, UInt32, UInt32) -> Void)?
    var onDisplayState: ((Bool, UInt32, UInt32) -> Void)?

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

    func disconnect() {
        client.disconnect()
        isConnected = false
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

    // MARK: - Receive

    private func startReceiveLoop() {
        client.startReceiveLoop(
            frameHandler: { [weak self] pixels, w, h, stride, resW, resH, dirtyX, dirtyY in
                self?.onFrame?(pixels, w, h, stride, resW, resH, dirtyX, dirtyY)
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
            }
        )
    }

    deinit {
        disconnect()
    }
}
