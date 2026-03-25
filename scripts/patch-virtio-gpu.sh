#!/bin/bash
# ---------------------------------------------------------------------------
# Patch virtio-gpu.ko to fix the damage-clip regression in Linux 6.4+
# ---------------------------------------------------------------------------
# Kernel 6.4+ introduced ignore_damage_clips in virtio_gpu_plane_atomic_check
# but never resets it to false, causing every subsequent dirtyfb commit to do a
# full-screen transfer.  We download the matching kernel headers (for Kbuild
# infrastructure) and the source .c files from GitHub, apply the fix, recompile
# only drivers/gpu/drm/virtio/, and replace the prebuilt .ko.
#
# This script is meant to be sourced (not executed) by make-initramfs.sh.
# Required variables from the caller:
#   WORKDIR   - temporary working directory
#   KVER      - full Debian kernel version (e.g. "6.12.73+deb13-amd64")
#   MIRROR    - Debian mirror URL
#   DESTDIR   - directory containing prebuilt virtio-gpu.ko to replace
#
# The caller must have a Packages file at "$WORKDIR/Packages".
# On exit, PATCHED_OK is set to true/false.
# ---------------------------------------------------------------------------

# Extract upstream kernel version (e.g. "6.12.73" from "6.12.73+deb13-amd64")
UPSTREAM_KVER=$(echo "$KVER" | grep -oP '^\d+\.\d+\.\d+')

HDRS_PKG="linux-headers-${KVER}"
HDRS_DEB_PATH=$(awk -v pkg="$HDRS_PKG" '$0 == "Package: " pkg, /^$/' "$WORKDIR/Packages" | grep -oP '^Filename: \K.*')

COMMON_HDRS_PKG=$(awk -v pkg="$HDRS_PKG" '$0 == "Package: " pkg, /^$/' "$WORKDIR/Packages" \
    | grep -oP '^Depends:.*\Klinux-headers-[0-9][^ ,]+common')
COMMON_HDRS_DEB_PATH=""
if [ -n "$COMMON_HDRS_PKG" ]; then
    COMMON_HDRS_DEB_PATH=$(awk -v pkg="$COMMON_HDRS_PKG" '$0 == "Package: " pkg, /^$/' "$WORKDIR/Packages" | grep -oP '^Filename: \K.*')
fi

PATCHED_OK=false

