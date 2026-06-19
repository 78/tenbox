#!/usr/bin/env bash
set -euo pipefail

# Patch Radxa/Allwinner A733 Cubie DTBs so arm64 KVM can expose an in-kernel
# VGIC device to userspace. This is a temporary host-side workaround until the
# kernel/DTS package contains the maintenance interrupt in source form.

INTERRUPTS_PROP='interrupts = <0x01 0x09 0x04>;'
GIC_NODE='interrupt-controller@3400000'
WORKDIR="${TMPDIR:-/tmp}/tenbox-radxa-a733-vgic-fix"

log() {
    printf '%s\n' "$*"
}

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
Usage:
  fix-radxa-a733-vgic.sh [--dtb /path/to/file.dtb] [--no-install-deps]

Patches the installed Radxa/Allwinner A733 Cubie DTB for the current kernel by
adding the arm64 KVM VGIC maintenance interrupt to interrupt-controller@3400000.

Environment:
  DTB=/path/to/file.dtb      Override target DTB path.

After a successful patch, reboot the host and rerun this script with --check or
run AgentSphere's KVM probe/doctor.
EOF
}

install_deps=1
check_only=0
dtb_override="${DTB:-}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --dtb)
            [ "$#" -ge 2 ] || die "--dtb requires a path"
            dtb_override="$2"
            shift 2
            ;;
        --dtb=*)
            dtb_override="${1#--dtb=}"
            shift
            ;;
        --no-install-deps)
            install_deps=0
            shift
            ;;
        --check)
            check_only=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown argument: $1"
            ;;
    esac
done

if [ "$(id -u)" -eq 0 ]; then
    SUDO=''
else
    command -v sudo >/dev/null 2>&1 || die "sudo is required when not running as root"
    SUDO='sudo'
fi

require_cmd() {
    if command -v "$1" >/dev/null 2>&1; then
        return 0
    fi
    if [ "$1" = dtc ] && [ "$install_deps" -eq 1 ] && command -v apt-get >/dev/null 2>&1; then
        log "dtc not found; installing device-tree-compiler with apt-get..."
        $SUDO apt-get update
        $SUDO apt-get install -y device-tree-compiler
        command -v dtc >/dev/null 2>&1 && return 0
    fi
    die "$1 is required"
}

read_dt_string() {
    local file="$1"
    [ -f "$file" ] || return 0
    tr '\000' ' ' <"$file" 2>/dev/null || true
}

detect_variant() {
    local text="$1"
    case "$text" in
        *cubie-a7a*|*Cubie\ A7A*) printf 'a7a' ;;
        *cubie-a7s*|*Cubie\ A7S*) printf 'a7s' ;;
        *cubie-a7z*|*Cubie\ A7Z*) printf 'a7z' ;;
        *) printf '' ;;
    esac
}

find_dtb() {
    if [ -n "$dtb_override" ]; then
        printf '%s\n' "$dtb_override"
        return 0
    fi

    local kernel_dir="/usr/lib/linux-image-$(uname -r)/allwinner"
    [ -d "$kernel_dir" ] || die "kernel DTB directory not found: $kernel_dir"

    local model compatible variant candidate count last
    model="$(read_dt_string /proc/device-tree/model)"
    compatible="$(read_dt_string /proc/device-tree/compatible)"
    variant="$(detect_variant "$model $compatible")"
    if [ -n "$variant" ]; then
        candidate="$kernel_dir/sun60i-a733-cubie-$variant.dtb"
        [ -f "$candidate" ] && {
            printf '%s\n' "$candidate"
            return 0
        }
    fi

    count=0
    last=''
    for candidate in "$kernel_dir"/sun60i-a733-cubie-a7*.dtb; do
        [ -f "$candidate" ] || continue
        count=$((count + 1))
        last="$candidate"
    done

    if [ "$count" -eq 1 ]; then
        printf '%s\n' "$last"
        return 0
    fi

    die "could not choose DTB automatically; pass --dtb /path/to/sun60i-a733-cubie-*.dtb"
}

node_has_interrupts() {
    local dts="$1"
    awk -v node="$GIC_NODE" '
        $0 ~ node "[[:space:]]*\\{" { show=1; depth=0 }
        show {
            if ($0 ~ /^[[:space:]]*interrupts[[:space:]]*=/) found=1
            depth += gsub(/\{/, "{")
            depth -= gsub(/\}/, "}")
            if (show && depth == 0) exit(found ? 0 : 1)
        }
        END {
            if (!show) exit 2
        }
    ' "$dts"
}

