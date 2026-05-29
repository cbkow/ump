---
title: Installation
permalink: /installation/
nav_order: 2
---

# Installation

## Windows

Download QCView from the [Microsoft Store](https://apps.microsoft.com/detail/9p4z15p5g805) for automatic updates:

<a href="https://apps.microsoft.com/detail/9p4z15p5g805?referrer=appbadge&mode=full" target="_blank" rel="noopener noreferrer">
  <img src="https://get.microsoft.com/images/en-us%20dark.svg" width="200"/>
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
