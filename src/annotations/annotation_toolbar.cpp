#include "annotation_toolbar.h"
#include "../app/app_ui_macros.h"

namespace qcview {
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

    // Align text to frame padding for consistent vertical centering
    ImGui::AlignTextToFramePadding();

    // Render toolbar content inline (no window creation - parent manages layout)
    // Horizontal layout
    RenderToolButtons(s_icon_font, s_accent_regular);
    ImGui::SameLine();

    // Vertical divider
    RenderDivider();
    ImGui::SameLine();

    RenderDrawingProperties();
    ImGui::SameLine();

    // Vertical divider
    RenderDivider();
    ImGui::SameLine();

    RenderActionButtons(can_undo, can_redo, s_icon_font, s_accent_regular, s_accent_muted_dark);
}

void AnnotationToolbar::RenderDivider() {
    ImGui::Dummy(ImVec2(8, 0));
    ImGui::SameLine();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 2), ImVec2(p.x, p.y + h - 2), ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::Dummy(ImVec2(1, 0));
    ImGui::SameLine(0, 12);
}

void AnnotationToolbar::RenderToolButtons(ImFont* icon_font, ImVec4 accent_regular) {
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 0.9f);
    ImGui::Text("Tools:");
    ImGui::SameLine();

    // Material Icons
    #define ICON_DRAW           u8"\uE22B"
    #define ICON_RECTANGLE      u8"\ueb36"
    #define ICON_CIRCLE         u8"\uEF4A"
    #define ICON_ARROW_FORWARD  u8"\uE5C8"
    #define ICON_REMOVE         u8"\uE15B"

    if (icon_font) ImGui::PushFont(icon_font);

    if (ToolButton(icon_font ? ICON_DRAW : "Freehand", DrawingTool::FREEHAND, "Draw freehand strokes (F)", accent_regular)) {
        selected_tool_ = DrawingTool::FREEHAND;
        if (callbacks_.on_tool_changed) {
            callbacks_.on_tool_changed(DrawingTool::FREEHAND);
        }
    }

    ImGui::SameLine();
    if (ToolButton(icon_font ? ICON_RECTANGLE : "Rectangle", DrawingTool::RECTANGLE, "Draw rectangles (R)", accent_regular)) {
        selected_tool_ = DrawingTool::RECTANGLE;
        if (callbacks_.on_tool_changed) {
            callbacks_.on_tool_changed(DrawingTool::RECTANGLE);
        }
    }

    ImGui::SameLine();
    if (ToolButton(icon_font ? ICON_CIRCLE : "Oval", DrawingTool::OVAL, "Draw ovals/circles (O)", accent_regular)) {
        selected_tool_ = DrawingTool::OVAL;
        if (callbacks_.on_tool_changed) {
            callbacks_.on_tool_changed(DrawingTool::OVAL);
        }
    }

    ImGui::SameLine();
    if (ToolButton(icon_font ? ICON_ARROW_FORWARD : "Arrow", DrawingTool::ARROW, "Draw arrows (A)", accent_regular)) {
        selected_tool_ = DrawingTool::ARROW;
        if (callbacks_.on_tool_changed) {
            callbacks_.on_tool_changed(DrawingTool::ARROW);
        }
    }

    ImGui::SameLine();
    if (ToolButton(icon_font ? ICON_REMOVE : "Line", DrawingTool::LINE, "Draw straight lines (L)", accent_regular)) {
        selected_tool_ = DrawingTool::LINE;
        if (callbacks_.on_tool_changed) {
            callbacks_.on_tool_changed(DrawingTool::LINE);
        }
    }

    if (icon_font) ImGui::PopFont();

    #undef ICON_DRAW
    #undef ICON_RECTANGLE
    #undef ICON_CIRCLE
    #undef ICON_ARROW_FORWARD
    #undef ICON_REMOVE
}

