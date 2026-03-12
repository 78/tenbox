import Foundation
import TenBoxBridge

class TenBoxBridgeWrapper {
    private let bridge = TBBridge()

    func getVmList() -> [VmInfo] {
        let objcList = bridge.getVmList()
        return objcList.map { info in
            let folders = info.sharedFolders.map { sf in
                SharedFolder(tag: sf.tag, hostPath: sf.hostPath, readonly: sf.readonly_, bookmark: sf.bookmark)
            }
            let pfs = info.portForwards.map { pf in
                PortForward(hostPort: pf.hostPort, guestPort: pf.guestPort)
            }
            return VmInfo(
                id: info.vmId,
                name: info.name,
                kernelPath: info.kernelPath,
                initrdPath: info.initrdPath,
                diskPath: info.diskPath,
                memoryMb: Int(info.memoryMb),
                cpuCount: Int(info.cpuCount),
                state: VmState(rawValue: info.state) ?? .stopped,
                netEnabled: info.netEnabled,
                sharedFolders: folders,
                portForwards: pfs,
                displayScale: max(1, min(2, Int(info.displayScale)))
            )
        }
    }

    func createVm(config: VmCreateConfig) {
        let objcConfig = TBVmCreateConfig()
        objcConfig.name = config.name
        objcConfig.kernelPath = config.kernelPath
        objcConfig.initrdPath = config.initrdPath
        objcConfig.diskPath = config.diskPath
        objcConfig.memoryMb = config.memoryMb
        objcConfig.cpuCount = config.cpuCount
        objcConfig.netEnabled = config.netEnabled
        objcConfig.sourceDir = config.sourceDir
        bridge.createVm(with: objcConfig)
    }

    func editVm(id: String, name: String, memoryMb: Int, cpuCount: Int, netEnabled: Bool) {
        bridge.editVm(withId: id, name: name, memoryMb: memoryMb, cpuCount: cpuCount, netEnabled: netEnabled)
    }

    func deleteVm(id: String) {
        bridge.deleteVm(withId: id)
    }

    @discardableResult
    func startVm(id: String) -> Bool {
        return bridge.startVm(withId: id)
    }

    func stopVm(id: String) {
        bridge.stopVm(withId: id)
    }

    func rebootVm(id: String) {
        bridge.rebootVm(withId: id)
    }

    func shutdownVm(id: String) {
        bridge.shutdownVm(withId: id)
    }

    func addSharedFolder(_ folder: SharedFolder, toVm vmId: String) -> Bool {
        let sf = TBSharedFolder()
        sf.tag = folder.tag
        sf.hostPath = folder.hostPath
        sf.readonly_ = folder.readonly
        sf.bookmark = folder.bookmark
        return bridge.add(sf, toVm: vmId)
    }

    func removeSharedFolder(tag: String, fromVm vmId: String) -> Bool {
        return bridge.removeSharedFolder(withTag: tag, fromVm: vmId)
    }

    func setSharedFolders(_ folders: [SharedFolder], forVm vmId: String) -> Bool {
        let objcFolders = folders.map { f -> TBSharedFolder in
            let sf = TBSharedFolder()
            sf.tag = f.tag
            sf.hostPath = f.hostPath
            sf.readonly_ = f.readonly
            sf.bookmark = f.bookmark
            return sf
        }
        return bridge.setSharedFolders(objcFolders, forVm: vmId)
    }

    func addPortForward(_ pf: PortForward, toVm vmId: String) -> Bool {
        let objcPf = TBPortForward()
        objcPf.hostPort = pf.hostPort
        objcPf.guestPort = pf.guestPort
        return bridge.add(objcPf, toVm: vmId)
    }

    func removePortForward(hostPort: UInt16, fromVm vmId: String) -> Bool {
        return bridge.removePortForward(withHostPort: hostPort, fromVm: vmId)
    }

    func setDisplayScale(_ scale: Int, forVm vmId: String) -> Bool {
        return bridge.setDisplayScale(scale, forVm: vmId)
    }

    func stopAllVms() {
        bridge.stopAllVms()
    }

    func waitForRuntimeConnection(vmId: String, timeout: TimeInterval = 30) -> Bool {
        return bridge.wait(forRuntimeConnection: vmId, timeout: timeout)
    }

    func takeAcceptedFd(vmId: String) -> Int32 {
        return bridge.takeAcceptedFd(forVm: vmId)
    }
}
