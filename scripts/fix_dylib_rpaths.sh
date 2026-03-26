#!/bin/bash
set -euo pipefail

# Fix dylib install names and inter-library references in Frameworks/
# Usage: fix_dylib_rpaths.sh <frameworks_dir> <executable_path>

FRAMEWORKS_DIR="$1"
EXECUTABLE="$2"

echo "Fixing dylib rpaths in $FRAMEWORKS_DIR"

# Step 1: Set each dylib's install name to @rpath/filename
for dylib in "$FRAMEWORKS_DIR"/*.dylib; do
    filename=$(basename "$dylib")
    current_id=$(otool -D "$dylib" | tail -1)
    if [ "$current_id" != "@rpath/$filename" ]; then
        install_name_tool -id "@rpath/$filename" "$dylib" 2>/dev/null || true
    fi
done

# Step 2: Fix inter-library references (e.g. OpenEXR libs referencing each other)
# and fix references to vendored libs that use absolute or @rpath with wrong versioned names
for dylib in "$FRAMEWORKS_DIR"/*.dylib; do
    # Get all dependencies
    otool -L "$dylib" | tail -n +2 | awk '{print $1}' | while read -r dep; do
        dep_filename=$(basename "$dep")
        # Skip system libs
        case "$dep" in
            /usr/lib/*|/System/*) continue ;;
        esac
        # If the dep exists in our Frameworks dir, rewrite to @rpath
        if [ -f "$FRAMEWORKS_DIR/$dep_filename" ]; then
            if [ "$dep" != "@rpath/$dep_filename" ]; then
                install_name_tool -change "$dep" "@rpath/$dep_filename" "$dylib" 2>/dev/null || true
            fi
        fi
    done
done

# Step 3: Fix the main executable's references to vendored libs
otool -L "$EXECUTABLE" | tail -n +2 | awk '{print $1}' | while read -r dep; do
    dep_filename=$(basename "$dep")
    # Skip system libs and already-correct @rpath refs
    case "$dep" in
        /usr/lib/*|/System/*|@rpath/*) continue ;;
    esac
    # If this dep exists in Frameworks, rewrite to @rpath
    if [ -f "$FRAMEWORKS_DIR/$dep_filename" ]; then
        install_name_tool -change "$dep" "@rpath/$dep_filename" "$EXECUTABLE" 2>/dev/null || true
        echo "  Fixed: $dep_filename"
    fi
done

echo "  Done"
