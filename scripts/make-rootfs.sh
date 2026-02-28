#!/bin/bash
# Build a Debian base rootfs as qcow2 for TenBox Phase 2.
# Requires: debootstrap, qemu-utils. Run as root in WSL2 or Linux.
set -e

ROOTFS_SIZE="20G"
SUITE="bookworm"
MIRROR="https://mirrors.ustc.edu.cn/debian"
INCLUDE_PKGS="systemd-sysv,udev,dbus,sudo,\
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
OUTPUT="$(realpath -m "${1:-$BUILD_DIR/share/rootfs.qcow2}")"
CACHE_TAR="$BUILD_DIR/.debootstrap-${SUITE}-base.tar"
CACHE_CHROME="$BUILD_DIR/.google-chrome-stable_current_amd64.deb"

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

# Download Google Chrome (with cache)
if [ -f "$CACHE_CHROME" ]; then
    echo "  Using cached Chrome: $CACHE_CHROME"
else
    echo "  Downloading Google Chrome..."
    wget -q https://dl.google.com/linux/direct/google-chrome-stable_current_amd64.deb -O "$CACHE_CHROME"
fi
sudo cp "$CACHE_CHROME" "$MOUNT_DIR/tmp/chrome.deb"

sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
echo "root:tenbox" | chpasswd

# Create openclaw user with sudo privileges
useradd -m -s /bin/bash openclaw
echo "openclaw:openclaw" | chpasswd
usermod -aG sudo openclaw
echo "openclaw ALL=(ALL:ALL) NOPASSWD: ALL" > /etc/sudoers.d/openclaw
chmod 440 /etc/sudoers.d/openclaw

echo "tenbox-vm" > /etc/hostname
cat > /etc/hosts << 'HOSTS'
127.0.0.1   localhost
127.0.0.1   tenbox-vm
::1         localhost ip6-localhost ip6-loopback
HOSTS
echo "/dev/vda / ext4 defaults 0 1" > /etc/fstab

cat > /etc/apt/sources.list << 'APT'
deb https://mirrors.ustc.edu.cn/debian bookworm main contrib non-free non-free-firmware
deb https://mirrors.ustc.edu.cn/debian bookworm-updates main contrib non-free non-free-firmware
deb https://mirrors.ustc.edu.cn/debian-security bookworm-security main contrib non-free non-free-firmware
APT

apt-get update
update-ca-certificates --fresh 2>/dev/null || true

# XFCE desktop + LightDM + X11
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    xfce4 xfce4-terminal \
    lightdm \
    xserver-xorg-core xserver-xorg-input-libinput \
    xfonts-base fonts-dejavu-core fonts-noto-cjk fonts-noto-color-emoji \
    locales \
    dbus-x11 at-spi2-core

# SPICE vdagent for clipboard sharing with host
DEBIAN_FRONTEND=noninteractive apt-get install -y spice-vdagent

# Google Chrome browser
echo "Installing Google Chrome..."
DEBIAN_FRONTEND=noninteractive apt-get install -y /tmp/chrome.deb
rm -f /tmp/chrome.deb

# Chrome policy: disable welcome page and first run
mkdir -p /etc/opt/chrome/policies/managed
cat > /etc/opt/chrome/policies/managed/no_first_run.json << 'CHROME'
{
    "BrowserSignin": 0,
    "SyncDisabled": true,
    "PasswordManagerEnabled": false,
    "AutofillAddressEnabled": false,
    "AutofillCreditCardEnabled": false,
    "TranslateEnabled": false,
    "RestoreOnStartup": 5,
    "MetricsReportingEnabled": false
}
CHROME
mkdir -p /home/openclaw/.config/google-chrome && touch "/home/openclaw/.config/google-chrome/First Run"
chown -R openclaw:openclaw /home/openclaw/.config

# Set Chrome as default browser
update-alternatives --install /usr/bin/x-www-browser x-www-browser /usr/bin/google-chrome-stable 200
update-alternatives --install /usr/bin/gnome-www-browser gnome-www-browser /usr/bin/google-chrome-stable 200
mkdir -p /home/openclaw/.config
cat > /home/openclaw/.config/mimeapps.list << 'MIME'
[Default Applications]
x-scheme-handler/http=google-chrome.desktop
x-scheme-handler/https=google-chrome.desktop
text/html=google-chrome.desktop
application/xhtml+xml=google-chrome.desktop
MIME
chown -R openclaw:openclaw /home/openclaw/.config

# Development tools
echo "Installing development tools..."
DEBIAN_FRONTEND=noninteractive apt-get install -y \
    python3 python3-pip python3-venv \
    g++ make cmake git

# Node.js 22 (from NodeSource)
echo "Installing Node.js 22..."
curl -fsSL https://deb.nodesource.com/setup_22.x | bash -
DEBIAN_FRONTEND=noninteractive apt-get install -y nodejs

