import Foundation

/// Centralized settings store that reads/writes `settings.json` and provides
/// computed properties for all data directories (VM storage, image cache, LLM logs).
///
/// Users can override the default VM storage and image cache directories via
/// `vm_storage_dir` and `image_cache_dir` keys in `settings.json`.
final class SettingsStore {
    static let shared = SettingsStore()

    private let queue = DispatchQueue(label: "com.agentsphere.settings-store", qos: .utility)
    private let fm = FileManager.default

    // MARK: - Default base directory

    /// The default Application Support directory for AgentSphere.
    /// `~/Library/Application Support/AgentSphere/`
    private var defaultAppSupportDir: URL {
        let appSupport = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        return appSupport.appendingPathComponent("AgentSphere")
    }

    private var defaultAppSupportPath: String {
        defaultAppSupportDir.path
    }

    // MARK: - Computed directory properties

    /// The effective Application Support directory (always the default — not overridable).
    var appSupportDirectory: URL {
        let dir = defaultAppSupportDir
        try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    /// The effective VM storage directory.
    ///
    /// Returns `vm_storage_dir` from settings.json if set, otherwise
    /// defaults to `<appSupport>/vms/`.
    var vmStorageDirectory: URL {
        if let custom = vmStorageDirOverride, !custom.isEmpty {
            let dir = URL(fileURLWithPath: custom)
            try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
            return dir
        }
        let dir = defaultAppSupportDir.appendingPathComponent("vms")
        try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    /// The effective image cache directory.
    ///
    /// Returns `image_cache_dir` from settings.json if set, otherwise
    /// defaults to `<appSupport>/images/`.
    var imageCacheDirectory: URL {
        if let custom = imageCacheDirOverride, !custom.isEmpty {
            let dir = URL(fileURLWithPath: custom)
            try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
            return dir
        }
        let dir = defaultAppSupportDir.appendingPathComponent("images")
        try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    /// The LLM proxy log directory (always under default appSupport).
    var llmLogDirectory: URL {
        let dir = defaultAppSupportDir.appendingPathComponent("llm_logs")
        try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    /// The path to `settings.json` (always under default appSupport).
    var settingsPath: String {
        let dir = defaultAppSupportPath
        try? fm.createDirectory(atPath: dir, withIntermediateDirectories: true)
        return (dir as NSString).appendingPathComponent("settings.json")
    }

    // MARK: - Overridable settings (mirrored from settings.json)

    /// Custom VM storage directory. `nil` means use default.
    var vmStorageDirOverride: String? {
        get { loadOverrides().vmStorageDir }
        set {
            updateOverrides { overrides in
                overrides.vmStorageDir = newValue
            }
        }
    }

    /// Custom image cache directory. `nil` means use default.
    var imageCacheDirOverride: String? {
        get { loadOverrides().imageCacheDir }
        set {
            updateOverrides { overrides in
                overrides.imageCacheDir = newValue
            }
        }
    }

    // MARK: - Private helpers

    private struct Overrides {
        var vmStorageDir: String?
        var imageCacheDir: String?
    }

    private func loadOverrides() -> Overrides {
        var result = Overrides()
        guard let data = fm.contents(atPath: settingsPath),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return result }

        if let v = json["vm_storage_dir"] as? String, !v.isEmpty {
            result.vmStorageDir = v
        }
        if let v = json["image_cache_dir"] as? String, !v.isEmpty {
            result.imageCacheDir = v
        }
        return result
    }

    private func updateOverrides(_ block: (inout Overrides) -> Void) {
        queue.sync {
            var json: [String: Any] = [:]
            if let data = fm.contents(atPath: settingsPath),
               let existing = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
            {
                json = existing
            }

            var overrides = Overrides(
                vmStorageDir: json["vm_storage_dir"] as? String,
                imageCacheDir: json["image_cache_dir"] as? String
            )
            block(&overrides)

            if let v = overrides.vmStorageDir, !v.isEmpty {
                json["vm_storage_dir"] = v
            } else {
                json.removeValue(forKey: "vm_storage_dir")
            }
            if let v = overrides.imageCacheDir, !v.isEmpty {
                json["image_cache_dir"] = v
            } else {
                json.removeValue(forKey: "image_cache_dir")
            }

            if let data = try? JSONSerialization.data(withJSONObject: json, options: .prettyPrinted) {
                try? data.write(to: URL(fileURLWithPath: settingsPath))
            }
        }
    }

    // MARK: - JSON read/write (for other settings users)

    /// Reads the entire settings.json as a dictionary. Returns empty dict if file doesn't exist.
    func load() -> [String: Any] {
        guard let data = fm.contents(atPath: settingsPath),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return [:] }
        return json
    }

    /// Writes a dictionary to settings.json (merges with existing).
    func save(_ json: [String: Any]) {
        queue.sync {
            if let data = try? JSONSerialization.data(withJSONObject: json, options: .prettyPrinted) {
                try? data.write(to: URL(fileURLWithPath: settingsPath))
            }
        }
    }

    /// Merges new key-value pairs into the existing settings.json.
    func merge(_ updates: [String: Any]) {
        queue.sync {
            var json = load()
            for (key, value) in updates {
                json[key] = value
            }
            if let data = try? JSONSerialization.data(withJSONObject: json, options: .prettyPrinted) {
                try? data.write(to: URL(fileURLWithPath: settingsPath))
            }
        }
    }
}