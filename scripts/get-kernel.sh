#!/bin/bash
# Extract a Debian vmlinuz for TenClaw testing.
# Run this in WSL2 or a Debian-based Linux environment.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTDIR="$(mkdir -p "${1:-$SCRIPT_DIR/../build}" && cd "${1:-$SCRIPT_DIR/../build}" && pwd)"
WORKDIR=$(mktemp -d)
trap "rm -rf $WORKDIR" EXIT

echo "[1/3] Resolving actual kernel package from meta-package..."
cd "$WORKDIR"
REAL_PKG=$(apt-cache depends linux-image-amd64 2>/dev/null \
    | grep -oP 'Depends:\s+\Klinux-image-[0-9].*')
if [ -z "$REAL_PKG" ]; then
    echo "Error: could not resolve kernel package name." >&2
    exit 1
fi
echo "    -> $REAL_PKG"

echo "[2/3] Downloading & extracting vmlinuz..."
apt-get download "$REAL_PKG" 2>/dev/null || \
    apt download "$REAL_PKG" 2>/dev/null
dpkg-deb -x linux-image-*.deb extract/
cp extract/boot/vmlinuz-* vmlinuz

echo "[3/3] Copying output..."
cp vmlinuz "$OUTDIR/vmlinuz"
echo "Done: $OUTDIR/vmlinuz ($(du -h "$OUTDIR/vmlinuz" | cut -f1))"
