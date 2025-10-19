#include "node_base.h"
#include "../ui/node_editor_theme.h"
#include "../utils/debug_utils.h" 
#include <imgui.h>

#define ICON_ARROW_FORWARD u8"\uE5C8"  
#define ICON_ARROW_BACK    u8"\uE5C4"  

extern ImFont* font_icons;

namespace ump {

    int NodeBase::next_pin_id = 1000;

    NodeBase::NodeBase(int node_id, NodeType node_type, const std::string& node_title)
        : id(node_id), type(node_type), title(node_title) {
    }

    void NodeBase::AddInputPin(const std::string& name, ImU32 color) {
        input_pins.emplace_back(next_pin_id++, PinType::INPUT, name, color);
    }

    void NodeBase::AddOutputPin(const std::string& name, ImU32 color) {
        output_pins.emplace_back(next_pin_id++, PinType::OUTPUT, name, color);
    }

    // InputColorSpaceNode Implementation
    InputColorSpaceNode::InputColorSpaceNode(int node_id, const std::string& colorspace_name)
        : NodeBase(node_id, NodeType::INPUT_COLORSPACE, "Input: " + colorspace_name)
        , colorspace(colorspace_name) {

        // Input nodes only have output pins (they're sources)
        AddOutputPin("Color", NodeEditorTheme::Colors::INPUT_NODE_TITLE);
    }

    void InputColorSpaceNode::Render() {
        ImNodes::BeginNode(id);

        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(title.c_str());
        ImNodes::EndNodeTitleBar();

        ImGui::Dummy(ImVec2(80.0f, 0.0f));  // Set minimum node width

        // Output pin - no label needed
        for (const auto& pin : output_pins) {
            ImNodes::BeginOutputAttribute(pin.id);
            ImNodes::EndOutputAttribute();
        }

        ImNodes::EndNode();
    }

    void InputColorSpaceNode::UpdateProperties() {
        ImGui::Text("Input ColorSpace Node");
        ImGui::Separator();
        ImGui::Text("ColorSpace: %s", colorspace.c_str());
        ImGui::Text("Type: Input Source");
    }

    ImU32 InputColorSpaceNode::GetTitleColor() const {
        return NodeEditorTheme::Colors::INPUT_NODE_TITLE;
    }

    ImU32 InputColorSpaceNode::GetTitleColorSelected() const {
        return NodeEditorTheme::Colors::INPUT_NODE_TITLE_SELECTED;
    }

    // LookNode Implementation
    LookNode::LookNode(int node_id, const std::string& look_name)
        : NodeBase(node_id, NodeType::LOOK, "Look: " + look_name)
        , look_name(look_name) {

        // Look nodes have both input and output
        AddInputPin("Color", NodeEditorTheme::Colors::LOOK_NODE_TITLE);
        AddOutputPin("Color", NodeEditorTheme::Colors::LOOK_NODE_TITLE);
    }

    void LookNode::Render() {
        ImNodes::BeginNode(id);

        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(title.c_str());
        ImNodes::EndNodeTitleBar();

        ImGui::Dummy(ImVec2(80.0f, 0.0f));  // Set minimum node width

        // Input pin - no label needed
        for (const auto& pin : input_pins) {
            ImNodes::BeginInputAttribute(pin.id);
            ImNodes::EndInputAttribute();
        }

        // Output pin - no label needed
        for (const auto& pin : output_pins) {
            ImNodes::BeginOutputAttribute(pin.id);
            ImNodes::EndOutputAttribute();
        }

        ImNodes::EndNode();
    }

    void LookNode::UpdateProperties() {
        ImGui::Text("Look Node");
        ImGui::Separator();
        ImGui::Text("Look: %s", look_name.c_str());
        ImGui::Text("Type: Creative Look/LUT");
    }

    ImU32 LookNode::GetTitleColor() const {
        return NodeEditorTheme::Colors::LOOK_NODE_TITLE;
    }

