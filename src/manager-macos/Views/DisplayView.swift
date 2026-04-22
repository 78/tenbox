import SwiftUI
import MetalKit

struct DisplayView: View {
    @ObservedObject var session: VmSession
    @StateObject private var viewModel = DisplayViewModel()

    private static func updateChromeExtra(viewSize: CGSize) {
        guard viewSize.width > 0 && viewSize.height > 0,
              let window = NSApplication.shared.keyWindow else { return }
        let extraW = window.frame.width - viewSize.width
        let extraH = window.frame.height - viewSize.height
        if extraW > 0 && extraH > 0 {
            VmDetailView.chromeExtraW = extraW
            VmDetailView.chromeExtraH = extraH
        }
    }

    var body: some View {
        GeometryReader { geo in
            ZStack {
                MetalDisplayViewWrapper(
                    renderer: session.renderer,
                    inputHandler: viewModel.inputHandler,
                    captureManager: viewModel.keyboardCaptureManager,
                    guestCursor: viewModel.guestCursor
                )
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .onAppear {
                    viewModel.attach(to: session)
                }
                .onDisappear {
                    viewModel.detach()
                }

                if !session.connected {
                    VStack(spacing: 12) {
                        ProgressView()
                            .scaleEffect(1.5)
                        Text("Waiting for display...")
                            .foregroundStyle(.secondary)
                    }
                }

                VStack {
                    if let bannerKind = viewModel.keyboardCaptureBannerKind {
                        KeyboardCaptureBanner(kind: bannerKind)
                    }
                    Spacer()
                }
                .padding(12)
            }
            .onAppear {
                session.displayViewSize = geo.size
                Self.updateChromeExtra(viewSize: geo.size)
            }
            .onChange(of: geo.size, perform: { newSize in
                session.displayViewSize = newSize
                Self.updateChromeExtra(viewSize: newSize)
                if session.displayInitialized {
                    let elapsed = CACurrentMediaTime() - session.lastResizeFromVmTime
                    if elapsed > 1.0 {
                        viewModel.notifyDisplaySizeIfNeeded(newSize, session: session)
                    }
                }
            })
        }
    }
}

class DisplayViewModel: ObservableObject {
    let inputHandler = InputHandler()
    let keyboardCaptureManager = KeyboardCaptureManager()
    @Published var guestCursor: NSCursor?
    @Published var keyboardCaptureBannerKind: KeyboardCaptureBannerKind?

    private var resizeTimer: Timer?
    private weak var session: VmSession?
    private var bannerHideWorkItem: DispatchWorkItem?

    private static var didShowFullCaptureBannerThisRun = false
    private static var didShowLocalOnlyBannerThisRun = false

    func attach(to session: VmSession) {
        self.session = session
        setupInputHandler(client: session.ipcClient)
        setupKeyboardCapture()
        setupCursorHandler(client: session.ipcClient)
    }

    func detach() {
        resizeTimer?.invalidate()
        resizeTimer = nil
        bannerHideWorkItem?.cancel()
        bannerHideWorkItem = nil
        keyboardCaptureBannerKind = nil
        let hadCapture = keyboardCaptureManager.isCaptureActive
        keyboardCaptureManager.endCapture()
        if !hadCapture {
            inputHandler.releaseAllPressedInputs()
        }
        session = nil
    }

