---
title: Transcoding
permalink: /transcoding/
nav_order: 12
---

# Transcoding Image Sequences or Videos

## Right click to add to Queue

To add a transcode to the **Queue Manager**, right-click on the file or files and submit.

![Window](images/ump_a8Ww1t8nMP.png)

- You can select different output formats, including H. 264, H. 265, ProRes 422LT, ProRes 422HQ, and ProRes 4444 (4444 will carry over Alpha information).
- Selecting the `Use Current OCIO Settings` option will apply the current color correction flow to the video outputs.


![Window](images/ump_mvIrWECryx.png)

Submitting will add to the **Queue Manager** and automatically begin transcoding.

![Window](images/ump_8IGPmFFudT.png)

## Loop in/out points

![window](images/ump_qSX9lzvuTy.png)
The loop in/out functions will also trim a region for transcoding. If you don't want to export the whole video/seqeunce you can trim here.

Pressing `:` or toggling the loop-in button will set an in point, then pressing `'` or toggling the loop out button will set an out point. Once the loop area is selected, only this constrained region will export for transcoding.