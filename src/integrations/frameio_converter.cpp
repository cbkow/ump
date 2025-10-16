#include "frameio_converter.h"
#include "../annotations/annotation_serializer.h"
#include "../utils/debug_utils.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace ump {
namespace Integrations {

FrameioConverter::ConversionResult FrameioConverter::ConvertComments(
    const std::vector<FrameioComment>& comments,
    double video_framerate
) {
    ConversionResult result;

    for (const auto& frameio_comment : comments) {
        AnnotationNote note;

        // Frame.io timestamps are in frame numbers, not seconds
        note.frame = static_cast<int>(std::round(frameio_comment.timestamp));
        note.timestamp_seconds = note.frame / video_framerate;
        note.timecode = FormatTimecode(note.timestamp_seconds, video_framerate);

        // Set text with commenter name if available
        if (!frameio_comment.owner_name.empty()) {
            note.text = frameio_comment.owner_name + ": " + frameio_comment.text;
        } else {
            note.text = frameio_comment.text;
        }

        // Convert annotation data if present
        if (!frameio_comment.annotation_json.empty()) {
            // Log what we're trying to convert (for debugging)
            std::string preview = frameio_comment.annotation_json.length() > 100
                ? frameio_comment.annotation_json.substr(0, 100) + "..."
                : frameio_comment.annotation_json;

            std::string converted_json = ConvertAnnotationJson(frameio_comment.annotation_json);
            if (!converted_json.empty()) {
                note.annotation_data = converted_json;
                result.converted_count++;
            } else {
                result.skipped_count++;
                // Empty conversion - annotation might be text-only or unsupported format
            }
        }

        // No image path for imported annotations
        note.image_path = "";

        result.notes.push_back(note);
    }

    result.success = true;
    return result;
}

std::string FrameioConverter::ConvertAnnotationJson(const std::string& frameio_annotation) {
    try {
        nlohmann::json frameio_json = nlohmann::json::parse(frameio_annotation);

        // Frame.io annotations can be an array of shapes or a single shape
        std::vector<Annotations::ActiveStroke> strokes;

        auto process_shape = [&](const nlohmann::json& shape) {
            Annotations::ActiveStroke stroke;

            // Get tool type
            std::string tool = shape.value("tool", "");
            stroke.tool = MapToolType(tool);

            if (stroke.tool == Annotations::DrawingTool::NONE) {
                return;  // Skip unknown tools
            }

            // Get color
            std::string color_hex = shape.value("color", "#FF0000");
            stroke.color = HexToRgba(color_hex);

            // Get stroke width (Frame.io calls it "size")
            stroke.stroke_width = shape.value("size", 3.0f);

            // Not filled by default
            stroke.filled = false;

            // Pen tool uses xs/ys arrays for freehand drawing
            if (tool == "pen" && shape.contains("xs") && shape.contains("ys")) {
                const auto& xs = shape["xs"];
                const auto& ys = shape["ys"];

                if (xs.is_array() && ys.is_array() && xs.size() == ys.size()) {
                    for (size_t i = 0; i < xs.size(); ++i) {
                        float x = static_cast<float>(xs[i].get<double>());
                        float y = static_cast<float>(ys[i].get<double>());
                        stroke.points.push_back(ImVec2(x, y));
                    }
                }
            } else {
                // Other tools use x/y/w/h or x1/y1/x2/y2 format
                double x = shape.value("x", 0.0);
                double y = shape.value("y", 0.0);
                double w = shape.value("w", 0.0);
                double h = shape.value("h", 0.0);
                double x1 = shape.value("x1", 0.0);
                double y1 = shape.value("y1", 0.0);
                double x2 = shape.value("x2", 0.0);
                double y2 = shape.value("y2", 0.0);

                // Convert to points
                stroke.points = ConvertShapeToPoints(tool, x, y, w, h, x1, y1, x2, y2);
            }

            if (!stroke.points.empty()) {
                stroke.is_complete = true;
                strokes.push_back(stroke);
            }
        };

        // Handle array of shapes or single shape
        if (frameio_json.is_array()) {
            for (const auto& shape : frameio_json) {
                process_shape(shape);
            }
        } else if (frameio_json.is_object()) {
            process_shape(frameio_json);
        }

        // Convert to ump JSON format
        if (!strokes.empty()) {
            Debug::Log("Converted " + std::to_string(strokes.size()) + " Frame.io strokes to ump format");
            return Annotations::AnnotationSerializer::StrokesToJsonString(strokes);
        }

    } catch (const std::exception&) {
        return "";  // Return empty on parse error
    }

    return "";
}

ImVec4 FrameioConverter::HexToRgba(const std::string& hex) {
    // Handle hex format: #RRGGBB or #RGB
    std::string clean_hex = hex;

    // Remove # if present
    if (!clean_hex.empty() && clean_hex[0] == '#') {
        clean_hex = clean_hex.substr(1);
    }

    // Default to red if invalid
    if (clean_hex.length() != 6 && clean_hex.length() != 3) {
        return ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    }

    // Expand 3-digit format to 6-digit (#RGB -> #RRGGBB)
    if (clean_hex.length() == 3) {
        clean_hex = std::string() +
            clean_hex[0] + clean_hex[0] +
            clean_hex[1] + clean_hex[1] +
            clean_hex[2] + clean_hex[2];
    }

    // Parse hex values
    unsigned int r, g, b;
    std::stringstream ss;
    ss << std::hex << clean_hex.substr(0, 2);
    ss >> r;
    ss.clear();
    ss << std::hex << clean_hex.substr(2, 2);
    ss >> g;
    ss.clear();
    ss << std::hex << clean_hex.substr(4, 2);
    ss >> b;

    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
}

Annotations::DrawingTool FrameioConverter::MapToolType(const std::string& frameio_tool) {
    if (frameio_tool == "rect") {
        return Annotations::DrawingTool::RECTANGLE;
    } else if (frameio_tool == "line") {
        return Annotations::DrawingTool::LINE;
    } else if (frameio_tool == "arrow") {
        return Annotations::DrawingTool::ARROW;
    } else if (frameio_tool == "ellipse" || frameio_tool == "oval") {
        return Annotations::DrawingTool::OVAL;
    } else if (frameio_tool == "path" || frameio_tool == "freehand" || frameio_tool == "pen") {
        return Annotations::DrawingTool::FREEHAND;
    }

    return Annotations::DrawingTool::NONE;
}

std::vector<ImVec2> FrameioConverter::ConvertShapeToPoints(
    const std::string& tool,
    double x, double y,
    double w, double h,
    double x1, double y1,
    double x2, double y2
) {
    std::vector<ImVec2> points;

    if (tool == "rect") {
        // Rectangle: x, y = top-left, w, h = width, height
        // ump expects 4 corners: top-left, top-right, bottom-right, bottom-left
        float x1 = static_cast<float>(x);
        float y1 = static_cast<float>(y);
        float x2 = static_cast<float>(x + w);
        float y2 = static_cast<float>(y + h);

        points.push_back(ImVec2(x1, y1));  // Top-left
        points.push_back(ImVec2(x2, y1));  // Top-right
        points.push_back(ImVec2(x2, y2));  // Bottom-right
        points.push_back(ImVec2(x1, y2));  // Bottom-left

        Debug::Log("Rect: 4 corners from (" + std::to_string(x) + "," + std::to_string(y) + ") to (" +
                   std::to_string(x + w) + "," + std::to_string(y + h) + ")");
    } else if (tool == "ellipse" || tool == "oval") {
        // Oval/Ellipse: x, y = top-left, w, h = width, height
        // Convert to two-point format: top-left and bottom-right
        points.push_back(ImVec2(static_cast<float>(x), static_cast<float>(y)));
        points.push_back(ImVec2(static_cast<float>(x + w), static_cast<float>(y + h)));
    } else if (tool == "arrow") {
        // Frame.io arrow: x, y = start point, w, h = width/height offsets (can be negative)
        // Convert to two-point format: start and end
        points.push_back(ImVec2(static_cast<float>(x), static_cast<float>(y)));
        points.push_back(ImVec2(static_cast<float>(x + w), static_cast<float>(y + h)));
        Debug::Log("Arrow: (" + std::to_string(x) + "," + std::to_string(y) + ") -> (" +
                   std::to_string(x + w) + "," + std::to_string(y + h) + ")");
    } else if (tool == "line") {
        // Line: x1, y1, x2, y2 (if Frame.io ever uses this format)
        points.push_back(ImVec2(static_cast<float>(x1), static_cast<float>(y1)));
        points.push_back(ImVec2(static_cast<float>(x2), static_cast<float>(y2)));
    }

    return points;
}

std::string FrameioConverter::FormatTimecode(double timestamp_seconds, double framerate) {
    int total_frames = static_cast<int>(timestamp_seconds * framerate);
    int hours = total_frames / static_cast<int>(3600 * framerate);
    int minutes = (total_frames / static_cast<int>(60 * framerate)) % 60;
    int seconds = (total_frames / static_cast<int>(framerate)) % 60;
    int frames = total_frames % static_cast<int>(framerate);

    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(2) << hours << ":"
        << std::setw(2) << minutes << ":"
        << std::setw(2) << seconds << ":"
        << std::setw(2) << frames;

    return oss.str();
}

} // namespace Integrations
} // namespace ump
