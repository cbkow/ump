#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "viewport_annotator.h"

namespace ump {
namespace Annotations {

/**
 * Serialization helpers for annotation drawing data.
 * Converts strokes to/from JSON format for persistence.
 */
class AnnotationSerializer {
public:
    /**
     * Serialize a single stroke to JSON object.
     */
    static nlohmann::json StrokeToJson(const ActiveStroke& stroke);

    /**
     * Serialize multiple strokes to complete annotation JSON string.
     */
    static std::string StrokesToJsonString(const std::vector<ActiveStroke>& strokes);

    /**
     * Deserialize JSON string to vector of strokes.
     * Returns empty vector if JSON is invalid or empty.
     */
    static std::vector<ActiveStroke> JsonStringToStrokes(const std::string& json_string);

    /**
     * Deserialize a single JSON object to stroke.
     * Returns nullptr if JSON is invalid.
     */
    static bool JsonToStroke(const nlohmann::json& json_obj, ActiveStroke& out_stroke);

    /**
     * Create empty annotation data JSON (for new annotations).
     */
    static std::string CreateEmptyAnnotationData();

    /**
     * Check if annotation data contains any strokes.
     */
    static bool HasStrokes(const std::string& json_string);

private:
    /**
     * Convert DrawingTool enum to string.
     */
    static std::string ToolToString(DrawingTool tool);

    /**
     * Convert string to DrawingTool enum.
     */
    static DrawingTool StringToTool(const std::string& tool_str);

    /**
     * Generate unique ID for stroke.
     */
    static std::string GenerateStrokeId();
};

} // namespace Annotations
} // namespace ump
