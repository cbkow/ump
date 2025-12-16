---
title: Home
permalink: /
nav_order: 1
---

# u.m.p.

![ump image](images/ump_4JAA0LYh5q.jpg)

## Overview

**u.m.p.** is a [Dear ImGui](https://github.com/ocornut/imgui) C++ [libmpv-based](https://mpv.io/) OpenGL video player with memory-mapped playback for EXR, PNG, TIFF, and JPEG image sequences for Windows. Basic features include:

 - RAM cache for scrubbing/seeking in videos, image sequence playback, and timelines
 - [OTIO](https://github.com/OpenTimelineIO) timelines for image sequences and timelines 
 - a custom playlist manager for mpv's playlist system
 - a [Thumbfast](https://github.com/po5/thumbfast)-inspired thumbnail preview panel
 - frame-stepping and cache-enhanced RW/FF
 - live [OCIO-based](https://opencolorio.org/) color correction switching with a node-based interface
 - live background switching for alpha-channel media review
 - an annotation/notes system with PDF/Markdown/HTML export + [Frame.io](https://frame.io/home) import
 - extracts embedded timecode in supported media 
 - title-safety guides for standard broadcast and social-media deliverables
 - screenshots from all media

---

![ump image](images/ump_2uG3ZAfllI.png)

## Notes and Bugs:

I have only tested Octane, Arnold, and Blender Cycles EXRs. I still need to test for and possibly adjust the code to support Redshift and other render engines.

---

## Source Code

u.m.p. is open souce. It's a personal, in-house app, but free to use and alter. It's only Licenesed GPL due to dependancies such FFMPEG and Exiftool. [https://github.com/cbkow/ump](https://github.com/cbkow/ump)