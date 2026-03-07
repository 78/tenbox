import SwiftUI
import AppKit

@main
struct TenBoxApp: App {
    @StateObject private var appState = AppState()

    init() {
        NSApplication.shared.setActivationPolicy(.regular)
        NSApplication.shared.activate(ignoringOtherApps: true)
        NSApplication.shared.applicationIconImage = Self.makeAppIcon()
    }

    private static func makeAppIcon() -> NSImage {
        let size = NSSize(width: 256, height: 256)
        let image = NSImage(size: size, flipped: false) { rect in
            let ctx = NSGraphicsContext.current!.cgContext

            let cornerRadius: CGFloat = 48
            let bgPath = CGPath(roundedRect: rect.insetBy(dx: 8, dy: 8),
                                cornerWidth: cornerRadius, cornerHeight: cornerRadius,
                                transform: nil)
            ctx.addPath(bgPath)
            ctx.clip()
            let colors = [
                CGColor(red: 0.18, green: 0.45, blue: 0.95, alpha: 1),
                CGColor(red: 0.10, green: 0.28, blue: 0.72, alpha: 1)
            ] as CFArray
            if let gradient = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                                         colors: colors, locations: [0, 1]) {
                ctx.drawLinearGradient(gradient,
                                       start: CGPoint(x: rect.midX, y: rect.maxY),
                                       end: CGPoint(x: rect.midX, y: rect.minY),
                                       options: [])
            }

            let attrs: [NSAttributedString.Key: Any] = [
                .font: NSFont.systemFont(ofSize: 110, weight: .bold),
                .foregroundColor: NSColor.white,
            ]
            let text = NSAttributedString(string: "TB", attributes: attrs)
            let textSize = text.size()
            let textOrigin = CGPoint(x: (rect.width - textSize.width) / 2,
                                     y: (rect.height - textSize.height) / 2)
            text.draw(at: textOrigin)
            return true
        }
        return image
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(appState)
                .frame(minWidth: 800, minHeight: 500)
        }
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("New VM...") {
                    appState.showCreateVmDialog = true
                }
                .keyboardShortcut("n")
            }
        }
    }
}

class AppState: ObservableObject {
    @Published var vms: [VmInfo] = []
    @Published var selectedVmId: String?
    @Published var showCreateVmDialog = false

    private var bridge = TenBoxBridgeWrapper()
    private var activeSessions: [String: WeakRef<VmSession>] = [:]
    private var stateObserver: NSObjectProtocol?

    init() {
        refreshVmList()
        stateObserver = NotificationCenter.default.addObserver(
            forName: NSNotification.Name("TenBoxVmStateChanged"),
            object: nil, queue: .main
        ) { [weak self] note in
            guard let self = self else { return }
            self.refreshVmList()
            if let vmId = note.object as? String,
               let session = self.activeSessions[vmId]?.value {
                let newState = self.vms.first(where: { $0.id == vmId })?.state ?? .stopped
                if newState == .stopped || newState == .crashed {
                    session.disconnect()
                }
            }
        }
    }

    deinit {
        if let obs = stateObserver {
            NotificationCenter.default.removeObserver(obs)
        }
    }

    func registerSession(_ session: VmSession, for vmId: String) {
        activeSessions[vmId] = WeakRef(session)
    }

    func unregisterSession(for vmId: String) {
        activeSessions.removeValue(forKey: vmId)
    }

    func refreshVmList() {
        vms = bridge.getVmList()
    }

    func createVm(config: VmCreateConfig) {
        bridge.createVm(config: config)
        refreshVmList()
    }

    func deleteVm(id: String) {
        bridge.deleteVm(id: id)
        refreshVmList()
    }

    func startVm(id: String) {
        bridge.startVm(id: id)
        refreshVmList()
        // Trigger IPC connection for any active session watching this VM
        if let session = activeSessions[id]?.value {
            session.connectIfNeeded()
        }
    }

    func stopVm(id: String) {
        if let session = activeSessions[id]?.value {
            if session.ipcClient.isConnected {
                session.ipcClient.sendControl("stop")
            }
            session.disconnect()
        }
        bridge.stopVm(id: id)
        refreshVmList()
    }

    func rebootVm(id: String) {
        if let session = activeSessions[id]?.value, session.ipcClient.isConnected {
            session.ipcClient.sendControl("reboot")
        } else {
            bridge.rebootVm(id: id)
        }
    }

    func shutdownVm(id: String) {
        if let session = activeSessions[id]?.value, session.ipcClient.isConnected {
            session.ipcClient.sendControl("shutdown")
        } else {
            bridge.shutdownVm(id: id)
        }
    }
}

private class WeakRef<T: AnyObject> {
    weak var value: T?
    init(_ value: T) { self.value = value }
}