    func notifyDisplaySizeIfNeeded(_ size: CGSize, session: VmSession) {
        let backingScale = NSScreen.main?.backingScaleFactor ?? 2.0
        let effectiveScale = backingScale / CGFloat(session.displayScale)
        var w = UInt32(size.width * effectiveScale)
        let h = UInt32(size.height * effectiveScale)
        w = (w + 7) & ~7
        guard w > 0 && h > 0 else { return }

        resizeTimer?.invalidate()
        resizeTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: false) { [weak session] _ in
            guard let session = session else { return }
            let client = session.ipcClient
            guard client.isConnected else {
                print("[DisplayView] resize ignored: client disconnected")
                return
            }
            let elapsed = CACurrentMediaTime() - session.lastResizeFromVmTime
            guard elapsed > 1.0 else { return }
            session.lastSentDisplayW = w
            session.lastSentDisplayH = h
            print("[DisplayView] sending display.set_size \(w)x\(h) (view: \(size.width)x\(size.height), displayScale: \(session.displayScale))")
            client.sendDisplaySetSize(width: w, height: h)
        }
    }

    private func setupInputHandler(client: IpcClientWrapper) {
        inputHandler.onKeyEvent = { [weak client] code, pressed in
            guard let client = client, client.isConnected else { return }
            client.sendKey(code: code, pressed: pressed)
        }

        inputHandler.onPointerEvent = { [weak client] x, y, buttons in
            guard let client = client, client.isConnected else { return }
            client.sendPointer(x: x, y: y, buttons: buttons)
        }

        inputHandler.onWheelEvent = { [weak client] delta in
            guard let client = client, client.isConnected else { return }
            client.sendScroll(delta: delta)
        }
    }

    private func setupKeyboardCapture() {
        keyboardCaptureManager.onKeyDown = { [weak self] keyCode in
            self?.inputHandler.handleKeyDown(keyCode: keyCode)
        }
        keyboardCaptureManager.onKeyUp = { [weak self] keyCode in
            self?.inputHandler.handleKeyUp(keyCode: keyCode)
        }
        keyboardCaptureManager.onFlagsChanged = { [weak self] keyCode, modifierFlags in
            self?.inputHandler.handleFlagsChanged(keyCode: keyCode, modifierFlags: modifierFlags)
        }
        keyboardCaptureManager.onCaptureEnded = { [weak self] in
            self?.inputHandler.releaseAllPressedInputs()
        }
        keyboardCaptureManager.onModeChanged = { [weak self] mode in
            DispatchQueue.main.async {
                self?.updateBanner(for: mode)
            }
        }
    }

    private func updateBanner(for mode: KeyboardCaptureMode) {
        bannerHideWorkItem?.cancel()
        bannerHideWorkItem = nil

        let bannerKind: KeyboardCaptureBannerKind?
        switch mode {
        case .inactive:
            bannerKind = nil
        case .localOnly:
            guard !Self.didShowLocalOnlyBannerThisRun else {
                keyboardCaptureBannerKind = nil
                return
            }
            Self.didShowLocalOnlyBannerThisRun = true
            bannerKind = .localOnly
        case .fullCapture:
            guard !Self.didShowFullCaptureBannerThisRun else {
                keyboardCaptureBannerKind = nil
                return
            }
            Self.didShowFullCaptureBannerThisRun = true
            bannerKind = .fullCapture
        }

        keyboardCaptureBannerKind = bannerKind
        guard bannerKind != nil else { return }

        let workItem = DispatchWorkItem { [weak self] in
            self?.keyboardCaptureBannerKind = nil
            self?.bannerHideWorkItem = nil
        }
        bannerHideWorkItem = workItem
        DispatchQueue.main.asyncAfter(deadline: .now() + 3.0, execute: workItem)
    }

    private func setupCursorHandler(client: IpcClientWrapper) {
        client.onCursor = { [weak self] visible, imageUpdated, w, h, hotX, hotY, pixels in
            guard let self = self else { return }
            if !visible {
                self.guestCursor = NSCursor(image: NSImage(size: NSSize(width: 1, height: 1)),
                                            hotSpot: .zero)
                return
            }
            guard imageUpdated, w > 0, h > 0, let pixels = pixels else { return }
            if let cursor = Self.buildNSCursor(width: w, height: h,
                                               hotX: hotX, hotY: hotY,
                                               pixelData: pixels) {
                self.guestCursor = cursor
            }
        }
    }

    private static func buildNSCursor(width: UInt32, height: UInt32,
                                       hotX: UInt32, hotY: UInt32,
                                       pixelData: Data) -> NSCursor? {
        let w = Int(width)
        let h = Int(height)
        let expectedSize = w * h * 4
        guard pixelData.count >= expectedSize else { return nil }

        let colorSpace = CGColorSpaceCreateDeviceRGB()
        guard let provider = CGDataProvider(data: pixelData as CFData) else { return nil }
        // Pixels from virtio-gpu are B8G8R8A8 in memory (BGRA byte order).
        // On little-endian, a 32-bit word reads as 0xAARRGGBB, so
        // byteOrder32Little + noneSkipFirst gives us [skip][R][G][B] in
        // the 32-bit word = B,G,R,A bytes in memory.
        guard let cgImage = CGImage(
            width: w,
            height: h,
            bitsPerComponent: 8,
            bitsPerPixel: 32,
            bytesPerRow: w * 4,
            space: colorSpace,
            bitmapInfo: CGBitmapInfo(rawValue: CGBitmapInfo.byteOrder32Little.rawValue |
                                    CGImageAlphaInfo.premultipliedFirst.rawValue),
            provider: provider,
            decode: nil,
            shouldInterpolate: false,
            intent: .defaultIntent
        ) else { return nil }

        let nsImage = NSImage(cgImage: cgImage, size: NSSize(width: w, height: h))
        return NSCursor(image: nsImage, hotSpot: NSPoint(x: Int(hotX), y: Int(hotY)))
    }

}

