# agentsphered — Linux daemon architecture

`agentsphered` is the host-side authority for AgentSphere on Linux. It owns VM lifecycle,
persists VM state, spawns and supervises one `agentsphere-vm-runtime` process per
running VM, exposes a local CLI RPC socket, and maintains an outbound cloud
tunnel when cloud registration is enabled.

## Process model

```
agentsphered
├── rpc_server          Unix socket server (one thread per connection)
│   └── runtime_manager VM process supervisor
│       ├── agentsphere-vm-runtime  [vm-abc ── KVM]
│       ├── agentsphere-vm-runtime  [vm-def ── KVM]
│       └── ...
├── cloud_tunnel        Outbound WSS thread (reconnects on drop)
│   └── remote_webrtc   WebRTC session per running VM (on demand)
├── resource_monitor    Host + VM telemetry (30 s tick)
├── host_updater        apt self-upgrade worker (on demand)
└── llm_proxy           OpenAI-compatible HTTP reverse proxy (optional)
```

Key source files:

| Component | Source |
| --- | --- |
| Entry point | `src/daemon/main.cpp` |
| VM store (persistence) | `src/daemon/vm_store.cpp` |
| Runtime supervisor | `src/daemon/runtime_manager.cpp` |
| Local RPC server | `src/daemon/rpc_server.cpp` |
| Cloud tunnel | `src/daemon/cloud_tunnel.cpp`, `src/daemon/cloud_protocol.cpp` |
| WebRTC session | `src/daemon/remote_session.cpp`, `src/daemon/remote_webrtc.cpp` |
| Resource telemetry | `src/daemon/resource_monitor.cpp` |
| Self-updater | `src/daemon/host_updater.cpp` |
| LLM proxy | `src/daemon/llm_proxy.cpp` |
| KVM doctor | `src/daemon/kvm_doctor.cpp` |
| Host settings | `src/daemon/host_settings.cpp` |

## Local RPC

The daemon listens on a Unix domain socket. Clients (`src/client/client.cpp`)
send newline-delimited JSON requests; the daemon replies with a JSON object
`{"ok": true, "payload": {...}}` or `{"ok": false, "error_code": "...",
"error": "..."}`.

**Socket path resolution** (`client.cpp:DefaultSocketPath()`, in priority order):

1. `$AGENTSPHERE_SOCK` environment variable (explicit override)
2. `/run/tenbox/tenbox.sock` — system install path (exists after `agentsphered` starts
   under systemd)
3. `$XDG_RUNTIME_DIR/tenbox.sock` — per-user dev daemon (when running
   `./agentsphered` from a build tree without root)
4. `/tmp/tenbox-<uid>.sock` — last-resort fallback

**Permissions**: on a system install the daemon reads `AGENTSPHERE_SOCKET_GROUP`
from the environment (set to `tenbox` by `packaging/systemd/agentsphered.service`),
then `chown :tenbox` + `chmod 0660` the socket after `Listen()` succeeds. The
directory `/run/tenbox/` is world-traversable (`0755`) so any user can
`stat(2)` the socket and receive an honest `permission denied` rather than
`no such file`. Only members of the `tenbox` system group can `connect(2)`.
The installer (`scripts/install-linux.sh`) adds `$SUDO_USER` to the group.

## Cloud tunnel

When `cloud_url` is non-empty (`--cloud-url` / `AGENTSPHERE_CLOUD_URL`; default
`wss://my.tenbox.ai/api/device-tunnel`), the daemon opens and maintains an
outbound WebSocket connection. Pass an empty string (`--cloud-url ""`) to
disable all cloud connectivity.

**Message envelope** (JSON): `{id, type, host_id, vm_id?, payload}`.
Requests from the cloud carry an `id`; the daemon replies with the same `id`.

**TLS**: OpenSSL with the system CA bundle and SNI hostname pinning.

### Pairing state machine

1. **First start** — no `<data_dir>/device.token` exists. The daemon generates
   a random 8-digit `pair_code` and includes it in `device.hello`. The cloud
   upserts a pending pair record; the daemon is not yet visible to the user's
   account. The daemon prints the pairing URL to stdout:
   `https://my.tenbox.ai/pair?code=XXXXXXXX`

2. **Claim** — the operator opens the URL, logs in, and clicks "Bind this host".
   The cloud pushes `device.paired { device_token }` over the existing WS. The
   daemon writes `device.token` atomically (tmp → `fsync` → `rename`, mode
   `0600`) and clears the pair_code. The current WS stays open.

3. **Subsequent starts** — `device.hello` carries `device_token` only. The
   cloud verifies `sha256(token)` against its database and admits the
   connection.

**Error responses**:

- `device.unauthorized` — token hash mismatch; daemon clears state, will
  reconnect.
- `device.pair_invalid` — pair_code rejected (expired or already claimed); daemon
  rotates a new pair_code on next connect.

## Remote desktop

Each running VM can host at most one active `RemoteSession` at a time. Sessions
are initiated by the cloud on behalf of a browser client (via `remote_session.open`
on the cloud tunnel), or locally via the RPC socket.

**Media pipeline**:

- Video: FFmpeg H.264 encoding via `src/daemon/ffmpeg_video_encoder.cpp`.
  Profile is auto-negotiated from the browser's SDP `profile-level-id` field:
  high profile (CABAC + 8×8 DCT) when advertised, constrained-baseline
  otherwise.
