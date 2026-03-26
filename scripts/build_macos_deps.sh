#!/bin/bash
# =============================================================================
# Build vendored arm64 dylibs for QCView macOS port
# Run from the repository root: ./scripts/build_macos_deps.sh
# Requires: Homebrew, cmake, nasm, pkg-config
# =============================================================================

set -e

eval "$(/opt/homebrew/bin/brew shellenv)"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="/tmp/qcview-deps-build"
INSTALL_DIR="/tmp/qcview-deps-install"
EXTERNAL_DIR="$REPO_ROOT/external"

MACOS_DEPLOYMENT_TARGET="13.0"
ARCH="arm64"
NPROC=$(sysctl -n hw.ncpu)

export MACOSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET"
export CMAKE_OSX_ARCHITECTURES="$ARCH"

mkdir -p "$BUILD_DIR" "$INSTALL_DIR"

echo "============================================"
echo "QCView macOS Dependencies Builder"
echo "  Arch: $ARCH"
echo "  Deployment target: $MACOS_DEPLOYMENT_TARGET"
echo "  Build dir: $BUILD_DIR"
echo "  Install dir: $INSTALL_DIR"
echo "  Parallel jobs: $NPROC"
echo "============================================"

# =============================================================================
# Helper: copy dylibs + headers to external/
# =============================================================================
vendor_lib() {
    local name="$1"
    local src_lib="$INSTALL_DIR/lib"
    local src_inc="$INSTALL_DIR/include"
    local dst="$EXTERNAL_DIR/$name/macos"

    echo ">>> Vendoring $name to $dst"
    mkdir -p "$dst/lib" "$dst/include"

    # Copy dylibs
    if ls "$src_lib"/lib${name}*.dylib 2>/dev/null 1>&2; then
        cp -a "$src_lib"/lib${name}*.dylib "$dst/lib/"
    fi

    # Copy headers if pattern-specific dir exists
    if [ -d "$src_inc/$name" ]; then
        cp -R "$src_inc/$name" "$dst/include/"
    fi
}

# =============================================================================
# Common flags for static codec dependencies
# These get linked INTO FFmpeg's dylibs so there are zero transitive deps.
# =============================================================================
COMMON_CFLAGS="-mmacosx-version-min=$MACOS_DEPLOYMENT_TARGET -arch arm64"
COMMON_LDFLAGS="-mmacosx-version-min=$MACOS_DEPLOYMENT_TARGET -arch arm64"
COMMON_CMAKE_ARGS=(
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR"
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_OSX_ARCHITECTURES=arm64
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET"
    -DBUILD_SHARED_LIBS=OFF
)

# =============================================================================
# 1a. x264 (static)
# =============================================================================
build_x264() {
    echo ""
    echo "============================================"
    echo "Building x264 (static)..."
    echo "============================================"

    local SRC="$BUILD_DIR/x264"
    if [ ! -d "$SRC" ]; then
        git clone --depth 1 https://code.videolan.org/videolan/x264.git "$SRC"
    fi

    cd "$SRC"
    make distclean 2>/dev/null || true

    ./configure \
        --prefix="$INSTALL_DIR" \
        --enable-static \
        --disable-shared \
        --disable-cli \
        --enable-pic \
        --host=aarch64-apple-darwin \
        --extra-cflags="$COMMON_CFLAGS" \
        --extra-ldflags="$COMMON_LDFLAGS"

    make -j"$NPROC"
    make install

    echo "x264: Done"
}

