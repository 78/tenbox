import Foundation
import CryptoKit

private let kLastSelectedSourceKey = "lastSelectedSource"

class ImageSourceService: ObservableObject {
    static let shared = ImageSourceService()

    private let fm = FileManager.default

    // MARK: - Directory helpers

    var imagesDir: String {
        let dir = appSupportDir + "/images"
        try? fm.createDirectory(atPath: dir, withIntermediateDirectories: true)
        return dir
    }

    private var appSupportDir: String {
        let paths = NSSearchPathForDirectoriesInDomains(.applicationSupportDirectory, .userDomainMask, true)
        let dir = (paths.first ?? NSHomeDirectory() + "/Library/Application Support") + "/TenBox"
        try? fm.createDirectory(atPath: dir, withIntermediateDirectories: true)
        return dir
    }

    private var settingsPath: String {
        appSupportDir + "/settings.json"
    }

    // MARK: - Default & effective sources

    static func defaultSources() -> [ImageSource] {
        [
            ImageSource(name: "China Mainland", url: "https://tenbox.ai/api/images.json"),
        ]
    }

    func effectiveSources() -> [ImageSource] {
        let configured = loadSourcesFromSettings()
        return configured.isEmpty ? Self.defaultSources() : configured
    }

    private func loadSourcesFromSettings() -> [ImageSource] {
        guard let data = fm.contents(atPath: settingsPath),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let sourcesArray = json["sources"] as? [[String: Any]] else {
            return []
        }
        var result: [ImageSource] = []
        for item in sourcesArray {
            guard let name = item["name"] as? String,
                  let url = item["url"] as? String,
                  !url.isEmpty else { continue }
            result.append(ImageSource(name: name, url: url))
        }
        return result
    }

    // MARK: - Last selected source persistence

    var lastSelectedSource: String? {
        get { UserDefaults.standard.string(forKey: kLastSelectedSourceKey) }
        set { UserDefaults.standard.set(newValue, forKey: kLastSelectedSourceKey) }
    }

    // MARK: - Fetch images

    func fetchImages(from sourceUrl: String) async throws -> [ImageEntry] {
        guard let url = URL(string: sourceUrl) else {
            throw ImageSourceError.invalidUrl
        }
        var request = URLRequest(url: url)
        request.cachePolicy = .reloadIgnoringLocalCacheData
        let (data, _) = try await URLSession.shared.data(for: request)
        let response = try JSONDecoder().decode(ImagesResponse.self, from: data)
        return response.images
    }

    // MARK: - Filtering

    func filterImages(_ images: [ImageEntry], appVersion: String) -> [ImageEntry] {
        let currentPlatform: String = {
            #if arch(arm64)
            return "arm64"
            #else
            return "x86_64"
            #endif
        }()

        return images.filter { img in
            guard img.arch == "microvm" else { return false }
            let imgPlatform = img.platform.isEmpty ? "x86_64" : img.platform
            guard imgPlatform == currentPlatform else { return false }
            guard compareVersions(img.minAppVersion, appVersion) <= 0 else { return false }
            guard !img.id.isEmpty, !img.version.isEmpty, !img.files.isEmpty else { return false }
            return true
        }
    }

    // MARK: - Cache management

    func imageCacheDir(for entry: ImageEntry) -> String {
        imagesDir + "/\(entry.cacheId)"
    }

    func isImageCached(_ entry: ImageEntry) -> Bool {
        let cacheDir = imageCacheDir(for: entry)
        guard fm.fileExists(atPath: cacheDir) else { return false }
        for file in entry.files {
            let filePath = (cacheDir as NSString).appendingPathComponent(file.name)
            guard fm.fileExists(atPath: filePath) else { return false }
            let tmpPath = filePath + ".tmp"
            if fm.fileExists(atPath: tmpPath) { return false }
        }
        return true
    }