if [ -n "$HDRS_DEB_PATH" ]; then
    echo "  Downloading kernel headers: $HDRS_PKG ..."
    curl -fsSL -o "$WORKDIR/headers.deb" "$MIRROR/$HDRS_DEB_PATH"
    mkdir -p "$WORKDIR/hdrs_extract"
    dpkg-deb -x "$WORKDIR/headers.deb" "$WORKDIR/hdrs_extract/"

    if [ -n "$COMMON_HDRS_DEB_PATH" ]; then
        echo "  Downloading common headers: $COMMON_HDRS_PKG ..."
        curl -fsSL -o "$WORKDIR/headers-common.deb" "$MIRROR/$COMMON_HDRS_DEB_PATH"
        dpkg-deb -x "$WORKDIR/headers-common.deb" "$WORKDIR/hdrs_extract/"
    fi

    # Download linux-kbuild package (contains scripts/Kbuild.include etc.)
    # Strip the trailing arch suffix (e.g. "-amd64", "-arm64") to get the
    # kbuild package version like "6.12.73+deb13".
    KBUILD_KVER=$(echo "$KVER" | sed 's/-[^-]*$//')
    KBUILD_PKG="linux-kbuild-${KBUILD_KVER}"
    KBUILD_DEB_PATH=$(awk -v pkg="$KBUILD_PKG" '$0 == "Package: " pkg, /^$/' "$WORKDIR/Packages" | grep -oP '^Filename: \K.*')
    if [ -n "$KBUILD_DEB_PATH" ]; then
        echo "  Downloading kbuild scripts: $KBUILD_PKG ..."
        curl -fsSL -o "$WORKDIR/kbuild.deb" "$MIRROR/$KBUILD_DEB_PATH"
        dpkg-deb -x "$WORKDIR/kbuild.deb" "$WORKDIR/hdrs_extract/"
    else
        echo "  WARNING: $KBUILD_PKG not found, module build may fail"
    fi

    # Locate the arch-specific headers directory (has .config, Module.symvers)
    ARCH_HDRS_DIR="$(find "$WORKDIR/hdrs_extract/usr/src" -maxdepth 1 -type d -name "linux-headers-${KVER}" | head -1)"
    # Locate the common headers directory (has source tree skeleton)
    COMMON_HDRS_DIR="$(find "$WORKDIR/hdrs_extract/usr/src" -maxdepth 1 -type d -name "linux-headers-*common*" | head -1)"

    if [ -n "$ARCH_HDRS_DIR" ]; then
        # Fix the "source" symlink: arch headers point source -> /usr/src/...
        # which doesn't exist on the build host; repoint to actual extracted path
        if [ -L "$ARCH_HDRS_DIR/source" ]; then
            rm "$ARCH_HDRS_DIR/source"
        fi
        if [ -n "$COMMON_HDRS_DIR" ]; then
            ln -s "$(realpath "$COMMON_HDRS_DIR")" "$ARCH_HDRS_DIR/source"

            # Fix absolute path in arch Makefile that includes common Makefile
            if [ -f "$ARCH_HDRS_DIR/Makefile" ]; then
                COMMON_BASENAME="$(basename "$COMMON_HDRS_DIR")"
                sed -i "s|/usr/src/${COMMON_BASENAME}|$(realpath "$COMMON_HDRS_DIR")|g" "$ARCH_HDRS_DIR/Makefile"
            fi

            # Fix the "scripts" symlink in common headers -> kbuild scripts
            KBUILD_SCRIPTS_DIR="$(find "$WORKDIR/hdrs_extract/usr/lib" -maxdepth 2 -type d -name "scripts" 2>/dev/null | head -1)"
            if [ -n "$KBUILD_SCRIPTS_DIR" ] && [ -d "$KBUILD_SCRIPTS_DIR" ]; then
                if [ -L "$COMMON_HDRS_DIR/scripts" ]; then
                    rm "$COMMON_HDRS_DIR/scripts"
                fi
                ln -s "$(realpath "$KBUILD_SCRIPTS_DIR")" "$COMMON_HDRS_DIR/scripts"
            fi

            # Also fix "tools" symlink if present
            KBUILD_TOOLS_DIR="$(find "$WORKDIR/hdrs_extract/usr/lib" -maxdepth 2 -type d -name "tools" 2>/dev/null | head -1)"
            if [ -n "$KBUILD_TOOLS_DIR" ] && [ -d "$KBUILD_TOOLS_DIR" ]; then
                if [ -L "$COMMON_HDRS_DIR/tools" ]; then
                    rm "$COMMON_HDRS_DIR/tools"
                fi
                ln -s "$(realpath "$KBUILD_TOOLS_DIR")" "$COMMON_HDRS_DIR/tools"
            fi
        fi

        KBUILD_DIR="$(realpath "$ARCH_HDRS_DIR")"

        # Download virtio-gpu source files from upstream kernel tag
        VIRTIO_SRC_DIR="$(realpath -m "$COMMON_HDRS_DIR/drivers/gpu/drm/virtio")"
        if [ -n "$COMMON_HDRS_DIR" ]; then
            mkdir -p "$VIRTIO_SRC_DIR"

            MAJOR_MINOR_KVER=$(echo "$UPSTREAM_KVER" | grep -oP '^\d+\.\d+')

            GH_BASE=""
            for try_ver in "$UPSTREAM_KVER" "$MAJOR_MINOR_KVER"; do
                TEST_URL="https://raw.githubusercontent.com/torvalds/linux/v${try_ver}/drivers/gpu/drm/virtio/virtgpu_plane.c"
                if curl -fsSL -o /dev/null "$TEST_URL" 2>/dev/null; then
                    GH_BASE="https://raw.githubusercontent.com/torvalds/linux/v${try_ver}/drivers/gpu/drm/virtio"
                    echo "  Downloading virtio-gpu source from kernel v${try_ver} ..."
                    break
                else
                    echo "  Tag v${try_ver} not found, trying next..."
                fi
            done

            if [ -z "$GH_BASE" ]; then
                echo "  WARNING: Could not find kernel source tag for v${UPSTREAM_KVER} or v${MAJOR_MINOR_KVER}"
            else
                for f in virtgpu_drv.h virtgpu_drv.c virtgpu_kms.c virtgpu_gem.c \
                         virtgpu_plane.c virtgpu_display.c virtgpu_vq.c \
                         virtgpu_fence.c virtgpu_object.c virtgpu_ioctl.c \
                         virtgpu_prime.c virtgpu_trace.h virtgpu_debugfs.c \
                         virtgpu_submit.c virtgpu_trace_points.c virtgpu_vram.c \
                         Makefile Kconfig; do
                    curl -fsSL -o "$VIRTIO_SRC_DIR/$f" "$GH_BASE/$f" 2>/dev/null || true
                done
            fi

            PLANE_C="$VIRTIO_SRC_DIR/virtgpu_plane.c"
            if [ -f "$PLANE_C" ]; then
                echo "  Applying damage-clip fixes to virtgpu_plane.c ..."

                # Fix 1: The original code only sets ignore_damage_clips = true
                # (when fb changes) but never resets it to false.  Since
                # plane_state is memcpy'd from the previous commit's state,
                # once set to true it stays true forever.  Fix: replace the
                # entire if + assignment with an unconditional assignment.
                if grep -q 'ignore_damage_clips = true' "$PLANE_C"; then
                    python3 -c "
import re, sys
path = sys.argv[1]
with open(path) as f: s = f.read()
s2 = re.sub(
    r'if\s*\(old_plane_state->fb\s*!=\s*new_plane_state->fb\)\s*\n\s*new_plane_state->ignore_damage_clips\s*=\s*true\s*;',
    'new_plane_state->ignore_damage_clips = (old_plane_state->fb != new_plane_state->fb);',
    s)
