---
title: Technical Overview
permalink: /technical-overview/
nav_order: 3
---

# Technical Overview

A quick orientation to what's under the hood. Nothing here changes how you use the app — skip to the feature pages if you're after a specific control.

## Stack

QCView is a Qt 6.11 Quick / QML application with a native player surface drawn under the UI for low-latency, color-managed video output.

| | Windows | macOS |
|---|---|---|
| **UI** | Qt Quick / QML | Qt Quick / QML |
| **GPU** | Direct3D 11 + DirectComposition (multi-visual for HDR) | Metal |
| **HW decode** | Vulkan-video for ProRes, D3D11 for h.264/h.265 via FFmpeg (with software fallback) | VideoToolbox via FFmpeg |
| **Audio** | WASAPI shared mode, MMCSS "Pro Audio" priority | CoreAudio AudioUnit |
| **HDR output** | HDR10 (PQ / ST.2084, BT.2020) via DXGI swapchain | EDR (Extended Dynamic Range, linear-light) |

The video frame path is hardware-accelerated end-to-end on both platforms — no CPU staging for the main pipeline.

## Pipeline modes

The pipeline mode controls the precision the source frame is carried at through OCIO and to the display. QCView picks one automatically based on the source format.

| Mode | Texture | Bit depth | When QCView picks it |
|---|---|---|---|
| **Normal** | RGBA8 | 8-bit | Standard video (.mp4, .mov 8-bit) — best performance |
| **High-Res** | RGBA16 | 12-bit | ProRes 4444, 16-bit TIFF / PNG |
| **Ultra-High-Res** | RGBA16F | 16-bit float | EXR sequences; OCIO chains needing float headroom |
| **HDR** | RGBA16F | 16-bit float | HDR video sources (HDR10, HLG) |

Higher precision modes use more memory per cached frame (8 bytes/pixel vs. 4 for Normal); cache window sizing accounts for this automatically.

## HDR

**Windows**: HDR10 output can be used when Windows HDR mode is enabled. The player surface is its own DirectComposition visual so the HDR swapchain can use PQ/ST.2084 + BT.2020 without dragging the Qt UI's sRGB swapchain along with it. scRGB (sRGB linear) is always available, but not reccomended since HDR mode on monitors is HDR10 and this forces Windows to translate. 

**macOS**: EDR (Extended Dynamic Range) is a linear-light system where `1.0 = SDR white` and values above 1.0 are brighter, up to the display's headroom. The ACES 2.0 and Blender 5.2 OCIO configs include Linear sRGB EDR and Linear P3 EDR display outputs for this workflow.

## Audio

WASAPI on Windows and CoreAudio on macOS, float32 stereo at 48 kHz. The audio mixer is shared cross-platform.

Multi-stream broadcast deliveries (5.1, 7.1, multiple discrete language stems, etc.) are routed per channel — you pick which file's channel goes to which output from the Audio routing controls in the Inspector. Per-clip routing also applies inside a playlist.

## Image sequences

EXR (multi-layer + multi-part with layer extraction), TIFF, PNG, JPEG, and JPEG-2000 / HTJ2K (via OpenJPH). Frames are loaded into a ring-buffer cache centered on the playhead position, with current > ahead > behind priority and automatic eviction outside the cache window.

## Color

Live OCIO chains are evaluated per frame on the GPU. Bundled configs: ACES 2.0, ACES 1.3, Blender 5.2, Blender 5.1. Screenshots and annotation exports apply the OCIO transform to the saved pixels.
