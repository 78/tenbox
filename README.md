# TenBox

A lightweight x86-64 Virtual Machine Monitor (VMM) for Windows, built on top of the Windows Hypervisor Platform (WHVP). TenBox can boot a full Linux system from a qcow2 disk image with virtio devices, NAT networking, and port forwarding — all in a single small binary.

## Features

- **WHVP hypervisor backend** — native hardware-accelerated virtualization on Windows 10/11
- **Linux boot protocol** — boots standard `vmlinuz` + `initramfs`
- **VirtIO MMIO devices** — block (disk) and network, discovered via ACPI DSDT
- **qcow2 & raw disk images** — with compressed cluster and copy-on-write support
- **NAT networking** — built-in DHCP server, TCP/UDP NAT proxy, ICMP relay
- **Port forwarding** — expose guest TCP services on host ports
- **Minimal device emulation** — UART 16550, i8254 PIT, CMOS RTC, I/O APIC, ACPI PM
- **Small footprint** — single executable, no external dependencies at runtime

## Quick Start

### Prerequisites

- Windows 10/11 with **Hyper-V** enabled (for WHVP)
- Visual Studio 2022+ with C++20 support
- CMake 3.20+
- WSL2 or a Linux environment (for building disk images)

### Build

```bash
cmake -B out/build -G Ninja
cmake --build out/build
```

### Prepare VM Images

Run these scripts in WSL2 or Linux:

```bash
# 1. Get a Debian kernel
./scripts/get-kernel.sh

# 2. Build initramfs (includes virtio + ext4 modules)
./scripts/make-initramfs.sh

# 3. Build a Debian rootfs
sudo ./scripts/make-rootfs.sh
```

### Run

```bash
TenBox.exe --kernel build/vmlinuz --initrd build/initramfs.cpio.gz --disk build/rootfs.qcow2 --net
```

With port forwarding (e.g. SSH):

```bash
TenBox.exe --kernel build/vmlinuz --initrd build/initramfs.cpio.gz --disk build/rootfs.qcow2 --net --forward 2222:22
```

### CLI Options

| Option | Description |
|---|---|
| `--kernel <path>` | Path to vmlinuz **(required)** |
| `--initrd <path>` | Path to initramfs |
| `--disk <path>` | Path to raw or qcow2 disk image |
| `--cmdline <str>` | Kernel command line (default: `console=ttyS0 earlyprintk=serial`) |
| `--memory <MB>` | Guest RAM in MB (default: 256, min: 16) |
| `--cpus <N>` | Number of vCPUs (default: 1, max: 128) |
| `--net` | Enable virtio-net with NAT networking |
| `--forward H:G` | Port forward host port H to guest port G (repeatable) |

## Architecture

```
┌─────────────────────────────────────────────────┐
│                   TenBox VMM                    │
├──────────┬──────────────────────┬───────────────┤
│ Hypervisor│     Device Layer     │  Net Backend  │
│  (WHVP)  │                      │  (lwIP+NAT)   │
│          │  UART  PIT  RTC      │               │
│  vCPU    │  IOAPIC  ACPI PM    │  DHCP Server  │
│  Memory  │  VirtIO-MMIO        │  TCP/UDP NAT  │
│  VM Exit │   ├─ virtio-blk     │  ICMP Relay   │
│          │   └─ virtio-net ◄───┤  Port Forward │
├──────────┴──────────────────────┴───────────────┤
│              Address Space (PIO / MMIO)          │
└─────────────────────────────────────────────────┘
```

### Source Layout

```
src/
├── main.cpp                 # Entry point & CLI parsing
├── vmm/                     # VM orchestration & address space
├── hypervisor/              # WHVP platform interface
├── arch/x86_64/             # Linux boot protocol & ACPI tables
├── device/
│   ├── serial/              # 16550 UART
│   ├── timer/               # i8254 PIT
│   ├── rtc/                 # CMOS RTC
│   ├── irq/                 # I/O APIC
│   ├── acpi/                # ACPI PM registers
│   └── virtio/              # VirtIO MMIO, block, net, qcow2
└── net/                     # lwIP integration, NAT, DHCP
scripts/
├── get-kernel.sh            # Extract vmlinuz from Debian package
├── make-initramfs.sh        # Build BusyBox initramfs with modules
└── make-rootfs.sh           # Build Debian rootfs as qcow2
```

### Networking

When `--net` is enabled, TenBox provides a user-mode NAT network:

| Address | Role |
|---|---|
| `10.0.2.2` | Gateway (host) |
| `10.0.2.15` | Guest IP (via DHCP) |
| `8.8.8.8` | DNS server (Google) |

- **Outbound TCP** — proxied through lwIP TCP stack to host Winsock sockets
- **Outbound UDP** — directly relayed via Winsock (DNS, NTP, etc.)
- **ICMP** — relayed via raw socket (requires admin for ping)
- **Port forwarding** — `--forward 8080:80` listens on host:8080, connects to guest:80

### Guest Defaults

- Root password: `tenbox`
- Auto-login on serial console (`ttyS0`)
- Network: DHCP on `eth0` (auto-configured)

## Dependencies

Fetched automatically by CMake:

- [zlib](https://github.com/madler/zlib) — qcow2 compressed cluster decompression
- [lwIP](https://github.com/lwip-tcpip/lwip) — lightweight TCP/IP stack for NAT networking

## License

MIT
