#pragma once

#include <string>
#include <vector>
#include "../annotations/annotation_note.h"
#include "../annotations/viewport_annotator.h"
#include "frameio_client.h"

namespace ump {
namespace Integrations {

/**
 * Converts Frame.io comments and annotations to ump format.
 */
class FrameioConverter {
public:
    struct ConversionResult {
        bool success = false;
        std::vector<AnnotationNote> notes;
        std::string error_message;
        int converted_count = 0;
        int skipped_count = 0;
    };

    /**
     * Convert Frame.io comments to ump annotation notes.
     *
     * @param comments Frame.io comments from API
     * @param video_framerate Framerate of the video for timecode conversion
     * @return ConversionResult with converted notes or error
     */
    static ConversionResult ConvertComments(
        const std::vector<FrameioComment>& comments,
        double video_framerate
    );

private:
    /**
     * Convert Frame.io annotation JSON to ump format.
     * Returns empty string if conversion fails.
     */
    static std::string ConvertAnnotationJson(const std::string& frameio_annotation);

    /**
     * Convert hex color (#RRGGBB) to RGBA array.
     */
    static ImVec4 HexToRgba(const std::string& hex);

    /**
     * Map Frame.io tool type to ump tool type.
     */
    static Annotations::DrawingTool MapToolType(const std::string& frameio_tool);

    /**
     * Convert Frame.io shape to ump points array.
     */
    static std::vector<ImVec2> ConvertShapeToPoints(
        const std::string& tool,
        double x, double y,
        double w, double h,
        double x1, double y1,
        double x2, double y2
    );

    /**
     * Format timestamp as timecode string (HH:MM:SS:FF).
     */
    static std::string FormatTimecode(double timestamp_seconds, double framerate);
};

} // namespace Integrations
} // namespace ump
