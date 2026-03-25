#!/bin/bash
# Build a minimal BusyBox initramfs for TenBox arm64 testing.
# Runs on macOS — only requires: curl, gzip, ar, tar, python3.
#
# Usage:
#   ./make-initramfs.sh [output_dir] [suite]
#     output_dir - where to place initramfs-arm64.cpio.gz (default: ../build/share)
#     suite      - Debian suite (default: trixie)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTDIR="$(mkdir -p "${1:-$SCRIPT_DIR/../../build/share}" && cd "${1:-$SCRIPT_DIR/../../build/share}" && pwd)"
SUITE="${2:-trixie}"
WORKDIR=$(mktemp -d)
trap "rm -rf $WORKDIR" EXIT

MIRROR="https://deb.debian.org/debian"
ARCH="arm64"

cd "$WORKDIR"

extract_deb() {
    local deb="$1" dest="$2"
    local abs_deb="$(cd "$(dirname "$deb")" && pwd)/$(basename "$deb")"
    local abs_dest="$(mkdir -p "$dest" && cd "$dest" && pwd)"
    local tmpdir="$WORKDIR/_deb_$$"
    mkdir -p "$tmpdir" && cd "$tmpdir"
    ar x "$abs_deb"
    local data_tar=$(ls data.tar.* 2>/dev/null | head -1)
    case "$data_tar" in
        *.xz)  xz -d "$data_tar" && tar xf data.tar -C "$abs_dest" ;;
        *.gz)  gzip -d "$data_tar" && tar xf data.tar -C "$abs_dest" ;;
        *.zst) zstd -d "$data_tar" && tar xf data.tar -C "$abs_dest" ;;
        *)     tar xf "$data_tar" -C "$abs_dest" ;;
    esac
    cd "$WORKDIR"
    rm -rf "$tmpdir"
}

echo "[1/5] Resolving kernel package from $SUITE/$ARCH ..."
curl -fsSL "$MIRROR/dists/$SUITE/main/binary-$ARCH/Packages.gz" | gunzip > Packages

# Parse the meta-package to find the real kernel package name
META_BLOCK=$(awk '/^Package: linux-image-arm64$/,/^$/' Packages)
KPKG=$(echo "$META_BLOCK" | sed -n 's/^Depends:.*\(linux-image-[0-9][^ ,]*\).*/\1/p')
KVER=$(echo "$KPKG" | sed 's/^linux-image-//')
if [ -z "$KPKG" ] || [ -z "$KVER" ]; then
    echo "Error: could not resolve kernel package from $SUITE/$ARCH." >&2
    exit 1
fi
echo "  -> kernel: $KPKG (version: $KVER)"

echo "[2/5] Downloading BusyBox & kernel package ..."

# BusyBox static (arm64)
BB_DEB_PATH=$(awk '/^Package: busybox-static$/,/^$/' Packages | sed -n 's/^Filename: //p')
if [ -z "$BB_DEB_PATH" ]; then
    echo "Error: could not find busybox-static for $ARCH in $SUITE." >&2
    exit 1
fi
echo "  -> busybox: $MIRROR/$BB_DEB_PATH"
curl -fsSL -o busybox-static.deb "$MIRROR/$BB_DEB_PATH"
extract_deb busybox-static.deb bb_extract/
BB_BIN=$(find bb_extract/ -name busybox -type f | head -1)
if [ -z "$BB_BIN" ]; then echo "Error: busybox binary not found in deb" >&2; exit 1; fi
cp "$BB_BIN" "$WORKDIR/busybox"
chmod +x "$WORKDIR/busybox"

# Kernel package (for modules)
KERN_DEB_PATH=$(awk -v pkg="$KPKG" '$0 == "Package: " pkg, /^$/' Packages | sed -n 's/^Filename: //p')
if [ -z "$KERN_DEB_PATH" ]; then
    echo "Error: could not find .deb path for $KPKG." >&2
    exit 1
fi
echo "  -> kernel: $MIRROR/$KERN_DEB_PATH"
curl -fsSL -o kernel.deb "$MIRROR/$KERN_DEB_PATH"
extract_deb kernel.deb kmod_extract/

