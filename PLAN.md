# tenboxd — Headless Daemon & Remote Control Plan (HISTORICAL)

> **Status (2026-05): superseded.** The product direction shifted from
> "BYO-overlay + self-hosted HTTPS+WS" to a managed cloud (`my.tenbox.ai`) with
> outbound device tunnels and browser remote desktop via WebRTC. The current
> active plan is in [`TENBOXD.md`](TENBOXD.md). This file is kept as a record
> of the original scoping discussion (Tailscale-friendly direct connections,
> embedded web UI inside `tenboxd`, etc.) so we don't relitigate decisions
> already made — but it is no longer the source of truth for what is shipping.

This document captures the plan for turning TenBox into a client/server product
with a headless daemon (`tenboxd`) that can run on Linux hosts (including
Raspberry Pi 5), be controlled remotely from desktop managers, a web UI, or a
CLI, and peacefully coexist with user-provided network overlays such as
Tailscale or WireGuard.

The plan is intentionally incremental — each phase is independently useful and
leaves the product in a shippable state.

---

## 1. Motivation

Today TenBox ships as a monolithic desktop app (Win32 manager on Windows,
SwiftUI manager on macOS). The `ManagerService` class already contains all of
the core VM lifecycle logic in a UI-agnostic form; the UI is just a callback
subscriber. This makes it natural to:

1. Run the core on a small headless Linux box (e.g. Raspberry Pi 5) and expose
  it as a network service.
2. Let users control that service from any of: the existing desktop manager,
  a web browser, or a CLI.
3. Support fleet/home-server use cases (AI agent sandboxes kept running 24/7
  on a Pi) without forcing users through a GUI.

This is the same architectural split Docker uses: `dockerd` + multiple
clients. TenBox's equivalent is `tenboxd` + thin clients.

---

## 2. Target Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  Host (Pi 5, Linux PC, or any Linux server)                     │
│                                                                 │
│   tenboxd  (daemon, headless)                                   │
│    ├─ ManagerService (existing code, mostly unchanged)          │
│    ├─ Local RPC:  Unix socket (trusted local clients)           │
│    ├─ Remote RPC: HTTPS + WebSocket (TLS + token auth)          │
│    ├─ Embedded static web UI                                    │
│    └─ Spawns `tenbox-vm-runtime` processes (one per VM)         │
│                                                                 │
│   tenbox-vm-runtime (per VM, unchanged IPC back to tenboxd)     │
└────────────────────────────────┬────────────────────────────────┘
                                 │ TLS + token
                                 │ (routed via LAN / Tailscale / WG)
        ┌────────────────────────┼────────────────────────┐
        ▼                        ▼                        ▼
