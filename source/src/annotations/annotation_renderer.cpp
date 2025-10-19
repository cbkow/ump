#include "annotation_renderer.h"
#include "stroke_smoother.h"
#include <cmath>

namespace ump {
namespace Annotations {

void AnnotationRenderer::RenderFromJSON(
    ImDrawList* draw_list,
    const std::string& json_data,
    const ImVec2& display_pos,
    const ImVec2& display_size
) {
    if (!draw_list || json_data.empty()) {
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
            RenderShape(draw_list, shape, display_pos, display_size);
        }
    }
    catch (const nlohmann::json::exception& e) {
        // JSON parsing error - silently ignore for now
        // In production, you might want to log this
        return;
    }
}

void AnnotationRenderer::RenderActiveStroke(
    ImDrawList* draw_list,
    const ActiveStroke& stroke,
    const ImVec2& display_pos,
    const ImVec2& display_size,
    bool apply_smoothing
) {
    if (!draw_list || stroke.points.empty()) {
        return;
    }

    ImU32 color = ColorToImU32(stroke.color);

    // Render based on tool type
    switch (stroke.tool) {
        case DrawingTool::FREEHAND: {
            std::vector<ImVec2> points_to_render = stroke.points;

            // Apply smoothing if requested and we have enough points
            if (apply_smoothing && stroke.points.size() >= 4) {
                StrokeSmoother::SmoothingConfig config;
                // Use high quality smoothing (default segments_per_curve from config is 20)
                points_to_render = StrokeSmoother::SmoothStroke(stroke.points, config);
            }

            RenderFreehand(draw_list, points_to_render, color, stroke.stroke_width,
                          display_pos, display_size);
            break;
        }

        case DrawingTool::RECTANGLE:
            RenderRectangle(draw_list, stroke.points, color, stroke.stroke_width,
                           stroke.filled, display_pos, display_size);
            break;

        case DrawingTool::OVAL:
            RenderOval(draw_list, stroke.points, color, stroke.stroke_width,
                      stroke.filled, display_pos, display_size);
            break;

        case DrawingTool::ARROW:
            RenderArrow(draw_list, stroke.points, color, stroke.stroke_width,
                       display_pos, display_size);
            break;

        case DrawingTool::LINE:
            RenderLine(draw_list, stroke.points, color, stroke.stroke_width,
                      display_pos, display_size);
            break;

        default:
            break;
    }
}

void AnnotationRenderer::RenderShape(
    ImDrawList* draw_list,
    const nlohmann::json& shape,
    const ImVec2& display_pos,
    const ImVec2& display_size
) {
    try {
        // Extract shape properties
        std::string type = shape.value("type", "");
        std::vector<float> color_array = shape.value("color", std::vector<float>{1.0f, 0.0f, 0.0f, 1.0f});
        float stroke_width = shape.value("stroke_width", 2.5f);
        bool filled = shape.value("filled", false);

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

        ImU32 color = ColorToImU32(color_array);

        // Render based on type
        if (type == "freehand") {
            // Apply smoothing to freehand strokes loaded from JSON
            std::vector<ImVec2> points_to_render = normalized_points;
            if (normalized_points.size() >= 4) {
                StrokeSmoother::SmoothingConfig config;
                // Use high quality smoothing for saved/loaded strokes
                points_to_render = StrokeSmoother::SmoothStroke(normalized_points, config);
            }
            RenderFreehand(draw_list, points_to_render, color, stroke_width,
                          display_pos, display_size);
        }
        else if (type == "rect") {
            RenderRectangle(draw_list, normalized_points, color, stroke_width,
                           filled, display_pos, display_size);
        }
        else if (type == "oval") {
            RenderOval(draw_list, normalized_points, color, stroke_width,
                      filled, display_pos, display_size);
        }
        else if (type == "arrow") {
            RenderArrow(draw_list, normalized_points, color, stroke_width,
                       display_pos, display_size);
        }
        else if (type == "line") {
            RenderLine(draw_list, normalized_points, color, stroke_width,
                      display_pos, display_size);
        }
    }
    catch (const nlohmann::json::exception& e) {
        // Shape parsing error - skip this shape
        return;
    }
}

void AnnotationRenderer::RenderFreehand(
    ImDrawList* draw_list,
    const std::vector<ImVec2>& normalized_points,
    ImU32 color,
    float thickness,
    const ImVec2& display_pos,
    const ImVec2& display_size
) {
    if (normalized_points.size() < 2) {
        return;
    }

    // Convert all points to screen coordinates
    std::vector<ImVec2> screen_points;
    screen_points.reserve(normalized_points.size());

    for (const auto& normalized_point : normalized_points) {
        screen_points.push_back(NormalizedToScreen(normalized_point, display_pos, display_size));
    }

    // Render as polyline
    draw_list->AddPolyline(
        screen_points.data(),
        static_cast<int>(screen_points.size()),
        color,
        ImDrawFlags_None,
        thickness
    );
}

void AnnotationRenderer::RenderRectangle(
    ImDrawList* draw_list,
    const std::vector<ImVec2>& normalized_points,
    ImU32 color,
    float thickness,
    bool filled,
    const ImVec2& display_pos,
    const ImVec2& display_size
) {
    if (normalized_points.size() < 4) {
        return;
    }

    // Convert corners to screen coordinates
    ImVec2 top_left = NormalizedToScreen(normalized_points[0], display_pos, display_size);
    ImVec2 bottom_right = NormalizedToScreen(normalized_points[2], display_pos, display_size);

    if (filled) {
        draw_list->AddRectFilled(top_left, bottom_right, color);
    }
    else {
        draw_list->AddRect(top_left, bottom_right, color, 0.0f, ImDrawFlags_None, thickness);
    }
}

void AnnotationRenderer::RenderOval(
    ImDrawList* draw_list,
    const std::vector<ImVec2>& normalized_points,
    ImU32 color,
    float thickness,
    bool filled,
    const ImVec2& display_pos,
    const ImVec2& display_size
) {
    if (normalized_points.size() < 2) {
        return;
    }

    // Point 0: center (normalized)
    // Point 1: radii (x=radius_x, y=radius_y) (normalized)
    ImVec2 center_normalized = normalized_points[0];
    ImVec2 radii_normalized = normalized_points[1];

    // Convert to screen coordinates
    ImVec2 center_screen = NormalizedToScreen(center_normalized, display_pos, display_size);

    // Convert radii to screen space (radii are relative to display size)
    float radius_x = radii_normalized.x * display_size.x;
    float radius_y = radii_normalized.y * display_size.y;

    // If radii are equal (or very close), render as circle
    if (std::abs(radius_x - radius_y) < 1.0f) {
        float radius = (radius_x + radius_y) * 0.5f;
        if (filled) {
            draw_list->AddCircleFilled(center_screen, radius, color, 32);
        }
        else {
            draw_list->AddCircle(center_screen, radius, color, 32, thickness);
        }
    }
    else {
        // Render as ellipse using AddPolyline with calculated points
        const int num_segments = 64;
        std::vector<ImVec2> ellipse_points;
        ellipse_points.reserve(num_segments);

        for (int i = 0; i < num_segments; ++i) {
            float angle = (static_cast<float>(i) / num_segments) * 2.0f * 3.14159265359f;
            ImVec2 point;
            point.x = center_screen.x + std::cos(angle) * radius_x;
            point.y = center_screen.y + std::sin(angle) * radius_y;
            ellipse_points.push_back(point);
        }

        if (filled) {
            draw_list->AddConvexPolyFilled(ellipse_points.data(),
                                          static_cast<int>(ellipse_points.size()),
                                          color);
        }
        else {
            draw_list->AddPolyline(ellipse_points.data(),
                                  static_cast<int>(ellipse_points.size()),
                                  color,
                                  ImDrawFlags_Closed,
                                  thickness);
        }
    }
}

void AnnotationRenderer::RenderArrow(
    ImDrawList* draw_list,
    const std::vector<ImVec2>& normalized_points,
    ImU32 color,
    float thickness,
    const ImVec2& display_pos,
    const ImVec2& display_size
) {
    if (normalized_points.size() < 2) {
        return;
    }

    // Convert start and end to screen coordinates
    ImVec2 start = NormalizedToScreen(normalized_points[0], display_pos, display_size);
    ImVec2 end = NormalizedToScreen(normalized_points[1], display_pos, display_size);

    // Draw the main line
    draw_list->AddLine(start, end, color, thickness);

    // Calculate arrowhead
    float arrow_size = 15.0f + thickness;  // Scale with stroke width
    ImVec2 direction;
    direction.x = start.x - end.x;
    direction.y = start.y - end.y;

    float length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
    if (length < 0.001f) {
        return;  // Too short to draw arrowhead
    }

    // Normalize direction
    direction.x /= length;
    direction.y /= length;

    // Calculate arrowhead points (at 30 degrees)
    float cos_angle = std::cos(0.523599f);  // 30 degrees in radians
    float sin_angle = std::sin(0.523599f);

    ImVec2 arrow1;
    arrow1.x = end.x + arrow_size * (direction.x * cos_angle - direction.y * sin_angle);
    arrow1.y = end.y + arrow_size * (direction.x * sin_angle + direction.y * cos_angle);

    ImVec2 arrow2;
    arrow2.x = end.x + arrow_size * (direction.x * cos_angle + direction.y * sin_angle);
    arrow2.y = end.y + arrow_size * (-direction.x * sin_angle + direction.y * cos_angle);

    // Draw arrowhead as filled triangle
    draw_list->AddTriangleFilled(end, arrow1, arrow2, color);
}

void AnnotationRenderer::RenderLine(
    ImDrawList* draw_list,
    const std::vector<ImVec2>& normalized_points,
    ImU32 color,
    float thickness,
    const ImVec2& display_pos,
    const ImVec2& display_size
) {
    if (normalized_points.size() < 2) {
        return;
    }

    // Convert start and end to screen coordinates
    ImVec2 start = NormalizedToScreen(normalized_points[0], display_pos, display_size);
    ImVec2 end = NormalizedToScreen(normalized_points[1], display_pos, display_size);

    draw_list->AddLine(start, end, color, thickness);
}

ImU32 AnnotationRenderer::ColorToImU32(const std::vector<float>& color) {
    if (color.size() < 4) {
        return IM_COL32(255, 0, 0, 255);  // Default: Red
    }

    return IM_COL32(
        static_cast<int>(color[0] * 255.0f),
        static_cast<int>(color[1] * 255.0f),
        static_cast<int>(color[2] * 255.0f),
        static_cast<int>(color[3] * 255.0f)
    );
}

ImU32 AnnotationRenderer::ColorToImU32(const ImVec4& color) {
    return IM_COL32(
        static_cast<int>(color.x * 255.0f),
        static_cast<int>(color.y * 255.0f),
        static_cast<int>(color.z * 255.0f),
        static_cast<int>(color.w * 255.0f)
    );
}

ImVec2 AnnotationRenderer::NormalizedToScreen(
    const ImVec2& normalized,
    const ImVec2& display_pos,
    const ImVec2& display_size
) {
    ImVec2 screen;
    screen.x = display_pos.x + (normalized.x * display_size.x);
    screen.y = display_pos.y + (normalized.y * display_size.y);
    return screen;
}

} // namespace Annotations
} // namespace ump
