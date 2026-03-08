import Foundation

enum VmState: String, Codable {
    case stopped
    case starting
    case running
    case rebooting
    case crashed

    var displayName: String {
        switch self {
        case .stopped: return "Stopped"
        case .starting: return "Starting"
        case .running: return "Running"
        case .rebooting: return "Rebooting"
        case .crashed: return "Crashed"
        }
    }
}

struct SharedFolder: Identifiable, Codable, Equatable {
    var id: String { tag }
    let tag: String
    let hostPath: String
    let readonly: Bool
    let bookmark: Data?

    init(tag: String, hostPath: String, readonly: Bool, bookmark: Data? = nil) {
        self.tag = tag
        self.hostPath = hostPath
        self.readonly = readonly
        self.bookmark = bookmark
    }
}

struct PortForward: Identifiable, Codable, Equatable {
    var id: String { "\(hostPort):\(guestPort)" }
    let hostPort: UInt16
    let guestPort: UInt16
}

struct VmInfo: Identifiable, Codable {
    let id: String
    let name: String
    let kernelPath: String
    let initrdPath: String
    let diskPath: String
    let memoryMb: Int
    let cpuCount: Int
    let state: VmState
    let netEnabled: Bool
    let cmdline: String
    let sharedFolders: [SharedFolder]
    let portForwards: [PortForward]
}

struct VmCreateConfig {
    let name: String
    let kernelPath: String
    let initrdPath: String
    let diskPath: String
    let memoryMb: Int
    let cpuCount: Int
    let netEnabled: Bool
}
