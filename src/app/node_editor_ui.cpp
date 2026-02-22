// ============================================================================
// Node editor UI (node graph, properties panels, pipeline generation)
// ============================================================================

#include "app/application.h"
#include "app/app_icons.h"
#include "app/app_ui_macros.h"
#include "project/project_manager.h"
#include "nodes/node_manager.h"
#include "player/video_player.h"
#include "ui/node_editor_theme.h"
#include "color/ocio_config_manager.h"
#include "color/ocio_pipeline.h"
#include "transcode/ocio_lut_baker.h"
#include <imgui.h>
#include <imnodes.h>
#include <nfd.h>
#include <filesystem>
#include <atomic>
#include <thread>

// Globals defined in main.cpp
extern ImFont* font_regular;
extern ImFont* font_bold;
extern ImFont* font_icons;
extern std::unique_ptr<OCIOConfigManager> ocio_manager;

// LUT export globals
extern bool show_lut_export_popup;
extern std::string lut_export_path;
extern std::atomic<float> lut_export_progress;
extern std::atomic<bool> lut_export_cancel;
extern std::atomic<bool> lut_export_complete;
extern std::atomic<bool> lut_export_error;
extern std::string lut_export_error_message;

    void Application::CreateNodeEditorContent() {

        struct DragPayload {
            qcview::NodeType type;
            char name[256];
        };

        ImGui::PushStyleColor(ImGuiCol_Text, UI_GRAY_VEC4);
        if (font_icons) {
            ImGui::PushFont(font_icons);
            ImGui::Text(ICON_FLOWCHART);
            ImGui::PopFont();
            ImGui::SameLine();
        }
        if (font_bold) ImGui::PushFont(font_bold);
        ImGui::Text("Color Nodes");
        if (font_bold) ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::Separator();

        ImNodes::BeginNodeEditor();


        // Check if editor is hovered and we're releasing a drag
        if (ImNodes::IsEditorHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            // Check if there's an active drag payload
            const ImGuiPayload* payload = ImGui::GetDragDropPayload();
            if (payload && payload->IsDataType("OCIO_NODE")) {
                const DragPayload* data = (const DragPayload*)payload->Data;

                // Create node at current mouse position
                if (node_manager) {
                    node_manager->SetPendingNodeCreation(data->type, data->name);
                    Debug::Log("Dropped in editor: " + std::string(data->name));
                }
            }
        }

        if (node_manager) {
            node_manager->HandlePendingNodeCreation();
            node_manager->RenderAllNodes();
        }

        ImNodes::EndNodeEditor();

        if (node_manager) {
            node_manager->HandleConnections();
            node_manager->UpdateSelection();
        }
    }

    // In main.cpp, replace the existing CreateNodePropertiesContent() function:

    void Application::CreateNodePropertiesContent() {
        // Header section (non-scrollable)
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15.0f, 12.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));

        ImGui::PushStyleColor(ImGuiCol_Text, UI_GRAY_VEC4);
        if (font_icons) {
            ImGui::PushFont(font_icons);
            ImGui::Text(ICON_DISPLAY_SETTINGS);
            ImGui::PopFont();
            ImGui::SameLine();
        }
        if (font_bold) ImGui::PushFont(font_bold);
        ImGui::Text("Color Parameters");
        if (font_bold) ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::Separator();

        // Get selected nodes early - needed for both content and footer
        int num_selected = ImNodes::NumSelectedNodes();
        bool output_node_selected = false;
        if (num_selected == 1) {
            int selected_id;
            ImNodes::GetSelectedNodes(&selected_id);
            auto* node = node_manager->GetNodeById(selected_id);
            output_node_selected = (node && node->GetType() == qcview::NodeType::OUTPUT_DISPLAY);
        }

        // Reserve space for sticky footer (scale with font, 0.65 dampened)
        const float ui_scale = ImGui::GetIO().FontGlobalScale;
        const float height_scale = 1.0f + (ui_scale - 1.0f) * 0.65f;
        float footer_reserve = 96.0f * height_scale;  // Space for shader toggle + export LUT buttons

        float available_height = ImGui::GetContentRegionAvail().y;

        // Main scrollable content area
        if (ImGui::BeginChild("ScrollableContent", ImVec2(0, available_height - footer_reserve), false)) {
            if (num_selected == 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                ImGui::Text("No node selected");
                ImGui::Spacing();
                ImGui::TextWrapped("Select a node in the editor to view and edit its properties.");
                ImGui::PopStyleColor();
            }
            else if (num_selected > 1) {
                ImGui::TextColored(MutedLight(GetWindowsAccentColor()),
                    "Multiple nodes selected (%d)", num_selected);
                ImGui::TextWrapped("Select a single node to edit properties.");
            }
            else {
                // Single node selected
                int selected_id;
                ImNodes::GetSelectedNodes(&selected_id);

                auto* node = node_manager->GetNodeById(selected_id);
                if (node) {
                    RenderNodeSpecificProperties(node);
                }
            }
        }
        ImGui::EndChild();

        // Sticky footer - always visible, disabled when output node not selected
        ImGui::Separator();

        float footer_width = ImGui::GetContentRegionAvail().x - 10.0f;  // 10px right margin
        if (ImGui::BeginChild("ShaderFooter", ImVec2(footer_width, 0), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            // Check if pipeline is ready (only meaningful when output node selected)
            bool pipeline_ready = output_node_selected && CheckPipelineReadiness();
            bool shader_active = video_player && video_player->HasActiveColorTransform();

            // Disable entire footer if output node not selected
            if (!output_node_selected) {
                ImGui::BeginDisabled();
            }

            // Apply and Remove buttons side by side
            float button_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

            // Apply button - disabled if pipeline not ready
            if (!pipeline_ready && output_node_selected) {
                ImGui::BeginDisabled();
            }

            // Accent color when pipeline ready AND no shader currently active (prompts user to apply)
            if (pipeline_ready && !shader_active) {
                ImGui::PushStyleColor(ImGuiCol_Button, MutedDark(GetWindowsAccentColor()));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GetWindowsAccentColor());
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, Bright(GetWindowsAccentColor()));
            } else {
                PushOutlineButtonStyle();
            }

            if (ImGui::Button("Apply", ImVec2(button_width, 0))) {
                GenerateOCIOPipeline();
            }

            if (pipeline_ready && !shader_active) {
                ImGui::PopStyleColor(3);
            } else {
                PopOutlineButtonStyle();
            }

            if (!pipeline_ready && output_node_selected) {
                ImGui::EndDisabled();
            }

            ImGui::SameLine();

            // Remove button - accent color when shader IS active (prompts user to remove)
            if (shader_active) {
                ImGui::PushStyleColor(ImGuiCol_Button, MutedDark(GetWindowsAccentColor()));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GetWindowsAccentColor());
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, Bright(GetWindowsAccentColor()));
            } else {
                PushOutlineButtonStyle();
            }

            if (ImGui::Button("Remove", ImVec2(button_width, 0))) {
                Debug::Log("User requested OCIO pipeline removal");
                video_player->ClearColorPipeline();
            }

            if (shader_active) {
                ImGui::PopStyleColor(3);
            } else {
                PopOutlineButtonStyle();
            }

            // Export LUT button - only enabled when shader is active
            ImGui::Spacing();
            if (!shader_active) {
                ImGui::BeginDisabled();
            }

            PushOutlineButtonStyle();
            if (ImGui::Button("Export LUT", ImVec2(-1, 0))) {
                // Open save dialog for .cube file
                nfdu8char_t* out_path = nullptr;
                nfdfilteritem_t filter[1] = { { "3D LUT", "cube" } };
                nfdresult_t result = NFD_SaveDialogU8(&out_path, filter, 1, nullptr, "color_transform.cube");

                if (result == NFD_OKAY) {
                    lut_export_path = out_path;
                    NFD_FreePathU8(out_path);

                    // Ensure .cube extension
                    if (lut_export_path.find(".cube") == std::string::npos) {
                        lut_export_path += ".cube";
                    }

                    // Reset export state
                    lut_export_progress = 0.0f;
                    lut_export_cancel = false;
                    lut_export_complete = false;
                    lut_export_error = false;
                    lut_export_error_message.clear();

                    // Extract OCIO settings from node graph
                    auto pipeline_nodes = node_manager->GetPipelineOrder();
                    std::string src_colorspace, display, view;
                    std::vector<std::string> look_chain, scene_luts, display_luts;

                    for (auto* node : pipeline_nodes) {
                        switch (node->GetType()) {
                            case qcview::NodeType::INPUT_COLORSPACE: {
                                auto* csNode = dynamic_cast<qcview::InputColorSpaceNode*>(node);
                                if (csNode) src_colorspace = csNode->GetColorSpace();
                                break;
                            }
                            case qcview::NodeType::LOOK: {
                                auto* lookNode = dynamic_cast<qcview::LookNode*>(node);
                                if (lookNode && !lookNode->GetLook().empty())
                                    look_chain.push_back(lookNode->GetLook());
                                break;
                            }
                            case qcview::NodeType::SCENE_LUT: {
                                auto* lutNode = dynamic_cast<qcview::SceneLUTNode*>(node);
                                if (lutNode && !lutNode->GetLUTPath().empty())
                                    scene_luts.push_back(lutNode->GetLUTPath());
                                break;
                            }
                            case qcview::NodeType::DISPLAY_LUT: {
                                auto* lutNode = dynamic_cast<qcview::DisplayLUTNode*>(node);
                                if (lutNode && !lutNode->GetLUTPath().empty())
                                    display_luts.push_back(lutNode->GetLUTPath());
                                break;
                            }
                            case qcview::NodeType::OUTPUT_DISPLAY: {
                                auto* displayNode = dynamic_cast<qcview::OutputDisplayNode*>(node);
                                if (displayNode) {
                                    display = displayNode->GetDisplay();
                                    view = displayNode->GetView();
                                }
                                break;
                            }
                        }
                    }

                    // Build looks string
                    std::string looks;
                    for (size_t i = 0; i < look_chain.size(); ++i) {
                        if (i > 0) looks += ", ";
                        looks += look_chain[i];
                    }

                    // Start export in background thread
                    std::string export_path = lut_export_path;
                    std::thread([=]() {
                        qcview::OCIOLutBaker::BakeConfig config;
                        config.src_colorspace = src_colorspace;
                        config.display = display;
                        config.view = view;
                        config.looks = looks;
                        config.scene_luts = scene_luts;
                        config.display_luts = display_luts;
                        config.cube_size = 65;

                        // Write directly to user-specified path (not cache)
                        std::string result = qcview::OCIOLutBaker::BakeTo(
                            config, export_path,
                            [](float p) { lut_export_progress = p; },
                            &lut_export_cancel
                        );

                        if (result.empty()) {
                            lut_export_error = true;
                            lut_export_error_message = "Failed to generate LUT";
                        } else {
                            lut_export_complete = true;
                        }
                    }).detach();

                    show_lut_export_popup = true;
                    Debug::Log("Starting LUT export to: " + lut_export_path);
                } else if (result == NFD_ERROR) {
                    Debug::Log("NFD Error: " + std::string(NFD_GetError()));
                }
            }
            PopOutlineButtonStyle();

            if (!shader_active) {
                ImGui::EndDisabled();
            }

            if (!output_node_selected) {
                ImGui::EndDisabled();
            }
            else if (!pipeline_ready && !shader_active) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, MutedLight(GetWindowsAccentColor()));
                ImGui::TextWrapped("Connect Input to Output");
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndChild();

        ImGui::PopStyleVar(2);
    }

    void Application::RenderNodeSpecificProperties(qcview::NodeBase* node) {
        if (!node) return;

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));

        // Node type header
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "%s", node->GetTitle().c_str());
        ImGui::Separator();
        ImGui::Spacing();

        switch (node->GetType()) {
        case qcview::NodeType::OUTPUT_DISPLAY:
            RenderOutputDisplayProperties(node);
            break;

        case qcview::NodeType::INPUT_COLORSPACE:
            RenderInputColorSpaceProperties(node);
            break;

        case qcview::NodeType::LOOK:
            RenderLookProperties(node);
            break;

        case qcview::NodeType::SCENE_LUT:
            RenderSceneLUTProperties(node);
            break;

        case qcview::NodeType::DISPLAY_LUT:
            RenderDisplayLUTProperties(node);
            break;

        default:
            ImGui::TextDisabled("No editable properties");
            break;
        }

        ImGui::PopStyleVar();
    }

    void Application::RenderOutputDisplayProperties(qcview::NodeBase* node) {
        auto* display_node = dynamic_cast<qcview::OutputDisplayNode*>(node);
        if (!display_node) return;

        if (font_bold) ImGui::PushFont(font_bold);
        ImGui::Text("Display Settings");
        if (font_bold) ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        std::string current_display = display_node->GetDisplay();
        std::string current_view = display_node->GetView();

        // Show current display
        ImGui::Text("Display:");
        ImGui::SameLine();
        ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s", current_display.c_str());

        // View selection with radio buttons
        ImGui::Spacing();
        ImGui::Text("Select View:");
        ImGui::Separator();

        // Get available views from OCIO config
        if (ocio_manager && ocio_manager->IsConfigLoaded()) {
            // Find connected input colorspace for ACES 2.0 viewing rules
            std::string src_colorspace;
            if (node_manager) {
                // Trace back through connections to find INPUT_COLORSPACE node
                auto connections = node_manager->GetConnections();
                for (const auto& conn : connections) {
                    // Check if this connection feeds into our display node
                    for (const auto& pin : display_node->GetInputPins()) {
                        if (pin.id == conn.to_pin) {
                            // Found connection to this display node, find the source node
                            auto all_nodes = node_manager->GetAllNodes();
                            for (auto* source_node : all_nodes) {
                                if (!source_node) continue;

                                // Check if source node has the from_pin
                                for (const auto& out_pin : source_node->GetOutputPins()) {
                                    if (out_pin.id == conn.from_pin) {
                                        // Found the source node, check if it's an input colorspace
                                        if (source_node->GetType() == qcview::NodeType::INPUT_COLORSPACE) {
                                            auto* cs_node = dynamic_cast<qcview::InputColorSpaceNode*>(source_node);
                                            if (cs_node) {
                                                src_colorspace = cs_node->GetColorSpace();
                                            }
                                        }
                                        break;
                                    }
                                }
                                if (!src_colorspace.empty()) break;
                            }
                            break;
                        }
                    }
                    if (!src_colorspace.empty()) break;
                }
            }

            // Get views - use encoding-aware method if we have source colorspace (ACES 2.0)
            std::vector<std::string> available_views;
            if (!src_colorspace.empty()) {
                // ACES 2.0 viewing rules: filter by source encoding
                available_views = ocio_manager->GetViewsForSource(current_display, src_colorspace);
            } else {
                // Fallback: get all views (for standalone OUTPUT node or configs without viewing rules)
                available_views = ocio_manager->GetViews(current_display);
            }

            if (!available_views.empty()) {
                ImGui::Indent();

                for (const auto& view_name : available_views) {
                    bool is_selected = (view_name == current_view);

                    if (ImGui::RadioButton(view_name.c_str(), is_selected)) {
                        display_node->SetView(view_name);
                        Debug::Log("View changed to: " + view_name);
                    }
                }

                ImGui::Unindent();
            }
            else {
                if (!src_colorspace.empty()) {
                    ImGui::TextColored(MutedLight(GetWindowsAccentColor()),
                        "No compatible views for source '%s'", src_colorspace.c_str());
                } else {
                    ImGui::TextColored(MutedLight(GetWindowsAccentColor()),
                        "No views found (connect an input colorspace)");
                }
            }
        }
        else {
            ImGui::TextColored(MutedLight(GetWindowsAccentColor()), "No OCIO config loaded");
        }

        // Show current selection
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(Bright(GetWindowsAccentColor()),
            "Current: %s -> %s", current_display.c_str(), current_view.c_str());
    }

    void Application::RenderInputColorSpaceProperties(qcview::NodeBase* node) {
        auto* cs_node = dynamic_cast<qcview::InputColorSpaceNode*>(node);
        if (!cs_node) return;

        if (font_bold) ImGui::PushFont(font_bold);
        ImGui::Text("Input ColorSpace");
        if (font_bold) ImGui::PopFont();
        ImGui::Spacing();

        std::string current_cs = cs_node->GetColorSpace();

        // Optional: Allow changing colorspace
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.07f, 0.07f, 1.00f));
        if (ImGui::BeginCombo("ColorSpace", current_cs.c_str())) {
            if (ocio_manager) {
                auto colorspaces = ocio_manager->GetInputColorSpaces();
                for (const auto& cs : colorspaces) {
                    bool is_selected = (cs == current_cs);
                    if (ImGui::Selectable(cs.c_str(), is_selected)) {
                        cs_node->SetColorSpace(cs);
                    }
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleColor();

        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            "Type: Input Source");
    }

    void Application::RenderLookProperties(qcview::NodeBase* node) {
        auto* look_node = dynamic_cast<qcview::LookNode*>(node);
        if (!look_node) return;

        if (font_bold) ImGui::PushFont(font_bold);
        ImGui::Text("Look Settings");
        if (font_bold) ImGui::PopFont();
        ImGui::Spacing();

        std::string current_look = look_node->GetLook();
        ImGui::Text("Look: %s", current_look.c_str());

        // Future: Add intensity slider, bypass checkbox, etc.
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            "Type: Creative Look/LUT");
    }

    void Application::RenderSceneLUTProperties(qcview::NodeBase* node) {
        auto* lut_node = dynamic_cast<qcview::SceneLUTNode*>(node);
        if (!lut_node) return;

        if (font_bold) ImGui::PushFont(font_bold);
        ImGui::Text("Scene-Referred LUT Settings");
        if (font_bold) ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        std::string current_path = lut_node->GetLUTPath();
        std::string current_filename = lut_node->GetLUTFileName();

        // Check if LUT is set
        bool is_lut_set = !current_path.empty();

        if (is_lut_set) {
            // Display current file
            ImGui::Text("File:");
            ImGui::SameLine();
            if (font_regular) ImGui::PushFont(font_regular);
            ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s", current_filename.c_str());
            if (font_regular) ImGui::PopFont();

            ImGui::Spacing();

            // Show full path (small text)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
            ImGui::TextWrapped("%s", current_path.c_str());
            ImGui::PopStyleColor();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        } else {
            // No LUT set yet
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
            ImGui::TextWrapped("No LUT file selected. Click the button below to select a LUT file.");
            ImGui::PopStyleColor();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }

        // Select/Change LUT button
        const char* button_label = is_lut_set ? "Change LUT File" : "Select LUT File";
        PushOutlineButtonStyle();
        if (ImGui::Button(button_label, ImVec2(-1, 0))) {
            nfdchar_t* out_path = nullptr;
            nfdfilteritem_t filters[2] = {
                { "LUT Files", "cube,3dl,spi1d,spi3d,csp,lut" },
                { "All Files", "*" }
            };

            nfdresult_t result = NFD_OpenDialog(&out_path, filters, 2, nullptr);

            if (result == NFD_OKAY) {
                lut_node->SetLUTPath(std::string(out_path));
                Debug::Log("Set Scene LUT file: " + std::string(out_path));
                NFD_FreePath(out_path);

                // Regenerate pipeline
                GenerateOCIOPipeline();
            } else if (result == NFD_ERROR) {
                Debug::Log("NFD Error: " + std::string(NFD_GetError()));
            }
        }
        PopOutlineButtonStyle();

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            "Type: Scene-Referred File Transform");
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            "Applied before display transform");
    }

    void Application::RenderDisplayLUTProperties(qcview::NodeBase* node) {
        auto* lut_node = dynamic_cast<qcview::DisplayLUTNode*>(node);
        if (!lut_node) return;

        if (font_bold) ImGui::PushFont(font_bold);
        ImGui::Text("Display-Referred LUT Settings");
        if (font_bold) ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        std::string current_path = lut_node->GetLUTPath();
        std::string current_filename = lut_node->GetLUTFileName();

        // Check if LUT is set
        bool is_lut_set = !current_path.empty();

        if (is_lut_set) {
            // Display current file
            ImGui::Text("File:");
            ImGui::SameLine();
            if (font_regular) ImGui::PushFont(font_regular);
            ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s", current_filename.c_str());
            if (font_regular) ImGui::PopFont();

            ImGui::Spacing();

            // Show full path (small text)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
            ImGui::TextWrapped("%s", current_path.c_str());
            ImGui::PopStyleColor();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        } else {
            // No LUT set yet
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
            ImGui::TextWrapped("No LUT file selected. Click the button below to select a LUT file.");
            ImGui::PopStyleColor();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }

        // Select/Change LUT button
        const char* button_label = is_lut_set ? "Change LUT File" : "Select LUT File";
        PushOutlineButtonStyle();
        if (ImGui::Button(button_label, ImVec2(-1, 0))) {
            nfdchar_t* out_path = nullptr;
            nfdfilteritem_t filters[2] = {
                { "LUT Files", "cube,3dl,spi1d,spi3d,csp,lut" },
                { "All Files", "*" }
            };

            nfdresult_t result = NFD_OpenDialog(&out_path, filters, 2, nullptr);

            if (result == NFD_OKAY) {
                lut_node->SetLUTPath(std::string(out_path));
                Debug::Log("Set Display LUT file: " + std::string(out_path));
                NFD_FreePath(out_path);

                // Regenerate pipeline
                GenerateOCIOPipeline();
            } else if (result == NFD_ERROR) {
                Debug::Log("NFD Error: " + std::string(NFD_GetError()));
            }
        }
        PopOutlineButtonStyle();

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            "Type: Display-Referred File Transform");
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            "Applied after display transform");
    }

    bool Application::CheckPipelineReadiness() {
        auto nodes = node_manager->GetAllNodes();

        bool has_input = false;
        bool has_valid_output = false;

        for (auto* node : nodes) {
            if (node->GetType() == qcview::NodeType::INPUT_COLORSPACE) {
                has_input = true;
            }
            else if (node->GetType() == qcview::NodeType::OUTPUT_DISPLAY) {
                auto* display_node = dynamic_cast<qcview::OutputDisplayNode*>(node);
                // Check that view is set and not empty
                if (display_node && !display_node->GetView().empty()) {
                    has_valid_output = true;
                }
            }
        }

        return has_input && has_valid_output;
    }

    void Application::GenerateOCIOPipeline() {
        Debug::Log("=== Pipeline Generation (Using Connected Chain) ===");

        // Get the connected pipeline in proper order
        auto pipeline_nodes = node_manager->GetPipelineOrder();
        if (pipeline_nodes.empty()) {
            Debug::Log("No connected pipeline found");
            return;
        }

        Debug::Log("Found connected pipeline with " + std::to_string(pipeline_nodes.size()) + " nodes");

        std::string src_colorspace;
        std::string display;
        std::string view;
        std::vector<std::string> look_chain;        // Proper chain of looks, not concatenated string
        std::vector<std::string> scene_lut_files;   // Scene-referred LUT file paths (before display transform)
        std::vector<std::string> display_lut_files; // Display-referred LUT file paths (after display transform)

        // Process nodes in connection order
        for (size_t i = 0; i < pipeline_nodes.size(); ++i) {
            auto* node = pipeline_nodes[i];
            Debug::Log("Processing node " + std::to_string(i) + ": " + std::to_string((int)node->GetType()));

            switch (node->GetType()) {
            case qcview::NodeType::INPUT_COLORSPACE: {
                auto* csNode = dynamic_cast<qcview::InputColorSpaceNode*>(node);
                if (csNode) {
                    src_colorspace = csNode->GetColorSpace();
                    Debug::Log("  Input ColorSpace: " + src_colorspace);
                }
                break;
            }

            case qcview::NodeType::LOOK: {
                auto* lookNode = dynamic_cast<qcview::LookNode*>(node);
                if (lookNode && !lookNode->GetLook().empty()) {
                    look_chain.push_back(lookNode->GetLook());
                    Debug::Log("  Look #" + std::to_string(look_chain.size()) + ": " + lookNode->GetLook());
                }
                break;
            }

            case qcview::NodeType::SCENE_LUT: {
                auto* lutNode = dynamic_cast<qcview::SceneLUTNode*>(node);
                if (lutNode && !lutNode->GetLUTPath().empty()) {
                    scene_lut_files.push_back(lutNode->GetLUTPath());
                    Debug::Log("  Scene LUT: " + lutNode->GetLUTFileName());
                }
                break;
            }

            case qcview::NodeType::DISPLAY_LUT: {
                auto* lutNode = dynamic_cast<qcview::DisplayLUTNode*>(node);
                if (lutNode && !lutNode->GetLUTPath().empty()) {
                    display_lut_files.push_back(lutNode->GetLUTPath());
                    Debug::Log("  Display LUT: " + lutNode->GetLUTFileName());
                }
                break;
            }

            case qcview::NodeType::OUTPUT_DISPLAY: {
                auto* displayNode = dynamic_cast<qcview::OutputDisplayNode*>(node);
                if (displayNode) {
                    display = displayNode->GetDisplay();
                    view = displayNode->GetView();
                    Debug::Log("  Output: " + display + " - " + view);
                }
                break;
            }
            }
        }

        // Convert look chain to comma-separated string for current OCIO implementation
        std::string looks;
        for (size_t i = 0; i < look_chain.size(); ++i) {
            if (i > 0) looks += ", ";
            looks += look_chain[i];
        }

        // Validate and build pipeline
        if (!src_colorspace.empty() && !display.empty() && !view.empty()) {
            Debug::Log("Building OCIO pipeline...");
            Debug::Log("  Source: " + src_colorspace);
            Debug::Log("  Display: " + display);
            Debug::Log("  View: " + view);
            if (!looks.empty()) {
                Debug::Log("  Looks: " + looks);
            }

            auto ocio_pipeline = std::make_unique<OCIOPipeline>();
            if (ocio_pipeline->BuildFromDescription(src_colorspace, display, view, looks, scene_lut_files, display_lut_files)) {
                video_player->SetColorPipeline(std::move(ocio_pipeline));
                Debug::Log("Pipeline generated successfully!");
            }
            else {
                Debug::Log("Pipeline generation failed");
            }
        }
        else {
            Debug::Log("Pipeline incomplete:");
            if (src_colorspace.empty()) Debug::Log("  - Missing input colorspace");
            if (display.empty()) Debug::Log("  - Missing output display");
            if (view.empty()) Debug::Log("  - Missing output view");
        }

    }
