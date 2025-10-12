#include "annotation_toolbar.h"

namespace ump {
namespace Annotations {

AnnotationToolbar::AnnotationToolbar() {
    // Initialize with defaults
}

void AnnotationToolbar::Render(ViewportAnnotator* viewport_annotator, bool can_undo, bool can_redo,
                               ImFont* icon_font,
                               ImVec4 accent_color_regular,
                               ImVec4 accent_color_muted_dark) {
    if (!is_visible_ || !viewport_annotator) {
        return;
    }

    // Store for use in render methods
    static ImFont* s_icon_font = nullptr;
    static ImVec4 s_accent_regular = accent_color_regular;
    static ImVec4 s_accent_muted_dark = accent_color_muted_dark;
    s_icon_font = icon_font;
    s_accent_regular = accent_color_regular;
    s_accent_muted_dark = accent_color_muted_dark;

    // Sync state with viewport annotator
    selected_tool_ = viewport_annotator->GetActiveTool();
    selected_color_ = viewport_annotator->GetDrawingColor();
    stroke_width_ = viewport_annotator->GetStrokeWidth();
    fill_enabled_ = viewport_annotator->IsFillEnabled();

    // Create a horizontal toolbar docked below the viewport
    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;

    if (ImGui::Begin("Annotation Toolbar", &is_visible_, window_flags)) {
        // Horizontal layout
        RenderToolButtons(s_icon_font, s_accent_regular);
        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine();
        RenderDrawingProperties();
        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine();
        RenderActionButtons(can_undo, can_redo, s_icon_font, s_accent_regular, s_accent_muted_dark);
    }
    ImGui::End();
}

void AnnotationToolbar::RenderToolButtons(ImFont* icon_font, ImVec4 accent_regular) {
    ImGui::Text("Tools:");
    ImGui::SameLine();

    // Material Icons
    #define ICON_DRAW           u8"\uE22B"
    #define ICON_RECTANGLE      u8"\ueb36"
    #define ICON_CIRCLE         u8"\uEF4A"
    #define ICON_ARROW_FORWARD  u8"\uE5C8"
    #define ICON_REMOVE         u8"\uE15B"

    if (icon_font) ImGui::PushFont(icon_font);

    // Push accent color for selected tool buttons
    ImGui::PushStyleColor(ImGuiCol_Button, accent_regular);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(accent_regular.x * 1.2f, accent_regular.y * 1.2f, accent_regular.z * 1.2f, accent_regular.w));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(accent_regular.x * 0.8f, accent_regular.y * 0.8f, accent_regular.z * 0.8f, accent_regular.w));

    if (ToolButton(icon_font ? ICON_DRAW : "Freehand", DrawingTool::FREEHAND, "Draw freehand strokes (F)")) {
        selected_tool_ = DrawingTool::FREEHAND;
        if (callbacks_.on_tool_changed) {
            callbacks_.on_tool_changed(DrawingTool::FREEHAND);
        }
    }

    ImGui::SameLine();
    if (ToolButton(icon_font ? ICON_RECTANGLE : "Rectangle", DrawingTool::RECTANGLE, "Draw rectangles (R)")) {
        selected_tool_ = DrawingTool::RECTANGLE;
        if (callbacks_.on_tool_changed) {
            callbacks_.on_tool_changed(DrawingTool::RECTANGLE);
        }
    }

    ImGui::SameLine();
    if (ToolButton(icon_font ? ICON_CIRCLE : "Oval", DrawingTool::OVAL, "Draw ovals/circles (O)")) {
        selected_tool_ = DrawingTool::OVAL;
        if (callbacks_.on_tool_changed) {
            callbacks_.on_tool_changed(DrawingTool::OVAL);
        }
    }

    ImGui::SameLine();
    if (ToolButton(icon_font ? ICON_ARROW_FORWARD : "Arrow", DrawingTool::ARROW, "Draw arrows (A)")) {
        selected_tool_ = DrawingTool::ARROW;
        if (callbacks_.on_tool_changed) {
            callbacks_.on_tool_changed(DrawingTool::ARROW);
        }
    }

    ImGui::SameLine();
    if (ToolButton(icon_font ? ICON_REMOVE : "Line", DrawingTool::LINE, "Draw straight lines (L)")) {
        selected_tool_ = DrawingTool::LINE;
        if (callbacks_.on_tool_changed) {
            callbacks_.on_tool_changed(DrawingTool::LINE);
        }
    }

    ImGui::PopStyleColor(3);

    if (icon_font) ImGui::PopFont();

    #undef ICON_DRAW
    #undef ICON_RECTANGLE
    #undef ICON_CIRCLE
    #undef ICON_ARROW_FORWARD
    #undef ICON_REMOVE
}

void AnnotationToolbar::RenderDrawingProperties() {
    // Color picker
    ImGui::Text("Color:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::ColorEdit4("##Color", &selected_color_.x,
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
        if (callbacks_.on_color_changed) {
            callbacks_.on_color_changed(selected_color_);
        }
    }

    // Stroke width
    ImGui::SameLine();
    ImGui::Text("Width:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderFloat("##Width", &stroke_width_, 1.0f, 20.0f, "%.1f")) {
        if (callbacks_.on_stroke_width_changed) {
            callbacks_.on_stroke_width_changed(stroke_width_);
        }
    }

    // Fill toggle (for shapes)
    if (selected_tool_ == DrawingTool::RECTANGLE || selected_tool_ == DrawingTool::OVAL) {
        ImGui::SameLine();
        if (ImGui::Checkbox("Fill", &fill_enabled_)) {
            if (callbacks_.on_fill_changed) {
                callbacks_.on_fill_changed(fill_enabled_);
            }
        }
    }
}

