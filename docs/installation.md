---
title: Installation
permalink: /installation/
nav_order: 2
---

# Installation

## Windows

Download QCView from the Microsoft Store for automatic updates:

<a href="https://apps.microsoft.com/detail/9p4z15p5g805?referrer=appbadge&mode=full" target="_blank" rel="noopener noreferrer">
  <img src="https://get.microsoft.com/images/en-us%20dark.svg" width="200"/>
</a>

Alternatively, download the unsigned `.exe` installer directly from [GitHub Releases](https://github.com/cbkow/QCView-Player/releases) if you need a specific version or prefer to install outside the Store.

---

## macOS

Download the signed and notarized `.dmg` from [GitHub Releases](https://github.com/cbkow/QCView-Player/releases).

1. Open the `.dmg` file
2. Drag **QCView** to your **Applications** folder
3. Launch from Applications or Spotlight

### Requirements

- macOS 13.0 (Ventura) or later
- Apple Silicon (arm64) — native Metal rendering, no Rosetta
- EDR-capable display recommended for HDR workflows

---

## Linux (Experimental)

The Linux build uses **Vulkan** for rendering and is currently experimental. It has been tested on **Kubuntu with KDE Plasma 6** running **Wayland**.

Download `.deb` or `.rpm` packages from [GitHub Releases](https://github.com/cbkow/QCView-Player/releases).

### DEB (Debian/Ubuntu)

```bash
sudo dpkg -i QCView-<version>-amd64.deb
sudo apt install -f  # install any missing dependencies
```

### Uninstalling

Uninstallation is handled by the package manager:

```bash
# DEB
sudo apt remove qcview

# RPM
sudo dnf remove qcview
```

### Requirements

- Vulkan-capable GPU with up-to-date drivers
- VA-API for hardware-accelerated video decoding (optional, falls back to FFmpeg software decode)
- PipeWire for audio playback
- Wayland session recommended