// swift-tools-version: 5.7
import PackageDescription

let package = Package(
    name: "AgentSphereManager",
    platforms: [.macOS(.v12)],
    products: [
        .executable(name: "AgentSphereManager", targets: ["AgentSphereManager"])
    ],
    dependencies: [
        .package(url: "https://github.com/sparkle-project/Sparkle", from: "2.6.0")
    ],
    targets: [
        .executableTarget(
            name: "AgentSphereManager",
            dependencies: [
                "AgentSphereBridge",
                .product(name: "Sparkle", package: "Sparkle"),
            ],
            path: ".",
            exclude: [
                "Bridge/include", "Bridge/Sources",
                "Bridge/AgentSphere-Bridging-Header.h",
                "Resources/Shaders.metal",
                "Resources/AgentSphere.entitlements",
                "Resources/Info.plist",
                "Package.swift",
            ],
            sources: [
                "AgentSphereApp.swift",
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
                "Bridge/AgentSphereBridgeWrapper.swift",
                "Bridge/IpcClientWrapper.swift",
                "Bridge/VmConfigStore.swift",
                "Bridge/SettingsStore.swift",
                "Bridge/VmProcessManager.swift",
                "Services/ImageSourceService.swift",
                "Services/LlmProxyService.swift",
                "Services/OidcService.swift",
                "Views/LlmProxyView.swift",
                "Views/LoginView.swift",
                "Views/SettingsView.swift",
            ],
            resources: [
                .copy("Resources/icon.png")
            ]
        ),
        .target(
            name: "AgentSphereBridge",
            path: "Bridge",
            exclude: [
                "IpcClientWrapper.swift", "Models.swift",
                "AgentSphereBridgeWrapper.swift", "VmConfigStore.swift",
                "VmProcessManager.swift", "AgentSphere-Bridging-Header.h",
            ],
            sources: [
                "Sources/AgentSphereBridge.mm", "Sources/AgentSphereIPC.mm",
                "Sources/ipc/unix_socket.cpp", "Sources/ipc/protocol_v1.cpp",
                "Sources/ipc/shared_framebuffer_posix.cpp",
            ],
            publicHeadersPath: "include",
            cxxSettings: [
                .headerSearchPath("Sources")
            ]
        ),
    ],
    cxxLanguageStandard: .cxx20
)
