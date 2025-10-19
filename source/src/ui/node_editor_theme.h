#pragma once
#include "../imnodes/imnodes.h"
#include <imgui.h>

namespace NodeEditorTheme {
    void ApplyDarkTheme();
    void ApplyNodeTypeColors();

    // Color constants for different node types
    namespace Colors {
        // Input nodes (camera/log colorspaces)
        constexpr ImU32 INPUT_NODE_TITLE = IM_COL32(70, 130, 180, 255);        // Steel blue
        constexpr ImU32 INPUT_NODE_TITLE_SELECTED = IM_COL32(90, 150, 200, 255);

        // Transform nodes (colorspace conversions)
        constexpr ImU32 TRANSFORM_NODE_TITLE = IM_COL32(60, 179, 113, 255);    // Medium sea green
        constexpr ImU32 TRANSFORM_NODE_TITLE_SELECTED = IM_COL32(80, 199, 133, 255);

        // Look nodes (creative looks/LUTs)
        constexpr ImU32 LOOK_NODE_TITLE = IM_COL32(218, 112, 214, 255);        // Orchid
        constexpr ImU32 LOOK_NODE_TITLE_SELECTED = IM_COL32(238, 132, 234, 255);

        // CDL/Grading nodes
        constexpr ImU32 CDL_NODE_TITLE = IM_COL32(255, 140, 0, 255);           // Dark orange
        constexpr ImU32 CDL_NODE_TITLE_SELECTED = IM_COL32(255, 160, 20, 255);

        // Output nodes (displays/delivery)
        constexpr ImU32 OUTPUT_NODE_TITLE = IM_COL32(220, 20, 60, 255);        // Crimson
        constexpr ImU32 OUTPUT_NODE_TITLE_SELECTED = IM_COL32(240, 40, 80, 255);

        // Connection colors - now dynamic based on Windows accent color
        // These will be updated in ApplyDarkTheme() using the accent color
        constexpr ImU32 CONNECTION_DEFAULT = IM_COL32(100, 140, 200, 255);     // Fallback blue
        constexpr ImU32 CONNECTION_HOVERED = IM_COL32(120, 160, 220, 255);     // Fallback brighter blue
        constexpr ImU32 CONNECTION_SELECTED = IM_COL32(255, 150, 50, 255);     // Orange for selection
    }
}