    func getCachedImages() -> [ImageEntry] {
        var result: [ImageEntry] = []
        guard fm.fileExists(atPath: imagesDir) else { return result }
        guard let items = try? fm.contentsOfDirectory(atPath: imagesDir) else { return result }

        for item in items {
            let dir = (imagesDir as NSString).appendingPathComponent(item)
            var isDir: ObjCBool = false
            guard fm.fileExists(atPath: dir, isDirectory: &isDir), isDir.boolValue else { continue }
            if let entry = loadImageMeta(from: dir), isImageCached(entry) {
                result.append(entry)
            }
        }
        return result
    }

    func deleteImageCache(for entry: ImageEntry) throws {
        let dir = imageCacheDir(for: entry)
        if fm.fileExists(atPath: dir) {
            try fm.removeItem(atPath: dir)
        }
    }

    // MARK: - Metadata persistence

    func saveImageMeta(_ entry: ImageEntry, to cacheDir: String) {
        try? fm.createDirectory(atPath: cacheDir, withIntermediateDirectories: true)
        let metaPath = (cacheDir as NSString).appendingPathComponent("image.json")
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        if let data = try? encoder.encode(entry) {
            try? data.write(to: URL(fileURLWithPath: metaPath))
        }
    }

    func loadImageMeta(from cacheDir: String) -> ImageEntry? {
        let metaPath = (cacheDir as NSString).appendingPathComponent("image.json")
        guard let data = fm.contents(atPath: metaPath) else { return nil }
        return try? JSONDecoder().decode(ImageEntry.self, from: data)
    }

    // MARK: - Download

    func downloadImage(
        _ entry: ImageEntry,
        progress: @escaping (_ fileIndex: Int, _ totalFiles: Int, _ fileName: String,
                             _ downloaded: UInt64, _ total: UInt64) -> Void,
        isCancelled: @escaping () -> Bool
    ) async throws {
        let cacheDir = imageCacheDir(for: entry)
        try fm.createDirectory(atPath: cacheDir, withIntermediateDirectories: true)

        let totalFiles = entry.files.count
        for (index, file) in entry.files.enumerated() {
            if isCancelled() { throw ImageSourceError.cancelled }

            let destPath = (cacheDir as NSString).appendingPathComponent(file.name)

            if fm.fileExists(atPath: destPath) {
                if !file.sha256.isEmpty {
                    let existingHash = fileSHA256(destPath)
                    if existingHash?.lowercased() == file.sha256.lowercased() {
                        progress(index, totalFiles, file.name, file.size, file.size)
                        continue
                    }
                } else {
                    progress(index, totalFiles, file.name, file.size, file.size)
                    continue
                }
            }

            guard let fileUrl = URL(string: file.url), !file.url.isEmpty else {
                throw ImageSourceError.invalidUrl
            }

            let tmpPath = destPath + ".tmp"
            try? fm.removeItem(atPath: tmpPath)

            try await downloadFile(
                from: fileUrl, to: tmpPath,
                progress: { downloaded, total in
                    progress(index, totalFiles, file.name, downloaded, total)
                },
                isCancelled: isCancelled
            )

            if !file.sha256.isEmpty {
                guard let hash = fileSHA256(tmpPath),
                      hash.lowercased() == file.sha256.lowercased() else {
                    try? fm.removeItem(atPath: tmpPath)
                    throw ImageSourceError.sha256Mismatch(file.name)
                }
            }

            try? fm.removeItem(atPath: destPath)
            try fm.moveItem(atPath: tmpPath, toPath: destPath)
        }

        saveImageMeta(entry, to: cacheDir)
    }

    // MARK: - Private helpers

