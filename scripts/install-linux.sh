#!/usr/bin/env sh
# TenBox one-line installer for Debian-derived systems.
#
#   curl -fsSL https://tenbox.ai/install.sh | sh
#
# Responsibilities (in order):
#  1. Validate platform (Debian-derivative + amd64/arm64 + glibc >= 2.35
#     + KVM available).
#  2. Install runtime apt dependencies that the deb does not bundle.
#  3. Register the TenBox apt repo and apt-get install tenbox.
#  4. Drop /etc/tenbox/tenboxd.env with the cloud tunnel URL.
#  5. Enable + start the systemd unit (the deb installs it but does not
#     enable it; the env file is only known to be valid here).
#  6. Tail journalctl until the pairing URL appears, then print it for
#     the operator.
#
# The previous tarball-based install path is gone: tenbox now ships as
# a single deb (tenbox_<ver>_{amd64,arm64}.deb) and `host.update` from
# the cloud console relies on apt being the source of truth. To pin to
# a specific version, set TENBOX_RELEASE_TAG and the script will
# `apt-get install tenbox=<version>` at the matching point release.
#
# Environment overrides (all optional):
#   TENBOX_CLOUD_URL       Cloud tunnel URL (default: wss://my.tenbox.ai/api/device-tunnel)
#   TENBOX_DATA_DIR        Daemon data dir (default: /var/lib/tenbox)
#   TENBOX_APT_REPO_URL    apt repo base URL (default: https://my.tenbox.ai/repo)
#   TENBOX_APT_SUITE       apt suite name (default: stable)
#   TENBOX_RELEASE_TAG     pin to vX.Y.Z (default: latest available)

set -eu

data_dir="${TENBOX_DATA_DIR:-/var/lib/tenbox}"
cloud_url="${TENBOX_CLOUD_URL:-wss://my.tenbox.ai/api/device-tunnel}"
repo_url="${TENBOX_APT_REPO_URL:-https://my.tenbox.ai/repo}"
suite="${TENBOX_APT_SUITE:-stable}"
release_tag="${TENBOX_RELEASE_TAG:-latest}"

die() {
    echo "tenbox install: $*" >&2
    exit 1
}

# Progress lines so the operator sees something between long apt steps
# instead of staring at a blank terminal for 10-30s while we run a
# silenced `apt-get update` / `apt-get install`.
step() {
    echo "==> $*"
}

require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        die "please run as root (e.g. via sudo)"
    fi
}

check_platform() {
    step "Checking platform compatibility..."
    if [ ! -r /etc/os-release ]; then
        die "/etc/os-release not found; this installer only supports Debian/Ubuntu derivatives"
    fi
    # shellcheck disable=SC1091
    . /etc/os-release
    id_like="${ID:-} ${ID_LIKE:-}"
    case "$id_like" in
        *debian*) ;;
        *) die "unsupported distro: ${PRETTY_NAME:-unknown}; install manually" ;;
    esac

    # arch must match a deb we publish. Stash it in a global so
    # check_kvm can branch on it (vmx/svm only exists on x86_64;
    # ARMv8 exposes virtualization through EL2 instead and there is
    # no equivalent flag in /proc/cpuinfo).
    arch="$(dpkg --print-architecture 2>/dev/null || true)"
    case "$arch" in
        amd64|arm64) ;;
        "") die "could not determine system architecture (dpkg unavailable?)" ;;
        *) die "tenbox supports amd64/arm64 only (got $arch)" ;;
    esac

    # glibc is the real compat boundary, not the codename. Reject hosts
    # whose loader is too old to map our bullseye-based binaries. The
    # build-base image (packaging/build-base/Dockerfile.bullseye) is
    # debian:11 with glibc 2.31, so anything from Debian 11 / Ubuntu
    # 20.04 onwards is supported. The deb itself only Depends on
    # libc6 (>= 2.31) + ca-certificates because OpenSSL, libstdc++,
    # and libgcc are all linked statically.
    if command -v ldd >/dev/null 2>&1; then
        glibc_ver="$(ldd --version 2>/dev/null | head -1 | awk '{print $NF}')"
        if [ -n "$glibc_ver" ]; then
            major="$(printf '%s' "$glibc_ver" | awk -F. '{print $1}')"
            minor="$(printf '%s' "$glibc_ver" | awk -F. '{print $2}')"
            if [ -n "$major" ] && [ -n "$minor" ]; then
                if [ "$major" -lt 2 ] || { [ "$major" -eq 2 ] && [ "$minor" -lt 31 ]; }; then
                    die "tenbox requires glibc 2.31+ (your system: $glibc_ver). Upgrade to Debian 11+ / Ubuntu 20.04+ / Raspberry Pi OS 11+ first."
                fi
            fi
        fi
    fi
}

