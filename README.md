# AgentSphere

AgentSphere lets you run AI agents safely on your personal computer. Each agent runs
inside a secure, isolated virtual machine — it can only access the files you
explicitly authorize, keeping your privacy and data protected.

Under the hood, AgentSphere is a cross-platform Virtual Machine Monitor (VMM) with a
shared C++ runtime. It runs full Linux desktop environments with
hardware-accelerated virtualization, GPU display output, audio, shared folders,
and clipboard integration. Windows hosts use WHVP with a Win32 manager; macOS
hosts use Hypervisor Framework (both Apple Silicon and Intel) with a
SwiftUI/AppKit manager; Linux hosts run `agentsphered`, a systemd daemon that manages
VM lifecycle, exposes a local CLI, and provides optional browser-based remote
desktop.

## Screenshots

<table>
  <tr>
    <td width="50%" align="center">
      <img src="website/public/images/macos_light.png" alt="AgentSphere on macOS" width="100%" style="border-radius: 12px;" />
    </td>
    <td width="50%" align="center">
      <img src="website/public/images/windows_light.png" alt="AgentSphere on Windows" width="100%" style="border-radius: 12px;" />
    </td>
  </tr>
</table>

## Features

- **Cross-platform hypervisor backends** — WHVP on Windows, Hypervisor Framework on macOS (Apple Silicon and Intel), KVM on Linux (x86_64 and arm64, including Raspberry Pi)
- **Native GUI managers** — Win32 on Windows, SwiftUI/AppKit on macOS
- **Linux daemon (`agentsphered`)** — systemd-managed, local RPC over `/run/tenbox/tenbox.sock`, `tenbox` system group access control
- **`tenbox` CLI** — `doctor` / `system info` / `vm ls|create|edit|start|stop|reboot|shutdown|rm|console|logs`
- **Linux boot support** — boots standard `vmlinuz` / `Image` kernels with `initramfs`
- **VirtIO MMIO devices** — block, network, GPU, input, serial, sound, and filesystem
- **qcow2 & raw disk images** — zlib and zstd compressed cluster support, copy-on-write
- **GPU display** — virtio-gpu with SPICE protocol, resizable display window
- **Audio output** — virtio-snd streamed to host via WASAPI on Windows and CoreAudio on macOS
- **Shared folders** — virtiofs (virtio-fs), configurable per VM with optional read-only mode
- **Clipboard sharing** — bidirectional host ↔ guest clipboard via SPICE vdagent protocol
- **Guest agent** — qemu-guest-agent integration for VM lifecycle management (graceful reboot/shutdown)
- **NAT networking** — built-in DHCP server, TCP/UDP NAT proxy, ICMP relay via lwIP
- **Port forwarding** — host-forward (expose guest TCP services on host ports) and guest-forward (route guest traffic to host services)
- **Multi-VM management** — create, edit, start, stop, reboot, and delete VMs; config persisted as `vm.json`
- **Platform-specific machine models** — x86_64 (Local APIC / I/O APIC) and aarch64 (GICv3) guest support
- **Browser remote desktop (Linux)** — daemon-embedded libdatachannel + FFmpeg H.264 (high/baseline) + Opus; dual `input-fast` / `control` DataChannels; bidirectional clipboard
- **Cloud pairing & self-update (Linux)** — 8-digit pairing code → `https://my.tenbox.ai/pair`; daemon accepts `host.update` and runs `apt-get install --only-upgrade tenbox` in-place
- **LLM proxy** — built-in OpenAI-compatible HTTP proxy mapping guest requests to configurable upstream providers; available in `agentsphered` on Linux and the GUI manager on Windows/macOS

## Install

### Linux (Debian 11+ / Ubuntu 20.04+ / Raspberry Pi OS 11+, amd64 / arm64)

```bash
curl -fsSL https://tenbox.ai/install.sh | sudo sh
```

Requires glibc 2.31+ and `/dev/kvm`. The installer registers the AgentSphere apt
repo, installs the `tenbox` deb, adds the current user to the `tenbox` group,
and enables `agentsphered.service`. After install, a pairing URL is printed to the
terminal — open it (or visit `https://my.tenbox.ai/`) to claim the host.
Tested on x86_64 PCs and Raspberry Pi 5; other arm64 boards may work but are
not yet validated.

### Windows / macOS

Download the latest installer from [tenbox.ai](https://tenbox.ai/) or the
[GitHub Releases](https://github.com/78/tenbox/releases) page.

## Documentation

- **User guide** (Chinese): **[养虾教程](https://my.feishu.cn/wiki/Q96KwUH1Di3cAik2W7kcQsWKncb)** (Feishu Wiki)
- **Build from source**: [docs/build.md](docs/build.md)
- **Daemon architecture (Linux)**: [docs/agentsphered.md](docs/agentsphered.md)
- **CLI reference**: [docs/cli.md](docs/cli.md)
- **Release process**: [docs/release.md](docs/release.md)
- **Linux update path**: [docs/linux-update.md](docs/linux-update.md)

## License

GPL v3 — see [LICENSE](LICENSE) for details.
