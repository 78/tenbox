#!/bin/bash
# Unified Docker-based build wrapper for TenBox rootfs/initramfs/kernel images.
#
# Usage:
#   ./scripts/docker/build.sh <arch> <target> [extra-args...]
#
# arch:   arm64 | x86_64
# target: rootfs-chromium | rootfs-copaw | rootfs-openclaw | initramfs | kernel
#
# Examples:
#   ./scripts/docker/build.sh arm64 rootfs-chromium
#   ./scripts/docker/build.sh arm64 rootfs-chromium --force
#   ./scripts/docker/build.sh arm64 rootfs-chromium --list-steps
#   ./scripts/docker/build.sh arm64 initramfs
#   ./scripts/docker/build.sh arm64 kernel
#   ./scripts/docker/build.sh x86_64 rootfs-chromium
#   ./scripts/docker/build.sh x86_64 rootfs-chromium --force
#   ./scripts/docker/build.sh x86_64 rootfs-copaw
#   ./scripts/docker/build.sh x86_64 rootfs-openclaw
#   ./scripts/docker/build.sh x86_64 initramfs
#   ./scripts/docker/build.sh x86_64 kernel
#
# The container runs with --privileged (required for loop mount, chroot,
# debootstrap). Project root is bind-mounted at /workspace so that
# build artifacts and caches persist across runs.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
IMAGE_NAME="tenbox-builder"

ARCH="${1:?Usage: $0 <arch> <target> [extra-args...]  (arch: arm64|x86_64)}"
TARGET="${2:?Usage: $0 <arch> <target> [extra-args...]  (target: rootfs-chromium|initramfs|kernel)}"
shift 2

resolve_script() {
    local arch="$1" target="$2"
    case "$target" in
        rootfs-chromium)
            if [ "$arch" = "arm64" ]; then
                echo "scripts/arm64/make-rootfs-chromium.sh"
            else
                echo "scripts/x86_64/make-rootfs-chromium.sh"
            fi
            ;;
        rootfs-copaw)
            if [ "$arch" = "arm64" ]; then
                echo "scripts/arm64/make-rootfs-copaw.sh"
            else
                echo "scripts/x86_64/make-rootfs-copaw.sh"
            fi
            ;;
        rootfs-openclaw)
            if [ "$arch" = "arm64" ]; then
                echo "scripts/arm64/make-rootfs-openclaw.sh"
            else
                echo "scripts/x86_64/make-rootfs-openclaw.sh"
            fi
            ;;
        initramfs)
            echo "scripts/${arch}/make-initramfs.sh"
            ;;
        kernel)
            echo "scripts/${arch}/get-kernel.sh"
            ;;
        *)
            echo "Error: unknown target '$target' (use: rootfs-chromium, rootfs-copaw, rootfs-openclaw, initramfs, kernel)" >&2
            exit 1
            ;;
    esac
}

SCRIPT_PATH="$(resolve_script "$ARCH" "$TARGET")"

if [ ! -f "$PROJECT_ROOT/$SCRIPT_PATH" ]; then
    echo "Error: script not found: $SCRIPT_PATH" >&2
    exit 1
fi

echo "=== TenBox Docker Build ==="
echo "  Arch:   $ARCH"
echo "  Target: $TARGET"
echo "  Script: $SCRIPT_PATH"
echo ""

if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
    echo "Building Docker image '$IMAGE_NAME'..."
    docker build -t "$IMAGE_NAME" "$SCRIPT_DIR"
    echo ""
fi

WORK_DIR="/tmp/tenbox-${ARCH}-${TARGET}"

exec docker run --rm --privileged \
    -v "$PROJECT_ROOT:/workspace" \
    -e ROOT_PASSWORD="${ROOT_PASSWORD:-tenbox}" \
    -e USER_NAME="${USER_NAME:-tenbox}" \
    -e USER_PASSWORD="${USER_PASSWORD:-tenbox}" \
    -e TENBOX_WORK_DIR="$WORK_DIR" \
    "$IMAGE_NAME" \
    -c "/workspace/$SCRIPT_PATH $*"
