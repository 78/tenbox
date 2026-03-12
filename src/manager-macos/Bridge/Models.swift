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

    /// Lower value = higher in the list.
    var sortPriority: Int {
        switch self {
        case .running:  return 0
        case .starting: return 1
        case .rebooting: return 2
        case .crashed:  return 3
        case .stopped:  return 4
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
    let sharedFolders: [SharedFolder]
    let portForwards: [PortForward]
    let displayScale: Int
}

struct VmCreateConfig {
    let name: String
    let kernelPath: String
    let initrdPath: String
    let diskPath: String
    let memoryMb: Int
    let cpuCount: Int
    let netEnabled: Bool
    let sourceDir: String
}

// MARK: - Image Source Models

struct ImageSource: Codable {
    let name: String
    let url: String
}

struct ImageSourcesResponse: Codable {
    let sources: [ImageSource]
}

struct ImageFile: Codable, Equatable {
    let name: String
    let url: String
    let sha256: String
    let size: UInt64

    enum CodingKeys: String, CodingKey {
        case name, url, sha256, size
    }

    init(name: String, url: String = "", sha256: String = "", size: UInt64 = 0) {
        self.name = name
        self.url = url
        self.sha256 = sha256
        self.size = size
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = try c.decode(String.self, forKey: .name)
        url = try c.decodeIfPresent(String.self, forKey: .url) ?? ""
        sha256 = try c.decodeIfPresent(String.self, forKey: .sha256) ?? ""
        size = try c.decodeIfPresent(UInt64.self, forKey: .size) ?? 0
    }
}

struct ImageEntry: Codable, Identifiable, Equatable {
    let id: String
    let version: String
    let displayName: String
    let description: String
    let minAppVersion: String
    let os: String
    let arch: String
    let platform: String
    let files: [ImageFile]

    var cacheId: String { "\(id)-\(version)" }

    var totalSize: UInt64 {
        files.reduce(0) { $0 + $1.size }
    }

    enum CodingKeys: String, CodingKey {
        case id, version, description, os, arch, platform, files
        case displayName = "name"
        case minAppVersion = "min_app_version"
    }

    init(id: String, version: String = "", displayName: String = "", description: String = "",
         minAppVersion: String = "0.0.0", os: String = "linux", arch: String = "microvm",
         platform: String = "arm64", files: [ImageFile] = []) {
        self.id = id
        self.version = version
        self.displayName = displayName
        self.description = description
        self.minAppVersion = minAppVersion
        self.os = os
        self.arch = arch
        self.platform = platform
        self.files = files
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decode(String.self, forKey: .id)
        version = try c.decodeIfPresent(String.self, forKey: .version) ?? ""
        displayName = try c.decodeIfPresent(String.self, forKey: .displayName) ?? ""
        description = try c.decodeIfPresent(String.self, forKey: .description) ?? ""
        minAppVersion = try c.decodeIfPresent(String.self, forKey: .minAppVersion) ?? "0.0.0"
        os = try c.decodeIfPresent(String.self, forKey: .os) ?? "linux"
        arch = try c.decodeIfPresent(String.self, forKey: .arch) ?? "microvm"
        platform = try c.decodeIfPresent(String.self, forKey: .platform) ?? "x86_64"
        files = try c.decodeIfPresent([ImageFile].self, forKey: .files) ?? []
    }
}

struct ImagesResponse: Codable {
    let images: [ImageEntry]
}
