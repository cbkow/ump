// ============================================================================
// Color presets, component palette, node tree save/load
// ============================================================================

#include "app/application.h"
#include "app/app_icons.h"
#include "app/app_ui_macros.h"
#include "app/ui_scale.h"
#include "project/project_manager.h"
#include "nodes/node_manager.h"
#include "player/video_player.h"
#include "color/ocio_config_manager.h"
#include "color/ocio_pipeline.h"
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

// Globals defined in main.cpp
extern ImFont* font_regular;
extern ImFont* font_bold;
extern ImFont* font_icons;
extern std::unique_ptr<OCIOConfigManager> ocio_manager;

// Icons for Config tab sections
#define ICON_INPUT_CS      "\xEF\xA0\x96"   // U+F816
#define ICON_LOOKS         "\xEE\xAB\xB5"   // U+EAF5
#define ICON_LUT           "\xEE\x8F\xB5"   // U+E3F5
#define ICON_OUTPUT_DISP   "\xEF\xA0\x9B"   // U+F81B
// Icon for Presets tab headers and sub-items
#define ICON_COLOR_ITEM    "\xEF\x94\x80"   // U+F500

// Helper: draw a transparent text-colored icon inline before a Selectable
static void DrawItemIcon(const char* icon) {
    if (font_icons) {
        ImGui::PushFont(font_icons);
        ImVec4 text_col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(text_col.x, text_col.y, text_col.z, 0.4f));
        ImGui::Text("%s", icon);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SameLine();
    }
}

    void Application::CreateComponentPaletteContent() {
        // Create tab bar for Components vs Presets
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.25f, 0.25f, 0.50f));
        if (ImGui::BeginTabBar("PaletteTabBar", ImGuiTabBarFlags_None)) {

            // Current/Components Tab
            if (ImGui::BeginTabItem("Configs")) {
                CreateCurrentComponentsTab();
                ImGui::EndTabItem();
            }

            // Presets Tab
            if (ImGui::BeginTabItem("Presets")) {
                CreatePresetsTab();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
        ImGui::PopStyleColor();  // Border
    }

    void Application::CreateCurrentComponentsTab() {
        ImGui::PushStyleColor(ImGuiCol_Text, UI_GRAY_VEC4);
        if (font_icons) {
            ImGui::PushFont(font_icons);
            ImGui::Text(ICON_INPUT_SETTINGS);
            ImGui::PopFont();
            ImGui::SameLine();
        }
        if (font_bold) ImGui::PushFont(font_bold);
        ImGui::Text("OCIO Configs");
        if (font_bold) ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::Separator();

        if (!ocio_manager) {
            ImGui::TextDisabled("OCIO manager not initialized");
            return;
        }

        // Config dropdown (keeping as is)
        const auto& available_configs = ocio_manager->GetAvailableConfigs();
        if (!available_configs.empty()) {
            std::string current_config_name = ocio_manager->GetActiveConfigName();

            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.07f, 0.07f, 1.00f));
            if (ImGui::BeginCombo("Config", current_config_name.c_str())) {
                for (const auto& config_info : available_configs) {
                    bool is_selected = (config_info.name == current_config_name);
                    if (ImGui::Selectable(config_info.name.c_str(), is_selected)) {
                        ocio_manager->LoadConfiguration(config_info.type);

                        // Refresh current frame with new config
                        RefreshCurrentFrame();
                        Debug::Log("Switched OCIO config and refreshed frame");
                    }
                    if (is_selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopStyleColor();
        }

        ImGui::Separator();

        // Input colorspaces
        PushOutlineHeaderStyle();
        if (CollapsingHeaderWithIcon("    Input Colorspaces##ConfigTab", font_icons, ICON_INPUT_CS, ImGui::GetStyleColorVec4(ImGuiCol_Text))) {
            ImGui::PushID("InputCS");  // Unique scope for input colorspaces
            ImGui::Indent(S(8.0f));
            auto colorspaces = ocio_manager->GetInputColorSpaces();
            int cs_idx = 0;
            for (const auto& cs : colorspaces) {
                ImGui::PushID(cs_idx++);
                DrawItemIcon(ICON_INPUT_CS);
                if (font_regular) ImGui::PushFont(font_regular);
                ImGui::Selectable(cs.c_str());
                if (font_regular) ImGui::PopFont();

                // Check for double-click AFTER the Selectable
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (node_manager) {
                        node_manager->SetPendingNodeCreation(
                            qcview::NodeType::INPUT_COLORSPACE, cs);
                        Debug::Log("Double-clicked to create: " + cs);
                        UpdateColorPipeline();
                    }
                }

                // Drag source - use SourceAllowNullID flag
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    struct DragPayload {
                        qcview::NodeType type;
                        char name[256];
                    };

                    DragPayload payload;
                    payload.type = qcview::NodeType::INPUT_COLORSPACE;
                    strncpy(payload.name, cs.c_str(), sizeof(payload.name) - 1); payload.name[sizeof(payload.name) - 1] = '\0';

                    ImGui::SetDragDropPayload("OCIO_NODE", &payload, sizeof(payload));
                    ImGui::Text("Creating: %s", cs.c_str());
                    ImGui::EndDragDropSource();
                    UpdateColorPipeline();
                }
                ImGui::PopID();
            }
            ImGui::Unindent(8.0f);
            ImGui::PopID();  // InputCS
        }
        PopOutlineHeaderStyle();

        // Looks
        auto looks = ocio_manager->GetLooks();
        if (!looks.empty()) {
            PushOutlineHeaderStyle();
            if (CollapsingHeaderWithIcon("    Looks##ConfigTab", font_icons, ICON_LOOKS, ImGui::GetStyleColorVec4(ImGuiCol_Text))) {
                ImGui::PushID("Looks");  // Unique scope for looks
                ImGui::Indent(S(8.0f));
                int look_idx = 0;
                for (const auto& look : looks) {
                    ImGui::PushID(look_idx++);
                    DrawItemIcon(ICON_LOOKS);
                    if (font_regular) ImGui::PushFont(font_regular);
                    ImGui::Selectable(look.c_str());
                    if (font_regular) ImGui::PopFont();

                    // Check for double-click AFTER the Selectable
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (node_manager) {
                            node_manager->SetPendingNodeCreation(
                                qcview::NodeType::LOOK, look);
                            Debug::Log("Double-clicked to create: " + look);
                            UpdateColorPipeline();
                        }
                    }

                    // Drag source
                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                        struct DragPayload {
                            qcview::NodeType type;
                            char name[256];
                        };

                        DragPayload payload;
                        payload.type = qcview::NodeType::LOOK;
                        strncpy(payload.name, look.c_str(), sizeof(payload.name) - 1); payload.name[sizeof(payload.name) - 1] = '\0';

                        ImGui::SetDragDropPayload("OCIO_NODE", &payload, sizeof(payload));
                        ImGui::Text("Creating: %s", look.c_str());
                        ImGui::EndDragDropSource();
                        UpdateColorPipeline();
                    }
                    ImGui::PopID();
                }
                ImGui::Unindent(8.0f);
                ImGui::PopID();  // Looks
            }
            PopOutlineHeaderStyle();
        }

        // LUT
        PushOutlineHeaderStyle();
        if (CollapsingHeaderWithIcon("    LUT##ConfigTab", font_icons, ICON_LUT, ImGui::GetStyleColorVec4(ImGuiCol_Text))) {
            ImGui::Indent(S(8.0f));
            // Scene-Referred LUT (applied before display transform)
            ImGui::PushID("SceneLUT##ConfigTab");
            DrawItemIcon(ICON_LUT);
            if (font_regular) ImGui::PushFont(font_regular);
            ImGui::Selectable("Scene-Referred LUT");
            if (font_regular) ImGui::PopFont();

            // Double-click to create node
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (node_manager) {
                    node_manager->SetPendingNodeCreation(qcview::NodeType::SCENE_LUT, "");
                    Debug::Log("Double-clicked to create Scene LUT node");
                }
            }

            // Drag source
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                struct DragPayload {
                    qcview::NodeType type;
                    char path[512];
                };
                DragPayload payload;
                payload.type = qcview::NodeType::SCENE_LUT;
                payload.path[0] = '\0';  // Empty path

                ImGui::SetDragDropPayload("OCIO_NODE", &payload, sizeof(payload));
                ImGui::Text("Creating: Scene LUT");
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();

            // Display-Referred LUT (applied after display transform)
            ImGui::PushID("DisplayLUT##ConfigTab");
            DrawItemIcon(ICON_LUT);
            if (font_regular) ImGui::PushFont(font_regular);
            ImGui::Selectable("Display-Referred LUT");
            if (font_regular) ImGui::PopFont();

            // Double-click to create node
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (node_manager) {
                    node_manager->SetPendingNodeCreation(qcview::NodeType::DISPLAY_LUT, "");
                    Debug::Log("Double-clicked to create Display LUT node");
                }
            }

            // Drag source
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                struct DragPayload {
                    qcview::NodeType type;
                    char path[512];
                };
                DragPayload payload;
                payload.type = qcview::NodeType::DISPLAY_LUT;
                payload.path[0] = '\0';  // Empty path

                ImGui::SetDragDropPayload("OCIO_NODE", &payload, sizeof(payload));
                ImGui::Text("Creating: Display LUT");
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
            ImGui::Unindent(8.0f);
        }
        PopOutlineHeaderStyle();

        // Output displays
        auto displays = ocio_manager->GetDisplays();
        if (!displays.empty()) {
            PushOutlineHeaderStyle();
            if (CollapsingHeaderWithIcon("    Output Displays##ConfigTab", font_icons, ICON_OUTPUT_DISP, ImGui::GetStyleColorVec4(ImGuiCol_Text))) {
                ImGui::PushID("OutputDisp");  // Unique scope for output displays
                ImGui::Indent(S(8.0f));
                int display_idx = 0;
                for (const auto& display : displays) {
                    ImGui::PushID(display_idx++);
                    DrawItemIcon(ICON_OUTPUT_DISP);
                    if (font_regular) ImGui::PushFont(font_regular);
                    ImGui::Selectable(display.c_str());
                    if (font_regular) ImGui::PopFont();

                    // Check for double-click AFTER the Selectable
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (node_manager) {
                            node_manager->SetPendingNodeCreation(
                                qcview::NodeType::OUTPUT_DISPLAY, display);
                            Debug::Log("Double-clicked to create: " + display);
                            UpdateColorPipeline();
                        }
                    }

                    // Drag source
                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                        struct DragPayload {
                            qcview::NodeType type;
                            char name[256];
                        };

                        DragPayload payload;
                        payload.type = qcview::NodeType::OUTPUT_DISPLAY;
                        strncpy(payload.name, display.c_str(), sizeof(payload.name) - 1); payload.name[sizeof(payload.name) - 1] = '\0';

                        ImGui::SetDragDropPayload("OCIO_NODE", &payload, sizeof(payload));
                        ImGui::Text("Creating: %s", display.c_str());
                        ImGui::EndDragDropSource();
                        UpdateColorPipeline();
                    }
                    ImGui::PopID();
                }
                ImGui::Unindent(8.0f);
                ImGui::PopID();  // OutputDisp
            }
            PopOutlineHeaderStyle();
        }
    }

    void Application::CreatePresetsTab() {
        ImGui::PushStyleColor(ImGuiCol_Text, UI_GRAY_VEC4);
        if (font_icons) {
            ImGui::PushFont(font_icons);
            ImGui::Text(ICON_FLOWCHART);
            ImGui::PopFont();
            ImGui::SameLine();
        }
        if (font_bold) ImGui::PushFont(font_bold);
        ImGui::Text("Color Presets");
        if (font_bold) ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::Separator();

        // Style for all collapsing headers in this tab
        PushOutlineHeaderStyle();

        // Standard Presets
        if (CollapsingHeaderWithIcon("    Standard##PresetsTab", font_icons, ICON_COLOR_ITEM, ImGui::GetStyleColorVec4(ImGuiCol_Text))) {
            ImGui::PushID("StandardPresets##Tab");
            ImGui::Indent(S(8.0f));
            CreateStandardPresets();
            ImGui::Unindent(8.0f);
            ImGui::PopID();
        }

        // ACES 1.3 Presets
        if (CollapsingHeaderWithIcon("    ACES 1.3##PresetsTab", font_icons, ICON_COLOR_ITEM, ImGui::GetStyleColorVec4(ImGuiCol_Text))) {
            ImGui::PushID("ACES13Presets##Tab");
            ImGui::Indent(S(8.0f));
            CreateACESPresets();
            ImGui::Unindent(8.0f);
            ImGui::PopID();
        }

        // ACES 2.0 Presets
        if (CollapsingHeaderWithIcon("    ACES 2.0##PresetsTab", font_icons, ICON_COLOR_ITEM, ImGui::GetStyleColorVec4(ImGuiCol_Text))) {
            ImGui::PushID("ACES20Presets##Tab");
            ImGui::Indent(S(8.0f));
            CreateACES20Presets();
            ImGui::Unindent(8.0f);
            ImGui::PopID();
        }

        // Blender 5.1 Presets
        if (CollapsingHeaderWithIcon("    Blender 5.1##PresetsTab", font_icons, ICON_COLOR_ITEM, ImGui::GetStyleColorVec4(ImGuiCol_Text))) {
            ImGui::PushID("Blender51Presets##Tab");
            ImGui::Indent(S(8.0f));
            CreateBlender5Presets();
            ImGui::Unindent(8.0f);
            ImGui::PopID();
        }

        // Custom Presets
        if (CollapsingHeaderWithIcon("    Custom##PresetsTab", font_icons, ICON_COLOR_ITEM, ImGui::GetStyleColorVec4(ImGuiCol_Text))) {
            ImGui::PushID("CustomPresets##Tab");
            ImGui::Indent(S(8.0f));
            CreateCustomPresets();
            ImGui::Unindent(8.0f);
            ImGui::PopID();
        }

        PopOutlineHeaderStyle();
    }

    void Application::CreateACESPresets() {
        if (!ocio_manager || !ocio_manager->IsConfigLoaded()) {
            ImGui::TextDisabled("No ACES config loaded");
            return;
        }


        // ACES presets using alias resolver

        if (font_regular) ImGui::PushFont(font_regular);
        DrawItemIcon(ICON_COLOR_ITEM);
        if (ImGui::Selectable("ACEScg -> sRGB")) {
            ApplyAliasPreset("lin_ap1", "srgb_display", "ACES 1.0 - SDR Video");
        }

        DrawItemIcon(ICON_COLOR_ITEM);
        if (ImGui::Selectable("ACES2065-1 -> sRGB")) {
            ApplyAliasPreset("aces2065_1", "srgb_display", "ACES 1.0 - SDR Video");
        }
        if (font_regular) ImGui::PopFont();
    }

    void Application::CreateStandardPresets() {
        if (!ocio_manager || !ocio_manager->IsConfigLoaded()) {
            ImGui::TextDisabled("No config loaded");
            return;
        }

        if (font_regular) ImGui::PushFont(font_regular);

        // SDR preset: Rec.709 -> sRGB
        DrawItemIcon(ICON_COLOR_ITEM);
        if (ImGui::Selectable("Rec.709 -> sRGB")) {
            ApplyPreset(R"({
                "name": "Rec.709 to sRGB Standard",
                "ocio_config": "Blender5.1",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "Rec.1886", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "sRGB", "view": "Standard", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Standard Rec.1886 to sRGB transform");
        }

#ifdef _WIN32
        // Windows HDR presets
        DrawItemIcon(ICON_COLOR_ITEM);
        if (ImGui::Selectable("sRGB to Windows HDR")) {
            ApplyPreset(R"JSON({
                "name": "sRGB to Windows HDR",
                "ocio_config": "ACES_2.0",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "sRGB - Display", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "Rec.2100-PQ - Display", "view": "Video (colorimetric)", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })JSON");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("sRGB to Rec.2100-PQ (HDR10) colorimetric transform\nFor Windows HDR output");
        }

        DrawItemIcon(ICON_COLOR_ITEM);
        if (ImGui::Selectable("Rec.709 to Windows HDR")) {
            ApplyPreset(R"JSON({
                "name": "Rec.709 to Windows HDR",
                "ocio_config": "ACES_2.0",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "Rec.1886 Rec.709 - Display", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "Rec.2100-PQ - Display", "view": "Video (colorimetric)", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })JSON");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Rec.1886/Rec.709 to Rec.2100-PQ (HDR10) colorimetric transform\nFor Windows HDR output");
        }
