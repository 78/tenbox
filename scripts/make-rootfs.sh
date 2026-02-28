#!/bin/bash
# Build a Debian base rootfs as qcow2 for TenBox Phase 2.
# Requires: debootstrap, qemu-utils. Run as root in WSL2 or Linux.
#
# Features:
#   - Checkpoint system: resume from last successful step after failure
#   - APT cache: reuse downloaded packages across runs
#   - External script cache: NodeSource, OpenClaw install scripts
#   - Edit mode: modify existing rootfs without full rebuild
#
# Usage:
#   ./make-rootfs.sh [output.qcow2]           # Normal run (resume if interrupted)
#   ./make-rootfs.sh --force [output.qcow2]   # Force rebuild from scratch
#   ./make-rootfs.sh --from-step N            # Resume from step N
#   ./make-rootfs.sh --edit                   # Edit existing rootfs (from cached raw)
#   ./make-rootfs.sh --list-steps             # Show all steps
#   ./make-rootfs.sh --status                 # Show current progress

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

# Cache directories
CACHE_DIR="$BUILD_DIR/.rootfs-cache"
CHECKPOINT_DIR="$CACHE_DIR/checkpoints"
APT_CACHE_DIR="$CACHE_DIR/apt-archives"
SCRIPTS_CACHE_DIR="$CACHE_DIR/scripts"
mkdir -p "$CHECKPOINT_DIR" "$APT_CACHE_DIR" "$SCRIPTS_CACHE_DIR"

# Cache files
CACHE_TAR="$CACHE_DIR/debootstrap-${SUITE}-base.tar"
CACHE_CHROME="$CACHE_DIR/google-chrome-stable_current_amd64.deb"
CACHE_NODESOURCE="$SCRIPTS_CACHE_DIR/nodesource_setup_22.x.sh"
CACHE_OPENCLAW="$SCRIPTS_CACHE_DIR/openclaw_install.sh"
CACHE_RAW="$CACHE_DIR/rootfs.raw"

# Parse arguments
FORCE_REBUILD=false
EDIT_MODE=false
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
  --edit          Edit existing rootfs using cached raw image
  --from-step N   Resume from step N (use --list-steps to see numbers)

Examples:
  ./make-rootfs.sh                    # Normal build (resume if interrupted)
  ./make-rootfs.sh --status           # Check progress
  ./make-rootfs.sh --force            # Rebuild from scratch
  ./make-rootfs.sh --edit             # Edit existing rootfs
  ./make-rootfs.sh --edit --from-step 14   # Edit from specific step
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
        --edit)
            EDIT_MODE=true
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

OUTPUT="$(realpath -m "${OUTPUT_ARG:-$BUILD_DIR/share/rootfs.qcow2}")"

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
    "install_chrome"
    "config_chrome"
    "install_devtools"
    "install_usertools"
    "install_audio"
    "install_nodejs"
    "install_openclaw"
    "config_locale"
    "config_services"
    "config_virtio_gpu"
    "config_network"
    "config_virtiofs"
    "config_spice"
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
    "Install Google Chrome"
    "Configure Chrome"
    "Install development tools"
    "Install user tools (editor, viewer, etc.)"
    "Install audio (PulseAudio + ALSA)"
    "Install Node.js 22"
    "Install OpenClaw"
    "Configure locale"
    "Configure systemd services"
    "Configure virtio-gpu resize"
    "Configure network"
    "Configure virtio-fs"
    "Configure SPICE"
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
    echo ""
    if [ -f "$CACHE_RAW" ]; then
        echo "Cached rootfs.raw: $(du -h "$CACHE_RAW" | cut -f1) (--edit available)"
    else
        echo "Cached rootfs.raw: not found (--edit not available)"
    fi
    exit 0
fi

