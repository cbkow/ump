# Acknowledgments

This document acknowledges projects and individuals that inspired the design and architecture of u.m.p.

---

## Design Inspirations

### tlRender

**Website:** https://github.com/darbyjohnston/tlRender
**License:** BSD 3-Clause

I drew significant inspiration from tlRender's architecture and design patterns, particularly:

- **EXR sequence caching strategy** - The concept of background spiral caching and cache segment management was inspired by tlRender's timeline cache design
- **Frame cache architecture** - Multi-threaded background loading patterns and cache eviction strategies
- **Pipeline design patterns** - Separation of concerns between video player, cache manager, and rendering pipeline


### thumbfast

**Website:** https://github.com/po5/thumbfast
**License:** MPL-2.0

Thumbfast's efficient thumbnail generation and caching approach for mpv inspired design aspects of u.m.p.'s timeline scrubbing system, particularly:

- **On-demand thumbnail generation** - The concept of generating thumbnails as needed during timeline hover
- **Lightweight caching strategy** - Efficient memory management for thumbnail cache
- **mpv integration patterns** - Best practices for thumbnail extraction from video files

## Community

Special thanks to:

- The **FFmpeg** community for comprehensive documentation and examples
- The **mpv** community for comprehensive documentation and examples
- The **OpenEXR** project for EXR extraction
- The **OpenColorIO** project for color management
- The **OpenTimelineIO** project timeline management
- The **ImGui** community for UI/UX patterns and widget design inspiration
- The **miniaudio** project for audio handling in timelines
- The **Exiftool** project for metadata access

---

*This document acknowledges design inspiration only. For actual code dependencies and their licenses, see `LICENSES/THIRD_PARTY_NOTICES.txt`.*

*Last updated: 2026-01-01*