# =============================================================================
# 1b. x265 (static)
# =============================================================================
build_x265() {
    echo ""
    echo "============================================"
    echo "Building x265 (static)..."
    echo "============================================"

    local SRC="$BUILD_DIR/x265"
    if [ ! -d "$SRC" ]; then
        git clone --depth 1 --branch 4.1 https://bitbucket.org/multicoreware/x265_git.git "$SRC"
    fi

    # Patch CMakeLists for CMake 4.x compatibility
    # CMP0025 NEW means AppleClang is reported as "AppleClang" not "Clang",
    # so we must also match AppleClang for NEON detection to work.
    sed -i '' 's/cmake_policy(SET CMP0025 OLD)/cmake_policy(SET CMP0025 NEW)/' "$SRC/source/CMakeLists.txt"
    sed -i '' 's/cmake_policy(SET CMP0054 OLD)/cmake_policy(SET CMP0054 NEW)/' "$SRC/source/CMakeLists.txt"
    sed -i '' 's/cmake_minimum_required (VERSION 2.8.8)/cmake_minimum_required(VERSION 3.10)/' "$SRC/source/CMakeLists.txt"
    # Fix: AppleClang must also set CLANG=1 for NEON/arm64 code paths
    sed -i '' 's/\${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang"/CMAKE_CXX_COMPILER_ID MATCHES "Clang"/' "$SRC/source/CMakeLists.txt"

    cmake -S "$SRC/source" -B "$SRC/build" \
        "${COMMON_CMAKE_ARGS[@]}" \
        -DENABLE_SHARED=OFF \
        -DENABLE_CLI=OFF

    cmake --build "$SRC/build" -j"$NPROC"
    cmake --install "$SRC/build"

    echo "x265: Done"
}

# =============================================================================
# 1c. dav1d (static) — AV1 decoder
# =============================================================================
build_dav1d() {
    echo ""
    echo "============================================"
    echo "Building dav1d (static)..."
    echo "============================================"

    # Requires meson + ninja
    if ! command -v meson &>/dev/null; then
        echo "Installing meson via pip..."
        pip3 install --user meson
    fi
    if ! command -v ninja &>/dev/null; then
        brew install ninja
    fi

    local SRC="$BUILD_DIR/dav1d"
    if [ ! -d "$SRC" ]; then
        git clone --depth 1 --branch 1.5.1 https://code.videolan.org/videolan/dav1d.git "$SRC"
    fi

    cd "$SRC"
    rm -rf builddir

    meson setup builddir \
        --prefix="$INSTALL_DIR" \
        --default-library=static \
        -Denable_tools=false \
        -Denable_tests=false

    ninja -C builddir -j"$NPROC"
    ninja -C builddir install

    echo "dav1d: Done"
}

# =============================================================================
# 1d. libvpx (static) — VP8/VP9
# =============================================================================
build_vpx() {
    echo ""
    echo "============================================"
    echo "Building libvpx (static)..."
    echo "============================================"

    local SRC="$BUILD_DIR/libvpx"
    if [ ! -d "$SRC" ]; then
        git clone --depth 1 --branch v1.15.0 https://chromium.googlesource.com/webm/libvpx "$SRC"
    fi

    cd "$SRC"
    make distclean 2>/dev/null || true

    ./configure \
        --prefix="$INSTALL_DIR" \
        --target=arm64-darwin23-gcc \
        --enable-static \
        --disable-shared \
        --disable-examples \
        --disable-tools \
        --disable-unit-tests \
        --enable-pic \
        --enable-vp8 \
        --enable-vp9

    make -j"$NPROC"
    make install

    echo "libvpx: Done"
}

# =============================================================================
# 1e. lame (static) — MP3 encoder
# =============================================================================
build_lame() {
    echo ""
    echo "============================================"
    echo "Building lame (static)..."
    echo "============================================"

    local SRC="$BUILD_DIR/lame"
    if [ ! -d "$SRC" ]; then
        mkdir -p "$SRC"
        cd "$SRC"
        curl -L "https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz" | tar xz --strip-components=1
    fi

    cd "$SRC"
    make distclean 2>/dev/null || true

    ./configure \
        --prefix="$INSTALL_DIR" \
        --enable-static \
        --disable-shared \
        --disable-frontend \
        --host=aarch64-apple-darwin \
        CFLAGS="$COMMON_CFLAGS" \
        LDFLAGS="$COMMON_LDFLAGS"

    make -j"$NPROC"
    make install

    echo "lame: Done"
}