patch_dts() {
    local src="$1"
    local dst="$2"
    awk -v node="$GIC_NODE" -v prop="$INTERRUPTS_PROP" '
        $0 ~ node "[[:space:]]*\\{" {
            in_node=1
            depth=0
        }
        {
            print
            if (in_node && $0 ~ /^[[:space:]]*compatible[[:space:]]*= "arm,gic-v3";/) {
                match($0, /^[[:space:]]*/)
                indent=substr($0, RSTART, RLENGTH)
                print indent prop
                inserted=1
            }
            if (in_node) {
                depth += gsub(/\{/, "{")
                depth -= gsub(/\}/, "}")
                if (depth == 0) in_node=0
            }
        }
        END {
            if (!inserted) exit 1
        }
    ' "$src" >"$dst"
}

print_live_gic() {
    local node=''
    for candidate in "/proc/device-tree/$GIC_NODE" "/sys/firmware/devicetree/base/$GIC_NODE"; do
        [ -d "$candidate" ] && node="$candidate" && break
    done
    [ -n "$node" ] || {
        log "live GIC node: not found"
        return 0
    }

    log "live GIC node: $node"
    for f in compatible interrupts reg interrupt-parent; do
        if [ ! -f "$node/$f" ]; then
            log "  $f: <missing>"
            continue
        fi
        printf '  %s: ' "$f"
        if [ "$f" = compatible ]; then
            tr '\000' ' ' <"$node/$f"
        else
            od -An -tx4 -v "$node/$f"
        fi
        printf '\n'
    done
}

[ "$(uname -m)" = aarch64 ] || die "this fix is only for aarch64 hosts"
require_cmd awk
require_cmd od
require_cmd dtc

log "Radxa A733 VGIC DTB fix"
log "kernel: $(uname -r)"
print_live_gic

DTB_PATH="$(find_dtb)"
[ -f "$DTB_PATH" ] || die "DTB not found: $DTB_PATH"
case "$DTB_PATH" in
    *sun60i-a733-cubie-a7*.dtb) ;;
    *) die "refusing to patch non-Cubie A733 DTB: $DTB_PATH" ;;
esac

log "target DTB: $DTB_PATH"
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"

dtc -I dtb -O dts -o "$WORKDIR/orig.dts" "$DTB_PATH" 2>"$WORKDIR/decompile-warnings.log" ||
    { cat "$WORKDIR/decompile-warnings.log" >&2; die "failed to decompile DTB"; }

if node_has_interrupts "$WORKDIR/orig.dts"; then
    log "target DTB already has an interrupts property in $GIC_NODE"
    if [ "$check_only" -eq 1 ]; then
        exit 0
    fi
    log "No DTB changes made. If live device tree still shows <missing>, reboot into this DTB."
    exit 0
fi

if [ "$check_only" -eq 1 ]; then
    log "target DTB is missing the VGIC maintenance interrupt"
    exit 1
fi

patch_dts "$WORKDIR/orig.dts" "$WORKDIR/patched.dts" ||
    die "failed to insert $INTERRUPTS_PROP into $GIC_NODE"

if ! node_has_interrupts "$WORKDIR/patched.dts"; then
    die "patched DTS does not contain the expected interrupts property"
fi

dtc -I dts -O dtb -o "$WORKDIR/patched.dtb" "$WORKDIR/patched.dts" 2>"$WORKDIR/compile-warnings.log" ||
    { cat "$WORKDIR/compile-warnings.log" >&2; die "failed to compile patched DTB"; }

backup="$DTB_PATH.bak-tenbox-vgic-$(date +%Y%m%d%H%M%S)"
log "backup: $backup"
$SUDO cp -a "$DTB_PATH" "$backup"
$SUDO cp "$WORKDIR/patched.dtb" "$DTB_PATH"
$SUDO chmod 0644 "$DTB_PATH"

dtc -I dtb -O dts -o "$WORKDIR/verify.dts" "$DTB_PATH" 2>"$WORKDIR/verify-warnings.log" ||
    { cat "$WORKDIR/verify-warnings.log" >&2; die "failed to verify installed DTB"; }
node_has_interrupts "$WORKDIR/verify.dts" ||
    die "installed DTB verification failed"

log "patched successfully."
log "Reboot is required before /proc/device-tree and KVM VGIC availability change."
log "Rollback: sudo cp '$backup' '$DTB_PATH' && sudo reboot"