#endif

#ifdef __APPLE__
        // macOS EDR presets
        DrawItemIcon(ICON_COLOR_ITEM);
        if (ImGui::Selectable("Linear Rec.709 -> EDR sRGB HDR 1000 nits")) {
            ApplyPreset(R"JSON({
                "name": "Linear Rec.709 to EDR sRGB HDR 1000 nits",
                "ocio_config": "Blender5.1",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "Linear Rec.709", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "Linear sRGB EDR", "view": "ACES 2.0 - HDR 1000 nits", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })JSON");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Linear Rec.709 to EDR linear sRGB with ACES 2.0 HDR 1000 nit tone mapping");
        }

        DrawItemIcon(ICON_COLOR_ITEM);
        if (ImGui::Selectable("Linear Rec.709 -> EDR P3 HDR 1000 nits")) {
            ApplyPreset(R"JSON({
                "name": "Linear Rec.709 to EDR P3 HDR 1000 nits",
                "ocio_config": "Blender5.1",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "Linear Rec.709", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "Linear P3 EDR", "view": "ACES 2.0 - HDR 1000 nits", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })JSON");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Linear Rec.709 to EDR linear P3 with ACES 2.0 HDR 1000 nit tone mapping");
        }

        DrawItemIcon(ICON_COLOR_ITEM);
        if (ImGui::Selectable("ACEScg -> EDR sRGB HDR 1000 nits")) {
            ApplyPreset(R"JSON({
                "name": "ACEScg to EDR sRGB HDR 1000 nits",
                "ocio_config": "ACES_2.0",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "ACEScg", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "Linear sRGB EDR - Display", "view": "ACES 2.0 - HDR 1000 nits (P3 D65)", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })JSON");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("ACEScg to EDR linear sRGB with ACES 2.0 HDR 1000 nit tone mapping");
        }

        DrawItemIcon(ICON_COLOR_ITEM);
        if (ImGui::Selectable("ACEScg -> EDR P3 HDR 1000 nits")) {
            ApplyPreset(R"JSON({
                "name": "ACEScg to EDR P3 HDR 1000 nits",
                "ocio_config": "ACES_2.0",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "ACEScg", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "Linear P3 EDR - Display", "view": "ACES 2.0 - HDR 1000 nits (P3 D65)", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })JSON");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("ACEScg to EDR linear P3 with ACES 2.0 HDR 1000 nit tone mapping");
        }