echo "[3/5] Creating initramfs layout & extracting modules ..."
mkdir -p "$WORKDIR/initramfs"/{bin,sbin,dev,proc,sys,etc,tmp,lib/modules,newroot}
cp "$WORKDIR/busybox" "$WORKDIR/initramfs/bin/"

# Install e2fsck (fsck.ext4) from the host system (already in Docker image)
E2FSCK_BIN="$(command -v e2fsck || echo /sbin/e2fsck)"
if [ -x "$E2FSCK_BIN" ]; then
    cp "$E2FSCK_BIN" "$WORKDIR/initramfs/sbin/e2fsck"
    chmod +x "$WORKDIR/initramfs/sbin/e2fsck"
    ln -sf e2fsck "$WORKDIR/initramfs/sbin/fsck.ext4"
    mkdir -p "$WORKDIR/initramfs/lib/aarch64-linux-gnu" "$WORKDIR/initramfs/lib"
    for lib in $(ldd "$E2FSCK_BIN" 2>/dev/null | grep -oP '/\S+'); do
        if [ -f "$lib" ]; then
            destdir="$WORKDIR/initramfs$(dirname "$lib")"
            mkdir -p "$destdir"
            cp "$lib" "$destdir/"
            echo "  lib: $lib"
        fi
    done
    echo "  Installed: e2fsck (fsck.ext4)"
else
    echo "  WARNING: e2fsck not found on host; install e2fsprogs in Docker image"
fi

if [ -d "kmod_extract/usr/lib/modules/$KVER/kernel" ]; then
    MODDIR="kmod_extract/usr/lib/modules/$KVER/kernel"
else
    MODDIR="kmod_extract/lib/modules/$KVER/kernel"
fi
DESTDIR="$WORKDIR/initramfs/lib/modules"

