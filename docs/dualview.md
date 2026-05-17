---
title: Dual Video Review
permalink: /dual-view/
nav_order: 12
---

# Dual View

Unlike QCView v1, Dual Views are now a prominent part of the UI and are always available in a session. Above the viewport are an A chip and a B chip. To populate the A chip, drag it from a bin, or double-click on media in a bin like normal. The A chip is the media loaded on the left side of the dual-view comparison modes. To load the right side, you also drag from a media panel bin. Once loaded, use the buttons on the right side of this panel to toggle between side-by-side and split view modes for dual media review.

![A and B chips above the viewport](images/qcv027.jpg)

## View modes

Three viewport compositor modes for the same A / B pair:

| Mode | What you see |
|---|---|
| **Single** | Only the A source — quick toggle to inspect A in isolation |
| **Side-by-Side** | A on the left, B on the right, full-height divider in the middle |
| **Split-Wipe** | Single image with a vertical seam between A (left of seam) and B (right of seam) |

In **Split-Wipe**, the seam is mouse-draggable directly on the viewport — hover the seam line until the resize cursor appears, then click and drag. The slider in the viewport overlay stays in sync with the seam position.

![Split-wipe seam between A and B](images/qcv028.jpg)

## Audio

Per-side mute and an A/B level mixer live in the Inspector's Audio routing controls. Multi-stream broadcast deliveries route per channel, same as in single-source mode (see [App Basics](app-basics)). The right side is muted by default.

A separate **A/V Sync Offset** value is stored for dual view (different from the single-view value) — your offset for compositing two sources doesn't get applied to single-source playback and vice versa.


## Aligning Clips

Drag clips on the track or trim their ends to sync them. Use `Ctrl + K` to cut a clip at the playhead (for example to clip off slates or unwanted heads), then **Delete** to remove the unwanted segment.

In edit mode (the per-track edit toggle), the currently-selected clip brightens noticeably and its border switches to white so it's distinguishable from other edit-mode clips on the same track.

![Selected clip in dual-view edit mode](images/qcv029.jpg)

## Save / Recall

Use **Save as Dual View** (the floppy-disk button in the viewport overlay when a dual is loaded) to capture the current A + B + edits as a reusable item in the **Dual Views** bin of the Project panel.
