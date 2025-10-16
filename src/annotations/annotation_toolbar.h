#pragma once

#include <imgui.h>
#include <functional>
#include "viewport_annotator.h"

namespace ump {
namespace Annotations {

/**
 * Annotation toolbar panel for drawing tools and controls.
 * Displays at the bottom of the video viewport when in annotation mode.
 */
class AnnotationToolbar {
public:
    /**
     * Callbacks for toolbar actions.
     */
    struct Callbacks {
        std::function<void()> on_undo;
        std::function<void()> on_redo;
        std::function<void()> on_clear_all;
        std::function<void()> on_done;          // Exit annotation mode and save
        std::function<void()> on_cancel;        // Exit annotation mode without saving
        std::function<void(DrawingTool)> on_tool_changed;
        std::function<void(const ImVec4&)> on_color_changed;
        std::function<void(float)> on_stroke_width_changed;
        std::function<void(bool)> on_fill_changed;
    };

    AnnotationToolbar();
    ~AnnotationToolbar() = default;

    /**
     * Set the callbacks for toolbar actions.
     */
    void SetCallbacks(const Callbacks& callbacks) { callbacks_ = callbacks; }

    /**
     * Render the toolbar panel.
     * Call this when in annotation mode.
     *
     * @param viewport_annotator Pointer to the viewport annotator for state sync
     * @param can_undo Whether undo is available
     * @param can_redo Whether redo is available
     * @param icon_font Material icons font (optional)
     * @param accent_color_regular Regular accent color for Done button and active tools
     * @param accent_color_muted_dark Muted dark accent color for Cancel button
     */
    void Render(ViewportAnnotator* viewport_annotator, bool can_undo, bool can_redo,
               ImFont* icon_font = nullptr,
               ImVec4 accent_color_regular = ImVec4(0.3f, 0.6f, 1.0f, 1.0f),
               ImVec4 accent_color_muted_dark = ImVec4(0.5f, 0.5f, 0.5f, 1.0f));

    /**
     * Check if the toolbar should be visible (annotation mode).
     */
    bool ShouldBeVisible() const { return is_visible_; }

    /**
     * Set toolbar visibility.
     */
    void SetVisible(bool visible) { is_visible_ = visible; }

private:
    Callbacks callbacks_;
    bool is_visible_ = false;

    // UI state
    DrawingTool selected_tool_ = DrawingTool::FREEHAND;
    ImVec4 selected_color_ = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);  // Default: Red
    float stroke_width_ = 2.5f;
    bool fill_enabled_ = false;

    /**
     * Render tool selection buttons.
     */
    void RenderToolButtons(ImFont* icon_font = nullptr, ImVec4 accent_regular = ImVec4(0.3f, 0.6f, 1.0f, 1.0f));

    /**
     * Render drawing properties (color, width, fill).
     */
    void RenderDrawingProperties();

    /**
     * Render action buttons (undo, redo, clear, done, cancel).
     */
    void RenderActionButtons(bool can_undo, bool can_redo, ImFont* icon_font = nullptr,
                            ImVec4 accent_regular = ImVec4(0.3f, 0.6f, 1.0f, 1.0f),
                            ImVec4 accent_muted_dark = ImVec4(0.5f, 0.5f, 0.5f, 1.0f));

    /**
     * Helper to render a tool button with icon.
     */
    bool ToolButton(const char* label, DrawingTool tool, const char* tooltip, ImVec4 accent_regular);

    /**
     * Helper to render an action button.
     */
    bool ActionButton(const char* label, bool enabled, const char* tooltip);
};

} // namespace Annotations
} // namespace ump
