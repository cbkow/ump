---
title: Dual Video Review
permalink: /dual-view/
nav_order: 13
---

# Dual View

## Comparsion modes for two video sources

To compare two videos at once, click on the dual view button. Once in this mode, the primary (left and control) video is locked and the right video can be trimmed and slipped to align better.

![Window](images/ump_xJD4QMPlzS.png)

Then drag a second video from the *Project Manager* into the second timeline or the second window.

![Window](images/ump_pXRpUqPWvm.png)

---

## Dual View Modes

### Side-by-Side

There are two views you can pick from for editing and basic review. The default view is a left-to-right side-by-side.

![Window](images/ump_Rziflo5GY0.png)

### Split Screen

The second option is a split-screen view. You can drag the center line and adjust the split in this mode.

![Window](images/ump_WTFgCvhAEh.png)

---

## Lineing Up Both Videos

### Drag & Trim

Like the **Timeline** view, you can drag clips on the timeline or trim their ends.

![Window](images/explorer_9Xmi8dS7GG.png)

### Enter Trim Mode

Additionally, there is a dedicated Trim Mode. Right-click on the bottom/right layer and select `Trim Mode...`.

![Window](images/ump_SUd5461pSX.png)

This will bring up a pop-up window where you can set In `I` and Out `O` points and then trim the clip.

![Window](images/ump_XYjBVmuKzP.png)

---

## Lavfi modes

### Letting mpv do the work

Up until this point, we had been opening two videos in two separate video players and loosely syncing them. For a tighter sync between the two sources, we can load them into a lavfi combo in mpv. This allows for a perfect frame sync between the two. We have a few choices here: **Side-by-Side** Modes, **Top-Bottom** vertical stacks, **50/50 Split Screens**, or a **Difference Mode** view. Select one of these options to load these two videos together.

![Window](images/ump_4vYlo8Cypb.png)

Press this button to shut down the lavfi combo view and return to and editable dual view. 

![Window](images/ump_YVJJAtyP4x.png)