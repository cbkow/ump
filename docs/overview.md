---
title: Technical Overview
permalink: /technical-overview/
nav_order: 3
---

# Technical Overview

## Rendering Architecture

QCView uses a layered GPU pipeline that flows from source media through decode, caching, color processing, and finally to display. Everything passes through the OCIO color pipeline before reaching the screen or being exported to files.

On **Windows**, the pipeline uses Direct3D 11 for video decoding and rendering, with D3D11-OpenGL interop to share textures with the ImGui UI layer. On **Linux**, the pipeline uses **Vulkan** throughout — VA-API for hardware video decoding with DMA-BUF zero-copy import into Vulkan, GLSL shaders compiled to SPIR-V for YUV conversion and OCIO processing, and PipeWire for audio. The diagrams below show both the Windows/D3D11 and Linux/Vulkan pipelines.

### Windows (D3D11 + OpenGL)

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

### Linux (Vulkan)

```
    ┌─────────────────────────────────────────────────────────────────────────┐
    │                              SOURCE MEDIA                               │
    └─────────────────────────────────────────────────────────────────────────┘
                │                                        │
        ┌───────▼───────┐                        ┌───────▼───────┐
        │  VIDEO FILES  │                        │    IMAGES     │
        │  .mp4 .mov    │                        │  .exr .tiff   │
        │  .mxf  etc    │                        │  .png .jpg    │
        └───────┬───────┘                        └───────┬───────┘
                │                                        │
    ┌─────────────────────────────────────────────────────────────────────────┐
    │                              DECODE LAYER                               │
    └─────────────────────────────────────────────────────────────────────────┘
                │                                        │
        ┌───────▼────────────────────┐           ┌───────▼───────────────────┐
        │   VulkanVideoDecoder       │           │  ImageSequenceDecoder     │
        │   src/player/              │           │  src/player/              │
        │                            │           │                           │
        │  ┌──────────┐ ┌──────────┐ │           │  ┌─────────────────────┐  │
        │  │  VA-API  │ │  FFmpeg  │ │           │  │     LOADERS         │  │
        │  │ Hardware │ │ Software │ │           │  │  ┌───────────────┐  │  │
        │  │  Decode  │ │  Decode  │ │           │  │  │ EXRImageLoader│  │  │
        │  └────┬─────┘ └────┬─────┘ │           │  │  │ TIFFLoader    │  │  │
        │       │            │       │           │  │  │ PNGLoader     │  │  │
        │       └─────┬──────┘       │           │  │  │ JPEGLoader    │  │  │
        │             │              │           │  │  └───────────────┘  │  │
        │      ┌──────▼──────┐       │           │  └──────────┬──────────┘  │
        │      │  DMA-BUF FD │       │           │             │             │
        │      │  (DRM PRIME)│       │           │      ┌──────▼──────┐      │
        │      │   Y + UV    │       │           │      │  PixelData  │      │
        │      └──────┬──────┘       │           │      │  (CPU RAM)  │      │
        │             │              │           │      └┬────────────┘      │
        └─────────────┼──────────────┘           └───────┼───────────────────┘
                      │                                  │
    ┌─────────────────────────────────────────────────────────────────────────┐
    │                              CACHE LAYER                                │
    │                         (shared cross-platform)                         │
    └─────────────────────────────────────────────────────────────────────────┘
                      │                                  │
    ┌─────────────────────────────────────────────────────────────────────────┐
    │                         GPU CONVERSION LAYER                            │
    └─────────────────────────────────────────────────────────────────────────┘
                                      │
                       ┌──────────────▼───────────────┐
                       │   VulkanHWFrameImporter      │
                       │   + VulkanYUVRenderer        │
                       │   src/gpu/                   │
                       │                              │
                       │  ┌────────────────────────┐  │
                       │  │  DMA-BUF → VkImage     │  │
                       │  │  (VK_EXT_external_     │  │
                       │  │   memory_dma_buf)      │  │
                       │  └──────────┬─────────────┘  │
                       │             │                │
                       │  ┌──────────▼─────────────┐  │
                       │  │   GLSL → SPIR-V        │  │
                       │  │                        │  │
                       │  │  • BT.709 matrix       │  │
                       │  │  • BT.2020 matrix      │  │
                       │  │  • PQ EOTF (HDR)       │  │
                       │  │  • Range expand        │  │
                       │  └──────────┬─────────────┘  │
                       │             │                │
                       │   Input:  DMA-BUF planes     │
                       │   Output: RGBA16F linear     │
                       └─────────────┬────────────────┘
                                     │
    ┌─────────────────────────────────────────────────────────────────────────┐
    │                            COLOR LAYER                                  │
    └─────────────────────────────────────────────────────────────────────────┘
                                     │
                      ┌──────────────▼───────────────┐
                      │    VulkanOCIORenderer        │
                      │    src/color/                │
                      │                              │
                      │  ┌────────────────────────┐  │
                      │  │  OCIO GLSL → SPIR-V    │  │
                      │  │  + 1D/3D LUT textures  │  │
                      │  │ Fullscreen render pass │  │
                      │  └──────────┬─────────────┘  │
                      └─────────────┼────────────────┘
                                    │
    ┌─────────────────────────────────────────────────────────────────────────┐
    │                            RENDER LAYER                                 │
    └─────────────────────────────────────────────────────────────────────────┘
                                    │
                      ┌─────────────▼────────────────┐
                      │         ImGui                │
                      │     + Vulkan Rendering       │
                      │                              │
                      │  ┌────────────────────────┐  │
                      │  │  ImGui_ImplVulkan      │  │
                      │  │  ImGui_ImplGlfw        │  │
                      │  └──────────┬─────────────┘  │
                      │             │                │
                      │  ┌──────────▼─────────────┐  │
                      │  │    UI Components       │  │
                      │  │  ┌──────────────────┐  │  │
                      │  │  │ Video Display    │  │  │
                      │  │  │ Timeline         │  │  │
                      │  │  │ Inspector        │  │  │
                      │  │  │ Color Controls   │  │  │
                      │  │  └──────────────────┘  │  │
                      │  └────────────────────────┘  │
                      └─────────────┬────────────────┘
                                    │
    ┌─────────────────────────────────────────────────────────────────────────┐
    │                           SWAPCHAIN LAYER                               │
    └─────────────────────────────────────────────────────────────────────────┘
                                    │
                      ┌─────────────▼────────────────┐
                      │   VulkanHDRSwapchain         │
                      │   src/hdr/                   │
                      │                              │
                      │  ┌────────────────────────┐  │
                      │  │  Runtime HDR Toggle    │  │
                      │  │                        │  │
                      │  │  SDR: B8G8R8A8_UNORM   │  │
                      │  │       sRGB Nonlinear   │  │
                      │  │                        │  │
                      │  │ HDR: A2B10G10R10_UNORM │  │
                      │  │       HDR10_ST2084     │  │
                      │  └────────────────────────┘  │
                      └─────────────┬────────────────┘
                                    │
    ┌─────────────────────────────────────────────────────────────────────────┐
    │                              DISPLAY                                    │
    └─────────────────────────────────────────────────────────────────────────┘
                                    │
                      ┌─────────────▼────────────────┐
                      │                              │
                      │      ╔═══════════════╗       │
                      │      ║               ║       │
                      │      ║    MONITOR    ║       │
                      │      ║               ║       │
                      │      ║   HDR / SDR   ║       │
                      │      ║               ║       │
                      │      ╚═══════════════╝       │
                      │                              │
                      └──────────────────────────────┘
```

