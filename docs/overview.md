---
title: Technical Overview
permalink: /technical-overview/
nav_order: 3
---

# Technical Overview

## Rendering Architecture

u.m.p. uses a layered GPU pipeline that flows from source media through decode, caching, color processing, and finally to display. Everything passes through the OCIO color pipeline before reaching the screen or being exported to files.

```
    ┌─────────────────────────────────────────────────────────────────────────┐
    │                              SOURCE MEDIA                               │
    └─────────────────────────────────────────────────────────────────────────┘
                │                                        │
                │                                        │
        ┌───────▼───────┐                        ┌───────▼───────┐
        │  VIDEO FILES  │                        │    IMAGES     │
        │  .mp4 .mov    │                        │  .exr .tiff   │
        │  .mxf  etc    │                        │  .png .jpg    │
        └───────┬───────┘                        └───────┬───────┘
                │                                        │
                │                                        │
    ┌─────────────────────────────────────────────────────────────────────────┐
    │                              DECODE LAYER                               │
    └─────────────────────────────────────────────────────────────────────────┘
                │                                        │
        ┌───────▼────────────────────┐           ┌───────▼───────────────────┐
        │   D3D11VideoDecoder        │           │  ImageSequenceDecoder     │
        │   src/player/              │           │  src/player/              │
        │                            │           │                           │
        │  ┌──────────┐ ┌──────────┐ │           │  ┌─────────────────────┐  │
        │  │ D3D11VA  │ │  FFmpeg  │ │           │  │     LOADERS         │  │
        │  │ Hardware │ │ Software │ │           │  │  ┌───────────────┐  │  │
        │  │  Decode  │ │  Decode  │ │           │  │  │ EXRImageLoader│  │  │
        │  └────┬─────┘ └────┬─────┘ │           │  │  │ TIFFLoader    │  │  │
        │       │            │       │           │  │  │ PNGLoader     │  │  │
        │       └─────┬──────┘       │           │  │  │ JPEGLoader    │  │  │
        │             │              │           │  │  └───────────────┘  │  │
        │      ┌──────▼──────┐       │           │  └──────────┬──────────┘  │
        │      │ YUVTextures │       │           │             │             │
        │      │  (NV12/P010)│       │           │      ┌──────▼──────┐      │
        │      │   Y + UV    │       │           │      │  PixelData  │      │
        │      └──────┬──────┘       │           │      │  (CPU RAM)  │      │
        │             │              │           │      └┬────────────┘      │
        └─────────────┼──────────────┘           └───────┼───────────────────┘
                      │                                  │
                      │                                  │
    ┌─────────────────────────────────────────────────────────────────────────┐
    │                              CACHE LAYER                                │
    └─────────────────────────────────────────────────────────────────────────┘
                      │                                 │
        ┌─────────────▼─────────────────────────────────▼─────────────────────┐
        │                      CacheWindowEngine                              │
        │                   src/timeline/cache_window_engine.h                │
        │                                                                     │
        │    ┌─────────────────────────────────────────────────────────┐      │
        │    │              RING BUFFER (Circular Cache)               │      │
        │    │                                                         │      │
        │    │         [Behind] ◄─── [CURRENT] ───► [Ahead]            │      │
        │    │                           ▲                             │      │
        │    │                           │                             │      │
        │    │                    Playhead Position                    │      │
        │    │                                                         │      │
        │    │           Priority: Current > Ahead > Behind            │      │
        │    │            Eviction: Outside window removed             │      │
        │    └─────────────────────────────────────────────────────────┘      │
        │                                                                     │
        │    FrameCache (Video)          TimelineCache (Composite)            │
        │    - max_cache_seconds         - Multi-decoder coordination         │
        │    - Background extraction     - Timeline → Source mapping          │
        └─────────────────────────────────┬───────────────────────────────────┘
                                          │
                                          │
    ┌─────────────────────────────────────────────────────────────────────────┐
    │                              COMPOSE LAYER                              │
    └─────────────────────────────────────────────────────────────────────────┘
                                          │
                    ┌─────────────────────┴─────────────────────┐
                    │                                           │
            ┌───────▼───────┐                         ┌─────────▼─────────┐
            │  SINGLE VIEW  │                         │    DUAL VIEW      │
            │               │                         │                   │
            │  One source   │                         │  D3D11Dual        │
            │  texture      │                         │  Compositor       │
            │               │                         │                   │
            └───────┬───────┘                         │   ┌─────┬─────┐   │
                    │                                 │   │ L   │  R  │   │
                    │                                 │   │ SRV │ SRV │   │
                    │                                 │   └──┬──┴──┬──┘   │
                    │                                 │      │     │      │
                    │                                 │   ┌──▼─────▼──┐   │
                    │                                 │   │ Composite │   │
                    │                                 │   │  RGBA16F  │   │
                    │                                 │   └─────┬─────┘   │
                    │                                 └─────────┼─────────┘
                    │                                           │
                    └──────────────────┬────────────────────────┘
                                       │
                                       │
    ┌─────────────────────────────────────────────────────────────────────────┐
    │                         GPU CONVERSION LAYER                            │
    └─────────────────────────────────────────────────────────────────────────┘
                                       │
                        ┌──────────────▼──────────────┐
                        │     D3D11YUVRenderer        │
                        │     src/gpu/                │
                        │                             │
                        │   ┌─────────────────────┐   │
                        │   │   HLSL Shaders      │   │
                        │   │                     │   │
                        │   │  • BT.709 matrix    │   │
                        │   │  • BT.2020 matrix   │   │
                        │   │  • PQ EOTF (HDR)    │   │
                        │   │  • Range expand     │   │
                        │   └─────────┬───────────┘   │
                        │             │               │
                        │   Input:  Y+UV (NV12/P010)  │
                        │   Output: RGBA16F linear    │
                        └─────────────┬───────────────┘
                                      │
                                      │
    ┌─────────────────────────────────────────────────────────────────────────┐
    │                           INTEROP LAYER                                 │
    └─────────────────────────────────────────────────────────────────────────┘
                                      │
                       ┌──────────────▼───────────────┐
                       │    D3D11VideoInterop         │
                       │    src/gpu/                  │
                       │                              │
                       │  ┌────────────────────────┐  │
                       │  │   TRIPLE BUFFERING     │  │
                       │  │                        │  │
                       │  │ ┌─────┐ ┌─────┐ ┌─────┐│  │
                       │  │ │ B0  │ │ B1  │ │ B2  ││  │
                       │  │ └──┬──┘ └──┬──┘ └──┬──┘│  │
                       │  │    │       │       │   │  │
                       │  │  D3D11   D3D11   D3D11 │  │
                       │  │  Tex     Tex     Tex   │  │
                       │  │    ↕       ↕       ↕   │  │
                       │  │   GL      GL      GL   │  │
                       │  │  Tex     Tex     Tex   │  │
                       │  │    +       +       +   │  │
                       │  │  FBO     FBO     FBO   │  │
                       │  └────────────────────────┘  │
                       │                              │
                       │  INTEROP MODES (priority):   │
                       │  ┌────────────────────────┐  │
                       │  │1. WGL_NV_DX_interop2   │  │
                       │  │   (NVIDIA, zero-copy)  │  │
                       │  ├────────────────────────┤  │
                       │  │2. EXT_external_objects │  │
                       │  │   (Cross-vendor)       │  │
                       │  ├────────────────────────┤  │
                       │  │3. CPU Staging Fallback │  │
                       │  │   (Universal, slow)    │  │
                       │  └────────────────────────┘  │
                       └──────────────┬───────────────┘
                                      │
                              GLuint texture
                                      │
                                      │
    ┌─────────────────────────────────────────────────────────────────────────┐
    │                            COLOR LAYER                                  │
    └─────────────────────────────────────────────────────────────────────────┘
                                      │
                       ┌──────────────▼──────────────┐
                       │       OCIO Pipeline         │
                       │    src/color/               │
                       │                             │
                       │  ┌───────────────────────┐  │
                       │  │  Color Transforms     │  │
                       │  │                       │  │
                       │  │  Input ──► Process    │  │
                       │  │            │          │  │
                       │  │     ┌──────┴──────┐   │  │
                       │  │     │ 3D LUT      │   │  │
                       │  │     │ GPU Shader  │   │  │
                       │  │     └──────┬──────┘   │  │
                       │  │            │          │  │
                       │  │  Output ◄──┘          │  │
                       │  └───────────────────────┘  │
                       │                             │
                       │  D3D11OCIORenderer (D3D11)  │
                       │  ocio_pipeline.h (OpenGL)   │
                       └──────────────┬──────────────┘
                                      │
                                      │
    ┌─────────────────────────────────────────────────────────────────────────┐
    │                            RENDER LAYER                                 │
    └─────────────────────────────────────────────────────────────────────────┘
                                      │
                       ┌──────────────▼──────────────┐
                       │         ImGui               │
                       │     + OpenGL Rendering      │
                       │                             │
                       │  ┌───────────────────────┐  │
                       │  │  ImGui_ImplOpenGL3    │  │
                       │  │  ImGui_ImplGlfw       │  │
                       │  └───────────┬───────────┘  │
                       │              │              │
                       │  ┌───────────▼───────────┐  │
                       │  │    UI Components      │  │
                       │  │  ┌─────────────────┐  │  │
                       │  │  │ Video Display   │  │  │
                       │  │  │ Timeline        │  │  │
                       │  │  │ Inspector       │  │  │
                       │  │  │ Color Controls  │  │  │
                       │  │  └─────────────────┘  │  │
                       │  └───────────────────────┘  │
                       │                             │
                       │  ImGui::Image(gl_texture)   │
                       └──────────────┬──────────────┘
                                      │
                                      │
    ┌─────────────────────────────────────────────────────────────────────────┐
    │                           SWAPCHAIN LAYER                               │
    └─────────────────────────────────────────────────────────────────────────┘
                                      │
                       ┌──────────────▼──────────────┐
                       │    D3D11HDRSwapchain        │
                       │    src/hdr/                 │
                       │                             │
                       │  ┌───────────────────────┐  │
                       │  │   Frame Lifecycle     │  │
                       │  │                       │  │
                       │  │  BeginFrame()         │  │
                       │  │      │                │  │
                       │  │      ▼                │  │
                       │  │  Bind GL FBO          │  │
                       │  │      │                │  │
                       │  │      ▼                │  │
                       │  │  [Render to FBO]      │  │
                       │  │      │                │  │
                       │  │      ▼                │  │
                       │  │  EndFrame(vsync)      │  │
                       │  │      │                │  │
                       │  │      ▼                │  │
                       │  │  Present()            │  │
                       │  └───────────────────────┘  │
                       │                             │
                       │  HDR: R10G10B10A2           │
                       │       PQ/ST.2084 (HDR10)    │
                       │  SDR: R8G8B8A8              │
                       └──────────────┬──────────────┘
                                      │
                                      │
    ┌─────────────────────────────────────────────────────────────────────────┐
    │                              DISPLAY                                    │
    └─────────────────────────────────────────────────────────────────────────┘
                                      │
                       ┌──────────────▼──────────────┐
                       │                             │
                       │      ╔═══════════════╗      │
                       │      ║               ║      │
                       │      ║    MONITOR    ║      │
                       │      ║               ║      │
                       │      ║   HDR / SDR   ║      │
                       │      ║               ║      │
                       │      ╚═══════════════╝      │
                       │                             │
                       └─────────────────────────────┘


```

