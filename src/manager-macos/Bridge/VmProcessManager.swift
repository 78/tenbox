import Foundation
import AgentSphereBridge

class VmProcessManager {

    private let ipcServer: TBIpcServer
    private let configStore: VmConfigStore
    private var processes: [String: Process] = [:]
    private var scopedUrls: [String: [URL]] = [:]
    private let lock = NSLock()

    init(ipcServer: TBIpcServer, configStore: VmConfigStore) {
        self.ipcServer = ipcServer
        self.configStore = configStore
    }

    // MARK: - Launch

    func launchRuntime(vmId: String, config: VmConfig, vmDir: URL,
                       socketPath: String, onReboot: @escaping (String) -> Void) -> Bool {
        guard !config.kernelPath.isEmpty else { return false }

        var args = ["--kernel", config.kernelPath,
                    "--vm-id", vmId,
                    "--interactive", "off"]

        if !config.diskPath.isEmpty {
            args += ["--disk", config.diskPath]
        }
        if !config.initrdPath.isEmpty {
            args += ["--initrd", config.initrdPath]
        }
        args += ["--memory", "\(config.memoryMb)"]
        args += ["--cpus", "\(config.cpuCount)"]

        if config.netEnabled { args.append("--net") }
        if config.debugMode { args.append("--debug") }

        var scopedUrlsForVm: [URL] = []
        for sf in config.sharedFolders {
            guard !sf.tag.isEmpty, !sf.hostPath.isEmpty else { continue }
            var hostPath = sf.hostPath

            if let bmBase64 = sf.bookmarkBase64, !bmBase64.isEmpty,
               let bmData = Data(base64Encoded: bmBase64) {
                var stale = false
                if let url = try? URL(resolvingBookmarkData: bmData,
                                      options: .withSecurityScope,
                                      bookmarkDataIsStale: &stale) {
                    if url.startAccessingSecurityScopedResource() {
                        scopedUrlsForVm.append(url)
                        hostPath = url.path
                    }
                }
            }

            let arg = sf.readonly ? "\(sf.tag):\(hostPath):ro" : "\(sf.tag):\(hostPath)"
            args += ["--share", arg]
        }

        args += ["--control-endpoint", socketPath]

        let runtimePath = Self.findRuntimeBinary()
        guard FileManager.default.fileExists(atPath: runtimePath) else {
            NSLog("Runtime binary not found")
            for url in scopedUrlsForVm { url.stopAccessingSecurityScopedResource() }
            return false
        }

        let process = Process()
        process.executableURL = URL(fileURLWithPath: runtimePath)
        process.arguments = args
        process.currentDirectoryURL = vmDir

        NSLog("Launch runtime with arguments: %@", args)

        do {
            try process.run()
        } catch {
            NSLog("Failed to launch runtime: %@", error.localizedDescription)
            for url in scopedUrlsForVm { url.stopAccessingSecurityScopedResource() }
            ipcServer.closeResources(forVm: vmId)
            return false
        }

        lock.lock()
        processes[vmId] = process
        if !scopedUrlsForVm.isEmpty {
            scopedUrls[vmId] = scopedUrlsForVm
        }
        lock.unlock()

        let capturedVmId = vmId
        let capturedConfigStore = configStore
        let capturedIpcServer = ipcServer

        process.terminationHandler = { [weak self] proc in
            let wantsReboot = proc.terminationStatus == 128

            self?.cleanupProcess(vmId: capturedVmId)
            capturedIpcServer.closeResources(forVm: capturedVmId)

            if wantsReboot {
                NSLog("VM %@ exited with code 128 (reboot), restarting...", capturedVmId)
                capturedConfigStore.updateState(vmId: capturedVmId, state: "rebooting")
                DispatchQueue.main.async {
                    NotificationCenter.default.post(
                        name: Notification.Name("AgentSphereVmStateChanged"),
                        object: capturedVmId)
                }
                DispatchQueue.main.async {
                    onReboot(capturedVmId)
                }
                return
            }

            let newState = proc.terminationStatus == 0 ? "stopped" : "crashed"
            capturedConfigStore.updateState(vmId: capturedVmId, state: newState)
            DispatchQueue.main.async {
                NotificationCenter.default.post(
                    name: Notification.Name("AgentSphereVmStateChanged"),
                    object: capturedVmId)
            }
        }

        return true
    }

    // MARK: - Terminate

    func terminateProcess(vmId: String) {
        lock.lock()
        let process = processes[vmId]
        lock.unlock()

        if let process = process, process.isRunning {
            process.terminate()
        }
    }

    func terminateAll() {
        lock.lock()
        let allProcesses = processes
        lock.unlock()

        for (_, process) in allProcesses {
            if process.isRunning {
                process.terminate()
                process.waitUntilExit()
            }
        }

        lock.lock()
        processes.removeAll()
        let allUrls = scopedUrls
        scopedUrls.removeAll()
        lock.unlock()

        for (_, urls) in allUrls {
            for url in urls { url.stopAccessingSecurityScopedResource() }
        }
    }

    /// IDs of VMs that currently have a running process.
    func runningVmIds() -> [String] {
        lock.lock()
        defer { lock.unlock() }
        return Array(processes.keys)
    }

    // MARK: - Internal

    private func cleanupProcess(vmId: String) {
        lock.lock()
        processes.removeValue(forKey: vmId)
        let urls = scopedUrls.removeValue(forKey: vmId)
        lock.unlock()

        if let urls = urls {
            for url in urls { url.stopAccessingSecurityScopedResource() }
        }
    }

    private static func findRuntimeBinary() -> String {
        let fm = FileManager.default
        if let exePath = Bundle.main.executablePath {
            let exeDir = (exePath as NSString).deletingLastPathComponent
            let beside = (exeDir as NSString).appendingPathComponent("agentsphere-vm-runtime")
            if fm.fileExists(atPath: beside) { return beside }

            var searchDir = (exeDir as NSString).deletingLastPathComponent
            while (searchDir as NSString).pathComponents.count > 1 {
                let candidate = (searchDir as NSString).appendingPathComponent("build/agentsphere-vm-runtime")
                if fm.fileExists(atPath: candidate) { return candidate }
                searchDir = (searchDir as NSString).deletingLastPathComponent
            }
        }
        return "agentsphere-vm-runtime"
    }
}
