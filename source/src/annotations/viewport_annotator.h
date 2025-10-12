#pragma once

#include <imgui.h>
#include <vector>
#include <memory>
#include "annotation_note.h"

namespace ump {
namespace Annotations {

/**
 * Viewport mode determines how the video viewport handles user input.
 */
enum class ViewportMode {
    PLAYBACK,    // Normal video playback/scrubbing - default mode
    ANNOTATION   // Drawing mode - playback disabled, annotation tools active
};

/**
 * Drawing tool types available in annotation mode.
 */
enum class DrawingTool {
    NONE,
    FREEHAND,
    RECTANGLE,
    OVAL,
    ARROW,
    LINE
};

/**
 * Represents a single drawing stroke or shape being created.
 */
struct ActiveStroke {
    DrawingTool tool = DrawingTool::NONE;
    std::vector<ImVec2> points;  // Normalized coordinates (0-1 range)
    ImVec4 color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);  // RGBA
    float stroke_width = 2.5f;
    bool filled = false;
    bool is_complete = false;

    void Clear() {
        tool = DrawingTool::NONE;
        points.clear();
        is_complete = false;
    }
};

/**
 * Manages viewport annotation mode, drawing interactions, and coordinate conversions.
 * Handles the transition between playback and annotation modes.
 */
class ViewportAnnotator {
public:
    ViewportAnnotator();
    ~ViewportAnnotator() = default;

    /**
     * Get current viewport mode.
     */
    ViewportMode GetMode() const { return mode_; }

    /**
     * Set viewport mode.
     */
    void SetMode(ViewportMode new_mode);

    /**
     * Toggle between PLAYBACK and ANNOTATION modes.
     * Returns the new mode.
     */
    ViewportMode ToggleMode();

    /**
     * Check if currently in annotation mode.
     */
    bool IsAnnotationMode() const { return mode_ == ViewportMode::ANNOTATION; }

    /**
     * Get current active drawing tool.
     */
    DrawingTool GetActiveTool() const { return active_tool_; }

    /**
     * Set active drawing tool.
     */
    void SetActiveTool(DrawingTool tool) { active_tool_ = tool; }

    /**
     * Get current drawing color.
     */
    ImVec4 GetDrawingColor() const { return drawing_color_; }

    /**
     * Set drawing color (RGBA).
     */
    void SetDrawingColor(const ImVec4& color) { drawing_color_ = color; }

    /**
     * Get current stroke width.
     */
    float GetStrokeWidth() const { return stroke_width_; }

    /**
     * Set stroke width (pixels).
     */
    void SetStrokeWidth(float width) { stroke_width_ = width; }

    /**
     * Get fill enabled state for shapes.
     */
    bool IsFillEnabled() const { return fill_enabled_; }

    /**
     * Set fill enabled for shapes.
     */
    void SetFillEnabled(bool enabled) { fill_enabled_ = enabled; }

    /**
     * Process mouse input for drawing in the viewport.
     * Call this from the viewport window's input handling.
     *
     * @param display_pos Top-left corner of video display area (screen coords)
     * @param display_size Size of video display area (screen coords)
     * @return true if input was consumed by annotation system
     */
    bool ProcessInput(const ImVec2& display_pos, const ImVec2& display_size);

    /**
     * Get the current active stroke being drawn (if any).
     */
    const ActiveStroke* GetActiveStroke() const {
        return active_stroke_.tool != DrawingTool::NONE ? &active_stroke_ : nullptr;
    }

    /**
     * Clear the current active stroke.
     */
    void ClearActiveStroke() { active_stroke_.Clear(); }

    /**
     * Finalize the current stroke and return it for storage.
     * Returns nullptr if no active stroke or stroke is invalid.
     */
    std::unique_ptr<ActiveStroke> FinalizeStroke();

    /**
     * Convert screen coordinates to normalized coordinates (0-1 range).
     */
    static ImVec2 ScreenToNormalized(
        const ImVec2& screen_pos,
        const ImVec2& display_pos,
        const ImVec2& display_size
    );

    /**
     * Convert normalized coordinates (0-1 range) to screen coordinates.
     */
    static ImVec2 NormalizedToScreen(
        const ImVec2& normalized_pos,
        const ImVec2& display_pos,
        const ImVec2& display_size
    );

    /**
     * Check if a screen position is within the display area.
     */
    static bool IsInsideDisplayArea(
        const ImVec2& screen_pos,
        const ImVec2& display_pos,
        const ImVec2& display_size
    );

private:
    ViewportMode mode_ = ViewportMode::PLAYBACK;
    DrawingTool active_tool_ = DrawingTool::NONE;

    // Drawing properties
    ImVec4 drawing_color_ = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);  // Default: Red
    float stroke_width_ = 2.5f;
    bool fill_enabled_ = false;

    // Current stroke being drawn
    ActiveStroke active_stroke_;
    bool is_drawing_ = false;

    // Mouse state tracking
    ImVec2 drag_start_pos_;  // Normalized coordinates

    /**
     * Handle freehand tool input.
     */
    void ProcessFreehandInput(const ImVec2& display_pos, const ImVec2& display_size);

    /**
     * Handle rectangle tool input.
     */
    void ProcessRectangleInput(const ImVec2& display_pos, const ImVec2& display_size);

    /**
     * Handle oval tool input.
     */
    void ProcessOvalInput(const ImVec2& display_pos, const ImVec2& display_size);

    /**
     * Handle arrow tool input.
     */
    void ProcessArrowInput(const ImVec2& display_pos, const ImVec2& display_size);

    /**
     * Handle line tool input.
     */
    void ProcessLineInput(const ImVec2& display_pos, const ImVec2& display_size);
};

} // namespace Annotations
} // namespace ump
