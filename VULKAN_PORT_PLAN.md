# QCView-Player: Vulkan/Linux Port Plan

## Context

QCView-Player is a professional video/image sequence player currently built on D3D11/Windows. The goal is to port the GPU rendering pipeline to Vulkan and add Linux support. The existing D3D11 Windows path stays intact via `#ifdef` guards — Linux uses all-Vulkan (no OpenGL), Windows continues using D3D11. Annotations (NanoVG) are included from the start. Audio uses PipeWire. Display target is Wayland-first with X11 fallback.

## Scope Summary

- **~7,000 lines** of D3D11 code to create Vulkan equivalents for
- **~22 pure-Windows files** need Vulkan counterparts
- **~8 hybrid files** need `#ifdef` branches for Linux/Vulkan
- **~200+ cross-platform files** need no changes (timeline, project, annotations logic, image loaders)
- HLSL shaders (~500 lines) need GLSL/SPIR-V equivalents

## Key Design Decisions

1. **Parallel implementations, not abstraction layer** — Vulkan files sit alongside D3D11 files, gated by `#ifdef __linux__` / `QCVIEW_USE_VULKAN`. The existing interfaces (`IVideoDecoder`, `IImageLoader`, `IFrameSource`) are the right abstraction boundary.

2. **VMA (Vulkan Memory Allocator)** for all GPU memory management — single-header, handles external memory import for VA-API DMA-BUF.

3. **shaderc** for runtime GLSL→SPIR-V compilation — needed for OCIO (generates GLSL at runtime). Ships with Vulkan SDK.

4. **`ImGui_ImplVulkan_AddTexture()`** bridges video textures to ImGui — returns `VkDescriptorSet` cast as `ImTextureID`.

5. **No D3D11↔GL interop complexity** — with all-Vulkan, video pipeline and ImGui share the same `VkDevice`. Triple-buffered `VkImage` ring with `VkSemaphore` sync replaces the interop layer entirely.

