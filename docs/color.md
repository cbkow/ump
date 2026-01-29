---
title: OCIO Color Nodes
permalink: /ocio-nodes/
nav_order: 8
---

# OCIO Color Panels

## The Panels

The OCIO color panel is a basic node builder for OCIO flows. Currently, u.m.p supports `ACES 1.3`, `ACES 2.0`, `Blender 4.5`, and the `Blender 5.0` configs. 

---

## The Flow

### Using OCIO Configs

To build an OCIO node tree, you need at least an **Input** node and an **Output** node. 

![Window](images/ump_RZvPsHv4pN.png)

### Select a Config

First, select a config that matches your DLC.

![Window](images/ump_ojcMYDhlai.png)

---

### Select a Input Node

Then, select an **Input** Node and drag it into the **Node Graph** panel.

![Window](images/ump_c305B3V4dx.png)

---

### Select a Output Node

Select and output node, drag it into the graph... 

![Window](images/ump_hTG1j6Hqfb.png)

...and hook them up.

![Window](images/ump_OY9cn0lZEI.png)

---

### Generate a Shader

Click on the output node to load a settings panel on the right side of the color panel layout. Select a View if the **Output** node requires one. Then click the `Apply` button. You will see the shader applied in your Viewer. Click `Remove` to undo. 

![Window](images/ump_ZmV4S85RZU.png)

---

## Node Management

You can click on any node or branch and use the keyboard's `X` or `Delete` to remove it.

![Window](images/ump_NFfYMeRh0z.png)

---

## LUTS

You can add custom LUTs to the chain. **Scene LUTs** go before the **Output** node, and **Display LUTs** go after. Click on the `Select LUT File` button to assign a .cube file. 

![Window](images/ump_2kJHGwc1y7.png)

---

## Looks

**Looks** belong in the chain between **Input** and **Output**. All Blender looks are supported (Contrast looks, AGX looks, Greyscale looks, Punchy, etc...)

![Window](images/ump_8iDIYyPewt.png)

---

## Presets

Presets are provided for each included config. Click on the preset to apply.

![Window](images/ump_HwoJpctLLg.png)

### Custom Presets

Save your own presets.

![Window](images/explorer_RG1aD8HfMc.png)

