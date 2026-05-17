// DualViewLayout — direct lift from old QCView's
// src/gpu/dual_view_layout.{h,cpp} per Guide 19 §2.3.
//
// Pure CPU compositor math. Used by the player's compositor pass to
// place LEFT and RIGHT video frames inside the composite texture
// (single-source = LEFT only; SBS / Wipe = both with horizontal or
// vertical layout depending on aspect + GPU texture limits).
//
// No GPU dependencies; reusable across Metal + Vulkan render paths.

#pragma once

#include <algorithm>

namespace qcv {

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

    // -------------------------------------------------------------------
    // UV coordinates for sampling each half of the composite texture
    // -------------------------------------------------------------------
    struct UVRect {
        float u_min = 0.0f;
        float u_max = 1.0f;
        float v_min = 0.0f;
        float v_max = 1.0f;
    };

    UVRect left_uv;                   // UV rect for left source in composite
    UVRect right_uv;                  // UV rect for right source in composite

    // -------------------------------------------------------------------
    // Viewport rects (in pixels) for compositor shader
    // Defines where each source is rendered within the composite texture
    // -------------------------------------------------------------------
    int left_x  = 0;
    int left_y  = 0;
    int left_w  = 0;
    int left_h  = 0;

    int right_x = 0;
    int right_y = 0;
    int right_w = 0;
    int right_h = 0;

    bool isValid() const {
        return composite_width > 0 && composite_height > 0 &&
               left_w > 0 && left_h > 0 &&
               right_w > 0 && right_h > 0;
    }

    bool needsUpdate(int new_left_w, int new_left_h,
                     int new_right_w, int new_right_h) const {
        return new_left_w  != left_source_width  ||
               new_left_h  != left_source_height ||
               new_right_w != right_source_width ||
               new_right_h != right_source_height;
    }
};

// Computes optimal layout for compositing two video sources.
//
// Layout strategy:
// - Horizontal (side-by-side): Preferred when it fits the GPU texture limit
// - Vertical (top-bottom): Used when horizontal would exceed the limit
// - Scaled: Applied if both orientations would exceed the limit
//
// GPU texture limits (typical): 16384x16384.
DualViewLayout computeDualViewLayout(int left_width,  int left_height,
                                     int right_width, int right_height,
                                     int gpu_texture_limit = 16384);

// Computes aspect-ratio-preserving fit of source into target rect.
// Used for letterboxing/pillarboxing when sources have different aspects.
struct FitRect {
    int x, y, w, h;
};

FitRect computeAspectFitRect(int source_w, int source_h,
                             int target_x, int target_y,
                             int target_w, int target_h);

} // namespace qcv