# =============================================================================
# 1f. opus (static)
# =============================================================================
build_opus() {
    echo ""
    echo "============================================"
    echo "Building opus (static)..."
    echo "============================================"

    local SRC="$BUILD_DIR/opus"
    if [ ! -d "$SRC" ]; then
        git clone --depth 1 --branch v1.5.2 https://gitlab.xiph.org/xiph/opus.git "$SRC"
    fi

    cmake -S "$SRC" -B "$SRC/build" \
        "${COMMON_CMAKE_ARGS[@]}" \
        -DOPUS_BUILD_TESTING=OFF \
        -DOPUS_BUILD_PROGRAMS=OFF

    cmake --build "$SRC/build" -j"$NPROC"
    cmake --install "$SRC/build"

    echo "opus: Done"
}

# =============================================================================
# 1g. SVT-AV1 (static) — AV1 encoder
# =============================================================================
build_svtav1() {
    echo ""
    echo "============================================"
    echo "Building SVT-AV1 (static)..."
    echo "============================================"

    local SRC="$BUILD_DIR/SVT-AV1"
    if [ ! -d "$SRC" ]; then
        git clone --depth 1 --branch v2.3.0 https://gitlab.com/AOMediaCodec/SVT-AV1.git "$SRC"
    fi

    cmake -S "$SRC" -B "$SRC/build" \
        "${COMMON_CMAKE_ARGS[@]}" \
        -DBUILD_APPS=OFF \
        -DBUILD_TESTING=OFF \
        -DBUILD_DEC=OFF

    cmake --build "$SRC/build" -j"$NPROC"
    cmake --install "$SRC/build"

    echo "SVT-AV1: Done"
}

