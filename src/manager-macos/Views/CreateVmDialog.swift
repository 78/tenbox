import SwiftUI

private let hostMaxCpus = ProcessInfo.processInfo.activeProcessorCount
private let hostMaxMemoryGb = max(1, Int(ProcessInfo.processInfo.physicalMemory / (1024 * 1024 * 1024)))

// MARK: - Create VM Dialog (Wizard)

struct CreateVmDialog: View {
    @EnvironmentObject var appState: AppState
    @Environment(\.dismiss) private var dismiss
    @StateObject private var vm = CreateVmViewModel()

    var body: some View {
        VStack(spacing: 0) {
            switch vm.page {
            case .selectImage:
                SelectImagePage(vm: vm, dismiss: dismiss)
            case .downloading:
                DownloadingPage(vm: vm)
            case .confirm:
                ConfirmPage(vm: vm, appState: appState, dismiss: dismiss)
            }
        }
        .frame(width: 560, height: 500)
        .onAppear {
            vm.loadCachedImages()
            vm.fetchSources()
        }
    }
}

// MARK: - Pages

private struct SelectImagePage: View {
    @ObservedObject var vm: CreateVmViewModel
    let dismiss: DismissAction

    var body: some View {
        VStack(spacing: 0) {
            Text("Create New VM")
                .font(.title2)
                .fontWeight(.semibold)
                .padding(.top, 16)
                .padding(.bottom, 8)

            HStack {
                Text("Source:")
                    .frame(width: 56, alignment: .trailing)
                Picker("", selection: $vm.selectedSourceIndex) {
                    if vm.sources.isEmpty {
                        Text("Loading...").tag(-1)
                    }
                    ForEach(Array(vm.sources.enumerated()), id: \.offset) { i, src in
                        Text(src.name).tag(i)
                    }
                }
                .labelsHidden()
                .disabled(vm.sources.isEmpty)
                .onChange(of: vm.selectedSourceIndex, perform: { _ in
                    vm.onSourceChanged()
                })
                Button {
                    vm.refreshOnlineImages()
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .disabled(vm.isLoadingOnline || vm.sources.isEmpty)
            }
            .padding(.horizontal, 16)
            .padding(.bottom, 8)

            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0, pinnedViews: []) {
                    if !vm.cachedImages.isEmpty {
                        Section {
                            ForEach(vm.cachedImages, id: \.cacheId) { img in
                                let tag = img.cacheId + "||cached"
                                SelectableImageRow(image: img, isCached: true, isSelected: vm.selectedImageId == tag) {
                                    vm.selectedImageId = tag
                                }
                                .id(tag)
                            }
                        } header: {
                            Text("Cached")
                                .font(.headline)
                                .foregroundStyle(.secondary)
                                .padding(.horizontal, 8)
                                .padding(.vertical, 6)
                        }
                    }
                    Section {
                        if vm.isLoadingSources || vm.isLoadingOnline {
                            HStack {
                                ProgressView()
                                    .scaleEffect(0.7)
                                Text("Loading...")
                                    .foregroundStyle(.secondary)
                            }
                            .padding(.horizontal, 8)
                            .padding(.vertical, 6)
                        }
                        ForEach(vm.filteredOnlineImages, id: \.cacheId) { img in
                            let tag = img.cacheId + "||online"
                            SelectableImageRow(image: img, isCached: false, isSelected: vm.selectedImageId == tag) {
                                vm.selectedImageId = tag
                            }
                            .id(tag)
                        }
                    } header: {
                        Text("Online")
                            .font(.headline)
                            .foregroundStyle(.secondary)
                            .padding(.horizontal, 8)
                            .padding(.vertical, 6)
                    }
                }
            }
            .background(Color(nsColor: .controlBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .overlay(
                RoundedRectangle(cornerRadius: 6)
                    .stroke(Color(nsColor: .separatorColor), lineWidth: 1)
            )
            .padding(.horizontal, 16)

            if !vm.errorMessage.isEmpty {
                Text(vm.errorMessage)
                    .font(.caption)
                    .foregroundStyle(.red)
                    .lineLimit(2)
                    .padding(.horizontal, 20)
                    .padding(.bottom, 4)
            }

            Divider()

            HStack {
                Button("Delete Cache") {
                    vm.deleteSelectedCache()
                }
                .disabled(!vm.canDeleteCache)

                Spacer()

                Button("Local Image...") {
                    vm.browseLocalImage()
                }

                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)

                Button("Next") {
                    vm.goNext()
                }
                .keyboardShortcut(.defaultAction)
                .disabled(vm.selectedImageId == nil)
            }
            .padding(16)
        }
    }
}

