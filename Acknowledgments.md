# Acknowledgments

This document credits projects that inspired the design and architecture of QCView.

For the licenses of code dependencies actually linked into the application, see [LICENSES/THIRD_PARTY_NOTICES.txt](LICENSES/THIRD_PARTY_NOTICES.txt).

---

## Design Inspirations

### tlRender

**Repository:** https://github.com/darbyjohnston/tlRender
**Author:** Darby Johnston
**License:** BSD 3-Clause

tlRender's architecture influenced several design decisions in QCView, including:

- Frame caching strategies for image sequences
- Background loading patterns for timeline playback
- Approach to OpenEXR and multi-channel image handling
- Thumbnail filmstrips

### mpv

**Website:** https://mpv.io
**Repository:** https://github.com/mpv-player/mpv
**License:** GPL v2+

mpv's design philosophy inspired aspects of QCView's video playback approach, particularly its clean separation of decoding, rendering, and display concerns.

---

## Community & Projects

Thanks to the communities behind these projects for their documentation, examples, and open-source contributions:

- **The Qt Company / Qt Project** — Qt 6.11 Quick + QRhi, the UI and rendering framework powering the rebuild.
- **Academy Software Foundation (ASWF)** — stewards of OpenEXR and OpenColorIO.
- **FFmpeg** — video / audio decoding and processing, including the Vulkan-video hardware-accelerated decode path.
- **BtbN** — Windows FFmpeg builds with libplacebo + libshaderc statically linked, enabling ProRes Vulkan decode on consumer GPUs.
- **Khronos Group** — Vulkan, the cross-vendor compute / video-decode API.
- **Phosphor Icons** — the icon family used throughout the QML UI.
- **Google Ink Stroke Modeler** — handwriting / drawing stroke smoothing.
- **Phil Harvey / ExifTool** — Adobe project metadata extraction.

---

## Predecessor app — legacy credits

The original QCView (a separate codebase, predecessor to this Qt rebuild) was built on a different stack. While the Qt rebuild does not link any of these libraries — Qt replaced their roles — the original project, and its users, were served well by them for years:

- **Dear ImGui** (MIT) — UI framework
- **ImPlot** (MIT), **ImNodes** (MIT), **imgui_markdown** (zlib) — ImGui ecosystem
- **GLFW** (zlib/libpng) — window / input
- **NanoVG** (zlib) — vector graphics
- **libmpv** (GPL v2+) — early playback engine, later replaced by direct FFmpeg + platform-GPU pipelines
- **miniaudio** (Public Domain / MIT-0) — earlier audio path
- **Native File Dialog Extended** (zlib) — file pickers
- **libharu / hpdf** (zlib) — PDF export
- **stb_image** / **stb_image_write** (Public Domain / MIT) — image I/O fallback
- **OpenTimelineIO** (Apache 2.0) — early playlist data model
- **nlohmann/json** (MIT) — JSON parsing
- **GLM** (MIT) — math types

Thanks to all the maintainers and contributors of those projects.

---

*This document acknowledges design inspiration and historical credits only. For code dependencies actually linked into the current application and their licenses, see [LICENSES/THIRD_PARTY_NOTICES.txt](LICENSES/THIRD_PARTY_NOTICES.txt).*
