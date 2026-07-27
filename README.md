# QCView

**QCView** is a media player and reviewer for Windows and macOS, built for artists and post-production teams who need accurate, color-managed playback of video, EXR, and image sequences.

This is the Qt-based rebuild of the original QCView (Dear ImGui + GLFW + native graphics APIs). Architecture, decode path, color pipeline, and review tools are all carried forward; the UI shell is now Qt Quick / QML.

### Windows

<a href="https://apps.microsoft.com/detail/9p4z15p5g805?referrer=appbadge&mode=full" target="_blank" rel="noopener noreferrer">
  <img src="https://get.microsoft.com/images/en-us%20dark.svg" width="200"/>
</a>

### macOS

Signed and notarized `.dmg` available from [GitHub Releases](https://github.com/cbkow/QCView-Player/releases/latest). Requires macOS 13.0 (Ventura) or later on Apple Silicon.

---

## Key Features

### Playback
- Hardware-accelerated video decoding (ProRes, h.264, h.265) — Vulkan-video / FFmpeg hwaccel on Windows, VideoToolbox on macOS — with FFmpeg software fallback
- HDR10 (PQ / ST.2084) output on Windows via DXGI + DirectComposition; Extended Dynamic Range (EDR) on macOS
- Frame-accurate stepping with press-and-hold fast-forward / rewind
- Playlists with cross-clip seek, per-clip in/out trims, and hover thumbnail filmstrip
- Embedded timecode (DV / TimeCode / MXF) with selectable origin
- Audio playback (WASAPI on Windows, CoreAudio on macOS) with per-channel routing for multi-stream broadcast deliveries
- A/V sync offset trim, per-OS defaults, separate dual-view value

### Image Sequences
- [OpenEXR](https://openexr.com/) with multichannel and multipart support
- TIFF, PNG, JPEG, and JPEG-2000 (HTJ2K via OpenJPH) sequence playback

### Color
- Live [OCIO](https://opencolorio.org/) color correction with a node-based interface
- Bundled OCIO configs: ACES 2.0, ACES 1.3, Blender 5.2, Blender 5.1
- Screenshots and notes exports with OCIO transforms applied

### Review Tools
- Annotation and notes system with brush, line smoothing, screenshot illustrations, and Markdown / HTML / PDF / DOCX export
- Dual-view comparison (side-by-side, split-wipe, drag-the-seam), difference mode, with independent A / B controls
- Title-safety guides for broadcast and social-media deliverables
- Live background switching for alpha-channel review

### Project Support
- Custom project files (`.qcvproj`)
- `qcview://` URI scheme for sharing file paths

### Platform Details

|                  | Windows                                | macOS                          |
|------------------|----------------------------------------|--------------------------------|
| **UI**           | Qt 6.11 Quick / QML                    | Qt 6.11 Quick / QML            |
| **GPU**          | Direct3D 11 + DirectComposition        | Metal                          |
| **HW Decode**    | Vulkan-video (FFmpeg hwaccel)          | VideoToolbox                   |
| **Audio**        | WASAPI                                 | CoreAudio                      |
| **HDR**          | HDR10 via DXGI + multi-visual DComp    | EDR (Extended Dynamic Range)   |
| **Packaging**    | Microsoft Store / signed sideload `.msix` | Signed `.dmg`                  |

## Manual and documentation

- [Docs](https://qcview.app/)
- [Acknowledgments](Acknowledgments.md)
- [Third-Party Notices](LICENSES/THIRD_PARTY_NOTICES.txt)
- [Privacy Policy](PRIVACY_POLICY.md)

## Building from source

Build instructions live in [`BUILDING.md`](BUILDING.md). Short version: install Qt 6.11 open-source, vcpkg, and a C++20 toolchain; configure with `cmake -B build-release -G Ninja -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DCMAKE_PREFIX_PATH=<Qt>/msvc2022_64`; build with `cmake --build build-release --target qcview --config Release`.

## License

QCView is free software released under the **GNU General Public License version 3**. See [LICENSE](LICENSE) for the full text. Source code, including build scripts and third-party-license notices, is available at https://github.com/cbkow/QCView-Player.

Copyright © 2025–2026 cbkow.