VIRTIO_MODS=(
    # virtio core (virtio, virtio_ring, virtio_pci) is built-in (=y) in trixie
    "drivers/virtio/virtio_mmio.ko"
    "drivers/block/virtio_blk.ko"
    "net/core/failover.ko"
    "drivers/net/net_failover.ko"
    "drivers/net/virtio_net.ko"
    # virtio_console is built-in (=y) in trixie
    "drivers/virtio/virtio_input.ko"
    "drivers/input/evdev.ko"
    "drivers/media/rc/rc-core.ko"
    "drivers/gpu/drm/drm.ko"
    "drivers/gpu/drm/drm_kms_helper.ko"
    "drivers/gpu/drm/drm_shmem_helper.ko"
    "drivers/virtio/virtio_dma_buf.ko"
    "drivers/gpu/drm/virtio/virtio-gpu.ko"
    # fuse + virtiofs are built-in (=y) in trixie
    "fs/mbcache.ko"
    "fs/jbd2/jbd2.ko"
    "lib/crc16.ko"
    "crypto/crc32c_generic.ko"
    "lib/libcrc32c.ko"
    "fs/ext4/ext4.ko"
    "sound/soundcore.ko"
    "sound/core/snd.ko"
    "sound/core/snd-timer.ko"
    "sound/core/snd-pcm.ko"
    "sound/virtio/virtio_snd.ko"
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
        found=$(find "$MODDIR" -name "$modname" -o -name "${modname}.xz" -o -name "${modname}.zst" 2>/dev/null | head -1)
        if [ -n "$found" ]; then
            rel="${found#$MODDIR/}"
            copy_module "$rel" || echo "  WARNING: $modname found but copy failed"
        else
            echo "  WARNING: $modname not found in $KPKG"
        fi
    fi
done

cat > "$WORKDIR/initramfs/init" << 'INITEOF'
#!/bin/busybox sh
/bin/busybox mkdir -p /proc /sys /dev /tmp /newroot

# Mount essential filesystems first
/bin/busybox mount -t devtmpfs none /dev 2>/dev/null
/bin/busybox mount -t proc none /proc
/bin/busybox mount -t sysfs none /sys

# Now that devtmpfs is mounted, /dev/console should exist.
# Reopen stdio on the console device so our output goes to ttyAMA0.
exec 0</dev/console 1>/dev/console 2>/dev/console

/bin/busybox --install -s /bin

echo ""
echo "===== Loading VirtIO modules ====="

MODDIR=/lib/modules
# virtio core (virtio, virtio_ring, virtio_pci) is built-in since trixie
for mod in virtio_mmio virtio_blk failover net_failover virtio_net virtio_input evdev; do
    [ -f "$MODDIR/$mod.ko" ] && insmod "$MODDIR/$mod.ko" 2>/dev/null && echo "  [OK] $mod" || true
done

echo ""
echo "===== Loading DRM / virtio-gpu ====="
for mod in rc-core cec drm drm_kms_helper drm_shmem_helper virtio_dma_buf virtio-gpu; do
    [ -f "$MODDIR/$mod.ko" ] && insmod "$MODDIR/$mod.ko" 2>/dev/null && echo "  [OK] $mod" || true
done

echo ""
echo "===== Loading ext4 ====="
for mod in crc16 crc32c_generic libcrc32c mbcache jbd2 ext4; do
    [ -f "$MODDIR/$mod.ko" ] && insmod "$MODDIR/$mod.ko" 2>/dev/null && echo "  [OK] $mod" || true
done

echo ""
echo "===== Loading ALSA / virtio-snd ====="
for mod in soundcore snd snd-timer snd-pcm virtio_snd; do
    [ -f "$MODDIR/$mod.ko" ] && insmod "$MODDIR/$mod.ko" 2>/dev/null && echo "  [OK] $mod" || true
done

sleep 0.2

echo ""
echo "========================================="
echo " TenBox VM booted successfully! (arm64)"
echo "========================================="
echo "Kernel:  $(uname -r) ($(uname -m))"
echo "Memory:  $(head -1 /proc/meminfo)"

NVIRTIO=$(ls /sys/bus/virtio/devices/ 2>/dev/null | wc -l)
echo "VirtIO:  $NVIRTIO devices"
[ -e /dev/fb0 ] && echo "Display: fb0" || echo "Display: (none)"

if [ -e /dev/vda ]; then
    echo "Disk:    /dev/vda ($(cat /sys/block/vda/size) sectors)"
    mkdir -p /newroot
    if mount -t ext4 /dev/vda /newroot 2>/dev/null; then
        INIT=""
        for c in /usr/lib/systemd/systemd /lib/systemd/systemd /sbin/init; do
            [ -x "/newroot${c}" ] && INIT="$c" && break
        done
        if [ -n "$INIT" ]; then
            echo "Switching to rootfs (init=$INIT) ..."
            umount /proc /sys /dev 2>/dev/null
            exec switch_root /newroot "$INIT"
        fi
        umount /newroot 2>/dev/null
    fi
fi

echo ""
echo "Dropping to BusyBox shell..."
exec /bin/sh
INITEOF
chmod +x "$WORKDIR/initramfs/init"

echo "[4/5] Packing initramfs..."
cd "$WORKDIR/initramfs"

python3 "$SCRIPT_DIR/../mkcpio.py" . "$WORKDIR/initramfs-arm64.cpio.gz" \
    dev/console,0600,5,1 \
    dev/null,0666,1,3 \
    dev/ttyAMA0,0660,204,64

PACKED_SIZE=$(stat -c '%s' "$WORKDIR/initramfs-arm64.cpio.gz" 2>/dev/null || stat -f '%z' "$WORKDIR/initramfs-arm64.cpio.gz")
if [ "$PACKED_SIZE" -le 20 ]; then
    echo "Error: initramfs-arm64.cpio.gz is too small (${PACKED_SIZE} bytes), packing likely failed." >&2
    exit 1
fi

echo "[5/5] Copying output..."
cp "$WORKDIR/initramfs-arm64.cpio.gz" "$OUTDIR/initramfs-arm64.cpio.gz"
echo "Done: $OUTDIR/initramfs-arm64.cpio.gz ($(ls -lh "$OUTDIR/initramfs-arm64.cpio.gz" | awk '{print $5}'))"