    private func downloadFile(
        from url: URL, to destPath: String,
        progress: @escaping (UInt64, UInt64) -> Void,
        isCancelled: @escaping () -> Bool
    ) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            let delegate = DownloadCompletionDelegate(
                destPath: destPath,
                progress: progress,
                isCancelled: isCancelled,
                continuation: continuation
            )
            let session = URLSession(configuration: .default, delegate: delegate, delegateQueue: nil)
            delegate.session = session
            session.downloadTask(with: url).resume()
        }
    }

    private func fileSHA256(_ path: String) -> String? {
        guard let handle = FileHandle(forReadingAtPath: path) else { return nil }
        defer { handle.closeFile() }

        var hasher = SHA256()
        while autoreleasepool(invoking: {
            let chunk = handle.readData(ofLength: 65536)
            if chunk.isEmpty { return false }
            hasher.update(data: chunk)
            return true
        }) {}

        let digest = hasher.finalize()
        return digest.map { String(format: "%02x", $0) }.joined()
    }

    func existingVmNames() throws -> [String] {
        let vmsDir = appSupportDir + "/vms"
        guard fm.fileExists(atPath: vmsDir) else { return [] }
        guard let items = try? fm.contentsOfDirectory(atPath: vmsDir) else { return [] }
        var names: [String] = []
        for item in items {
            let configPath = (vmsDir as NSString).appendingPathComponent(item + "/config.json")
            guard let data = fm.contents(atPath: configPath),
                  let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let name = json["name"] as? String else { continue }
            names.append(name)
        }
        return names
    }

    private func compareVersions(_ a: String, _ b: String) -> Int {
        let parse: (String) -> [Int] = { v in
            var parts = v.split(separator: ".").compactMap { Int($0) }
            while parts.count < 3 { parts.append(0) }
            return parts
        }
        let va = parse(a), vb = parse(b)
        for i in 0..<3 {
            if va[i] < vb[i] { return -1 }
            if va[i] > vb[i] { return 1 }
        }
        return 0
    }
}

// MARK: - Download delegate for progress tracking + completion

private class DownloadCompletionDelegate: NSObject, URLSessionDownloadDelegate {
    let destPath: String
    let progressCallback: (UInt64, UInt64) -> Void
    let isCancelled: () -> Bool
    let continuation: CheckedContinuation<Void, Error>
    var session: URLSession?
    private var resumed = false

    init(destPath: String,
         progress: @escaping (UInt64, UInt64) -> Void,
         isCancelled: @escaping () -> Bool,
         continuation: CheckedContinuation<Void, Error>) {
        self.destPath = destPath
        self.progressCallback = progress
        self.isCancelled = isCancelled
        self.continuation = continuation
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                    didWriteData bytesWritten: Int64,
                    totalBytesWritten: Int64,
                    totalBytesExpectedToWrite: Int64) {
        if isCancelled() {
            downloadTask.cancel()
            return
        }
        let total = totalBytesExpectedToWrite > 0 ? UInt64(totalBytesExpectedToWrite) : 0
        progressCallback(UInt64(totalBytesWritten), total)
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask,
                    didFinishDownloadingTo location: URL) {
        guard !resumed else { return }
        resumed = true

        do {
            if let http = downloadTask.response as? HTTPURLResponse, http.statusCode != 200 {
                throw ImageSourceError.httpError(http.statusCode)
            }
            let dest = URL(fileURLWithPath: destPath)
            try? FileManager.default.removeItem(at: dest)
            try FileManager.default.moveItem(at: location, to: dest)
            continuation.resume()
        } catch {
            continuation.resume(throwing: error)
        }
        self.session?.invalidateAndCancel()
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard !resumed else { return }
        resumed = true
        continuation.resume(throwing: error ?? ImageSourceError.downloadFailed("Unknown error"))
        self.session?.invalidateAndCancel()
    }
}

// MARK: - Errors

enum ImageSourceError: LocalizedError {
    case invalidUrl
    case httpError(Int)
    case sha256Mismatch(String)
    case cancelled
    case downloadFailed(String)

    var errorDescription: String? {
        switch self {
        case .invalidUrl: return "Invalid URL"
        case .httpError(let code): return "HTTP error \(code)"
        case .sha256Mismatch(let file): return "SHA256 mismatch: \(file)"
        case .cancelled: return "Download cancelled"
        case .downloadFailed(let msg): return "Download failed: \(msg)"
        }
    }
}
