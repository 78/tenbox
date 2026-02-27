#!/bin/bash
# Extract a Debian vmlinuz for TenBox testing.
# Only requires curl, xz-utils, and dpkg-deb — no apt/root needed.
#
# Usage:
#   ./get-kernel.sh [output_dir] [suite]
#     output_dir  - where to place vmlinuz (default: ../build/share)
#     suite       - Debian suite: bookworm(6.x), bullseye(5.x), trixie, etc.
#                   Default: bookworm
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTDIR="$(mkdir -p "${1:-$SCRIPT_DIR/../build/share}" && cd "${1:-$SCRIPT_DIR/../build/share}" && pwd)"
SUITE="${2:-bookworm}"
WORKDIR=$(mktemp -d)
trap "rm -rf $WORKDIR" EXIT

MIRROR="https://deb.debian.org/debian"
ARCH="amd64"
PACKAGES_URL="$MIRROR/dists/$SUITE/main/binary-$ARCH/Packages.gz"

cd "$WORKDIR"

echo "[1/3] Fetching package index for $SUITE ..."
curl -fsSL "$PACKAGES_URL" | gunzip > Packages

# Resolve linux-image-amd64 -> actual versioned package name
META_BLOCK=$(awk '/^Package: linux-image-amd64$/,/^$/' Packages)
REAL_PKG=$(echo "$META_BLOCK" | grep -oP '^Depends:.*\Klinux-image-[0-9]\S+')
if [ -z "$REAL_PKG" ]; then
    echo "Error: could not resolve kernel package from $SUITE." >&2
    exit 1
fi

# Find the .deb path for the resolved package
DEB_PATH=$(awk "/^Package: ${REAL_PKG}$/,/^$/" Packages | grep -oP '^Filename: \K.*')
if [ -z "$DEB_PATH" ]; then
    echo "Error: could not find .deb path for $REAL_PKG." >&2
    exit 1
fi
DEB_URL="$MIRROR/$DEB_PATH"
echo "    -> $REAL_PKG"
echo "    -> $DEB_URL"

echo "[2/3] Downloading & extracting vmlinuz ..."
curl -fsSL -o kernel.deb "$DEB_URL"
dpkg-deb -x kernel.deb extract/
cp extract/boot/vmlinuz-* vmlinuz

echo "[3/3] Copying output ..."
cp vmlinuz "$OUTDIR/vmlinuz"
echo "Done: $OUTDIR/vmlinuz ($(du -h "$OUTDIR/vmlinuz" | cut -f1))"