---

## Video

Videos are decoded using D3D11VA hardware acceleration (with FFmpeg software fallback). Frames arrive as YUV textures (NV12/P010) and are converted to linear RGBA16F via HLSL shaders on the GPU. The D3D11-GL interop layer shares these textures with OpenGL using zero-copy methods (NV_DX_interop2 on NVIDIA, EXT_external_objects cross-vendor, or CPU staging as fallback). This allows the ImGui/OpenGL UI to display video frames without expensive GPU-to-CPU-to-GPU copies.

---

## Image Sequences

Image sequences use OTIO for timeline control. When loading a sequence, u.m.p. creates a virtual timeline and loads images directly via dedicated loaders:

- **EXR**: Multi-layer and multi-part support with layer extraction
- **TIFF**: Scanline-based loading
- **PNG/JPEG**: Standard decoding with fast thumbnail generation

Images are loaded into a ring buffer cache centered around the playhead position. The cache prioritizes: current frame > frames ahead > frames behind, with automatic eviction outside the window.

---

## D3D11-OpenGL Interop

The interop layer uses triple-buffered shared textures to eliminate GPU blocking:

| Mode | Vendor | Method |
|------|--------|--------|
| WGL_NV_DX_interop2 | NVIDIA | Zero-copy, mature |
| EXT_external_objects | Intel/AMD/NVIDIA | Zero-copy, cross-vendor |
| CPU Staging | All | Universal fallback |