#endif

        if (font_regular) ImGui::PopFont();
    }

    void Application::CreateBlender5Presets() {
        if (font_regular) ImGui::PushFont(font_regular);

        DrawItemIcon(ICON_COLOR_ITEM);
        if (ImGui::Selectable("Linear Rec.709 -> sRGB Standard##B5")) {
            ApplyPreset(R"({
                "name": "Linear Rec.709 to sRGB Standard",
                "ocio_config": "Blender5.1",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "Linear Rec.709", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "sRGB", "view": "Standard", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })");
        }

        DrawItemIcon(ICON_COLOR_ITEM);
        if (ImGui::Selectable("Linear Rec.709 -> sRGB AgX##B5")) {
            ApplyPreset(R"({
                "name": "Linear Rec.709 to sRGB AgX",
                "ocio_config": "Blender5.1",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "Linear Rec.709", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "sRGB", "view": "AgX", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })");
        }

        DrawItemIcon(ICON_COLOR_ITEM);
        if (ImGui::Selectable("Linear Rec.709 -> Display P3 Standard##B5")) {
            ApplyPreset(R"({
                "name": "Linear Rec.709 to Display P3 Standard",
                "ocio_config": "Blender5.1",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "Linear Rec.709", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "Display P3", "view": "Standard", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })");
        }

