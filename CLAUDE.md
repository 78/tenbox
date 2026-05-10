# TenBox Project Guide

## Overview

Cross-platform VMM for running AI agents in isolated Linux VMs.

- **Windows / macOS**: native GUI manager (`tenbox-manager.exe` / `TenBox.app`) launches per-VM `tenbox-vm-runtime` processes.
- **Linux**: `tenboxd` daemon manages VM lifecycle, exposes a local `tenbox` CLI, and provides optional browser-based remote desktop via WebRTC.

All platforms share `src/core/` (VMM engine), `src/platform/` (hypervisor backends), `src/ipc/` (manager↔runtime protocol), and `src/runtime/` (the runtime process). Linux adds `src/daemon/`, `src/cli/`, and `src/client/`.

## Source layout

```
src/
├── common/         Shared types: VmSpec, SharedFolder, ImageSource
├── core/           VMM engine — arch/ device/ disk/ net/ vmm/ vdagent/ guest_agent/
├── platform/       Hypervisor backends — windows/ (WHVP), macos/ (HVF), linux/ (KVM), posix/
├── ipc/            Manager↔runtime protocol v1 + POSIX Unix socket transport
├── runtime/        tenbox-vm-runtime process (all platforms)
├── daemon/         tenboxd (Linux only)
│   ├── main.cpp            Entry point, CLI flags, startup sequence
│   ├── vm_store.cpp        VM registry (vm.json persistence)
│   ├── runtime_manager.cpp VM process supervisor + display/audio/console IPC
│   ├── rpc_server.cpp      Local Unix socket RPC server
│   ├── cloud_tunnel.cpp    Outbound WSS cloud tunnel + message dispatch
│   ├── cloud_protocol.cpp  CloudEnvelope JSON types
│   ├── remote_session.cpp  RemoteSession lifecycle
│   ├── remote_webrtc.cpp   WebRTC signaling + DataChannel setup (libdatachannel)
│   ├── ffmpeg_video_encoder.cpp  H.264 encoding (FFmpeg)
│   ├── opus_audio_encoder.cpp    Opus audio encoding
│   ├── resource_monitor.cpp      Host + VM telemetry
│   ├── host_updater.cpp    apt self-upgrade worker
│   ├── llm_proxy.cpp       OpenAI-compatible HTTP reverse proxy
│   ├── kvm_doctor.cpp      KVM support check
│   └── host_settings.cpp   LLM proxy config persistence
├── cli/            tenbox CLI (src/cli/main.cpp)
├── client/         Local RPC client library (src/client/client.cpp)
├── manager/        Windows GUI (Win32)
└── manager-macos/  macOS GUI (SwiftUI/AppKit)
```

## Common commands

```sh
# Build (Linux / macOS dev)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build

# Build (macOS app bundle)
./scripts/build-macos.sh --release

# Build deb (see packaging/debian/build-deb.sh for the full static build)

# Run tests
ctest --test-dir build

# KVM check
tenbox doctor            # via CLI
tenboxd --doctor         # standalone

# VM lifecycle
tenbox vm create --name my-vm --kernel build/Image --disk build/rootfs.qcow2
tenbox vm start <id>
tenbox vm console <id>
tenbox vm logs <id> -f
tenbox vm stop <id>
tenbox vm ls
```

## Architecture quick reference

```
Linux:    browser/CLI ──► tenboxd (Unix socket / WSS → my.tenbox.ai)
                                  │
                    ┌─────────────┴─────────────┐
                    ▼                           ▼
          tenbox-vm-runtime [KVM]    tenbox-vm-runtime [KVM]

Win/macOS: tenbox-manager ──IPC v1──► tenbox-vm-runtime (WHVP / HVF)
           Named Pipe (Win) / Unix socket (macOS)
```

## Key conventions

- **C++20** throughout. Code comments in English, only where intent is non-obvious.
- **ipc/protocol_v1.h** is the manager↔runtime wire boundary — check compatibility before touching it.
- **Offline-first daemon**: `tenboxd --cloud-url ""` must disable all cloud connectivity without breaking local CLI.
- **LLM proxy** exists in two places: `src/daemon/llm_proxy.cpp` (Linux) and `src/manager/llm_proxy.cpp` (Windows); change both when the protocol changes.
- **RemoteSession** is single-instance per VM. Read `remote_webrtc.cpp`'s `force` takeover path before adding DataChannels.
- **macOS Caps Lock forwarding**: send Caps Lock as a tap (`down` then `up`) on each `flagsChanged` event; AppKit exposes it as a toggle state, but the guest input stack needs a full key press for every switch.
- **Agent data profile packages**: `TenBox.app` exports/imports Hermes/OpenClaw data without image changes by using a temporary shared folder and console-injected standard shell commands. Keep the gzip package format documented in `docs/agent-profile.md` and reject cross-agent imports.
- **Agent data backups**: `TenBox.app` writes host-managed backups to `~/Library/Application Support/TenBox/AgentBackups/<vm-id>/<agent>` and retains the latest 5 packages.
- **Agent health checks**: `TenBox.app` runs health, restart, model test, config reset, and diagnostics through console-injected commands. Repair actions must create a host-managed Agent data backup first.
- **macOS Agent data UI**: `TenBox.app` exposes Agent data export/import from the VM toolbar/menu while a VM is running. It must not depend on preinstalled guest TenBox scripts.
- **macOS Agent backup UI**: `TenBox.app` exposes backup status, immediate backup, and restore latest backup actions. Host-triggered backups use `~/Library/Application Support/TenBox/AgentBackups/<vm-id>` as the durable shared folder.
- **macOS Agent health UI**: `TenBox.app` exposes health check, restart, model test, config reset, and diagnostics actions while a VM is running. Repair actions run through app-generated shell commands so no image rebuild is required.
- **macOS app signing**: the app entitlement includes `com.apple.security.cs.disable-library-validation` so the hardened-runtime app can load the bundled Sparkle framework.
- **Static build** (`TENBOX_STATIC_FFMPEG=ON`) requires `/opt/tenbox-deps` (only present inside the CI/packaging container). Dev builds use system shared libs — keep `ON` off by default.
- **Release**: `docs/release.md` — VERSION bump → commit → push → tag → push tag. Always push commit before tag.

## More details

| Topic | Document |
| --- | --- |
| Daemon architecture | [docs/tenboxd.md](docs/tenboxd.md) |
| Build & images | [docs/build.md](docs/build.md) |
| CLI reference | [docs/cli.md](docs/cli.md) |
| Release process | [docs/release.md](docs/release.md) |
| Linux upgrade path | [docs/linux-update.md](docs/linux-update.md) |
| User guide (Chinese) | [Feishu Wiki](https://my.feishu.cn/wiki/Q96KwUH1Di3cAik2W7kcQsWKncb) |