---

## HDR Output

u.m.p. supports HDR10 output via the D3D11 swapchain when Windows HDR mode is enabled. The display pipeline uses:

- **Format**: R10G10B10A2
- **Transfer**: PQ/ST.2084 (HDR10)
- **Color Space**: BT.2020

HDR detection is automatic via DXGI Output6, and the UI colors are converted to PQ when HDR is active.

---

## Pipeline Modes

u.m.p. supports multiple pipeline modes that control texture formats, bit depth, and cache sizing throughout the rendering chain. The pipeline mode affects FBOs at both the video and color processing stages.

| Mode | Format | Bit Depth | Bytes/Pixel | Use Case |
|------|--------|-----------|-------------|----------|
| **Normal** | RGBA8 | 8-bit | 4 | Standard video, 8-bit sequences (best performance) |
| **High-Res** | RGBA16 | 12-bit | 8 | ProRes 4444, 16-bit TIFF/PNG (OCIO optimized) |
| **Ultra-High-Res** | RGBA16F | 16-bit float | 8 | EXR sequences, complex OCIO workflows |
| **HDR** | RGBA16F | 16-bit float | 8 | HDR video (wide dynamic range) |

### Pipeline Mode Selection

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         PIPELINE MODE SELECTION                             │
└─────────────────────────────────────────────────────────────────────────────┘

  SOURCE TYPE                              AUTO-SELECTED MODE
  ───────────────────────────────────────────────────────────────────────────
  8-bit video (.mp4, .mov, etc.)     ───►  NORMAL (RGBA8)

  12-bit video (ProRes 4444)         ───►  HIGH_RES (RGBA16)
  16-bit TIFF/PNG sequences          ───►  HIGH_RES (RGBA16)

  EXR sequences (float)              ───►  ULTRA_HIGH_RES (RGBA16F)

  HDR video (HDR10, HLG)             ───►  HDR_RES (RGBA16F)
  ───────────────────────────────────────────────────────────────────────────

  Note: For videos, pipeline mode can be manually overridden.
        For image sequences, mode is auto-selected based on format.
