#pragma once

#include <imgui.h>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "viewport_annotator.h"

// Forward declaration for NanoVG
struct NVGcontext;
struct NVGcolor;

namespace qcview {
namespace Annotations {

/**
 * Renders annotation overlays on the video viewport using NanoVG.
 * Uses a single rendering backend for both display and export,
 * ensuring visual consistency.
 */
class AnnotationRenderer {
public:
    AnnotationRenderer() = default;
    ~AnnotationRenderer() = default;

    /**
     * Begin a new rendering frame.
     * Must be called before any rendering and after OpenGL viewport is set.
     *
     * @param width Viewport width in pixels
     * @param height Viewport height in pixels
     * @param devicePixelRatio Device pixel ratio for HiDPI displays (default 1.0)
     */
    void BeginFrame(float width, float height, float devicePixelRatio = 1.0f);

    /**
     * End the current rendering frame.
     * Must be called after all rendering is complete.
     */
    void EndFrame();

    /**
     * Render annotations from JSON data.
     * This renders stored/completed annotations.
     *
     * @param json_data JSON string containing annotation data
     * @param display_pos_x Left edge of video display area (screen coords)
     * @param display_pos_y Top edge of video display area (screen coords)
     * @param display_width Width of video display area (screen coords)
     * @param display_height Height of video display area (screen coords)
     */
    void RenderFromJSON(
        const std::string& json_data,
        float display_pos_x, float display_pos_y,
        float display_width, float display_height
    );

    /**
     * Render an active stroke being drawn.
     * This renders the in-progress stroke during drawing.
     *
     * @param stroke Active stroke to render
     * @param display_pos_x Left edge of video display area (screen coords)
     * @param display_pos_y Top edge of video display area (screen coords)
     * @param display_width Width of video display area (screen coords)
     * @param display_height Height of video display area (screen coords)
     * @param apply_smoothing Whether to apply stroke smoothing to freehand strokes
     * @param line_width_scale Scale factor for line width (viewport_size / video_size)
     */
    void RenderActiveStroke(
        const ActiveStroke& stroke,
        float display_pos_x, float display_pos_y,
        float display_width, float display_height,
        bool apply_smoothing = true,
        float line_width_scale = 1.0f
    );

private:
    /**
     * Render a single shape from JSON data.
     */
    void RenderShape(
        NVGcontext* vg,
        const nlohmann::json& shape,
        float display_pos_x, float display_pos_y,
        float display_width, float display_height
    );

    /**
     * Render a freehand stroke.
     */
    void RenderFreehand(
        NVGcontext* vg,
        const std::vector<ImVec2>& normalized_points,
        NVGcolor color,
        float thickness,
        float display_pos_x, float display_pos_y,
        float display_width, float display_height
    );

    /**
     * Render a rectangle.
     */
    void RenderRectangle(
        NVGcontext* vg,
        const std::vector<ImVec2>& normalized_points,
        NVGcolor color,
        float thickness,
        bool filled,
        float display_pos_x, float display_pos_y,
        float display_width, float display_height
    );

    /**
     * Render an oval/circle.
     */
    void RenderOval(
        NVGcontext* vg,
        const std::vector<ImVec2>& normalized_points,
        NVGcolor color,
        float thickness,
        bool filled,
        float display_pos_x, float display_pos_y,
        float display_width, float display_height
    );

    /**
     * Render an arrow.
     */
    void RenderArrow(
        NVGcontext* vg,
        const std::vector<ImVec2>& normalized_points,
        NVGcolor color,
        float thickness,
        float display_pos_x, float display_pos_y,
        float display_width, float display_height
    );

    /**
     * Render a line.
     */
    void RenderLine(
        NVGcontext* vg,
        const std::vector<ImVec2>& normalized_points,
        NVGcolor color,
        float thickness,
        float display_pos_x, float display_pos_y,
        float display_width, float display_height
    );

    /**
     * Convert a color array [r, g, b, a] to NVGcolor.
     */
    static NVGcolor ColorToNVG(const std::vector<float>& color);

    /**
     * Convert ImVec4 color to NVGcolor.
     */
    static NVGcolor ColorToNVG(const ImVec4& color);

    /**
     * Convert normalized point to screen coordinates.
     */
    static void NormalizedToScreen(
        float normalized_x, float normalized_y,
        float display_pos_x, float display_pos_y,
        float display_width, float display_height,
        float& out_x, float& out_y
    );
};

} // namespace Annotations
} // namespace qcview