# =============================================================================
# 1. FFmpeg 8.1 (with VideoToolbox + all codecs)
#    Links codec deps statically — resulting dylibs are self-contained.
# =============================================================================
build_ffmpeg() {
    echo ""
    echo "============================================"
    echo "Building FFmpeg 8.1 with VideoToolbox + codecs..."
    echo "============================================"

    local SRC="$BUILD_DIR/ffmpeg"
    if [ ! -d "$SRC" ]; then
        git clone --depth 1 --branch n8.1 https://git.ffmpeg.org/ffmpeg.git "$SRC"
    fi

    cd "$SRC"

    # Clean any previous build
    make distclean 2>/dev/null || true

    # pkg-config must find our static deps
    export PKG_CONFIG_PATH="$INSTALL_DIR/lib/pkgconfig:$INSTALL_DIR/share/pkgconfig"

    ./configure \
        --prefix="$INSTALL_DIR" \
        --enable-shared \
        --disable-static \
        --enable-pthreads \
        --enable-videotoolbox \
        --enable-audiotoolbox \
        --enable-hwaccel=h264_videotoolbox \
        --enable-hwaccel=hevc_videotoolbox \
        --enable-hwaccel=prores_videotoolbox \
        --enable-gpl \
        --enable-version3 \
        --enable-nonfree \
        --enable-libx264 \
        --enable-libx265 \
        --enable-libdav1d \
        --enable-libvpx \
        --enable-libmp3lame \
        --enable-libopus \
        --enable-libsvtav1 \
        --disable-programs \
        --disable-doc \
        --disable-debug \
        --arch=arm64 \
        --extra-cflags="-I$INSTALL_DIR/include $COMMON_CFLAGS" \
        --extra-ldflags="-L$INSTALL_DIR/lib $COMMON_LDFLAGS" \
        --extra-libs="-lc++"

    make -j"$NPROC"
    make install

    # Vendor — include libpostproc alongside av*/sw*
    local dst="$EXTERNAL_DIR/ffmpeg/macos"
    rm -rf "$dst/lib" "$dst/include"
    mkdir -p "$dst/lib" "$dst/include"
    cp -a "$INSTALL_DIR"/lib/libav*.dylib \
          "$INSTALL_DIR"/lib/libsw*.dylib \
          "$dst/lib/"
    cp -R "$INSTALL_DIR"/include/libav* \
          "$INSTALL_DIR"/include/libsw* \
          "$dst/include/"

    # Fix install names to use @rpath (self-contained bundle)
    for dylib in "$dst"/lib/*.dylib; do
        [ -L "$dylib" ] && continue  # skip symlinks
        install_name_tool -id "@rpath/$(basename "$dylib")" "$dylib" 2>/dev/null || true
        # Fix inter-library references (install_dir → @rpath)
        otool -L "$dylib" | grep "$INSTALL_DIR" | awk '{print $1}' | while read old_path; do
            local depname=$(basename "$old_path")
            install_name_tool -change "$old_path" "@rpath/$depname" "$dylib" 2>/dev/null || true
        done
    done

    # Verify no absolute paths leaked
    local leaks=$(for f in "$dst"/lib/*.dylib; do otool -L "$f" | grep -v '@rpath\|/usr/lib\|/System\|:$'; done | wc -l)
    if [ "$leaks" -gt 0 ]; then
        echo "WARNING: Found $leaks non-system absolute paths in FFmpeg dylibs"
        for f in "$dst"/lib/*.dylib; do otool -L "$f" | grep -v '@rpath\|/usr/lib\|/System\|:$'; done
    else
        echo "FFmpeg dylibs are fully self-contained"
    fi

    echo "FFmpeg 8.1: Done"
}

# =============================================================================
# 2. OpenColorIO 2.5
# =============================================================================
build_ocio() {
    echo ""
    echo "============================================"
    echo "Building OpenColorIO 2.5..."
    echo "============================================"

    local SRC="$BUILD_DIR/OpenColorIO"
    if [ ! -d "$SRC" ]; then
        git clone --depth 1 --branch v2.5.0 https://github.com/AcademySoftwareFoundation/OpenColorIO.git "$SRC"
    fi

    cmake -S "$SRC" -B "$SRC/build" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" \
        -DBUILD_SHARED_LIBS=ON \
        -DOCIO_BUILD_APPS=OFF \
        -DOCIO_BUILD_TESTS=OFF \
        -DOCIO_BUILD_GPU_TESTS=OFF \
        -DOCIO_BUILD_PYTHON=OFF \
        -DOCIO_INSTALL_EXT_PACKAGES=ALL

    cmake --build "$SRC/build" -j"$NPROC"
    cmake --install "$SRC/build"

    # Vendor
    local dst="$EXTERNAL_DIR/ocio/macos"
    mkdir -p "$dst/lib" "$dst/include"
    cp -a "$INSTALL_DIR"/lib/libOpenColorIO*.dylib "$dst/lib/"
    cp -R "$INSTALL_DIR"/include/OpenColorIO "$dst/include/"

    # Fix install names
    for dylib in "$dst"/lib/*.dylib; do
        [ -L "$dylib" ] && continue
        install_name_tool -id "@rpath/$(basename "$dylib")" "$dylib" 2>/dev/null || true
    done

    echo "OpenColorIO: Done"
}

# =============================================================================
# 3. OpenEXR + Imath
# =============================================================================
build_openexr() {
    echo ""
    echo "============================================"
    echo "Building Imath + OpenEXR..."
    echo "============================================"

    # Imath first
    local IMATH_SRC="$BUILD_DIR/Imath"
    if [ ! -d "$IMATH_SRC" ]; then
        git clone --depth 1 --branch v3.2.1 https://github.com/AcademySoftwareFoundation/Imath.git "$IMATH_SRC"
    fi

    cmake -S "$IMATH_SRC" -B "$IMATH_SRC/build" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" \
        -DBUILD_SHARED_LIBS=ON \
        -DBUILD_TESTING=OFF

    cmake --build "$IMATH_SRC/build" -j"$NPROC"
    cmake --install "$IMATH_SRC/build"

    # OpenEXR
    local EXR_SRC="$BUILD_DIR/openexr"
    if [ ! -d "$EXR_SRC" ]; then
        git clone --depth 1 --branch v3.3.5 https://github.com/AcademySoftwareFoundation/openexr.git "$EXR_SRC"
    fi

    cmake -S "$EXR_SRC" -B "$EXR_SRC/build" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" \
        -DBUILD_SHARED_LIBS=ON \
        -DBUILD_TESTING=OFF \
        -DOPENEXR_BUILD_TOOLS=OFF \
        -DOPENEXR_INSTALL_EXAMPLES=OFF \
        -DCMAKE_PREFIX_PATH="$INSTALL_DIR"

    cmake --build "$EXR_SRC/build" -j"$NPROC"
    cmake --install "$EXR_SRC/build"

    # Vendor
    local dst="$EXTERNAL_DIR/openexr/macos"
    mkdir -p "$dst/lib" "$dst/include"
    cp -a "$INSTALL_DIR"/lib/libOpenEXR*.dylib "$INSTALL_DIR"/lib/libIex*.dylib \
          "$INSTALL_DIR"/lib/libIlmThread*.dylib "$INSTALL_DIR"/lib/libOpenEXRCore*.dylib \
          "$INSTALL_DIR"/lib/libImath*.dylib "$dst/lib/" 2>/dev/null || true
    cp -R "$INSTALL_DIR"/include/OpenEXR "$dst/include/" 2>/dev/null || true
    cp -R "$INSTALL_DIR"/include/Imath "$dst/include/" 2>/dev/null || true

    # Fix install names
    for dylib in "$dst"/lib/*.dylib; do
        [ -L "$dylib" ] && continue
        install_name_tool -id "@rpath/$(basename "$dylib")" "$dylib" 2>/dev/null || true
    done

    echo "OpenEXR + Imath: Done"
}

# =============================================================================
# 4. Image libraries (libtiff, libpng, libjpeg-turbo)
# =============================================================================
build_image_libs() {
    echo ""
    echo "============================================"
    echo "Building image libraries..."
    echo "============================================"

    # libjpeg-turbo
    local JPEG_SRC="$BUILD_DIR/libjpeg-turbo"
    if [ ! -d "$JPEG_SRC" ]; then
        git clone --depth 1 --branch 3.1.0 https://github.com/libjpeg-turbo/libjpeg-turbo.git "$JPEG_SRC"
    fi
    cmake -S "$JPEG_SRC" -B "$JPEG_SRC/build" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" \
        -DENABLE_SHARED=ON -DENABLE_STATIC=OFF
    cmake --build "$JPEG_SRC/build" -j"$NPROC"
    cmake --install "$JPEG_SRC/build"

    # libpng
    local PNG_SRC="$BUILD_DIR/libpng"
    if [ ! -d "$PNG_SRC" ]; then
        git clone --depth 1 --branch v1.6.44 https://github.com/pnggroup/libpng.git "$PNG_SRC"
    fi
    cmake -S "$PNG_SRC" -B "$PNG_SRC/build" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" \
        -DPNG_SHARED=ON -DPNG_STATIC=OFF -DPNG_TESTS=OFF
    cmake --build "$PNG_SRC/build" -j"$NPROC"
    cmake --install "$PNG_SRC/build"

    # libtiff
    local TIFF_SRC="$BUILD_DIR/libtiff"
    if [ ! -d "$TIFF_SRC" ]; then
        git clone --depth 1 --branch v4.7.0 https://gitlab.com/libtiff/libtiff.git "$TIFF_SRC"
    fi
    cmake -S "$TIFF_SRC" -B "$TIFF_SRC/build" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" \
        -DBUILD_SHARED_LIBS=ON -Dtiff-tests=OFF -Dtiff-tools=OFF -Dtiff-docs=OFF \
        -DCMAKE_PREFIX_PATH="$INSTALL_DIR"
    cmake --build "$TIFF_SRC/build" -j"$NPROC"
    cmake --install "$TIFF_SRC/build"

    # Vendor all image libs
    for name in jpeg png tiff; do
        local dst="$EXTERNAL_DIR/$name/macos"
        mkdir -p "$dst/lib" "$dst/include"
    done

    cp -a "$INSTALL_DIR"/lib/libjpeg*.dylib "$INSTALL_DIR"/lib/libturbojpeg*.dylib \
          "$EXTERNAL_DIR/jpeg/macos/lib/" 2>/dev/null || true
    cp -R "$INSTALL_DIR"/include/jpeglib.h "$INSTALL_DIR"/include/jconfig.h \
          "$INSTALL_DIR"/include/jmorecfg.h "$INSTALL_DIR"/include/jerror.h \
          "$INSTALL_DIR"/include/turbojpeg.h "$EXTERNAL_DIR/jpeg/macos/include/" 2>/dev/null || true

    cp -a "$INSTALL_DIR"/lib/libpng*.dylib "$EXTERNAL_DIR/png/macos/lib/" 2>/dev/null || true
    cp -R "$INSTALL_DIR"/include/png*.h "$EXTERNAL_DIR/png/macos/include/" 2>/dev/null || true

    cp -a "$INSTALL_DIR"/lib/libtiff*.dylib "$EXTERNAL_DIR/tiff/macos/lib/" 2>/dev/null || true
    cp -R "$INSTALL_DIR"/include/tiff*.h "$EXTERNAL_DIR/tiff/macos/include/" 2>/dev/null || true

    # Fix install names
    for dir in jpeg png tiff; do
        for dylib in "$EXTERNAL_DIR/$dir/macos/lib"/*.dylib; do
            [ -L "$dylib" ] && continue
            install_name_tool -id "@rpath/$(basename "$dylib")" "$dylib" 2>/dev/null || true
        done
    done

    echo "Image libraries: Done"
}

# =============================================================================
# 5. SoundTouch
# =============================================================================
build_soundtouch() {
    echo ""
    echo "============================================"
    echo "Building SoundTouch..."
    echo "============================================"

    local SRC="$BUILD_DIR/soundtouch"
    if [ ! -d "$SRC" ]; then
        git clone --depth 1 --branch 2.4.0 https://codeberg.org/soundtouch/soundtouch.git "$SRC"
    fi

    cmake -S "$SRC" -B "$SRC/build" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" \
        -DBUILD_SHARED_LIBS=ON

    cmake --build "$SRC/build" -j"$NPROC"
    cmake --install "$SRC/build"

    local dst="$EXTERNAL_DIR/soundtouch/macos"
    mkdir -p "$dst/lib" "$dst/include"
    cp -a "$INSTALL_DIR"/lib/libSoundTouch*.dylib "$dst/lib/" 2>/dev/null || true
    cp -R "$INSTALL_DIR"/include/soundtouch "$dst/include/" 2>/dev/null || true

    for dylib in "$dst"/lib/*.dylib; do
        [ -L "$dylib" ] && continue
        install_name_tool -id "@rpath/$(basename "$dylib")" "$dylib" 2>/dev/null || true
    done

    echo "SoundTouch: Done"
}

# =============================================================================
# Run all builds
# =============================================================================
echo ""
echo "Starting builds..."
echo ""

# FFmpeg codec dependencies (static — linked into FFmpeg dylibs)
build_x264
build_x265
build_dav1d
build_vpx
build_lame
build_opus
build_svtav1

# FFmpeg itself (shared dylibs, codec deps linked statically)
build_ffmpeg

# Other dependencies
build_openexr
build_image_libs
build_soundtouch
build_ocio

echo ""
echo "============================================"
echo "ALL DONE!"
echo ""
echo "Vendored dylibs in:"
echo "  $EXTERNAL_DIR/ffmpeg/macos/"
echo "  $EXTERNAL_DIR/ocio/macos/"
echo "  $EXTERNAL_DIR/openexr/macos/"
echo "  $EXTERNAL_DIR/jpeg/macos/"
echo "  $EXTERNAL_DIR/png/macos/"
echo "  $EXTERNAL_DIR/tiff/macos/"
echo "  $EXTERNAL_DIR/soundtouch/macos/"
echo ""
echo "Next: cmake -B build -G Xcode"
echo "============================================"
