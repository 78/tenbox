#!/bin/bash
# Build a Debian arm64 rootfs with Hermes Agent as qcow2 for TenBox macOS.
#
# Requires: debootstrap, qemu-utils.
# Run as root on an arm64 host (or in a container).
#
# Features:
#   - Checkpoint system: resume from last successful step after failure
#   - APT cache: reuse downloaded packages across runs
#   - Docker layer cache: hermes pip/uv cache preserved across builds
#
# Usage:
#   ./make-rootfs-hermes.sh [output.qcow2]
#   ./make-rootfs-hermes.sh --force [output.qcow2]
#   ./make-rootfs-hermes.sh --from-step N
#   ./make-rootfs-hermes.sh --list-steps
#   ./make-rootfs-hermes.sh --status

set -e

ROOTFS_SIZE="100G"
SUITE="${TENBOX_DEBIAN_SUITE:-trixie}"
MIRROR="${TENBOX_DEBIAN_MIRROR:-http://deb.debian.org/debian}"
MIRROR_SECURITY="${TENBOX_DEBIAN_SECURITY_MIRROR:-http://deb.debian.org/debian-security}"
ARCH="arm64"
ROOT_PASSWORD="${ROOT_PASSWORD:-tenbox}"
USER_NAME="${USER_NAME:-tenbox}"
USER_PASSWORD="${USER_PASSWORD:-tenbox}"
HERMES_VERSION="${HERMES_VERSION-}"
HERMES_INSTALL_BROWSER_TOOLS="${HERMES_INSTALL_BROWSER_TOOLS:-0}"
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
linux-image-arm64,iptables,util-linux,util-linux-extra"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$(mkdir -p "$SCRIPT_DIR/../../build" && cd "$SCRIPT_DIR/../../build" && pwd)"
CACHE_DIR="$BUILD_DIR/.rootfs-cache"
mkdir -p "$CACHE_DIR"

# Parse arguments
FORCE_REBUILD=false
FROM_STEP=0
LIST_STEPS=false
SHOW_STATUS=false
SHOW_HELP=false
OUTPUT_ARG=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --help|-h) SHOW_HELP=true; shift ;;
        --force) FORCE_REBUILD=true; shift ;;
        --from-step) FROM_STEP="$2"; shift 2 ;;
        --list-steps) LIST_STEPS=true; shift ;;
        --status) SHOW_STATUS=true; shift ;;
        *) OUTPUT_ARG="$1"; shift ;;
    esac
done

LATEST_HINT_KEY="$(printf '%s\n%s\n%s\n%s\n%s\n%s\n%s\n' \
    "$SUITE" "$MIRROR" "$MIRROR_SECURITY" "$ROOT_PASSWORD" "$USER_NAME" "$USER_PASSWORD" \
    "$HERMES_INSTALL_BROWSER_TOOLS" \
    | cksum | awk '{print $1}')"
LATEST_VERSION_HINT="$CACHE_DIR/hermes-latest-arm64-${LATEST_HINT_KEY}.txt"

resolve_latest_hermes_version() {
    local api_url latest_url resolved_version

    api_url="https://api.github.com/repos/NousResearch/hermes-agent/releases/latest"
    resolved_version="$(
        curl -fsSL "$api_url" \
        | sed -n 's/.*"tag_name":[[:space:]]*"\([^"]*\)".*/\1/p' \
        | head -n 1
    )"
    if [ -n "$resolved_version" ]; then
        printf '%s\n' "$resolved_version"
        return 0
    fi

    latest_url="$(
        curl -fsSLI -o /dev/null -w '%{url_effective}' \
        "https://github.com/NousResearch/hermes-agent/releases/latest"
    )"
    resolved_version="${latest_url##*/}"
    if [[ "$resolved_version" == v* ]]; then
        printf '%s\n' "$resolved_version"
        return 0
    fi

    echo "Failed to resolve latest Hermes Agent release version" >&2
    return 1
}

load_cached_latest_hermes_version() {
    if [ -f "$LATEST_VERSION_HINT" ]; then
        local hinted_version
        hinted_version="$(head -n 1 "$LATEST_VERSION_HINT" 2>/dev/null || true)"
        if [[ "$hinted_version" == v* ]]; then
            printf '%s\n' "$hinted_version"
            return 0
        fi
    fi

    return 1
}

