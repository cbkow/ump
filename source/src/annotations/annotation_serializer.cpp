#include "annotation_serializer.h"
#include "stroke_smoother.h"
#include <sstream>
#include <iomanip>
#include <random>
#include <chrono>

namespace ump {
namespace Annotations {

nlohmann::json AnnotationSerializer::StrokeToJson(const ActiveStroke& stroke) {
    nlohmann::json shape;

    // Generate unique ID
    shape["id"] = GenerateStrokeId();

    // Tool type
    shape["type"] = ToolToString(stroke.tool);

    // Color (RGBA)
    shape["color"] = {
        stroke.color.x,
        stroke.color.y,
        stroke.color.z,
        stroke.color.w
    };

    // Stroke properties
    shape["stroke_width"] = stroke.stroke_width;
    shape["filled"] = stroke.filled;

    // Points (normalized coordinates) - store raw points, smoothing applied during rendering
    nlohmann::json points_array = nlohmann::json::array();
    for (const auto& point : stroke.points) {
        points_array.push_back({point.x, point.y});
    }
    shape["points"] = points_array;

    return shape;
}

std::string AnnotationSerializer::StrokesToJsonString(const std::vector<ActiveStroke>& strokes) {
    nlohmann::json root;

    root["version"] = "1.0";
    root["coordinate_system"] = "normalized";

    nlohmann::json shapes_array = nlohmann::json::array();
    for (const auto& stroke : strokes) {
        shapes_array.push_back(StrokeToJson(stroke));
    }
    root["shapes"] = shapes_array;

    // Pretty print with 2-space indent
    return root.dump(2);
}

std::vector<ActiveStroke> AnnotationSerializer::JsonStringToStrokes(const std::string& json_string) {
    std::vector<ActiveStroke> strokes;

    if (json_string.empty()) {
        return strokes;
    }

    try {
        nlohmann::json root = nlohmann::json::parse(json_string);

        // Check version (future-proofing)
        if (root.contains("version")) {
            std::string version = root["version"].get<std::string>();
            // Currently only support version 1.0
            if (version != "1.0") {
                return strokes; // Unknown version
            }
        }

        // Check coordinate system
        if (root.contains("coordinate_system")) {
            std::string coord_system = root["coordinate_system"].get<std::string>();
            if (coord_system != "normalized") {
                return strokes; // Unsupported coordinate system
            }
        }

        // Parse shapes array
        if (root.contains("shapes") && root["shapes"].is_array()) {
            for (const auto& shape_json : root["shapes"]) {
                ActiveStroke stroke;
                if (JsonToStroke(shape_json, stroke)) {
                    strokes.push_back(stroke);
                }
            }
        }
    }
    catch (const nlohmann::json::exception& e) {
        // JSON parsing error - return empty vector
        return std::vector<ActiveStroke>();
    }

    return strokes;
}

bool AnnotationSerializer::JsonToStroke(const nlohmann::json& json_obj, ActiveStroke& out_stroke) {
    try {
        // Tool type (required)
        if (!json_obj.contains("type")) {
            return false;
        }
        std::string type_str = json_obj["type"].get<std::string>();
        out_stroke.tool = StringToTool(type_str);

        if (out_stroke.tool == DrawingTool::NONE) {
            return false; // Unknown tool type
        }

        // Color (required)
        if (json_obj.contains("color") && json_obj["color"].is_array() && json_obj["color"].size() >= 4) {
            out_stroke.color.x = json_obj["color"][0].get<float>();
            out_stroke.color.y = json_obj["color"][1].get<float>();
            out_stroke.color.z = json_obj["color"][2].get<float>();
            out_stroke.color.w = json_obj["color"][3].get<float>();
        } else {
            // Default color if missing
            out_stroke.color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
        }

        // Stroke width
        if (json_obj.contains("stroke_width")) {
            out_stroke.stroke_width = json_obj["stroke_width"].get<float>();
        } else {
            out_stroke.stroke_width = 2.5f; // Default
        }

        // Fill flag
        if (json_obj.contains("filled")) {
            out_stroke.filled = json_obj["filled"].get<bool>();
        } else {
            out_stroke.filled = false; // Default
        }

        // Points (required)
        if (json_obj.contains("points") && json_obj["points"].is_array()) {
            for (const auto& point_array : json_obj["points"]) {
                if (point_array.is_array() && point_array.size() >= 2) {
                    ImVec2 point;
                    point.x = point_array[0].get<float>();
                    point.y = point_array[1].get<float>();
                    out_stroke.points.push_back(point);
                }
            }
        }

        if (out_stroke.points.empty()) {
            return false; // No valid points
        }

        // Mark as complete
        out_stroke.is_complete = true;

        return true;
    }
    catch (const nlohmann::json::exception& e) {
        return false;
    }
}

std::string AnnotationSerializer::CreateEmptyAnnotationData() {
    nlohmann::json root;
    root["version"] = "1.0";
    root["coordinate_system"] = "normalized";
    root["shapes"] = nlohmann::json::array();
    return root.dump(2);
}

bool AnnotationSerializer::HasStrokes(const std::string& json_string) {
    if (json_string.empty()) {
        return false;
    }

    try {
        nlohmann::json root = nlohmann::json::parse(json_string);
        if (root.contains("shapes") && root["shapes"].is_array()) {
            return !root["shapes"].empty();
        }
    }
    catch (const nlohmann::json::exception& e) {
        return false;
    }

    return false;
}

std::string AnnotationSerializer::ToolToString(DrawingTool tool) {
    switch (tool) {
        case DrawingTool::FREEHAND:  return "freehand";
        case DrawingTool::RECTANGLE: return "rect";
        case DrawingTool::OVAL:      return "oval";
        case DrawingTool::ARROW:     return "arrow";
        case DrawingTool::LINE:      return "line";
        default:                     return "unknown";
    }
}

DrawingTool AnnotationSerializer::StringToTool(const std::string& tool_str) {
    if (tool_str == "freehand") return DrawingTool::FREEHAND;
    if (tool_str == "rect")     return DrawingTool::RECTANGLE;
    if (tool_str == "oval")     return DrawingTool::OVAL;
    if (tool_str == "arrow")    return DrawingTool::ARROW;
    if (tool_str == "line")     return DrawingTool::LINE;
    return DrawingTool::NONE;
}

std::string AnnotationSerializer::GenerateStrokeId() {
    // Simple UUID-like ID using timestamp + random number
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);

    std::ostringstream oss;
    oss << "stroke-" << timestamp << "-" << dis(gen);
    return oss.str();
}

} // namespace Annotations
} // namespace ump
