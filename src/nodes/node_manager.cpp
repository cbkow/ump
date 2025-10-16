#include "node_manager.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <set>  

#include "../ui/node_editor_theme.h"
#include "../imnodes/imnodes.h"
#include "../utils/debug_utils.h"

namespace ump {

    NodeManager::NodeManager()
        : next_node_id(1), next_connection_id(1), has_pending_node(false) {
    }

    NodeBase* NodeManager::GetNodeById(int node_id) {
        auto it = nodes.find(node_id);
        return (it != nodes.end()) ? it->second.get() : nullptr;
    }

    std::vector<NodeBase*> NodeManager::GetAllNodes() const {
        std::vector<NodeBase*> result;
        for (const auto& [id, node] : nodes) {
            result.push_back(node.get());
        }
        return result;
    }

    int NodeManager::CreateInputColorSpaceNode(const std::string& colorspace_name, ImVec2 position) {
        int node_id = next_node_id++;
        auto node = std::make_unique<InputColorSpaceNode>(node_id, colorspace_name);

        if (position.x != 0 || position.y != 0) {
            ImNodes::SetNodeGridSpacePos(node_id, position);  // Use GridSpacePos instead of ScreenSpacePos
        }

        nodes[node_id] = std::move(node);
        return node_id;
    }

    int NodeManager::CreateLookNode(const std::string& look_name, ImVec2 position) {

        int node_id = next_node_id++;
        auto node = std::make_unique<LookNode>(node_id, look_name);

        // Set node position if specified - SAME FIX AS INPUT NODES
        if (position.x != 0 || position.y != 0) {
            ImNodes::SetNodeGridSpacePos(node_id, position);  // Make sure this uses GridSpacePos
        }

        nodes[node_id] = std::move(node);
        return node_id;
    }

    int NodeManager::CreateOutputDisplayNode(const std::string& display_name, ImVec2 position) {

        int node_id = next_node_id++;
        auto node = std::make_unique<OutputDisplayNode>(node_id, display_name);

        // Set node position if specified
        if (position.x != 0 || position.y != 0) {
            ImNodes::SetNodeGridSpacePos(node_id, position);  // Use GridSpacePos
        }

        nodes[node_id] = std::move(node);
        return node_id;
    }

    void NodeManager::DeleteNode(int node_id) {
        auto it = nodes.find(node_id);
        if (it != nodes.end()) {
            // Remove connections involving this node
            auto pin_belongs_to_node = [&](int pin_id) {
                const auto& node = it->second;
                for (const auto& input_pin : node->GetInputPins()) {
                    if (input_pin.id == pin_id) return true;
                }
                for (const auto& output_pin : node->GetOutputPins()) {
                    if (output_pin.id == pin_id) return true;
                }
                return false;
                };

            connections.erase(
                std::remove_if(connections.begin(), connections.end(),
                    [&](const NodeConnection& conn) {
                        return pin_belongs_to_node(conn.from_pin) || pin_belongs_to_node(conn.to_pin);
                    }),
                connections.end()
            );

            nodes.erase(it);
        }
    }

    void NodeManager::DeleteSelectedNodes() {
        for (int node_id : selected_nodes) {
            DeleteNode(node_id);
        }
        selected_nodes.clear();
    }

    NodeBase* NodeManager::GetNode(int node_id) {
        auto it = nodes.find(node_id);
        return (it != nodes.end()) ? it->second.get() : nullptr;
    }

    void NodeManager::RenderAllNodes() {
        if (!nodes.empty()) {
        }

        for (const auto& [node_id, node] : nodes) {

            // TEMPORARILY REMOVE COLOR STYLING - just render the node directly
            node->Render();
        }

        // Render connections
        for (const auto& connection : connections) {
            ImNodes::Link(connection.connection_id, connection.from_pin, connection.to_pin);
        }
    }

    void NodeManager::UpdateSelection() {
        // Get selected nodes from ImNodes
        const int num_selected_nodes = ImNodes::NumSelectedNodes();
        if (num_selected_nodes > 0) {
            selected_nodes.resize(num_selected_nodes);
            ImNodes::GetSelectedNodes(selected_nodes.data());
        }
        else {
            selected_nodes.clear();
        }
    }

    void NodeManager::RenderSelectedNodeProperties() {
        if (selected_nodes.empty()) {
            ImGui::Text("No node selected");
            ImGui::TextWrapped("Select a node in the editor to view its properties.");
            return;
        }

        if (selected_nodes.size() > 1) {
            ImGui::Text("Multiple nodes selected (%zu)", selected_nodes.size());
            ImGui::TextWrapped("Select a single node to edit properties.");
            return;
        }

        // Single node selected
        int selected_node_id = selected_nodes[0];
        NodeBase* node = GetNode(selected_node_id);
        if (node && node->HasProperties()) {
            node->UpdateProperties();
        }
        else {
            ImGui::Text("Node has no editable properties");
        }
    }