resolve_effective_hermes_version() {
    if [ -n "$HERMES_VERSION" ]; then
        return 0
    fi

    if [ "$SHOW_STATUS" = true ] || [ "$LIST_STEPS" = true ] || [ "$SHOW_HELP" = true ]; then
        local cached_version
        cached_version="$(load_cached_latest_hermes_version || true)"
        if [ -n "$cached_version" ]; then
            HERMES_VERSION="$cached_version"
            return 0
        fi
        HERMES_VERSION="latest"
        return 0
    fi

    # 真正执行构建时始终联网解析官方最新 release，避免被旧缓存误导。
    HERMES_VERSION="$(resolve_latest_hermes_version)"
    printf '%s\n' "$HERMES_VERSION" > "$LATEST_VERSION_HINT"
}

resolve_effective_hermes_version

STATE_KEY="$(printf '%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n' \
    "$SUITE" "$MIRROR" "$MIRROR_SECURITY" "$ROOT_PASSWORD" "$USER_NAME" "$USER_PASSWORD" \
    "$HERMES_VERSION" "$HERMES_INSTALL_BROWSER_TOOLS" \
    | cksum | awk '{print $1}')"
STATE_SUFFIX="${SUITE}-${STATE_KEY}"

CHECKPOINT_DIR="$CACHE_DIR/checkpoints-hermes-arm64-$STATE_SUFFIX"
APT_CACHE_DIR="$CACHE_DIR/apt-archives-arm64-$STATE_SUFFIX"
SCRIPTS_CACHE_DIR="$CACHE_DIR/scripts"
PIP_CACHE_DIR="$CACHE_DIR/pip-arm64"
mkdir -p "$CHECKPOINT_DIR" "$APT_CACHE_DIR" "$SCRIPTS_CACHE_DIR" "$PIP_CACHE_DIR"

CACHE_TAR="$(realpath -m "$CACHE_DIR/debootstrap-${SUITE}-arm64-${STATE_KEY}.tar")"
CACHE_UV="$SCRIPTS_CACHE_DIR/uv_install.sh"
CACHE_NODESOURCE="$SCRIPTS_CACHE_DIR/nodesource_setup_22.x.sh"
CACHE_HERMES_WHEELS="$CACHE_DIR/hermes-wheels-arm64"
CACHE_HERMES_SRC="$CACHE_DIR/hermes-agent-${HERMES_VERSION}.tar.gz"

WORK_ROOT="${TENBOX_WORK_DIR:-/tmp/tenbox-rootfs-hermes-arm64}"
WORK_DIR="${WORK_ROOT}-${STATE_SUFFIX}"

show_help() {
    cat << 'HELP'
Usage: ./make-rootfs-hermes.sh [OPTIONS] [output.qcow2]

Build a Debian arm64 rootfs image with Hermes Agent for TenBox macOS.

Options:
  --help          Show this help message
  --status        Show current build progress
  --list-steps    Show all build steps with numbers
  --force         Force rebuild from scratch
  --from-step N   Resume from step N

Requires: debootstrap, qemu-utils
HELP
    exit 0
}

[ "$SHOW_HELP" = true ] && show_help

if [ -n "$OUTPUT_ARG" ]; then
    OUTPUT="$(realpath -m "$OUTPUT_ARG")"
else
    OUTPUT=""
    OUTPUT_DIR="$(realpath -m "$BUILD_DIR/share")"
fi

DETECTED_HERMES_VERSION=""

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
    "install_python_deps"
    "install_uv"
    "install_hermes"
    "config_hermes"
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
    "Bootstrap Debian arm64"
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
    "Install Python system dependencies"
    "Install uv package manager"
    "Install Hermes Agent"
    "Configure Hermes Agent"
    "Copy desktop links (Help + Hermes)"
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

if $LIST_STEPS; then
    echo "Available steps:"
    for i in "${!STEPS[@]}"; do
        printf "  %2d. %-20s - %s\n" "$i" "${STEPS[$i]}" "${STEP_DESCRIPTIONS[$i]}"
    done
    exit 0
fi

if $SHOW_STATUS; then
    echo "Build progress:"
    for i in "${!STEPS[@]}"; do
        if [ -f "$CHECKPOINT_DIR/${STEPS[$i]}.done" ]; then
            status="done"
        else
            status="  pending"
        fi
        printf "  %2d. %-20s %s\n" "$i" "${STEPS[$i]}" "$status"
    done
    exit 0
