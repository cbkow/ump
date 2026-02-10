# Acknowledgments

This document credits projects that inspired the design and architecture of u.m.p. No code from these projects are used.

---

## Design Inspirations

### tlRender

**Repository:** https://github.com/darbyjohnston/tlRender
**Author:** Darby Johnston
**License:** BSD 3-Clause

tlRender's architecture influenced several design decisions in u.m.p., including:

- Frame caching strategies for image sequences
- Background loading patterns for timeline playback
- Approach to OpenEXR and multi-channel image handling
- Thumbnail filmstrips

I'm grateful to Darby Johnston for creating an excellent open-source reference. I wouldn't have understood OpenEXR without it.

### mpv

**Website:** https://mpv.io
**Repository:** https://github.com/mpv-player/mpv
**License:** GPL v2+

mpv's design philosophy and architecture inspired aspects of u.m.p.'s video playback approach, particularly its clean separation of decoding, rendering, and display concerns. Early versions of u.m.p. embedded libmpv, but it is now decoupled to avoid tone-mapping in video flows and to allow HDR passthrough.

### thumbfast

**Repository:** https://github.com/po5/thumbfast
**Author:** po5
**License:** MPL-2.0

thumbfast's efficient approach to thumbnail generation for mpv inspired u.m.p.'s timeline scrubbing and hover preview system.

---

## Community & Projects

Thanks to the communities behind these projects for their documentation, examples, and open-source contributions:

- **Academy Software Foundation (ASWF)** - For stewarding OpenEXR, OpenColorIO, and OpenTimelineIO
- **FFmpeg** - Video/audio decoding and processing
- **Dear ImGui** - Immediate mode UI framework
- **NanoVG** - Antialiased vector graphics rendering
- **Google Ink Stroke Modeler** - Handwriting/drawing stroke smoothing
- **GLFW** - Cross-platform windowing and OpenGL context
- **nlohmann/json** - JSON parsing
- **miniaudio** - Cross-platform audio playback (we are no longer using miniaudio, but earlier app versions did)

---

*This document acknowledges design inspiration only. For code dependencies and their licenses, see [LICENSES/THIRD_PARTY_NOTICES.txt](LICENSES/THIRD_PARTY_NOTICES.txt).*
