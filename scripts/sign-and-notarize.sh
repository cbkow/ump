#!/bin/bash
set -euo pipefail

# QCView macOS Signing & Notarization Script
# Usage: ./scripts/sign-and-notarize.sh [build-dir]
#
# Prerequisites:
#   - Developer ID Application certificate installed in Keychain
#   - notarytool credentials stored as "AC_PASSWORD"
#   - Clean Release build in build-dir

BUILD_DIR="${1:-build-macos}"
APP_BUNDLE="$BUILD_DIR/qcview.app"
ENTITLEMENTS="packaging/macos/entitlements.plist"
SIGNING_IDENTITY="Developer ID Application: Christopher Bialkowski (5Z4S9VHV56)"
DMG_NAME="QCView-$(plutil -extract CFBundleShortVersionString raw "$APP_BUNDLE/Contents/Info.plist")-macOS.dmg"
DMG_PATH="$BUILD_DIR/$DMG_NAME"

echo "=== QCView macOS Signing & Notarization ==="
echo "App:          $APP_BUNDLE"
echo "Identity:     $SIGNING_IDENTITY"
echo "Entitlements: $ENTITLEMENTS"
echo ""

# Verify prerequisites
if [ ! -d "$APP_BUNDLE" ]; then
    echo "ERROR: App bundle not found at $APP_BUNDLE"
    exit 1
fi

if [ ! -f "$ENTITLEMENTS" ]; then
    echo "ERROR: Entitlements not found at $ENTITLEMENTS"
    exit 1
fi

security find-identity -v -p codesigning | grep -q "Developer ID Application" || {
    echo "ERROR: No Developer ID Application certificate found"
    exit 1
}

# =========================================================================
# Step 1: Strip existing ad-hoc signatures
# =========================================================================
echo "[1/6] Stripping existing signatures..."
codesign --remove-signature "$APP_BUNDLE" 2>/dev/null || true
for dylib in "$APP_BUNDLE/Contents/Frameworks/"*.dylib; do
    codesign --remove-signature "$dylib" 2>/dev/null || true
done

# =========================================================================
# Step 2: Sign all dylibs in Frameworks/ (must sign inside-out)
# =========================================================================
echo "[2/6] Signing Frameworks..."
for dylib in "$APP_BUNDLE/Contents/Frameworks/"*.dylib; do
    codesign --force --sign "$SIGNING_IDENTITY" \
        --timestamp \
        --options runtime \
        "$dylib"
done
echo "  Signed $(ls "$APP_BUNDLE/Contents/Frameworks/"*.dylib | wc -l | tr -d ' ') dylibs"

# =========================================================================
# Step 3: Sign the main executable with entitlements + hardened runtime
# =========================================================================
echo "[3/6] Signing app bundle..."
codesign --force --sign "$SIGNING_IDENTITY" \
    --timestamp \
    --options runtime \
    --entitlements "$ENTITLEMENTS" \
    "$APP_BUNDLE"

# =========================================================================
# Step 4: Verify signature
# =========================================================================
echo "[4/6] Verifying signature..."
codesign --verify --deep --strict --verbose=2 "$APP_BUNDLE" 2>&1 | tail -3
echo "  Signature valid"

# Also check Gatekeeper assessment
spctl --assess --type exec --verbose "$APP_BUNDLE" 2>&1 || echo "  (spctl may fail before notarization — this is expected)"

# =========================================================================
# Step 5: Create DMG for distribution
# =========================================================================
echo "[5/6] Creating DMG..."
rm -f "$DMG_PATH"

# Create a temporary directory with the app + Applications symlink
STAGING_DIR=$(mktemp -d)
cp -R "$APP_BUNDLE" "$STAGING_DIR/"
ln -s /Applications "$STAGING_DIR/Applications"

hdiutil create -volname "QCView" \
    -srcfolder "$STAGING_DIR" \
    -ov -format UDZO \
    "$DMG_PATH"

rm -rf "$STAGING_DIR"

# Sign the DMG itself
codesign --force --sign "$SIGNING_IDENTITY" --timestamp "$DMG_PATH"
echo "  Created $DMG_PATH"

# =========================================================================
# Step 6: Submit for notarization
# =========================================================================
echo "[6/6] Submitting for notarization..."
echo "  This may take several minutes..."

xcrun notarytool submit "$DMG_PATH" \
    --keychain-profile "AC_PASSWORD" \
    --wait

# Staple the notarization ticket to the DMG
echo "  Stapling notarization ticket..."
xcrun stapler staple "$DMG_PATH"

echo ""
echo "=== Done ==="
echo "Signed & notarized DMG: $DMG_PATH"
echo ""
echo "To verify: spctl --assess --type open --context context:primary-signature --verbose $DMG_PATH"
