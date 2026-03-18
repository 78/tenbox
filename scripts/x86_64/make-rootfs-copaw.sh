#!/bin/bash
# Build a Debian base rootfs as qcow2 for TenBox Phase 2.
# Requires: debootstrap, qemu-utils. Run as root in WSL2 or Linux.
#
# Features:
#   - Checkpoint system: resume from last successful step after failure
#   - APT cache: reuse downloaded packages across runs
#
# Usage:
#   ./make-rootfs.sh [output.qcow2]           # Normal run (resume if interrupted)
#   ./make-rootfs.sh --force [output.qcow2]   # Force rebuild from scratch
#   ./make-rootfs.sh --from-step N            # Resume from step N
#   ./make-rootfs.sh --list-steps             # Show all steps
#   ./make-rootfs.sh --status                 # Show current progress

set -e

ROOTFS_SIZE="20G"
SUITE="bookworm"
MIRROR="http://deb.debian.org/debian"
MIRROR_SECURITY="http://deb.debian.org/debian-security"
ROOT_PASSWORD="${ROOT_PASSWORD:-tenbox}"
USER_NAME="${USER_NAME:-tenbox}"
USER_PASSWORD="${USER_PASSWORD:-tenbox}"
INCLUDE_PKGS="systemd-sysv,udev,dbus,sudo,\
iproute2,iputils-ping,ifupdown,isc-dhcp-client,\
ca-certificates,curl,wget,\
procps,psmisc,\
netcat-openbsd,net-tools,traceroute,dnsutils,\
less,vim,bash-completion,\
openssh-client,gnupg,apt-transport-https,\
lsof,strace,sysstat,\
kmod,pciutils,usbutils,\
coreutils,findutils,grep,gawk,sed,tar,gzip,bzip2,xz-utils,\
linux-image-amd64,iptables"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$(mkdir -p "$SCRIPT_DIR/../../build" && cd "$SCRIPT_DIR/../../build" && pwd)"

# Cache directories
CACHE_DIR="$BUILD_DIR/.rootfs-cache"
CHECKPOINT_DIR="$CACHE_DIR/checkpoints-copaw"
APT_CACHE_DIR="$CACHE_DIR/apt-archives"
SCRIPTS_CACHE_DIR="$CACHE_DIR/scripts"
mkdir -p "$CHECKPOINT_DIR" "$APT_CACHE_DIR" "$SCRIPTS_CACHE_DIR"

# Cache files
CACHE_TAR="$(realpath -m "$CACHE_DIR/debootstrap-${SUITE}-base.tar")"
CACHE_NODESOURCE="$SCRIPTS_CACHE_DIR/nodesource_setup_22.x.sh"

# Work dir must be on WSL Linux FS (/tmp), not NTFS (DrvFS /mnt/*) - loop devices need mknod
WORK_DIR="${TENBOX_WORK_DIR:-/tmp/tenbox-rootfs-copaw}"

# Parse arguments
FORCE_REBUILD=false
FROM_STEP=0
LIST_STEPS=false
SHOW_STATUS=false
OUTPUT_ARG=""

show_help() {
    cat << 'HELP'
Usage: ./make-rootfs.sh [OPTIONS] [output.qcow2]

Build a Debian rootfs image for TenBox.

Options:
  --help          Show this help message
  --status        Show current build progress
  --list-steps    Show all build steps with numbers
  --force         Force rebuild from scratch (clear all checkpoints)
  --from-step N   Resume from step N (use --list-steps to see numbers)

Examples:
  ./make-rootfs.sh                    # Normal build (resume if interrupted)
  ./make-rootfs.sh --status           # Check progress
  ./make-rootfs.sh --force            # Rebuild from scratch
HELP
    exit 0
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --help|-h)
            show_help
            ;;
        --force)
            FORCE_REBUILD=true
            shift
            ;;
        --from-step)
            FROM_STEP="$2"
            shift 2
            ;;
        --list-steps)
            LIST_STEPS=true
            shift
            ;;
        --status)
            SHOW_STATUS=true
            shift
            ;;
        *)
            OUTPUT_ARG="$1"
            shift
            ;;
    esac
done

