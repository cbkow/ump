# Dependencies — Pin Manifest

Single source of truth for every external dependency used by
QCView-Player-QT. Every version below is **pinned** — `CMakeLists.txt`
references this file's pin choices via `FetchContent_Declare(... GIT_TAG <pin>)`
or system package version constraints.

## Pin policy

- **Every dep is pinned** to a specific tag or commit hash. Never
  `master` / `main` / `HEAD`. Reproducible builds are non-negotiable.
- **Bump cadence**: deliberate, scheduled. No "latest" upgrades from CI.
  Quarterly review at minimum; security/CVE fixes immediately.
- **Each bump documents**: old version, new version, reason, tested
  platforms. Append to a `dependencies-changelog.md` per bump.
- **`OCIO library version` ceiling** (Guide 05 §12) is derived from the
  pinned OCIO version; ship-time supported config profile listed below.

## Categories

1. **Qt SDK** — installed via Qt's installer or system package; not
   vendored.
2. **FetchContent** — fetched at configure time, built in-tree, pinned
   by Git tag or commit hash.
3. **System libraries** — fixed minimum version requirements; resolved
   via platform package managers or vendored at packaging time.
4. **Bundled assets** — files vendored in the repo (OCIO configs,
   safety-guide SVGs, fonts).

---

## 1. Qt SDK

| Dep | Pin | Source | License | Notes |
|---|---|---|---|---|
| **Qt** | **6.11.0** (latest 6.11.x patch at build) | Qt installer or `qt6` system package | LGPLv3 / commercial | Confirmed installed at `/Applications/Qt/6.11.0` on dev machine. Modules required: `Core`, `Gui`, `Quick`, `QuickControls2`, `Network`, `Widgets` (for QFileDialog only), `Concurrent`, `Multimedia` (audio output only — video pipeline is custom QRhi). |

Required Qt modules (explicit list):

- `Qt6::Core`
- `Qt6::Gui` — for QRhi
- `Qt6::Quick` — Qt Quick scene graph
- `Qt6::QuickControls2` — UI controls
- `Qt6::ShaderTools` — shader compilation pipeline (qsb)
- `Qt6::Network` — Frame.io HTTP client
- `Qt6::Concurrent` — QFuture for worker threads
- `Qt6::Multimedia` — `QAudioSink` for audio output

Build flag: `QT_NO_DEPRECATED_BEFORE=0x060B00` (Qt 6.11) — locks API
surface against accidental use of pre-6.11 deprecations.

---

## 2. FetchContent dependencies

Each entry includes the `FetchContent_Declare()` block to copy into
`external/CMakeLists.txt`.

### FFmpeg

```cmake
FetchContent_Declare(
    ffmpeg
    GIT_REPOSITORY https://github.com/FFmpeg/FFmpeg.git
    GIT_TAG n8.1.2                              # release tag, ABI-stable
    GIT_SHALLOW TRUE
)
```

| | Value |
|---|---|
| **Pin** | **n8.1.2** (8.1 release branch) — both **macOS (self-built, in-tree at `external/install/`)** and **Windows (`n8.1.2-20260624`)** |
| **License** | **GPL v3** on both shipped platforms (macOS self-built `--enable-gpl --enable-version3`; Windows BtbN GPL-Shared `--enable-gpl`) |
| **Verified** | macOS self-built `n8.1.2` (libavcodec 62.28.102 / libavutil 60.26.102 / libavformat 62.12.102) loads + app links/builds clean; Windows build version-checked |
| **Build flags** | `--enable-videotoolbox --enable-vulkan --enable-libdav1d --enable-libsvtav1 --enable-libopus --disable-x86asm-on-cross` |
| **Hwaccels needed** | `videotoolbox` (macOS), `vulkan` (Win+Linux) |
| **Codecs needed** | H.264, H.265, AV1, ProRes, DNxHR, JPEG (image-sequence fallback) |

Note: building FFmpeg from source via FetchContent is a **slow build**
(~10–15 min on first config). The dev workflow may prefer Homebrew /
distro packages; CI and shipped builds use FetchContent for
reproducibility. CMake handles both via a `QCVIEW_USE_SYSTEM_FFMPEG`
toggle.

#### Windows: BtbN GPL-Shared build (vendored, in-tree)

