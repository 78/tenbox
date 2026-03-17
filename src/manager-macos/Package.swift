// swift-tools-version: 5.7
import PackageDescription

let package = Package(
    name: "TenBoxManager",
    platforms: [.macOS(.v12)],
    products: [
        .executable(name: "TenBoxManager", targets: ["TenBoxManager"]),
    ],
    dependencies: [
        .package(url: "https://github.com/sparkle-project/Sparkle", from: "2.6.0"),
    ],
    targets: [
        .executableTarget(
            name: "TenBoxManager",
            dependencies: [
                "TenBoxBridge",
                .product(name: "Sparkle", package: "Sparkle"),
            ],
            path: ".",
            exclude: ["Bridge/include", "Bridge/Sources",
                       "Bridge/TenBox-Bridging-Header.h",
                       "Resources/Shaders.metal",
                       "Resources/TenBox.entitlements",
                       "Resources/Info.plist",
                       "Package.swift"],
            sources: [
                "TenBoxApp.swift",
                "Views/ContentView.swift",
                "Views/VmListView.swift",
                "Views/VmDetailView.swift",
                "Views/InfoView.swift",
                "Views/ConsoleView.swift",
                "Views/DisplayView.swift",
                "Views/MetalDisplayView.swift",
                "Views/CreateVmDialog.swift",
                "Audio/CoreAudioPlayer.swift",
                "Input/InputHandler.swift",
                "Input/KeyboardCaptureManager.swift",
                "Clipboard/ClipboardHandler.swift",
                "Bridge/Models.swift",
                "Bridge/TenBoxBridgeWrapper.swift",
                "Bridge/IpcClientWrapper.swift",
                "Services/ImageSourceService.swift",
                "Services/LlmProxyService.swift",
                "Views/LlmProxyView.swift",
            ],
            resources: [
                .copy("Resources/icon.png"),
            ]
        ),
        .target(
            name: "TenBoxBridge",
            path: "Bridge",
            exclude: ["IpcClientWrapper.swift", "Models.swift",
                       "TenBoxBridgeWrapper.swift", "TenBox-Bridging-Header.h"],
            sources: ["Sources/TenBoxBridge.mm", "Sources/TenBoxIPC.mm",
                       "Sources/ipc/unix_socket.cpp", "Sources/ipc/protocol_v1.cpp",
                       "Sources/ipc/shared_framebuffer_posix.cpp"],
            publicHeadersPath: "include",
            cxxSettings: [
                .headerSearchPath("Sources"),
            ]
        ),
    ],
    cxxLanguageStandard: .cxx20
)
