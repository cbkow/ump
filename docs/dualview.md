---
title: Dual Video Review
permalink: /dual-view/
nav_order: 12
---

# Dual View

To compare two media items side by side, create a Dual View setup via right-click in the Dual View bin or **File > New Dual View**.

The setup has **LEFT** and **RIGHT** tracks. Drag media into each track, or drop it directly into the viewport. If image sequences are loaded, each will have its own cache progress bar.

> **How frame sync works:** Both media items are decoded in parallel and written to a shared memory-mapped texture. Each side of the viewer UV-maps to its half of that texture, which guarantees accurate frame alignment. Sync is time-based rather than frame-based, so media with different frame rates will align by timestamp.

![Dual view layout](images/QCView_v053.webp)

The **Split Screen** button switches to a draggable divider view you can move freely during playback.

![Split screen view](images/QCView_v054.webp)

---

## Audio

Toggle both sources together to mix all audio, or solo one side. The right side is muted by default.

![Audio controls](images/QCView_v055.webp)

---

## Aligning Clips

You can drag clips or trim their ends to sync them up. Use `Ctrl + K` or the **Edit** button to trim slates.

![Timeline alignment](images/QCView_v056.webp)
