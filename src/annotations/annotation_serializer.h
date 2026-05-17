// AnnotationSerializer — direct lift from old QCView's
// src/annotations/annotation_serializer.{h,cpp} per Guide 19 §2.4.
//
// Stroke list ↔ JSON round-trip. Schema preserved exactly:
//
//   {
//     "version":           "2.0",       // also accepts "1.0"
//     "coordinate_system": "normalized",
//     "shapes": [
//       {
//         "id":           "stroke-<ms>-<rand>",
//         "type":         "freehand" | "rect" | "oval" | "arrow" | "line",
//         "color":        [r, g, b, a],
//         "stroke_width": <float>,
//         "filled":       <bool>,
//         "is_modeled":   <bool>,        // v2.0 only
//         "points":       [[nx, ny], ...]
//       }, ...
//     ]
//   }
//
// v1.0 strokes load with is_modeled=false (legacy strokes need
// render-time smoothing); v2.0 strokes preserve the flag verbatim.
//
// Adaptation notes vs old app:
//   - Namespace qcview::Annotations → qcv
//   - nlohmann::json → QJsonObject / QJsonArray / QJsonDocument
//   - std::string → QString
//   - ImVec2/ImVec4 → QPointF/QColor (already adapted in ActiveStroke)
//   - GenerateStrokeId() uses QDateTime + QRandomGenerator (was
//     std::chrono + std::mt19937; the format on disk is identical)

#pragma once

#include "active_stroke.h"

#include <QJsonObject>
#include <QString>

#include <vector>

namespace qcv {

class AnnotationSerializer {
public:
    static QJsonObject strokeToJson(const ActiveStroke &stroke);

    static QString strokesToJsonString(const std::vector<ActiveStroke> &strokes);

    static std::vector<ActiveStroke> jsonStringToStrokes(const QString &jsonString);

    // Returns false if required fields are missing or unparseable.
    static bool jsonToStroke(const QJsonObject &obj, ActiveStroke &out);

    // {"version":"2.0","coordinate_system":"normalized","shapes":[]}
    static QString createEmptyAnnotationData();

    static bool hasStrokes(const QString &jsonString);

private:
    static QString toolToString(DrawingTool tool);
    static DrawingTool stringToTool(const QString &str);

    static QString generateStrokeId();
};

} // namespace qcv