vcpkg's FFmpeg 8.1 ships **without libplacebo + libshaderc**, so the
ProRes Vulkan compute decoder is unavailable. Windows therefore uses a
[BtbN GPL-Shared 8.1-branch build](https://github.com/BtbN/FFmpeg-Builds)
with both baked into the DLLs (same major API — libavcodec 62 /
libavutil 60 / libavformat 62 — so it ABI-matches vcpkg's headers and is
a drop-in for the DLL-copy step in `src/app/CMakeLists.txt`).

- **Vendored at** `external/ffmpeg-win64/` — the default
  `QCV_BTBN_FFMPEG_DIR`. **Gitignored** (~200 MB; `avcodec-62.dll` alone
  is ~98 MB, and `main` auto-pushes to GitHub which hard-blocks
  ≥100 MB files). The `.pc` files are relocatable
  (`prefix=${pcfiledir}/../..`), so the tree works from any location.
- **Current build:** `n8.1.2-20260624` — **patches CVE-2026-8461
  "PixelSmash"** (heap OOB write in the MagicYUV decoder; fixed upstream
  in FFmpeg 8.1.2, 2026-06-17). Supersedes the prior `n8.1-11-g75d37c499d`
  winget build, which was vulnerable.
- **Why not winget:** the `BtbN.FFmpeg.GPL.Shared.8.1` winget package was
  still pinned to the vulnerable April build (`8.1-20260430`) with no
  upgrade available, so we consume BtbN's rolling `latest` release zip
  directly instead.
- **Override** with the `BTBN_FFMPEG_DIR` env var (bound by the
  `windows-release` preset) or `-DQCV_BTBN_FFMPEG_DIR=<path>`.

**Re-fetch / refresh** (run from repo root, replacing the contents of
`external/ffmpeg-win64/`):

```powershell
$zip = "$env:TEMP\ffmpeg-81.zip"
Invoke-WebRequest -Uri "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-gpl-shared-8.1.zip" -OutFile $zip
Expand-Archive $zip -DestinationPath "$env:TEMP\ffmpeg-extract" -Force
Remove-Item external\ffmpeg-win64 -Recurse -Force -ErrorAction SilentlyContinue
Move-Item "$env:TEMP\ffmpeg-extract\ffmpeg-n8.1-latest-win64-gpl-shared-8.1" external\ffmpeg-win64
# Verify before trusting: expect 8.1.2+ and all three Vulkan deps.
external\ffmpeg-win64\bin\ffmpeg.exe -hide_banner -version | Select-Object -First 1
external\ffmpeg-win64\bin\ffmpeg.exe -hide_banner -buildconf | Select-String "libplacebo|libshaderc|vulkan"
```

#### macOS: self-built GPL v3 (vendored, in-tree)

macOS does **not** use Homebrew for the shipped binary — it links a
**self-built arm64 FFmpeg** installed into `external/install/` (dylibs in
`external/install/lib`, headers in `external/install/include`). CMake
finds it because `CMakeLists.txt` prepends `external/install/lib/pkgconfig`
to `PKG_CONFIG_PATH` (see `QCV_VENDOR_PREFIX`). The whole `external/install/`
tree is **gitignored**, so the binary is never committed and must be
rebuilt from source on each dev machine / version bump.

- **Current build:** `n8.1.2` — **patches CVE-2026-8461 "PixelSmash"**
  (heap OOB write in the MagicYUV decoder; fixed upstream 2026-06-17).
  Sonames unchanged from the prior `n8.1` build (libavcodec 62 /
  libavutil 60 / libavformat 62), so it's an ABI-clean drop-in.
- **Codec deps** are pre-built **static** archives already in
  `external/install/lib` (`libx264/libx265/libdav1d/libvpx/libmp3lame/`
  `libopus/libSvtAv1Enc.a`) with matching `.pc` files in
  `external/install/lib/pkgconfig` (note: `x265.pc` is vendored there too,
  since FFmpeg's configure requires pkg-config for libx265).

**Re-build** (run from repo root; reuses the in-tree static codec deps,
overwrites the `libav*`/`libsw*` dylibs + headers in `external/install/`):

```bash
INSTALL="$PWD/external/install"
git clone --depth 1 --branch n8.1.2 https://github.com/FFmpeg/FFmpeg.git external/source/ffmpeg
cd external/source/ffmpeg
PKG_CONFIG_PATH="$INSTALL/lib/pkgconfig" ./configure \
  --prefix="$INSTALL" --enable-shared --disable-static --enable-pthreads \
  --enable-videotoolbox --enable-audiotoolbox \
  --enable-hwaccel=h264_videotoolbox --enable-hwaccel=hevc_videotoolbox \
  --enable-hwaccel=prores_videotoolbox \
  --enable-gpl --enable-version3 \
  --enable-libx264 --enable-libx265 --enable-libdav1d --enable-libvpx \
  --enable-libmp3lame --enable-libopus --enable-libsvtav1 \
  --disable-programs --disable-doc --disable-debug --arch=arm64 \
  --extra-cflags="-I$INSTALL/include -mmacosx-version-min=13.0 -arch arm64" \
  --extra-ldflags="-L$INSTALL/lib -mmacosx-version-min=13.0 -arch arm64" \
  --extra-libs=-lc++
make -j"$(sysctl -n hw.ncpu)" && make install
# make install leaves the old versioned dylibs behind — prune stale ones:
#   ls external/install/lib/libav*.*.*.dylib  (keep only the newest micro)
# Verify: cat external/install/include/libavutil/ffversion.h  → n8.1.2
```

**NOTE — no `--enable-nonfree`:** the previous build carried a vestigial
`--enable-nonfree` (no GPL-incompatible codec was ever linked). It was
dropped in this rebuild; the binary now matches the
`LICENSES/THIRD_PARTY_NOTICES.txt` macOS entry exactly.

### OCIO (OpenColorIO)

```cmake
FetchContent_Declare(
    ocio
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/OpenColorIO.git
    GIT_TAG v2.5.0                              # latest 2.5.x stable
    GIT_SHALLOW TRUE
)
```

| | Value |
|---|---|
| **Pin** | **v2.5.0** (bump to latest 2.5.x patch on schedule) |
| **License** | BSD-3-Clause |
| **Supported config profile** | Up to **2.5** (matches Guide 05 §12 / D17) |
| **Build flags** | `OCIO_BUILD_APPS=OFF`, `OCIO_BUILD_TESTS=OFF`, `OCIO_BUILD_PYTHON=OFF`, `OCIO_BUILD_GPU_TESTS=OFF` |
| **Required for** | Color management (Guide 05), OCIO snapshot in transcode (Guide 11) |

### OpenEXR

```cmake
FetchContent_Declare(
    openexr
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/openexr.git
    GIT_TAG v3.4.7                              # current local Homebrew version
    GIT_SHALLOW TRUE
)
```

| | Value |
|---|---|
| **Pin** | **v3.4.7** |
| **License** | BSD-3-Clause |
| **Verified** | Local install 3.4.7 confirmed |
| **Build flags** | `OPENEXR_INSTALL=OFF`, `OPENEXR_BUILD_TOOLS=OFF`, `BUILD_TESTING=OFF` |
| **Used by** | EXR sequence loader (Guide 02 §4), EXR layer detection (Guide 07 §5) |

### ink-stroke-modeler (Google)

```cmake
FetchContent_Declare(
    ink_stroke_modeler
    GIT_REPOSITORY https://github.com/google/ink-stroke-modeler.git
    # Pinned commit — current app tracks main, which is the bug we're fixing
    GIT_TAG <commit-hash-to-fill-on-first-build>
    GIT_SHALLOW FALSE                            # need full history for pin to resolve
)
```

| | Value |
|---|---|
| **Pin** | **commit hash, TBD at first build** |
| **License** | Apache 2.0 |
| **Verified** | Library is mature; specific commit verified at port time |
| **Caveat** | No regular release tags as of latest check. Pin to whatever HEAD is on day 1 of integration; document hash in this file's revision history. |
| **Build flags** | `INK_STROKE_MODELER_BUILD_TESTING=OFF`, `INK_STROKE_MODELER_FIND_DEPENDENCIES=OFF`, `INK_STROKE_MODELER_ENABLE_INSTALL=OFF` |
| **Used by** | Annotation stroke smoothing (Guide 04 §3) |

**Action item**: at first integration, fetch the ink-stroke-modeler
repo, pick `HEAD`, replace `<commit-hash-to-fill-on-first-build>`
with the actual hash, and update this entry.

### SoundTouch (Olli Parviainen)

```cmake
FetchContent_Declare(
    soundtouch
    GIT_REPOSITORY https://codeberg.org/soundtouch/soundtouch.git
    GIT_TAG 0047e0b1ecfceb041348579119bf79b73a322a3a   # tag 2.4.1
    GIT_SHALLOW FALSE
)
```

| | Value |
|---|---|
| **Pin** | **commit 0047e0b1 (= release tag 2.4.1, 2026 vintage)** |
| **License** | LGPL v2.1 |
| **License note** | Statically linked. LGPL-2.1 compliance is satisfied the same way the app's GPL FFmpeg build already forces: the whole work is distributed under GPL-compatible terms and source offers cover relinking. Notice in `LICENSES/SoundTouch-LICENSE.txt` + THIRD_PARTY_NOTICES §. |
| **Build flags** | `INTEGER_SAMPLES=OFF` (float pipeline), `SOUNDSTRETCH=OFF` (no CLI tool), `SOUNDTOUCH_DLL=OFF` |
| **Used by** | TempoStage (`src/audio/tempo_stage.{h,cpp}`) — constant-pitch WSOLA time-stretch for review-speed playback (0.5x–2x). Both decoder shapes route through it on their decode threads; 1x bypasses. |

### Abseil (transitive from ink-stroke-modeler)

```cmake
FetchContent_Declare(
    abseil
    GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
    GIT_TAG <matching-commit>                   # whatever ink-stroke-modeler uses
    GIT_SHALLOW TRUE
)
```

| | Value |
|---|---|
| **Pin** | **matched to ink-stroke-modeler's pinned Abseil commit** |
| **License** | Apache 2.0 |
| **Note** | ink-stroke-modeler usually fetches its own Abseil; we pin explicitly to avoid version drift. |
| **Build flags** | `ABSL_ENABLE_INSTALL=OFF`, `ABSL_PROPAGATE_CXX_STD=ON` |

### QtKeychain

```cmake
FetchContent_Declare(
    qtkeychain
    GIT_REPOSITORY https://github.com/frankosterfeld/qtkeychain.git
    GIT_TAG v0.14.3                             # latest stable as of Q1 2026
    GIT_SHALLOW TRUE
)
```

| | Value |
|---|---|
| **Pin** | **v0.14.3** |
| **License** | BSD-3-Clause |
| **Used by** | Frame.io API token storage (Guide 04 D8 / Guide 07) |
| **Build flags** | `BUILD_WITH_QT6=ON`, `BUILD_TEST_APPLICATION=OFF`, `BUILD_TRANSLATIONS=OFF` |
| **Platform hooks** | macOS Keychain Services, Windows Credential Manager, Linux libsecret / KDE KWallet — selected automatically based on platform |

### OpenTimelineIO (OTIO)

```cmake
FetchContent_Declare(
    opentimelineio
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/OpenTimelineIO.git
    GIT_TAG v0.18.0                             # latest stable, bumped from old app's v0.17.0
    GIT_SHALLOW TRUE
)
```

| | Value |
|---|---|
| **Pin** | **v0.18.0** |
| **License** | Apache 2.0 |
| **Used by** | Timeline data model across all source modes (Guide 02 §7), playlist serialization (Guide 09 §11) |
| **Build flags** | `OTIO_PYTHON_INSTALL=OFF`, `OTIO_INSTALL_PYTHON_MODULES=OFF`, `OTIO_FIND_IMATH=OFF` |
| **Note** | Bumped from old app's v0.17.0; verify schema compatibility with old `.qcvproj` files during migration testing |

### glslang (Khronos)

```cmake
FetchContent_Declare(
    glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG 14.3.0                              # tagged Khronos release
    GIT_SHALLOW TRUE
)
```

| | Value |
|---|---|
| **Pin** | **14.3.0** |
| **License** | BSD-3-Clause + Apache 2.0 (mixed) |
| **Used by** | Runtime SPIR-V compilation for OCIO-generated GLSL shaders on Vulkan backend (Guide 05 §11) |
| **Note** | Vulkan-side only; Metal backend skips this entirely (uses OCIO's MSL output directly). Build a stub on macOS to keep CMakeLists uniform. |

### nlohmann/json

```cmake
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3                             # current stable
    GIT_SHALLOW TRUE
)
```

| | Value |
|---|---|
| **Pin** | **v3.11.3** |
| **License** | MIT |
| **Used by** | `.qcvproj` v2 schema (Guide 07 §9), shortcuts.json (Guide 02), color-presets.json (Guide 05), transcode-queue.json (Guide 11), every other JSON-backed settings file |
| **Build flags** | `JSON_BuildTests=OFF`, `JSON_Install=OFF` |
| **Alternative** | Qt 6 has built-in JSON support (`QJsonDocument`). Decision: use Qt's JSON for QML-bound model data; nlohmann for fixed-schema files where a real C++ type is preferable. |

---

## 3. System libraries

These are fixed minimum versions, resolved via platform package
managers on dev / CI; vendored at packaging time for distributable
binaries.

### Image format libraries

| Lib | Min version | Local | License | Used by |
|---|---|---|---|---|
| **libpng** | 1.6.0+ | 1.6.55 ✓ | libpng license (BSD-like) | PNG sequence loader (Guide 02 §4), screenshot encoder fallback if Qt's PNG path doesn't fit |
| **libjpeg-turbo** | 3.0.0+ | 3.1.3 ✓ | BSD-3-Clause | JPEG sequence loader |
| **libtiff** | 4.5.0+ | 4.7.1_1 ✓ | libtiff license (BSD-like) | TIFF sequence loader |

Not vendored as FetchContent because:
- Universally available via system packages.
- Qt 6 already links these for `QImage`.
- ABI is stable across patch versions.

### Vulkan SDK (Win + Linux only)

| Component | Pin | Notes |
|---|---|---|
| **Vulkan SDK** | 1.3.280+ | LunarG distribution; required for Vulkan headers + validation layers in dev builds |
| **VK_KHR_video_decode_*** extensions | required at runtime | mature on NVIDIA + AMD + recent Intel as of early 2026 |

macOS uses Metal directly; no Vulkan SDK needed.

### Audio output

| Platform | API | Notes |
|---|---|---|
| **macOS** | CoreAudio (via `QAudioSink`) | system framework; Qt wraps |
| **Windows** | WASAPI (via `QAudioSink`) | system; Qt wraps |
| **Linux** | PipeWire / PulseAudio (via `QAudioSink`) | Qt selects automatically |

No additional pinning needed — Qt's `QAudioSink` is the abstraction.

---

## 4. Bundled assets (in repo)

### OCIO configs (`assets/OCIO/`)

| Config | Source | License | Notes |
|---|---|---|---|
| **Blender 5.1** | Blender repo (release v5.1) | CC0 | Default config (Guide 05 §5). Linear sRGB EDR / Linear P3 EDR displays for macOS HDR. |
| **Blender 4.5** | Blender repo (release v4.5) | CC0 | Fallback config |
| **ACES 2.0** | OCIO Configs project (`studio-config-all-views-v4.0.0_aces-v2.0_ocio-v2.5`) | CC0 | **Will be patched at port time** to add EDR display entries (Guide 05 D12) |
| **ACES 1.3** | OCIO Configs project (`studio-config-v1.0.0_aces-v1.3_ocio-v2.1`) | CC0 | Legacy compatibility |

### Safety guide SVGs (`assets/safety/`)

12 SVG files ported verbatim from the current app (Guide 10 §3):
`16x9.svg`, `Youtube_16x9_Masthead.svg`, `Youtube_16x9.svg`,
`TikTok_9x16.svg`, `Youtube_9x16.svg`, `Meta_Reels_9x16.svg`,
`Meta_Stories_9x16.svg`, `Pinterest_9x16.svg`,
`Samsung_9x16_Safety.svg`, `Snapchat_9x16_unofficial.svg`,
`Youtube_1x1.svg`, `Pinterest_PremiumSpotlight_1x1.svg`.

Plus a `custom/` subdirectory for user-imported SVGs (Guide 10 D6).

### Exiftool binary (`assets/exiftool/`)

Bundled as in current app for Adobe XMP metadata extraction
(Guide 07 §6). Per-platform binaries:

| Platform | Source | License |
|---|---|---|
| macOS | exiftool.org/macOS_distribution | Artistic 2.0 / GPL (dual) |
| Windows | exiftool.org/Windows_distribution | same |
| Linux | bundled or system-`exiftool` | same |

Pin the version once per shipped binary; update annually.

### Fonts (`assets/fonts/`)

| Font | License | Used for |
|---|---|---|
| **Inter** | OFL | UI text, PDF export body |
| **JetBrains Mono** | OFL | code blocks in markdown, monospace timecode |
| **Material Symbols** (icon font) | Apache 2.0 | icons throughout the UI |

Stable; no version pin needed beyond the asset files themselves.

---

## 5. What we explicitly DON'T depend on

Sometimes documenting absence matters as much as documenting presence.

| Not a dep | Why not | Replacement |
|---|---|---|
| **libharu** | Used by current app for PDF export | `QPdfWriter` + `QTextDocument` (Guide 04 §10) |
| **md4c / cmark-gfm** | Custom markdown parsers | Qt 6 built-in `MarkdownDialectGitHub` (Guide 04 D11) |
| **stb_image_write** | PNG encoding | `QImage::save()` (Guide 04 §8) |
| **NanoVG** | Per-platform annotation rendering | Unified QRhi stroke pass (Guide 04 D5) |
| **GLFW** | Window management | Qt's `QQuickWindow` |
| **GLEW / GLAD** | OpenGL loader | We don't use OpenGL (Guide 01 D4) |
| **D3D11 SDK** | Direct3D 11 | Vulkan via QRhi (Guide 01 D3) |
| **D3D12 SDK** | Direct3D 12 | Same |
| **MoltenVK** | Vulkan-on-Metal translation | Native Metal via QRhi (Guide 01 D3) |
| **imnodes** | Node graph editor | Slot-machine layout (Guide 05 D5) |
| **NativeFileDialog (NFD)** | Native file dialogs | Qt's `QFileDialog` |
| **stb_image** | Image decode | Qt + libpng/libjpeg/libtiff |
| **ImGui + ImPlot** | UI framework | Qt Quick |

---

## 6. Bump procedure

When bumping any dep:

1. Pick the new tag/commit.
2. Update this file's pin entry. Bump the date in §0 below.
3. Update `external/CMakeLists.txt` to match.
4. Run a full CMake configure + clean build on macOS, Windows, Linux.
5. Run the test matrix (TBD — at minimum: open old `.qcvproj`, transcode
   one ProRes clip, draw an annotation, save and reload).
6. Append to `dependencies-changelog.md`:
   - Date
   - Dep name + old pin → new pin
   - Reason for bump
   - Tested platforms
   - Anything to watch for in production

If the bump touches OCIO's profile version, update Guide 05 §12's
"supported config profile" entry too.

---

## 7. Quick-reference summary table

| Dep | Pin | Source | Category |
|---|---|---|---|
| Qt | 6.11.0 | installer | SDK |
| FFmpeg | n8.1.2 (macOS self-built; Win: `n8.1.2-20260624`) | self-built / BtbN, both vendored in-tree | core |
| OCIO | v2.5.0 | FetchContent | core |
| OpenEXR | v3.4.7 | FetchContent | core |
| ink-stroke-modeler | (commit TBD) | FetchContent | core |
| Abseil | (matched) | FetchContent | core (transitive) |
| QtKeychain | v0.14.3 | FetchContent | secure storage |
| OpenTimelineIO | v0.18.0 | FetchContent | timeline |
| glslang | 14.3.0 | FetchContent | shader compile |
| nlohmann/json | v3.11.3 | FetchContent | JSON |
| libpng | 1.6.0+ | system | image |
| libjpeg-turbo | 3.0.0+ | system | image |
| libtiff | 4.5.0+ | system | image |
| Vulkan SDK | 1.3.280+ | platform | Win+Linux only |

---

## 0. Revision history

| Date | Change | By |
|---|---|---|
| 2026-04-24 | Initial pin manifest from port plan | Plan + Week-0 verification |
| 2026-06-24 | Windows FFmpeg: BtbN GPL-Shared `n8.1-11-g75d37c499d` → `n8.1.2-20260624` (vendored in-tree at `external/ffmpeg-win64/`, gitignored). Security fix for CVE-2026-8461 "PixelSmash". See `dependencies-changelog.md`. | Chris |
| 2026-06-24 | macOS FFmpeg: self-built `n8.1` → `n8.1.2` (rebuilt in-tree at `external/install/`, gitignored). Same CVE-2026-8461 fix; sonames unchanged (62/60/62), ABI-clean drop-in. Also dropped vestigial `--enable-nonfree`. See `dependencies-changelog.md`. | Chris |

(Append future bumps here.)
