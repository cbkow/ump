---
title: Image Sequences
permalink: /image-sequences/
nav_order: 10
---

# Image Sequences

## Loading a Sequence

Open or drag a single image from a sequence, and QCView automatically detects the rest. Choose a frame rate, and for multi-layer EXR files, select which layer to load.

![Loading an image sequence](images/QCView_v046.webp)

> **Performance note:** Large multi-layer EXRs and high-resolution TIFF sequences (4K+) can exceed what CPU-bound decompression can deliver in real time, regardless of thread count. QCView offers transcoding to work around this — see below.

---

## Transcoding

For smoother playback of heavy sequences, transcode them into a lighter format before reviewing. Choose a resolution and compression scheme.

![Transcode options](images/QCView_v047.webp)

**Recommended compression:** `B44A` decompresses significantly faster than `DWAA`/`DWAB`. It trades some quality for speed, but is well-suited for review playback.

![Transcode settings](images/QCView_v048.webp)

---

## Playback Cache

Image sequences are cached to RAM and uploaded to the GPU as you traverse the timeline. The cache progress bar shows how much read-ahead and read-behind is available at the current position.

![Cache progress bar](images/QCView_v049.webp)

### Cache Settings

Adjust cache size and behavior in the **Pipeline & Cache Settings** panel. See the [Settings](settings) page for details.

### Disk Cache for Transcodes

Transcoded sequences are stored in a temporary disk cache. For best performance, set this to a fast NVMe drive rather than your system drive.

![Disk cache settings](images/QCView_v051.webp)

> The playback cache is RAM-based and subject to the Memory Safety system. If system RAM is full, image playback will pause. See the [Memory](memory) page for details.

> You can switch EXR layers after loading — see the [Inspector](inspector) page.

---

## Broken and Missing Frames

QCView detects incomplete sequences and fills gaps with a transparent texture so you can review in-progress renders. Files under 15 KB are flagged as corrupt and replaced with a placeholder frame rather than attempting to decode them.

![Broken frame indicator](images/QCView_v050.webp)