# Configure npm to use Alibaba Cloud mirror (global + user level)
npm config set registry https://registry.npmmirror.com --global
echo "registry=https://registry.npmmirror.com" >> /etc/npmrc
su - openclaw -c "npm config set registry https://registry.npmmirror.com"

# OpenClaw
echo "Installing OpenClaw..."
curl -fsSL https://openclaw.ai/install.sh | bash -s -- --no-onboard

# Chinese locale
sed -i 's/^# *zh_CN.UTF-8/zh_CN.UTF-8/' /etc/locale.gen
sed -i 's/^# *en_US.UTF-8/en_US.UTF-8/' /etc/locale.gen
locale-gen
update-locale LANG=zh_CN.UTF-8

apt-get clean
rm -rf /var/lib/apt/lists/*

mkdir -p /etc/systemd/system/serial-getty@ttyS0.service.d
cat > /etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf << 'INNER'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin openclaw --noclear %I 115200 linux
INNER
mkdir -p /etc/lightdm/lightdm.conf.d
cat > /etc/lightdm/lightdm.conf.d/50-autologin.conf << 'LDM'
[Seat:*]
autologin-user=openclaw
autologin-user-timeout=0
autologin-session=xfce
user-session=xfce
greeter-session=lightdm-gtk-greeter
LDM

# Auto-resize display when host window changes (virtio-gpu hotplug)
echo "Setting up virtio-gpu auto-resize..."
cat > /etc/udev/rules.d/95-virtio-gpu-resize.rules << 'UDEV'
ACTION=="change", SUBSYSTEM=="drm", ENV{HOTPLUG}=="1", RUN+="/usr/bin/bash -c '/usr/local/bin/virtio-gpu-resize.sh &'"
UDEV
echo "  Created: /etc/udev/rules.d/95-virtio-gpu-resize.rules"

OPENCLAW_UID=$(id -u openclaw)
cat > /usr/local/bin/virtio-gpu-resize.sh << RESIZE
#!/bin/bash
sleep 0.1
export DISPLAY=:0

# Try to find valid XAUTHORITY
for auth in /home/openclaw/.Xauthority /var/run/lightdm/openclaw/:0 /run/user/${OPENCLAW_UID}/gdm/Xauthority; do
    if [ -f "\$auth" ]; then
        export XAUTHORITY="\$auth"
        break
    fi
done

for output in Virtual-1 Virtual-0; do
    xrandr --output "\$output" --auto 2>/dev/null && break
done
RESIZE
chmod +x /usr/local/bin/virtio-gpu-resize.sh
echo "  Created: /usr/local/bin/virtio-gpu-resize.sh"

mkdir -p /etc/network
cat > /etc/network/interfaces << 'NET'
auto lo
iface lo inet loopback
auto eth0
iface eth0 inet dhcp
NET

systemctl enable networking.service 2>/dev/null || true
systemctl set-default graphical.target
systemctl enable lightdm.service 2>/dev/null || true

# SPICE vdagent setup for clipboard sharing
echo "Setting up spice-vdagent..."
cat > /etc/udev/rules.d/99-spice-vdagent.rules << 'UDEV'
SUBSYSTEM=="virtio-ports", ATTR{name}=="com.redhat.spice.0", SYMLINK+="virtio-ports/com.redhat.spice.0"
UDEV
echo "  Created: /etc/udev/rules.d/99-spice-vdagent.rules"

# Configure spice-vdagentd for microvm environment (no uinput, no session integration)
mkdir -p /etc/systemd/system/spice-vdagentd.service.d
cat > /etc/systemd/system/spice-vdagentd.service.d/override.conf << 'OVERRIDE'
[Service]
ExecStart=
ExecStart=/usr/sbin/spice-vdagentd -X -f -u /dev/null
OVERRIDE
echo "  Created: spice-vdagentd systemd override"

# Enable spice-vdagentd service (the daemon that communicates with spice-vdagent)
systemctl enable spice-vdagentd.service 2>/dev/null || true
echo "  Enabled: spice-vdagentd.service"

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
# Prefer Windows qemu-img.exe (supports zstd), fallback to WSL version
WIN_QEMU="/mnt/c/Program Files/qemu/qemu-img.exe"
if [ -x "$WIN_QEMU" ]; then
    echo "  Using Windows qemu-img with zstd compression..."
    # Convert WSL paths to Windows paths for qemu-img.exe
    WIN_RAW=$(wslpath -w "$WORK_DIR/rootfs.raw")
    WIN_OUTPUT=$(wslpath -w "$OUTPUT")
    "$WIN_QEMU" convert -f raw -O qcow2 -o cluster_size=65536,compression_type=zstd -c "$WIN_RAW" "$WIN_OUTPUT"
else
    echo "  Using WSL qemu-img with zlib compression..."
    qemu-img convert -f raw -O qcow2 -o cluster_size=65536 -c "$WORK_DIR/rootfs.raw" "$OUTPUT"
fi

echo "Done: $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"