if s2 != s:
    with open(path,'w') as f: f.write(s2)
    print('    Applied fix 1: unconditional ignore_damage_clips assignment')
else:
    print('    WARNING: fix 1 pattern not matched -- file may differ from expected')
" "$PLANE_C"
                fi

                # Fix 2: Replace drm_atomic_helper_damage_merged() call in
                # virtio_gpu_primary_plane_update with a custom version that
                # reads fb_damage_clips directly.  The stock helper's
                # damage_iter_init also triggers full-update when
                # state->src != old_state->src (16.16 fixed-point comparison
                # that can mismatch after check_plane_state recomputes src),
                # which makes damage clips useless on virtio-gpu.
                python3 - "$PLANE_C" << 'PYEOF'
import sys, re
path = sys.argv[1]
with open(path) as f:
    src = f.read()

REPLACEMENT = (
    '\t{\n'
    '\t\tstruct drm_plane_state *cur = plane->state;\n'
    '\t\tconst struct drm_mode_rect *cr;\n'
    '\t\tuint32_t nc = drm_plane_get_damage_clips_count(cur);\n'
    '\t\tif (nc > 0 && !cur->ignore_damage_clips) {\n'
    '\t\t\tuint32_t ii;\n'
    '\t\t\tcr = drm_plane_get_damage_clips(cur);\n'
    '\t\t\trect.x1 = cr[0].x1; rect.y1 = cr[0].y1;\n'
    '\t\t\trect.x2 = cr[0].x2; rect.y2 = cr[0].y2;\n'
    '\t\t\tfor (ii = 1; ii < nc; ii++) {\n'
    '\t\t\t\tif ((int)cr[ii].x1 < rect.x1) rect.x1 = cr[ii].x1;\n'
    '\t\t\t\tif ((int)cr[ii].y1 < rect.y1) rect.y1 = cr[ii].y1;\n'
    '\t\t\t\tif ((int)cr[ii].x2 > rect.x2) rect.x2 = cr[ii].x2;\n'
    '\t\t\t\tif ((int)cr[ii].y2 > rect.y2) rect.y2 = cr[ii].y2;\n'
    '\t\t\t}\n'
    '\t\t} else if (!cur->visible) {\n'
    '\t\t\treturn;\n'
    '\t\t} else {\n'
    '\t\t\tstruct drm_rect s = drm_plane_state_src(cur);\n'
    '\t\t\trect.x1 = s.x1 >> 16;\n'
    '\t\t\trect.y1 = s.y1 >> 16;\n'
    '\t\t\trect.x2 = (s.x2 >> 16) + !!(s.x2 & 0xFFFF);\n'
    '\t\t\trect.y2 = (s.y2 >> 16) + !!(s.y2 & 0xFFFF);\n'
    '\t\t}\n'
    '\t\tif (rect.x1 >= rect.x2 || rect.y1 >= rect.y2)\n'
    '\t\t\treturn;\n'
    '\t}'
)

pattern = r'[ \t]*if\s*\(\s*!drm_atomic_helper_damage_merged\s*\(\s*old_state\s*,\s*plane->state\s*,\s*&rect\s*\)\s*\)\s*\n\s*return\s*;'
m = re.search(pattern, src)
if m:
    src = src[:m.start()] + REPLACEMENT + src[m.end():]
    with open(path, 'w') as f:
        f.write(src)
    print("    Applied fix 2: bypass drm_atomic_helper_damage_merged")
else:
    print("    WARNING: could not find drm_atomic_helper_damage_merged block to patch")
    print(f"    File size: {len(src)} bytes, lines: {src.count(chr(10))}")
PYEOF

                echo "  Building patched virtio-gpu.ko ..."
                make -C "$KBUILD_DIR" M="$VIRTIO_SRC_DIR" modules \
                    -j"$(nproc)" 2>&1 | tail -20

                PATCHED_KO="$(find "$VIRTIO_SRC_DIR" -name 'virtio-gpu.ko' -o -name 'virtio_gpu.ko' | head -1)"
                if [ -n "$PATCHED_KO" ] && [ -f "$PATCHED_KO" ]; then
                    strip --strip-debug "$PATCHED_KO"
                    cp "$PATCHED_KO" "$DESTDIR/virtio-gpu.ko"
                    echo "  Replaced virtio-gpu.ko with patched version ($(du -h "$PATCHED_KO" | cut -f1))"
                    PATCHED_OK=true
                else
                    echo "  WARNING: patched .ko not found after build, keeping prebuilt"
                fi
            else
                echo "  virtgpu_plane.c not found, skipping patch"
            fi
        else
            echo "  WARNING: common headers dir not found, skipping patch"
        fi
    else
        echo "  WARNING: arch headers dir not found, skipping patch"
    fi
else
    echo "  WARNING: $HDRS_PKG not found in $SUITE, skipping damage-clip patch"
fi

if ! $PATCHED_OK; then
    echo "  NOTE: using prebuilt virtio-gpu.ko (damage-clip fix not applied)"
fi