check_kvm() {
    step "Checking KVM support..."
    # On x86_64 the VT-x / AMD-V capability shows up in /proc/cpuinfo
    # as the vmx (Intel) or svm (AMD) flag, and missing it is a hard
    # failure that no amount of kernel module loading will fix.
    #
    # On ARMv8 there is no analogous flag: KVM relies on the CPU
    # being able to enter EL2 (the hypervisor exception level),
    # which is not advertised through /proc/cpuinfo. The only
    # reliable signal there is whether the kernel actually exposes
    # /dev/kvm, so we skip the cpuinfo grep on arm64.
    if [ "$arch" = "amd64" ]; then
        if ! grep -Eq '(vmx|svm)' /proc/cpuinfo 2>/dev/null; then
            die "KVM unsupported: CPU virtualization flag vmx/svm was not found (enable VT-x/AMD-V in BIOS?)"
        fi
    fi
    [ -e /dev/kvm ] || die "KVM unsupported: /dev/kvm does not exist (load kvm_intel/kvm_amd on x86, or boot a kernel with KVM enabled on arm64)"
    if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
        die "KVM unsupported: /dev/kvm is not r/w; check permissions"
    fi
}

apt_install_deps() {
    export DEBIAN_FRONTEND=noninteractive
    step "Refreshing apt package index..."
    apt-get update -y >/dev/null
    step "Ensuring ca-certificates is installed..."
    # Only ca-certificates is actually required here: apt itself
    # talks to the HTTPS repo registered below, and the trust store
    # has to be in place before that fetch runs. The deb's own
    # runtime deps (libc6 / libssl3{,t64}) are pulled in by the
    # later `apt-get install tenbox` step.
    apt-get install -y --no-install-recommends ca-certificates >/dev/null
}

register_apt_repo() {
    step "Registering TenBox apt repository ($repo_url $suite)..."
    install -d -m 0755 /etc/apt/sources.list.d
    install -d -m 0755 /etc/apt/keyrings

    # The expected sha256 of the public archive keyring. Bumped when
    # the TenBox release key is rotated; clients with a stale value
    # will refuse to register the repo until they re-run a fresh
    # install-linux.sh. Compare against the file checked in at
    # scripts/keys/tenbox-archive-keyring.gpg in the public repo.
    keyring_url="$repo_url/tenbox-archive-keyring.gpg"
    keyring_path="/etc/apt/keyrings/tenbox-archive-keyring.gpg"
    expected_sha256="22b124e3370f54335c8588a4a0a672f22916970714be0c41c540e995be7a7e2d"

    step "Fetching TenBox archive keyring..."
    tmp_keyring="$(mktemp)"
    if ! curl -fsSL "$keyring_url" -o "$tmp_keyring"; then
        rm -f "$tmp_keyring"
        die "could not download archive keyring from $keyring_url"
    fi
    actual_sha256="$(sha256sum "$tmp_keyring" | awk '{print $1}')"
    # Refuse to register the repo if the bytes we just downloaded
    # don't match the value baked into this script. This catches
    # MITM that swaps the keyring AND the deb in lockstep, since the
    # attacker would need to also patch this script before it ran.
    if [ "$actual_sha256" != "$expected_sha256" ]; then
        rm -f "$tmp_keyring"
        die "archive keyring sha256 mismatch (got $actual_sha256, expected $expected_sha256)"
    fi
    install -m 0644 "$tmp_keyring" "$keyring_path"
    rm -f "$tmp_keyring"

    cat > /etc/apt/sources.list.d/tenbox.list <<EOF
deb [signed-by=$keyring_path] $repo_url $suite main
EOF
    apt-get update -y >/dev/null
}

# Print the tail of apt's term.log so the operator can see WHY the
# install actually failed. We swallow apt's stdout/stderr above (to
# keep the happy path quiet), so without this hook a dpkg error like
# "unknown compression for member control.tar.zst" or a postinst
# failure shows up only as a bare "Sub-process /usr/bin/dpkg returned
# an error code (1)" — useless for triage.
print_apt_failure_log() {
    apt_log="/var/log/apt/term.log"
    echo
    echo "tenbox install: apt-get failed. last lines of $apt_log:" >&2
    if [ -r "$apt_log" ]; then
        tail -n 40 "$apt_log" >&2 || true
    else
        echo "  (log not readable; try: sudo tail -n 80 $apt_log)" >&2
    fi
    echo >&2
}