private struct DownloadingPage: View {
    @ObservedObject var vm: CreateVmViewModel

    var body: some View {
        VStack(spacing: 16) {
            Spacer()

            Text("Downloading Image")
                .font(.title2)
                .fontWeight(.semibold)

            ProgressView(value: vm.downloadProgress, total: 1.0)
                .progressViewStyle(.linear)
                .padding(.horizontal, 40)

            Text(vm.downloadStatusText)
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 40)

            Spacer()

            Divider()
            HStack {
                Spacer()
                Button("Cancel") {
                    vm.cancelDownload()
                }
                .keyboardShortcut(.cancelAction)
            }
            .padding(16)
        }
    }
}

private struct ConfirmPage: View {
    @ObservedObject var vm: CreateVmViewModel
    let appState: AppState
    let dismiss: DismissAction

    private let labelWidth: CGFloat = 72

    var body: some View {
        VStack(spacing: 0) {
            Text("Create New VM")
                .font(.title2)
                .fontWeight(.semibold)
                .padding()

            VStack(alignment: .leading, spacing: 16) {
                VmFormSection(title: "General") {
                    VmFormRow(label: "Name", labelWidth: labelWidth) {
                        TextField("", text: $vm.vmName)
                            .textFieldStyle(.roundedBorder)
                    }
                    VmFormRow(label: "CPUs", labelWidth: labelWidth) {
                        VmSliderRow(value: $vm.cpuCount, range: 1...hostMaxCpus)
                    }
                    VmFormRow(label: "Memory", labelWidth: labelWidth) {
                        VmSliderRow(value: $vm.memoryGb, range: 1...hostMaxMemoryGb, unit: "GB")
                    }
                }

                if let img = vm.selectedImage {
                    VmFormSection(title: "Image") {
                        VmFormRow(label: "Image", labelWidth: labelWidth) {
                            Text(img.displayName)
                                .foregroundStyle(.secondary)
                        }
                    }
                }
            }
            .padding(.horizontal, 24)

            Spacer(minLength: 8)

            if !vm.errorMessage.isEmpty {
                Text(vm.errorMessage)
                    .font(.caption)
                    .foregroundStyle(.red)
                    .padding(.horizontal, 20)
            }

            Divider()

            HStack {
                Button("Back") {
                    vm.goBack()
                }
                Spacer()
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("Create") {
                    vm.createVm(appState: appState)
                    if vm.created {
                        dismiss()
                    }
                }
                .keyboardShortcut(.defaultAction)
                .disabled(vm.vmName.isEmpty)
            }
            .padding(16)
        }
    }
}

// MARK: - Form helpers

private struct VmFormSection<Content: View>: View {
    let title: String
    @ViewBuilder let content: () -> Content

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title)
                .font(.headline)
                .foregroundStyle(.secondary)
            content()
        }
    }
}

private struct VmFormRow<Content: View>: View {
    let label: String
    let labelWidth: CGFloat
    @ViewBuilder let content: () -> Content

    var body: some View {
        HStack(alignment: .center, spacing: 8) {
            Text(label)
                .frame(width: labelWidth, alignment: .trailing)
            content()
        }
    }
}

private struct VmSliderRow: View {
    @Binding var value: Int
    let range: ClosedRange<Int>
    var unit: String = ""

    var body: some View {
        HStack(spacing: 8) {
            Slider(
                value: Binding<Double>(
                    get: { Double(value) },
                    set: { value = Int($0.rounded()) }
                ),
                in: Double(range.lowerBound)...Double(range.upperBound),
                step: 1
            )
            Text(unit.isEmpty ? "\(value)" : "\(value) \(unit)")
                .monospacedDigit()
                .frame(width: 48, alignment: .trailing)
        }
    }
}

// MARK: - Image row

private struct ImageRow: View {
    let image: ImageEntry
    let isCached: Bool

    var body: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(image.displayName)
                    .lineLimit(1)
                if !image.description.isEmpty {
                    Text(image.description)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }
            Spacer()
            if image.totalSize > 0 {
                Text(formatSize(image.totalSize))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }
}