    bool NodeManager::HasSelectedNode() const {
        return !selected_nodes.empty();
    }

    void NodeManager::HandleConnections() {
        // Handle new connections
        int start_pin, end_pin;
        if (ImNodes::IsLinkCreated(&start_pin, &end_pin)) {
            // Validate connection (basic validation - can be expanded)
            bool valid_connection = true;

            if (valid_connection) {
                NodeConnection new_connection;
                new_connection.from_pin = start_pin;
                new_connection.to_pin = end_pin;
                new_connection.connection_id = next_connection_id++;

                connections.push_back(new_connection);

                // Trigger the callback if set
                if (on_connections_changed) {
                    on_connections_changed();
                }
            }
        }

        // Handle connection deletion
        int deleted_link_id;
        if (ImNodes::IsLinkDestroyed(&deleted_link_id)) {
            auto it = std::find_if(connections.begin(), connections.end(),
                [deleted_link_id](const NodeConnection& conn) {
                    return conn.connection_id == deleted_link_id;
                });

            if (it != connections.end()) {
                connections.erase(it);

                // Trigger the callback if set
                if (on_connections_changed) {
                    on_connections_changed();
                }
            }
        }
    }

    void NodeManager::DeleteConnection(int connection_id) {
        auto it = std::find_if(connections.begin(), connections.end(),
            [connection_id](const NodeConnection& conn) {
                return conn.connection_id == connection_id;
            });

        if (it != connections.end()) {
            connections.erase(it);
        }
    }

    void NodeManager::AddConnectionDirect(const NodeConnection& conn) {
        connections.push_back(conn);

        // Update next_connection_id if needed
        if (conn.connection_id >= next_connection_id) {
            next_connection_id = conn.connection_id + 1;
        }
    }

    void NodeManager::CreateConnection(int from_node_id, int from_pin_index, int to_node_id, int to_pin_index) {
        // Get the nodes
        NodeBase* from_node = GetNode(from_node_id);
        NodeBase* to_node = GetNode(to_node_id);

        if (!from_node || !to_node) {
            Debug::Log("ERROR: Could not find nodes for connection");
            return;
        }

        // Get the pin IDs
        const auto& output_pins = from_node->GetOutputPins();
        const auto& input_pins = to_node->GetInputPins();

        if (from_pin_index >= (int)output_pins.size() || to_pin_index >= (int)input_pins.size()) {
            Debug::Log("ERROR: Invalid pin indices for connection");
            return;
        }

        int from_pin_id = output_pins[from_pin_index].id;
        int to_pin_id = input_pins[to_pin_index].id;

        // Create the connection
        NodeConnection new_connection;
        new_connection.from_pin = from_pin_id;
        new_connection.to_pin = to_pin_id;
        new_connection.connection_id = next_connection_id++;

        connections.push_back(new_connection);

        Debug::Log("Created connection: " + std::to_string(from_node_id) + " -> " + std::to_string(to_node_id));

        // Trigger the callback if set
        if (on_connections_changed) {
            on_connections_changed();
        }
    }

    void NodeManager::SetPendingNodeCreation(NodeType type, const std::string& data) {
        has_pending_node = true;
        pending_node_type = type;
        pending_node_data = data;

        // Use mouse position relative to the current ImGui window content area
        ImVec2 mouse_pos = ImGui::GetMousePos();
        ImVec2 window_pos = ImGui::GetWindowPos();
        ImVec2 content_region_min = ImGui::GetWindowContentRegionMin();

        // Account for window position + content region offset
        pending_node_position = ImVec2(
            mouse_pos.x - window_pos.x - content_region_min.x,
            mouse_pos.y - window_pos.y - content_region_min.y
        );
    }

    void NodeManager::HandlePendingNodeCreation() {
        if (!has_pending_node) {
            return; 
        }

        Debug::Log("CREATING: " + pending_node_data);

        // Create the appropriate node type
        switch (pending_node_type) {
        case NodeType::INPUT_COLORSPACE:
            CreateInputColorSpaceNode(pending_node_data, pending_node_position);
            break;
        case NodeType::LOOK:
            CreateLookNode(pending_node_data, pending_node_position);
            break;
        case NodeType::OUTPUT_DISPLAY:
            CreateOutputDisplayNode(pending_node_data, pending_node_position);
            break;
        default:
            break;
        }

        // Clear pending state
        has_pending_node = false;
        pending_node_data.clear();
    }

    // OCIO

