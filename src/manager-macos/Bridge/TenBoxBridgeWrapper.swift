import Foundation
import TenBoxBridge

class TenBoxBridgeWrapper {
    let configStore = VmConfigStore()
    let ipcServer = TBIpcServer()
    private(set) lazy var processManager = VmProcessManager(ipcServer: ipcServer, configStore: configStore)

    func getVmList() -> [VmInfo] {
        configStore.listVms().map { (id, config) in
            var config = config
            if config.state == "running" && !ipcServer.isServerActive(forVm: id) {
                config.state = "stopped"
                configStore.updateState(vmId: id, state: "stopped")
            }
            return config.toVmInfo(id: id)
        }
    }

    func createVm(config: VmCreateConfig) {
        configStore.createVm(from: config)
    }

    func editVm(id: String, name: String, memoryMb: Int, cpuCount: Int, netEnabled: Bool, debugMode: Bool) {
        configStore.editVm(id: id, name: name, memoryMb: memoryMb, cpuCount: cpuCount,
                           netEnabled: netEnabled, debugMode: debugMode)
    }

    @discardableResult
    func cloneVm(id: String) -> String? {
        configStore.cloneVm(id: id)
    }

    func deleteVm(id: String) {
        configStore.deleteVm(id: id)
    }

    @discardableResult
    func startVm(id: String) -> Bool {
        guard let config = configStore.readConfig(vmId: id) else { return false }
        let vmDir = configStore.vmDirectory(for: id)
        let socketPath = TBIpcServer.socketPath(forVm: id)

        guard ipcServer.listen(forVm: id, socketPath: socketPath) else { return false }

        let success = processManager.launchRuntime(
            vmId: id, config: config, vmDir: vmDir,
            socketPath: socketPath) { [weak self] vmId in
                guard let self = self else { return }
                self.startVm(id: vmId)
                NotificationCenter.default.post(
                    name: Notification.Name("TenBoxVmStateChanged"),
                    object: vmId)
            }

        guard success else {
            ipcServer.closeResources(forVm: id)
            return false
        }

        configStore.updateState(vmId: id, state: "running")
        return true
    }

    func stopVm(id: String) {
        ipcServer.sendControlCommand("stop", toVm: id)
        processManager.terminateProcess(vmId: id)
        ipcServer.closeResources(forVm: id)
        configStore.updateState(vmId: id, state: "stopped")
    }

    func rebootVm(id: String) {
        ipcServer.sendControlCommand("reboot", toVm: id)
    }

    func shutdownVm(id: String) {
        ipcServer.sendControlCommand("shutdown", toVm: id)
    }

    // MARK: - Shared Folders

    func addSharedFolder(_ folder: SharedFolder, toVm vmId: String) -> Bool {
        guard configStore.addSharedFolder(folder, toVm: vmId) else { return false }
        sendSharedFoldersIpcUpdate(vmId: vmId)
        return true
    }

    func removeSharedFolder(tag: String, fromVm vmId: String) -> Bool {
        guard configStore.removeSharedFolder(tag: tag, fromVm: vmId) else { return false }
        sendSharedFoldersIpcUpdate(vmId: vmId)
        return true
    }

    func setSharedFolders(_ folders: [SharedFolder], forVm vmId: String) -> Bool {
        guard configStore.setSharedFolders(folders, forVm: vmId) else { return false }
        sendSharedFoldersIpcUpdate(vmId: vmId)
        return true
    }

    private func sendSharedFoldersIpcUpdate(vmId: String) {
        guard let config = configStore.readConfig(vmId: vmId) else { return }
        let entries = config.sharedFolders.map { sf in
            "\(sf.tag)|\(sf.hostPath)|\(sf.readonly ? "1" : "0")"
        }
        ipcServer.sendSharedFoldersUpdate(entries, toVm: vmId)
    }

    // MARK: - Port Forwards

    func addPortForward(_ pf: PortForward, toVm vmId: String) -> Bool {
        configStore.addPortForward(pf, toVm: vmId)
    }

    func removePortForward(hostPort: UInt16, fromVm vmId: String) -> Bool {
        configStore.removePortForward(hostPort: hostPort, fromVm: vmId)
    }

    // MARK: - Guest Forwards

    func addGuestForward(_ gf: GuestForward, toVm vmId: String) -> Bool {
        configStore.addGuestForward(gf, toVm: vmId)
    }

    func removeGuestForward(guestIp: String, guestPort: UInt16, fromVm vmId: String) -> Bool {
        configStore.removeGuestForward(guestIp: guestIp, guestPort: guestPort, fromVm: vmId)
    }

    // MARK: - Display Scale

    func setDisplayScale(_ scale: Int, forVm vmId: String) -> Bool {
        configStore.setDisplayScale(vmId: vmId, scale: scale)
    }

    // MARK: - Stop All

    func stopAllVms() {
        let runningIds = processManager.runningVmIds()
        for vmId in runningIds {
            ipcServer.sendControlCommand("stop", toVm: vmId)
        }
        processManager.terminateAll()
        ipcServer.closeAllResources()
    }

    // MARK: - IPC Connection

    func waitForRuntimeConnection(vmId: String, timeout: TimeInterval = 30) -> Bool {
        ipcServer.wait(forConnection: vmId, timeout: timeout)
    }

    func takeAcceptedFd(vmId: String) -> Int32 {
        ipcServer.takeAcceptedFd(forVm: vmId)
    }
}
