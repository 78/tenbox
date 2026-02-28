#!/bin/bash
# Build a minimal BusyBox initramfs for TenBox testing.
# Includes virtio kernel modules for block device support.
# Only requires: curl, gzip, dpkg-deb, cpio. Run in WSL2 or Linux.
#
# Usage:
#   ./make-initramfs.sh [output_dir] [suite]
#     output_dir - where to place initramfs.cpio.gz (default: ../build/share)
#     suite      - Debian suite: bookworm(6.x), bullseye(5.x), etc. Default: bookworm
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTDIR="$(mkdir -p "${1:-$SCRIPT_DIR/../build/share}" && cd "${1:-$SCRIPT_DIR/../build/share}" && pwd)"
SUITE="${2:-bookworm}"
WORKDIR=$(mktemp -d)
trap "rm -rf $WORKDIR" EXIT

MIRROR="https://deb.debian.org/debian"
ARCH="amd64"

cd "$WORKDIR"

echo "[1/5] Resolving kernel package from $SUITE ..."
curl -fsSL "$MIRROR/dists/$SUITE/main/binary-$ARCH/Packages.gz" | gunzip > Packages

META_BLOCK=$(awk '/^Package: linux-image-amd64$/,/^$/' Packages)
KPKG=$(echo "$META_BLOCK" | grep -oP '^Depends:.*\Klinux-image-[0-9]\S+')
KVER=$(echo "$KPKG" | sed 's/^linux-image-//')
if [ -z "$KPKG" ] || [ -z "$KVER" ]; then
    echo "Error: could not resolve kernel package from $SUITE." >&2
    exit 1
fi
echo "  -> kernel package: $KPKG  (version: $KVER)"

echo "[2/5] Downloading BusyBox & kernel package ..."
# BusyBox
BB_DEB_PATH=$(awk '/^Package: busybox-static$/,/^$/' Packages | grep -oP '^Filename: \K.*')
if [ -z "$BB_DEB_PATH" ]; then
    echo "Error: could not find busybox-static in $SUITE." >&2
    exit 1
fi
curl -fsSL -o busybox-static.deb "$MIRROR/$BB_DEB_PATH"
dpkg-deb -x busybox-static.deb bb_extract/
cp bb_extract/bin/busybox "$WORKDIR/busybox"
chmod +x "$WORKDIR/busybox"

# Kernel (for modules)
KERN_DEB_PATH=$(awk "/^Package: ${KPKG}$/,/^$/" Packages | grep -oP '^Filename: \K.*')
if [ -z "$KERN_DEB_PATH" ]; then
    echo "Error: could not find .deb path for $KPKG." >&2
    exit 1
fi
echo "  -> $MIRROR/$KERN_DEB_PATH"
curl -fsSL -o kernel.deb "$MIRROR/$KERN_DEB_PATH"
mkdir -p kmod_extract
dpkg-deb -x kernel.deb kmod_extract/

echo "[3/5] Creating initramfs layout & extracting modules ..."
mkdir -p "$WORKDIR/initramfs"/{bin,dev,proc,sys,etc,tmp,lib/modules}
cp "$WORKDIR/busybox" "$WORKDIR/initramfs/bin/"

MODDIR="kmod_extract/lib/modules/$KVER/kernel"
DESTDIR="$WORKDIR/initramfs/lib/modules"

# Modules needed for virtio block/net/input/gpu/fs devices + ext4 filesystem support
VIRTIO_MODS=(
    "drivers/virtio/virtio.ko"
    "drivers/virtio/virtio_ring.ko"
    "drivers/virtio/virtio_mmio.ko"
    "drivers/block/virtio_blk.ko"
    "net/core/failover.ko"
    "drivers/net/net_failover.ko"
    "drivers/net/virtio_net.ko"
    "drivers/char/virtio_console.ko"
    "drivers/virtio/virtio_input.ko"
    "drivers/input/evdev.ko"
    "drivers/media/rc/rc-core.ko"
    "drivers/media/cec/cec.ko"
    "drivers/gpu/drm/drm.ko"
    "drivers/gpu/drm/drm_kms_helper.ko"
    "drivers/gpu/drm/drm_shmem_helper.ko"
    "drivers/virtio/virtio_dma_buf.ko"
    "drivers/gpu/drm/virtio/virtio-gpu.ko"
    "fs/fuse/fuse.ko"
    "fs/fuse/virtiofs.ko"
    "fs/mbcache.ko"
    "fs/jbd2/jbd2.ko"
    "lib/crc16.ko"
    "crypto/crc32c_generic.ko"
    "lib/libcrc32c.ko"
    "fs/ext4/ext4.ko"
)

copy_module() {
    local relmod="$1"
    local modname="$(basename "$relmod")"
    local src="$MODDIR/$relmod"
    if [ -f "$src" ]; then
        cp "$src" "$DESTDIR/"
        echo "  Copied: $modname"
        return 0
    elif [ -f "${src}.xz" ]; then
        xz -d < "${src}.xz" > "$DESTDIR/$modname"
        echo "  Decompressed: $modname (.xz)"
        return 0
    elif [ -f "${src}.zst" ]; then
        zstd -d "${src}.zst" -o "$DESTDIR/$modname" 2>/dev/null
        echo "  Decompressed: $modname (.zst)"
        return 0
    fi
    return 1
}

