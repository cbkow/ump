# QCView-Player: Linux Port — Remaining Issues

## Status

All 6 phases of the Vulkan port plan are complete. The application is fully functional on Linux with Vulkan rendering, VA-API hardware decode, PipeWire audio, HDR output, OCIO color management, and annotation rendering.

This document tracks remaining gaps discovered during the final audit.

---

## Medium Priority

### ~~1. Clipboard Support~~ DONE
- **File:** `src/app/window_management.cpp:624`
- **Fix:** Uses `glfwSetClipboardString()` — works on Wayland and X11 via GLFW

### ~~2. Frame.io HTTP Client~~ DONE
- **Files:** `src/integrations/frameio_client.cpp`, `src/integrations/frameio_url_parser.cpp`
- **Fix:** `HttpGet()` and `FollowRedirect()` implemented with libcurl behind `#ifdef QCVIEW_HAS_CURL`

### ~~3. Screenshot Support~~ DONE
- **`annotation_io.cpp`** — `SaveScreenshot()` now uses `stbi_write_png()`
- **`video_display_component.cpp`** — `CaptureScreenshotToClipboard()` uses native Wayland/X11 clipboard (no external tools needed)
- **`video_display_component.cpp`** — `CaptureScreenshotToDesktop()` uses XDG_DESKTOP_DIR / ~/Desktop

---

## Low Priority

### ~~4. File Selection in File Manager~~ SKIPPED
- **Workaround:** `xdg-open` opens the parent directory, which is acceptable
- **Note:** Windows `/select,` flag has no cross-DE equivalent on Linux

### ~~5. Single-Instance IPC~~ DONE
- **Files:** `src/utils/single_instance.h/cpp`, `src/main.cpp`, `src/app/window_management.cpp`
- **Fix:** D-Bus session bus (`com.qcview.Player`) — second instance sends file paths to first via method call

### ~~6. Async Annotation I/O~~ DONE
- **File:** `src/annotations/annotation_io.cpp`
- **Fix:** `LoadNotesAsync()` and `SaveNotesAsync()` now use detached `std::thread`

---

## Completed (For Reference)

| Phase | Feature | Status |
|-------|---------|--------|
| 0 | Vulkan device, ImGui backend, GLFW window | Done |
| 1 | Texture pool, CPU decode display, staging upload | Done |
| 2 | YUV shader, OCIO color pipeline, shader compiler | Done |
| 3 | VA-API HW decode, DMA-BUF import, VulkanVideoDecoder | Done |
| 4.1 | PipeWire audio | Done |
| 4.2 | Dual view compositor | Done |
| 5 | Vulkan annotation rendering (tessellation + MSAA) | Done |
| 6 | HDR output (swapchain, UI brightness, passthrough) | Done |
| — | Settings persistence (XDG paths) | Done |
| — | System accent color (D-Bus portal) | Done |
| — | ExifTool integration (bundled Perl script) | Done |
| — | File browser / URL opening (xdg-open) | Done |
| — | Fullscreen (Wayland-aware) | Done |
| — | Force software decode toggle | Done |
| — | Settings panel Linux text fixes | Done |
| — | Keyboard shortcuts / font settings | Done |