┌─────────────────┐      ┌─────────────────┐      ┌─────────────────┐
│  tenbox CLI     │      │ Web UI          │      │ Desktop Manager │
│  (local+remote) │      │ (browser)       │      │ (Win32/SwiftUI) │
└─────────────────┘      └─────────────────┘      └─────────────────┘
```

### 2.1 Process boundary

- `tenboxd` owns VM state, persists `vm.json`, spawns runtimes, holds shared
memory framebuffers, proxies LLM traffic, and exposes the RPC surface.
- `tenbox-vm-runtime` is unchanged — it still talks to its parent over the
existing `protocol_v1` IPC (Unix socket on Linux/macOS, named pipe on
Windows).
- Clients never talk to runtimes directly. Everything flows through `tenboxd`.

### 2.2 Local vs remote transports


| Transport   | Endpoint                                                         | Auth                   | Intended use                               |
| ----------- | ---------------------------------------------------------------- | ---------------------- | ------------------------------------------ |
| Unix socket | `$XDG_RUNTIME_DIR/tenbox.sock` (fallback `/var/run/tenbox.sock`) | filesystem permissions | CLI, local desktop manager, local web UI   |
| HTTPS + WS  | `0.0.0.0:8443` (configurable)                                    | TLS + bearer token     | Remote desktop manager, remote browser, CI |


Both transports serve the **same** RPC schema.

### 2.3 Non-goals

- **TenBox does not implement its own NAT traversal.** Tailscale / WireGuard /
Cloudflare Tunnel / frp already solve this better than we ever could. We aim
to be a good citizen on top of those overlays, not compete with them.
- **No multi-tenant permissions in v1.** Single-admin token. Multi-user and
quotas are a future consideration only if the product goes that direction.

---

## 3. RPC Design

### 3.1 Protocol choice: HTTP + WebSocket

Chosen over gRPC for these reasons:

- Browser-native, no `grpc-web` + Envoy proxy required for the web UI.
- Trivial to debug with `curl` and browser devtools.
- Standard TLS / bearer-token stack; no custom auth middleware.
- Desktop clients can use any mature WS library (libwebsockets, IXWebSocket,
`URLSessionWebSocketTask` on Apple platforms).

We explicitly keep the door open to swap in gRPC later if strong typing across
many clients becomes a pain point; the RPC schema will be defined in a single
source of truth (JSON schema / OpenAPI) regardless.

### 3.2 Surface sketch

REST-ish HTTP for request/response operations:

```
GET    /v1/vms                    # list VMs
POST   /v1/vms                    # create VM
GET    /v1/vms/{id}               # get VM
PATCH  /v1/vms/{id}               # edit mutable patch
DELETE /v1/vms/{id}               # delete
POST   /v1/vms/{id}:start
POST   /v1/vms/{id}:stop
POST   /v1/vms/{id}:reboot
POST   /v1/vms/{id}:shutdown
POST   /v1/vms/{id}/shared-folders
POST   /v1/vms/{id}/port-forwards
GET    /v1/settings
PATCH  /v1/settings
GET    /v1/system                 # hypervisor availability, versions, capabilities
```

WebSocket for event streams and high-rate / bidirectional channels. One WS
connection can multiplex many channels, mirroring the existing `ipc::Channel`
split:

```
WS /v1/ws
  subscribe { channel: "state",    vm_id?: "..." }
  subscribe { channel: "console",  vm_id: "..." }
  subscribe { channel: "display",  vm_id: "..." }   # frames
  subscribe { channel: "input",    vm_id: "..." }   # client -> server
  subscribe { channel: "audio",    vm_id: "..." }
  subscribe { channel: "clipboard",vm_id: "..." }
  subscribe { channel: "events" }                   # global: VM lifecycle, PF errors, GA state