for relmod in "${VIRTIO_MODS[@]}"; do
    modname="$(basename "$relmod")"
    if ! copy_module "$relmod"; then
        # Some modules have different paths across kernel versions;
        # fall back to a find-based search.
        found=$(find "$MODDIR" -name "$modname" -o -name "${modname}.xz" -o -name "${modname}.zst" 2>/dev/null | head -1)
        if [ -n "$found" ]; then
            rel="${found#$MODDIR/}"
            copy_module "$rel" || echo "  WARNING: $modname found but copy failed"
        else
            echo "  WARNING: $modname not found in $KPKG"
        fi
    fi
done

cat > "$WORKDIR/initramfs/init" << 'EOF'
#!/bin/busybox sh
/bin/busybox mkdir -p /proc /sys /dev /tmp
/bin/busybox mount -t proc none /proc
/bin/busybox mount -t sysfs none /sys
/bin/busybox mount -t devtmpfs none /dev 2>/dev/null

/bin/busybox --install -s /bin

# Load virtio modules — device discovery is handled by ACPI DSDT
MODDIR=/lib/modules
for mod in virtio virtio_ring virtio_mmio virtio_blk failover net_failover virtio_net virtio_console virtio_input evdev; do
    if [ -f "$MODDIR/$mod.ko" ]; then
        insmod "$MODDIR/$mod.ko" 2>/dev/null && \
            echo "Loaded: $mod" || echo "Failed: $mod"
    fi
done

# Load DRM / virtio-gpu modules (order matters: rc-core before cec,
# drm_shmem_helper before virtio-gpu)
for mod in rc-core cec drm drm_kms_helper drm_shmem_helper virtio_dma_buf virtio-gpu; do
    if [ -f "$MODDIR/$mod.ko" ]; then
        insmod "$MODDIR/$mod.ko" 2>/dev/null && \
            echo "Loaded: $mod" || echo "Failed: $mod"
    fi
done

# Load ext4 filesystem modules
for mod in crc16 crc32c_generic libcrc32c mbcache jbd2 ext4; do
    if [ -f "$MODDIR/$mod.ko" ]; then
        insmod "$MODDIR/$mod.ko" 2>/dev/null && \
            echo "Loaded: $mod" || echo "Failed: $mod"
    fi
done

# Load virtio-fs / fuse modules for shared folder support
for mod in fuse virtiofs; do
    if [ -f "$MODDIR/$mod.ko" ]; then
        insmod "$MODDIR/$mod.ko" 2>/dev/null && \
            echo "Loaded: $mod" || echo "Failed: $mod"
    fi
done

# Wait for block device to appear (poll instead of fixed sleep)
for i in $(seq 1 20); do
    [ -e /dev/vda ] && break
    sleep 0.05
done

echo ""
echo "====================================="
echo " TenBox VM booted successfully!"
echo "====================================="
echo "Kernel: $(uname -r)"
echo "Memory: $(cat /proc/meminfo | head -1)"
echo ""

if [ -e /dev/vda ]; then
    echo "Block device: /dev/vda detected"
    echo "  Size: $(cat /sys/block/vda/size) sectors"

    # Try to switch_root into the real rootfs
    mkdir -p /newroot
    if mount -t ext4 /dev/vda /newroot 2>/dev/null; then
        # Bookworm uses usrmerge: /sbin/init is an absolute symlink
        # to /lib/systemd/systemd. We must check inside the rootfs
        # context, not from initramfs where the symlink would escape.
        INIT=""
        for candidate in /usr/lib/systemd/systemd /lib/systemd/systemd /sbin/init; do
            if [ -x "/newroot${candidate}" ]; then
                INIT="$candidate"
                break
            fi
        done
        if [ -n "$INIT" ]; then
            echo "Switching to rootfs on /dev/vda (init=$INIT) ..."
            umount /proc /sys /dev 2>/dev/null
            exec switch_root /newroot "$INIT"
        else
            echo "No init found on rootfs, staying in initramfs"
            umount /newroot 2>/dev/null
        fi
    else
        echo "Failed to mount /dev/vda as ext4"
    fi
fi
echo ""

# Fallback to interactive shell if no rootfs
/bin/sh

echo "Shutting down..."
poweroff -f
EOF
chmod +x "$WORKDIR/initramfs/init"

echo "[4/5] Packing initramfs..."
cd "$WORKDIR/initramfs"
find . | cpio -o -H newc 2>/dev/null | gzip > "$WORKDIR/initramfs.cpio.gz"

echo "[5/5] Copying output..."
cp "$WORKDIR/initramfs.cpio.gz" "$OUTDIR/initramfs.cpio.gz"
echo "Done: $OUTDIR/initramfs.cpio.gz ($(du -h "$OUTDIR/initramfs.cpio.gz" | cut -f1))"