private struct SelectableImageRow: View {
    let image: ImageEntry
    let isCached: Bool
    let isSelected: Bool
    let onSelect: () -> Void

    var body: some View {
        Button(action: onSelect) {
            ImageRow(image: image, isCached: isCached)
                .padding(.horizontal, 8)
                .padding(.vertical, 6)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(isSelected ? Color.accentColor.opacity(0.2) : Color.clear)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}

// MARK: - ViewModel

enum CreateVmPage {
    case selectImage
    case downloading
    case confirm
}

@MainActor
class CreateVmViewModel: ObservableObject {
    @Published var page: CreateVmPage = .selectImage
    @Published var sources: [ImageSource] = []
    @Published var selectedSourceIndex: Int = -1
    @Published var isLoadingSources = false
    @Published var isLoadingOnline = false
    @Published var cachedImages: [ImageEntry] = []
    @Published var onlineImages: [ImageEntry] = []
    @Published var selectedImageId: String?
    @Published var errorMessage = ""

    // Download state
    @Published var downloadProgress: Double = 0
    @Published var downloadStatusText = ""
    private var downloadCancelled = false
    private var downloadTask: Task<Void, Never>?
    private var speedLastBytes: UInt64 = 0
    private var speedLastTime: CFAbsoluteTime = 0
    private var speedBytesPerSec: Double = 0

    // Confirm state
    @Published var selectedImage: ImageEntry?
    @Published var isLocalImage = false
    @Published var localImageDir = ""
    @Published var vmName = ""
    @Published var memoryGb: Int = min(4, hostMaxMemoryGb)
    @Published var cpuCount: Int = min(4, hostMaxCpus)
    @Published var created = false

    private let service = ImageSourceService.shared

    var filteredOnlineImages: [ImageEntry] {
        let cachedIds = Set(cachedImages.map { "\($0.id)-\($0.version)" })
        return onlineImages.filter { !cachedIds.contains("\($0.id)-\($0.version)") }
    }

    var resolvedSelectedImage: ImageEntry? {
        guard let selectedId = selectedImageId else { return nil }
        let parts = selectedId.components(separatedBy: "||")
        guard parts.count == 2 else { return nil }
        let cacheId = parts[0]
        let source = parts[1]
        if source == "cached" {
            return cachedImages.first { $0.cacheId == cacheId }
        } else {
            return onlineImages.first { $0.cacheId == cacheId }
                ?? cachedImages.first { $0.cacheId == cacheId }
        }
    }

    var canDeleteCache: Bool {
        guard let selectedId = selectedImageId else { return false }
        return selectedId.hasSuffix("||cached")
    }

    // MARK: - Actions

    func loadCachedImages() {
        cachedImages = service.getCachedImages()
    }

    func fetchSources() {
        guard !isLoadingSources else { return }
        isLoadingSources = true
        errorMessage = ""

        let fetched = service.effectiveSources()
        sources = fetched
        if !fetched.isEmpty && selectedSourceIndex < 0 {
            var defaultIndex = 0
            if let last = service.lastSelectedSource {
                if let idx = fetched.firstIndex(where: { $0.name == last }) {
                    defaultIndex = idx
                }
            }
            selectedSourceIndex = defaultIndex
            fetchOnlineImages()
        }
        isLoadingSources = false
    }

    func onSourceChanged() {
        onlineImages = []
        selectedImageId = nil
        errorMessage = ""
        if selectedSourceIndex >= 0, selectedSourceIndex < sources.count {
            service.lastSelectedSource = sources[selectedSourceIndex].name
        }
        fetchOnlineImages()
    }

    func refreshOnlineImages() {
        onlineImages = []
        selectedImageId = nil
        errorMessage = ""
        fetchOnlineImages()
    }

    private func fetchOnlineImages() {
        guard selectedSourceIndex >= 0, selectedSourceIndex < sources.count else { return }
        guard !isLoadingOnline else { return }
        isLoadingOnline = true
        errorMessage = ""

        let url = sources[selectedSourceIndex].url
        Task {
            do {
                let images = try await service.fetchImages(from: url)
                let appVersion = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "99.99.99"
                onlineImages = service.filterImages(images, appVersion: appVersion)
            } catch {
                errorMessage = "Failed to load images: \(error.localizedDescription)"
            }
            isLoadingOnline = false
        }
    }

    func deleteSelectedCache() {
        guard let img = resolvedSelectedImage else { return }
        do {
            try service.deleteImageCache(for: img)
            loadCachedImages()
            selectedImageId = nil
        } catch {
            errorMessage = "Delete failed: \(error.localizedDescription)"
        }
    }

    func browseLocalImage() {
        let panel = NSOpenPanel()
        panel.title = "Select Image Directory"
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }

        let dirPath = url.path
        let fm = FileManager.default
        var kernel = "", initrd = "", disk = ""

        if let items = try? fm.contentsOfDirectory(atPath: dirPath) {
            for name in items {
                let fullPath = (dirPath as NSString).appendingPathComponent(name)
                var isDir: ObjCBool = false
                guard fm.fileExists(atPath: fullPath, isDirectory: &isDir), !isDir.boolValue else { continue }

                if name == "vmlinuz" || name.hasPrefix("vmlinuz") || name.hasPrefix("Image") {
                    if kernel.isEmpty { kernel = name }
                } else if name.hasPrefix("initrd") || name.hasPrefix("initramfs") || name.hasSuffix(".cpio.gz") {
                    if initrd.isEmpty { initrd = name }
                } else if name.hasSuffix(".qcow2") {
                    if disk.isEmpty { disk = name }
                }
            }
        }

        if disk.isEmpty && kernel.isEmpty {
            errorMessage = "No valid image files found (vmlinuz or .qcow2)"
            return
        }

        var files: [ImageFile] = []
        if !kernel.isEmpty { files.append(ImageFile(name: kernel)) }
        if !initrd.isEmpty { files.append(ImageFile(name: initrd)) }
        if !disk.isEmpty { files.append(ImageFile(name: disk)) }

        let dirName = url.lastPathComponent
        let localEntry = ImageEntry(id: dirName, displayName: dirName, files: files)
        selectedImage = localEntry
        isLocalImage = true
        localImageDir = dirPath
        vmName = nextVmName(for: dirName)
        page = .confirm
    }

    func goNext() {
        guard let img = resolvedSelectedImage else { return }
        selectedImage = img
        isLocalImage = false
        localImageDir = ""
        vmName = nextVmName(for: img.id)

        if service.isImageCached(img) {
            page = .confirm
        } else {
            startDownload(img)
        }
    }

    func goBack() {
        page = .selectImage
        isLocalImage = false
        localImageDir = ""
        errorMessage = ""
    }

    // MARK: - Download

    private func startDownload(_ entry: ImageEntry) {
        page = .downloading
        downloadCancelled = false
        downloadProgress = 0
        downloadStatusText = "Preparing..."
        errorMessage = ""

        speedLastBytes = 0
        speedLastTime = CFAbsoluteTimeGetCurrent()
        speedBytesPerSec = 0

        downloadTask = Task {
            do {
                try await service.downloadImage(entry, progress: { [weak self] fileIdx, totalFiles, fileName, downloaded, total in
                    Task { @MainActor in
                        guard let self = self else { return }
                        let fileProgress = total > 0 ? Double(downloaded) / Double(total) : 0
                        self.downloadProgress = fileProgress

                        let now = CFAbsoluteTimeGetCurrent()
                        let dt = now - self.speedLastTime
                        if dt >= 0.5 {
                            let db = Double(downloaded) - Double(self.speedLastBytes)
                            if db > 0 {
                                let instantSpeed = db / dt
                                self.speedBytesPerSec = self.speedBytesPerSec > 0
                                    ? self.speedBytesPerSec * 0.3 + instantSpeed * 0.7
                                    : instantSpeed
                            }
                            self.speedLastBytes = downloaded
                            self.speedLastTime = now
                        }

                        var text = "File \(fileIdx + 1)/\(totalFiles): \(fileName)"
                        text += "\n\(formatSize(downloaded)) / \(formatSize(total))"
                        if self.speedBytesPerSec > 0 {
                            text += "  ·  \(formatSize(UInt64(self.speedBytesPerSec)))/s"
                            let remaining = total > downloaded ? Double(total - downloaded) / self.speedBytesPerSec : 0
                            text += "  ·  \(formatDuration(remaining))"
                        }
                        self.downloadStatusText = text
                    }
                }, isCancelled: { [weak self] in
                    self?.downloadCancelled ?? true
                })

                loadCachedImages()
                page = .confirm
            } catch {
                if !downloadCancelled {
                    errorMessage = error.localizedDescription
                }
                page = .selectImage
            }
        }
    }

    func cancelDownload() {
        downloadCancelled = true
        downloadTask?.cancel()
        page = .selectImage
    }

    // MARK: - Create VM

    func createVm(appState: AppState) {
        guard let img = selectedImage else { return }
        errorMessage = ""

        let sourceDir: String
        if isLocalImage {
            sourceDir = localImageDir
        } else {
            sourceDir = service.imageCacheDir(for: img)
        }

        var kernelPath = "", initrdPath = "", diskPath = ""
        for file in img.files {
            let path = (sourceDir as NSString).appendingPathComponent(file.name)
            if file.name == "vmlinuz" || file.name.hasPrefix("vmlinuz") || file.name.hasPrefix("Image") {
                kernelPath = path
            } else if file.name.hasPrefix("initrd") || file.name.hasPrefix("initramfs") {
                initrdPath = path
            } else if file.name.hasSuffix(".qcow2") || file.name.contains("rootfs") {
                diskPath = path
            }
        }

        let config = VmCreateConfig(
            name: vmName,
            kernelPath: kernelPath,
            initrdPath: initrdPath,
            diskPath: diskPath,
            memoryMb: memoryGb * 1024,
            cpuCount: cpuCount,
            netEnabled: true,
            sourceDir: sourceDir
        )
        appState.createVm(config: config)
        created = true
    }

    // MARK: - Helpers

    private func nextVmName(for imageId: String) -> String {
        let prefix = imageId + "-"
        var maxN = 0
        for vm in (try? ImageSourceService.shared.existingVmNames()) ?? [] {
            if vm.hasPrefix(prefix), let n = Int(vm.dropFirst(prefix.count)) {
                maxN = max(maxN, n)
            }
        }
        return prefix + "\(maxN + 1)"
    }
}

// MARK: - Helpers

private func formatSize(_ bytes: UInt64) -> String {
    let units = ["B", "KB", "MB", "GB", "TB"]
    var value = Double(bytes)
    var unit = 0
    while value >= 1024 && unit < units.count - 1 {
        value /= 1024
        unit += 1
    }
    if unit == 0 {
        return "\(bytes) \(units[0])"
    }
    return String(format: "%.1f %@", value, units[unit])
}

private func formatDuration(_ seconds: Double) -> String {
    let s = Int(seconds)
    if s < 60 { return "\(s)s" }
    if s < 3600 { return "\(s / 60)m \(s % 60)s" }
    return "\(s / 3600)h \((s % 3600) / 60)m"
}

// MARK: - Edit VM Dialog (unchanged)

struct EditVmDialog: View {
    @EnvironmentObject var appState: AppState
    @Environment(\.dismiss) private var dismiss

