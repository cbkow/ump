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

![Window](images/ump_ydFsO4WezD.png)

### Select a Config

First, select a config that matches your DLC.

![Window](images/ump_uEnl8hpK8x.png)

### Select a Input Node

Then, select an **Input** Node and drag it into the **Node Graph** panel.

![Window](images/ump_XGluoayzve.png)

### Select a Output Node

Select and output node, drag it into the graph, and hook them up.

![Window](images/explorer_0z6uiKV5EV.png)

### Generate a Shader

![Window](images/ump_EXrXTwjwRl.png)

Click on the output node to load a settings panel on the right side of the color panel layout. Select a View if the **Output** node requires one. Then click the `Generate Shader` button. You will see the shader applied in your Viewer. Click `Remove Shader` to undo. 

![Window](images/ump_uunzFGdQMh.png)

---

## Node Management

You can click on any node or branch and use the keyboard's `X` or `Delete` to remove it.

![Window](images/ump_2yqaN6B8CU.png)

---

## LUTS

You can add custom LUTs to the chain. **Scene LUTs** go before the **Output** node, and **Display LUTs** go after. Click on the `Select LUT File` button to assign a .cube file. 

![Window](images/ump_bL9of4xkez.png)

---

## Looks

**Looks** belong in the chain between **Input** and **Output**. All Blender looks are supported (Contrast looks, AGX looks, Greyscale looks, Punchy, etc...)

![Window](images/ump_fUG6wiUesZ.png)

---

## Presets

Presets are provided for each included config. Click on the preset to apply.

![Window](images/ump_uPFJ32z6TF.png)

### Custom Presets

Save your own if you would like.

![Window](images/ump_2LrIQX51pb.png)

