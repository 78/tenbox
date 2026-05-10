#!/bin/bash
# Build a Debian base rootfs as qcow2 for TenBox Phase 2.
# Requires: debootstrap, qemu-utils. Run as root in WSL2 or Linux.
#
# Features:
#   - Checkpoint system: resume from last successful step after failure
#   - APT cache: reuse downloaded packages across runs
#   - External script cache: NodeSource, OpenClaw install scripts
#
# Usage:
#   ./make-rootfs.sh [output.qcow2]           # Normal run (resume if interrupted)
#   ./make-rootfs.sh --force [output.qcow2]   # Force rebuild from scratch
#   ./make-rootfs.sh --from-step N            # Resume from step N
#   ./make-rootfs.sh --list-steps             # Show all steps
#   ./make-rootfs.sh --status                 # Show current progress

set -e

ROOTFS_SIZE="100G"
SUITE="trixie"
MIRROR="http://deb.debian.org/debian"
MIRROR_SECURITY="http://deb.debian.org/debian-security"
ROOT_PASSWORD="${ROOT_PASSWORD:-tenbox}"
USER_NAME="${USER_NAME:-tenbox}"
USER_PASSWORD="${USER_PASSWORD:-tenbox}"
INCLUDE_PKGS="systemd-sysv,udev,dbus,sudo,\
iproute2,iputils-ping,ifupdown,dhcpcd-base,\
ca-certificates,curl,wget,\
procps,psmisc,\
netcat-openbsd,net-tools,traceroute,bind9-dnsutils,\
less,vim,bash-completion,\
openssh-client,gnupg,apt-transport-https,\
lsof,strace,sysstat,\
kmod,pciutils,usbutils,\
coreutils,findutils,grep,gawk,sed,tar,gzip,bzip2,xz-utils,\
linux-image-amd64,iptables,util-linux,util-linux-extra"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$(mkdir -p "$SCRIPT_DIR/../../build" && cd "$SCRIPT_DIR/../../build" && pwd)"

# Cache directories
CACHE_DIR="$BUILD_DIR/.rootfs-cache"
CHECKPOINT_DIR="$CACHE_DIR/checkpoints-openclaw"
APT_CACHE_DIR="$CACHE_DIR/apt-archives"
SCRIPTS_CACHE_DIR="$CACHE_DIR/scripts"
mkdir -p "$CHECKPOINT_DIR" "$APT_CACHE_DIR" "$SCRIPTS_CACHE_DIR"

# Cache files
CACHE_TAR="$(realpath -m "$CACHE_DIR/debootstrap-${SUITE}-base.tar")"
CACHE_NODESOURCE="$SCRIPTS_CACHE_DIR/nodesource_setup_22.x.sh"
CACHE_OPENCLAW="$SCRIPTS_CACHE_DIR/openclaw_install.sh"

# Work dir must be on WSL Linux FS (/tmp), not NTFS (DrvFS /mnt/*) - loop devices need mknod
WORK_DIR="${TENBOX_WORK_DIR:-/tmp/tenbox-rootfs-openclaw}"

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
    OUTPUT=""  # will be determined after openclaw version is known
    OUTPUT_DIR="$(realpath -m "$BUILD_DIR/share")"
fi