    let vm: VmInfo

    @State private var name: String
    @State private var memoryGb: Int
    @State private var cpuCount: Int

    private let labelWidth: CGFloat = 72

    init(vm: VmInfo) {
        self.vm = vm
        _name = State(initialValue: vm.name)
        _memoryGb = State(initialValue: max(1, vm.memoryMb / 1024))
        _cpuCount = State(initialValue: vm.cpuCount)
    }

    var body: some View {
        VStack(spacing: 0) {
            Text("Edit VM")
                .font(.title2)
                .fontWeight(.semibold)
                .padding()

            VStack(alignment: .leading, spacing: 16) {
                VmFormSection(title: "General") {
                    VmFormRow(label: "Name", labelWidth: labelWidth) {
                        TextField("", text: $name)
                            .textFieldStyle(.roundedBorder)
                    }
                    VmFormRow(label: "CPUs", labelWidth: labelWidth) {
                        VmSliderRow(value: $cpuCount, range: 1...hostMaxCpus)
                    }
                    VmFormRow(label: "Memory", labelWidth: labelWidth) {
                        VmSliderRow(value: $memoryGb, range: 1...hostMaxMemoryGb, unit: "GB")
                    }
                }
            }
            .padding(.horizontal, 24)

            Spacer(minLength: 8)

            Divider()

            HStack {
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Spacer()
                Button("Save") { saveVm() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(name.isEmpty)
            }
            .padding(16)
        }
        .frame(width: 450, height: 280)
    }

    private func saveVm() {
        appState.editVm(
            id: vm.id,
            name: name,
            memoryMb: memoryGb * 1024,
            cpuCount: cpuCount,
            netEnabled: true
        )
        dismiss()
    }
}
