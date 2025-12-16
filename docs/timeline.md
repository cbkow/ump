---
title: Timeline Controls
permalink: /timeline-transport/
nav_order: 5
---

# Timeline and Transport Controls

The bottom of the **Viewport** panel contains controls for media playback and app state shortcuts. It dynamically shifts based on the loaded media and presents appropriate options. 

![window](images/ump_5PKciehAgY.png)

## Transport Controls

Timecode mode looks for QT start time or XMP timecode in video metadata and changes the time counter to match the timecode of the video.

### Transport Controls

Media navigation is controlled with these buttons:

* Previous video in playlist (only available in playlist mode)
* Beginning of media
* Rewind (press and hold–it will speed up over time)
* Back one frame
* Play/Pause
* Forward one frame
* Fast Forward (press and hold–it will speed up over time)
* End of media
* Next video in playlist (only available in playlist mode)

![window](images/ump_uOfVbnvZe8.png)

---

### Overlays

These three buttons toggle overlay selectors:

* Colorspace Presets
* Safety Guide Overlays
* Background colors/patterns.

![window](images/ump_VbwPwqOKM4.png)

#### Color Presets

Color Presets trigger OCIO node trees for commonly accessed color correction flows. Click any to activate the flow, and click `Remove All Color Profiles` to remove any preset applied.

![window](images/ump_qiVKQXG5ME.png)

#### Safety Overlays

The **Safety Overlays** panel triggers title safety overlays in accordance with common broadcast and social media standards. You can control opacity, line thickness, and color. The app will remember your preferences for these variables. 

![window](images/ump_4c3CuWCcOp.png)

#### Background

This panel allows you to change the **Viewer’s** background color and pattern. This background will be presented behind the media, with alpha channels, as well as in the **Viewer's** periphery.

Options are:

* A plain black background
* A default sold grey background
* A dark variation of the mpv-style checkerboard background
* A recreation of the default light-themed mpv checkerboard background

![window](images/ump_8qgyVRhPrr.png)

#### 

---

### Screenshots

These two buttons allow you to take screenshots of the **Viewer**. The first saves the screenshot to the Windows clipboard so you can paste it into other apps. The second saves a screenshot to your Desktop.

![window](images/ump_Ojwleo4WTq.png)

---

### Panel Toggles

The buttons toggle commonly used app panels including:

- The Inspector
- The Project Manager
- The OCIO color panels
- Annotations / Notes
- Miminal mode (just the viewport and timeline)

![window](images/ump_6Sj2YsykIL.png)

---

### Timecode mode

The Timecode mode button is a toggle state that searches the loaded media for embedded timecode.

![window](images/ump_44NZ4FZ9ih.png)

If the loaded media item has embedded timecode, our timecode readout will adjust to display it. 

![window](images/ump_RnrI923Q8v.png)

If you click on the button to the right of the timecode and frame counter, you can navigate to a specific timecode or frame in the timeline.

![window](images/ump_PdJlTR7IHX.png)

---

### The Timeline

You can zoom into a tighter presentation of this timeline visual by using `Ctrl + Mouse Scroll`. If you `Ctrl + Middle-click` this area, you can pan it. Triggering the `Mouse scroll wheel` without `Ctrl` pans it as well. 

![window](images/ump_GsLLrIfwud.png)

When zoomed in, you may also click on this representation of the zoom zone and drag to pan the timeline.

![window](images/ump_vZfKlj7oTv.png)

---

### Volume, Mute, Loop, and Follow-Playhead Mode

Sliding the Volume slider adjusts the volume, and clicking the speaker button toggles Mute mode. Clicking the Loop button will trigger a loop mode for single media, playlists, or timelines—-depending on the media loaded. The `F*` button signifies Follow Playhead Mode. When this is toggled, and the timeline is zoomed in, it will auto-pan to keep the playhead in view.

![window](images/ump_LlVwycO4LJ.png)

---

### Loop Zones

The keyboard command `I` and `O`, as well as the two buttons in this section, will toggle In and Out points on your timeline. Once an Out point is toggled, a Loop Zone will appear. This visual signifies a set range within which the media will play. Pressing `Clear` releases the Loop Zone and lets you play the entire clip again. 

![window](images/ump_rSNUJutW6B.png)

---

### Zoom / Pan

In addition to your middle mouse button, you can use these sliders to zoom and pan a timeline.

![window](images/ump_loXCSy9iUw.png)