---

## Video

### Windows (D3D11)

Videos are decoded using D3D11VA hardware acceleration (with FFmpeg software fallback). Frames arrive as YUV textures (NV12/P010) and are converted to linear RGBA16F via HLSL shaders on the GPU. The D3D11-GL interop layer shares these textures with OpenGL using zero-copy methods (NV_DX_interop2 on NVIDIA, EXT_external_objects cross-vendor, or CPU staging as fallback). This allows the ImGui/OpenGL UI to display video frames without expensive GPU-to-CPU-to-GPU copies.

### Linux (Vulkan)

Videos are decoded using VA-API hardware acceleration (with FFmpeg software fallback). Hardware-decoded frames remain in GPU memory and are imported into Vulkan as VkImages via DMA-BUF file descriptors — a zero-copy path that avoids any GPU-to-CPU readback. The flow is:

1. FFmpeg decodes with `AV_HWDEVICE_TYPE_VAAPI`
2. Frames are mapped to DRM PRIME format via `av_hwframe_map()`
3. DMA-BUF file descriptors are imported into Vulkan via `VK_EXT_external_memory_dma_buf`
4. GLSL fragment shaders (compiled to SPIR-V) perform YUV-to-RGBA16F conversion

Supported YUV formats include NV12 (8-bit), P010 (10-bit), YUV420P/422P/444P planar, and GBRP/GBRAP for ProRes pathways.