class InputMTKView: MTKView {
    var inputHandler: InputHandler?
    var captureManager: KeyboardCaptureManager?
    var customCursor: NSCursor?

    override var acceptsFirstResponder: Bool { true }

    override func becomeFirstResponder() -> Bool {
        return true
    }

    override func resignFirstResponder() -> Bool {
        releaseCapturedInputs()
        return super.resignFirstResponder()
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        NotificationCenter.default.removeObserver(self)
        if let win = window {
            NotificationCenter.default.addObserver(
                self, selector: #selector(windowDidResignKey),
                name: NSWindow.didResignKeyNotification, object: win)
            NotificationCenter.default.addObserver(
                self, selector: #selector(windowDidBecomeKey),
                name: NSWindow.didBecomeKeyNotification, object: win)
        }
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(appDidResignActive),
            name: NSApplication.didResignActiveNotification,
            object: nil
        )
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(appDidBecomeActive),
            name: NSApplication.didBecomeActiveNotification,
            object: nil
        )
    }

    @objc private func windowDidResignKey(_ note: Notification) {
        releaseCapturedInputs()
    }

    @objc private func windowDidBecomeKey(_ note: Notification) {
        needsDisplay = true
    }

    @objc private func appDidResignActive(_ note: Notification) {
        releaseCapturedInputs()
    }

    @objc private func appDidBecomeActive(_ note: Notification) {
        needsDisplay = true
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        guard self == window?.firstResponder else {
            return super.performKeyEquivalent(with: event)
        }
        if let captureManager, captureManager.isCaptureActive {
            if !captureManager.handlesKeyboardLocally {
                captureManager.demoteToLocalOnlyIfNeeded()
            }
        }
        if event.type == .keyDown {
            inputHandler?.handleKeyDown(event)
        }
        return true
    }

    override func keyDown(with event: NSEvent) {
        if let captureManager, captureManager.isCaptureActive, !captureManager.handlesKeyboardLocally {
            captureManager.demoteToLocalOnlyIfNeeded()
        }
        inputHandler?.handleKeyDown(event)
    }

    override func keyUp(with event: NSEvent) {
        if let captureManager, captureManager.isCaptureActive, !captureManager.handlesKeyboardLocally {
            captureManager.demoteToLocalOnlyIfNeeded()
        }
        inputHandler?.handleKeyUp(event)
    }

    override func flagsChanged(with event: NSEvent) {
        if let captureManager, captureManager.isCaptureActive {
            if captureManager.isReleaseGesture(keyCode: event.keyCode, modifierFlags: event.modifierFlags) {
                releaseCapturedInputs()
                return
            }
            if !captureManager.handlesKeyboardLocally {
                captureManager.demoteToLocalOnlyIfNeeded()
            }
        }
        inputHandler?.handleFlagsChanged(event)
    }

    private func localMousePosition(for event: NSEvent) -> (Int32, Int32)? {
        let loc = convert(event.locationInWindow, from: nil)
        let w = bounds.width
        let h = bounds.height
        guard w > 0 && h > 0 else { return nil }
        let nx = max(0, min(loc.x / w, 1.0))
        let ny = max(0, min(1.0 - loc.y / h, 1.0))
        return (Int32(nx * 32767), Int32(ny * 32767))
    }

    override func mouseDown(with event: NSEvent) {
        captureManager?.beginCapture()
        window?.makeFirstResponder(self)
        guard let (x, y) = localMousePosition(for: event) else { return }
        inputHandler?.handleMouseButton(button: 0, pressed: true, absX: x, absY: y)
    }

    override func mouseUp(with event: NSEvent) {
        guard let (x, y) = localMousePosition(for: event) else { return }
        inputHandler?.handleMouseButton(button: 0, pressed: false, absX: x, absY: y)
    }

    override func rightMouseDown(with event: NSEvent) {
        guard let (x, y) = localMousePosition(for: event) else { return }
        inputHandler?.handleMouseButton(button: 1, pressed: true, absX: x, absY: y)
    }

    override func rightMouseUp(with event: NSEvent) {
        guard let (x, y) = localMousePosition(for: event) else { return }
        inputHandler?.handleMouseButton(button: 1, pressed: false, absX: x, absY: y)
    }

    override func otherMouseDown(with event: NSEvent) {
        guard let (x, y) = localMousePosition(for: event) else { return }
        inputHandler?.handleMouseButton(button: 2, pressed: true, absX: x, absY: y)
    }

    override func otherMouseUp(with event: NSEvent) {
        guard let (x, y) = localMousePosition(for: event) else { return }
        inputHandler?.handleMouseButton(button: 2, pressed: false, absX: x, absY: y)
    }

    override func mouseMoved(with event: NSEvent) {
        guard let (x, y) = localMousePosition(for: event) else { return }
        inputHandler?.handleMouseMoved(absX: x, absY: y)
    }

    override func mouseDragged(with event: NSEvent) {
        guard let (x, y) = localMousePosition(for: event) else { return }
        inputHandler?.handleMouseMoved(absX: x, absY: y)
    }

    override func rightMouseDragged(with event: NSEvent) {
        guard let (x, y) = localMousePosition(for: event) else { return }
        inputHandler?.handleMouseMoved(absX: x, absY: y)
    }

    override func otherMouseDragged(with event: NSEvent) {
        guard let (x, y) = localMousePosition(for: event) else { return }
        inputHandler?.handleMouseMoved(absX: x, absY: y)
    }

    private var lastScrollTime: Double = 0.0
    private static let scrollMinInterval: Double = 0.1
    private static let scrollDivisor: CGFloat = 30.0

    override func scrollWheel(with event: NSEvent) {
        let now = CACurrentMediaTime()
        if now - lastScrollTime < Self.scrollMinInterval { return }

        let raw = event.scrollingDeltaY
        if raw == 0 { return }
        let divided = raw / Self.scrollDivisor
        let delta: Int32 = raw > 0
            ? Int32(max(1.0, divided))
            : Int32(min(-1.0, divided))
        inputHandler?.onWheelEvent?(delta)
        lastScrollTime = now
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        for area in trackingAreas {
            removeTrackingArea(area)
        }
        let area = NSTrackingArea(
            rect: bounds,
            options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect, .mouseEnteredAndExited,
                      .cursorUpdate],
            owner: self,
            userInfo: nil
        )
        addTrackingArea(area)
    }

    override func resetCursorRects() {
        super.resetCursorRects()
        let cursor = customCursor ?? NSCursor.arrow
        addCursorRect(bounds, cursor: cursor)
    }

    func updateCustomCursor(_ cursor: NSCursor?) {
        customCursor = cursor
        window?.invalidateCursorRects(for: self)
    }

    private func releaseCapturedInputs() {
        captureManager?.endCapture()
    }
}

