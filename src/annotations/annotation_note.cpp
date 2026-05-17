#include "annotation_note.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace qcv {

QJsonObject noteToJson(const AnnotationNote &note)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("timecode"),          note.timecode);
    obj.insert(QStringLiteral("timestamp_seconds"), note.timestamp_seconds);
    obj.insert(QStringLiteral("frame"),             note.frame);
    obj.insert(QStringLiteral("image"),             note.image_path);

    // annotation_data: in-memory string → on-disk nested object.
    // Empty string serializes as JSON null (matches old app shape).
    if (note.annotation_data.isEmpty()) {
        obj.insert(QStringLiteral("annotation_data"), QJsonValue::Null);
    } else {
        const QJsonDocument doc =
            QJsonDocument::fromJson(note.annotation_data.toUtf8());
        if (doc.isObject()) {
            obj.insert(QStringLiteral("annotation_data"), doc.object());
        } else if (doc.isArray()) {
            obj.insert(QStringLiteral("annotation_data"), doc.array());
        } else {
            obj.insert(QStringLiteral("annotation_data"), QJsonValue::Null);
        }
    }

    obj.insert(QStringLiteral("text"),      note.text);
    obj.insert(QStringLiteral("addressed"), note.addressed);
    return obj;
}

bool noteFromJson(const QJsonObject &obj, AnnotationNote &out)
{
    if (!obj.contains(QStringLiteral("timecode")) ||
        !obj.contains(QStringLiteral("timestamp_seconds")) ||
        !obj.contains(QStringLiteral("frame")) ||
        !obj.contains(QStringLiteral("image"))) {
        return false;
    }

    out.timecode          = obj.value(QStringLiteral("timecode")).toString();
    out.timestamp_seconds = obj.value(QStringLiteral("timestamp_seconds")).toDouble();
    out.frame             = obj.value(QStringLiteral("frame")).toInt();
    out.image_path        = obj.value(QStringLiteral("image")).toString();

    // annotation_data: nested object → string. Null / missing → empty.
    if (obj.contains(QStringLiteral("annotation_data"))) {
        const QJsonValue v = obj.value(QStringLiteral("annotation_data"));
        if (v.isNull() || v.isUndefined()) {
            out.annotation_data.clear();
        } else if (v.isObject()) {
            out.annotation_data = QString::fromUtf8(
                QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
        } else if (v.isArray()) {
            out.annotation_data = QString::fromUtf8(
                QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
        } else {
            out.annotation_data.clear();
        }
    } else {
        out.annotation_data.clear();
    }

    out.text      = obj.value(QStringLiteral("text")).toString();
    out.addressed = obj.value(QStringLiteral("addressed")).toBool(false);
    return true;
}

} // namespace qcv
