---
title: HDR
permalink: /hdr/
nav_order: 17
---

# SDR vs HDR

Unlike most media players, u.m.p. does not tonemap media. Other players make assumptions about color space that are at least partially wrong — or at minimum, inconvenient for a professional review workflow.

As a result, video output is presented in Windows' working color space. In SDR desktop mode, Windows assumes `sRGB`, so SDR video displays correctly without any adjustment. In HDR mode, Windows assumes `Rec.2100 PQ`, so HDR content (such as PQ HEVC) displays correctly — but SDR content will not. In that case, you'll need to use OCIO to convert.

---

## Color Output with OCIO

### SDR Mode

In SDR mode, use an `sRGB` output node for a standard display-referred result. If you want to preview how content will look on a typical consumer display — `~Rec.709 / Gamma 2.4` — any `Rec.1886` option will work. (Given the general state of video color management on consumer hardware, this is how most viewers will see your content.)

![Window](images/QCView_v064.webp)

---

### HDR Mode

![Window](images/ApplicationFrameHost_54kNfGakT2.png)

With Windows HDR mode enabled, you can present video in `Rec.2100 PQ`. To view SDR content in this mode, use the appropriate Display node to transform it into that space.

![Window](images/QCView_v065.webp)