apt_install_tenbox() {
    export DEBIAN_FRONTEND=noninteractive

    # tenboxd is a self-contained KVM-based VMM (it talks to /dev/kvm
    # directly and ships its own virtio + qcow2 + lwIP NAT), so we do
    # NOT pull in qemu-system-* here. --no-install-recommends keeps
    # the install set minimal (only libc6 / libssl3{,t64} /
    # ca-certificates land via the deb's Depends).
    if [ "$release_tag" = "latest" ]; then
        step "Installing tenbox (latest)..."
        if ! apt-get install -y --no-install-recommends tenbox >/dev/null; then
            print_apt_failure_log
            die "apt could not install tenbox (see log above)"
        fi
    else
        # Strip the leading `v` so apt sees the bare version number.
        version="${release_tag#v}"
        step "Installing tenbox=$version..."
        if ! apt-get install -y --no-install-recommends \
                "tenbox=$version" >/dev/null; then
            print_apt_failure_log
            die "apt could not install tenbox=$version (not in repo yet?)"
        fi
    fi
}

write_env_file() {
    step "Writing /etc/tenbox/tenboxd.env..."
    install -d -m 0755 /etc/tenbox
    cat > /etc/tenbox/tenboxd.env <<EOF
# Managed by scripts/install-linux.sh; safe to edit manually.
# Re-run the installer (or re-create this file) to restore defaults.
TENBOX_CLOUD_URL=$cloud_url
TENBOX_DATA_DIR=$data_dir
EOF
    chmod 0644 /etc/tenbox/tenboxd.env
    install -d -m 0755 "$data_dir"
}

grant_group_access() {
    # The deb's postinst created the `tenbox` system group; tenboxd
    # chgrp's its socket to it. Add the human who ran sudo (SUDO_USER)
    # so they can `tenbox vm ls` without sudo from their next shell.
    # Skipped when SUDO_USER is unset (e.g. when running as bare root
    # over ssh) — that user already has access.
    if [ -z "${SUDO_USER:-}" ] || [ "$SUDO_USER" = "root" ]; then
        return
    fi
    if ! getent group tenbox >/dev/null; then
        # Should never happen — postinst makes the group — but if it
        # does, leaving it silent is fine: the user will hit a clear
        # EACCES on `tenbox vm ls` which is easier to debug than a
        # mysterious "group not found".
        return
    fi
    if id -nG "$SUDO_USER" 2>/dev/null | tr ' ' '\n' | grep -qx tenbox; then
        return
    fi
    if usermod -aG tenbox "$SUDO_USER" 2>/dev/null; then
        echo
        echo "added user '$SUDO_USER' to group 'tenbox'."
        echo "  log out and back in (or run \`newgrp tenbox\`) before"
        echo "  using the \`tenbox\` CLI without sudo."
    fi
}

enable_systemd() {
    # The deb installs the unit at /lib/systemd/system/tenboxd.service
    # but deliberately does not enable it, because the env file above
    # is what makes the unit actually safe to launch with the right
    # cloud URL / data dir.
    #
    # `enable --now` would only `start` (not `restart`) the service,
    # which is fine on a brand-new install. But on a re-install the
    # daemon may already be active from a previous run; in that case
    # `--now` becomes a no-op and the freshly written env file is
    # ignored. Force a restart so the new env always wins.
    step "Enabling and starting tenboxd.service..."
    systemctl daemon-reload
    systemctl enable tenboxd >/dev/null
    systemctl restart tenboxd
}

await_pair_url() {
    echo
    echo "tenboxd is starting. Waiting for the pairing URL (up to 60s)..."
    pair_url=""
    seen_token=0
    for _ in $(seq 1 60); do
        if journalctl -u tenboxd --no-pager --since "-2 min" 2>/dev/null \
            | grep -q "cloud pairing complete"; then
            seen_token=1
            break
        fi
        line="$(journalctl -u tenboxd --no-pager --since "-2 min" 2>/dev/null \
            | grep -oE 'https?://[^ ]+/pair\?code=[0-9]{8}' | tail -n 1)"
        if [ -n "$line" ]; then
            pair_url="$line"
            break
        fi
        sleep 1
    done

    if [ "$seen_token" -eq 1 ]; then
        echo "this host is already paired; tenboxd is running."
        echo "open https://my.tenbox.ai/ to manage it."
        return
    fi
    if [ -n "$pair_url" ]; then
        echo
        echo "============================================================"
        echo " open this URL in a browser to bind this host:"
        echo "   $pair_url"
        echo "============================================================"
    else
        echo
        echo "could not find a pairing URL in journalctl. inspect:"
        echo "   sudo journalctl -u tenboxd -n 50"
    fi
}

echo "TenBox installer starting (this can take 30-60s on a fresh host)..."
require_root
check_platform
check_kvm
apt_install_deps
register_apt_repo
apt_install_tenbox
grant_group_access
write_env_file
enable_systemd
await_pair_url
