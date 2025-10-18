#pragma once

#include "node_base.h"
#include <unordered_map>
#include <vector>
#include <functional>

namespace ump {

    struct NodeConnection {
        int from_pin;
        int to_pin;
        int connection_id;
    };

    class NodeManager {
    public:
        NodeManager();
        ~NodeManager() = default;

        // Node creation
        int CreateInputColorSpaceNode(const std::string& colorspace_name, ImVec2 position = ImVec2(0, 0));
        int CreateLookNode(const std::string& look_name, ImVec2 position = ImVec2(0, 0));
        int CreateSceneLUTNode(const std::string& lut_path, ImVec2 position = ImVec2(0, 0));
        int CreateDisplayLUTNode(const std::string& lut_path, ImVec2 position = ImVec2(0, 0));
        int CreateOutputDisplayNode(const std::string& display_name, ImVec2 position = ImVec2(0, 0));

        // Node management
        void DeleteNode(int node_id);
        void DeleteSelectedNodes();
        NodeBase* GetNode(int node_id);
        NodeBase* GetNodeById(int node_id);
        std::vector<NodeBase*> GetAllNodes() const;

        // Rendering
        void RenderAllNodes();

        // Selection and properties
        void UpdateSelection();
        void RenderSelectedNodeProperties();
        bool HasSelectedNode() const;

        // Connections
        void HandleConnections();
        void DeleteConnection(int connection_id);
        void CreateConnection(int from_node_id, int from_pin_index, int to_node_id, int to_pin_index);
        void AddConnectionDirect(const NodeConnection& conn);  // For loading from file

        // Drag & Drop support
        void SetPendingNodeCreation(NodeType type, const std::string& data);
        void HandlePendingNodeCreation();

        // Pipeline building
        std::string BuildOCIOTransform();  // Returns OCIO transform string
        bool HasValidPipeline();  // Check if we have a complete pipeline

        // Get connected nodes in pipeline order
        std::vector<NodeBase*> GetPipelineOrder();
        const std::vector<NodeConnection>& GetConnections() const { return connections; }
        std::function<void()> on_connections_changed;


    private:
        std::unordered_map<int, std::unique_ptr<NodeBase>> nodes;
        std::vector<NodeConnection> connections;
        std::vector<int> selected_nodes;
        int next_node_id;
        int next_connection_id;

        // Drag & Drop state
        bool has_pending_node;
        NodeType pending_node_type;
        std::string pending_node_data;
        ImVec2 pending_node_position;

        // Helper methods for graph traversal
        NodeBase* FindStartNode();  // Find the input node (no incoming connections)
        NodeBase* FindNodeByPinId(int pin_id);  // Find node that owns a pin
    };

}