# Clear checkpoints if force rebuild
if $FORCE_REBUILD; then
    echo "Force rebuild: clearing all checkpoints and work directory..."
    rm -f "$CHECKPOINT_DIR"/*.done
    rm -f "$CHECKPOINT_DIR/.work_dir"
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

# Handle --edit mode: restore from cached raw image
if $EDIT_MODE; then
    if [ ! -f "$CACHE_RAW" ]; then
        echo "Error: No cached rootfs.raw found at $CACHE_RAW"
        echo "Run a full build first, or use --force to rebuild from scratch."
        exit 1
    fi
    echo "Edit mode: using cached rootfs.raw"
    
    # Mark early steps as done (we have a complete image)
    touch "$CHECKPOINT_DIR/create_image.done"
    touch "$CHECKPOINT_DIR/debootstrap.done"
    
    # If --from-step not specified, default to config_services (step 15)
    if [ "$FROM_STEP" -eq 0 ]; then
        FROM_STEP=15
        echo "  Defaulting to --from-step 15 (config_services)"
    fi
fi

# DrvFS (/mnt/*) does not support mknod through loop devices.
# Build everything on the native Linux filesystem, copy result back.
WORK_DIR=$(mktemp -d /tmp/make-rootfs.XXXXXX)
MOUNT_DIR=""

cleanup() {
    echo "Cleaning up..."
    if [ -n "$MOUNT_DIR" ] && [ -d "$MOUNT_DIR" ]; then
        for sub in proc sys dev; do
            mountpoint -q "$MOUNT_DIR/$sub" 2>/dev/null && \
                sudo umount -l "$MOUNT_DIR/$sub" 2>/dev/null || true
        done
        mountpoint -q "$MOUNT_DIR" 2>/dev/null && \
            (sudo umount "$MOUNT_DIR" 2>/dev/null || sudo umount -l "$MOUNT_DIR" 2>/dev/null || true)
    fi
    # Don't remove WORK_DIR on failure so we can resume
    if [ "${BUILD_SUCCESS:-false}" = "true" ]; then
        # Save raw image to cache for future --edit runs
        if [ -f "$WORK_DIR/rootfs.raw" ]; then
            echo "Saving rootfs.raw to cache for future edits..."
            cp "$WORK_DIR/rootfs.raw" "$CACHE_RAW"
        fi
        rm -rf "$WORK_DIR"
        rm -f "$CHECKPOINT_DIR/.work_dir"
    else
        echo "Work directory preserved for resume: $WORK_DIR"
        # Save work dir path for resume
        echo "$WORK_DIR" > "$CHECKPOINT_DIR/.work_dir"
    fi
}
trap cleanup EXIT

# Try to reuse existing work directory
if [ -f "$CHECKPOINT_DIR/.work_dir" ]; then
    SAVED_WORK_DIR=$(cat "$CHECKPOINT_DIR/.work_dir")
    if [ -d "$SAVED_WORK_DIR" ] && [ -f "$SAVED_WORK_DIR/rootfs.raw" ]; then
        echo "Resuming with existing work directory: $SAVED_WORK_DIR"
        rm -rf "$WORK_DIR"
        WORK_DIR="$SAVED_WORK_DIR"
        
        # Clear runtime checkpoints (mount states don't persist across runs)
        rm -f "$CHECKPOINT_DIR/mount_image.done"
        rm -f "$CHECKPOINT_DIR/setup_chroot.done"
        rm -f "$CHECKPOINT_DIR/cleanup_chroot.done"
        rm -f "$CHECKPOINT_DIR/unmount_image.done"
    fi
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
        if $EDIT_MODE && [ -f "$CACHE_RAW" ]; then
            echo "  Copying cached rootfs.raw to work directory..."
            cp "$CACHE_RAW" "$WORK_DIR/rootfs.raw"
        else
            truncate -s "$ROOTFS_SIZE" "$WORK_DIR/rootfs.raw"
            mkfs.ext4 -F "$WORK_DIR/rootfs.raw"
        fi
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
        sudo debootstrap --include="$INCLUDE_PKGS" \
            --unpack-tarball="$CACHE_TAR" "$SUITE" "$MOUNT_DIR" "$MIRROR"
    else
        echo "  No cache found, downloading packages (first run)..."
        sudo debootstrap --include="$INCLUDE_PKGS" \
            --make-tarball="$CACHE_TAR" "$SUITE" "$WORK_DIR/tarball-tmp" "$MIRROR"
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
    sudo cp -r "$SCRIPT_DIR/rootfs-scripts" "$MOUNT_DIR/tmp/"
    sudo cp -r "$SCRIPT_DIR/rootfs-services" "$MOUNT_DIR/tmp/"
}

do_config_basic() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
# Skip if already configured
if id openclaw &>/dev/null; then
    echo "  Basic config already done"
    exit 0
fi

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
EOF
}

do_apt_update() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
apt-get update
update-ca-certificates --fresh 2>/dev/null || true
EOF
}

do_install_xfce() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
# Check if already installed
if dpkg -l xfce4 &>/dev/null; then
    echo "  XFCE already installed"
    exit 0
fi

DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    xfce4 xfce4-terminal xfce4-power-manager \
    lightdm \
    xserver-xorg-core xserver-xorg-input-libinput \
    xfonts-base fonts-dejavu-core fonts-noto-cjk fonts-noto-color-emoji \
    locales \
    dbus-x11 at-spi2-core \
    policykit-1 policykit-1-gnome
EOF
}

do_install_spice() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if dpkg -l spice-vdagent &>/dev/null; then
    echo "  SPICE vdagent already installed"
    exit 0
fi
DEBIAN_FRONTEND=noninteractive apt-get install -y spice-vdagent
EOF
}

do_install_guest_agent() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if dpkg -l qemu-guest-agent &>/dev/null; then
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

do_install_chrome() {
    # Download Chrome (with cache, atomic write to avoid corruption)
    if [ -f "$CACHE_CHROME" ] && dpkg-deb --info "$CACHE_CHROME" &>/dev/null; then
        echo "  Using cached Chrome: $CACHE_CHROME"
    else
        echo "  Downloading Google Chrome..."
        rm -f "$CACHE_CHROME" "$CACHE_CHROME.tmp"
        wget -q https://dl.google.com/linux/direct/google-chrome-stable_current_amd64.deb -O "$CACHE_CHROME.tmp"
        mv "$CACHE_CHROME.tmp" "$CACHE_CHROME"
    fi
    sudo cp "$CACHE_CHROME" "$MOUNT_DIR/tmp/chrome.deb"
    
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if dpkg -l google-chrome-stable &>/dev/null; then
    echo "  Chrome already installed"
    rm -f /tmp/chrome.deb
    exit 0
fi
echo "Installing Google Chrome..."
DEBIAN_FRONTEND=noninteractive apt-get install -y /tmp/chrome.deb
rm -f /tmp/chrome.deb
EOF
}

do_config_chrome() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
# Skip if already configured
if [ -f /etc/opt/chrome/policies/managed/tenbox_policy.json ]; then
    echo "  Chrome already configured"
    exit 0
fi

mkdir -p /etc/opt/chrome/policies/managed
cat > /etc/opt/chrome/policies/managed/tenbox_policy.json << 'CHROME'
{
    "DefaultBrowserSettingEnabled": false,
    "BrowserSignin": 0,
    "SyncDisabled": true,
    "PasswordManagerEnabled": false,
    "AutofillAddressEnabled": false,
    "AutofillCreditCardEnabled": false,
    "TranslateEnabled": false,
    "RestoreOnStartup": 5,
    "MetricsReportingEnabled": false,
    "PromotionalTabsEnabled": false,
    "WelcomePageOnOSUpgradeEnabled": false,
    "ImportBookmarks": false,
    "BookmarkBarEnabled": true,
    "ShowHomeButton": true,
    "HomepageLocation": "chrome://newtab"
}
CHROME
mkdir -p /home/openclaw/.config/google-chrome
touch "/home/openclaw/.config/google-chrome/First Run"
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
EOF
}

do_install_devtools() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if dpkg -l python3 g++ cmake &>/dev/null; then
    echo "  Dev tools already installed"
    exit 0
fi
echo "Installing development tools..."
DEBIAN_FRONTEND=noninteractive apt-get install -y \
    python3 python3-pip python3-venv \
    g++ make cmake git \
    inotify-tools
EOF
}

do_install_usertools() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if dpkg -l mousepad &>/dev/null; then
    echo "  User tools already installed"
    exit 0
fi
echo "Installing user tools (text editor, image viewer, file manager, etc.)..."
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    mousepad \
    ristretto \
    thunar thunar-archive-plugin \
    engrampa \
    xfce4-taskmanager
EOF
}

do_install_audio() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if dpkg -l pulseaudio &>/dev/null; then
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

do_install_nodejs() {
    # Cache NodeSource setup script (atomic write)
    if [ ! -f "$CACHE_NODESOURCE" ] || [ ! -s "$CACHE_NODESOURCE" ]; then
        echo "  Downloading NodeSource setup script..."
        rm -f "$CACHE_NODESOURCE" "$CACHE_NODESOURCE.tmp"
        curl -fsSL https://deb.nodesource.com/setup_22.x -o "$CACHE_NODESOURCE.tmp"
        mv "$CACHE_NODESOURCE.tmp" "$CACHE_NODESOURCE"
    fi
    sudo cp "$CACHE_NODESOURCE" "$MOUNT_DIR/tmp/nodesource_setup.sh"
    
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if command -v node &>/dev/null && node --version | grep -q "v22"; then
    echo "  Node.js 22 already installed"
    exit 0
fi
echo "Installing Node.js 22..."
bash /tmp/nodesource_setup.sh
DEBIAN_FRONTEND=noninteractive apt-get install -y nodejs
rm -f /tmp/nodesource_setup.sh

# Configure npm to use Alibaba Cloud mirror
npm config set registry https://registry.npmmirror.com --global
echo "registry=https://registry.npmmirror.com" >> /etc/npmrc
su - openclaw -c "npm config set registry https://registry.npmmirror.com"
EOF
}

do_install_openclaw() {
    # Cache OpenClaw install script (atomic write)
    if [ ! -f "$CACHE_OPENCLAW" ] || [ ! -s "$CACHE_OPENCLAW" ]; then
        echo "  Downloading OpenClaw install script..."
        rm -f "$CACHE_OPENCLAW" "$CACHE_OPENCLAW.tmp"
        curl -fsSL https://openclaw.ai/install.sh -o "$CACHE_OPENCLAW.tmp"
        mv "$CACHE_OPENCLAW.tmp" "$CACHE_OPENCLAW"
    fi
    sudo cp "$CACHE_OPENCLAW" "$MOUNT_DIR/tmp/openclaw_install.sh"
    
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if command -v openclaw &>/dev/null; then
    echo "  OpenClaw already installed"
    exit 0
fi
echo "Installing OpenClaw..."
bash /tmp/openclaw_install.sh --no-onboard
rm -f /tmp/openclaw_install.sh
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
EOF
}

do_config_services() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
# Skip if already configured
if [ -f /etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf ]; then
    echo "  Services already configured"
    exit 0
fi

# Allow openclaw to shutdown/reboot from GUI without password (polkit rules)
mkdir -p /etc/polkit-1/rules.d
cp /tmp/rootfs-services/50-openclaw-power.rules /etc/polkit-1/rules.d/

mkdir -p /etc/systemd/system/serial-getty@ttyS0.service.d
cp /tmp/rootfs-services/serial-getty-autologin.conf /etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf

mkdir -p /etc/lightdm/lightdm.conf.d
cat > /etc/lightdm/lightdm.conf.d/50-autologin.conf << 'LDM'
[Seat:*]
autologin-user=openclaw
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
ls -la /sbin/init
echo "init OK: $(readlink -f /sbin/init)"
echo "nc:   $(which nc 2>/dev/null || echo MISSING)"
echo "free: $(which free 2>/dev/null || echo MISSING)"
echo "ps:   $(which ps 2>/dev/null || echo MISSING)"
echo "curl: $(which curl 2>/dev/null || echo MISSING)"
echo "wget: $(which wget 2>/dev/null || echo MISSING)"
echo "node: $(node --version 2>/dev/null || echo MISSING)"
EOF
}

do_cleanup_chroot() {
    # Clean apt cache inside chroot (but keep host cache)
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
apt-get clean
rm -rf /var/lib/apt/lists/*
EOF
    
    sudo rm -f "$MOUNT_DIR/usr/sbin/policy-rc.d"
    
    # Unmount apt cache
    mountpoint -q "$MOUNT_DIR/var/cache/apt/archives" 2>/dev/null && \
        sudo umount "$MOUNT_DIR/var/cache/apt/archives" || true
    
    # Unmount proc/sys/dev
    sudo umount "$MOUNT_DIR/proc" "$MOUNT_DIR/sys" "$MOUNT_DIR/dev" 2>/dev/null || true
}

do_unmount_image() {
    sudo umount "$MOUNT_DIR" 2>/dev/null || true
    MOUNT_DIR=""
}

do_convert_qcow2() {
    echo "Converting to qcow2..."
    # Prefer Windows qemu-img.exe (supports zstd), fallback to WSL version
    WIN_QEMU="/mnt/c/Program Files/qemu/qemu-img.exe"
    if [ -x "$WIN_QEMU" ]; then
        echo "  Using Windows qemu-img with zstd compression..."
        WIN_RAW=$(wslpath -w "$WORK_DIR/rootfs.raw")
        WIN_OUTPUT=$(wslpath -w "$OUTPUT")
        "$WIN_QEMU" convert -f raw -O qcow2 -o cluster_size=65536,compression_type=zstd -c "$WIN_RAW" "$WIN_OUTPUT"
    else
        echo "  Using WSL qemu-img with zlib compression..."
        qemu-img convert -f raw -O qcow2 -o cluster_size=65536 -c "$WORK_DIR/rootfs.raw" "$OUTPUT"
    fi
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
run_step "install_chrome" "Installing Chrome"         do_install_chrome
run_step "config_chrome"  "Configuring Chrome"        do_config_chrome
run_step "install_devtools" "Installing dev tools"    do_install_devtools
run_step "install_usertools" "Installing user tools"  do_install_usertools
run_step "install_audio"  "Installing audio"          do_install_audio
run_step "install_nodejs" "Installing Node.js"        do_install_nodejs
run_step "install_openclaw" "Installing OpenClaw"     do_install_openclaw
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
rm -f "$CHECKPOINT_DIR/.work_dir"
rm -f "$CHECKPOINT_DIR"/*.done

echo ""
echo "============================================"
echo "Done: $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"
echo "============================================"
