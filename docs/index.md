---
title: Home
permalink: /
nav_order: 1
---

# u.m.p.

## Overview

![ump image](images/ump_HceQxrXtfQ.png)

**u.m.p.** is a [Dear ImGui](https://github.com/ocornut/imgui) C++ [libmpv-based](https://mpv.io/) OpenGL video player with direct memory-mapping playback for EXR, PNG, TIFF, and JPEG image sequences for Windows. Basic features include:

 - a spiraling seek/scrubbing cache for videos
 - a separate live playback memory cache for image sequences 
 - a [Thumbfast-inspired](https://github.com/po5/thumbfast) thumbnail system for all media
 - frame-stepping and cache-enhanced RW/FF
 - live [OCIO-based](https://opencolorio.org/) color correction switching with a node-based interface
 - live background switching for alpha-channel visibility
 - an annotation/notes system with PDF/Markdown/HTML export + [Frame.io](https://frame.io/home) import
 - embedded timecode for supported media
 - title-safety guides for standard broadcast and social-media deliverables


## Notes and Bugs:

- Timeline mode is currently only looking for QT Start time and XMP timecode in the metdata, but I am sure there are other metadata I can look for. I need to test more media.
- I have only tested Octane and Blender Cycles EXRs. I still need to test for and possibly adjust the code to support Redshift and other render engines.
- Image sequence info is barebones in the inspector panel at the moment. I would like to add to this--especially with EXRs.
- Audio has very basic visualization currently. I would like to rethink this.