if [ -n "$OUTPUT_ARG" ]; then
    OUTPUT="$(realpath -m "$OUTPUT_ARG")"
else
    OUTPUT=""  # will be determined after copaw version is known
    OUTPUT_DIR="$(realpath -m "$BUILD_DIR/share")"
fi

COPAW_VERSION=""

# Step definitions
STEPS=(
    "create_image"
    "mount_image"
    "debootstrap"
    "setup_chroot"
    "config_basic"
    "apt_update"
    "install_xfce"
    "install_spice"
    "install_guest_agent"
    "install_devtools"
    "install_audio"
    "install_ibus"
    "install_usertools"
    "install_nodejs"
    "install_ntp"
    "install_copaw"
    "config_copaw"
    "config_locale"
    "config_services"
    "config_virtio_gpu"
    "config_network"
    "config_virtiofs"
    "config_spice"
    "config_guest_agent"
    "verify_install"
    "cleanup_chroot"
    "unmount_image"
    "convert_qcow2"
)

STEP_DESCRIPTIONS=(
    "Create raw image file"
    "Mount image"
    "Bootstrap Debian"
    "Setup chroot environment"
    "Basic system configuration"
    "Update apt sources"
    "Install XFCE desktop"
    "Install SPICE vdagent"
    "Install Guest Agent"
    "Install development tools"
    "Install audio (PulseAudio + ALSA)"
    "Install IBus Chinese input method"
    "Install user tools (Chromium, etc.)"
    "Install Node.js 22"
    "Install NTP time sync"
    "Install Copaw"
    "Configure Copaw autostart"
    "Configure locale"
    "Configure systemd services"
    "Configure virtio-gpu resize"
    "Configure network"
    "Configure virtio-fs"
    "Configure SPICE"
    "Configure Guest Agent"
    "Verify installation"
    "Cleanup chroot"
    "Unmount image"
    "Convert to qcow2"
)

# Show steps and exit
if $LIST_STEPS; then
    echo "Available steps:"
    for i in "${!STEPS[@]}"; do
        printf "  %2d. %-20s - %s\n" "$i" "${STEPS[$i]}" "${STEP_DESCRIPTIONS[$i]}"
    done
    exit 0
fi

# Show status and exit
if $SHOW_STATUS; then
    echo "Build progress:"
    for i in "${!STEPS[@]}"; do
        if [ -f "$CHECKPOINT_DIR/${STEPS[$i]}.done" ]; then
            status="✓ done"
        else
            status="  pending"
        fi
        printf "  %2d. %-20s %s\n" "$i" "${STEPS[$i]}" "$status"
    done
    exit 0
fi

