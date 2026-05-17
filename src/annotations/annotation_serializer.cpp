#include "annotation_serializer.h"

#include <QColor>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointF>
#include <QRandomGenerator>

namespace qcv {

QJsonObject AnnotationSerializer::strokeToJson(const ActiveStroke &stroke)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("id"),   generateStrokeId());
    obj.insert(QStringLiteral("type"), toolToString(stroke.tool));

    QJsonArray colorArr;
    colorArr.append(stroke.color.redF());
    colorArr.append(stroke.color.greenF());
    colorArr.append(stroke.color.blueF());
    colorArr.append(stroke.color.alphaF());
    obj.insert(QStringLiteral("color"), colorArr);

    obj.insert(QStringLiteral("stroke_width"), stroke.strokeWidth);
    obj.insert(QStringLiteral("filled"),       stroke.filled);
    obj.insert(QStringLiteral("is_modeled"),   stroke.isModeled);

    QJsonArray pointsArr;
    for (const QPointF &p : stroke.points) {
        QJsonArray pt;
        pt.append(p.x());
        pt.append(p.y());
        pointsArr.append(pt);
    }
    obj.insert(QStringLiteral("points"), pointsArr);

    return obj;
}

QString AnnotationSerializer::strokesToJsonString(
    const std::vector<ActiveStroke> &strokes)
{
    QJsonObject root;
    root.insert(QStringLiteral("version"),           QStringLiteral("2.0"));
    root.insert(QStringLiteral("coordinate_system"), QStringLiteral("normalized"));

    QJsonArray shapes;
    for (const ActiveStroke &s : strokes) {
        shapes.append(strokeToJson(s));
    }
    root.insert(QStringLiteral("shapes"), shapes);

    return QString::fromUtf8(
        QJsonDocument(root).toJson(QJsonDocument::Indented));
}

std::vector<ActiveStroke> AnnotationSerializer::jsonStringToStrokes(
    const QString &jsonString)
{
    std::vector<ActiveStroke> result;
    if (jsonString.isEmpty()) return result;

    QJsonParseError err{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(jsonString.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return result;
    }

    const QJsonObject root = doc.object();

    // Versions 1.0 + 2.0 supported. Unknown version → empty.
    QString version = QStringLiteral("1.0");
    if (root.contains(QStringLiteral("version"))) {
        version = root.value(QStringLiteral("version")).toString();
        if (version != QStringLiteral("1.0") && version != QStringLiteral("2.0")) {
            return result;
        }
    }
    const bool isV2 = (version == QStringLiteral("2.0"));

    if (root.contains(QStringLiteral("coordinate_system"))) {
        const QString cs = root.value(QStringLiteral("coordinate_system")).toString();
        if (cs != QStringLiteral("normalized")) return result;
    }

    if (!root.contains(QStringLiteral("shapes"))) return result;
    const QJsonValue shapesV = root.value(QStringLiteral("shapes"));
    if (!shapesV.isArray()) return result;

    const QJsonArray shapes = shapesV.toArray();
    for (const QJsonValue &v : shapes) {
        if (!v.isObject()) continue;
        ActiveStroke stroke;
        if (jsonToStroke(v.toObject(), stroke)) {
            // v1.0: always raw points (need render-time smoothing).
            if (!isV2) stroke.isModeled = false;
            result.push_back(std::move(stroke));
        }
    }
    return result;
}

bool AnnotationSerializer::jsonToStroke(const QJsonObject &obj,
                                        ActiveStroke &out)
{
    if (!obj.contains(QStringLiteral("type"))) return false;

    out.tool = stringToTool(obj.value(QStringLiteral("type")).toString());
    if (out.tool == DrawingTool::None) return false;

    if (obj.contains(QStringLiteral("color"))) {
        const QJsonArray c = obj.value(QStringLiteral("color")).toArray();
        if (c.size() >= 4) {
            out.color = QColor::fromRgbF(
                static_cast<float>(c.at(0).toDouble()),
                static_cast<float>(c.at(1).toDouble()),
                static_cast<float>(c.at(2).toDouble()),
                static_cast<float>(c.at(3).toDouble()));
        } else {
            out.color = QColor(255, 0, 0, 255);
        }
    } else {
        out.color = QColor(255, 0, 0, 255);
    }

    out.strokeWidth = obj.contains(QStringLiteral("stroke_width"))
        ? static_cast<float>(obj.value(QStringLiteral("stroke_width")).toDouble())
        : 2.5f;

    out.filled    = obj.value(QStringLiteral("filled")).toBool(false);
    out.isModeled = obj.value(QStringLiteral("is_modeled")).toBool(false);

    if (!obj.contains(QStringLiteral("points"))) return false;
    const QJsonArray pts = obj.value(QStringLiteral("points")).toArray();
    out.points.clear();
    for (const QJsonValue &v : pts) {
        if (!v.isArray()) continue;
        const QJsonArray pa = v.toArray();
        if (pa.size() < 2) continue;
        out.points.emplace_back(pa.at(0).toDouble(), pa.at(1).toDouble());
    }
    if (out.points.empty()) return false;

    out.isComplete = true;
    return true;
}

QString AnnotationSerializer::createEmptyAnnotationData()
{
    QJsonObject root;
    root.insert(QStringLiteral("version"),           QStringLiteral("2.0"));
    root.insert(QStringLiteral("coordinate_system"), QStringLiteral("normalized"));
    root.insert(QStringLiteral("shapes"),            QJsonArray{});
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

bool AnnotationSerializer::hasStrokes(const QString &jsonString)
{
    if (jsonString.isEmpty()) return false;
    QJsonParseError err{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(jsonString.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonValue v = doc.object().value(QStringLiteral("shapes"));
    return v.isArray() && !v.toArray().isEmpty();
}

QString AnnotationSerializer::toolToString(DrawingTool tool)
{
    switch (tool) {
        case DrawingTool::Freehand:  return QStringLiteral("freehand");
        case DrawingTool::Rectangle: return QStringLiteral("rect");
        case DrawingTool::Oval:      return QStringLiteral("oval");
        case DrawingTool::Arrow:     return QStringLiteral("arrow");
        case DrawingTool::Line:      return QStringLiteral("line");
        case DrawingTool::None:
        default:                     return QStringLiteral("unknown");
    }
}

DrawingTool AnnotationSerializer::stringToTool(const QString &str)
{
    if (str == QLatin1String("freehand")) return DrawingTool::Freehand;
    if (str == QLatin1String("rect"))     return DrawingTool::Rectangle;
    if (str == QLatin1String("oval"))     return DrawingTool::Oval;
    if (str == QLatin1String("arrow"))    return DrawingTool::Arrow;
    if (str == QLatin1String("line"))     return DrawingTool::Line;
    return DrawingTool::None;
}

QString AnnotationSerializer::generateStrokeId()
{
    const qint64 ms = QDateTime::currentMSecsSinceEpoch();
    const quint32 r = QRandomGenerator::global()->bounded(1000u, 10000u);
    return QStringLiteral("stroke-%1-%2").arg(ms).arg(r);
}

} // namespace qcv
