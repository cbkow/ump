---
title: Transcoding
permalink: /transcoding/
nav_order: 12
---

# Transocoding Image Sequences or Videos

## Right click to add to Queue

To add a transcode to the **Queue Manager**, right-click on the file or files and submit.

![Window](images/ump_a8Ww1t8nMP.png)

- You can select different output formats, including H. 264, H. 265, ProRes 422LT, ProRes 422HQ, and ProRes 4444 (4444 will carry over Alpha information).
- Selecting the `Use Current OCIO Settings` option will apply the current color correction flow to the video outputs.


![Window](images/ump_mvIrWECryx.png)

On the topic of RAM safety, there is also a System-Stats panel that could be helpful to diagnose performance issues. If your system surpasses 92% RAM capacity, this will automatically appear, and all memory options will cease until RAM has safely lowered. This will mean that you will not be able to play image sequences until RAM is free--they are 100% reliant on memory-mapping textures.

![Window](images/ump_8IGPmFFudT.png)

Submitting will add to the **Queue Manager** and automatically begin transcoding.