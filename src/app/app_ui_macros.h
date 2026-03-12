#pragma once

#include <imgui.h>

// HDR-aware UI color macros
// These convert SDR colors to PQ-encoded values when HDR is active
// Cross-platform: uses hdr_color_utils.h which queries the appropriate backend
#include "hdr/hdr_color_utils.h"
#define UI_WHITE qcview::GetUIWhite()
#define UI_WHITE_A(a) qcview::GetUIColor(IM_COL32(255, 255, 255, a))
#define UI_COLOR(r,g,b,a) qcview::GetUIColor(IM_COL32(r, g, b, a))
#define UI_WHITE_VEC4 qcview::GetUIColorVec4(ImVec4(1.0f, 1.0f, 1.0f, 1.0f))
#define UI_GRAY_VEC4 qcview::GetUIColorVec4(ImVec4(0.6f, 0.6f, 0.6f, 1.0f))
#define UI_LIGHT_GRAY qcview::GetUIColor(IM_COL32(220, 220, 220, 255))
#define UI_LIGHT_GRAY_A(a) qcview::GetUIColor(IM_COL32(220, 220, 220, a))

// Outline button style: transparent bg + visible border, solid fill on hover
inline void PushOutlineButtonStyle() {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));       // Transparent
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.28f, 0.28f, 0.28f, 0.50f));   // Current default fill on hover
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.15f, 0.15f, 0.15f, 1.00f));   // Dark on click
    ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(0.45f, 0.45f, 0.45f, 0.50f));   // Subtle gray outline
}

inline void PopOutlineButtonStyle() {
    ImGui::PopStyleColor(4);
}

// Outline header style: transparent bg + faint border, solid fill on hover
inline void PushOutlineHeaderStyle() {
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.25f, 0.25f, 0.25f, 0.15));       // Transparent-ish
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  ImVec4(0.28f, 0.28f, 0.28f, 0.50f));   // Fill on hover
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,   ImVec4(0.15f, 0.15f, 0.15f, 1.00f));   // Dark on click
    ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(0.25f, 0.25f, 0.25f, 0.50f));   // Faint border
}

inline void PopOutlineHeaderStyle() {
    ImGui::PopStyleColor(4);
}

// CollapsingHeader with an accent-colored icon drawn after the arrow via draw list.
// Label should have leading spaces ("      Text") to leave room for the icon overlay.
inline bool CollapsingHeaderWithIcon(const char* label, ImFont* icon_font,
                                      const char* icon, const ImVec4& color,
                                      ImGuiTreeNodeFlags flags = 0) {
    bool open = ImGui::CollapsingHeader(label, flags);
    if (icon_font) {
        ImVec2 header_min = ImGui::GetItemRectMin();
        float arrow_offset = ImGui::GetTreeNodeToLabelSpacing();
        float pad_y = ImGui::GetStyle().FramePadding.y;
        // Center icon between arrow and text: nudge right by 2px for even spacing
        float icon_x = header_min.x + arrow_offset + 5.0f;
        ImU32 col = ImGui::ColorConvertFloat4ToU32(color);
        ImGui::PushFont(icon_font);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(icon_x, header_min.y + pad_y), col, icon);
        ImGui::PopFont();
    }
    return open;
}
