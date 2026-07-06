---
title: Installation
permalink: /installation/
nav_order: 2
---

# Installation

## Windows

Download QCView from the [Microsoft Store](https://apps.microsoft.com/detail/9p4z15p5g805) for automatic updates:

<a href="https://apps.microsoft.com/detail/9p4z15p5g805?referrer=appbadge&mode=full" target="_blank" rel="noopener noreferrer">
  <img src="https://get.microsoft.com/images/en-us%20dark.svg" width="200" style="border: 1px solid #707070; border-radius: 4.5px;"/>
</a>

### Requirements

- Windows 10 version 1809 or later (10.0.17763.0), x64.
- Vulkan-capable GPU with up-to-date drivers (FFmpeg software fallback when unavailable).
- HDR-capable display + Windows HDR mode enabled for HDR10 output.

---

## macOS

Download the latest release [here](https://github.com/cbkow/QCView-Player/releases/latest/).

<a href="https://github.com/cbkow/QCView-Player/releases/latest/download/QCView-MacOS.dmg" target="_blank" rel="noopener noreferrer">
  <img src="https://qcview.app/images/download.png" width="200"/>
</a>

1. Open the `.dmg` file.
2. Drag **QCView** to your **Applications** folder.
3. Launch from Applications or Spotlight.

### Updates

Once installed in **Applications**, QCView keeps itself up to date — it checks once a day and offers to download and install new versions in place. You can also check any time from **About → Check for Updates…**, or turn automatic checks off in **Settings → Updates**. (In-place updates require running QCView from the Applications folder, not from the mounted disk image.)

### Requirements

- macOS 13.0 (Ventura) or later.
- Apple Silicon (arm64) — native Metal rendering, no Rosetta.
- EDR-capable display recommended for HDR workflows.

---

## Version History

What’s new in 2.1.9
- Improved audio scrubbing. It still pitch-shifts when slower than 1x speed, but no longer pitch-shifts when faster than 1x
- Minimized latency when scrubbing
- Fixed a bug when loading a solitary EXR image.

What’s new in 2.1.8
- Improved audio sync and added pitch-shifting audio for scrubbing + FF/RW 

What’s new in 2.1.5
- tighted up the UI in the timeline, tracks and other elements are less tall
- sidebar rails now fully close
- there is a new setting to always load the app in minimal mode
- a new tooltip and modal system is added, and more tooltips are included

What’s new in 2.1.4

- fixed an issue with odd-width videos with the software scrubbing path on Windows
- added triangle markers on the timeline for notes

What’s new in 2.1.3

- Moved screenshot exporting to its own thread.
- Added options for screenshot exports in settings: PNG, TIFF, and JPEG
- Added messaging on the bottom toolbar for screenshot progress

What’s new in 2.1.1/2.10

- Improved scrubbing for MP4s and other B-frame media
- This release is v2.1.1 on MacOS and v2.1.0 for Windows. Windows didn't need the last-minute regression fix. We will get back in sync for the next release.

What’s new in 2.0.9

- Added track identifiers to the left of the timeline; this is mainly to keep the playhead and grab handles of the zoom/pan tool away from the app’s edges.
- Fixed persistence on image sequence FPS when switching to other media and back.
- Added a dual-view panel for saving dual-view sessions and moved dual-views out of the media panel
- Made the entire surface of collapsed sidebars a trigger to open the rails.
- Removed the max size from the media bin—-eliminating dual scroller contention
- Fixed a glitch where the minimal mode menu option hid the frame counter row
- Added detection for ARRI RAW mxfs and a message to guide users to Resolve or other apps
- Added video range overrides to the scrub decoders (single and dual media flows)
- Added a dark background behind the MacOS icon for light-mode users.

What’s new in 2.08

- Editing in dual-view and playlist modes has better viewport feedback with slips and trims.
- Added a difference mode option for dual-views
- MacOS now checks for and optionally installs updates

What's new in 2.06
- UI tweaks for better panel and selection legibility
- UI tweaks to the playhead in dual-view and playlist edit modes
- Better scrubbing in dual-view modes with video sources
- Added grab handles to the timeline's zoom/pan tool
- Fixed an edge-case crash in playlist mode with thumbnails
- Added a height limit to the thumbnails so vertical videos don't occupy so much viewport space
- Added a loading spinner--especially helpful for large projects that take a moment to load

What's new in 2.04
- Slight UI change to playhead when a timeline track is in edit mode.
- Fixed a bug where thumbnails didn't respect clip edits in dual-view mode.

What's new in 2.03
- Fixed issues in dual view mode when two clips with different FPS were compared.
- Fixed issues with dual view when two ProRes clips were loaded in systems with NVIDIA gpus.

What's new in 2.02
- fixed glitches with windows resizing.
- fixed glitches with the playhead flickering on seek operations.

What's new in 2.01
- new UI and backend - a complete rewrite of the frontend.
- stereo audio mixing for 5.1 SMTPE broadcast master video review.
- added extended linear sRGB as an HDR option for Windows.
- OCIO default presets are now based on app display mode.
- vulkan HW accelerated decoding for ProRes in Windows.
- new dual view and playlist flows. new edit modes for both.