6. **VA-API → Vulkan zero-copy** via DMA-BUF fd import (`VK_EXT_external_memory_dma_buf`). Copy-on-import pattern (matches D3D11 path's `BufferedFrame::hw_texture` copy).

7. **NanoVG**: Use community `nanovg_vulkan` backend. Fallback: CPU rasterizer (`nanovg_sw`) → upload as VkImage overlay.

---

## Phase 0: Build System & Platform Foundation (1-2 weeks)

**Goal**: App opens on Linux with GLFW + ImGui Vulkan backend rendering. No video yet.

### New files
- `src/gpu/vulkan_device.h/cpp` — Singleton `VulkanDeviceManager`: VkInstance, VkPhysicalDevice, VkDevice, VmaAllocator, queue families (graphics + compute + transfer). Mirrors `d3d11_device_manager.{h,cpp}`.

### Files to modify
- **`CMakeLists.txt`** — Major changes:
  - Linux platform detection, `QCVIEW_USE_VULKAN` option
  - `find_package(Vulkan REQUIRED)`
  - System FFmpeg/OCIO/OpenEXR via `pkg-config` on Linux
  - Conditional source file lists (`d3d11_*` on Windows, `vulkan_*` on Linux)
  - Remove `WIN32` subsystem flag on Linux
  - Link `vulkan`, `shaderc`, system libs instead of `d3d11`, `d3dcompiler`, `dxgi`, etc.
- **`src/main.cpp`** — Add Linux branch:
  - `glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API)` (no GL context)
  - `ImGui_ImplGlfw_InitForVulkan()` + `ImGui_ImplVulkan_Init()`
  - Skip HWND/DWM/GPU-hints code
- **`src/app/application_init.cpp`** — Skip D3D11DeviceManager/HDROutputManager init on Linux, init VulkanDeviceManager instead
- **`src/app/application.h`** — Guard HWND members with `#ifdef _WIN32`

### External deps to add
- `external/vma/vk_mem_alloc.h` (single header, add to repo)
- System packages: `vulkan-sdk`, `libshaderc-dev`, `libglfw3-dev`

### Validation
App opens on Linux, ImGui demo window renders via Vulkan, clean shutdown.

---

## Phase 1: CPU Decode → Vulkan Display (2-3 weeks)

**Goal**: Play video files (SW decode) and image sequences, displayed via Vulkan.

### New files
- `src/gpu/vulkan_texture_pool.h/cpp` — VkImage pool with LRU eviction. Uses VMA. Tracks `VkImageView` + pre-allocated `VkDescriptorSet` per texture. Mirrors `d3d11_texture_pool.{h,cpp}` and `texture_pool.{h,cpp}`.
- `src/gpu/vulkan_upload.h/cpp` — Staging buffer upload: CPU `PixelData` → `VkBuffer` (staging, host-visible) → `vkCmdCopyBufferToImage` → `VkImage`. Dedicated transfer queue. Triple-buffered staging ring with `VkFence` per slot.

### Interface changes (cascading)
- **`src/player/image_loader_interface.h`** — `PixelData` struct: replace `GLenum gl_format`/`gl_type` with a platform-agnostic enum:
  ```cpp
  enum class PixelFormat { RGBA8, RGBA16, RGBA16F };
  ```
  Keep `gl_format`/`gl_type` as derived helpers behind `#ifdef`. This touches every file that creates/consumes `PixelData` — but the data payload (`std::vector<uint8_t> pixels`) is unchanged.
- **`src/player/frame_source_interface.h`** — `IFrameSource::GetFrame()` returns `GLuint`. Change to `uint64_t` (opaque handle: `GLuint` on GL, `VkDescriptorSet` on Vulkan). Typedef as `TextureHandle`.
- **`src/player/frame_cache.h`** — `CachedFrame::texture_id` from `GLuint` to `TextureHandle`. `CreateTexture()` calls vulkan_upload on Linux.

### Files to modify
- **`src/player/video_display_component.h/cpp`** — Add Vulkan path:
  - Vulkan texture handles instead of `GLuint video_texture_`
  - `GetDisplayTextureID()` returns VkDescriptorSet on Vulkan
  - `CreateVideoTextures()` creates VkImages
  - `ApplyColorPipeline()` — passthrough initially (OCIO is Phase 2)
- **`src/player/pipeline_mode.h`** — Keep `PipelineConfig` but make `internal_format`/`data_type` derive from `PixelFormat` enum rather than GL enums.

### Validation
Open ProRes/DNxHD file → frames display on Linux. Open EXR sequence → frames display. Scrubbing works. No color management yet (passthrough).

---

## Phase 2: YUV Shader + OCIO Color Pipeline (2-3 weeks)

**Goal**: GPU YUV→RGB conversion and OCIO color transforms on Vulkan.

### New files
- **`shaders/yuv_to_rgb.frag`** — GLSL port of HLSL from `d3d11_yuv_renderer.cpp` (~160 lines). BT.709/2020 matrices, PQ EOTF, video/full range, 8/10/12-bit, multi-plane (NV12, planar, GBRAP). Compiled to SPIR-V at build time via `glslangValidator` or at runtime via shaderc.
- **`shaders/yuv_to_rgb.vert`** — Fullscreen triangle vertex shader.
- **`shaders/dual_composite.frag`** — GLSL port of dual compositor HLSL from `d3d11_dual_compositor.cpp`.
- **`shaders/passthrough.frag`** — Identity blit shader.
- `src/gpu/vulkan_yuv_renderer.h/cpp` — Vulkan graphics pipeline for YUV→RGB. Takes Y/UV/planar `VkImageView` inputs, renders to RGBA16F `VkImage`. Mirrors `d3d11_yuv_renderer.{h,cpp}`.
- `src/gpu/vulkan_shader_compiler.h/cpp` — Thin shaderc wrapper for runtime GLSL→SPIR-V. Caches compiled modules.
- `src/color/vulkan_ocio_renderer.h/cpp` — Mirrors `d3d11_ocio_renderer.{h,cpp}`. Takes OCIO GLSL shader source, compiles to SPIR-V, creates VkPipeline. Uploads 1D/3D LUT textures as VkImages. Renders fullscreen quad.

### Files to modify
- **`src/color/ocio_pipeline.h/cpp`** — Add `GenerateAndCompileShaderVulkan()` alongside GL/D3D11 paths. Reuses OCIO's GLSL generation, compiles to SPIR-V.
- **`src/gpu/yuv_textures.h`** — Add Vulkan variant: `VkImageView` instead of `ID3D11ShaderResourceView*`. Guard with `#ifdef`.
- **`src/player/video_display_component.cpp`** — `ApplyColorPipeline()` calls `vulkan_ocio_renderer` on Linux.

### Validation
Load HDR PQ content → verify BT.2020 colors. Toggle OCIO color spaces → transforms apply correctly. Compare pixel output against D3D11 reference on Windows.

---

## Phase 3: VA-API Hardware Decode + DMA-BUF (2-3 weeks)

**Goal**: H.264/HEVC/VP9/AV1 decode via VA-API with zero-copy import to Vulkan.

### New files
- `src/gpu/vulkan_hwframe_importer.h/cpp` — Imports DMA-BUF fd from VA-API `AVFrame` into `VkImage`. Handles NV12/P010 multi-plane. Uses `VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier`. Mirrors `d3d11_hwframe_extractor.{h,cpp}`.
- `src/player/vulkan_video_decoder.h/cpp` — Full `IVideoDecoder` implementation for Linux. Mirrors `d3d11_video_decoder.{h,cpp}` structure:
  - FFmpeg with `AV_HWDEVICE_TYPE_VAAPI`
  - HW frames: DMA-BUF → VkImage → YUV shader → RGBA16F VkImage
  - SW frames: CPU AVFrame → staging upload → YUV shader → RGBA16F VkImage
  - Background decode thread, 16-slot frame buffer ring, keyframe index — same design as D3D11 decoder
  - Copy-on-import for HW frames (VA-API surface copied to Vulkan-owned VkImage, then released)

### Files to modify
- **`src/player/hw_context_manager.h/cpp`** — Add `InitializeVAAPI()`, `GetVAAPIContext()`.
- **`src/player/video_decoder_interface.h`** — Add `VULKAN` to `VideoDecoderBackend` enum, `VAAPI` already exists in `HWAccelType`.
- **`src/player/video_decoder_factory.h`** — On Linux, return `VulkanVideoDecoder`.

### Required Vulkan extensions
`VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier` — supported on Mesa (Intel/AMD) and NVIDIA 515+.

### Validation
Play H.264/HEVC file → verify HW decode via `vainfo`. Check 10-bit P010 content. Benchmark vs SW decode. Test on Intel, AMD, NVIDIA.

---

## Phase 4: Audio (PipeWire) + Dual View (1-2 weeks)

**Goal**: Audio playback and dual view comparison working on Linux.

### New files
- `src/audio/pipewire_audio_device.h/cpp` — PipeWire output via `pw_stream`. Float32 stereo @ 48kHz. Event-driven callback maps 1:1 to `WasapiAudioDevice` pattern (`Initialize`, `Start`, `Stop`, `Shutdown`). Uses `SPA_AUDIO_FORMAT_F32_LE`.
- `src/gpu/vulkan_dual_compositor.h/cpp` — Composites two VkImages via fragment shader. Direct port of D3D11 compositor. Much simpler than D3D11 version (no interop needed, same VkDevice). Mirrors `d3d11_dual_compositor.{h,cpp}`.

### Files to modify
- **`src/audio/audio_player.h/cpp`** — Conditional: `PipeWireAudioDevice` on Linux, `WasapiAudioDevice` on Windows.
- **`src/player/dual_view_pipeline.h/cpp`** — Add Vulkan compositor path.
- **`src/gpu/dual_view_layout.h/cpp`** — No changes (pure C++ math).

### Validation
Play video with audio → verify lip sync. Open dual view → both sources display. Audio rate correction works.

---

## Phase 5: NanoVG Annotations (1-2 weeks)

**Goal**: Annotation rendering works on Vulkan.

### Approach
1. Integrate `nanovg_vulkan` backend (community fork, e.g. `memononen/nanovg` has Vulkan PRs, or `aspect-build/nanovg`).
2. Add as external dependency or vendored source in `external/nanovg_vk/`.

### Files to modify
- **`src/annotations/nanovg_context.h/cpp`** — `Initialize()` calls `nvgCreateVk()` on Vulkan instead of `nvgCreateGL3()`. Save/restore Vulkan state instead of GL state.
- **`src/annotations/annotation_renderer.cpp`** — Should work as-is since it uses NanoVG API (not GL directly).
- **`src/annotations/viewport_annotator.cpp`** — Same — NanoVG abstraction should handle backend.

### Fallback
If `nanovg_vulkan` proves unreliable:
- Render annotations to offscreen CPU buffer using `nanovg` software rasterizer
- Upload as VkImage overlay texture
- Composite in presentation pass

### Validation
Draw annotations → verify strokes, fills, text render correctly. Export annotations → verify output matches.

---

## Phase 6: HDR Output (Future — when Linux HDR stabilizes)

**Goal**: HDR10 output on Wayland compositors (KDE Plasma 6.x, GNOME 47+).

### New files
- `src/hdr/vulkan_hdr_swapchain.h/cpp` — `VkSwapchainKHR` with `VK_COLOR_SPACE_HDR10_ST2084_EXT`. Uses `VK_EXT_hdr_metadata`.

### Notes
- Deferred because Wayland HDR (`wp_color_management_v1`) is compositor-dependent and not universally deployed.
- The PQ encoding math in `hdr_output_manager.h` is pure C++ — ports directly.
- SDR output is fully functional from Phase 0.

---

## New File Summary

```
src/gpu/vulkan_device.h/cpp              Phase 0  (mirrors d3d11_device_manager)
src/gpu/vulkan_texture_pool.h/cpp        Phase 1  (mirrors d3d11_texture_pool + texture_pool)
src/gpu/vulkan_upload.h/cpp              Phase 1  (staging buffer upload)
src/gpu/vulkan_yuv_renderer.h/cpp        Phase 2  (mirrors d3d11_yuv_renderer)
src/gpu/vulkan_shader_compiler.h/cpp     Phase 2  (shaderc wrapper)
src/gpu/vulkan_dual_compositor.h/cpp     Phase 4  (mirrors d3d11_dual_compositor)
src/gpu/vulkan_hwframe_importer.h/cpp    Phase 3  (mirrors d3d11_hwframe_extractor)
src/color/vulkan_ocio_renderer.h/cpp     Phase 2  (mirrors d3d11_ocio_renderer)
src/player/vulkan_video_decoder.h/cpp    Phase 3  (mirrors d3d11_video_decoder)
src/audio/pipewire_audio_device.h/cpp    Phase 4  (mirrors wasapi_audio_device)
src/hdr/vulkan_hdr_swapchain.h/cpp       Phase 6  (mirrors d3d11_hdr_swapchain)
shaders/yuv_to_rgb.vert                  Phase 2
shaders/yuv_to_rgb.frag                  Phase 2
shaders/dual_composite.frag              Phase 4
shaders/passthrough.frag                 Phase 2
external/vma/vk_mem_alloc.h              Phase 0
```

## Risks

| Risk | Severity | Mitigation |
|---|---|---|
| VA-API DMA-BUF import driver bugs | High | CPU fallback (`av_hwframe_transfer`), test Intel/AMD/NVIDIA |
| NanoVG Vulkan backend quality | Medium | CPU rasterizer fallback ready |
| OCIO GLSL→SPIR-V edge cases | Medium | Cache compiled pipelines, test all built-in transforms |
| `PixelData`/`TextureHandle` refactor scope | Medium | Do it in Phase 1 before other code depends on it |
| Linux HDR compositor fragmentation | Low | Deferred to Phase 6, SDR works from Phase 0 |

## Verification Strategy

Each phase has a self-contained validation milestone:
- **Phase 0**: App opens, ImGui renders, clean shutdown
- **Phase 1**: SW decode video + EXR sequences display, scrubbing works
- **Phase 2**: OCIO color transforms apply, HDR content colors correct
- **Phase 3**: HW decode works, zero-copy pipeline, 10-bit content
- **Phase 4**: Audio syncs, dual view composites correctly
- **Phase 5**: Annotations draw correctly on Vulkan
- **Phase 6**: HDR10 output on supported Wayland compositors

Cross-validate each phase against Windows D3D11 output for pixel accuracy.