- Audio: Opus encoding via `src/daemon/opus_audio_encoder.cpp`.
- Pixel conversion: libyuv (fast BGRA→YUV420P path), libswscale fallback.

**DataChannels** (libdatachannel):

- `input-fast` (`{ ordered: false, maxRetransmits: 0 }`) — pointer moves and
  wheel events.
- `control` (reliable, ordered) — keystrokes, button transitions, clipboard
  messages, and host→browser cursor shape updates.

**Clipboard** — bidirectional `text/plain` and `image/png` over the `control`
channel. Payloads > 8 MB are refused. All guest-visible content (cursor
bitmaps, clipboard, audio, framebuffer) travels exclusively over the WebRTC
DTLS+SRTP DataChannels; the cloud WebSocket relay is treated as an untrusted
plaintext channel.

**ICE servers** — defaults (CN-reachable order):

```
stun:stun.qq.com:3478
stun:stun.miwifi.com:3478
stun:stun.cloudflare.com:3478
```

Override with `AGENTSPHERE_ICE_SERVERS` (JSON array of W3C `RTCIceServer` objects,
supports TURN with credentials) or the legacy `AGENTSPHERE_STUN_SERVERS`
(comma-separated STUN URIs).

## Telemetry

`resource_monitor.cpp` reads host and per-VM metrics every 30 seconds and
pushes them over the cloud tunnel:

- `host.resources_tick` — CPU, memory (total/available/VM-used), disk
  (data-dir filesystem + image cache + per-VM), daemon version and uptime,
  encoder capabilities, `arch` ("amd64"/"arm64"), `os_release`, `glibc_version`.
- `vm.resources_tick` — per-VM runtime state, RSS memory, CPU %, PID, uptime,
  display/audio session state.

On tunnel reconnect, the daemon immediately pushes a fresh `host.resources_tick`
so a newly-connected browser sees current state without waiting up to 30 s.

## Self-update

`host_updater.cpp` handles the `host.update` cloud message:

1. Refuses if any VM is in `starting` / `running` / `stopping` / `rebooting`
   state — returns `vms_running` with the offending list.
2. Confirms the binary is dpkg-managed (checks for `tenbox.list` and
   `/usr/local/bin/agentsphered`).
3. Runs `apt-get update && apt-get install -y --only-upgrade tenbox` on a worker
   thread; streams the transcript to `/var/lib/tenbox/logs/update.log`.
4. Sends the reply envelope **before** `dpkg postinst` calls
   `deb-systemd-invoke restart agentsphered`, so the cloud receives a structured
   result rather than a bare connection close.

Manual equivalent: `sudo apt-get install --only-upgrade tenbox`.

## LLM proxy

An OpenAI-compatible HTTP reverse proxy runs in-process on a configurable port
(`LlmProxySettings::listen_port`, default 0 = auto-pick). Guest VMs direct
their LLM API requests to the host IP (gateway `10.0.2.2`) on that port. The
daemon rewrites the `model` field, injects the `Authorization` header from the
configured API key, and forwards to the upstream provider URL.

Settings persist in `<data_dir>/host_settings.json`. Mappings can be
configured via the `host.llm_proxy.set` cloud message or the local
`host.llm_proxy.set` RPC.

## KVM doctor

`tenbox doctor` (CLI) and `agentsphered --doctor` run `kvm_doctor.cpp`, which
checks:

- CPU virtualisation flags (`vmx`/`svm` on x86_64; EL2 on arm64)
- `/dev/kvm` existence and read/write permission
- Required kernel modules

Output is structured JSON; exit code 2 means unsupported.

## Data layout

```
/var/lib/tenbox/        (default; overridable via AGENTSPHERE_DATA_DIR / --data-dir)
├── vms/
│   └── <vm-id>/
│       ├── vm.json           VM spec and runtime state
│       ├── runtime_state.json
│       ├── crash/            minidump / backtrace captures
│       └── logs/             bounded VM console/runtime log ring
├── images/
│   └── <image-id>/           downloaded kernel, initramfs, rootfs
├── logs/
│   └── update.log            self-update transcript
├── host_settings.json        LLM proxy config
└── device.token              cloud device token (mode 0600)
```

## Environment variables

| Variable | Default | Effect |
| --- | --- | --- |
| `AGENTSPHERE_CLOUD_URL` | `wss://my.tenbox.ai/api/device-tunnel` | Cloud tunnel endpoint; set to empty to disable |
| `AGENTSPHERE_DATA_DIR` | `/var/lib/tenbox` | Daemon data directory |
| `AGENTSPHERE_SOCK` | — | Override CLI socket path |
| `AGENTSPHERE_SOCKET_GROUP` | — | Group to chown/chmod the socket to after `Listen()` |
| `AGENTSPHERE_ICE_SERVERS` | — | JSON array of W3C `RTCIceServer` objects |
| `AGENTSPHERE_STUN_SERVERS` | — | Comma-separated STUN URIs (legacy; used if `AGENTSPHERE_ICE_SERVERS` unset) |
| `AGENTSPHERE_WEBRTC_WORKER_THREADS` | `4` | libdatachannel thread pool size (`0` = `hardware_concurrency`) |
| `AGENTSPHERE_ENCODER_THREADS` | `1` | FFmpeg encoder threads per session (`0` = auto) |
