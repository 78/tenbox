# Linux update path

`agentsphered` ships as a single Debian package (`tenbox_<ver>_{amd64,arm64}.deb`)
served from the AgentSphere apt repository at `https://my.tenbox.ai/repo`. Updates
can be triggered by the cloud control plane or applied manually by a sysadmin.

## Compatibility matrix

A single deb covers every Debian-derived distribution with `glibc >= 2.31`, on
both `amd64` and `arm64`. OpenSSL, libstdc++, and libgcc are statically linked
into the binary, so the deb does **not** depend on `libssl3` / `libssl3t64` /
`libstdc++6` from the host — it installs unchanged on bullseye (which only
ships `libssl1.1`) and trixie (which renamed to `libssl3t64`) alike.

| Distro | amd64 | arm64 |
| --- | :---: | :---: |
| Debian 11 (bullseye) | ✅ | ✅ |
| Debian 12 (bookworm) | ✅ | ✅ |
| Debian 13 (trixie) | ✅ | ✅ |
| Ubuntu 20.04 LTS (focal) | ✅ | ✅ |
| Ubuntu 22.04 LTS (jammy) | ✅ | ✅ |
| Ubuntu 24.04 LTS (noble) | ✅ | ✅ |
| Raspberry Pi OS 11 | — | ✅ |
| Raspberry Pi OS 12 | — | ✅ |
| Armbian Bullseye | — | ✅ |
| Armbian Bookworm | — | ✅ |

KVM is required: x86_64 needs `vmx` or `svm` in `/proc/cpuinfo`; arm64 hosts
need an EL2-enabled kernel exposing `/dev/kvm`.

## Cloud-driven upgrade

The cloud control plane can trigger a remote upgrade via the `host.update`
message. When the daemon receives this message:

1. **Refuses** if any VM is in `starting` / `running` / `stopping` / `rebooting`
   state — returns `vms_running` with the offending list.
2. **Confirms** the binary is dpkg-managed (checks for `tenbox.list` and the
   expected binary path).
3. **Runs** `apt-get update && apt-get install -y --only-upgrade tenbox` on a
   worker thread, streaming the transcript to `/var/lib/tenbox/logs/update.log`.
4. **Replies** before `dpkg postinst` calls `deb-systemd-invoke restart agentsphered`,
   so the cloud receives a structured result rather than a bare connection close.

Daemon error codes returned in the `host.update` reply:

| Code | Meaning |
| --- | --- |
| `vms_running` | One or more VMs were non-stopped; daemon returns the list |
| `update_disabled` | Binary not at expected path or apt source not configured |
| `apt_failed` | `apt-get install --only-upgrade tenbox` exited non-zero |
| `version_unchanged` | apt returned success but installed version is unchanged (apt repo may not be synced yet) |

## Manual upgrade

```sh
sudo apt-get update
sudo apt-get install --only-upgrade tenbox
```

To pin a specific version:

```sh
sudo apt-get install tenbox=0.7.5
```

## Stop all VMs first

The cloud-driven path refuses to upgrade while any VM is non-stopped (error
code `vms_running`). To clean up before upgrading:

```sh
tenbox vm ls
tenbox vm stop <vm-id>   # repeat for each running VM
```

…then re-trigger the upgrade from the control plane (or run apt by hand). There
is intentionally no `force` flag.

## Socket permissions and the `tenbox` group

`agentsphered` listens on `/run/tenbox/tenbox.sock`. The deb's `postinst` creates a
system group `tenbox` and a system user `tenbox` (the user is reserved for a
future drop-priv step; today the daemon still runs as root for `/dev/kvm` and
`apt-get`). At startup the daemon:

1. Lets systemd create `/run/tenbox/` mode `0755` (owned `root:root`).
2. After `Listen()` succeeds, reads `AGENTSPHERE_SOCKET_GROUP` from the environment
   (set to `tenbox` by the unit file), and `chown :tenbox` + `chmod 0660` on
   the socket file itself.

The result mirrors libvirt and docker: the directory is world-traversable so
any user can `stat(2)` the socket and the CLI can give an honest
`permission denied` error rather than a misleading `no such file`, but only
members of the `tenbox` group can `connect(2)`. The installer adds `$SUDO_USER`
to the group automatically; new shells (or `newgrp tenbox`) pick up the
membership.

CLI default-socket lookup is in `client.cpp:DefaultSocketPath()` and goes:
`$AGENTSPHERE_SOCK` → `/run/tenbox/tenbox.sock` (if `/run/tenbox` exists) →
`$XDG_RUNTIME_DIR/tenbox.sock` (per-user dev daemon) → `/tmp/tenbox-<uid>.sock`
(last-resort fallback).

## postinst / prerm behaviour

The package's `postinst` reloads systemd and bounces `agentsphered` only if the unit
was already enabled — fresh installs are still gated on `scripts/install-linux.sh`
writing `/etc/tenbox/agentsphered.env` and calling `systemctl enable + restart agentsphered`
(note: not `enable --now`, because on a re-install `--now` would no-op when the
daemon is already active and the freshly written env file would never be loaded).
Symmetrically, `prerm` runs `systemctl disable agentsphered` so a subsequent
`apt install tenbox` doesn't see a stale enabled state and start the daemon
before the env file lands.

## Static link & GPL note

`agentsphered` is built inside `packaging/build-base/Dockerfile.bullseye` against
glibc 2.31, with FFmpeg, libx264, libopus, libyuv, libcurl, and OpenSSL all
linked statically. The build also passes `-static-libstdc++ -static-libgcc`
(via `AGENTSPHERE_STATIC_RUNTIME=ON`) so the C++ runtime is baked into each
executable. As a result:

- `apt show tenbox | grep -E '^(Depends|Suggests)'` lists:
  - **Depends:** `libc6 (>= 2.31), ca-certificates`
  - **Suggests:** `qemu-utils` (only needed when building custom rootfs
    templates with `scripts/*/make-rootfs-*.sh`)
- `agentsphered` is self-contained: it opens `/dev/kvm` directly and ships its own
  virtio devices, qcow2 backend, and lwIP-based user-mode NAT. There is **no**
  runtime dependency on `qemu-system-*` or any of the Ceph/Gluster/NFS tail.
- Both `scripts/install-linux.sh` and `host_updater.cpp` pass
  `--no-install-recommends` so a future Recommends entry can never silently
  bloat the install set.
- `agentsphered` does not pick up host-side `ffmpeg`/`libavcodec` upgrades —
  behaviour is pinned to whatever was baked into the deb on release day.
- libx264 is GPL-licensed; tenbox is GPLv3 (see top-level `LICENSE`), so
  static linkage is fine.

## Troubleshooting

| Symptom | Where to look |
| --- | --- |
| Upgrade refused with `vms_running` | `tenbox vm ls`; stop each VM, then retry |
| `update_disabled` | Daemon binary path or `/etc/apt/sources.list.d/tenbox.list` missing |
| `apt_failed` | `cat /var/lib/tenbox/logs/update.log` on the host |
| `version_unchanged` after apt success | apt repo may not be synced yet; wait ~10 min or ask your cloud operator to sync manually |
| Daemon never reconnects after restart | `journalctl -u agentsphered -n 200` on the host |
