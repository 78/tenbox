#!/bin/bash
# Build TenBox for macOS (Apple Silicon).
#
# This script builds:
#   1. tenbox-vm-runtime (C++ via CMake)
#   2. TenBoxManager (Swift/Obj-C++ via SPM)
#   3. Assembles TenBox.app bundle
#   4. Signs and optionally creates DMG
#
# Usage:
#   ./build-macos.sh [--release|--debug]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."
BUILD_TYPE="${1:---release}"
CPU_COUNT=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

case "$BUILD_TYPE" in
    --release)
        CMAKE_BUILD_TYPE="Release"
        SWIFT_CONFIG="release"
        ;;
    --debug)
        CMAKE_BUILD_TYPE="Debug"
        SWIFT_CONFIG="debug"
        ;;
    *)
        echo "Usage: $0 [--release|--debug]"
        exit 1
        ;;
esac

VERSION=$(tr -d '[:space:]' < "$ROOT_DIR/VERSION")
if [ -z "$VERSION" ]; then
    echo "Error: Could not read version from $ROOT_DIR/VERSION"
    exit 1
fi

ARCH=$(uname -m)  # arm64 or x86_64

BUILD_DIR="$ROOT_DIR/build"
MANAGER_SRC="$ROOT_DIR/src/manager-macos"
APP_DIR="$BUILD_DIR/TenBox.app"
PLIST="$MANAGER_SRC/Resources/Info.plist"
ENTITLEMENTS="$MANAGER_SRC/Resources/TenBox.entitlements"

echo "===================================="
echo " TenBox macOS Build v$VERSION ($CMAKE_BUILD_TYPE)"
echo "===================================="
echo ""

# Stamp the version into Info.plist before building
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" "$PLIST"
echo "Version $VERSION written to Info.plist"
echo ""

# ── Step 1: Build tenbox-vm-runtime (C++ via CMake) ─────────────────────────
echo "[1/3] Building tenbox-vm-runtime..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$ROOT_DIR" \
    -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
    -DCMAKE_OSX_ARCHITECTURES=arm64

cmake --build . --target tenbox-vm-runtime -j"$CPU_COUNT"

if [ ! -f "$BUILD_DIR/tenbox-vm-runtime" ]; then
    echo "Error: tenbox-vm-runtime binary not found after build."
    exit 1
fi
echo "  -> $BUILD_DIR/tenbox-vm-runtime"

codesign --force --sign - --entitlements "$ENTITLEMENTS" "$BUILD_DIR/tenbox-vm-runtime"
echo "  -> codesign applied (ad-hoc + Hypervisor entitlement)"

# ── Step 2: Build TenBoxManager (Swift/Obj-C++ via SPM) ─────────────────────
echo ""
echo "[2/3] Building TenBoxManager via SPM ($SWIFT_CONFIG)..."

cd "$MANAGER_SRC"
swift build -c "$SWIFT_CONFIG"

SWIFT_BUILD_DIR="$MANAGER_SRC/.build/$SWIFT_CONFIG"
if [ ! -f "$SWIFT_BUILD_DIR/TenBoxManager" ]; then
    echo "Error: TenBoxManager binary not found at $SWIFT_BUILD_DIR/TenBoxManager"
    exit 1
fi
echo "  -> $SWIFT_BUILD_DIR/TenBoxManager"

# ── Step 3: Assemble TenBox.app bundle ──────────────────────────────────────
echo ""
echo "[3/3] Assembling TenBox.app..."

rm -rf "$APP_DIR"
mkdir -p "$APP_DIR/Contents/MacOS"
mkdir -p "$APP_DIR/Contents/Resources"

# Copy Info.plist
cp "$PLIST" "$APP_DIR/Contents/Info.plist"

# Copy executables
cp "$SWIFT_BUILD_DIR/TenBoxManager" "$APP_DIR/Contents/MacOS/TenBoxManager"
cp "$BUILD_DIR/tenbox-vm-runtime" "$APP_DIR/Contents/MacOS/tenbox-vm-runtime"

# Copy SPM resource bundles (icon, etc.)
BUNDLE_PATH=$(find "$SWIFT_BUILD_DIR" -name "TenBoxManager_TenBoxManager.bundle" -type d 2>/dev/null | head -1)
if [ -n "$BUNDLE_PATH" ] && [ -d "$BUNDLE_PATH" ]; then
    cp -R "$BUNDLE_PATH" "$APP_DIR/Contents/Resources/"
    echo "  -> Copied resource bundle"
fi

