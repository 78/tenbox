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

struct HostForward: Identifiable, Codable, Equatable {
    var id: String { "\(effectiveHostIp):\(hostPort):\(guestPort)" }
    var name: String = ""
    let hostPort: UInt16
    let guestPort: UInt16
    var hostIp: String = "127.0.0.1"
    var guestIp: String = ""

    var effectiveHostIp: String { hostIp.isEmpty ? "127.0.0.1" : hostIp }
    var effectiveGuestIp: String { guestIp.isEmpty ? "" : guestIp }
}

struct GuestForward: Identifiable, Codable, Equatable {
    var id: String { "\(guestIp):\(guestPort)" }
    var name: String = ""
    let guestIp: String
    let guestPort: UInt16
    var hostAddr: String = "127.0.0.1"
    let hostPort: UInt16

    var effectiveHostAddr: String { hostAddr.isEmpty ? "127.0.0.1" : hostAddr }
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
    let hostForwards: [HostForward]
    let guestForwards: [GuestForward]
    let displayScale: Int
    let debugMode: Bool
}

// MARK: - LLM Proxy Models

enum LlmApiType: String, Codable, CaseIterable, Identifiable {
    case openaiCompletions = "openai_completions"

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .openaiCompletions: return "OpenAI Completions"
        }
    }
}

struct LlmModelMapping: Identifiable, Codable, Equatable {
    var id: String { alias }
    var alias: String
    var targetUrl: String
    var apiKey: String
    var model: String
    var apiType: LlmApiType = .openaiCompletions
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
    let debugMode: Bool
}

// MARK: - Disk Config Models (config.json persistence)

struct SharedFolderConfig: Codable, Equatable {
    var tag: String
    var hostPath: String
    var readonly: Bool
    var bookmarkBase64: String?

    enum CodingKeys: String, CodingKey {
        case tag
        case hostPath = "host_path"
        case readonly
        case bookmarkBase64 = "bookmark_base64"
    }

    init(tag: String, hostPath: String, readonly: Bool, bookmarkBase64: String? = nil) {
        self.tag = tag
        self.hostPath = hostPath
        self.readonly = readonly
        self.bookmarkBase64 = bookmarkBase64
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        tag = try c.decodeIfPresent(String.self, forKey: .tag) ?? ""
        hostPath = try c.decodeIfPresent(String.self, forKey: .hostPath) ?? ""
        readonly = try c.decodeIfPresent(Bool.self, forKey: .readonly) ?? false
        bookmarkBase64 = try c.decodeIfPresent(String.self, forKey: .bookmarkBase64)
    }

    func toSharedFolder() -> SharedFolder {
        let bookmark = bookmarkBase64.flatMap { Data(base64Encoded: $0) }
        return SharedFolder(tag: tag, hostPath: hostPath, readonly: readonly, bookmark: bookmark)
    }

    static func from(_ folder: SharedFolder) -> SharedFolderConfig {
        SharedFolderConfig(
            tag: folder.tag,
            hostPath: folder.hostPath,
            readonly: folder.readonly,
            bookmarkBase64: folder.bookmark?.base64EncodedString()
        )
    }
}

struct HostForwardConfig: Codable, Equatable {
    var hostPort: UInt16
    var guestPort: UInt16
    var hostIp: String?
    var guestIp: String?
    var lan: Bool?
    var name: String?

    enum CodingKeys: String, CodingKey {
        case hostPort = "host_port"
        case guestPort = "guest_port"
        case hostIp = "host_ip"
        case guestIp = "guest_ip"
        case lan
        case name
    }

    func toHostForward() -> HostForward {
        let resolvedHostIp: String
        if let hip = hostIp {
            resolvedHostIp = hip
        } else if lan == true {
            resolvedHostIp = "0.0.0.0"
        } else {
            resolvedHostIp = "127.0.0.1"
        }
        return HostForward(name: name ?? "", hostPort: hostPort, guestPort: guestPort,
                           hostIp: resolvedHostIp, guestIp: guestIp ?? "")
    }

    static func from(_ pf: HostForward) -> HostForwardConfig {
        var cfg = HostForwardConfig(hostPort: pf.hostPort, guestPort: pf.guestPort)
        if !pf.hostIp.isEmpty && pf.hostIp != "127.0.0.1" {
            cfg.hostIp = pf.hostIp
        }
        if !pf.guestIp.isEmpty {
            cfg.guestIp = pf.guestIp
        }
        if !pf.name.isEmpty {
            cfg.name = pf.name
        }
        return cfg
    }
}

struct GuestForwardConfig: Codable, Equatable {
    var guestIp: String
    var guestPort: UInt16
    var hostPort: UInt16
    var hostAddr: String?
    var name: String?

    enum CodingKeys: String, CodingKey {
        case guestIp = "guest_ip"
        case guestPort = "guest_port"
        case hostPort = "host_port"
        case hostAddr = "host_addr"
        case name
    }

    func toGuestForward() -> GuestForward {
        GuestForward(name: name ?? "", guestIp: guestIp, guestPort: guestPort,
                     hostAddr: hostAddr ?? "127.0.0.1", hostPort: hostPort)
    }

