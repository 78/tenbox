# Linux update path

`tenboxd` ships as a single Debian package (`tenbox_<ver>_{amd64,arm64}.deb`)
served from the TenBox apt repository at `https://my.tenbox.ai/repo`. The
end-to-end update story has three personas:

- **Operator** at the cloud console: clicks "升级" on a host badge and
  watches the daemon come back online a minute later.
- **Sysadmin** at the host shell: runs `apt-get install --only-upgrade tenbox`
  manually, or pins to a specific version with `apt-get install tenbox=0.7.5`.
- **Maintainer**: cuts a Git tag, the [`release.yml`](../.github/workflows/release.yml)
  workflow uploads new debs, and `tenbox-apt-sync.timer` republishes the
  apt repo within ~10 minutes (see [`docs/release.md`](release.md) for the
  step-by-step).

## Compatibility matrix

A single deb covers every Debian-derived distribution with
`glibc >= 2.35` and `libssl3` available, on both `amd64` and `arm64`.
That includes:

| Distro                     | amd64 | arm64 |
| -------------------------- | :---: | :---: |
| Debian 12 (bookworm)       |   ✅  |   ✅  |
| Debian 13 (trixie)         |   ✅  |   ✅  |
| Ubuntu 22.04 LTS (jammy)   |   ✅  |   ✅  |
| Ubuntu 24.04 LTS (noble)   |   ✅  |   ✅  |
| Raspberry Pi OS 12         |   —   |   ✅  |
| Armbian Bookworm           |   —   |   ✅  |

The `install-linux.sh` preflight refuses anything older. Hosts also
report their `arch` and `glibc_version` to the cloud on every
`host.resources_tick`, so the console can grey out the upgrade button
before the daemon's own preflight kicks in.

KVM is required: x86_64 needs `vmx` or `svm` in `/proc/cpuinfo`; arm64
hosts need an EL2-enabled kernel exposing `/dev/kvm`.

## Operator flow (cloud-driven)

1. The cloud console polls `/api/releases/linux/latest` and shows a
   `升级到 X.Y.Z` pill on `HostBadge` and a `可升级` tag on the device
   list when `daemon_version < latest_version`.
2. Clicking the pill opens `HostUpdateModal`. The modal does three
   client-side prechecks (running VMs, platform compatibility, host
   online) before enabling the confirm button. None of these are
   authoritative — the daemon repeats them.
3. `POST /api/hosts/:hostId/update` forwards a `host.update` envelope
   over the device tunnel. The cloud widens its device-request timeout
   to 120s for this command alone.
4. The daemon refuses if any VM is in `starting`/`running`/`stopping`/
   `rebooting`, returning `vms_running` with the offending list. Else
   it confirms the binary is dpkg-managed and runs
   `apt-get update && apt-get install -y --only-upgrade tenbox` on a
   worker thread, streaming the transcript to `/var/lib/tenbox/logs/update.log`.
5. The reply lands on the wire **before** dpkg's `postinst` calls
   `deb-systemd-invoke restart tenboxd`, so the cloud sees structured
   success rather than a bare connection close.
6. The console flips into "等待重启" mode and polls `/api/hosts` every
   3s. When the host comes back online with `daemon_version === target`,
   the modal flashes a success toast.

## Manual upgrade

```sh
sudo apt-get update
sudo apt-get install --only-upgrade tenbox
```

To pin a specific version:

```sh
sudo apt-get install tenbox=0.7.5
```

The package's `postinst` reloads systemd and bounces `tenboxd` only if
the unit was already enabled — fresh installs are still gated on
`scripts/install-linux.sh` writing `/etc/tenbox/tenboxd.env` and calling
`systemctl enable + restart tenboxd` (note: not `enable --now`, because
on a re-install `--now` would no-op when the daemon is already active
and the freshly written env file would never be loaded). Symmetrically,
`prerm` runs `systemctl disable tenboxd` so a subsequent
`apt install tenbox` doesn't see a stale enabled state and start the
daemon before the env file lands.

## Socket permissions and the `tenbox` group

