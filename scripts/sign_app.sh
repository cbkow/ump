#!/bin/bash
# Sign QCView.app bundle for macOS
# Usage: sign_app.sh <identity> <entitlements> <app_bundle>
#
# For local dev:    sign_app.sh - entitlements.plist qcview.app
# For distribution: sign_app.sh "Developer ID Application: ..." entitlements.plist qcview.app

set -e

IDENTITY="$1"
ENTITLEMENTS="$2"
APP_BUNDLE="$3"

if [ -z "$APP_BUNDLE" ]; then
    echo "Usage: sign_app.sh <identity> <entitlements> <app_bundle>"
    exit 1
fi

FRAMEWORKS_DIR="$APP_BUNDLE/Contents/Frameworks"
EXECUTABLE="$APP_BUNDLE/Contents/MacOS/qcview"

# Step 0: Remove stale data from Contents/MacOS/ (only the executable belongs here).
# Assets/shaders/fonts live in Contents/Resources/ — leftover copies in MacOS/
# cause codesign to fail because non-Mach-O files can't be signed.
if [ -d "$APP_BUNDLE/Contents/MacOS/assets" ]; then
    rm -rf "$APP_BUNDLE/Contents/MacOS/assets"
    echo "  Cleaned stale MacOS/assets/"
fi
if [ -d "$APP_BUNDLE/Contents/MacOS/shaders" ]; then
    rm -rf "$APP_BUNDLE/Contents/MacOS/shaders"
    echo "  Cleaned stale MacOS/shaders/"
fi

# Step 1: Sign all dylibs in Frameworks/
if [ -d "$FRAMEWORKS_DIR" ]; then
    find "$FRAMEWORKS_DIR" -name '*.dylib' -print0 | while IFS= read -r -d '' dylib; do
        codesign --force --sign "$IDENTITY" --options runtime "$dylib" 2>/dev/null || true
    done
    echo "  Signed dylibs in Frameworks/"
fi

# Step 2: Sign the main executable with entitlements
codesign --force --sign "$IDENTITY" \
    --entitlements "$ENTITLEMENTS" \
    --options runtime \
    "$EXECUTABLE"
echo "  Signed executable"

# Step 3: Sign the app bundle (seals Info.plist, Resources, Frameworks, etc.)
codesign --force --sign "$IDENTITY" \
    --entitlements "$ENTITLEMENTS" \
    --options runtime \
    "$APP_BUNDLE"

echo "  Signed $APP_BUNDLE"
