# Dependencies Changelog

Per `dependencies.md` §6, every dependency bump is logged here: old → new
pin, reason, tested platforms, and anything to watch in production.

---

## 2026-06-24 — Windows FFmpeg: `n8.1-11-g75d37c499d` → `n8.1.2-20260624`

**Dependency:** FFmpeg (Windows GPL-Shared build from
[BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds))

**Old pin:** `ffmpeg-n8.1-11-g75d37c499d-win64-gpl-shared-8.1`
(BtbN release `autobuild-2026-04-30-13-44`, consumed via the
`BtbN.FFmpeg.GPL.Shared.8.1` winget package).

**New pin:** `n8.1.2-20260624` — BtbN rolling `latest` release asset
`ffmpeg-n8.1-latest-win64-gpl-shared-8.1.zip`, now **vendored in-tree** at
`external/ffmpeg-win64/` (gitignored; see below).

**Reason:** Security — **CVE-2026-8461 "PixelSmash"** (CVSS 8.8): a heap
out-of-bounds write in libavcodec's MagicYUV slice decoder, where the
frame allocator and decoder disagree on chroma plane height. A crafted
AVI/MKV/MOV can crash or (with a refined chain) RCE. Fixed upstream in
FFmpeg **8.1.2** (released 2026-06-17). The prior `n8.1-11` build predated
the fix and was vulnerable.

**Why not winget:** the `BtbN.FFmpeg.GPL.Shared.8.1` winget package was
still pinned to the vulnerable April build (`8.1-20260430`) with
`winget upgrade` reporting no update available. We consume BtbN's rolling
`latest` GitHub release zip directly instead.

**Why vendored + gitignored:** the build is ~200 MB and `avcodec-62.dll`
alone is ~98 MB. `main` auto-pushes to GitHub, which hard-blocks files
≥100 MB — committing the binaries risks breaking the push entirely on any
future size bump. Files live in the repo tree for the build to find but
are excluded via `.gitignore` (`external/ffmpeg-win64/`); re-fetch
instructions are in `dependencies.md` §2.

**Verification (uniongraphics box, 2026-06-24):**
- `ffmpeg -version` → `ffmpeg version n8.1.2-20260624`
- `ffmpeg -buildconf` confirms `--enable-vulkan`, `--enable-libshaderc`,
  `--enable-libplacebo` (the reason we use BtbN over vcpkg — ProRes Vulkan
  compute decode) and `--enable-gpl`.
- Library majors unchanged from the old build: **libavcodec 62 /
  libavutil 60 / libavformat 62 / libavfilter 11 / libavdevice 62 /
  libswresample 6 / libswscale 9** — matches the DLL names copied in
  `src/app/CMakeLists.txt`, so it's an ABI-clean drop-in (no code or
  DLL-list changes needed).
- `.pc` files are relocatable (`prefix=${pcfiledir}/../..`), so the tree
  resolves correctly from `external/ffmpeg-win64/`.

**CMake wiring changes:**
- `external/CMakeLists.txt`: default `QCV_BTBN_FFMPEG_DIR` changed from the
  winget cache path to `${CMAKE_SOURCE_DIR}/external/ffmpeg-win64`, now
  `FORCE`-set when empty/unset (the `windows-release` preset caches the
  var as `""` when `BTBN_FFMPEG_DIR` is unset, and a non-FORCE `set` can't
  override an existing empty cache entry). Fallback warning updated.
- `.gitignore`: added `external/ffmpeg-win64/`.

**Tested platforms:** Windows (verified the build's version + buildconf;
full configure/build + ProRes Vulkan-decode smoke test still recommended
before shipping). macOS unaffected — still resolves FFmpeg via Homebrew
pkg-config.

**Watch for in production:** confirm ProRes Vulkan compute decode still
works end-to-end after the bump (libplacebo/libshaderc are present, but
exercise an actual ProRes clip). Re-verify after any future BtbN refresh —
the rolling `latest` tag is not version-stamped in the asset name, so
always check `ffmpeg -version` after re-fetching.
