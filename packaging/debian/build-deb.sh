#!/bin/bash
# Pack the prebuilt tenbox / agentsphered / agentsphere-vm-runtime binaries into a
# .deb. Expects the binaries to already exist under <build_dir>/ (i.e.
# `cmake --build` has run and stripped them); does NOT build anything
# itself, because the release workflow runs the cmake step inside the
# packaging/build-base/Dockerfile.jammy container.
#
# Usage: build-deb.sh <arch> <version> [build_dir]

set -euo pipefail

ARCH="${1:?usage: build-deb.sh <arch> <version> [build_dir]}"
VERSION="${2:?usage: build-deb.sh <arch> <version> [build_dir]}"
BUILD_DIR="${3:-build}"

case "$ARCH" in
    amd64|arm64) ;;
    *) echo "build-deb: unsupported arch '$ARCH' (expected amd64 or arm64)" >&2; exit 2 ;;
esac

if [ ! -x "$BUILD_DIR/tenbox" ] || [ ! -x "$BUILD_DIR/agentsphered" ] || [ ! -x "$BUILD_DIR/agentsphere-vm-runtime" ]; then
    echo "build-deb: required binaries missing under $BUILD_DIR" >&2
    echo "expected: tenbox, agentsphered, agentsphere-vm-runtime" >&2
    exit 2
fi

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PACKAGING_DIR="$REPO_ROOT/packaging/debian"
SYSTEMD_UNIT="$REPO_ROOT/packaging/systemd/agentsphered.service"

if [ ! -f "$SYSTEMD_UNIT" ]; then
    echo "build-deb: missing systemd unit at $SYSTEMD_UNIT" >&2
    exit 2
fi

PKG_NAME="tenbox_${VERSION}_${ARCH}"
STAGE_DIR="$(mktemp -d)"
trap 'rm -rf "$STAGE_DIR"' EXIT

DEB_ROOT="$STAGE_DIR/$PKG_NAME"
mkdir -p "$DEB_ROOT/DEBIAN"
mkdir -p "$DEB_ROOT/usr/local/bin"
mkdir -p "$DEB_ROOT/lib/systemd/system"

# Drop binaries (already stripped by the workflow's strip step).
install -m 0755 "$BUILD_DIR/tenbox"            "$DEB_ROOT/usr/local/bin/tenbox"
install -m 0755 "$BUILD_DIR/agentsphered"           "$DEB_ROOT/usr/local/bin/agentsphered"
install -m 0755 "$BUILD_DIR/agentsphere-vm-runtime" "$DEB_ROOT/usr/local/bin/agentsphere-vm-runtime"

# systemd unit. Same content the install script previously dropped under
# /etc/systemd/system; moving it into the deb means upgrades pick up unit
# changes for free.
install -m 0644 "$SYSTEMD_UNIT" "$DEB_ROOT/lib/systemd/system/agentsphered.service"

# Render control file from template.
sed \
    -e "s/@VERSION@/$VERSION/g" \
    -e "s/@ARCH@/$ARCH/g" \
    "$PACKAGING_DIR/control.in" > "$DEB_ROOT/DEBIAN/control"

install -m 0755 "$PACKAGING_DIR/postinst" "$DEB_ROOT/DEBIAN/postinst"
install -m 0755 "$PACKAGING_DIR/prerm"    "$DEB_ROOT/DEBIAN/prerm"
install -m 0755 "$PACKAGING_DIR/postrm"   "$DEB_ROOT/DEBIAN/postrm"

# Optional sanity: complain (loudly, not fatally) if the daemon binary
# still has dynamic deps outside the documented set. Catches regressions
# where a developer accidentally adds a new shared dep without updating
# control.in.
#
# After the bullseye base + AGENTSPHERE_STATIC_RUNTIME switch the only DT_NEEDED
# entries that should remain are the libc/threads/loader trio. If you see
# libssl, libcrypto, libstdc++, or libgcc_s reappear here it means
# AGENTSPHERE_STATIC_FFMPEG / AGENTSPHERE_STATIC_RUNTIME did not take effect, the
# OPENSSL_ROOT_DIR hint missed, or libdatachannel pulled in the system
# /usr/lib OpenSSL behind our backs — in which case the bullseye
# compatibility promise (single deb works on libssl1.1 hosts) breaks.
if command -v ldd >/dev/null 2>&1; then
    # awk strips directory components so /lib64/ld-linux-x86-64.so.2
    # collapses to ld-linux-x86-64.so.2 and matches the whitelist.
    UNEXPECTED=$(
        ldd "$BUILD_DIR/agentsphered" 2>/dev/null \
            | awk '{print $1}' \
            | awk -F/ '{print $NF}' \
            | grep -E '\.so' \
            | grep -vE '^(linux-vdso|libc\.so|libm\.so|libdl\.so|libpthread\.so|librt\.so|ld-linux-(x86-64|aarch64)\.so)' \
            || true
    )
    if [ -n "$UNEXPECTED" ]; then
        echo "build-deb: WARNING agentsphered has unexpected dynamic deps:" >&2
        echo "$UNEXPECTED" >&2
        echo "build-deb: update packaging/debian/control.in if these are intentional," >&2
        echo "           or fix the static-link config so they go away." >&2
    fi
fi

# Build with xz compression. We MUST pass -Z explicitly: starting with
# dpkg 1.21 (Debian 12 / Ubuntu 22.04+) dpkg-deb's default switched to
# zstd, but Bullseye's dpkg 1.20 cannot read control.tar.zst and rejects
# the package with "unknown compression for member control.tar.zst".
# Since our supported floor is Debian 11 / Ubuntu 20.04 (see
# scripts/install-linux.sh's glibc preflight), forcing xz here keeps
# the single deb installable across the whole supported range.
# Keep root:root ownership inside the archive.
DIST_DIR="$REPO_ROOT/dist"
mkdir -p "$DIST_DIR"
DEB_OUT="$DIST_DIR/${PKG_NAME}.deb"

dpkg-deb -Z xz --root-owner-group --build "$DEB_ROOT" "$DEB_OUT"
( cd "$DIST_DIR" && sha256sum "$(basename "$DEB_OUT")" > "$(basename "$DEB_OUT").sha256" )

echo "wrote $DEB_OUT"
echo "wrote $DEB_OUT.sha256"
