---
title: OCIO Color Nodes
permalink: /ocio-nodes/
nav_order: 8
---

# OCIO Color Nodes

QCView includes a node-based [OpenColorIO](https://opencolorio.org/) pipeline for live color management. Build transform chains visually and see results applied to the Viewer in real time.

**Bundled configs:** ACES 1.3, ACES 2.0, Blender 4.5, Blender 5.0

![Inspector panel for video](images/QCView_v027.webp)

---

## Building a Node Tree

A color transform requires at minimum an **Input** node and an **Output** node. Follow these steps:

### 1. Select a Config

Choose a config that matches your color pipeline.

![Config selection](images/QCView_v028.webp)

### 2. Add an Input Node

Drag an **Input** node into the Node Graph panel.

![Input node](images/QCView_v029.webp)

### 3. Add an Output Node

Drag an **Output** node into the graph.

![Output node](images/QCView_v030.webp)

### 4. Connect and Apply

Connect the nodes, then click the Output node to open its settings. Select a **View** if required, then click **Apply** to see the transform in the Viewer. Click **Remove** to clear it.

![Applied color transform](images/QCView_v031.webp)

---

## Node Management

Select any node or connection and press `X` or `Delete` to remove it.

![Deleting a node](images/QCView_v032.webp)

---

## LUTs

Add custom LUT files (.cube) to the transform chain:

| LUT Type | Position in Chain |
|---|---|
| Scene LUT | Before the Output node |
| Display LUT | After the Output node |

Click **Select LUT File** on the node to assign a .cube file.

![LUT node](images/QCView_v033.webp)

---

## Looks

**Look** nodes go between Input and Output. All Blender looks are supported — Contrast, AgX, Greyscale, Punchy, and more.

![Look node](images/QCView_v035.webp)

---

## Presets

Each bundled config includes ready-made presets. Click a preset to apply it instantly.

![Built-in presets](images/QCView_v036.webp)

### Custom Presets

Save your own node tree configurations as presets for quick recall.

![Saving a custom preset](images/QCView_v038.webp)

---

## Exporting LUTS

Click here to export a Cube LUT for other software/pipelines.

![Saving a custom preset](images/QCView_v039.webp)
