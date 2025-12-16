---
title: Duel Video Review
permalink: /dual-view/
nav_order: 13
---

# Dual View

## Comparsion modes for two video sources

To compare two videos at once, click on the dual view button. Once in this mode, the primary (left and control) video is locked and the right video can be trimmed and slipped to align better.

![Window](images/ump_vOCS2gstJ1.png)

Then drag a second video from the *Project Manager* into the second timeline or the second window.

![Window](images/ump_L2kEKjjz5J.png)

---

## Dual View Modes

### Side-by-Side

There are two views you can pick from for editing and basic review. The default view is a left-to-right side-by-side.

![Window](images/ump_V26gpvrYDx.png)

### Split Screen

The second option is a split-screen view. You can drag the center line and adjust the split in this mode.

![Window](images/ump_hL0wsuMr2X.png)

---

## Lineing Up Both Videos

### Drag & Trim

Like the **Timeline** view, you can drag clips on the timeline or trim their ends.

![Window](images/explorer_xDBvlW6zUd.png)

### Enter Trim Mode

Additionally, there is a dedicated Trim Mode. Right-click on the bottom/right layer and select `Trim Mode...`.

![Window](images/Code_nzKyqjDPIH.png)

This will bring up a pop-up window where you can set In `I` and Out `O` points and then trim the clip.

![Window](images/explorer_ngD5rdzNuM.png)

---

## Lavfi modes

### Letting mpv do the work

Up until this point, we had been opening two videos in two separate video players and loosely syncing them. For a tighter sync between the two sources, we can load them into a lavfi combo in mpv. This allows for a perfect frame sync between the two. We have a few choices here: Side-by-Side Modes, Top-Bottom vertical stacks, 50/50 split screens, or a Difference mode view. Select one of these options to load these two videos together.

![Window](images/explorer_mZ0dCXHhwn.png)

Press this button to shut down the lavfi combo view and return to and editable dual view. 

![Window](images/ump_cHsoWUwia3.png)