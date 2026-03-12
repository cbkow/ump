# QCView

**QCView** is a professional media player and reviewer for Windows and Linux, built for artists, colorists, and post-production teams who need accurate, color-managed playback of video, EXR, and image sequences.

### Windows

<a href="https://apps.microsoft.com/detail/9p4z15p5g805?referrer=appbadge&mode=full" target="_blank" rel="noopener noreferrer">
  <img src="https://get.microsoft.com/images/en-us%20dark.svg" width="200"/>
</a>

### Linux (Experimental)

`.deb` and `.rpm` packages are available from [GitHub Releases](https://github.com/cbkow/QCView-Player/releases). The Linux build uses Vulkan for rendering and has been tested on Kubuntu with KDE Plasma 6 (Wayland).

---

## Key Features

### Playback
- Hardware-accelerated video decoding — D3D11VA on Windows, VA-API on Linux — with FFmpeg software fallback
- HDR10 output (PQ/ST.2084, BT.2020) with adjustable interface tonemapping target nits
- Frame-accurate stepping with press-and-hold fast forward/rewind
- Playlists and hover thumbnails with filmstrip view
- Embedded timecode support
- Audio playback with tempo-preserving time stretch (WASAPI on Windows, PipeWire on Linux)

### Image Sequences
- [OpenEXR](https://openexr.com/en/latest/) with multichannel and multipart support
- TIFF, PNG, and JPEG sequence playback
- Automatic pipeline mode selection based on bit depth

### Color
- Live [OCIO](https://opencolorio.org/) color correction with a node-based interface
- Multiple pipeline modes: 8-bit, 12-bit, 16-bit float
- Screenshots with OCIO transforms applied

### Review Tools
- Annotation and notes system with drawing, line smoothing, and screenshot illustrations
- Dual-view comparison mode
- Title-safety guides for broadcast and social-media deliverables
- Live background switching for alpha-channel review
- [Frame.io](https://frame.io) comment integration

### Project Support
- Adobe project metadata import
- Custom project files (.qcv, .qcvproj, .qcvexr)
- `qcview://` URI scheme for sharing file paths

### Platform Details

| | Windows | Linux (Experimental) |
|---|---|---|
| **GPU** | Direct3D 11 | Vulkan |
| **HW Decode** | D3D11VA | VA-API + DMA-BUF |
| **Audio** | WASAPI | PipeWire |
| **HDR** | Automatic via DXGI | Toggleable in-app (requires display HDR enabled) |
| **Packaging** | Microsoft Store / .exe installer | .deb / .rpm |
## Manual and documentation

- [Docs](https://qcview.app/) 
- [Acknowledgments](https://github.com/cbkow/QCView-Player/blob/main/Acknowledgments.md)
- [Third-Party Notices](https://github.com/cbkow/QCView-Player/blob/main/LICENSES/THIRD_PARTY_NOTICES.txt)
- [Privacy Policy](https://github.com/cbkow/QCView-Player/blob/main/PRIVACY_POLICY.md)

QCView is open source under the GPL v3 license.