    ImU32 LookNode::GetTitleColorSelected() const {
        return NodeEditorTheme::Colors::LOOK_NODE_TITLE_SELECTED;
    }

    // OutputDisplayNode Implementation
    OutputDisplayNode::OutputDisplayNode(int node_id, const std::string& display_name)
        : NodeBase(node_id, NodeType::OUTPUT_DISPLAY, "Output: " + display_name)
        , display_name(display_name)
        , view_name("Standard") {  // Default to "Standard" not "sRGB"

        // For now, just use the display name as-is
        // Don't try to parse view from it
        this->display_name = display_name;

        // Set initial title
        UpdateTitle();

        // Output nodes have input pin (receives scene-referred data)
        AddInputPin("Color", NodeEditorTheme::Colors::OUTPUT_NODE_TITLE);
        // And output pin (outputs display-referred data for display LUTs)
        AddOutputPin("Color", NodeEditorTheme::Colors::OUTPUT_NODE_TITLE);
    }

    void OutputDisplayNode::UpdateTitle() {
        if (!view_name.empty() && view_name != "Standard") {
            title = "Output: " + display_name + " - " + view_name;
        }
        else {
            title = "Output: " + display_name;
        }
    }

    void OutputDisplayNode::Render() {
        ImNodes::BeginNode(id);

        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(title.c_str());
        ImNodes::EndNodeTitleBar();

        ImGui::Dummy(ImVec2(80.0f, 0.0f));  // Set minimum node width

        // Input pin - no label needed
        for (const auto& pin : input_pins) {
            ImNodes::BeginInputAttribute(pin.id);
            ImNodes::EndInputAttribute();
        }

        // Output pin - no label needed (for display LUTs)
        for (const auto& pin : output_pins) {
            ImNodes::BeginOutputAttribute(pin.id);
            ImNodes::EndOutputAttribute();
        }

        ImNodes::EndNode();
    }

    void OutputDisplayNode::UpdateProperties() {
        // This will be replaced by the interactive version in main.cpp
        ImGui::Text("Output Display Node");
        ImGui::Separator();
        ImGui::Text("Display: %s", display_name.c_str());
        ImGui::Text("View: %s", view_name.c_str());
        ImGui::Text("Type: Display Transform");
    }

    ImU32 OutputDisplayNode::GetTitleColor() const {
        return NodeEditorTheme::Colors::OUTPUT_NODE_TITLE;
    }

    ImU32 OutputDisplayNode::GetTitleColorSelected() const {
        return NodeEditorTheme::Colors::OUTPUT_NODE_TITLE_SELECTED;
    }

    // SceneLUTNode Implementation (scene-referred, applied before display transform)
    SceneLUTNode::SceneLUTNode(int node_id, const std::string& lut_path)
        : NodeBase(node_id, NodeType::SCENE_LUT, "Scene LUT")
        , lut_file_path(lut_path) {

        // Handle empty path case
        if (lut_path.empty()) {
            lut_filename = "";
            title = "Scene LUT: (not set)";
        } else {
            // Extract filename from path
            size_t last_slash = lut_path.find_last_of("/\\");
            lut_filename = (last_slash != std::string::npos)
                ? lut_path.substr(last_slash + 1) : lut_path;

            // Update title with filename
            title = "Scene LUT: " + lut_filename;
        }

        // Scene LUT nodes have both input and output (like Look nodes)
        AddInputPin("Color", IM_COL32(160, 100, 200, 255));   // Purple
        AddOutputPin("Color", IM_COL32(160, 100, 200, 255));  // Purple
    }

    void SceneLUTNode::Render() {
        ImNodes::BeginNode(id);

        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(title.c_str());
        ImNodes::EndNodeTitleBar();

        ImGui::Dummy(ImVec2(120.0f, 0.0f));  // Set minimum node width (wider for file names)

        // Input pin - no label needed
        for (const auto& pin : input_pins) {
            ImNodes::BeginInputAttribute(pin.id);
            ImNodes::EndInputAttribute();
        }

        // Output pin - no label needed
        for (const auto& pin : output_pins) {
            ImNodes::BeginOutputAttribute(pin.id);
            ImNodes::EndOutputAttribute();
        }

        ImNodes::EndNode();
    }