struct MetalDisplayViewWrapper: NSViewRepresentable {
    let renderer: MetalDisplayRenderer?
    let inputHandler: InputHandler?
    let captureManager: KeyboardCaptureManager?
    let guestCursor: NSCursor?

    func makeNSView(context: Context) -> InputMTKView {
        let view = InputMTKView()
        view.inputHandler = inputHandler
        view.captureManager = captureManager
        view.customCursor = guestCursor
        if let renderer = renderer {
            view.device = renderer.device
            view.colorPixelFormat = .bgra8Unorm
            view.isPaused = true
            view.enableSetNeedsDisplay = true
            view.clearColor = MTLClearColorMake(0, 0, 0, 1)
            view.delegate = renderer
            renderer.view = view
        }
        return view
    }

    func updateNSView(_ nsView: InputMTKView, context: Context) {
        nsView.delegate = renderer
        nsView.inputHandler = inputHandler
        nsView.captureManager = captureManager
        renderer?.view = nsView
        if nsView.customCursor !== guestCursor {
            nsView.updateCustomCursor(guestCursor)
        }
    }
}

enum KeyboardCaptureBannerKind {
    case localOnly
    case fullCapture
}

private struct KeyboardCaptureBanner: View {
    let kind: KeyboardCaptureBannerKind

    private var text: Text {
        switch kind {
        case .localOnly:
            return Text("Keyboard capture is limited. Allow ") +
                Text("Input Monitoring").bold() +
                Text(" and ") +
                Text("Accessibility").bold() +
                Text(" to block macOS shortcuts.")
        case .fullCapture:
            return Text("Keyboard captured. Press ") +
                Text("Right Option").bold() +
                Text(" to release.")
        }
    }

    private var isWarning: Bool {
        kind == .localOnly
    }

    var body: some View {
        text
            .font(.caption)
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .background(isWarning ? Color.orange.opacity(0.18) : Color.black.opacity(0.72))
            .foregroundStyle(isWarning ? Color.orange : Color.white)
            .clipShape(Capsule())
            .frame(maxWidth: .infinity, alignment: .top)
    }
}
