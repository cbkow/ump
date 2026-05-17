// DualViewLayout — direct lift from old QCView's
// src/gpu/dual_view_layout.cpp per Guide 19 §2.3.

#include "dual_view_layout.h"

#include <QtLogging>
#include <algorithm>
#include <cmath>

namespace qcv {

FitRect computeAspectFitRect(int source_w, int source_h,
                             int target_x, int target_y,
                             int target_w, int target_h)
{
    FitRect result = {target_x, target_y, target_w, target_h};

    if (source_w <= 0 || source_h <= 0 || target_w <= 0 || target_h <= 0) {
        return result;
    }

    const float source_aspect = static_cast<float>(source_w) / source_h;
    const float target_aspect = static_cast<float>(target_w) / target_h;

    if (source_aspect > target_aspect) {
        // Source is wider — letterbox (bars on top/bottom)
        result.w = target_w;
        result.h = static_cast<int>(target_w / source_aspect);
        result.x = target_x;
        result.y = target_y + (target_h - result.h) / 2;
    } else {
        // Source is taller — pillarbox (bars on left/right)
        result.h = target_h;
        result.w = static_cast<int>(target_h * source_aspect);
        result.x = target_x + (target_w - result.w) / 2;
        result.y = target_y;
    }

    return result;
}

DualViewLayout computeDualViewLayout(int left_width,  int left_height,
                                     int right_width, int right_height,
                                     int gpu_texture_limit)
{
    DualViewLayout layout;

    if (left_width <= 0 || left_height <= 0 ||
        right_width <= 0 || right_height <= 0) {
        qWarning("computeDualViewLayout: invalid source dimensions");
        return layout;
    }

    layout.left_source_width   = left_width;
    layout.left_source_height  = left_height;
    layout.right_source_width  = right_width;
    layout.right_source_height = right_height;

    const int horizontal_width  = left_width + right_width;
    const int horizontal_height = std::max(left_height, right_height);

    const int vertical_width  = std::max(left_width, right_width);
    const int vertical_height = left_height + right_height;

    auto setUVsFromRects = [&layout]() {
        const float inv_w = 1.0f / static_cast<float>(layout.composite_width);
        const float inv_h = 1.0f / static_cast<float>(layout.composite_height);

        layout.left_uv.u_min = static_cast<float>(layout.left_x) * inv_w;
        layout.left_uv.u_max = static_cast<float>(layout.left_x + layout.left_w) * inv_w;
        layout.left_uv.v_min = static_cast<float>(layout.left_y) * inv_h;
        layout.left_uv.v_max = static_cast<float>(layout.left_y + layout.left_h) * inv_h;

        layout.right_uv.u_min = static_cast<float>(layout.right_x) * inv_w;
        layout.right_uv.u_max = static_cast<float>(layout.right_x + layout.right_w) * inv_w;
        layout.right_uv.v_min = static_cast<float>(layout.right_y) * inv_h;
        layout.right_uv.v_max = static_cast<float>(layout.right_y + layout.right_h) * inv_h;
    };

    if (horizontal_width <= gpu_texture_limit && horizontal_height <= gpu_texture_limit) {
        // Horizontal layout (preferred)
        layout.horizontal       = true;
        layout.composite_width  = horizontal_width;
        layout.composite_height = horizontal_height;
        layout.scale            = 1.0f;

        const FitRect left_fit = computeAspectFitRect(
            left_width, left_height, 0, 0, left_width, horizontal_height);
        layout.left_x = left_fit.x; layout.left_y = left_fit.y;
        layout.left_w = left_fit.w; layout.left_h = left_fit.h;

        const FitRect right_fit = computeAspectFitRect(
            right_width, right_height, left_width, 0, right_width, horizontal_height);
        layout.right_x = right_fit.x; layout.right_y = right_fit.y;
        layout.right_w = right_fit.w; layout.right_h = right_fit.h;

        setUVsFromRects();
    } else if (vertical_width <= gpu_texture_limit && vertical_height <= gpu_texture_limit) {
        // Vertical layout
        layout.horizontal       = false;
        layout.composite_width  = vertical_width;
        layout.composite_height = vertical_height;
        layout.scale            = 1.0f;

        const FitRect left_fit = computeAspectFitRect(
            left_width, left_height, 0, 0, vertical_width, left_height);
        layout.left_x = left_fit.x; layout.left_y = left_fit.y;
        layout.left_w = left_fit.w; layout.left_h = left_fit.h;

        const FitRect right_fit = computeAspectFitRect(
            right_width, right_height, 0, left_height, vertical_width, right_height);
        layout.right_x = right_fit.x; layout.right_y = right_fit.y;
        layout.right_w = right_fit.w; layout.right_h = right_fit.h;

        setUVsFromRects();
    } else {
        // Both exceed limit — scale down horizontal to fit.
        layout.horizontal = true;

        const float width_scale  = static_cast<float>(gpu_texture_limit) / horizontal_width;
        const float height_scale = static_cast<float>(gpu_texture_limit) / horizontal_height;
        layout.scale = std::min(width_scale, height_scale);

        const int scaled_left_w  = static_cast<int>(left_width  * layout.scale);
        const int scaled_left_h  = static_cast<int>(left_height * layout.scale);
        const int scaled_right_w = static_cast<int>(right_width  * layout.scale);
        const int scaled_right_h = static_cast<int>(right_height * layout.scale);

        layout.composite_width  = scaled_left_w + scaled_right_w;
        layout.composite_height = std::max(scaled_left_h, scaled_right_h);

        const FitRect left_fit = computeAspectFitRect(
            scaled_left_w, scaled_left_h,
            0, 0, scaled_left_w, layout.composite_height);
        layout.left_x = left_fit.x; layout.left_y = left_fit.y;
        layout.left_w = left_fit.w; layout.left_h = left_fit.h;

        const FitRect right_fit = computeAspectFitRect(
            scaled_right_w, scaled_right_h,
            scaled_left_w, 0, scaled_right_w, layout.composite_height);
        layout.right_x = right_fit.x; layout.right_y = right_fit.y;
        layout.right_w = right_fit.w; layout.right_h = right_fit.h;

        setUVsFromRects();
    }

    return layout;
}

} // namespace qcv