`tenboxd` listens on `/run/tenbox/tenbox.sock`. The deb's `postinst`
creates a system group `tenbox` and a system user `tenbox` (the user
is reserved for a future drop-priv step; today the daemon still runs
as root for `/dev/kvm` and `apt-get`). At startup the daemon:

1. Lets systemd create `/run/tenbox/` mode `0755` (owned `root:root`).
2. After `Listen()` succeeds, reads `TENBOX_SOCKET_GROUP` from the
   environment (set to `tenbox` by the unit file), and `chown :tenbox`
   + `chmod 0660` on the socket file itself.

The result mirrors libvirt and docker: the directory is
world-traversable so any user can `stat(2)` the socket and the CLI
can give an honest `permission denied` error rather than a misleading
`no such file`, but only members of the `tenbox` group can `connect(2)`.
The installer adds `$SUDO_USER` to the group automatically; new
shells (or `newgrp tenbox`) pick up the membership.

CLI default-socket lookup is in `client.cpp:DefaultSocketPath()` and
goes: `$TENBOX_SOCK` → `/run/tenbox/tenbox.sock` (if `/run/tenbox`
exists) → `$XDG_RUNTIME_DIR/tenbox.sock` (per-user dev daemon) →
`/tmp/tenbox-<uid>.sock` (last-resort fallback).

## Stop all VMs first

The cloud-driven path strictly refuses to upgrade while any VM is
non-stopped (decision: "refuse with list"). To clean up before
upgrading:

```sh
tenbox vm list
tenbox vm stop <vm-id>   # repeat for each running VM
```

…then re-trigger the upgrade from the cloud console (or run apt by
hand). There is intentionally no "force" flag.

## Static link & GPL note

`tenboxd` is built inside the [`packaging/build-base/Dockerfile.jammy`](../packaging/build-base/Dockerfile.jammy)
container against `glibc 2.35` with FFmpeg, libx264, libopus, libyuv,
and libcurl linked statically. As a result:

- `apt show tenbox | grep -E '^(Depends|Suggests)'` lists:
  - **Depends:** `libc6 (>= 2.35), libssl3t64 | libssl3, ca-certificates`
  - **Suggests:** `qemu-utils` (only needed when building custom rootfs templates with `scripts/*/make-rootfs-*.sh`)
- `tenboxd` is a self-contained KVM-based VMM: it opens `/dev/kvm`
  directly and ships its own virtio devices, qcow2 backend, and
  lwIP-based user-mode NAT. There is **no** runtime dependency on
  `qemu-system-*`, `qemu-block-extra`, or any of the
  Ceph/Gluster/RBD/NFS/RDMA tail (`librados2`, `librbd1`,
  `libgfapi0`, `libibverbs1`, `libnfs14`, ...) that `qemu-utils`
  used to drag in via Recommends.
- Both `scripts/install-linux.sh` and the daemon self-update path
  (`host_updater.cpp`) pass `--no-install-recommends` so a future
  Recommends entry can never silently bloat the install set.
- `tenboxd` does not pick up host-side `ffmpeg`/`libavcodec` upgrades.
  Behaviour is pinned to whatever was baked into the deb on release day,
  which is the explicit goal: "ffmpeg version on the host" stops being
  a debugging variable.
- libx264 is GPL-licensed; tenbox is GPLv3 (see top-level
  [`LICENSE`](../LICENSE)), so static linkage is fine. Switching the
  build to OpenH264 only makes sense if tenbox itself relicenses, which
  is explicitly out of scope.

## Where to look when something breaks

| Symptom                                | Where to look                                                       |
| -------------------------------------- | ------------------------------------------------------------------- |
| Console never shows "可升级" pill      | `/api/releases/linux/latest` → cloud `tenbox-apt-sync.timer` status |
| Modal disabled with "VM 在运行"        | `tenbox vm list`, stop each one, retry                              |
| Modal: `update_disabled`               | Daemon binary not at `/usr/local/bin/tenboxd` or no `tenbox.list`   |
| Modal: `apt_failed`                    | `/var/lib/tenbox/logs/update.log` on the host                       |
| Modal: `version_unchanged`             | Apt repo not yet synced; rerun `sync_apt_repo.py` on the cloud      |
| Daemon never reconnects after restart  | `journalctl -u tenboxd -n 200` on the host                          |