void AnnotationToolbar::RenderActionButtons(bool can_undo, bool can_redo, ImFont* icon_font,
                                            ImVec4 accent_regular, ImVec4 accent_muted_dark) {
    // Material Icons
    #define ICON_UNDO           u8"\uE166"
    #define ICON_REDO           u8"\uE15A"
    #define ICON_DELETE_SWEEP   u8"\uE16C"
    #define ICON_CHECK          u8"\uE5CA"
    #define ICON_CANCEL         u8"\uE5C9"

    // Undo button
    if (icon_font) ImGui::PushFont(icon_font);
    bool undo_clicked = ImGui::Button(icon_font ? ICON_UNDO : "Undo");
    if (!can_undo) {
        ImGui::BeginDisabled();
        undo_clicked = false;
    }
    if (icon_font) ImGui::PopFont();

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Undo last action (Ctrl+Z)");
    }

    if (undo_clicked && can_undo && callbacks_.on_undo) {
        callbacks_.on_undo();
    }
    if (!can_undo) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();

    // Redo button
    if (icon_font) ImGui::PushFont(icon_font);
    bool redo_clicked = ImGui::Button(icon_font ? ICON_REDO : "Redo");
    if (!can_redo) {
        ImGui::BeginDisabled();
        redo_clicked = false;
    }
    if (icon_font) ImGui::PopFont();

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Redo last undone action (Ctrl+Y)");
    }

    if (redo_clicked && can_redo && callbacks_.on_redo) {
        callbacks_.on_redo();
    }
    if (!can_redo) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();

    // Clear All button
    if (icon_font) ImGui::PushFont(icon_font);
    bool clear_clicked = ImGui::Button(icon_font ? ICON_DELETE_SWEEP : "Clear All");
    if (icon_font) ImGui::PopFont();

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Clear all annotations");
    }

    if (clear_clicked && callbacks_.on_clear_all) {
        callbacks_.on_clear_all();
    }

    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();

    // Calculate hover/active colors from accent colors
    ImVec4 regular_hover = ImVec4(
        accent_regular.x * 1.2f,
        accent_regular.y * 1.2f,
        accent_regular.z * 1.2f,
        accent_regular.w
    );
    ImVec4 regular_active = ImVec4(
        accent_regular.x * 0.8f,
        accent_regular.y * 0.8f,
        accent_regular.z * 0.8f,
        accent_regular.w
    );

    ImVec4 muted_dark_hover = ImVec4(
        accent_muted_dark.x * 1.2f,
        accent_muted_dark.y * 1.2f,
        accent_muted_dark.z * 1.2f,
        accent_muted_dark.w
    );
    ImVec4 muted_dark_active = ImVec4(
        accent_muted_dark.x * 0.8f,
        accent_muted_dark.y * 0.8f,
        accent_muted_dark.z * 0.8f,
        accent_muted_dark.w
    );

    // Done button (system accent regular) - always use text for visibility
    ImGui::PushStyleColor(ImGuiCol_Button, accent_regular);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, regular_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, regular_active);

    if (ImGui::Button("Save")) {
        if (callbacks_.on_done) {
            callbacks_.on_done();
        }
    }

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Save annotations and exit annotation mode (Enter)");
    }

    ImGui::PopStyleColor(3);

    ImGui::SameLine();

    // Cancel button (system accent muted dark) - always use text for visibility
    ImGui::PushStyleColor(ImGuiCol_Button, accent_muted_dark);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, muted_dark_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, muted_dark_active);

    if (ImGui::Button("Discard")) {
        if (callbacks_.on_cancel) {
            callbacks_.on_cancel();
        }
    }

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Discard annotations and exit annotation mode (Esc)");
    }

    ImGui::PopStyleColor(3);

    #undef ICON_UNDO
    #undef ICON_REDO
    #undef ICON_DELETE_SWEEP
    #undef ICON_CHECK
    #undef ICON_CANCEL
}

bool AnnotationToolbar::ToolButton(const char* label, DrawingTool tool, const char* tooltip) {
    bool is_selected = (selected_tool_ == tool);

    // Note: Colors are already pushed in RenderToolButtons for selected state
    // Just need to pop them temporarily for unselected buttons
    if (!is_selected) {
        ImGui::PopStyleColor(3);
    }

    bool clicked = ImGui::Button(label);

    if (!is_selected) {
        // Re-push for next button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.26f, 0.59f, 0.98f, 0.4f));  // Default button color
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.59f, 0.98f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.06f, 0.53f, 0.98f, 1.0f));
    }

    // Pop icon font before showing tooltip
    ImFont* current_font = ImGui::GetFont();
    bool using_icon_font = (current_font != ImGui::GetIO().FontDefault && current_font != ImGui::GetIO().Fonts->Fonts[0]);

    if (ImGui::IsItemHovered() && tooltip) {
        if (using_icon_font) ImGui::PopFont();
        ImGui::SetTooltip("%s", tooltip);
        if (using_icon_font) ImGui::PushFont(current_font);
    }

    return clicked;
}

bool AnnotationToolbar::ActionButton(const char* label, bool enabled, const char* tooltip) {
    if (!enabled) {
        ImGui::BeginDisabled();
    }

    bool clicked = ImGui::Button(label);

    if (!enabled) {
        ImGui::EndDisabled();
    }

    if (ImGui::IsItemHovered() && tooltip) {
        ImGui::SetTooltip("%s", tooltip);
    }

    return clicked && enabled;
}

} // namespace Annotations
} // namespace ump
