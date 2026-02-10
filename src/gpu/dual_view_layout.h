#pragma once

#include <algorithm>

namespace ump {

//=============================================================================
// DualViewLayout
//
// Describes the layout of a composite dual view texture.
// The composite texture contains both LEFT and RIGHT video frames
// arranged either side-by-side (horizontal) or top-bottom (vertical).
//
// This enables a single D3D11->GL interop instead of two competing interops,
// solving the buffer exhaustion issue that occurred with dual independent interops.
//=============================================================================

struct DualViewLayout {
    // Layout orientation
    bool horizontal = true;           // true = side-by-side, false = top-bottom

    // Composite texture dimensions
    int composite_width = 0;          // Total texture width
    int composite_height = 0;         // Total texture height

    // Scale factor if sources were downsampled to fit GPU limits
    float scale = 1.0f;

    // Original source dimensions (before any scaling)
    int left_source_width = 0;
    int left_source_height = 0;
    int right_source_width = 0;
    int right_source_height = 0;

    //=========================================================================
    // UV coordinates for sampling each half of the composite texture
    // Used by OpenGL/ImGui to display LEFT and RIGHT panels
    //=========================================================================

    struct UVRect {
        float u_min = 0.0f;
        float u_max = 1.0f;
        float v_min = 0.0f;
        float v_max = 1.0f;
    };

    UVRect left_uv;                   // UV rect for left source in composite
    UVRect right_uv;                  // UV rect for right source in composite

    //=========================================================================
    // Viewport rects (in pixels) for compositor shader
    // Defines where each source is rendered within the composite texture
    //=========================================================================

    int left_x = 0;
    int left_y = 0;
    int left_w = 0;
    int left_h = 0;

    int right_x = 0;
    int right_y = 0;
    int right_w = 0;
    int right_h = 0;

    //=========================================================================
    // Helper methods
    //=========================================================================

    bool IsValid() const {
        return composite_width > 0 && composite_height > 0 &&
               left_w > 0 && left_h > 0 &&
               right_w > 0 && right_h > 0;
    }

    // Check if this layout needs recomputation based on new source dimensions
    bool NeedsUpdate(int new_left_w, int new_left_h,
                     int new_right_w, int new_right_h) const {
        return new_left_w != left_source_width ||
               new_left_h != left_source_height ||
               new_right_w != right_source_width ||
               new_right_h != right_source_height;
    }
};

//=============================================================================
// ComputeDualViewLayout
//
// Computes optimal layout for compositing two video sources.
//
// Layout strategy:
// - Horizontal (side-by-side): Preferred for most resolutions
// - Vertical (top-bottom): Used when horizontal would exceed GPU texture limit
// - Scaled: Applied if both orientations would exceed limits
//
// GPU texture limits (typical): 16384x16384
// Examples:
// - Two 4K plates (3840x2160): 7680 wide -> horizontal OK
// - Two 6K plates (6144x3160): 12288 wide -> horizontal OK
// - Two 8K plates (7680x4320): 15360 wide -> horizontal OK
// - Two 12K plates: May need vertical or scaling
//=============================================================================

DualViewLayout ComputeDualViewLayout(int left_width, int left_height,
                                      int right_width, int right_height,
                                      int gpu_texture_limit = 16384);

//=============================================================================
// ComputeAspectFitRect
//
// Computes aspect-ratio-preserving fit of source into target rect.
// Used for letterboxing/pillarboxing when sources have different aspects.
//=============================================================================

struct FitRect {
    int x, y, w, h;
};

FitRect ComputeAspectFitRect(int source_w, int source_h,
                              int target_x, int target_y,
                              int target_w, int target_h);

} // namespace ump
