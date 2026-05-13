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
- **Agent toolbox**: macOS and Windows desktop managers expose the Agent toolbox without image/rootfs changes. Guest-side toolbox logic lives in `src/agent_tools/guest/agent_tools.sh`; managers copy it into a runtime-only shared folder and execute it with qemu-guest-agent `guest-exec` as user `tenbox`. Keep console-marker paths as fallback only, throttle bulk console input, and never persist temporary Agent share tags into VM manifests.
- **Agent profile and backups**: keep the gzip package format in `docs/agent-profile.md`, include `export_scope`, reject cross-Agent imports, merge imports into the existing Agent home, and exclude reinstallable binaries plus volatile logs/caches/runtime locks. Host backups live under the platform TenBox data dir in `AgentBackups/<vm-id>/<agent>`, use time-based filenames, tolerate live-file tar churn, and rotate by the configured retention count.
- **Agent toolbox tests**: keep guest script archive-safety, cross-Agent import rejection, import rollback, and OpenClaw migration preflight coverage in `tests/test_agent_tools_guest.sh`; run it through CTest after changing `src/agent_tools/guest/agent_tools.sh`.
- **Agent scheduled backups**: store per-VM/per-Agent schedules in `settings.json` under `agent_backups.schedules`; only run them when the VM is running and guest execution is connected, and surface the last automatic backup attempt in the UI.
- **Agent health and repair**: health, restart, reset, and diagnostics run through guest-exec as user `tenbox`; fail instead of falling back to root if user switching fails, and enforce guest-side timeouts for long commands. Destructive or repair actions must create a host-managed backup first, patch only the necessary config, confirm with the user, and avoid full guest `/tmp` extraction that can exhaust small images.
- **OpenClaw to Hermes migration**: use official `hermes claw migrate` with a separate dry run; pass `--migrate-secrets`, `--workspace-target`, Hermes global `--overwrite`, and map UI conflict choices only to `--skill-conflict`. Treat `Refusing to apply` as failure, save dry-run/final reports beside Hermes backups, restore TenBox model proxy settings after migration, and only copy compatible Feishu/WeCom env settings best-effort.
- **Agent UI responsiveness**: Agent tool UI defaults to English and switches to Chinese only when the current system language is Chinese. Put destructive/low-frequency actions behind confirmation, run guest-exec and shared-folder IPC off the UI thread, and show compact progress/results while writing full logs/reports to files.
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