    static func from(_ gf: GuestForward) -> GuestForwardConfig {
        var cfg = GuestForwardConfig(guestIp: gf.guestIp, guestPort: gf.guestPort, hostPort: gf.hostPort)
        if !gf.hostAddr.isEmpty && gf.hostAddr != "127.0.0.1" {
            cfg.hostAddr = gf.hostAddr
        }
        if !gf.name.isEmpty {
            cfg.name = gf.name
        }
        return cfg
    }
}

struct VmConfig: Codable {
    var name: String
    var kernelPath: String
    var initrdPath: String
    var diskPath: String
    var memoryMb: Int
    var cpuCount: Int
    var state: String
    var netEnabled: Bool
    var debugMode: Bool
    var displayScale: Int
    var sharedFolders: [SharedFolderConfig]
    var hostForwards: [HostForwardConfig]
    var guestForwards: [GuestForwardConfig]

    enum CodingKeys: String, CodingKey {
        case name, state
        case kernelPath = "kernel_path"
        case initrdPath = "initrd_path"
        case diskPath = "disk_path"
        case memoryMb = "memory_mb"
        case cpuCount = "cpu_count"
        case netEnabled = "net_enabled"
        case debugMode = "debug_mode"
        case displayScale = "display_scale"
        case sharedFolders = "shared_folders"
        case hostForwards = "host_forwards"
        case guestForwards = "guest_forwards"
    }

    // Legacy key accepted on decode for backward compatibility with pre-rename
    // config.json files (which used `port_forwards` for the H->G list).
    private struct LegacyCodingKeys: CodingKey {
        var stringValue: String
        var intValue: Int? { nil }
        init(stringValue: String) { self.stringValue = stringValue }
        init?(intValue: Int) { return nil }
        static let portForwards = LegacyCodingKeys(stringValue: "port_forwards")
    }

    init(name: String = "", kernelPath: String = "", initrdPath: String = "",
         diskPath: String = "", memoryMb: Int = 512, cpuCount: Int = 2,
         state: String = "stopped", netEnabled: Bool = false, debugMode: Bool = false,
         displayScale: Int = 1, sharedFolders: [SharedFolderConfig] = [],
         hostForwards: [HostForwardConfig] = [], guestForwards: [GuestForwardConfig] = []) {
        self.name = name
        self.kernelPath = kernelPath
        self.initrdPath = initrdPath
        self.diskPath = diskPath
        self.memoryMb = memoryMb
        self.cpuCount = cpuCount
        self.state = state
        self.netEnabled = netEnabled
        self.debugMode = debugMode
        self.displayScale = displayScale
        self.sharedFolders = sharedFolders
        self.hostForwards = hostForwards
        self.guestForwards = guestForwards
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = try c.decodeIfPresent(String.self, forKey: .name) ?? ""
        kernelPath = try c.decodeIfPresent(String.self, forKey: .kernelPath) ?? ""
        initrdPath = try c.decodeIfPresent(String.self, forKey: .initrdPath) ?? ""
        diskPath = try c.decodeIfPresent(String.self, forKey: .diskPath) ?? ""
        memoryMb = try c.decodeIfPresent(Int.self, forKey: .memoryMb) ?? 512
        cpuCount = try c.decodeIfPresent(Int.self, forKey: .cpuCount) ?? 2
        state = try c.decodeIfPresent(String.self, forKey: .state) ?? "stopped"
        netEnabled = try c.decodeIfPresent(Bool.self, forKey: .netEnabled) ?? false
        debugMode = try c.decodeIfPresent(Bool.self, forKey: .debugMode) ?? false
        displayScale = try c.decodeIfPresent(Int.self, forKey: .displayScale) ?? 1
        sharedFolders = try c.decodeIfPresent([SharedFolderConfig].self, forKey: .sharedFolders) ?? []
        // Prefer the new `host_forwards` key, falling back to the legacy
        // `port_forwards` so pre-rename config.json files still load.
        if let modern = try c.decodeIfPresent([HostForwardConfig].self, forKey: .hostForwards) {
            hostForwards = modern
        } else {
            let legacy = try decoder.container(keyedBy: LegacyCodingKeys.self)
            hostForwards = (try legacy.decodeIfPresent([HostForwardConfig].self, forKey: .portForwards)) ?? []
        }
        guestForwards = try c.decodeIfPresent([GuestForwardConfig].self, forKey: .guestForwards) ?? []
    }

    func toVmInfo(id: String) -> VmInfo {
        VmInfo(
            id: id,
            name: name,
            kernelPath: kernelPath,
            initrdPath: initrdPath,
            diskPath: diskPath,
            memoryMb: memoryMb,
            cpuCount: cpuCount,
            state: VmState(rawValue: state) ?? .stopped,
            netEnabled: netEnabled,
            sharedFolders: sharedFolders.compactMap { $0.tag.isEmpty ? nil : $0.toSharedFolder() },
            hostForwards: hostForwards.map { $0.toHostForward() },
            guestForwards: guestForwards.map { $0.toGuestForward() },
            displayScale: max(1, min(2, displayScale)),
            debugMode: debugMode
        )
    }
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