void AnnotationToolbar::RenderDrawingProperties() {
    // Color picker with extra padding around popup to prevent accidental drawing
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 0.9f);
    ImGui::Text("Color:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    if (ImGui::ColorEdit4("##Color", &selected_color_.x,
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
        if (callbacks_.on_color_changed) {
            callbacks_.on_color_changed(selected_color_);
        }
    }
    ImGui::PopStyleVar(2);

    // Stroke width
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 0.9f);
    ImGui::Text("Width:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    // Match slider styling from bottom row
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y + 1));
    ImGui::SetWindowFontScale(0.85f);
    if (ImGui::SliderFloat("##Width", &stroke_width_, 1.0f, 20.0f, "%.1f")) {
        if (callbacks_.on_stroke_width_changed) {
            callbacks_.on_stroke_width_changed(stroke_width_);
        }
    }
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleVar();

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

    float btn_size = ImGui::GetFrameHeight();

    // Undo button - transparent background
    if (!can_undo) {
        ImGui::BeginDisabled();
    }

    if (icon_font) ImGui::PushFont(icon_font);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
    bool undo_clicked = ImGui::Button(icon_font ? ICON_UNDO : "Undo", ImVec2(btn_size, btn_size));
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);

    if (icon_font) ImGui::PopFont();

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Undo last action (Ctrl+Z)");
    }

    if (!can_undo) {
        ImGui::EndDisabled();
    }

    if (undo_clicked && can_undo && callbacks_.on_undo) {
        callbacks_.on_undo();
    }

    ImGui::SameLine();

    // Redo button - transparent background
    if (!can_redo) {
        ImGui::BeginDisabled();
    }

    if (icon_font) ImGui::PushFont(icon_font);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
    bool redo_clicked = ImGui::Button(icon_font ? ICON_REDO : "Redo", ImVec2(btn_size, btn_size));
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);

    if (icon_font) ImGui::PopFont();

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Redo last undone action (Ctrl+Y)");
    }

    if (!can_redo) {
        ImGui::EndDisabled();
    }

    if (redo_clicked && can_redo && callbacks_.on_redo) {
        callbacks_.on_redo();
    }

    ImGui::SameLine();

    // Clear All button - transparent background
    if (icon_font) ImGui::PushFont(icon_font);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
    bool clear_clicked = ImGui::Button(icon_font ? ICON_DELETE_SWEEP : "Clear All", ImVec2(btn_size, btn_size));
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);

    if (icon_font) ImGui::PopFont();

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Clear all annotations");
    }

    if (clear_clicked && callbacks_.on_clear_all) {
        callbacks_.on_clear_all();
    }

    ImGui::SameLine();

    // Vertical divider before Save/Discard
    RenderDivider();
    ImGui::SameLine();

    // Save button - uses regular button colors (not system accent)
    PushOutlineButtonStyle();
    if (ImGui::Button("Save")) {
        if (callbacks_.on_done) {
            callbacks_.on_done();
        }
    }
    PopOutlineButtonStyle();

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Save annotations and exit annotation mode (Enter)");
    }

    ImGui::SameLine();

    // Discard button - uses regular button colors (not system accent)
    PushOutlineButtonStyle();
    if (ImGui::Button("Discard")) {
        if (callbacks_.on_cancel) {
            callbacks_.on_cancel();
        }
    }
    PopOutlineButtonStyle();

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Discard annotations and exit annotation mode (Esc)");
    }

    #undef ICON_UNDO
    #undef ICON_REDO
    #undef ICON_DELETE_SWEEP
    #undef ICON_CHECK
    #undef ICON_CANCEL
}

bool AnnotationToolbar::ToolButton(const char* label, DrawingTool tool, const char* tooltip, ImVec4 accent_regular) {
    bool is_selected = (selected_tool_ == tool);
    float btn_size = ImGui::GetFrameHeight();

    // Push colors based on selection state - transparent by default, accent when selected
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));

    if (is_selected) {
        // Selected tool - use accent color background
        ImGui::PushStyleColor(ImGuiCol_Button, accent_regular);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(accent_regular.x * 1.2f, accent_regular.y * 1.2f, accent_regular.z * 1.2f, accent_regular.w));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(accent_regular.x * 0.8f, accent_regular.y * 0.8f, accent_regular.z * 0.8f, accent_regular.w));
    } else {
        // Not selected - transparent background with hover states
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
    }

    bool clicked = ImGui::Button(label, ImVec2(btn_size, btn_size));

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);

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
} // namespace qcview
