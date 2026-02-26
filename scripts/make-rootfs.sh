#!/bin/bash
# Build a Debian base rootfs as qcow2 for TenBox Phase 2.
# Requires: debootstrap, qemu-utils. Run as root in WSL2 or Linux.
set -e

ROOTFS_SIZE="10G"
SUITE="bookworm"
MIRROR="https://mirrors.tuna.tsinghua.edu.cn/debian"
INCLUDE_PKGS="systemd-sysv,udev,dbus,\
iproute2,iputils-ping,ifupdown,isc-dhcp-client,\
ca-certificates,curl,wget,\
procps,psmisc,\
netcat-openbsd,net-tools,traceroute,dnsutils,\
less,vim-tiny,bash-completion,\
openssh-client,gnupg,apt-transport-https,\
lsof,strace,sysstat,\
kmod,pciutils,usbutils,\
coreutils,findutils,grep,gawk,sed,tar,gzip,bzip2,xz-utils"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build"
mkdir -p "$BUILD_DIR"
OUTPUT="$(realpath -m "${1:-$BUILD_DIR/rootfs.qcow2}")"
CACHE_TAR="$BUILD_DIR/.debootstrap-${SUITE}-base.tar"

# DrvFS (/mnt/*) does not support mknod through loop devices.
# Build everything on the native Linux filesystem, copy result back.
WORK_DIR=$(mktemp -d /tmp/make-rootfs.XXXXXX)
MOUNT_DIR=""

cleanup() {
    if [ -n "$MOUNT_DIR" ]; then
        for sub in proc sys dev; do
            mountpoint -q "$MOUNT_DIR/$sub" 2>/dev/null && \
                sudo umount -l "$MOUNT_DIR/$sub" 2>/dev/null
        done
        mountpoint -q "$MOUNT_DIR" 2>/dev/null && \
            (sudo umount "$MOUNT_DIR" || sudo umount -l "$MOUNT_DIR")
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

echo "[1/5] Creating raw image..."
truncate -s "$ROOTFS_SIZE" "$WORK_DIR/rootfs.raw"
mkfs.ext4 -F "$WORK_DIR/rootfs.raw"

echo "[2/5] Bootstrapping Debian ${SUITE}..."
MOUNT_DIR="$WORK_DIR/mnt"
mkdir -p "$MOUNT_DIR"
sudo mount -o loop "$WORK_DIR/rootfs.raw" "$MOUNT_DIR"
if [ -f "$CACHE_TAR" ]; then
    echo "  Using cached tarball: $CACHE_TAR"
    sudo debootstrap --include="$INCLUDE_PKGS" \
        --unpack-tarball="$CACHE_TAR" "$SUITE" "$MOUNT_DIR" "$MIRROR"
else
    echo "  No cache found, downloading packages (first run)..."
    sudo debootstrap --include="$INCLUDE_PKGS" \
        --make-tarball="$CACHE_TAR" "$SUITE" "$WORK_DIR/tarball-tmp" "$MIRROR"
    sudo debootstrap --include="$INCLUDE_PKGS" \
        --unpack-tarball="$CACHE_TAR" "$SUITE" "$MOUNT_DIR" "$MIRROR"
fi

echo "[3/5] Configuring system..."

# Ensure DNS works inside chroot
sudo cp /etc/resolv.conf "$MOUNT_DIR/etc/resolv.conf"

# Mount proc/sys/dev for chroot package installation
sudo mount --bind /proc "$MOUNT_DIR/proc"
sudo mount --bind /sys  "$MOUNT_DIR/sys"
sudo mount --bind /dev  "$MOUNT_DIR/dev"

# Prevent service start failures in chroot
sudo tee "$MOUNT_DIR/usr/sbin/policy-rc.d" > /dev/null << 'PRC'
#!/bin/sh
exit 101
PRC
sudo chmod +x "$MOUNT_DIR/usr/sbin/policy-rc.d"

sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
echo "root:tenbox" | chpasswd
echo "tenbox-vm" > /etc/hostname
echo "/dev/vda / ext4 defaults 0 1" > /etc/fstab

cat > /etc/apt/sources.list << 'APT'
deb https://mirrors.tuna.tsinghua.edu.cn/debian bookworm main contrib non-free non-free-firmware
deb https://mirrors.tuna.tsinghua.edu.cn/debian bookworm-updates main contrib non-free non-free-firmware
deb https://mirrors.tuna.tsinghua.edu.cn/debian-security bookworm-security main contrib non-free non-free-firmware
APT

apt-get update
update-ca-certificates --fresh 2>/dev/null || true
apt-get clean
rm -rf /var/lib/apt/lists/*

mkdir -p /etc/systemd/system/serial-getty@ttyS0.service.d
cat > /etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf << 'INNER'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --noclear %I 115200 linux
INNER

cat > /etc/profile.d/term-size.sh << 'RESIZE'
# Provide a `resize` command for serial consoles and auto-detect
# terminal size on login. Uses DSR (ESC[6n) cursor position query:
# move cursor to far corner (999,999 → clamped to actual size),
# query position, then set stty rows/cols.
resize() {
    [ -t 0 ] && [ -t 1 ] || return 0
    local saved rows cols _
    saved=$(stty -g 2>/dev/null)
    stty raw -echo min 0 time 20 2>/dev/null
    printf '\033[s\033[999;999H\033[6n\033[u'
    IFS='[;' read -r -d R -t 2 _ rows cols 2>/dev/null
    stty "$saved" 2>/dev/null
    if [ -n "$rows" ] && [ "$rows" -gt 0 ] 2>/dev/null &&
       [ -n "$cols" ] && [ "$cols" -gt 0 ] 2>/dev/null; then
        stty rows "$rows" cols "$cols"
        export LINES="$rows" COLUMNS="$cols"
    fi
}
case "$(tty 2>/dev/null)" in /dev/ttyS*) resize ;; esac
RESIZE

mkdir -p /etc/network
cat > /etc/network/interfaces << 'NET'
auto lo
iface lo inet loopback
auto eth0
iface eth0 inet dhcp
NET

systemctl enable networking.service 2>/dev/null || true

# Verify key commands
ls -la /sbin/init
echo "init OK: $(readlink -f /sbin/init)"
echo "nc:   $(which nc 2>/dev/null || echo MISSING)"
echo "free: $(which free 2>/dev/null || echo MISSING)"
echo "ps:   $(which ps 2>/dev/null || echo MISSING)"
echo "curl: $(which curl 2>/dev/null || echo MISSING)"
echo "wget: $(which wget 2>/dev/null || echo MISSING)"
EOF

sudo rm -f "$MOUNT_DIR/usr/sbin/policy-rc.d"

sudo umount "$MOUNT_DIR/proc" "$MOUNT_DIR/sys" "$MOUNT_DIR/dev" 2>/dev/null || true

echo "[4/5] Unmounting..."
sudo umount "$MOUNT_DIR"
MOUNT_DIR=""

echo "[5/5] Converting to qcow2..."
qemu-img convert -f raw -O qcow2 -o cluster_size=65536 "$WORK_DIR/rootfs.raw" "$OUTPUT"

echo "Done: $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"