#ifdef __APPLE__
        // macOS EDR presets
        DrawItemIcon(ICON_COLOR_ITEM);
        if (ImGui::Selectable("Linear Rec.709 -> EDR sRGB HDR 1000 nits##B5")) {
            ApplyPreset(R"JSON({
                "name": "Linear Rec.709 to EDR sRGB HDR 1000 nits",
                "ocio_config": "Blender5.1",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "Linear Rec.709", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "Linear sRGB EDR", "view": "ACES 2.0 - HDR 1000 nits", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })JSON");
        }

        DrawItemIcon(ICON_COLOR_ITEM);
        if (ImGui::Selectable("Linear Rec.709 -> EDR P3 HDR 1000 nits##B5")) {
            ApplyPreset(R"JSON({
                "name": "Linear Rec.709 to EDR P3 HDR 1000 nits",
                "ocio_config": "Blender5.1",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "Linear Rec.709", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "Linear P3 EDR", "view": "ACES 2.0 - HDR 1000 nits", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })JSON");
        }
#endif

        if (font_regular) ImGui::PopFont();
    }

    void Application::CreateACES20Presets() {
        if (font_regular) ImGui::PushFont(font_regular);

        // SDR Video (colorimetric) presets
        DrawItemIcon(ICON_COLOR_ITEM);
        if (ImGui::Selectable("ACEScg -> sRGB Video (colorimetric)##A20")) {
            ApplyPreset(R"JSON({
                "name": "ACEScg to sRGB Video (colorimetric)",
                "ocio_config": "ACES_2.0",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "ACEScg", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "sRGB - Display", "view": "Video (colorimetric)", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })JSON");
        }

        DrawItemIcon(ICON_COLOR_ITEM);
        if (ImGui::Selectable("ACES2065-1 -> sRGB Video (colorimetric)##A20")) {
            ApplyPreset(R"JSON({
                "name": "ACES2065-1 to sRGB Video (colorimetric)",
                "ocio_config": "ACES_2.0",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "ACES2065-1", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "sRGB - Display", "view": "Video (colorimetric)", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })JSON");
        }

        DrawItemIcon(ICON_COLOR_ITEM);
        if (ImGui::Selectable("ACEScg -> Display P3 Video (colorimetric)##A20")) {
            ApplyPreset(R"JSON({
                "name": "ACEScg to Display P3 Video (colorimetric)",
                "ocio_config": "ACES_2.0",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "ACEScg", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "Display P3 - Display", "view": "Video (colorimetric)", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })JSON");
        }

#ifdef __APPLE__
        // macOS EDR presets
        DrawItemIcon(ICON_COLOR_ITEM);
        if (ImGui::Selectable("ACEScg -> EDR sRGB HDR 1000 nits##A20")) {
            ApplyPreset(R"JSON({
                "name": "ACEScg to EDR sRGB HDR 1000 nits",
                "ocio_config": "ACES_2.0",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "ACEScg", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "Linear sRGB EDR - Display", "view": "ACES 2.0 - HDR 1000 nits (P3 D65)", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })JSON");
        }

        DrawItemIcon(ICON_COLOR_ITEM);
        if (ImGui::Selectable("ACEScg -> EDR P3 HDR 1000 nits##A20")) {
            ApplyPreset(R"JSON({
                "name": "ACEScg to EDR P3 HDR 1000 nits",
                "ocio_config": "ACES_2.0",
                "nodes": [
                    {"type": "INPUT_COLORSPACE", "data": "ACEScg", "position": [100, 100]},
                    {"type": "OUTPUT_DISPLAY", "display": "Linear P3 EDR - Display", "view": "ACES 2.0 - HDR 1000 nits (P3 D65)", "position": [400, 100]}
                ],
                "connections": [{"from_node": 0, "from_pin": 0, "to_node": 1, "to_pin": 0}]
            })JSON");
        }
