#!/bin/bash
# Package TenBox.app into a DMG disk image for distribution.
#
# Usage:
#   ./make-dmg.sh                             # auto-detect universal app
#   ./make-dmg.sh path/to/TenBox.app          # single app
#   ./make-dmg.sh path/to/TenBox.app out.dmg  # single app with custom output
#
# When invoked without arguments, the script looks for
#   build/TenBox-<VERSION>.app  (universal binary)
#
# Prerequisites:
#   - TenBox.app must be a valid macOS application bundle
#
# The script creates a DMG with:
#   - TenBox.app
#   - A symlink to /Applications for drag-and-drop install
#   - tenbox-vm-runtime bundled inside the .app

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."
BUILD_DIR="$ROOT_DIR/build"

VERSION=$(tr -d '[:space:]' < "$ROOT_DIR/VERSION")
if [ -z "$VERSION" ]; then
    echo "Error: Could not read version from $ROOT_DIR/VERSION"
    exit 1
fi

VOLUME_NAME="TenBox"

# ── Build DMG for a single .app ─────────────────────────────────────────────
make_dmg_for_app() {
    local APP_PATH="$1"
    local OUTPUT="$2"

    OUTPUT="${OUTPUT:-$BUILD_DIR/TenBox-${VERSION}.dmg}"

    if [ ! -d "$APP_PATH" ]; then
        echo "Error: Application bundle not found: $APP_PATH"
        return 1
    fi

    echo ""
    echo "──────────────────────────────────────────────"
    echo "  Packaging: $(basename "$APP_PATH")"
    echo "──────────────────────────────────────────────"

    # Sign the app if a signing identity is available
    local ENTITLEMENTS="$SCRIPT_DIR/../src/manager-macos/Resources/TenBox.entitlements"
    if security find-identity -v -p codesigning 2>/dev/null | grep -q "Developer ID"; then
        local IDENTITY
        IDENTITY=$(security find-identity -v -p codesigning | grep "Developer ID" | head -1 | awk -F'"' '{print $2}')
        echo "Signing with: $IDENTITY"
        codesign --deep --force --options runtime \
            --entitlements "$ENTITLEMENTS" \
            --sign "$IDENTITY" "$APP_PATH"
    else
        echo "No Developer ID found, signing with ad-hoc identity..."
        codesign --deep --force --options runtime \
            --entitlements "$ENTITLEMENTS" \
            --sign - "$APP_PATH"
    fi

    # ── Create DMG ───────────────────────────────────────────────────────
    # Uses APFS (required for macOS 26 Tahoe where HFS+ has mounting bugs) and
    # a temporary volume name to avoid Gatekeeper blocking ditto when the .app
    # name matches the volume name. Produces a styled DMG with 128px icons,
    # drag-and-drop arrow background, and correct Finder view settings.

    rm -f "$OUTPUT"

    # If the .app has a build name (e.g. TenBox-0.4.2.app), stage it as
    # TenBox.app so the DMG always shows the clean name to users.
    local DMG_APP_PATH="$APP_PATH"
    local STAGING_DIR=""
    local APP_BASENAME
    APP_BASENAME=$(basename "$APP_PATH")
    if [ "$APP_BASENAME" != "TenBox.app" ]; then
        STAGING_DIR="$BUILD_DIR/.dmg-staging-$$"
        rm -rf "$STAGING_DIR"
        mkdir -p "$STAGING_DIR"
        cp -R "$APP_PATH" "$STAGING_DIR/TenBox.app"
        DMG_APP_PATH="$STAGING_DIR/TenBox.app"
    fi

    echo "Building styled DMG..."
    python3 "$SCRIPT_DIR/make-dmg-styled.py" "$DMG_APP_PATH" "$VOLUME_NAME" "$OUTPUT"

    if [ -n "$STAGING_DIR" ]; then
        rm -rf "$STAGING_DIR"
    fi

    echo "  -> DMG created: $OUTPUT"

    # ── Notarize the DMG ─────────────────────────────────────────────────
    if xcrun notarytool history --keychain-profile "AC_PASSWORD" >/dev/null 2>&1; then
        echo ""
        echo "Submitting DMG for Apple notarization..."
        echo "(This typically takes 2-10 minutes)"

        xcrun notarytool submit "$OUTPUT" \
            --keychain-profile "AC_PASSWORD" \
            --wait
        local NOTARY_EXIT=$?

        if [ $NOTARY_EXIT -eq 0 ]; then
            echo ""
            echo "Stapling notarization ticket to DMG..."
            xcrun stapler staple "$OUTPUT"
            echo "Notarization complete!"
        else
            echo ""
            echo "WARNING: Notarization failed (exit code $NOTARY_EXIT)."
            echo "  Check details: xcrun notarytool log <submission-id> --keychain-profile AC_PASSWORD"
            return 1
        fi
    else
        echo ""
        echo "Skipping notarization (no AC_PASSWORD keychain profile found)."
        echo "To enable, run:"
        echo "  xcrun notarytool store-credentials AC_PASSWORD \\"
        echo "      --apple-id YOUR_APPLE_ID --team-id YOUR_TEAM_ID --password APP_SPECIFIC_PASSWORD"
    fi

    echo ""
    echo "============================================"
    echo "DMG created: $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"
    echo "============================================"
}

# ── Main ─────────────────────────────────────────────────────────────────────

if [ $# -ge 1 ]; then
    make_dmg_for_app "$1" "$2"
else
    APP="$BUILD_DIR/TenBox-${VERSION}.app"
    if [ -d "$APP" ]; then
        make_dmg_for_app "$APP" ""
    else
        echo "Error: App bundle not found: $APP"
        echo ""
        echo "Run build-macos.sh first, or specify the .app path explicitly:"
        echo "  $0 path/to/TenBox.app"
        exit 1
    fi
fi