```

### 3.3 Authentication

- **Local Unix socket**: trust by filesystem permissions (0600, owner only).
- **Remote HTTPS**:
  - Bearer token via `Authorization: Bearer <token>` header (and
  `?token=...` query param for WS when headers are awkward in browsers).
  - Token persisted at `$XDG_CONFIG_HOME/tenbox/auth.token` (mode 0600).
  - First-run UX: daemon prints an "access URL" with the token embedded,
  identical to Jupyter's `?token=...` pattern.
  - Self-signed TLS cert auto-generated on first run; users can drop in their
  own cert/key via `--tls-cert` / `--tls-key`.

### 3.4 Discovery & context switching

- CLI stores named contexts in `$XDG_CONFIG_HOME/tenbox/contexts.json`:
`{ name, url, token, tls_ca? }`. `tenbox context use <name>` switches.
- Desktop manager shows a "Hosts" picker (analogous to Docker Desktop's
context switcher). A context can be "Local" (unix socket) or a named remote.

---

## 4. Display Transport (the hard problem)

The desktop managers currently receive raw RGBA frames via shared memory. That
obviously cannot traverse the network. The plan:


| Phase                        | Method                                         | Tradeoffs                                                                 |
| ---------------------------- | ---------------------------------------------- | ------------------------------------------------------------------------- |
| First remote-capable release | **JPEG/WebP frame push over WS** (noVNC-style) | Easy to implement; ~5–20 Mbps for 30 fps desktop; acceptable latency      |
| Later                        | **WebRTC + H.264 (hardware encoded on Pi 5)**  | Lower bitrate, lower latency; significant implementation complexity       |
| Optional                     | **SPICE over WebSocket** with `spice-html5`    | Reuses existing SPICE vdagent investment; less control over codec choices |


Design principles:

- Keep shared-memory path for local same-host clients (zero-copy, no encode).
- Encoder lives inside `tenboxd`, not in the runtime — runtime stays simple.
- Frame deltas: we already have dirty-rect information in the display pipeline;
use it to send only changed tiles where possible.
- Input events (keyboard/pointer/wheel/resize) travel back over the same WS
channel.

Pi 5 hardware notes: BCM2712 has a hardware H.264 encoder accessible via V4L2
(`/dev/video11`). We will evaluate using it in the WebRTC phase.

---

## 5. Web UI

Scope for v1:

- VM list, create/edit/delete dialogs (parity with desktop managers' forms).
- VM detail: info, console (xterm.js), display (JPEG canvas initially), shared
folders, port forwards.
- Settings: LLM proxy config, image sources, auth token rotation.
- System dashboard: hypervisor status, resource usage, daemon version/uptime.

Tech choices (initial proposal):

- **React + Vite + TypeScript** for the frontend.
- **TanStack Query** for the REST layer, thin WS client for streams.
- **shadcn/ui** or **Mantine** for components — both have serious a11y and
dark-mode out of the box.
- Built assets embedded into `tenboxd` via a resource file (no separate web
server to deploy).

The web UI lives under `website-app/` (separate from the existing marketing
`website/`). Build output is copied into `src/daemon/embedded_web/` at build
time.

---

## 6. Desktop Manager Refactor

The goal is for Win32 and SwiftUI managers to become thin clients over the
same RPC as the web UI.

Plan:

1. Extract a pure C++ `tenbox_client` library that wraps HTTP+WS calls and
  exposes the same callback-based API as today's `ManagerService`. The UI
   layer is callback-driven, so ideally it does not know whether it is talking
   to an in-process `ManagerService` or a remote `tenboxd`.
2. Keep the "embedded" mode on Windows/macOS for v1: the manager can still
  spawn a `tenboxd` child process and connect to it over Unix socket / named
   pipe. This gives us a single code path internally.
3. Add a "Hosts" switcher in the UI with:
  - "Local" (embedded daemon, default)
  - user-added remote hosts (URL + token, optionally imported from a CLI
  context)

Windows note: Unix domain sockets are supported on Windows 10 1803+, so the
same "local socket" transport can work cross-platform. Named pipes remain a
fallback.

---

## 7. CLI

A single static binary (`tenbox`) suitable for shipping alongside the daemon.

Initial commands (Docker-inspired, adjusted to our domain):

```
tenbox context ls | use | add | rm
tenbox vm ls
tenbox vm create --name ... --kernel ... --disk ... --memory 4096
tenbox vm start | stop | reboot | shutdown  <id>
tenbox vm rm [--force] <id>
tenbox vm inspect <id>
tenbox vm console <id>          # interactive
tenbox vm logs <id>
tenbox vm exec <id> -- <cmd>    # via guest_agent, later
tenbox image ls | pull | rm
tenbox system info | version
tenbox auth print | rotate
```

Language: C++ to share the client library with the desktop managers.
(Go would be tempting for a static single-binary, but re-implementing the
client in two languages is not worth it yet.)

---

## 8. Repository Layout Impact

Proposed additions:

```
src/
├── daemon/                # NEW: tenboxd entry point
│   ├── main.cpp
│   ├── rpc/
│   │   ├── http_server.{h,cpp}
│   │   ├── ws_server.{h,cpp}
│   │   ├── auth.{h,cpp}
│   │   ├── tls.{h,cpp}
│   │   └── handlers/      # REST + WS handlers, thin shims over ManagerService
│   ├── display_encoder/
│   │   ├── jpeg_encoder.{h,cpp}
│   │   └── frame_delta.{h,cpp}
│   └── embedded_web/      # build-time copy of web UI assets
├── client/                # NEW: shared C++ RPC client library
│   ├── tenbox_client.{h,cpp}
│   ├── http_client.{h,cpp}
│   └── ws_client.{h,cpp}
├── cli/                   # NEW: `tenbox` CLI binary
│   └── main.cpp
├── platform/
│   └── linux/             # NEW: see Linux/KVM plan (separate but related)
└── manager/ and manager-macos/  # refactored to use src/client
website-app/               # NEW: web UI source (React/TS)
```

### 8.1 Relationship to the Linux / KVM port

The daemon work is logically independent from adding a Linux platform backend
(KVM for aarch64 on Pi 5, optionally KVM for x86_64 later), but the two are
the most natural to do **together** because:

- `tenboxd` is most interesting on Linux (Pi 5 / home servers).
- The existing `ManagerService` is already portable; the missing piece on
Linux is `src/platform/linux/hypervisor/`* (KVM backend).
- Doing them in the same push avoids writing throwaway scaffolding.

See the phase plan below for ordering.

---

## 9. Phased Delivery

### Phase 0 — Prep (no behavior change)

- Audit `ManagerService` for any residual platform-specific bits (Win32
`HANDLE`, libuv usage, process spawn on Windows) and factor them behind an
abstraction so the class compiles cleanly on Linux.
- Lock down an RPC schema document (`docs/rpc-v1.md` or OpenAPI YAML). Single
source of truth before any handler is written.

### Phase 1 — Linux runtime + local-only daemon

- Add `src/platform/linux/` implementing `HypervisorVm` / `HypervisorVCpu`
over KVM (aarch64 first for Pi 5; x86_64 Linux can follow).
- Add `src/daemon/` serving the RPC schema over **Unix socket only**.
- Add `src/cli/` with a minimal command set (`vm ls/create/start/stop/console`).
- Target: `./tenbox vm create ... && ./tenbox vm start foo && ./tenbox vm console foo`
works end-to-end on a Pi 5. No network access yet.

Exit criteria: a developer can SSH into a Pi 5 and run real AI-agent VMs with
the CLI.

### Phase 2 — Remote access

- Add HTTPS listener with auto-generated self-signed cert.
- Add token-based auth, with a "first run prints access URL" UX.
- Publish the web UI MVP (VM list, create/edit/delete, console via xterm.js,
JPEG-based display viewer, settings).
- CLI gains context support (`tenbox context add`, remote hosts).
- Document Tailscale / WireGuard integration patterns (no code — just a
recipe page in `docs/`).

Exit criteria: from a laptop on another network (via Tailscale), a user can
open `https://pi5:8443/?token=...`, create a VM, see its display, and control
it.