#endif

        if (font_regular) ImGui::PopFont();
    }

    std::string Application::GetNodeTreesFolder() {
        const char* localappdata = std::getenv("LOCALAPPDATA");
        if (localappdata) {
            std::filesystem::path nodes_path = std::filesystem::path(localappdata) / "qcview" / "nodes";

            // Create directory if it doesn't exist
            if (!std::filesystem::exists(nodes_path)) {
                std::filesystem::create_directories(nodes_path);
                Debug::Log("Created node trees folder: " + nodes_path.string());
            }

            return nodes_path.string();
        }

        // Fallback
        return "nodes/";
    }

    void Application::SaveCurrentNodeTree(const std::string& name) {
        if (!node_manager) {
            Debug::Log("ERROR: No node manager available");
            return;
        }

        using json = nlohmann::json;

        try {
            // Build preset JSON in the same format as existing presets
            json preset;
            preset["name"] = name;

            // Save the active OCIO config type for this preset
            if (ocio_manager && ocio_manager->IsConfigLoaded()) {
                OCIOConfigType config_type = ocio_manager->GetActiveConfigType();
                if (config_type == OCIOConfigType::ACES_13) {
                    preset["ocio_config"] = "ACES_1.3";
                } else if (config_type == OCIOConfigType::ACES_20) {
                    preset["ocio_config"] = "ACES_2.0";
                } else if (config_type == OCIOConfigType::BLENDER) {
                    preset["ocio_config"] = "Blender";
                } else if (config_type == OCIOConfigType::BLENDER5) {
                    preset["ocio_config"] = "Blender5";
                } else if (config_type == OCIOConfigType::CUSTOM) {
                    preset["ocio_config"] = "Custom";
                }
                Debug::Log("Saving preset with config: " + preset["ocio_config"].get<std::string>());
            }

            // Serialize nodes
            json nodes_array = json::array();
            auto all_nodes = node_manager->GetAllNodes();

            Debug::Log("SaveNodeTree: Found " + std::to_string(all_nodes.size()) + " nodes");

            // Create a map from node ID to array index
            std::unordered_map<int, int> node_id_to_index;
            int index = 0;

            for (auto* node : all_nodes) {
                if (!node) continue;

                node_id_to_index[node->GetId()] = index++;

                json node_obj;
                ImVec2 pos = ImNodes::GetNodeGridSpacePos(node->GetId());
                node_obj["position"] = json::array({pos.x, pos.y});

                // Store type-specific data
                switch (node->GetType()) {
                    case qcview::NodeType::INPUT_COLORSPACE: {
                        auto* cs_node = dynamic_cast<qcview::InputColorSpaceNode*>(node);
                        if (cs_node) {
                            node_obj["type"] = "INPUT_COLORSPACE";
                            node_obj["data"] = cs_node->GetColorSpace();
                            Debug::Log("  - Node " + std::to_string(index-1) + ": INPUT " + cs_node->GetColorSpace());
                        }
                        break;
                    }
                    case qcview::NodeType::LOOK: {
                        auto* look_node = dynamic_cast<qcview::LookNode*>(node);
                        if (look_node) {
                            node_obj["type"] = "LOOK";
                            node_obj["data"] = look_node->GetLook();
                            Debug::Log("  - Node " + std::to_string(index-1) + ": LOOK " + look_node->GetLook());
                        }
                        break;
                    }
                    case qcview::NodeType::SCENE_LUT: {
                        auto* lut_node = dynamic_cast<qcview::SceneLUTNode*>(node);
                        if (lut_node) {
                            node_obj["type"] = "SCENE_LUT";
                            node_obj["data"] = lut_node->GetLUTPath();
                            Debug::Log("  - Node " + std::to_string(index-1) + ": SCENE_LUT " + lut_node->GetLUTFileName());
                        }
                        break;
                    }
                    case qcview::NodeType::DISPLAY_LUT: {
                        auto* lut_node = dynamic_cast<qcview::DisplayLUTNode*>(node);
                        if (lut_node) {
                            node_obj["type"] = "DISPLAY_LUT";
                            node_obj["data"] = lut_node->GetLUTPath();
                            Debug::Log("  - Node " + std::to_string(index-1) + ": DISPLAY_LUT " + lut_node->GetLUTFileName());
                        }
                        break;
                    }
                    case qcview::NodeType::OUTPUT_DISPLAY: {
                        auto* output_node = dynamic_cast<qcview::OutputDisplayNode*>(node);
                        if (output_node) {
                            node_obj["type"] = "OUTPUT_DISPLAY";
                            node_obj["display"] = output_node->GetDisplay();
                            node_obj["view"] = output_node->GetView();

                            // Save aliases for config portability (if available)
                            std::string display_alias = output_node->GetDisplayAlias();
                            std::string view_alias = output_node->GetViewAlias();

                            if (!display_alias.empty()) {
                                node_obj["display_alias"] = display_alias;
                            }
                            if (!view_alias.empty()) {
                                node_obj["view_alias"] = view_alias;
                            }

                            Debug::Log("  - Node " + std::to_string(index-1) + ": OUTPUT " + output_node->GetDisplay() +
                                      (display_alias.empty() ? "" : " (alias: " + display_alias + ")"));
                        }
                        break;
                    }
                }

                nodes_array.push_back(node_obj);
            }
            preset["nodes"] = nodes_array;

            // Serialize connections using node indices
            json connections_array = json::array();
            auto connections = node_manager->GetConnections();

            Debug::Log("SaveNodeTree: Found " + std::to_string(connections.size()) + " connections");

            for (const auto& conn : connections) {
                // Find which nodes own these pins
                int from_node_idx = -1;
                int to_node_idx = -1;

                for (auto* node : all_nodes) {
                    if (!node) continue;

                    // Check if this node owns the from_pin (output pin)
                    if (from_node_idx == -1) {  // Only search if not found yet
                        for (const auto& pin : node->GetOutputPins()) {
                            if (pin.id == conn.from_pin) {
                                from_node_idx = node_id_to_index[node->GetId()];
                                break;
                            }
                        }
                    }

                    // Check if this node owns the to_pin (input pin)
                    if (to_node_idx == -1) {  // Only search if not found yet
                        for (const auto& pin : node->GetInputPins()) {
                            if (pin.id == conn.to_pin) {
                                to_node_idx = node_id_to_index[node->GetId()];
                                break;
                            }
                        }
                    }

                    // If we found both, no need to continue
                    if (from_node_idx >= 0 && to_node_idx >= 0) {
                        break;
                    }
                }

                if (from_node_idx >= 0 && to_node_idx >= 0) {
                    json conn_obj;
                    conn_obj["from_node"] = from_node_idx;
                    conn_obj["from_pin"] = 0;  // Always 0 for our simple nodes
                    conn_obj["to_node"] = to_node_idx;
                    conn_obj["to_pin"] = 0;  // Always 0 for our simple nodes
                    connections_array.push_back(conn_obj);
                    Debug::Log("  - Connection: " + std::to_string(from_node_idx) + " -> " + std::to_string(to_node_idx));
                } else {
                    Debug::Log("  - WARNING: Could not find nodes for connection (from_pin=" + std::to_string(conn.from_pin) + ", to_pin=" + std::to_string(conn.to_pin) + ")");
                }
            }
            preset["connections"] = connections_array;

            // Write to file
            std::string folder = GetNodeTreesFolder();
            std::string file_path = folder + "/" + name + ".qcvnode";

            std::ofstream file(file_path);
            if (!file.is_open()) {
                Debug::Log("ERROR: Failed to save node tree: " + file_path);
                return;
            }

            file << preset.dump(2);
            file.close();

            Debug::Log("Saved node tree: " + file_path);

            // Reload custom presets list
            LoadCustomNodeTrees();

        } catch (const std::exception& e) {
            Debug::Log("ERROR saving node tree: " + std::string(e.what()));
        }
    }


    void Application::LoadCustomNodeTrees() {
        custom_node_trees.clear();

        std::string folder = GetNodeTreesFolder();

        try {
            for (const auto& entry : std::filesystem::directory_iterator(folder)) {
                if (entry.is_regular_file() && entry.path().extension() == ".qcvnode") {
                    std::string name = entry.path().stem().string();
                    custom_node_trees.push_back(name);
                }
            }

            Debug::Log("Loaded " + std::to_string(custom_node_trees.size()) + " custom node trees");

        } catch (const std::exception& e) {
            Debug::Log("ERROR loading custom node trees: " + std::string(e.what()));
        }
    }

    void Application::LoadNodeTreeFromFile(const std::string& name) {
        try {
            std::string folder = GetNodeTreesFolder();
            std::string file_path = folder + "/" + name + ".qcvnode";

            std::ifstream file(file_path);
            if (!file.is_open()) {
                Debug::Log("ERROR: Failed to open node tree: " + file_path);
                return;
            }

            // Read the entire file as string
            std::string json_content((std::istreambuf_iterator<char>(file)),
                                      std::istreambuf_iterator<char>());
            file.close();

            // Use the existing ApplyPreset function which handles everything
            ApplyPreset(json_content);

            Debug::Log("Loaded custom node tree: " + name);

        } catch (const std::exception& e) {
            Debug::Log("ERROR loading node tree: " + std::string(e.what()));
        }
    }

    void Application::DeleteNodeTree(const std::string& name) {
        try {
            std::string folder = GetNodeTreesFolder();
            std::string file_path = folder + "/" + name + ".qcvnode";

            if (std::filesystem::exists(file_path)) {
                std::filesystem::remove(file_path);
                Debug::Log("Deleted node tree: " + name);

                // Reload list
                LoadCustomNodeTrees();
            }

        } catch (const std::exception& e) {
            Debug::Log("ERROR deleting node tree: " + std::string(e.what()));
        }
    }

    // ------------------------------------------------------------------------
    // CUSTOM LUT LIBRARY MANAGEMENT
    // ------------------------------------------------------------------------

    void Application::CreateCustomPresets() {
        // Show custom presets
        if (font_regular) ImGui::PushFont(font_regular);
        for (const auto& tree_name : custom_node_trees) {
            ImGui::PushID(tree_name.c_str());

            DrawItemIcon(ICON_COLOR_ITEM);
            if (ImGui::Selectable(tree_name.c_str())) {
                LoadNodeTreeFromFile(tree_name);
            }

            // Right-click to delete
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Delete")) {
                    DeleteNodeTree(tree_name);
                }
                ImGui::EndPopup();
            }

            ImGui::PopID();
        }
        if (font_regular) ImGui::PopFont();

        if (custom_node_trees.empty()) {
            ImGui::TextDisabled("No custom presets defined");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Save button with name input
        static char preset_name[256] = "My Preset";
        ImGui::InputText("Name", preset_name, sizeof(preset_name));

        PushOutlineButtonStyle();
        if (ImGui::Button("Save Current as Preset", ImVec2(-1, 0))) {
            if (strlen(preset_name) > 0) {
                SaveCurrentNodeTree(preset_name);
            } else {
                Debug::Log("ERROR: Preset name cannot be empty");
            }
        }
        PopOutlineButtonStyle();
    }

    void Application::ApplyPreset(const std::string& json_preset) {
        Debug::Log("=== Applying Color Preset ===");

        // Parse JSON to check if it specifies a config type
        OCIOConfigType target_config = OCIOConfigType::BLENDER;  // Default to Blender for backward compatibility

        try {
            nlohmann::json j = nlohmann::json::parse(json_preset);

            // Check if preset specifies which OCIO config to use
            if (j.contains("ocio_config")) {
                std::string config_str = j["ocio_config"].get<std::string>();
                Debug::Log("Preset requires OCIO config: " + config_str);

                if (config_str == "ACES_1.3") {
                    target_config = OCIOConfigType::ACES_13;
                } else if (config_str == "ACES_2.0") {
                    target_config = OCIOConfigType::ACES_20;
                } else if (config_str == "Blender") {
                    target_config = OCIOConfigType::BLENDER;
                } else if (config_str == "Blender5" || config_str == "Blender5.1") {
                    target_config = OCIOConfigType::BLENDER5;
                } else if (config_str == "Custom") {
                    target_config = OCIOConfigType::CUSTOM;
                }
            } else {
                Debug::Log("Preset has no config type specified - defaulting to Blender");
            }
        } catch (const std::exception& e) {
            Debug::Log("WARNING: Could not parse config type from preset: " + std::string(e.what()));
        }

        // Switch to the required config
        if (!ocio_manager || !ocio_manager->SwitchToConfig(target_config)) {
            Debug::Log("ERROR: Failed to switch to required OCIO config");
            return;
        }
        Debug::Log("Successfully switched to config for preset");

        // Store the active config in node manager so transcode knows which config to use
        if (node_manager) {
            node_manager->SetActiveOCIOConfig(target_config);
            Debug::Log("Set node manager config type: " + std::to_string(static_cast<int>(target_config)));
        }

        // Parse JSON (simplified - in production would use proper JSON library)
        // For now, extract the key information manually

        // Clear existing nodes first
        if (node_manager) {
            Debug::Log("Clearing existing nodes...");
            auto all_nodes = node_manager->GetAllNodes();
            for (auto* node : all_nodes) {
                if (node) {
                    node_manager->DeleteNode(node->GetId());
                }
            }
        }

        // Parse JSON using nlohmann::json
        std::vector<NodePresetData> nodes;
        std::vector<ConnectionPresetData> connections;

        try {
            nlohmann::json j = nlohmann::json::parse(json_preset);
            Debug::Log("Parsing JSON preset with " + std::to_string(j["nodes"].size()) + " nodes");

            // Parse nodes
            for (const auto& node_json : j["nodes"]) {
                NodePresetData node_data;
                node_data.type = node_json["type"].get<std::string>();

                // Get position
                if (node_json.contains("position")) {
                    node_data.position.x = node_json["position"][0].get<float>();
                    node_data.position.y = node_json["position"][1].get<float>();
                }

                // Get data based on node type
                if (node_data.type == "INPUT_COLORSPACE" || node_data.type == "LOOK" || node_data.type == "SCENE_LUT" || node_data.type == "DISPLAY_LUT") {
                    node_data.data = node_json["data"].get<std::string>();
                }
                else if (node_data.type == "OUTPUT_DISPLAY") {
                    node_data.display = node_json["display"].get<std::string>();
                    node_data.view = node_json["view"].get<std::string>();

                    // Parse aliases (optional, for backward compatibility with old presets)
                    if (node_json.contains("display_alias")) {
                        node_data.display_alias = node_json["display_alias"].get<std::string>();
                    } else if (ocio_manager && ocio_manager->IsConfigLoaded()) {
                        // Old preset without alias - try to get alias from current config
                        node_data.display_alias = ocio_manager->GetBestAlias(node_data.display);
                    }

                    if (node_json.contains("view_alias")) {
                        node_data.view_alias = node_json["view_alias"].get<std::string>();
                    } else {
                        // Playback controller already exists - reload dummy video and re-enable timeline mode
                        // Views typically don't have separate aliases, use view name
                        node_data.view_alias = node_data.view;
                    }
                }

                nodes.push_back(node_data);
                Debug::Log("Parsed " + node_data.type + " node: " +
                          (node_data.type == "OUTPUT_DISPLAY" ? node_data.display : node_data.data));
            }

            // Parse connections
            for (const auto& conn_json : j["connections"]) {
                ConnectionPresetData conn_data;
                conn_data.from_node = conn_json["from_node"].get<int>();
                conn_data.from_pin = conn_json["from_pin"].get<int>();
                conn_data.to_node = conn_json["to_node"].get<int>();
                conn_data.to_pin = conn_json["to_pin"].get<int>();
                connections.push_back(conn_data);
            }

        } catch (const std::exception& e) {
            Debug::Log("ERROR parsing JSON preset: " + std::string(e.what()));
            return;
        }

        // Create nodes with proper display/view setup
        std::vector<int> node_ids;
        for (const auto& node_data : nodes) {
            int node_id = -1;

            if (node_data.type == "INPUT_COLORSPACE") {
                node_id = node_manager->CreateInputColorSpaceNode(node_data.data, node_data.position);
                Debug::Log("Created Input ColorSpace node: " + node_data.data);
            }
            else if (node_data.type == "OUTPUT_DISPLAY") {
                // Create display node with proper display AND view setup
                node_id = node_manager->CreateOutputDisplayNode(node_data.display, node_data.position);

                // CRITICAL: Set both display and view with aliases for config portability
                auto* display_node = dynamic_cast<qcview::OutputDisplayNode*>(node_manager->GetNode(node_id));
                if (display_node) {
                    if (!node_data.display_alias.empty()) {
                        display_node->SetDisplayWithAlias(node_data.display, node_data.display_alias);
                    } else {
                        // Playback controller already exists - reload dummy video and re-enable timeline mode
                        display_node->SetDisplay(node_data.display);
                    }

                    if (!node_data.view_alias.empty()) {
                        display_node->SetViewWithAlias(node_data.view, node_data.view_alias);
                    } else {
                        // Playback controller already exists - reload dummy video and re-enable timeline mode
                        display_node->SetView(node_data.view);
                    }

                    Debug::Log("Created Output Display node: " + node_data.display + " - " + node_data.view +
                              (node_data.display_alias.empty() ? "" : " (alias: " + node_data.display_alias + ")"));
                }
            }
            else if (node_data.type == "LOOK") {
                node_id = node_manager->CreateLookNode(node_data.data, node_data.position);
                Debug::Log("Created Look node: " + node_data.data);
            }
            else if (node_data.type == "SCENE_LUT") {
                node_id = node_manager->CreateSceneLUTNode(node_data.data, node_data.position);
                Debug::Log("Created Scene LUT node: " + node_data.data);
            }
            else if (node_data.type == "DISPLAY_LUT") {
                node_id = node_manager->CreateDisplayLUTNode(node_data.data, node_data.position);
                Debug::Log("Created Display LUT node: " + node_data.data);
            }

            node_ids.push_back(node_id);
        }

        // Create connections using the new method
        for (const auto& conn_data : connections) {
            if (conn_data.from_node < (int)node_ids.size() && conn_data.to_node < (int)node_ids.size()) {
                int from_node_id = node_ids[conn_data.from_node];
                int to_node_id = node_ids[conn_data.to_node];

                // Use the new CreateConnection method
                node_manager->CreateConnection(from_node_id, conn_data.from_pin, to_node_id, conn_data.to_pin);
            }
        }

        // Auto-generate the pipeline
        Debug::Log("Auto-generating OCIO pipeline...");
        GenerateOCIOPipeline();

        Debug::Log("=== Preset Applied Successfully ===");
    }


    void Application::ApplyAliasPreset(const std::string& input_alias, const std::string& display_alias, const std::string& view_name) {
        Debug::Log("=== ApplyAliasPreset ENTRY ===");
        Debug::Log("Input params: " + input_alias + ", " + display_alias + ", " + view_name);

        Debug::Log("Checking ocio_manager pointer...");
        if (!ocio_manager) {
            Debug::Log("ERROR: ocio_manager is nullptr!");
            return;
        }

        Debug::Log("Checking if config is loaded...");
        if (!ocio_manager->IsConfigLoaded()) {
            Debug::Log("ERROR: No OCIO config loaded");
            return;
        }

        Debug::Log("OCIO manager and config OK, switching to ACES config...");

        // Switch to ACES config for alias-based presets
        if (!ocio_manager->SwitchToConfig(OCIOConfigType::ACES_13)) {
            Debug::Log("ERROR: Failed to switch to ACES 1.3 config");
            return;
        }
        Debug::Log("Successfully switched to ACES 1.3 config");

        // Store the active config in node manager so transcode knows which config to use
        if (node_manager) {
            node_manager->SetActiveOCIOConfig(OCIOConfigType::ACES_13);
            Debug::Log("Set node manager config type: ACES_1.3");
        }

        // Resolve aliases to full names
        Debug::Log("Resolving input alias: " + input_alias);
        std::string input_colorspace = ocio_manager->ResolveAlias(input_alias);
        Debug::Log("Input resolved to: " + input_colorspace);

        Debug::Log("Resolving display alias: " + display_alias);
        std::string display_name = ocio_manager->ResolveAlias(display_alias);
        Debug::Log("Display resolved to: " + display_name);

        Debug::Log("Resolved aliases:");
        Debug::Log("  Input: " + input_alias + " -> " + input_colorspace);
        Debug::Log("  Display: " + display_alias + " -> " + display_name);
        Debug::Log("  View: " + view_name);

        // Clear existing nodes first
        if (node_manager) {
            Debug::Log("Clearing existing nodes...");
            auto all_nodes = node_manager->GetAllNodes();
            for (auto* node : all_nodes) {
                if (node) {
                    node_manager->DeleteNode(node->GetId());
                }
            }
        }

        // Create nodes with resolved names
        std::vector<int> node_ids;

        // Create input colorspace node
        int input_node_id = node_manager->CreateInputColorSpaceNode(input_colorspace, ImVec2(S(100), S(100)));
        if (input_node_id == -1) {
            Debug::Log("ERROR: Failed to create input colorspace node for: " + input_colorspace);
            return;
        }
        node_ids.push_back(input_node_id);
        Debug::Log("Created Input ColorSpace node: " + input_colorspace);

        // Create display node with proper display AND view setup
        int display_node_id = node_manager->CreateOutputDisplayNode(display_name, ImVec2(S(400), S(100)));
        if (display_node_id == -1) {
            Debug::Log("ERROR: Failed to create display node for: " + display_name);
            return;
        }
        node_ids.push_back(display_node_id);

        // CRITICAL: Set both display and view properly
        auto* base_node = node_manager->GetNode(display_node_id);
        if (!base_node) {
            Debug::Log("ERROR: Failed to get display node with ID " + std::to_string(display_node_id));
            return;
        }

        auto* display_node = dynamic_cast<qcview::OutputDisplayNode*>(base_node);
        if (!display_node) {
            Debug::Log("ERROR: Failed to cast to OutputDisplayNode");
            return;
        }

        Debug::Log("Setting display: " + display_name + " (alias: " + display_alias + ") and view: " + view_name);
        // Store both full names and aliases for config portability
        display_node->SetDisplayWithAlias(display_name, display_alias);
        display_node->SetViewWithAlias(view_name, view_name);  // Views don't typically have separate aliases
        Debug::Log("Created Output Display node: " + display_name + " - " + view_name + " with aliases");

        // Connect the nodes
        if (node_ids.size() >= 2) {
            node_manager->CreateConnection(node_ids[0], 0, node_ids[1], 0);
            Debug::Log("Connected nodes");
        }

        // Auto-generate the pipeline
        Debug::Log("Auto-generating OCIO pipeline...");
        GenerateOCIOPipeline();

        Debug::Log("=== Alias Preset Applied Successfully ===");
    }
