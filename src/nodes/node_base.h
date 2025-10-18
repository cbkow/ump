#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include "../imnodes/imnodes.h"

namespace ump {

    enum class NodeType {
        INPUT_COLORSPACE,
        LOOK,
        SCENE_LUT,      // LUT applied before display transform (scene-referred)
        DISPLAY_LUT,    // LUT applied after display transform (display-referred)
        TRANSFORM,
        CDL,
        OUTPUT_DISPLAY
    };

    enum class PinType {
        INPUT,
        OUTPUT
    };

    struct NodePin {
        int id;
        PinType type;
        std::string name;
        ImU32 color;

        NodePin(int pin_id, PinType pin_type, const std::string& pin_name, ImU32 pin_color = IM_COL32(100, 140, 200, 255))
            : id(pin_id), type(pin_type), name(pin_name), color(pin_color) {
        }
    };

    class NodeBase {
    public:
        NodeBase(int node_id, NodeType node_type, const std::string& title);
        virtual ~NodeBase() = default;

        // Core interface
        virtual void Render() = 0;
        virtual void UpdateProperties() = 0;
        virtual bool HasProperties() const { return true; }

        // Getters
        int GetId() const { return id; }
        NodeType GetType() const { return type; }
        const std::string& GetTitle() const { return title; }
        const std::vector<NodePin>& GetInputPins() const { return input_pins; }
        const std::vector<NodePin>& GetOutputPins() const { return output_pins; }

        // Pin management
        void AddInputPin(const std::string& name, ImU32 color = IM_COL32(100, 140, 200, 255));
        void AddOutputPin(const std::string& name, ImU32 color = IM_COL32(100, 140, 200, 255));

        // Node colors
        virtual ImU32 GetTitleColor() const = 0;
        virtual ImU32 GetTitleColorSelected() const = 0;

    protected:
        int id;
        NodeType type;
        std::string title;
        std::vector<NodePin> input_pins;
        std::vector<NodePin> output_pins;

        static int next_pin_id;
    };

    // Specific node types
    class InputColorSpaceNode : public NodeBase {
    public:
        InputColorSpaceNode(int node_id, const std::string& colorspace_name);

        void Render() override;
        void UpdateProperties() override;
        ImU32 GetTitleColor() const override;
        ImU32 GetTitleColorSelected() const override;

        const std::string& GetColorSpace() const { return colorspace; }
        void SetColorSpace(const std::string& cs) { colorspace = cs; title = "Input: " + cs; }

    private:
        std::string colorspace;
    };

    class LookNode : public NodeBase {
    public:
        LookNode(int node_id, const std::string& look_name);

        void Render() override;
        void UpdateProperties() override;
        ImU32 GetTitleColor() const override;
        ImU32 GetTitleColorSelected() const override;

        const std::string& GetLook() const { return look_name; }
        void SetLook(const std::string& look) { look_name = look; title = "Look: " + look; }

    private:
        std::string look_name;
    };

    class OutputDisplayNode : public NodeBase {
    public:
        OutputDisplayNode(int node_id, const std::string& display_name);

        void Render() override;
        void UpdateProperties() override;
        ImU32 GetTitleColor() const override;
        ImU32 GetTitleColorSelected() const override;

        const std::string& GetDisplay() const { return display_name; }
        const std::string& GetView() const { return view_name; }
        void SetDisplay(const std::string& display) {
            display_name = display;
            UpdateTitle();
        }
        void SetView(const std::string& view) {
            view_name = view;
            UpdateTitle();
        }

    private:
        std::string display_name;
        std::string view_name;

        void UpdateTitle();
    };

    // Scene-Referred LUT (applied before display transform)
    class SceneLUTNode : public NodeBase {
    public:
        SceneLUTNode(int node_id, const std::string& lut_path);

        void Render() override;
        void UpdateProperties() override;
        ImU32 GetTitleColor() const override;
        ImU32 GetTitleColorSelected() const override;

        const std::string& GetLUTPath() const { return lut_file_path; }
        const std::string& GetLUTFileName() const { return lut_filename; }
        void SetLUTPath(const std::string& path);

    private:
        std::string lut_file_path;   // Full path
        std::string lut_filename;    // Display name (filename only)
    };

    // Display-Referred LUT (applied after display transform)
    class DisplayLUTNode : public NodeBase {
    public:
        DisplayLUTNode(int node_id, const std::string& lut_path);

        void Render() override;
        void UpdateProperties() override;
        ImU32 GetTitleColor() const override;
        ImU32 GetTitleColorSelected() const override;

        const std::string& GetLUTPath() const { return lut_file_path; }
        const std::string& GetLUTFileName() const { return lut_filename; }
        void SetLUTPath(const std::string& path);

    private:
        std::string lut_file_path;   // Full path
        std::string lut_filename;    // Display name (filename only)
    };

}