---

## Image Sequences

Image sequences use OTIO for timeline control. When loading a sequence, QCView creates a virtual timeline and loads images directly via dedicated loaders:

- **EXR**: Multi-layer and multi-part support with layer extraction
- **TIFF**: Scanline-based loading
- **PNG/JPEG**: Standard decoding with fast thumbnail generation

Images are loaded into a ring buffer cache centered around the playhead position. The cache prioritizes: current frame > frames ahead > frames behind, with automatic eviction outside the window.

Image sequence loading is cross-platform — the same loaders and cache engine are used on both Windows and Linux.

---

## GPU Interop

### Windows: D3D11-OpenGL Interop

The interop layer uses triple-buffered shared textures to eliminate GPU blocking:

| Mode | Vendor | Method |
|------|--------|--------|
| WGL_NV_DX_interop2 | NVIDIA | Zero-copy, mature |
| EXT_external_objects | Intel/AMD/NVIDIA | Zero-copy, cross-vendor |
| CPU Staging | All | Universal fallback |

### Linux: VA-API DMA-BUF Import

On Linux, the interop problem is different — there is no D3D11/OpenGL boundary to cross. Instead, VA-API decoded frames need to be imported into Vulkan. This is handled via DMA-BUF, a Linux kernel mechanism for sharing GPU memory between APIs:

| Step | Operation |
|------|-----------|
| VA-API decode | Frame decoded in GPU memory |
| DRM PRIME map | VA-API surface exported as DMA-BUF file descriptors |
| Vulkan import | `VK_EXT_external_memory_dma_buf` creates VkImage per YUV plane |
| YUV conversion | GLSL shader converts to RGBA16F |

The DMA-BUF path is zero-copy across the entire pipeline — the decoded frame never leaves GPU memory.

---

## HDR Output

### Windows

QCView supports HDR10 output via the D3D11 swapchain when Windows HDR mode is enabled. The display pipeline uses:

- **Format**: R10G10B10A2
- **Transfer**: PQ/ST.2084 (HDR10)
- **Color Space**: BT.2020

HDR detection is automatic via DXGI Output6, and the UI colors are converted to PQ when HDR is active.

### Linux

HDR output on Linux is toggleable at runtime within QCView. The Vulkan swapchain switches between SDR and HDR modes:

| Mode | Swapchain Format | Color Space |
|------|-----------------|-------------|
| SDR | B8G8R8A8_UNORM | sRGB Nonlinear |
| HDR | A2B10G10R10_UNORM | HDR10_ST2084 |

When HDR is active, the ImGui interface is converted to PQ via a GPU fragment shader with configurable target nits for UI brightness. HDR must also be enabled at the compositor level (e.g., KDE Plasma Display settings) for HDR output to reach the monitor.

---

## Color Processing

### Windows

OCIO color transforms are applied via `D3D11OCIORenderer` using DirectX shaders and 3D LUT textures.

### Linux

OCIO color transforms are applied via `VulkanOCIORenderer`, which compiles OCIO-generated GLSL shaders to SPIR-V at runtime and builds 1D/3D LUT textures from the OCIO pipeline. The Vulkan graphics pipeline applies the transforms in a fullscreen rendering pass.

---

## Audio

| Platform | Backend | Details |
|----------|---------|---------|
| Windows | WASAPI | Event-driven with MMCSS "Pro Audio" thread priority |
| Linux | PipeWire | Event-driven callback, low-latency buffer (default 10ms) |

Both backends output float32 stereo at 48kHz and expose identical callback interfaces, so the audio mixer and SoundTouch time-stretch processing are shared cross-platform.

---

## Pipeline Modes

QCView supports multiple pipeline modes that control texture formats, bit depth, and cache sizing throughout the rendering chain. The pipeline mode affects FBOs at both the video and color processing stages.

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
