// panel_color.cpp — CreateAnnotationPanel, CreateAnnotationToolbar, CreateColorPanels

#include "app/application.h"
#include "app/app_icons.h"
#include "app/app_ui_macros.h"
#include "app/ui_scale.h"
#include "ui/annotation_panel.h"

#include <imgui.h>

// Accent color helpers (declared in app_style.cpp)
ImVec4 GetWindowsAccentColor();
ImVec4 MutedDark(const ImVec4& color);

// Transparent border constant (used across panels)
static const ImVec4 kTransparentBorder = ImVec4(0, 0, 0, 0);

// Fonts
extern ImFont* font_icons;
extern ImFont* font_bold;

    void Application::CreateAnnotationPanel() {
        if (!annotation_panel) return;

        // Get system accent colors
        ImVec4 accent_regular = GetWindowsAccentColor();
        ImVec4 accent_muted_dark = MutedDark(GetWindowsAccentColor());

        annotation_panel->Render(&show_annotation_panel, accent_regular, accent_muted_dark);
    }

    void Application::CreateAnnotationToolbar() {
        // NOTE: Annotation toolbar is now rendered inline within the Video Viewport window
        // (see CreateVideoViewport - ##ToolbarArea child window around line 4546)
        // This function is kept as a no-op for backward compatibility but does nothing
        return;
    }

    void Application::CreateColorPanels() {
        if (!show_color_panels) return;

        // Load custom node trees on first open
        static bool custom_trees_loaded = false;
        if (!custom_trees_loaded) {
            LoadCustomNodeTrees();
            custom_trees_loaded = true;
        }

        ImGui::PushStyleColor(ImGuiCol_Border, kTransparentBorder);
        if (!ImGui::Begin("Color", &show_color_panels)) {
            ImGui::End();
            ImGui::PopStyleColor();  // kTransparentBorder
            return;
        }

        // Header row with close button
        {
            ImGui::PushStyleColor(ImGuiCol_Text, UI_GRAY_VEC4);
            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImGui::Text(ICON_PALETTE);
                ImGui::PopFont();
                ImGui::SameLine();
            }
            if (font_bold) ImGui::PushFont(font_bold);
            ImGui::Text("Color");
            if (font_bold) ImGui::PopFont();
            ImGui::PopStyleColor();

            // Close button on the right
            float button_size = ImGui::GetFontSize() + S(4.0f);  // Compact size
            ImGui::SameLine(ImGui::GetWindowWidth() - button_size - ImGui::GetStyle().WindowPadding.x);
            ImVec2 button_pos = ImGui::GetCursorScreenPos();
            bool clicked = ImGui::InvisibleButton("##CloseColor", ImVec2(button_size, button_size));
            bool hovered = ImGui::IsItemHovered();
            // Draw icon centered - disabled color by default, regular on hover
            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImVec2 icon_size = ImGui::CalcTextSize(ICON_CLOSE);
                ImVec2 icon_pos = ImVec2(button_pos.x + (button_size - icon_size.x) / 2,
                                         button_pos.y + (button_size - icon_size.y) / 2 - 1.0f);
                ImU32 icon_col = hovered ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
                ImGui::GetWindowDrawList()->AddText(icon_pos, icon_col, ICON_CLOSE);
                ImGui::PopFont();
            }
            if (clicked) {
                show_color_panels = false;
            }
            if (hovered) {
                ImGui::SetTooltip("Close Color (Ctrl+4)");
            }
        }
        ImGui::Separator();

        static float left_panel_width = S(350.0f);
        static float right_panel_width = S(400.0f);

        ImVec2 avail = ImGui::GetContentRegionAvail();

        // Left panel
        ImGui::BeginChild("ComponentPalette", ImVec2(left_panel_width, 0), true);
        CreateComponentPaletteContent();
        ImGui::EndChild();

        ImGui::SameLine();

        // Splitter between left and middle
        ImGui::InvisibleButton("##vsplitter1", ImVec2(S(4.0f), -1));
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);  // East-West resize cursor
        }
        if (ImGui::IsItemActive()) {
            left_panel_width += ImGui::GetIO().MouseDelta.x;
        }

        ImGui::SameLine();

        // Middle panel (node editor)
        float middle_width = avail.x - left_panel_width - right_panel_width - S(24.0f);
        ImGui::BeginChild("NodeEditor", ImVec2(middle_width, 0), true);
        CreateNodeEditorContent();
        ImGui::EndChild();

        ImGui::SameLine();

        // Splitter between middle and right
        ImGui::InvisibleButton("##vsplitter2", ImVec2(S(4.0f), -1));
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);  // East-West resize cursor
        }
        if (ImGui::IsItemActive()) {
            right_panel_width -= ImGui::GetIO().MouseDelta.x;
        }


        ImGui::SameLine();

        // Right panel
        ImGui::BeginChild("NodeProperties", ImVec2(right_panel_width, 0), true);
        CreateNodePropertiesContent();
        ImGui::EndChild();

        ImGui::End();
        ImGui::PopStyleColor();  // kTransparentBorder
    }