```

### Where Pipeline Modes Apply

```
                                       │
                               GLuint texture
                                       │
                    ┌──────────────────▼───────────────────┐
                    │         VIDEO FBO                    │
                    │  ┌────────────────────────────────┐  │
                    │  │  Format set by Pipeline Mode:  │  │
                    │  │  • NORMAL ──► GL_RGBA8         │  │
                    │  │  • HIGH_RES ──► GL_RGBA16      │  │
                    │  │  • ULTRA/HDR ──► GL_RGBA16F    │  │
                    │  └────────────────────────────────┘  │
                    └──────────────────┬───────────────────┘
                                       │
                    ┌──────────────────▼───────────────────┐
                    │         COLOR FBO (OCIO)             │
                    │  ┌────────────────────────────────┐  │
                    │  │  Same format as Video FBO      │  │
                    │  │  Linear processing enabled for │  │
                    │  │  HIGH_RES, ULTRA, and HDR      │  │
                    │  └────────────────────────────────┘  │
                    └──────────────────┬───────────────────┘
                                       │
                    ┌──────────────────▼───────────────────┐
                    │         CACHE SIZING                 │
                    │  ┌────────────────────────────────┐  │
                    │  │  Memory = W × H × bytes/pixel  │  │
                    │  │  • NORMAL: 4 bytes/pixel       │  │
                    │  │  • Others: 8 bytes/pixel       │  │
                    │  └────────────────────────────────┘  │
                    └──────────────────────────────────────┘
```
