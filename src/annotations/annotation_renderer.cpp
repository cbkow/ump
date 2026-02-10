#include "annotation_renderer.h"
#include "nanovg_context.h"
#include "stroke_smoother.h"
#include <nanovg.h>
#include <cmath>

namespace ump {
namespace Annotations {

void AnnotationRenderer::BeginFrame(float width, float height, float devicePixelRatio) {
    NanoVGContext::Instance().BeginFrame(width, height, devicePixelRatio);
}

void AnnotationRenderer::EndFrame() {
    NanoVGContext::Instance().EndFrame();
}

void AnnotationRenderer::RenderFromJSON(
    const std::string& json_data,
    float display_pos_x, float display_pos_y,
    float display_width, float display_height
) {
    NVGcontext* vg = NanoVGContext::Instance().Get();
    if (!vg || json_data.empty()) {
        return;
    }

    try {
        nlohmann::json annotation_json = nlohmann::json::parse(json_data);

        // Check for shapes array
        if (!annotation_json.contains("shapes") || !annotation_json["shapes"].is_array()) {
            return;
        }

        // Render each shape
        for (const auto& shape : annotation_json["shapes"]) {
            RenderShape(vg, shape, display_pos_x, display_pos_y, display_width, display_height);
        }
    }
    catch (const nlohmann::json::exception& e) {
        // JSON parsing error - silently ignore
        return;
    }
}

void AnnotationRenderer::RenderActiveStroke(
    const ActiveStroke& stroke,
    float display_pos_x, float display_pos_y,
    float display_width, float display_height,
    bool apply_smoothing,
    float line_width_scale
) {
    NVGcontext* vg = NanoVGContext::Instance().Get();
    if (!vg) {
        // Debug: NanoVG context is null
        return;
    }
    if (stroke.points.empty()) {
        // Debug: Stroke has no points
        return;
    }

    NVGcolor color = ColorToNVG(stroke.color);

    // Scale stroke width based on viewport size relative to video resolution
    float scaled_width = stroke.stroke_width * line_width_scale;

    // Render based on tool type
    switch (stroke.tool) {
        case DrawingTool::FREEHAND: {
            std::vector<ImVec2> points_to_render = stroke.points;

            // Only apply Catmull-Rom smoothing for LEGACY strokes (is_modeled = false)
            // Modeled strokes are already smoothed by ink-stroke-modeler
            if (apply_smoothing && !stroke.is_modeled && stroke.points.size() >= 4) {
                StrokeSmoother::SmoothingConfig config;
                points_to_render = StrokeSmoother::SmoothStroke(stroke.points, config);
            }

            RenderFreehand(vg, points_to_render, color, scaled_width,
                          display_pos_x, display_pos_y, display_width, display_height);
            break;
        }

        case DrawingTool::RECTANGLE:
            RenderRectangle(vg, stroke.points, color, scaled_width,
                           stroke.filled, display_pos_x, display_pos_y, display_width, display_height);
            break;

        case DrawingTool::OVAL:
            RenderOval(vg, stroke.points, color, scaled_width,
                      stroke.filled, display_pos_x, display_pos_y, display_width, display_height);
            break;

        case DrawingTool::ARROW:
            RenderArrow(vg, stroke.points, color, scaled_width,
                       display_pos_x, display_pos_y, display_width, display_height);
            break;

        case DrawingTool::LINE:
            RenderLine(vg, stroke.points, color, scaled_width,
                      display_pos_x, display_pos_y, display_width, display_height);
            break;

        default:
            break;
    }
}

void AnnotationRenderer::RenderShape(
    NVGcontext* vg,
    const nlohmann::json& shape,
    float display_pos_x, float display_pos_y,
    float display_width, float display_height
) {
    try {
        // Extract shape properties
        std::string type = shape.value("type", "");
        std::vector<float> color_array = shape.value("color", std::vector<float>{1.0f, 0.0f, 0.0f, 1.0f});
        float stroke_width = shape.value("stroke_width", 2.5f);
        bool filled = shape.value("filled", false);
        bool is_modeled = shape.value("is_modeled", false);

        // Extract points
        std::vector<ImVec2> normalized_points;
        if (shape.contains("points") && shape["points"].is_array()) {
            for (const auto& point_array : shape["points"]) {
                if (point_array.is_array() && point_array.size() >= 2) {
                    ImVec2 point;
                    point.x = point_array[0].get<float>();
                    point.y = point_array[1].get<float>();
                    normalized_points.push_back(point);
                }
            }
        }

        if (normalized_points.empty()) {
            return;
        }

        NVGcolor color = ColorToNVG(color_array);

        // Render based on type
        if (type == "freehand") {
            std::vector<ImVec2> points_to_render = normalized_points;

            // Only apply Catmull-Rom smoothing for LEGACY strokes (is_modeled = false)
            if (!is_modeled && normalized_points.size() >= 4) {
                StrokeSmoother::SmoothingConfig config;
                points_to_render = StrokeSmoother::SmoothStroke(normalized_points, config);
            }

            RenderFreehand(vg, points_to_render, color, stroke_width,
                          display_pos_x, display_pos_y, display_width, display_height);
        }
        else if (type == "rect") {
            RenderRectangle(vg, normalized_points, color, stroke_width,
                           filled, display_pos_x, display_pos_y, display_width, display_height);
        }
        else if (type == "oval") {
            RenderOval(vg, normalized_points, color, stroke_width,
                      filled, display_pos_x, display_pos_y, display_width, display_height);
        }
        else if (type == "arrow") {
            RenderArrow(vg, normalized_points, color, stroke_width,
                       display_pos_x, display_pos_y, display_width, display_height);
        }
        else if (type == "line") {
            RenderLine(vg, normalized_points, color, stroke_width,
                      display_pos_x, display_pos_y, display_width, display_height);
        }
    }
    catch (const nlohmann::json::exception& e) {
        // Shape parsing error - skip this shape
        return;
    }
}

void AnnotationRenderer::RenderFreehand(
    NVGcontext* vg,
    const std::vector<ImVec2>& normalized_points,
    NVGcolor color,
    float thickness,
    float display_pos_x, float display_pos_y,
    float display_width, float display_height
) {
    if (normalized_points.size() < 2) {
        return;
    }


    nvgBeginPath(vg);

    // First point
    float x, y;
    NormalizedToScreen(normalized_points[0].x, normalized_points[0].y,
                       display_pos_x, display_pos_y, display_width, display_height, x, y);
    nvgMoveTo(vg, x, y);

    // Subsequent points
    for (size_t i = 1; i < normalized_points.size(); ++i) {
        NormalizedToScreen(normalized_points[i].x, normalized_points[i].y,
                           display_pos_x, display_pos_y, display_width, display_height, x, y);
        nvgLineTo(vg, x, y);
    }

    // Configure stroke style - round caps and joins as requested
    nvgStrokeColor(vg, color);
    nvgStrokeWidth(vg, thickness);
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    nvgStroke(vg);
}

void AnnotationRenderer::RenderRectangle(
    NVGcontext* vg,
    const std::vector<ImVec2>& normalized_points,
    NVGcolor color,
    float thickness,
    bool filled,
    float display_pos_x, float display_pos_y,
    float display_width, float display_height
) {
    if (normalized_points.size() < 4) {
        return;
    }

    // Convert corners to screen coordinates
    float x1, y1, x2, y2;
    NormalizedToScreen(normalized_points[0].x, normalized_points[0].y,
                       display_pos_x, display_pos_y, display_width, display_height, x1, y1);
    NormalizedToScreen(normalized_points[2].x, normalized_points[2].y,
                       display_pos_x, display_pos_y, display_width, display_height, x2, y2);

    nvgBeginPath(vg);
    nvgRect(vg, x1, y1, x2 - x1, y2 - y1);

    if (filled) {
        nvgFillColor(vg, color);
        nvgFill(vg);
    } else {
        nvgStrokeColor(vg, color);
        nvgStrokeWidth(vg, thickness);
        nvgLineCap(vg, NVG_ROUND);
        nvgLineJoin(vg, NVG_ROUND);
        nvgStroke(vg);
    }
}

void AnnotationRenderer::RenderOval(
    NVGcontext* vg,
    const std::vector<ImVec2>& normalized_points,
    NVGcolor color,
    float thickness,
    bool filled,
    float display_pos_x, float display_pos_y,
    float display_width, float display_height
) {
    if (normalized_points.size() < 2) {
        return;
    }

    // Point 0: center (normalized)
    // Point 1: radii (x=radius_x, y=radius_y) (normalized)
    ImVec2 center_normalized = normalized_points[0];
    ImVec2 radii_normalized = normalized_points[1];

    // Convert center to screen coordinates
    float cx, cy;
    NormalizedToScreen(center_normalized.x, center_normalized.y,
                       display_pos_x, display_pos_y, display_width, display_height, cx, cy);

    // Convert radii to screen space
    float rx = radii_normalized.x * display_width;
    float ry = radii_normalized.y * display_height;

    nvgBeginPath(vg);
    nvgEllipse(vg, cx, cy, rx, ry);

    if (filled) {
        nvgFillColor(vg, color);
        nvgFill(vg);
    } else {
        nvgStrokeColor(vg, color);
        nvgStrokeWidth(vg, thickness);
        nvgStroke(vg);
    }
}

void AnnotationRenderer::RenderArrow(
    NVGcontext* vg,
    const std::vector<ImVec2>& normalized_points,
    NVGcolor color,
    float thickness,
    float display_pos_x, float display_pos_y,
    float display_width, float display_height
) {
    if (normalized_points.size() < 2) {
        return;
    }

    // Convert start and end to screen coordinates
    float start_x, start_y, end_x, end_y;
    NormalizedToScreen(normalized_points[0].x, normalized_points[0].y,
                       display_pos_x, display_pos_y, display_width, display_height, start_x, start_y);
    NormalizedToScreen(normalized_points[1].x, normalized_points[1].y,
                       display_pos_x, display_pos_y, display_width, display_height, end_x, end_y);

    // Calculate direction from start to end
    float dir_x = end_x - start_x;
    float dir_y = end_y - start_y;

    float length = std::sqrt(dir_x * dir_x + dir_y * dir_y);
    if (length < 0.001f) {
        return;  // Too short to draw
    }

    // Normalize direction
    dir_x /= length;
    dir_y /= length;

    // Calculate arrowhead size (scales with stroke width)
    float arrow_size = 12.0f + thickness * 2.0f;

    // Arrowhead angle (25 degrees for a sharper look)
    float arrow_angle = 0.436332f;  // 25 degrees in radians
    float cos_angle = std::cos(arrow_angle);
    float sin_angle = std::sin(arrow_angle);

    // Calculate where the line should end (at the back of the arrowhead)
    // The arrowhead base is arrow_size * cos(angle) back from the tip
    float arrow_back_dist = arrow_size * cos_angle;
    float line_end_x = end_x - dir_x * arrow_back_dist;
    float line_end_y = end_y - dir_y * arrow_back_dist;

    // Draw the main line (stopping at arrowhead base)
    nvgBeginPath(vg);
    nvgMoveTo(vg, start_x, start_y);
    nvgLineTo(vg, line_end_x, line_end_y);
    nvgStrokeColor(vg, color);
    nvgStrokeWidth(vg, thickness);
    nvgLineCap(vg, NVG_ROUND);
    nvgStroke(vg);

    // Calculate arrowhead wing points
    // Rotate direction vector by +/- arrow_angle
    float arrow1_x = end_x - arrow_size * (dir_x * cos_angle - dir_y * sin_angle);
    float arrow1_y = end_y - arrow_size * (dir_y * cos_angle + dir_x * sin_angle);

    float arrow2_x = end_x - arrow_size * (dir_x * cos_angle + dir_y * sin_angle);
    float arrow2_y = end_y - arrow_size * (dir_y * cos_angle - dir_x * sin_angle);

    // Draw arrowhead as filled triangle
    nvgBeginPath(vg);
    nvgMoveTo(vg, end_x, end_y);
    nvgLineTo(vg, arrow1_x, arrow1_y);
    nvgLineTo(vg, arrow2_x, arrow2_y);
    nvgClosePath(vg);
    nvgFillColor(vg, color);
    nvgFill(vg);
}

void AnnotationRenderer::RenderLine(
    NVGcontext* vg,
    const std::vector<ImVec2>& normalized_points,
    NVGcolor color,
    float thickness,
    float display_pos_x, float display_pos_y,
    float display_width, float display_height
) {
    if (normalized_points.size() < 2) {
        return;
    }

    // Convert start and end to screen coordinates
    float start_x, start_y, end_x, end_y;
    NormalizedToScreen(normalized_points[0].x, normalized_points[0].y,
                       display_pos_x, display_pos_y, display_width, display_height, start_x, start_y);
    NormalizedToScreen(normalized_points[1].x, normalized_points[1].y,
                       display_pos_x, display_pos_y, display_width, display_height, end_x, end_y);

    nvgBeginPath(vg);
    nvgMoveTo(vg, start_x, start_y);
    nvgLineTo(vg, end_x, end_y);
    nvgStrokeColor(vg, color);
    nvgStrokeWidth(vg, thickness);
    nvgLineCap(vg, NVG_ROUND);
    nvgStroke(vg);
}

NVGcolor AnnotationRenderer::ColorToNVG(const std::vector<float>& color) {
    if (color.size() < 4) {
        return nvgRGBA(255, 0, 0, 255);  // Default: Red
    }
    return nvgRGBAf(color[0], color[1], color[2], color[3]);
}

NVGcolor AnnotationRenderer::ColorToNVG(const ImVec4& color) {
    return nvgRGBAf(color.x, color.y, color.z, color.w);
}

void AnnotationRenderer::NormalizedToScreen(
    float normalized_x, float normalized_y,
    float display_pos_x, float display_pos_y,
    float display_width, float display_height,
    float& out_x, float& out_y
) {
    out_x = display_pos_x + (normalized_x * display_width);
    out_y = display_pos_y + (normalized_y * display_height);
}

} // namespace Annotations
} // namespace ump