# Clear checkpoints if force rebuild
if $FORCE_REBUILD; then
    echo "Force rebuild: clearing all checkpoints and work directory..."
    rm -f "$CHECKPOINT_DIR"/*.done
    rm -rf "$WORK_DIR"
fi

# Clear checkpoints from specified step onwards
if [ "$FROM_STEP" -gt 0 ]; then
    echo "Resuming from step $FROM_STEP: clearing subsequent checkpoints..."
    for i in "${!STEPS[@]}"; do
        if [ "$i" -ge "$FROM_STEP" ]; then
            rm -f "$CHECKPOINT_DIR/${STEPS[$i]}.done"
        fi
    done
fi

# Checkpoint helpers
step_done() {
    [ -f "$CHECKPOINT_DIR/$1.done" ]
}

mark_done() {
    touch "$CHECKPOINT_DIR/$1.done"
}

# DrvFS (/mnt/*) does not support mknod through loop devices.
# Build everything on the native Linux filesystem, copy result back.
MOUNT_DIR=""
mkdir -p "$WORK_DIR"

cleanup() {
    echo "Cleaning up..."
    if [ -n "$MOUNT_DIR" ] && [ -d "$MOUNT_DIR" ]; then
        for sub in dev/pts proc sys dev; do
            mountpoint -q "$MOUNT_DIR/$sub" 2>/dev/null && \
                sudo umount -l "$MOUNT_DIR/$sub" 2>/dev/null || true
        done
        mountpoint -q "$MOUNT_DIR" 2>/dev/null && \
            (sudo umount "$MOUNT_DIR" 2>/dev/null || sudo umount -l "$MOUNT_DIR" 2>/dev/null || true)
    fi
    # Don't remove WORK_DIR on failure so we can resume
    if [ "${BUILD_SUCCESS:-false}" = "true" ]; then
        rm -rf "$WORK_DIR"
    else
        echo "Work directory preserved for resume: $WORK_DIR"
    fi
}
trap cleanup EXIT

# Resume: reuse existing work directory if it has rootfs.raw (fixed path, no .work_dir needed)
if [ -d "$WORK_DIR" ] && [ -f "$WORK_DIR/rootfs.raw" ]; then
    echo "Resuming with existing work directory: $WORK_DIR"
    # Clear runtime checkpoints (mount states don't persist across runs)
    rm -f "$CHECKPOINT_DIR/mount_image.done"
    rm -f "$CHECKPOINT_DIR/setup_chroot.done"
    rm -f "$CHECKPOINT_DIR/cleanup_chroot.done"
    rm -f "$CHECKPOINT_DIR/unmount_image.done"
fi

MOUNT_DIR="$WORK_DIR/mnt"
mkdir -p "$MOUNT_DIR"

total_steps=${#STEPS[@]}
current_step=0

run_step() {
    local step_name="$1"
    local step_desc="$2"
    shift 2
    
    current_step=$((current_step + 1))
    
    if step_done "$step_name"; then
        echo "[$current_step/$total_steps] $step_desc... (skipped, already done)"
        return 0
    fi
    
    echo "[$current_step/$total_steps] $step_desc..."
    "$@"
    mark_done "$step_name"
}

# Step implementations

do_create_image() {
    if [ ! -f "$WORK_DIR/rootfs.raw" ]; then
        truncate -s "$ROOTFS_SIZE" "$WORK_DIR/rootfs.raw"
        mkfs.ext4 -F "$WORK_DIR/rootfs.raw"
    fi
}

do_mount_image() {
    if ! mountpoint -q "$MOUNT_DIR" 2>/dev/null; then
        sudo mount -o loop "$WORK_DIR/rootfs.raw" "$MOUNT_DIR"
    fi
}

do_debootstrap() {
    # Check if already bootstrapped
    if [ -f "$MOUNT_DIR/etc/debian_version" ]; then
        echo "  Debian already bootstrapped"
        return 0
    fi
    
    if [ -f "$CACHE_TAR" ]; then
        echo "  Using cached tarball: $CACHE_TAR"
        if ! sudo debootstrap --include="$INCLUDE_PKGS" \
            --unpack-tarball="$CACHE_TAR" "$SUITE" "$MOUNT_DIR" "$MIRROR"; then
            echo "  Cache tarball failed, removing stale cache and cleaning mount dir..."
            rm -f "$CACHE_TAR"
            sudo rm -rf "${MOUNT_DIR:?}"/*
            sudo debootstrap --include="$INCLUDE_PKGS" \
                "$SUITE" "$MOUNT_DIR" "$MIRROR"
        fi
    else
        echo "  No cache found, downloading packages (first run)..."
        sudo debootstrap --include="$INCLUDE_PKGS" \
            --make-tarball="$CACHE_TAR" "$SUITE" "$WORK_DIR/tarball-tmp" "$MIRROR"
        rm -rf "$WORK_DIR/tarball-tmp"
        sudo debootstrap --include="$INCLUDE_PKGS" \
            --unpack-tarball="$CACHE_TAR" "$SUITE" "$MOUNT_DIR" "$MIRROR"
    fi
}

do_setup_chroot() {
    # Ensure DNS works inside chroot
    sudo cp /etc/resolv.conf "$MOUNT_DIR/etc/resolv.conf"
    
    # Mount proc/sys/dev for chroot package installation
    mountpoint -q "$MOUNT_DIR/proc" 2>/dev/null || sudo mount --bind /proc "$MOUNT_DIR/proc"
    mountpoint -q "$MOUNT_DIR/sys" 2>/dev/null || sudo mount --bind /sys "$MOUNT_DIR/sys"
    mountpoint -q "$MOUNT_DIR/dev" 2>/dev/null || sudo mount --bind /dev "$MOUNT_DIR/dev"
    sudo mkdir -p "$MOUNT_DIR/dev/pts"
    mountpoint -q "$MOUNT_DIR/dev/pts" 2>/dev/null || sudo mount -t devpts devpts "$MOUNT_DIR/dev/pts"
    
    # Prevent service start failures in chroot
    sudo tee "$MOUNT_DIR/usr/sbin/policy-rc.d" > /dev/null << 'PRC'
#!/bin/sh
exit 101
PRC
    sudo chmod +x "$MOUNT_DIR/usr/sbin/policy-rc.d"
    
    # Setup apt cache directory (bind mount for reuse)
    sudo mkdir -p "$MOUNT_DIR/var/cache/apt/archives"
    if ! mountpoint -q "$MOUNT_DIR/var/cache/apt/archives" 2>/dev/null; then
        sudo mount --bind "$APT_CACHE_DIR" "$MOUNT_DIR/var/cache/apt/archives"
    fi
    
    # Copy rootfs helper scripts and services
    sudo cp -r "$SCRIPT_DIR/../rootfs-scripts" "$MOUNT_DIR/tmp/"
    sudo cp -r "$SCRIPT_DIR/../rootfs-services" "$MOUNT_DIR/tmp/"
    sudo cp -r "$SCRIPT_DIR/../rootfs-configs" "$MOUNT_DIR/tmp/"
}

do_config_basic() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << EOF
# Skip if already configured
if id $USER_NAME &>/dev/null; then
    echo "  Basic config already done"
    exit 0
fi

echo "root:$ROOT_PASSWORD" | chpasswd

useradd -m -s /bin/bash $USER_NAME
echo "$USER_NAME:$USER_PASSWORD" | chpasswd
usermod -aG sudo $USER_NAME
echo "$USER_NAME ALL=(ALL:ALL) NOPASSWD: ALL" > /etc/sudoers.d/$USER_NAME
chmod 440 /etc/sudoers.d/$USER_NAME

echo "tenbox-vm" > /etc/hostname
cat > /etc/hosts << 'HOSTS'
127.0.0.1   localhost
127.0.0.1   tenbox-vm
::1         localhost ip6-localhost ip6-loopback
HOSTS
echo "/dev/vda / ext4 defaults 0 1" > /etc/fstab
EOF
}

do_apt_update() {
    # Always write apt sources (ensures correct config even on resume)
    sudo chroot "$MOUNT_DIR" /bin/bash -e << EOF
rm -f /etc/apt/sources.list
mkdir -p /etc/apt/sources.list.d
cat > /etc/apt/sources.list.d/debian.sources << DEB822
Types: deb
URIs: $MIRROR
Suites: $SUITE $SUITE-updates
Components: main contrib non-free non-free-firmware
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg

Types: deb
URIs: $MIRROR_SECURITY
Suites: $SUITE-security
Components: main contrib non-free non-free-firmware
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
DEB822

apt-get update
update-ca-certificates --fresh 2>/dev/null || true
EOF
}

do_install_xfce() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
# Check if already installed
if dpkg -s xfce4 &>/dev/null; then
    echo "  XFCE already installed"
    exit 0
fi

DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    xfce4 xfce4-terminal xfce4-power-manager \
    lightdm \
    xserver-xorg-core xserver-xorg-input-libinput \
    xfonts-base fonts-dejavu-core fonts-liberation fonts-noto-cjk fonts-noto-color-emoji \
    locales \
    dbus-x11 at-spi2-core \
    policykit-1 policykit-1-gnome \
    mousepad ristretto \
    thunar thunar-archive-plugin engrampa \
    tumbler xfce4-taskmanager
EOF
}

do_install_spice() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if dpkg -s spice-vdagent &>/dev/null; then
    echo "  SPICE vdagent already installed"
    exit 0
fi
DEBIAN_FRONTEND=noninteractive apt-get install -y spice-vdagent
EOF
}

do_install_guest_agent() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if dpkg -s qemu-guest-agent &>/dev/null; then
    echo "  qemu-guest-agent already installed"
    exit 0
fi
DEBIAN_FRONTEND=noninteractive apt-get install -y qemu-guest-agent
EOF
}

do_config_guest_agent() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if [ -f /etc/udev/rules.d/99-qemu-guest-agent.rules ]; then
    echo "  Guest agent already configured"
    exit 0
fi

echo "Setting up qemu-guest-agent..."

cat > /etc/udev/rules.d/99-qemu-guest-agent.rules << 'UDEV'
SUBSYSTEM=="virtio-ports", ATTR{name}=="org.qemu.guest_agent.0", SYMLINK+="virtio-ports/org.qemu.guest_agent.0", TAG+="systemd"
UDEV

mkdir -p /etc/systemd/system/qemu-guest-agent.service.d
cat > /etc/systemd/system/qemu-guest-agent.service.d/override.conf << 'OVERRIDE'
[Unit]
ConditionPathExists=/dev/virtio-ports/org.qemu.guest_agent.0

[Service]
ExecStart=
ExecStart=/usr/sbin/qemu-ga --method=virtio-serial --path=/dev/virtio-ports/org.qemu.guest_agent.0
OVERRIDE

systemctl enable qemu-guest-agent.service 2>/dev/null || true
EOF
}

do_install_devtools() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if dpkg -s python3 &>/dev/null; then
    echo "  Dev tools already installed"
    exit 0
fi
echo "Installing development tools..."
DEBIAN_FRONTEND=noninteractive apt-get install -y \
    python3 python3-pip python3-venv
EOF
}

do_install_usertools() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << EOF
if dpkg -s chromium &>/dev/null; then
    echo "  User tools already installed"
    exit 0
fi
echo "Installing user tools (browser, etc.)..."
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    chromium

echo "Setting PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH in .bashrc..."
su - $USER_NAME -c 'echo "export PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH=/usr/bin/chromium" >> ~/.bashrc'
EOF
}

do_install_audio() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if dpkg -s pulseaudio &>/dev/null && dpkg -s pavucontrol &>/dev/null; then
    echo "  Audio packages already installed"
    exit 0
fi
echo "Installing PulseAudio + ALSA for virtio-snd..."
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    pulseaudio pulseaudio-utils \
    alsa-utils \
    pavucontrol \
    xfce4-pulseaudio-plugin
EOF
}

do_install_ibus() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << EOF
if dpkg -s ibus-libpinyin &>/dev/null; then
    echo "  IBus already installed"
    exit 0
fi
echo "Installing IBus Chinese input method..."
DEBIAN_FRONTEND=noninteractive apt-get install -y \
    ibus ibus-libpinyin ibus-gtk3 ibus-gtk4

cat >> /home/$USER_NAME/.bashrc << 'IBUS'

# IBus input method
export GTK_IM_MODULE=ibus
export QT_IM_MODULE=ibus
export XMODIFIERS=@im=ibus
IBUS
EOF
}

do_install_nodejs() {
    if [ ! -f "$CACHE_NODESOURCE" ] || [ ! -s "$CACHE_NODESOURCE" ]; then
        echo "  Downloading NodeSource setup script..."
        rm -f "$CACHE_NODESOURCE" "$CACHE_NODESOURCE.tmp"
        curl -fsSL https://deb.nodesource.com/setup_22.x -o "$CACHE_NODESOURCE.tmp"
        mv "$CACHE_NODESOURCE.tmp" "$CACHE_NODESOURCE"
    fi
    sudo cp "$CACHE_NODESOURCE" "$MOUNT_DIR/tmp/nodesource_setup.sh"

    sudo chroot "$MOUNT_DIR" /bin/bash -e << EOF
if command -v node &>/dev/null && node --version | grep -q "v22"; then
    echo "  Node.js 22 already installed"
    exit 0
fi
echo "Installing Node.js 22..."
bash /tmp/nodesource_setup.sh
DEBIAN_FRONTEND=noninteractive apt-get install -y nodejs
rm -f /tmp/nodesource_setup.sh

npm config set registry https://registry.npmmirror.com --global
echo "registry=https://registry.npmmirror.com" >> /etc/npmrc
su - $USER_NAME -c "npm config set registry https://registry.npmmirror.com"
EOF
}

do_install_ntp() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if dpkg -s systemd-timesyncd &>/dev/null; then
    echo "  NTP time sync already installed"
    exit 0
fi
DEBIAN_FRONTEND=noninteractive apt-get install -y systemd-timesyncd

mkdir -p /etc/systemd/timesyncd.conf.d
cat > /etc/systemd/timesyncd.conf.d/tenbox.conf << 'NTP'
[Time]
NTP=ntp.aliyun.com
FallbackNTP=cn.pool.ntp.org pool.ntp.org
NTP

systemctl enable systemd-timesyncd.service 2>/dev/null || true
timedatectl set-ntp true 2>/dev/null || true
EOF
}

do_install_copaw() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << EOF
if [ -d /home/$USER_NAME/.copaw ]; then
    echo "  Copaw already installed"
    exit 0
fi

echo "Installing Copaw..."
su - $USER_NAME -c 'curl -fsSL https://copaw.agentscope.io/install.sh | bash'

echo "Initializing Copaw..."
su - $USER_NAME -c '~/.copaw/bin/copaw init --defaults --accept-security'

EOF

    # Extract version for output filename
    COPAW_VERSION=$(sudo chroot "$MOUNT_DIR" su - "$USER_NAME" -c \
        '~/.copaw/venv/bin/python -c "from copaw.__version__ import __version__; print(__version__)"' 2>/dev/null || true)
    if [ -n "$COPAW_VERSION" ]; then
        echo "  Copaw version: $COPAW_VERSION"
    else
        echo "  WARNING: Could not detect Copaw version"
    fi
}

do_config_copaw() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << EOF
if [ -f /home/$USER_NAME/.config/autostart/copaw.desktop ]; then
    echo "  Copaw already configured"
    exit 0
fi

echo "Setting up Copaw autostart..."
mkdir -p /home/$USER_NAME/.config/autostart
cat > /home/$USER_NAME/.config/autostart/copaw.desktop << 'DESKTOP'
[Desktop Entry]
Type=Application
Exec=xfce4-terminal -e "bash -ic 'echo Starting CoPaw...; copaw app --host 0.0.0.0; exec bash'"
Hidden=false
NoDisplay=false
X-GNOME-Autostart-enabled=true
Name=Copaw AutoRun
Comment=Start copaw app in terminal on login
DESKTOP
chown -R $USER_NAME:$USER_NAME /home/$USER_NAME/.config
EOF
}

do_config_locale() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
# Check if locale already configured
if locale -a 2>/dev/null | grep -q "zh_CN.utf8"; then
    echo "  Locale already configured"
    exit 0
fi
sed -i 's/^# *zh_CN.UTF-8/zh_CN.UTF-8/' /etc/locale.gen
sed -i 's/^# *en_US.UTF-8/en_US.UTF-8/' /etc/locale.gen
locale-gen
update-locale LANG=zh_CN.UTF-8

# Set timezone to China (Asia/Shanghai)
ln -sf /usr/share/zoneinfo/Asia/Shanghai /etc/localtime
echo "Asia/Shanghai" > /etc/timezone
EOF
}

do_config_services() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << EOF
# Skip if already configured
if [ -f /etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf ]; then
    echo "  Services already configured"
    exit 0
fi

mkdir -p /etc/polkit-1/rules.d
cp /tmp/rootfs-services/50-user-power.rules /etc/polkit-1/rules.d/

mkdir -p /etc/systemd/system/serial-getty@ttyS0.service.d
cp /tmp/rootfs-services/serial-getty-autologin.conf /etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf

mkdir -p /etc/lightdm/lightdm.conf.d
cat > /etc/lightdm/lightdm.conf.d/50-autologin.conf << LDM
[Seat:*]
autologin-user=$USER_NAME
autologin-user-timeout=0
autologin-session=xfce
user-session=xfce
greeter-session=lightdm-gtk-greeter
LDM

systemctl enable networking.service 2>/dev/null || true
systemctl set-default graphical.target
systemctl enable lightdm.service 2>/dev/null || true
EOF
}

do_config_virtio_gpu() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if [ -f /etc/udev/rules.d/95-virtio-gpu-resize.rules ]; then
    echo "  Virtio-GPU already configured"
    exit 0
fi

echo "Setting up virtio-gpu auto-resize..."
cat > /etc/udev/rules.d/95-virtio-gpu-resize.rules << 'UDEV'
ACTION=="change", SUBSYSTEM=="drm", ENV{HOTPLUG}=="1", RUN+="/usr/bin/bash -c '/usr/local/bin/virtio-gpu-resize.sh &'"
UDEV

cp /tmp/rootfs-scripts/virtio-gpu-resize.sh /usr/local/bin/
chmod +x /usr/local/bin/virtio-gpu-resize.sh
EOF
}

do_config_network() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if [ -f /etc/network/interfaces ] && grep -q "eth0" /etc/network/interfaces; then
    echo "  Network already configured"
    exit 0
fi

mkdir -p /etc/network
cat > /etc/network/interfaces << 'NET'
auto lo
iface lo inet loopback
auto eth0
iface eth0 inet dhcp
NET
EOF
}

do_config_virtiofs() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if [ -f /etc/systemd/system/virtiofs-automount.service ]; then
    echo "  Virtio-FS already configured"
    exit 0
fi

echo "Setting up virtio-fs auto-mount..."
mkdir -p /mnt/shared

cp /tmp/rootfs-scripts/virtiofs-automount /usr/local/bin/
chmod +x /usr/local/bin/virtiofs-automount

cp /tmp/rootfs-scripts/virtiofs-desktop-sync /usr/local/bin/
chmod +x /usr/local/bin/virtiofs-desktop-sync

cp /tmp/rootfs-services/virtiofs-automount.service /etc/systemd/system/
systemctl enable virtiofs-automount.service 2>/dev/null || true

cp /tmp/rootfs-services/virtiofs-desktop-sync.service /etc/systemd/system/
systemctl enable virtiofs-desktop-sync.service 2>/dev/null || true
EOF
}

do_config_spice() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if [ -f /etc/udev/rules.d/99-spice-vdagent.rules ]; then
    echo "  SPICE already configured"
    exit 0
fi

echo "Setting up spice-vdagent..."
cat > /etc/udev/rules.d/99-spice-vdagent.rules << 'UDEV'
SUBSYSTEM=="virtio-ports", ATTR{name}=="com.redhat.spice.0", SYMLINK+="virtio-ports/com.redhat.spice.0"
UDEV

mkdir -p /etc/systemd/system/spice-vdagentd.service.d
cp /tmp/rootfs-services/spice-vdagentd-override.conf /etc/systemd/system/spice-vdagentd.service.d/override.conf

systemctl enable spice-vdagentd.service 2>/dev/null || true
EOF
}

do_verify_install() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
echo "Verifying installation..."
FAIL=0
check() {
    local label="$1"; shift
    if "$@" &>/dev/null; then
        printf "  ✓ %s\n" "$label"
    else
        printf "  ✗ %s\n" "$label"
        FAIL=1
    fi
}

check "init"              test -x /sbin/init
check "systemd"           dpkg -s systemd
check "xfce4"             dpkg -s xfce4
check "lightdm"           dpkg -s lightdm
check "spice-vdagent"     dpkg -s spice-vdagent
check "qemu-guest-agent"  dpkg -s qemu-guest-agent
check "pulseaudio"        dpkg -s pulseaudio
check "node"              command -v node
check "ntp"               dpkg -s systemd-timesyncd
check "copaw"             test -d /home/tenbox/.copaw
check "chromium"          dpkg -s chromium
check "curl"              command -v curl
check "wget"              command -v wget
check "vim"               command -v vim

if [ "$FAIL" -ne 0 ]; then
    echo "WARNING: some components are missing!"
fi
EOF
}

do_cleanup_chroot() {
    # Detect version from chroot if not already known (e.g. on resume)
    if [ -z "$COPAW_VERSION" ]; then
        COPAW_VERSION=$(sudo chroot "$MOUNT_DIR" su - "$USER_NAME" -c \
            '~/.copaw/venv/bin/python -c "from copaw.__version__ import __version__; print(__version__)"' 2>/dev/null || true)
        [ -n "$COPAW_VERSION" ] && echo "  Detected Copaw version: $COPAW_VERSION"
    fi

    # Clean apt cache inside chroot (but keep host cache)
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
apt-get clean
rm -rf /var/lib/apt/lists/*
rm -rf /var/log/*.log /var/log/apt/* /var/log/dpkg.log
EOF
    
    sudo rm -f "$MOUNT_DIR/usr/sbin/policy-rc.d"
    sudo rm -rf "$MOUNT_DIR/tmp/rootfs-scripts" "$MOUNT_DIR/tmp/rootfs-services" "$MOUNT_DIR/tmp/rootfs-configs"
    sudo rm -f "$MOUNT_DIR/etc/resolv.conf"
    
    # Unmount apt cache
    mountpoint -q "$MOUNT_DIR/var/cache/apt/archives" 2>/dev/null && \
        sudo umount "$MOUNT_DIR/var/cache/apt/archives" || true
    
    # Unmount proc/sys/dev (dev/pts must be unmounted before dev)
    sudo umount "$MOUNT_DIR/dev/pts" 2>/dev/null || true
    sudo umount "$MOUNT_DIR/proc" "$MOUNT_DIR/sys" "$MOUNT_DIR/dev" 2>/dev/null || true
}

do_unmount_image() {
    sudo umount "$MOUNT_DIR" 2>/dev/null || true
    MOUNT_DIR=""
}

do_convert_qcow2() {
    # Resolve final output path with version if not explicitly specified
    if [ -z "$OUTPUT" ]; then
        if [ -n "$COPAW_VERSION" ]; then
            OUTPUT="$OUTPUT_DIR/rootfs-copaw-${COPAW_VERSION}-x86_64.qcow2"
        else
            OUTPUT="$OUTPUT_DIR/rootfs-copaw-x86_64.qcow2"
        fi
    fi
    mkdir -p "$(dirname "$OUTPUT")"

    echo "Converting to qcow2..."
    qemu-img convert -f raw -O qcow2 -o cluster_size=65536,compression_type=zstd -c "$WORK_DIR/rootfs.raw" "$OUTPUT"
}

# Execute all steps
run_step "create_image"   "Creating raw image"        do_create_image
run_step "mount_image"    "Mounting image"            do_mount_image
run_step "debootstrap"    "Bootstrapping Debian"      do_debootstrap
run_step "setup_chroot"   "Setting up chroot"         do_setup_chroot
run_step "config_basic"   "Basic configuration"       do_config_basic
run_step "apt_update"     "Updating apt sources"      do_apt_update
run_step "install_xfce"   "Installing XFCE desktop"   do_install_xfce
run_step "install_spice"  "Installing SPICE vdagent"  do_install_spice
run_step "install_guest_agent" "Installing Guest Agent" do_install_guest_agent
run_step "install_devtools" "Installing dev tools"    do_install_devtools
run_step "install_audio"  "Installing audio"          do_install_audio
run_step "install_ibus"   "Installing IBus"           do_install_ibus
run_step "install_usertools" "Installing user tools"  do_install_usertools
run_step "install_nodejs" "Installing Node.js"        do_install_nodejs
run_step "install_ntp"    "Installing NTP time sync"  do_install_ntp
run_step "install_copaw"  "Installing Copaw"          do_install_copaw
run_step "config_copaw"   "Configuring Copaw"         do_config_copaw
run_step "config_locale"  "Configuring locale"        do_config_locale
run_step "config_services" "Configuring services"     do_config_services
run_step "config_virtio_gpu" "Configuring virtio-gpu" do_config_virtio_gpu
run_step "config_network" "Configuring network"       do_config_network
run_step "config_virtiofs" "Configuring virtio-fs"    do_config_virtiofs
run_step "config_spice"   "Configuring SPICE"         do_config_spice
run_step "config_guest_agent" "Configuring Guest Agent" do_config_guest_agent
run_step "verify_install" "Verifying installation"    do_verify_install
run_step "cleanup_chroot" "Cleaning up chroot"        do_cleanup_chroot
run_step "unmount_image"  "Unmounting image"          do_unmount_image
run_step "convert_qcow2"  "Converting to qcow2"       do_convert_qcow2

# Mark build as successful
BUILD_SUCCESS=true
rm -f "$CHECKPOINT_DIR"/*.done

echo ""
echo "============================================"
echo "Done: $OUTPUT ($(ls -lh "$OUTPUT" | awk '{print $5}'))"
echo "============================================"