fi

if $FORCE_REBUILD; then
    echo "Force rebuild: clearing all checkpoints and work directory..."
    rm -f "$CHECKPOINT_DIR"/*.done
    rm -rf "$WORK_DIR"
fi

if [ "$FROM_STEP" -gt 0 ]; then
    echo "Resuming from step $FROM_STEP..."
    for i in "${!STEPS[@]}"; do
        if [ "$i" -ge "$FROM_STEP" ]; then
            rm -f "$CHECKPOINT_DIR/${STEPS[$i]}.done"
        fi
    done
fi

step_done() { [ -f "$CHECKPOINT_DIR/$1.done" ]; }
mark_done() { touch "$CHECKPOINT_DIR/$1.done"; }

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
    if [ "${BUILD_SUCCESS:-false}" = "true" ]; then
        rm -rf "$WORK_DIR"
    else
        echo "Work directory preserved for resume: $WORK_DIR"
    fi
}
trap cleanup EXIT

if [ -d "$WORK_DIR" ] && [ -f "$WORK_DIR/rootfs.raw" ]; then
    echo "Resuming with existing work directory: $WORK_DIR"
    rm -f "$CHECKPOINT_DIR/mount_image.done"
    rm -f "$CHECKPOINT_DIR/setup_chroot.done"
    rm -f "$CHECKPOINT_DIR/cleanup_chroot.done"
    rm -f "$CHECKPOINT_DIR/unmount_image.done"
elif find "$CHECKPOINT_DIR" -maxdepth 1 -name '*.done' | grep -q .; then
    echo "Checkpoint exists but work directory is missing, clearing stale checkpoints..."
    rm -f "$CHECKPOINT_DIR"/*.done
fi

MOUNT_DIR="$WORK_DIR/mnt"
mkdir -p "$MOUNT_DIR"

total_steps=${#STEPS[@]}
current_step=0
DEBOOTSTRAP_RETRIES="${TENBOX_DEBOOTSTRAP_RETRIES:-3}"

run_step() {
    local step_name="$1"
    local step_desc="$2"
    shift 2
    current_step=$((current_step + 1))
    if step_done "$step_name"; then
        echo "[$current_step/$total_steps] $step_desc... (skipped)"
        return 0
    fi
    echo "[$current_step/$total_steps] $step_desc..."
    "$@"
    mark_done "$step_name"
}

retry_command() {
    local desc="$1"
    shift
    local attempt=1

    while true; do
        if "$@"; then
            return 0
        fi

        if [ "$attempt" -ge "$DEBOOTSTRAP_RETRIES" ]; then
            echo "  $desc failed after $attempt attempts"
            return 1
        fi

        echo "  $desc failed (attempt $attempt/$DEBOOTSTRAP_RETRIES), retrying..."
        attempt=$((attempt + 1))
        sleep 2
    done
}

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
    if [ -f "$MOUNT_DIR/etc/debian_version" ]; then
        echo "  Debian already bootstrapped"
        return 0
    fi

    make_tarball() {
        sudo rm -f "$CACHE_TAR"
        sudo rm -rf "$WORK_DIR/tarball-tmp"
        sudo debootstrap --arch=$ARCH --include="$INCLUDE_PKGS" \
            --cache-dir="$APT_CACHE_DIR" \
            --make-tarball="$CACHE_TAR" "$SUITE" "$WORK_DIR/tarball-tmp" "$MIRROR"
    }

    unpack_tarball() {
        sudo rm -rf "${MOUNT_DIR:?}"/*
        sudo debootstrap --arch=$ARCH --include="$INCLUDE_PKGS" \
            --unpack-tarball="$CACHE_TAR" "$SUITE" "$MOUNT_DIR" "$MIRROR"
    }

    direct_bootstrap() {
        sudo rm -rf "${MOUNT_DIR:?}"/*
        sudo debootstrap --arch=$ARCH --include="$INCLUDE_PKGS" \
            --cache-dir="$APT_CACHE_DIR" \
            "$SUITE" "$MOUNT_DIR" "$MIRROR"
    }

    if [ -f "$CACHE_TAR" ]; then
        echo "  Using cached tarball: $CACHE_TAR"
        if ! retry_command "Unpack cached Debian tarball" unpack_tarball; then
            echo "  Cache tarball failed, removing stale cache and cleaning mount dir..."
            rm -f "$CACHE_TAR"
            retry_command "Bootstrap Debian rootfs" direct_bootstrap
        fi
    else
        echo "  No cache found, downloading packages (first run)..."
        retry_command "Generate Debian cache tarball" make_tarball
        rm -rf "$WORK_DIR/tarball-tmp"
        retry_command "Unpack Debian cache tarball" unpack_tarball
    fi
}

do_setup_chroot() {
    sudo cp /etc/resolv.conf "$MOUNT_DIR/etc/resolv.conf"
    mountpoint -q "$MOUNT_DIR/proc" 2>/dev/null || sudo mount --bind /proc "$MOUNT_DIR/proc"
    mountpoint -q "$MOUNT_DIR/sys" 2>/dev/null || sudo mount --bind /sys "$MOUNT_DIR/sys"
    mountpoint -q "$MOUNT_DIR/dev" 2>/dev/null || sudo mount --bind /dev "$MOUNT_DIR/dev"
    sudo mkdir -p "$MOUNT_DIR/dev/pts"
    mountpoint -q "$MOUNT_DIR/dev/pts" 2>/dev/null || sudo mount -t devpts devpts "$MOUNT_DIR/dev/pts"

    sudo tee "$MOUNT_DIR/usr/sbin/policy-rc.d" > /dev/null << 'PRC'
#!/bin/sh
exit 101
PRC
    sudo chmod +x "$MOUNT_DIR/usr/sbin/policy-rc.d"

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

    sudo cp -r "$SCRIPT_DIR/../rootfs-scripts" "$MOUNT_DIR/tmp/"
    sudo cp -r "$SCRIPT_DIR/../rootfs-services" "$MOUNT_DIR/tmp/"
    sudo cp -r "$SCRIPT_DIR/../rootfs-configs" "$MOUNT_DIR/tmp/"

    # Bind-mount pip cache for Hermes wheel caching
    sudo mkdir -p "$MOUNT_DIR/var/cache/pip"
    if ! mountpoint -q "$MOUNT_DIR/var/cache/pip" 2>/dev/null; then
        sudo mount --bind "$PIP_CACHE_DIR" "$MOUNT_DIR/var/cache/pip"
    fi
}

do_config_basic() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << EOF
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
cat > /etc/apt/apt.conf.d/99tenbox-network << 'APTCONF'
Acquire::Retries "8";
Acquire::http::Timeout "30";
Acquire::https::Timeout "30";
Acquire::ForceIPv4 "true";
Acquire::http::Pipeline-Depth "0";
APT::Get::Fix-Missing "true";
APTCONF
apt-get update
update-ca-certificates --fresh 2>/dev/null || true
EOF
}

do_install_xfce() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
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

do_install_devtools() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if dpkg -s python3 &>/dev/null && dpkg -s g++ &>/dev/null && dpkg -s cmake &>/dev/null; then
    echo "  Dev tools already installed"
    exit 0
fi
DEBIAN_FRONTEND=noninteractive apt-get install -y \
    python3 python3-pip python3-venv \
    g++ make cmake git
EOF
}

do_install_audio() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if dpkg -s pulseaudio &>/dev/null && dpkg -s pavucontrol &>/dev/null; then
    echo "  Audio packages already installed"
    exit 0
fi
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

do_install_usertools() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << EOF
if dpkg -s jq &>/dev/null; then
    echo "  User tools already installed"
    exit 0
fi
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    jq

if [ "$HERMES_INSTALL_BROWSER_TOOLS" = "1" ]; then
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        chromium ffmpeg
    if ! grep -q 'PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH' /home/$USER_NAME/.bashrc 2>/dev/null; then
        echo "export PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH=/usr/bin/chromium" >> /home/$USER_NAME/.bashrc
    fi
fi
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

do_install_python_deps() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if dpkg -s python3-dev &>/dev/null && dpkg -s libffi-dev &>/dev/null; then
    echo "  Python deps already installed"
    exit 0
fi
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    python3-dev \
    python3-venv \
    libffi-dev \
    libssl-dev \
    libsqlite3-dev \
    zlib1g-dev \
    libbz2-dev \
    libreadline-dev \
    libncurses5-dev \
    libncursesw5-dev \
    liblzma-dev \
    uuid-dev \
    ripgrep
EOF
}

do_install_uv() {
    if [ ! -f "$CACHE_UV" ] || [ ! -s "$CACHE_UV" ]; then
        echo "  Downloading uv install script..."
        rm -f "$CACHE_UV" "$CACHE_UV.tmp"
        curl -fsSL https://astral.sh/uv/install.sh -o "$CACHE_UV.tmp"
        mv "$CACHE_UV.tmp" "$CACHE_UV"
    fi
    sudo cp "$CACHE_UV" "$MOUNT_DIR/tmp/uv_install.sh"

    sudo chroot "$MOUNT_DIR" /bin/bash -e << EOF
if command -v uv &>/dev/null; then
    echo "  uv already installed"
    exit 0
fi
echo "Installing uv..."
bash /tmp/uv_install.sh
rm -f /tmp/uv_install.sh

if [ -x /root/.local/bin/uv ]; then
    ln -sf /root/.local/bin/uv /usr/local/bin/uv
fi
if [ -x /root/.local/bin/uvx ]; then
    ln -sf /root/.local/bin/uvx /usr/local/bin/uvx
fi

# Ensure uv is on PATH for all users
if ! grep -q '/.local/bin' /home/$USER_NAME/.bashrc 2>/dev/null; then
    cat >> /home/$USER_NAME/.bashrc << 'UV_PATH'
export PATH="$HOME/.local/bin:$PATH"
UV_PATH
fi
EOF
}

do_install_hermes() {
    if [ ! -f "$CACHE_HERMES_SRC" ] || [ ! -s "$CACHE_HERMES_SRC" ]; then
        echo "  Downloading Hermes Agent source tarball..."
        rm -f "$CACHE_HERMES_SRC" "$CACHE_HERMES_SRC.tmp"
        curl -fsSL "https://codeload.github.com/NousResearch/hermes-agent/tar.gz/refs/tags/${HERMES_VERSION}" -o "$CACHE_HERMES_SRC.tmp"
        mv "$CACHE_HERMES_SRC.tmp" "$CACHE_HERMES_SRC"
    fi
    sudo cp "$CACHE_HERMES_SRC" "$MOUNT_DIR/tmp/hermes-agent.tar.gz"

    sudo chroot "$MOUNT_DIR" /bin/bash -e << EOF
if command -v hermes >/dev/null 2>&1; then
    echo "  Hermes Agent already installed"
    exit 0
fi
echo "Installing Hermes Agent ${HERMES_VERSION}..."

USER_HOME=/home/$USER_NAME
HERMES_HOME=\$USER_HOME/.hermes
INSTALL_DIR=\$HERMES_HOME/hermes-agent
TMP_DIR=\$(mktemp -d)

export HOME=\$USER_HOME
export PIP_CACHE_DIR=/var/cache/pip
export UV_CACHE_DIR=/var/cache/pip/uv
mkdir -p "\$HERMES_HOME" /var/cache/pip/uv

tar -xzf /tmp/hermes-agent.tar.gz -C "\$TMP_DIR"
EXTRACTED_DIR=\$(find "\$TMP_DIR" -mindepth 1 -maxdepth 1 -type d | head -1)
if [ -z "\$EXTRACTED_DIR" ]; then
    echo "Failed to extract Hermes Agent source"
    exit 1
fi

rm -rf "\$INSTALL_DIR"
mv "\$EXTRACTED_DIR" "\$INSTALL_DIR"
cd "\$INSTALL_DIR"

uv python install 3.11
uv venv venv --python 3.11
VIRTUAL_ENV="\$INSTALL_DIR/venv" uv pip install -e .

ln -sf "\$INSTALL_DIR/venv/bin/hermes" /usr/local/bin/hermes
ln -sf "\$INSTALL_DIR/venv/bin/hermes-agent" /usr/local/bin/hermes-agent

chown -R $USER_NAME:$USER_NAME "\$HERMES_HOME"
rm -rf "\$TMP_DIR" /tmp/hermes-agent.tar.gz
EOF

    DETECTED_HERMES_VERSION=$(sudo chroot "$MOUNT_DIR" runuser -l "$USER_NAME" -c \
        'PATH="/usr/local/bin:$HOME/.local/bin:$PATH" hermes --version 2>/dev/null || hermes-agent --version 2>/dev/null' \
        | grep -oE '[0-9]+(\.[0-9]+)+' | head -n1 || true)
    if [ -n "$DETECTED_HERMES_VERSION" ]; then
        echo "  Hermes Agent version: $DETECTED_HERMES_VERSION"
    else
        echo "  WARNING: Could not detect Hermes Agent version"
    fi
}

do_config_hermes() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << EOF
USER_HOME=/home/$USER_NAME
HERMES_DIR=\$USER_HOME/.hermes
UNIT_DIR=\$USER_HOME/.config/systemd/user

if [ -f "\$UNIT_DIR/hermes-agent.service" ]; then
    echo "  Hermes systemd service already installed"
    exit 0
fi

echo "Configuring Hermes Agent..."

# Create .env template if not present
if [ ! -f "\$HERMES_DIR/.env" ]; then
    mkdir -p "\$HERMES_DIR"
    cat > "\$HERMES_DIR/.env" << 'ENVEOF'
# Hermes Agent Configuration
# Uncomment and set your API keys:

# OpenAI
# OPENAI_API_KEY=sk-...

# Anthropic
# ANTHROPIC_API_KEY=sk-ant-...

# TenBox LLM proxy provider (guestfwd: 10.0.2.3:80 -> host proxy)
OPENAI_BASE_URL=http://10.0.2.3/v1
OPENAI_API_KEY=tenbox
ENVEOF
    chown -R $USER_NAME:$USER_NAME "\$HERMES_DIR"
fi

cat > "\$HERMES_DIR/config.yaml" << 'CFGEOF'
model:
  default: "default"
  provider: "custom"
  base_url: "http://10.0.2.3/v1"
CFGEOF
chown $USER_NAME:$USER_NAME "\$HERMES_DIR/config.yaml"

# Install systemd user service for Hermes Agent
mkdir -p "\$UNIT_DIR"

cat > "\$UNIT_DIR/hermes-agent.service" << UNIT
[Unit]
Description=Hermes Agent
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/local/bin/hermes-agent
Restart=on-failure
RestartSec=10
KillMode=process
WorkingDirectory=\$USER_HOME
Environment=HOME=\$USER_HOME
Environment=PATH=\$USER_HOME/.local/bin:\$USER_HOME/bin:/usr/local/bin:/usr/bin:/bin
Environment=HERMES_HOME=\$HERMES_DIR
EnvironmentFile=-\$HERMES_DIR/.env

[Install]
WantedBy=default.target
UNIT

mkdir -p "\$UNIT_DIR/hermes-agent.service.d"
cat > "\$UNIT_DIR/hermes-agent.service.d/override.conf" << OVERRIDE
[Service]
Environment=DISPLAY=:0
OVERRIDE

mkdir -p "\$UNIT_DIR/default.target.wants"
ln -sf ../hermes-agent.service "\$UNIT_DIR/default.target.wants/hermes-agent.service"

chown -R $USER_NAME:$USER_NAME \$USER_HOME/.config

mkdir -p /var/lib/systemd/linger
touch /var/lib/systemd/linger/$USER_NAME
EOF
}

do_copy_readme() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << EOF
DESKTOP_DIR="/home/$USER_NAME/Desktop"
if [ -f "\$DESKTOP_DIR/Help.desktop" ] && [ -f "\$DESKTOP_DIR/Hermes.desktop" ]; then
    echo "  Desktop links already copied"
    exit 0
fi

mkdir -p "\$DESKTOP_DIR"
chown $USER_NAME:$USER_NAME "\$DESKTOP_DIR"

cp /tmp/rootfs-configs/Help.desktop "\$DESKTOP_DIR/Help.desktop"
chown $USER_NAME:$USER_NAME "\$DESKTOP_DIR/Help.desktop"
chmod +x "\$DESKTOP_DIR/Help.desktop"

if [ -f /tmp/rootfs-configs/Hermes.desktop ]; then
    cp /tmp/rootfs-configs/Hermes.desktop "\$DESKTOP_DIR/Hermes.desktop"
    chown $USER_NAME:$USER_NAME "\$DESKTOP_DIR/Hermes.desktop"
    chmod +x "\$DESKTOP_DIR/Hermes.desktop"
fi
EOF
}

do_config_locale() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if locale -a 2>/dev/null | grep -q "zh_CN.utf8"; then
    echo "  Locale already configured"
    exit 0
fi
sed -i 's/^# *zh_CN.UTF-8/zh_CN.UTF-8/' /etc/locale.gen
sed -i 's/^# *en_US.UTF-8/en_US.UTF-8/' /etc/locale.gen
locale-gen
update-locale LANG=zh_CN.UTF-8
ln -sf /usr/share/zoneinfo/Asia/Shanghai /etc/localtime
echo "Asia/Shanghai" > /etc/timezone
EOF
}

do_config_services() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << EOF
if [ -f /etc/systemd/system/serial-getty@ttyAMA0.service.d/autologin.conf ]; then
    echo "  Services already configured"
    exit 0
fi

mkdir -p /etc/polkit-1/rules.d
cp /tmp/rootfs-services/50-user-power.rules /etc/polkit-1/rules.d/

mkdir -p /etc/systemd/system/serial-getty@ttyAMA0.service.d
cp /tmp/rootfs-services/serial-getty-autologin.conf /etc/systemd/system/serial-getty@ttyAMA0.service.d/autologin.conf

systemctl enable serial-getty@ttyAMA0.service 2>/dev/null || true

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
cat > /etc/udev/rules.d/99-spice-vdagent.rules << 'UDEV'
SUBSYSTEM=="virtio-ports", ATTR{name}=="com.redhat.spice.0", SYMLINK+="virtio-ports/com.redhat.spice.0"
UDEV
mkdir -p /etc/systemd/system/spice-vdagentd.service.d
cp /tmp/rootfs-services/spice-vdagentd-override.conf /etc/systemd/system/spice-vdagentd.service.d/override.conf
systemctl enable spice-vdagentd.service 2>/dev/null || true
EOF
}

do_config_guest_agent() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
if [ -f /etc/udev/rules.d/99-qemu-guest-agent.rules ]; then
    echo "  Guest agent already configured"
    exit 0
fi
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

do_verify_install() {
    sudo chroot "$MOUNT_DIR" /bin/bash -e << EOF
echo "Verifying arm64 installation..."
FAIL=0
check() {
    local label="\$1"; shift
    if "\$@" &>/dev/null; then
        printf "  OK %s\n" "\$label"
    else
        printf "  FAIL %s\n" "\$label"
        FAIL=1
    fi
}
check "init"              test -x /sbin/init
check "systemd"           dpkg -s systemd
check "xfce4"             dpkg -s xfce4
check "lightdm"           dpkg -s lightdm
if [ "$HERMES_INSTALL_BROWSER_TOOLS" = "1" ]; then
    check "chromium"      dpkg -s chromium
fi
check "spice-vdagent"     dpkg -s spice-vdagent
check "qemu-guest-agent"  dpkg -s qemu-guest-agent
check "pulseaudio"        dpkg -s pulseaudio
check "node"              command -v node
check "python3"           command -v python3
check "uv"                command -v uv
check "hermes"            command -v hermes
check "hermes-agent"      command -v hermes-agent
check "ntp"               dpkg -s systemd-timesyncd
check "curl"              command -v curl
check "arch=arm64"        test "\$(dpkg --print-architecture)" = "arm64"
if [ "\$FAIL" -ne 0 ]; then
    echo "WARNING: some components are missing!"
fi
EOF
}

do_cleanup_chroot() {
    if [ -z "$DETECTED_HERMES_VERSION" ]; then
        DETECTED_HERMES_VERSION=$(sudo chroot "$MOUNT_DIR" runuser -l "$USER_NAME" -c \
            'PATH="/usr/local/bin:$HOME/.local/bin:$PATH" hermes --version 2>/dev/null || hermes-agent --version 2>/dev/null' \
            | grep -oE '[0-9]+(\.[0-9]+)+' | head -n1 || true)
        [ -n "$DETECTED_HERMES_VERSION" ] && echo "  Detected Hermes Agent version: $DETECTED_HERMES_VERSION"
    fi

    sudo chroot "$MOUNT_DIR" /bin/bash -e << 'EOF'
apt-get clean
rm -rf /var/lib/apt/lists/*
rm -rf /var/log/*.log /var/log/apt/* /var/log/dpkg.log
EOF
    sudo rm -f "$MOUNT_DIR/usr/sbin/policy-rc.d"
    sudo rm -rf "$MOUNT_DIR/tmp/rootfs-scripts" "$MOUNT_DIR/tmp/rootfs-services" "$MOUNT_DIR/tmp/rootfs-configs"
    sudo rm -f "$MOUNT_DIR/etc/resolv.conf"
    mountpoint -q "$MOUNT_DIR/var/cache/pip" 2>/dev/null && \
        sudo umount "$MOUNT_DIR/var/cache/pip" || true
    mountpoint -q "$MOUNT_DIR/var/cache/apt/archives" 2>/dev/null && \
        sudo umount "$MOUNT_DIR/var/cache/apt/archives" || true
    sudo umount "$MOUNT_DIR/dev/pts" 2>/dev/null || true
    sudo umount "$MOUNT_DIR/proc" "$MOUNT_DIR/sys" "$MOUNT_DIR/dev" 2>/dev/null || true
}

do_unmount_image() {
    sudo umount "$MOUNT_DIR" 2>/dev/null || true
    MOUNT_DIR=""
}

do_convert_qcow2() {
    if [ -z "$OUTPUT" ]; then
        if [ -n "$DETECTED_HERMES_VERSION" ]; then
            OUTPUT="$OUTPUT_DIR/rootfs-hermes-${DETECTED_HERMES_VERSION}-arm64.qcow2"
        else
            OUTPUT="$OUTPUT_DIR/rootfs-hermes-arm64.qcow2"
        fi
    fi
    mkdir -p "$(dirname "$OUTPUT")"

    echo "Converting to qcow2..."
    qemu-img convert -f raw -O qcow2 -o cluster_size=65536,compression_type=zstd -c "$WORK_DIR/rootfs.raw" "$OUTPUT"
}

# Execute all steps
run_step "create_image"   "Creating raw image"           do_create_image
run_step "mount_image"    "Mounting image"               do_mount_image
run_step "debootstrap"    "Bootstrapping Debian arm64"   do_debootstrap
run_step "setup_chroot"   "Setting up chroot"            do_setup_chroot
run_step "config_basic"   "Basic configuration"          do_config_basic
run_step "apt_update"     "Updating apt sources"         do_apt_update
run_step "install_xfce"   "Installing XFCE desktop"      do_install_xfce
run_step "install_spice"  "Installing SPICE vdagent"     do_install_spice
run_step "install_guest_agent" "Installing Guest Agent"  do_install_guest_agent
run_step "install_devtools" "Installing dev tools"       do_install_devtools
run_step "install_audio"  "Installing audio"             do_install_audio
run_step "install_ibus"   "Installing IBus"              do_install_ibus
run_step "install_usertools" "Installing user tools"     do_install_usertools
run_step "install_nodejs" "Installing Node.js"           do_install_nodejs
run_step "install_python_deps" "Installing Python deps"  do_install_python_deps
run_step "install_uv"     "Installing uv"                do_install_uv
run_step "install_hermes" "Installing Hermes Agent"      do_install_hermes
run_step "config_hermes"  "Configuring Hermes Agent"     do_config_hermes
run_step "copy_readme"    "Copying wiki desktop link"    do_copy_readme
run_step "config_locale"  "Configuring locale"           do_config_locale
run_step "config_services" "Configuring services"        do_config_services
run_step "config_virtio_gpu" "Configuring virtio-gpu"    do_config_virtio_gpu
run_step "config_network" "Configuring network"          do_config_network
run_step "config_virtiofs" "Configuring virtio-fs"       do_config_virtiofs
run_step "config_spice"   "Configuring SPICE"            do_config_spice
run_step "config_guest_agent" "Configuring Guest Agent"  do_config_guest_agent
run_step "verify_install" "Verifying installation"       do_verify_install
run_step "cleanup_chroot" "Cleaning up chroot"           do_cleanup_chroot
run_step "unmount_image"  "Unmounting image"             do_unmount_image
run_step "convert_qcow2"  "Converting to qcow2"          do_convert_qcow2

BUILD_SUCCESS=true
rm -f "$CHECKPOINT_DIR"/*.done

echo ""
echo "============================================"
echo "Done: $OUTPUT ($(ls -lh "$OUTPUT" | awk '{print $5}'))"
echo "============================================"