OPENCLAW_VERSION=""

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
    "install_openclaw"
    "config_openclaw"
    "copy_readme"
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
    "Install OpenClaw"
    "Configure OpenClaw (tools, daemon)"
    "Copy desktop links (Help + OpenClaw)"
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
    sudo rm -rf "$WORK_DIR"
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
        sudo rm -rf "$WORK_DIR"
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
    # Remove stale apt locks left over from interrupted builds
    sudo rm -f "$MOUNT_DIR/var/cache/apt/archives/lock"
    sudo rm -f "$MOUNT_DIR/var/lib/apt/lists/lock"
    sudo rm -f "$MOUNT_DIR/var/lib/dpkg/lock"
    sudo rm -f "$MOUNT_DIR/var/lib/dpkg/lock-frontend"
    # Purge corrupt/truncated .deb files from shared apt cache
    for deb in "$APT_CACHE_DIR"/*.deb; do
        [ -f "$deb" ] || continue
        if ! dpkg-deb --info "$deb" >/dev/null 2>&1; then
            echo "  Removing corrupt cached package: $(basename "$deb")"
            rm -f "$deb"
        fi
    done

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
    adwaita-icon-theme-legacy \
    locales \
    dbus-x11 at-spi2-core \
    polkitd pkexec lxpolkit \
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
if dpkg -s python3 &>/dev/null && dpkg -s g++ &>/dev/null && dpkg -s cmake &>/dev/null; then
    echo "  Dev tools already installed"
    exit 0
fi
echo "Installing development tools..."
DEBIAN_FRONTEND=noninteractive apt-get install -y \
    python3 python3-pip python3-venv \
    g++ make cmake git
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
    chromium \
    ffmpeg jq

echo "Setting PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH in .bashrc..."
echo "export PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH=/usr/bin/chromium" >> /home/$USER_NAME/.bashrc
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
    # Cache NodeSource setup script (atomic write)
    if [ ! -f "$CACHE_NODESOURCE" ] || [ ! -s "$CACHE_NODESOURCE" ]; then
        echo "  Downloading NodeSource setup script..."
        rm -f "$CACHE_NODESOURCE" "$CACHE_NODESOURCE.tmp"
        curl -fsSL https://deb.nodesource.com/setup_22.x -o "$CACHE_NODESOURCE.tmp"
        mv "$CACHE_NODESOURCE.tmp" "$CACHE_NODESOURCE"
    fi
    sudo cp "$CACHE_NODESOURCE" "$MOUNT_DIR/tmp/nodesource_setup.sh"
    
    sudo chroot "$MOUNT_DIR" /bin/bash -e << EOF
if ! command -v node &>/dev/null || ! node --version | grep -q "v22"; then
    echo "Installing Node.js 22..."
    bash /tmp/nodesource_setup.sh
    DEBIAN_FRONTEND=noninteractive apt-get install -y nodejs
    rm -f /tmp/nodesource_setup.sh
else
    echo "  Node.js 22 already installed"
fi

# Global npm registry: Alibaba npmmirror (runs on fresh install and resume)
if command -v npm &>/dev/null; then
    npm config set registry https://registry.npmmirror.com --global
    runuser -l $USER_NAME -s /bin/bash -c "npm config set registry https://registry.npmmirror.com"
fi
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

do_install_openclaw() {
    # Cache OpenClaw install script (atomic write)
    if [ ! -f "$CACHE_OPENCLAW" ] || [ ! -s "$CACHE_OPENCLAW" ]; then
        echo "  Downloading OpenClaw install script..."
        rm -f "$CACHE_OPENCLAW" "$CACHE_OPENCLAW.tmp"
        curl -fsSL https://openclaw.ai/install.sh -o "$CACHE_OPENCLAW.tmp"
        mv "$CACHE_OPENCLAW.tmp" "$CACHE_OPENCLAW"
    fi
    sudo cp "$CACHE_OPENCLAW" "$MOUNT_DIR/tmp/openclaw_install.sh"
    
    sudo chroot "$MOUNT_DIR" /bin/bash -e << EOF
if runuser -l $USER_NAME -c 'command -v openclaw' &>/dev/null; then
    echo "  OpenClaw already installed"
    exit 0
fi
echo "Installing OpenClaw..."
runuser -l $USER_NAME -c 'bash /tmp/openclaw_install.sh --no-onboard'
rm -f /tmp/openclaw_install.sh

# Ensure npm global bin and OpenClaw env vars are set for interactive sessions
if ! grep -q 'npm-global/bin' /home/$USER_NAME/.bashrc 2>/dev/null; then
    cat >> /home/$USER_NAME/.bashrc << 'BASHRC'
export PATH="\$HOME/.npm-global/bin:\$PATH"
export NODE_COMPILE_CACHE=/var/tmp/openclaw-compile-cache
export OPENCLAW_NO_RESPAWN=1
BASHRC
fi
EOF

    # Extract version for output filename
    OPENCLAW_VERSION=$(sudo chroot "$MOUNT_DIR" runuser -l "$USER_NAME" -c \
        'PATH="$HOME/.npm-global/bin:$PATH" npm ls -g openclaw 2>/dev/null' \
        | grep 'openclaw@' | sed 's/.*openclaw@//' || true)
    if [ -n "$OPENCLAW_VERSION" ]; then
        echo "  OpenClaw version: $OPENCLAW_VERSION"
    else
        echo "  WARNING: Could not detect OpenClaw version"
    fi
}

do_config_openclaw() {
    # Write config script to avoid shell escaping issues with nested su/heredoc
    sudo tee "$MOUNT_DIR/tmp/openclaw_config.sh" > /dev/null << 'SCRIPT'
#!/bin/bash
set -e
export PATH="$HOME/.npm-global/bin:$PATH"
openclaw config set tools.profile full
openclaw config set tools.exec.security full
openclaw config set gateway '{"mode":"local","bind":"lan","auth":{"mode":"token","token":"tenbox"},"controlUi":{"allowInsecureAuth":true,"dangerouslyDisableDeviceAuth":true,"allowedOrigins":["*"]},"port":18789,"tailscale":{"mode":"off","resetOnExit":false}}'

# TenBox LLM proxy provider (guestfwd: 10.0.2.3:80 -> host proxy)
openclaw config set models.providers.tenbox '{"baseUrl":"http://10.0.2.3/v1","apiKey":"tenbox","api":"openai-completions","models":[{"id":"default","name":"Default (TenBox Proxy)","reasoning":false,"input":["text","image"],"contextWindow":200000,"maxTokens":65536,"cost":{"input":0,"output":0,"cacheRead":0,"cacheWrite":0}}]}'
openclaw config set models.mode merge
openclaw config set agents.defaults '{"model":{"primary":"tenbox/default"},"compaction":{"mode":"safeguard"},"workspace":"'"$HOME"'/.openclaw/workspace","models":{"tenbox/default":{}}}'
SCRIPT
    sudo chmod +x "$MOUNT_DIR/tmp/openclaw_config.sh"

    sudo chroot "$MOUNT_DIR" /bin/bash -e << EOF
# Configure openclaw settings
if [ -f /home/$USER_NAME/.openclaw/openclaw.json ] && \
   grep -q '"profile"' /home/$USER_NAME/.openclaw/openclaw.json 2>/dev/null; then
    echo "  OpenClaw config already set"
else
    echo "Configuring OpenClaw..."
    runuser -l $USER_NAME -c 'bash /tmp/openclaw_config.sh'
fi
rm -f /tmp/openclaw_config.sh

# Install systemd user service manually (openclaw daemon install needs running systemd)
USER_HOME=/home/$USER_NAME
UNIT_DIR=\$USER_HOME/.config/systemd/user

if [ -f "\$UNIT_DIR/openclaw-gateway.service" ]; then
    echo "  OpenClaw systemd service already installed"
else
    echo "Installing OpenClaw systemd service..."
    mkdir -p "\$UNIT_DIR"

    OC_VERSION=\$(node \$USER_HOME/.npm-global/lib/node_modules/openclaw/dist/index.js --version 2>/dev/null || echo "unknown")

    cat > "\$UNIT_DIR/openclaw-gateway.service" << UNIT
[Unit]
Description=OpenClaw Gateway (v\$OC_VERSION)
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/usr/bin/node \$USER_HOME/.npm-global/lib/node_modules/openclaw/dist/index.js gateway --port 18789
Restart=always
RestartSec=5
TimeoutStopSec=30
TimeoutStartSec=30
SuccessExitStatus=0 143
KillMode=control-group
Environment=HOME=\$USER_HOME
Environment=TMPDIR=/tmp
Environment=PATH=/usr/bin:\$USER_HOME/.local/bin:\$USER_HOME/.npm-global/bin:\$USER_HOME/bin:\$USER_HOME/.volta/bin:\$USER_HOME/.asdf/shims:\$USER_HOME/.bun/bin:\$USER_HOME/.nvm/current/bin:\$USER_HOME/.fnm/current/bin:\$USER_HOME/.local/share/pnpm:/usr/local/bin:/bin
Environment=OPENCLAW_GATEWAY_PORT=18789
Environment=OPENCLAW_SYSTEMD_UNIT=openclaw-gateway.service
Environment="OPENCLAW_WINDOWS_TASK_NAME=OpenClaw Gateway"
Environment=OPENCLAW_SERVICE_MARKER=openclaw
Environment=OPENCLAW_SERVICE_KIND=gateway
Environment=OPENCLAW_SERVICE_VERSION=\$OC_VERSION

[Install]
WantedBy=default.target
UNIT

    mkdir -p "\$UNIT_DIR/openclaw-gateway.service.d"
    cat > "\$UNIT_DIR/openclaw-gateway.service.d/override.conf" << OVERRIDE
[Service]
Environment=PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH=/usr/bin/chromium
Environment=NODE_COMPILE_CACHE=/var/tmp/openclaw-compile-cache
Environment=OPENCLAW_NO_RESPAWN=1
Environment=DISPLAY=:0
OVERRIDE

    # Enable the service (create symlink since systemctl --user is unavailable in chroot)
    mkdir -p "\$UNIT_DIR/default.target.wants"
    ln -sf ../openclaw-gateway.service "\$UNIT_DIR/default.target.wants/openclaw-gateway.service"

    chown -R $USER_NAME:$USER_NAME \$USER_HOME/.config
fi

# Enable lingering so gateway starts at boot even before user login
mkdir -p /var/lib/systemd/linger
touch /var/lib/systemd/linger/$USER_NAME
EOF
}

do_copy_readme() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << EOF
DESKTOP_DIR="/home/$USER_NAME/Desktop"
if [ -f "\$DESKTOP_DIR/Help.desktop" ] && [ -f "\$DESKTOP_DIR/OpenClaw.desktop" ]; then
    echo "  Desktop links already copied"
    exit 0
fi

mkdir -p "\$DESKTOP_DIR"
chown $USER_NAME:$USER_NAME "\$DESKTOP_DIR"

cp /tmp/rootfs-configs/Help.desktop "\$DESKTOP_DIR/Help.desktop"
chown $USER_NAME:$USER_NAME "\$DESKTOP_DIR/Help.desktop"
chmod +x "\$DESKTOP_DIR/Help.desktop"

cp /tmp/rootfs-configs/OpenClaw.desktop "\$DESKTOP_DIR/OpenClaw.desktop"
chown $USER_NAME:$USER_NAME "\$DESKTOP_DIR/OpenClaw.desktop"
chmod +x "\$DESKTOP_DIR/OpenClaw.desktop"
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

systemctl mask systemd-binfmt.service 2>/dev/null || true

USER_HOME=/home/$USER_NAME
install -D -m 644 -o $USER_NAME -g $USER_NAME /tmp/rootfs-configs/xfce4-panel.xml \$USER_HOME/.config/xfce4/xfconf/xfce-perchannel-xml/xfce4-panel.xml
chown -R $USER_NAME:$USER_NAME \$USER_HOME/.config
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
allow-hotplug eth0
iface eth0 inet dhcp
NET

cat >> /etc/dhcpcd.conf << 'DHCPCD'

background
noarp
noipv6rs
ipv4only
DHCPCD

mkdir -p /etc/modprobe.d
cat > /etc/modprobe.d/no-wireless.conf << 'MODPROBE'
blacklist cfg80211
blacklist mac80211
MODPROBE
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
check "chromium"          dpkg -s chromium
check "spice-vdagent"     dpkg -s spice-vdagent
check "qemu-guest-agent"  dpkg -s qemu-guest-agent
check "pulseaudio"        dpkg -s pulseaudio
check "node"              command -v node
check "ntp"               dpkg -s systemd-timesyncd
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
    if [ -z "$OPENCLAW_VERSION" ]; then
        OPENCLAW_VERSION=$(sudo chroot "$MOUNT_DIR" runuser -l "$USER_NAME" -c \
            'PATH="$HOME/.npm-global/bin:$PATH" npm ls -g openclaw 2>/dev/null' \
            | grep 'openclaw@' | sed 's/.*openclaw@//' || true)
        [ -n "$OPENCLAW_VERSION" ] && echo "  Detected OpenClaw version: $OPENCLAW_VERSION"
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
    
    # Unmount proc/sys/dev
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
        if [ -n "$OPENCLAW_VERSION" ]; then
            OUTPUT="$OUTPUT_DIR/rootfs-openclaw-${OPENCLAW_VERSION}-x86_64.qcow2"
        else
            OUTPUT="$OUTPUT_DIR/rootfs-openclaw-x86_64.qcow2"
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
run_step "install_openclaw" "Installing OpenClaw"     do_install_openclaw
run_step "config_openclaw" "Configuring OpenClaw"     do_config_openclaw
run_step "copy_readme"    "Copying wiki desktop link"  do_copy_readme
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