    std::string NodeManager::BuildOCIOTransform() {
        auto pipeline_nodes = GetPipelineOrder();
        if (pipeline_nodes.empty()) {
            Debug::Log("No valid pipeline to build");
            return "";
        }

        // Build OCIO transform string
        std::stringstream transform;

        for (size_t i = 0; i < pipeline_nodes.size(); ++i) {
            auto* node = pipeline_nodes[i];

            switch (node->GetType()) {
            case NodeType::INPUT_COLORSPACE: {  // Not InputColorSpace
                auto* csNode = dynamic_cast<InputColorSpaceNode*>(node);
                if (csNode && !csNode->GetColorSpace().empty()) {
                    // For input, we need to specify the source colorspace
                    if (i == 0) {
                        transform << csNode->GetColorSpace();
                    }
                }
                break;
            }
            case NodeType::LOOK: {  // Not Look
                auto* lookNode = dynamic_cast<LookNode*>(node);
                if (lookNode && !lookNode->GetLook().empty()) {  // GetLook() not GetLookName()
                    if (transform.str().length() > 0) transform << "|";
                    transform << "look:" << lookNode->GetLook();  // GetLook() not GetLookName()
                }
                break;
            }
            case NodeType::OUTPUT_DISPLAY: {  // Not OutputDisplay
                auto* displayNode = dynamic_cast<OutputDisplayNode*>(node);
                if (displayNode) {
                    if (transform.str().length() > 0) transform << "|";
                    transform << displayNode->GetDisplay() << " - " << displayNode->GetView();
                }
                break;
            }
            }
        }

        std::string result = transform.str();
        Debug::Log("Built OCIO transform: " + result);
        return result;
    }

    bool NodeManager::HasValidPipeline() {
        auto pipeline = GetPipelineOrder();

        // A valid pipeline should have at least an input and output
        if (pipeline.size() < 2) return false;

        // Check first node is input type
        if (pipeline.front()->GetType() != NodeType::INPUT_COLORSPACE) return false;  // Fix here

        // Check last node is output type
        if (pipeline.back()->GetType() != NodeType::OUTPUT_DISPLAY) return false;  // Fix here

        return true;
    }

    std::vector<NodeBase*> NodeManager::GetPipelineOrder() {
        std::vector<NodeBase*> ordered;

        // Find the input node (node with no incoming connections)
        NodeBase* current = FindStartNode();
        if (!current) {
            Debug::Log("No start node found");
            return ordered;
        }

        // Follow the chain through connections
        std::set<NodeBase*> visited;  // Prevent infinite loops

        while (current && visited.find(current) == visited.end()) {
            ordered.push_back(current);
            visited.insert(current);

            // Find next connected node
            // Get the output pin of the current node
            const auto& output_pins = current->GetOutputPins();
            if (output_pins.empty()) {
                break;  // No output pins, end of chain
            }

            int output_pin_id = output_pins[0].id;  // Assume single output
            NodeBase* next = nullptr;

            // Look for a connection from this output pin
            for (const auto& conn : connections) {
                if (conn.from_pin == output_pin_id) {
                    // Find the node that owns the end pin
                    next = FindNodeByPinId(conn.to_pin);
                    if (next) break;
                }
            }

            current = next;
        }

        Debug::Log("Pipeline order: " + std::to_string(ordered.size()) + " nodes");
        return ordered;
    }

    NodeBase* NodeManager::FindStartNode() {
        // Find a node with no incoming connections to its input pins
        for (auto& [id, node] : nodes) {
            // Only consider input colorspace nodes as potential start nodes
            if (node->GetType() != NodeType::INPUT_COLORSPACE) {
                continue;
            }

            bool has_incoming = false;

            // Check all input pins of this node
            for (const auto& input_pin : node->GetInputPins()) {
                // Check if any connection ends at this input pin
                for (const auto& conn : connections) {
                    if (conn.to_pin == input_pin.id) {
                        has_incoming = true;
                        break;
                    }
                }
                if (has_incoming) break;
            }

            if (!has_incoming) {
                return node.get();
            }
        }

        // If no pure input node found, just return first input colorspace node
        for (auto& [id, node] : nodes) {
            if (node->GetType() == NodeType::INPUT_COLORSPACE) {
                return node.get();
            }
        }

        return nullptr;
    }

    NodeBase* NodeManager::FindNodeByPinId(int pin_id) {
        for (auto& [id, node] : nodes) {
            // Check input pins
            for (const auto& pin : node->GetInputPins()) {
                if (pin.id == pin_id) {
                    return node.get();
                }
            }
            // Check output pins
            for (const auto& pin : node->GetOutputPins()) {
                if (pin.id == pin_id) {
                    return node.get();
                }
            }
        }
        return nullptr;
    }

}