# Convert icon.png to AppIcon.icns (macOS requires .icns format)
if [ -f "$MANAGER_SRC/Resources/icon.png" ]; then
    ICONSET_DIR="$BUILD_DIR/AppIcon.iconset"
    rm -rf "$ICONSET_DIR"
    mkdir -p "$ICONSET_DIR"
    sips -z 16 16     "$MANAGER_SRC/Resources/icon.png" --out "$ICONSET_DIR/icon_16x16.png"      >/dev/null
    sips -z 32 32     "$MANAGER_SRC/Resources/icon.png" --out "$ICONSET_DIR/icon_16x16@2x.png"   >/dev/null
    sips -z 32 32     "$MANAGER_SRC/Resources/icon.png" --out "$ICONSET_DIR/icon_32x32.png"      >/dev/null
    sips -z 64 64     "$MANAGER_SRC/Resources/icon.png" --out "$ICONSET_DIR/icon_32x32@2x.png"   >/dev/null
    sips -z 128 128   "$MANAGER_SRC/Resources/icon.png" --out "$ICONSET_DIR/icon_128x128.png"    >/dev/null
    sips -z 256 256   "$MANAGER_SRC/Resources/icon.png" --out "$ICONSET_DIR/icon_128x128@2x.png" >/dev/null
    sips -z 256 256   "$MANAGER_SRC/Resources/icon.png" --out "$ICONSET_DIR/icon_256x256.png"    >/dev/null
    sips -z 512 512   "$MANAGER_SRC/Resources/icon.png" --out "$ICONSET_DIR/icon_256x256@2x.png" >/dev/null
    sips -z 512 512   "$MANAGER_SRC/Resources/icon.png" --out "$ICONSET_DIR/icon_512x512.png"    >/dev/null
    cp "$MANAGER_SRC/Resources/icon.png"                       "$ICONSET_DIR/icon_512x512@2x.png"
    iconutil -c icns "$ICONSET_DIR" -o "$APP_DIR/Contents/Resources/AppIcon.icns"
    rm -rf "$ICONSET_DIR"
    echo "  -> Generated AppIcon.icns from icon.png"
fi

# Compile Metal shaders if present
METAL_SRC="$MANAGER_SRC/Resources/Shaders.metal"
if [ -f "$METAL_SRC" ]; then
    echo "  -> Compiling Metal shaders..."
    if xcrun metal -c "$METAL_SRC" -o "$BUILD_DIR/Shaders.air" 2>/dev/null && \
       xcrun metallib "$BUILD_DIR/Shaders.air" -o "$APP_DIR/Contents/Resources/default.metallib" 2>/dev/null; then
        rm -f "$BUILD_DIR/Shaders.air"
    else
        rm -f "$BUILD_DIR/Shaders.air"
        echo "  -> WARNING: Metal shader compilation failed, copying .metal source as fallback"
        echo "     To install Metal toolchain: xcodebuild -downloadComponent MetalToolchain"
        cp "$METAL_SRC" "$APP_DIR/Contents/Resources/"
    fi
fi

# Copy Sparkle framework from SPM build artifacts
SPARKLE_FRAMEWORK=$(find "$MANAGER_SRC/.build/artifacts" -name "Sparkle.framework" -type d 2>/dev/null | head -1)
if [ -n "$SPARKLE_FRAMEWORK" ] && [ -d "$SPARKLE_FRAMEWORK" ]; then
    mkdir -p "$APP_DIR/Contents/Frameworks"
    cp -R "$SPARKLE_FRAMEWORK" "$APP_DIR/Contents/Frameworks/"
    echo "  -> Copied Sparkle.framework"
fi

# Add rpath so the binary can find Sparkle.framework at runtime
install_name_tool -add_rpath "@loader_path/../Frameworks" \
    "$APP_DIR/Contents/MacOS/TenBoxManager" 2>/dev/null || true

# Sign the app bundle
echo "  -> Signing TenBox.app..."
if security find-identity -v -p codesigning 2>/dev/null | grep -q "Developer ID"; then
    IDENTITY=$(security find-identity -v -p codesigning | grep "Developer ID" | head -1 | awk -F'"' '{print $2}')
    echo "     Using: $IDENTITY"
    codesign --deep --force --options runtime \
        --entitlements "$ENTITLEMENTS" \
        --sign "$IDENTITY" "$APP_DIR"
else
    echo "     Using: ad-hoc (no Developer ID found)"
    codesign --deep --force --options runtime \
        --entitlements "$ENTITLEMENTS" \
        --sign - "$APP_DIR"
fi

echo ""
echo "  -> $APP_DIR"

# ── Step 4: Create ZIP for Sparkle updates + EdDSA signature ────────────────
echo ""
ZIP_PATH="$BUILD_DIR/TenBox_${VERSION}_${ARCH}.zip"
echo "Creating Sparkle update ZIP..."
ditto -c -k --keepParent "$APP_DIR" "$ZIP_PATH"
echo "  -> $ZIP_PATH"

SIGN_TOOL=$(find "$MANAGER_SRC/.build/artifacts" -name "sign_update" -type f 2>/dev/null | head -1)
if [ -n "$SIGN_TOOL" ] && [ -x "$SIGN_TOOL" ]; then
    echo ""
    echo "Signing ZIP with Sparkle EdDSA key..."
    ED_SIGNATURE=$("$SIGN_TOOL" "$ZIP_PATH" 2>&1) || true
    echo "$ED_SIGNATURE"
    echo ""
    echo "Copy the sparkle:edSignature value above into publish.py when releasing."
else
    echo ""
    echo "WARNING: Sparkle sign_update tool not found."
    echo "  Run 'swift build' once in src/manager-macos to fetch Sparkle artifacts,"
    echo "  then sign manually: sign_update $ZIP_PATH"
fi

echo ""
echo "===================================="
echo " Build complete!"
echo "===================================="
echo ""
echo "Artifacts:"
echo "  App:  $APP_DIR"
echo "  ZIP:  $ZIP_PATH"
echo ""
echo "To create a DMG for distribution:"
echo "  $SCRIPT_DIR/make-dmg.sh $APP_DIR"
echo ""
