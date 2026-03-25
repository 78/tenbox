#!/bin/bash
# Extract a Debian arm64 kernel Image for TenBox macOS (AArch64).
# Only requires curl, xz-utils, and dpkg-deb — no apt/root needed.
#
# The AArch64 Linux kernel uses a flat "Image" binary (not compressed vmlinuz).
# This script downloads the Debian arm64 kernel package and extracts it.
#
# Usage:
#   ./get-kernel.sh [output_dir] [suite]
#     output_dir  - where to place Image (default: ../build/share)
#     suite       - Debian suite: trixie(6.12.x), bookworm(6.1.x), etc.
#                   Default: trixie
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTDIR="$(mkdir -p "${1:-$SCRIPT_DIR/../../build/share}" && cd "${1:-$SCRIPT_DIR/../../build/share}" && pwd)"
SUITE="${2:-trixie}"
WORKDIR=$(mktemp -d)
trap "rm -rf $WORKDIR" EXIT

MIRROR="https://deb.debian.org/debian"
ARCH="arm64"
PACKAGES_URL="$MIRROR/dists/$SUITE/main/binary-$ARCH/Packages.gz"

cd "$WORKDIR"

echo "[1/3] Fetching package index for $SUITE ($ARCH) ..."
curl -fsSL "$PACKAGES_URL" | gunzip > Packages

# Resolve linux-image-arm64 -> actual versioned package name
META_BLOCK=$(awk '/^Package: linux-image-arm64$/,/^$/' Packages)
REAL_PKG=$(echo "$META_BLOCK" | sed -n 's/^Depends:.*\(linux-image-[0-9][^ ,]*\).*/\1/p')
if [ -z "$REAL_PKG" ]; then
    echo "Error: could not resolve kernel package from $SUITE." >&2
    exit 1
fi

# Find the .deb path for the resolved package
DEB_PATH=$(awk -v pkg="$REAL_PKG" '$0 == "Package: " pkg, /^$/' Packages | sed -n 's/^Filename: //p')
if [ -z "$DEB_PATH" ]; then
    echo "Error: could not find .deb path for $REAL_PKG." >&2
    exit 1
fi
DEB_URL="$MIRROR/$DEB_PATH"
echo "    -> $REAL_PKG"
echo "    -> $DEB_URL"

echo "[2/3] Downloading & extracting arm64 Image ..."
curl -fsSL -o kernel.deb "$DEB_URL"

# Extract .deb (ar archive) without dpkg-deb (macOS compatible)
mkdir -p extract
if command -v dpkg-deb &>/dev/null; then
    dpkg-deb -x kernel.deb extract/
else
    cd "$WORKDIR"
    ar x kernel.deb
    # data.tar may be .xz, .zst, or .gz
    DATA_TAR=$(ls data.tar.* 2>/dev/null | head -1)
    if [ -z "$DATA_TAR" ]; then
        echo "Error: no data.tar.* found in .deb" >&2
        exit 1
    fi
    case "$DATA_TAR" in
        *.xz)   xz -d "$DATA_TAR" && tar xf data.tar -C extract/ ;;
        *.gz)   gzip -d "$DATA_TAR" && tar xf data.tar -C extract/ ;;
        *.zst)  zstd -d "$DATA_TAR" && tar xf data.tar -C extract/ ;;
        *)      tar xf "$DATA_TAR" -C extract/ ;;
    esac
fi

# AArch64 uses uncompressed Image, not vmlinuz
VMLINUX_PATH=$(find extract/boot/ -name 'vmlinuz-*' -o -name 'Image-*' | head -1)
if [ -z "$VMLINUX_PATH" ]; then
    echo "Error: no kernel image found in package." >&2
    exit 1
fi

echo "[3/3] Copying output ..."
cp "$VMLINUX_PATH" "$OUTDIR/Image-arm64"
echo "Done: $OUTDIR/Image-arm64 ($(du -h "$OUTDIR/Image-arm64" | cut -f1))"
