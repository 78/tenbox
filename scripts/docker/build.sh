#!/bin/bash
# Unified Docker-based build wrapper for TenBox rootfs/initramfs/kernel images.
#
# Usage:
#   ./scripts/docker/build.sh <arch> <target> [extra-args...]
#
# arch:   arm64 | x86_64
# target: rootfs-chromium | rootfs-qwenpaw | rootfs-openclaw | rootfs-hermes | initramfs | kernel
#
# Examples:
#   ./scripts/docker/build.sh arm64 rootfs-chromium
#   ./scripts/docker/build.sh arm64 rootfs-chromium --force
#   ./scripts/docker/build.sh arm64 rootfs-chromium --list-steps
#   ./scripts/docker/build.sh arm64 initramfs
#   ./scripts/docker/build.sh arm64 kernel
#   ./scripts/docker/build.sh x86_64 rootfs-chromium
#   ./scripts/docker/build.sh x86_64 rootfs-chromium --force
#   ./scripts/docker/build.sh x86_64 rootfs-qwenpaw
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

ARCH="${1:?Usage: $0 <arch> <target> [extra-args...]  (arch: arm64|x86_64)}"
TARGET="${2:?Usage: $0 <arch> <target> [extra-args...]  (target: rootfs-chromium|initramfs|kernel)}"
shift 2

needs_qemu_img() {
    case "$1" in
        rootfs-*) return 0 ;;
        *)        return 1 ;;
    esac
}

if needs_qemu_img "$TARGET"; then
    IMAGE_NAME="tenbox-builder"
    DOCKER_TARGET="full"
else
    IMAGE_NAME="tenbox-builder-base"
    DOCKER_TARGET="base"
fi

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
        rootfs-qwenpaw)
            if [ "$arch" = "arm64" ]; then
                echo "scripts/arm64/make-rootfs-qwenpaw.sh"
            else
                echo "scripts/x86_64/make-rootfs-qwenpaw.sh"
            fi
            ;;
        rootfs-openclaw)
            if [ "$arch" = "arm64" ]; then
                echo "scripts/arm64/make-rootfs-openclaw.sh"
            else
                echo "scripts/x86_64/make-rootfs-openclaw.sh"
            fi
            ;;
        rootfs-hermes)
            if [ "$arch" = "arm64" ]; then
                echo "scripts/arm64/make-rootfs-hermes.sh"
            else
                echo "scripts/x86_64/make-rootfs-hermes.sh"
            fi
            ;;
        initramfs)
            echo "scripts/${arch}/make-initramfs.sh"
            ;;
        kernel)
            echo "scripts/${arch}/get-kernel.sh"
            ;;
        *)
            echo "Error: unknown target '$target' (use: rootfs-chromium, rootfs-qwenpaw, rootfs-openclaw, rootfs-hermes, initramfs, kernel)" >&2
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
echo "  Image:  $IMAGE_NAME (docker target: $DOCKER_TARGET)"
echo ""

if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
    echo "Building Docker image '$IMAGE_NAME' (target=$DOCKER_TARGET)..."
    docker build --target "$DOCKER_TARGET" -t "$IMAGE_NAME" "$SCRIPT_DIR"
    echo ""
fi

WORK_DIR="/tmp/tenbox-${ARCH}-${TARGET}"

# Rootfs builds require loop-mounting rootfs.raw inside the container.
# Even with --privileged, Docker does not share the host's /dev by default,
# so losetup inside the container may fail to create/allocate loop devices.
# Bind-mount /dev and ensure the loop module is available on the host.
DEV_ARGS=()
if needs_qemu_img "$TARGET"; then
    if [ ! -e /dev/loop-control ]; then
        echo "Loading 'loop' kernel module on host..."
        sudo modprobe loop || true
    fi
    DEV_ARGS=(-v /dev:/dev)
fi

exec docker run --rm --privileged \
    "${DEV_ARGS[@]}" \
    -v "$PROJECT_ROOT:/workspace" \
    -e ROOT_PASSWORD="${ROOT_PASSWORD:-tenbox}" \
    -e USER_NAME="${USER_NAME:-tenbox}" \
    -e USER_PASSWORD="${USER_PASSWORD:-tenbox}" \
    -e HERMES_VERSION="${HERMES_VERSION:-}" \
    -e TENBOX_DEBIAN_SUITE="${TENBOX_DEBIAN_SUITE:-}" \
    -e TENBOX_DEBIAN_MIRROR="${TENBOX_DEBIAN_MIRROR:-}" \
    -e TENBOX_DEBIAN_SECURITY_MIRROR="${TENBOX_DEBIAN_SECURITY_MIRROR:-}" \
    -e TENBOX_DEBOOTSTRAP_RETRIES="${TENBOX_DEBOOTSTRAP_RETRIES:-}" \
    -e TENBOX_WORK_DIR="$WORK_DIR" \
    "$IMAGE_NAME" \
    -c "/workspace/$SCRIPT_PATH $*"