    void SceneLUTNode::UpdateProperties() {
        ImGui::Text("Scene-Referred LUT Node");
        ImGui::Separator();
        ImGui::Text("File: %s", lut_filename.c_str());
        ImGui::TextWrapped("Path: %s", lut_file_path.c_str());
        ImGui::Text("Type: Scene-Referred File Transform");
    }

    void SceneLUTNode::SetLUTPath(const std::string& path) {
        lut_file_path = path;

        // Handle empty path case
        if (path.empty()) {
            lut_filename = "";
            title = "Scene LUT: (not set)";
        } else {
            // Extract filename from new path
            size_t last_slash = path.find_last_of("/\\");
            lut_filename = (last_slash != std::string::npos)
                ? path.substr(last_slash + 1) : path;

            // Update title
            title = "Scene LUT: " + lut_filename;
        }
    }

    ImU32 SceneLUTNode::GetTitleColor() const {
        return IM_COL32(160, 100, 200, 255);  // Purple
    }

    ImU32 SceneLUTNode::GetTitleColorSelected() const {
        return IM_COL32(180, 120, 220, 255);  // Lighter purple
    }

    // DisplayLUTNode Implementation (display-referred, applied after display transform)
    DisplayLUTNode::DisplayLUTNode(int node_id, const std::string& lut_path)
        : NodeBase(node_id, NodeType::DISPLAY_LUT, "Display LUT")
        , lut_file_path(lut_path) {

        // Handle empty path case
        if (lut_path.empty()) {
            lut_filename = "";
            title = "Display LUT: (not set)";
        } else {
            // Extract filename from path
            size_t last_slash = lut_path.find_last_of("/\\");
            lut_filename = (last_slash != std::string::npos)
                ? lut_path.substr(last_slash + 1) : lut_path;

            // Update title with filename
            title = "Display LUT: " + lut_filename;
        }

        // Display LUT nodes only have input pin (terminal node, no passthrough)
        AddInputPin("Color", IM_COL32(200, 120, 80, 255));   // Orange
    }

    void DisplayLUTNode::Render() {
        ImNodes::BeginNode(id);

        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(title.c_str());
        ImNodes::EndNodeTitleBar();

        ImGui::Dummy(ImVec2(120.0f, 0.0f));  // Set minimum node width (wider for file names)

        // Input pin only - no label needed
        for (const auto& pin : input_pins) {
            ImNodes::BeginInputAttribute(pin.id);
            ImNodes::EndInputAttribute();
        }

        ImNodes::EndNode();
    }

    void DisplayLUTNode::UpdateProperties() {
        ImGui::Text("Display-Referred LUT Node");
        ImGui::Separator();
        ImGui::Text("File: %s", lut_filename.c_str());
        ImGui::TextWrapped("Path: %s", lut_file_path.c_str());
        ImGui::Text("Type: Display-Referred File Transform");
    }

    void DisplayLUTNode::SetLUTPath(const std::string& path) {
        lut_file_path = path;

        // Handle empty path case
        if (path.empty()) {
            lut_filename = "";
            title = "Display LUT: (not set)";
        } else {
            // Extract filename from new path
            size_t last_slash = path.find_last_of("/\\");
            lut_filename = (last_slash != std::string::npos)
                ? path.substr(last_slash + 1) : path;

            // Update title
            title = "Display LUT: " + lut_filename;
        }
    }

    ImU32 DisplayLUTNode::GetTitleColor() const {
        return IM_COL32(200, 120, 80, 255);  // Orange
    }

    ImU32 DisplayLUTNode::GetTitleColorSelected() const {
        return IM_COL32(220, 140, 100, 255);  // Lighter orange
    }

}