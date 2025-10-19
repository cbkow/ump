#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "viewport_annotator.h"

namespace ump {
namespace Annotations {

/**
 * Renders annotation overlays on the video viewport.
 * Handles both stored annotations (from JSON) and active strokes being drawn.
 */
class AnnotationRenderer {
public:
    AnnotationRenderer() = default;
    ~AnnotationRenderer() = default;

    /**
     * Render annotations from JSON data.
     * This renders stored/completed annotations.
     *
     * @param draw_list ImGui draw list for the viewport
     * @param json_data JSON string containing annotation data
     * @param display_pos Top-left corner of video display area (screen coords)
     * @param display_size Size of video display area (screen coords)
     */
    void RenderFromJSON(
        ImDrawList* draw_list,
        const std::string& json_data,
        const ImVec2& display_pos,
        const ImVec2& display_size
    );

    /**
     * Render an active stroke being drawn.
     * This renders the in-progress stroke during drawing.
     *
     * @param draw_list ImGui draw list for the viewport
     * @param stroke Active stroke to render
     * @param display_pos Top-left corner of video display area (screen coords)
     * @param display_size Size of video display area (screen coords)
     * @param apply_smoothing Whether to apply stroke smoothing to freehand strokes
     */
    void RenderActiveStroke(
        ImDrawList* draw_list,
        const ActiveStroke& stroke,
        const ImVec2& display_pos,
        const ImVec2& display_size,
        bool apply_smoothing = true
    );

private:
    /**
     * Render a single shape from JSON data.
     */
    void RenderShape(
        ImDrawList* draw_list,
        const nlohmann::json& shape,
        const ImVec2& display_pos,
        const ImVec2& display_size
    );

    /**
     * Render a freehand stroke.
     */
    void RenderFreehand(
        ImDrawList* draw_list,
        const std::vector<ImVec2>& normalized_points,
        ImU32 color,
        float thickness,
        const ImVec2& display_pos,
        const ImVec2& display_size
    );

    /**
     * Render a rectangle.
     */
    void RenderRectangle(
        ImDrawList* draw_list,
        const std::vector<ImVec2>& normalized_points,
        ImU32 color,
        float thickness,
        bool filled,
        const ImVec2& display_pos,
        const ImVec2& display_size
    );

    /**
     * Render an oval/circle.
     */
    void RenderOval(
        ImDrawList* draw_list,
        const std::vector<ImVec2>& normalized_points,
        ImU32 color,
        float thickness,
        bool filled,
        const ImVec2& display_pos,
        const ImVec2& display_size
    );

    /**
     * Render an arrow.
     */
    void RenderArrow(
        ImDrawList* draw_list,
        const std::vector<ImVec2>& normalized_points,
        ImU32 color,
        float thickness,
        const ImVec2& display_pos,
        const ImVec2& display_size
    );

    /**
     * Render a line.
     */
    void RenderLine(
        ImDrawList* draw_list,
        const std::vector<ImVec2>& normalized_points,
        ImU32 color,
        float thickness,
        const ImVec2& display_pos,
        const ImVec2& display_size
    );

    /**
     * Convert a color array [r, g, b, a] to ImU32 color.
     */
    static ImU32 ColorToImU32(const std::vector<float>& color);

    /**
     * Convert ImVec4 color to ImU32.
     */
    static ImU32 ColorToImU32(const ImVec4& color);

    /**
     * Convert normalized point to screen coordinates.
     */
    static ImVec2 NormalizedToScreen(
        const ImVec2& normalized,
        const ImVec2& display_pos,
        const ImVec2& display_size
    );
};

} // namespace Annotations
} // namespace ump