### Phase 3 — Desktop managers as thin clients

- Extract `src/client/tenbox_client`.
- Win32 and SwiftUI managers switch to the client library; "Local" uses
embedded daemon, "Remote" connects over HTTPS+WS.
- Add Hosts picker UI.

Exit criteria: a Windows or macOS user can add their Pi 5 as a remote host in
the desktop manager and get a native experience for remote VMs.

### Phase 4 — Quality & polish (optional, ongoing)

- WebRTC display transport with Pi 5 hardware H.264 encoding.
- Optional embedded `tsnet` (Tailscale's Go library via cgo or a sidecar) so
that `tenboxd` can join a tailnet without the user installing `tailscaled`.
- Audit / session log, basic RBAC if product pushes that direction.
- Packaging: `.deb` for Debian-based distros (Pi OS, Ubuntu), systemd unit
file, `tenbox-web` service.

---

## 10. Open Questions

These are things to decide before/during Phase 1:

1. **HTTP/WS library**: `cpp-httplib` + custom WS, `Boost.Beast`,
  `uWebSockets`, or extending libuv (already a dependency) with a thin HTTP
   layer? Leaning toward `cpp-httplib` + `uWebSockets` for simplicity, but
   library surface on aarch64/glibc/musl needs to be checked.
2. **TLS stack**: OpenSSL (biggest ABI, ships everywhere) vs mbedTLS (smaller,
  already common in embedded) vs BoringSSL. Default guess: OpenSSL for Linux,
   SChannel on Windows, SecureTransport/Network.framework on macOS — abstract
   behind a thin `TlsContext`.
3. **Schema format**: hand-written Markdown, OpenAPI 3.1, or Protobuf (even if
  transport stays JSON-over-HTTP)? OpenAPI gives us codegen for TS clients
   "for free".
4. **Config file format**: reuse the existing `AppSettings` JSON layout, or
  introduce a `/etc/tenbox/config.yaml` for daemon-specific knobs (listen
   addr, cert path, data dir)? Probably keep them separate — daemon-level
   config shouldn't be edited by the app.
5. **Data directory location on Linux**: `/var/lib/tenbox` for system-wide
  installs vs `$XDG_DATA_HOME/tenbox` for per-user installs. We probably need
   both, selected by how the daemon was launched (systemd unit vs user
   session).
6. **Backward compat**: do we keep "no-daemon" embedded mode on
  Windows/macOS, or is `tenboxd` always running as a child process of the
   manager app on those platforms? Child-process approach keeps one code
   path; direct in-process usage is simpler for debugging.

---

## 11. Risks

- **Display transport is the dominant performance concern.** JPEG works but
can be CPU-heavy on a Pi 5 for full-frame updates; we must use dirty rects
and adaptive quality from day one, not bolt them on later.
- **Security surface grows sharply once we listen on TCP.** Token rotation,
TLS hygiene, rate limiting on auth failures, and CSRF protection for the
browser UI are all must-haves before we advertise remote access as
"supported".
- **Windows desktop UDS support** requires Windows 10 1803+. Fine for our
stated Windows 10/11 baseline, but the named-pipe fallback must keep working.
- **Scope creep into "home server / fleet manager" territory.** It is very
tempting to add multi-user, quotas, scheduling, etc. once there is a
daemon. We must resist until the single-admin case is genuinely polished.

---

## 12. Out of Scope for this Plan

- Full multi-tenant / multi-user support.
- A hosted control plane ("TenBox Cloud"). Users bring their own networking.
- Guest OS image marketplace beyond the existing `image_manager.py` sources.
- Cluster / multi-host VM scheduling.

---

## 13. Prior Art & Positioning

A clear-eyed look at what TenBox's architecture resembles, and — more
importantly — why we are still building it rather than composing existing
tools.

### 13.1 Architectural analogues


| TenBox component                                  | VMware analogue          | libvirt-world analogue              |
| ------------------------------------------------- | ------------------------ | ----------------------------------- |
| `tenboxd` (headless daemon)                       | ESXi `hostd`             | `libvirtd`                          |
| `tenbox-vm-runtime` (one per VM)                  | VMX process              | per-VM `qemu-system-`*              |
| HTTPS + token RPC, Unix socket locally            | vSphere API over HTTPS   | `qemu:///system` + `qemu+tls://...` |
| Embedded Web UI                                   | ESXi Host Client (HTML5) | Cockpit VMs module / Kimchi         |
| Desktop manager as thin client                    | vSphere Client           | virt-manager                        |
| `tenbox` CLI                                      | ESXCLI / PowerCLI        | `virsh`                             |
| Local shared-mem framebuffer + remote JPEG/WebRTC | VMware MKS / Blast       | SPICE / VNC + noVNC                 |


Topologically, TenBox is **closest to a single-host ESXi with its built-in
Host Client**, and **closest in spirit to the libvirtd + virsh +
virt-manager + Cockpit** open-source stack. Proxmox VE is the product that
combines both (single-host-friendly, Web UI first, hobbyist-accessible) and
is a useful UX reference — though our scope is narrower.

**Explicitly not in scope:** anything resembling vCenter, Proxmox Cluster,
or KubeVirt. TenBox is single-host by design; multi-host fleet management
is a different product.

### 13.2 Why not just libvirt on a Raspberry Pi?

It is worth being honest: **a motivated user can already achieve much of
what TenBox provides on a Pi 5 today** by installing
`qemu-system-aarch64 + libvirt-daemon-system + cockpit-machines`, pointing
virt-manager or Cockpit at it, and running their own guest images. Several
hobbyists already do. That makes the "why TenBox" question non-trivial, and
the answer needs to hold up.

TenBox's differentiation is at the **product layer, not the virtualization
layer**. We are not trying to beat libvirt at being a general-purpose
virtualization toolkit; we are trying to be a better fit for a specific
audience and a specific use case.

**1. Vertical focus on AI-agent sandboxing.**
libvirt is a general-purpose platform; it assumes users are comfortable
editing domain XML, building storage pools, wiring up bridged networking,
and installing a guest OS from scratch. TenBox's audience is "people who
want to run an AI agent safely on their own machine" — which includes
researchers, PMs, and hobbyists, not only sysadmins. For that audience we
ship:

- Curated guest images (Chromium, OpenClaw, QwenPaw, Hermes) with SPICE
vdagent, `qemu-guest-agent`, desktop environment, and an agent runtime
pre-installed.
- A built-in LLM proxy that maps guest OpenAI-style traffic to the user's
configured upstream provider, with per-request logging. libvirt has no
equivalent and never will — it is out of scope for them.
- One-click virtiofs shared folders, port forwarding, and clipboard
integration via a GUI, with sensible defaults.

**2. Truly cross-platform client experience.**
libvirt is Linux-only on the host side. Mac and Windows users either run
Cockpit in a browser (adequate but not native) or install virt-manager
(painful on macOS, requires X11). With TenBox:

- The same Desktop Manager app on macOS, Windows, or Linux can manage
local VMs (HVF / WHVP / KVM) **and** remote VMs on a Pi 5 (KVM) from a
single UI.
- Users on Apple Silicon get native HVF locally plus a unified remote
experience — something no libvirt-based setup offers.

**3. Single-binary, zero-config deployment.**
A libvirt setup on Debian 12+ now requires `libvirtd` + `virtqemud` +
`virtnetworkd` + `virtlogd` + `qemu-system-`* plus PolicyKit rules, a
network bridge, and a storage pool. `tenboxd` is designed to be **one
binary + one systemd unit + an auto-generated TLS cert + token**.
Installation and uninstallation are trivial. For "install TenBox on my Pi
this afternoon to try it" this matters a lot.

**4. Purpose-built remote UX, not retrofitted.**
The `ipc::protocol_v1` already separates display / input / audio /
clipboard into distinct channels designed for low-latency desktop
interaction. Our remote path reuses the same model with JPEG/WebRTC
transport. Achieving the equivalent with libvirt + SPICE + noVNC requires
a significant glue layer (cf. Kasm Workspaces) and exposes the user to
SPICE TLS configuration, client compatibility issues, and separate port
forwarding for each VM.

**5. Integrated release vehicle.**
`tenboxd`, the runtime, the guest images, the image catalog
(`image_manager.py`), and the managers are all released together and
versioned together. A user gets a coherent experience; libvirt users
assemble it themselves from many independently-versioned pieces.

### 13.3 When users should NOT use TenBox

Being explicit about this keeps us honest and keeps the scope tight:

- Running Windows Server / complex enterprise guests → **libvirt or
Proxmox**.
- Live migration, HA, DRS, clustering → **Proxmox Cluster / vCenter /
KubeVirt**.
- PCI passthrough, SR-IOV, custom NUMA topology → **libvirt**.
- Bare-metal provisioning and infrastructure-as-code heavy workflows →
**libvirt + Terraform/Ansible providers**.

If the user's question is "how do I virtualize things in general", libvirt
is the correct answer. TenBox's answer is narrower and therefore can be
sharper: "how do I safely run AI agents on my own hardware, including a
Raspberry Pi, and control them from anywhere?"