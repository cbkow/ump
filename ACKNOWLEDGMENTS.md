# Acknowledgments

This document acknowledges projects and individuals that inspired the design and architecture of u.m.p., even though no actual code from these projects was used in our implementation. It's also worth noting that I am an artist and not a developer, so I heavily leaned on LLMs or so-called "vibe-coding" workflows to help me build u.m.p. This is a personal app, built as an in-house tool, that I am sharing online out of respect for the Open-Source community and ethos. 

---

## Design Inspirations

### tlRender

**Website:** https://github.com/darbyjohnston/tlRender
**License:** BSD 3-Clause

I drew significant inspiration from tlRender's architecture and design patterns, particularly:

- **EXR sequence caching strategy** - The concept of background spiral caching and cache segment management was inspired by tlRender's timeline cache design
- **Frame cache architecture** - Multi-threaded background loading patterns and cache eviction strategies
- **Thumbnail generation** - Approach to generating and caching timeline thumbnails for scrubbing
- **Pipeline design patterns** - Separation of concerns between video player, cache manager, and rendering pipeline

**Important Note:** No actual code from tlRender was copied or used in u.m.p. All implementations were written from scratch based on understanding the architectural concepts and design patterns. I acknowledge tlRender as a significant source of inspiration for the handling of image sequences and cache systems in u.m.p.

I am grateful to Darby Johnston and tlRender for creating an excellent open-source reference implementation that helped me better understand OpenEXR usage in general. Still, I also want to ensure that none of my mistakes or amateur coding are attributed to his project.

## Community

Special thanks to:

- The **FFmpeg** community for comprehensive documentation and examples
- The **mpv** community for comprehensive documentation and examples
- The **OpenEXR** community for technical guidance 
- The **OpenColorIO** project for color management
- The **ImGui** community for UI/UX patterns and widget design inspiration

---

*This document acknowledges design inspiration only. For actual code dependencies and their licenses, see `LICENSES/THIRD_PARTY_NOTICES.txt`.*

*Last updated: 2025-10-12*
