# CLI reference

## `tenbox` — daemon CLI

`tenbox` communicates with a running `agentsphered` over the local Unix socket. The
socket is auto-discovered (see `docs/agentsphered.md` — Local RPC); override with
`$AGENTSPHERE_SOCK`.

```
tenbox <command> [options]
```

### Top-level commands

| Command | Description |
| --- | --- |
| `doctor` | Run KVM support check and print a structured JSON report |
| `system info` | Print daemon config, data directory, and current host resources |
| `--version` / `version` | Print daemon version and exit |
| `--help` / `help` | Print usage and exit |

### `tenbox vm` subcommands

| Command | Description |
| --- | --- |
| `vm ls` | List all VMs (state, resources, last failure) |
| `vm create` | Create a new VM (see options below) |
| `vm edit <id>` | Edit an existing VM (see options below) |
| `vm start <id>` | Start a VM |
| `vm stop <id>` | Hard-kill a running VM (SIGKILL to the runtime process) |
| `vm reboot <id>` | Graceful reboot via guest agent (requires guest agent connected) |
| `vm shutdown <id>` | Graceful shutdown via guest agent (requires guest agent connected) |
| `vm rm <id>` | Stop (if running) and delete a VM and its data directory |
| `vm console <id>` | Attach terminal to the VM's text console (raw mode; Ctrl-] to detach) |
| `vm logs <id>` | Print the last N lines of VM console/runtime logs |

#### `vm create` options

| Option | Description |
| --- | --- |
| `--name NAME` | VM display name **(required)** |
| `--kernel PATH` | Path to Linux kernel image (`vmlinuz` or `Image`) **(required)** |
| `--initrd PATH` | Path to initramfs |
| `--disk PATH` | Path to raw or qcow2 disk image |
| `--memory MB` | Guest RAM in MB (default: 256) |
| `--cpus N` | Number of vCPUs (default: 1) |

#### `vm edit` options

| Option | Description |
| --- | --- |
| `--name NAME` | New display name |
| `--memory MB` | New RAM size in MB |
| `--cpus N` | New vCPU count |
| `--debug on\|off` | Enable/disable verbose kernel output |
| `--net on\|off` | Enable/disable virtio-net link |

#### `vm logs` options

| Option | Description |
| --- | --- |
| `--lines N` | Number of lines to print (default: 200) |
| `-f` / `--follow` | Follow log output (print historical tail then stream new lines) |

---

## `agentsphere-vm-runtime` — direct runtime CLI

The runtime is normally launched by `agentsphered` (or by the GUI manager on
Windows/macOS). It can also be invoked directly for development and debugging.

On Windows the binary is `agentsphere-vm-runtime.exe`; on macOS and Linux it is
`agentsphere-vm-runtime`.

```
agentsphere-vm-runtime --kernel <path> [options]
```

### Options

| Option | Description |
| --- | --- |
| `--kernel <path>` | Path to Linux kernel image (`vmlinuz` or `Image`) **(required)** |
| `--initrd <path>` | Path to initramfs |
| `--disk <path>` | Path to raw or qcow2 disk image |
| `--cmdline <str>` | Kernel command line |
| `--memory <MB>` | Guest RAM in MB (default: 256, minimum: 16) |
| `--cpus <N>` | Number of vCPUs (default: 1, max: 128) |
| `--net` | Start with virtio-net link up (default: link down) |
| `--debug` | Enable debug mode (verbose kernel output) |
| `--hostfwd <spec>` | Host-to-guest port forward (repeatable), e.g. `tcp:127.0.0.1:8080-:80` |
| `--guestfwd <spec>` | Guest-to-host forward (repeatable), e.g. `guestfwd:10.0.2.3:80-127.0.0.1:18981` |
| `--share TAG:PATH[:ro]` | Share a host directory via virtiofs (repeatable) |
| `--interactive on\|off` | Attach stdio as a serial console (default: on when no `--control-endpoint`) |
| `--vm-id <id>` | VM instance identifier (default: `default`) |
| `--vm-dir <path>` | VM working directory; crash dumps go to `<vm-dir>/crash` |
| `--control-endpoint <name>` | IPC endpoint for manager: Named Pipe name (Windows, without `\\.\pipe\`) or Unix socket path (macOS/Linux) |
| `--version` / `-v` | Show version and exit |
| `--help` / `-h` | Show help and exit |

### NAT networking

When `--net` is passed, AgentSphere provides a user-mode network stack (lwIP):

| Address | Role |
| --- | --- |
| `10.0.2.2` | Gateway (host) |
| `10.0.2.15` | Guest IP (via DHCP) |
| `8.8.8.8` | DNS server |

- **Outbound TCP** — proxied through the lwIP TCP stack to host sockets
- **Outbound UDP** — directly relayed by the host networking layer
- **ICMP** — relayed via raw socket where supported
- **Port forwarding** — `--hostfwd` (host→guest) and `--guestfwd` (guest→host)

### Port forward spec format

`--hostfwd tcp:<host-addr>:<host-port>-:<guest-port>`

- `tcp:127.0.0.1:2222-:22` — expose guest SSH on loopback only
- `tcp:0.0.0.0:2222-:22` — expose guest SSH on all interfaces (LAN-accessible)

`--guestfwd guestfwd:<guest-addr>:<guest-port>-[<host-addr>]:<host-port>`

- `guestfwd:10.0.2.3:80-127.0.0.1:18981` — route guest requests to `10.0.2.3:80` to the host's port 18981
