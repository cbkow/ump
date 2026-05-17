// SvgPathParser — see header for adaptation notes.

#include "svg_path_parser.h"

#include <QFile>
#include <QXmlStreamReader>
#include <QtLogging>

#include <cctype>

namespace qcv {

namespace {

// Direct lift of old SVGOverlayRenderer::ParseSVGPath. Hand-rolled
// mini-parser for the subset of SVG path commands the shipped safety
// SVGs use: M (moveto), L (lineto), H (h-line), V (v-line), and the
// relative `h` / `v`, plus Z/z (close).
//
// std::isspace / std::isdigit / std::isalpha are deliberate — they
// match the old behavior on ASCII path data.
void parsePathCommands(const QString &pathData, ParsedSvg &out)
{
    const QByteArray bytes = pathData.toLatin1();
    const char *data       = bytes.constData();
    const int   length     = bytes.size();

    double currentX = 0.0, currentY = 0.0;
    double pathStartX = 0.0, pathStartY = 0.0;

    int  pos             = 0;
    char currentCommand  = '\0';

    auto skipSep = [&]() {
        while (pos < length &&
               (std::isspace(static_cast<unsigned char>(data[pos])) ||
                data[pos] == ',')) {
            ++pos;
        }
    };

    while (pos < length) {
        skipSep();
        if (pos >= length) break;

        if (std::isalpha(static_cast<unsigned char>(data[pos]))) {
            currentCommand = data[pos++];
        }

        std::vector<double> coords;
        while (pos < length) {
            skipSep();
            if (pos >= length) break;
            if (std::isalpha(static_cast<unsigned char>(data[pos]))) break;

            const int start = pos;
            if (data[pos] == '-' || data[pos] == '+') ++pos;
            while (pos < length &&
                   (std::isdigit(static_cast<unsigned char>(data[pos])) ||
                    data[pos] == '.')) {
                ++pos;
            }
            if (pos > start) {
                bool ok = false;
                const double v = QByteArray(data + start, pos - start).toDouble(&ok);
                if (ok) coords.push_back(v);
            }
        }

        switch (currentCommand) {
            case 'M':
                if (coords.size() >= 2) {
                    currentX = coords[0]; currentY = coords[1];
                    pathStartX = currentX; pathStartY = currentY;
                }
                break;
            case 'L':
                if (coords.size() >= 2) {
                    out.lines.push_back({QPointF(currentX, currentY),
                                         QPointF(coords[0], coords[1])});
                    currentX = coords[0]; currentY = coords[1];
                }
                break;
            case 'H':
                if (!coords.empty()) {
                    out.lines.push_back({QPointF(currentX, currentY),
                                         QPointF(coords[0], currentY)});
                    currentX = coords[0];
                }
                break;
            case 'V':
                if (!coords.empty()) {
                    out.lines.push_back({QPointF(currentX, currentY),
                                         QPointF(currentX, coords[0])});
                    currentY = coords[0];
                }
                break;
            case 'h':
                if (!coords.empty()) {
                    const double newX = currentX + coords[0];
                    out.lines.push_back({QPointF(currentX, currentY),
                                         QPointF(newX, currentY)});
                    currentX = newX;
                }
                break;
            case 'v':
                if (!coords.empty()) {
                    const double newY = currentY + coords[0];
                    out.lines.push_back({QPointF(currentX, currentY),
                                         QPointF(currentX, newY)});
                    currentY = newY;
                }
                break;
            case 'Z':
            case 'z':
                out.lines.push_back({QPointF(currentX, currentY),
                                     QPointF(pathStartX, pathStartY)});
                currentX = pathStartX; currentY = pathStartY;
                break;
            default:
                break;
        }
    }
}

void parseRect(const QXmlStreamAttributes &attrs, ParsedSvg &out)
{
    bool xOk = false, yOk = false, wOk = false, hOk = false;
    const double x = attrs.value(QStringLiteral("x")).toDouble(&xOk);
    const double y = attrs.value(QStringLiteral("y")).toDouble(&yOk);
    const double w = attrs.value(QStringLiteral("width")).toDouble(&wOk);
    const double h = attrs.value(QStringLiteral("height")).toDouble(&hOk);

    // x and y default to 0 when omitted (per SVG spec).
    if (!wOk || !hOk) return;
    const double rx = xOk ? x : 0.0;
    const double ry = yOk ? y : 0.0;

    out.lines.push_back({QPointF(rx,     ry    ), QPointF(rx + w, ry    )});  // top
    out.lines.push_back({QPointF(rx + w, ry    ), QPointF(rx + w, ry + h)});  // right
    out.lines.push_back({QPointF(rx + w, ry + h), QPointF(rx,     ry + h)});  // bottom
    out.lines.push_back({QPointF(rx,     ry + h), QPointF(rx,     ry    )});  // left
}

void parseLineEl(const QXmlStreamAttributes &attrs, ParsedSvg &out)
{
    bool x1Ok = false, y1Ok = false, x2Ok = false, y2Ok = false;
    const double x1 = attrs.value(QStringLiteral("x1")).toDouble(&x1Ok);
    const double y1 = attrs.value(QStringLiteral("y1")).toDouble(&y1Ok);
    const double x2 = attrs.value(QStringLiteral("x2")).toDouble(&x2Ok);
    const double y2 = attrs.value(QStringLiteral("y2")).toDouble(&y2Ok);
    out.lines.push_back({
        QPointF(x1Ok ? x1 : 0.0, y1Ok ? y1 : 0.0),
        QPointF(x2Ok ? x2 : 0.0, y2Ok ? y2 : 0.0),
    });
}

// Shared by <polygon> and <polyline>; closeShape closes the loop.
void parsePointsList(const QStringView &pointsData, bool closeShape,
                     ParsedSvg &out)
{
    QString clean = pointsData.toString();
    clean.replace(QLatin1Char(','), QLatin1Char(' '));

    std::vector<double> coords;
    const QStringList tokens =
        clean.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    coords.reserve(tokens.size());
    for (const QString &t : tokens) {
        bool ok = false;
        const double v = t.toDouble(&ok);
        if (ok) coords.push_back(v);
    }

    std::vector<QPointF> points;
    points.reserve(coords.size() / 2);
    for (std::size_t i = 0; i + 1 < coords.size(); i += 2) {
        points.emplace_back(coords[i], coords[i + 1]);
    }
    if (points.size() < 2) return;

    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        out.lines.push_back({points[i], points[i + 1]});
    }
    if (closeShape && points.size() >= 3) {
        out.lines.push_back({points.back(), points.front()});
    }
}

} // namespace

ParsedSvg SvgPathParser::parseContent(const QString &svgContent)
{
    ParsedSvg out;

    QXmlStreamReader reader(svgContent);
    while (!reader.atEnd() && !reader.hasError()) {
        if (reader.readNext() != QXmlStreamReader::StartElement) continue;

        const QStringView name = reader.name();
        const QXmlStreamAttributes attrs = reader.attributes();

        if (name == QLatin1String("svg")) {
            const QStringView viewBox =
                attrs.value(QStringLiteral("viewBox"));
            if (!viewBox.isEmpty()) {
                const QStringList parts =
                    viewBox.toString().split(QLatin1Char(' '),
                                             Qt::SkipEmptyParts);
                if (parts.size() == 4) {
                    bool wOk = false, hOk = false;
                    const double w = parts.at(2).toDouble(&wOk);
                    const double h = parts.at(3).toDouble(&hOk);
                    if (wOk && hOk && w > 0.0 && h > 0.0) {
                        out.viewBox = QSizeF(w, h);
                    }
                }
            }
        } else if (name == QLatin1String("path")) {
            const QString d = attrs.value(QStringLiteral("d")).toString();
            if (!d.isEmpty()) parsePathCommands(d, out);
        } else if (name == QLatin1String("rect")) {
            parseRect(attrs, out);
        } else if (name == QLatin1String("line")) {
            parseLineEl(attrs, out);
        } else if (name == QLatin1String("polygon")) {
            parsePointsList(attrs.value(QStringLiteral("points")),
                            /*closeShape=*/true, out);
        } else if (name == QLatin1String("polyline")) {
            parsePointsList(attrs.value(QStringLiteral("points")),
                            /*closeShape=*/false, out);
        }
    }

    if (reader.hasError()) {
        qWarning("SvgPathParser: parse error: %s",
                 qPrintable(reader.errorString()));
    }
    return out;
}

ParsedSvg SvgPathParser::parseFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning("SvgPathParser: cannot open %s: %s",
                 qPrintable(path), qPrintable(f.errorString()));
        return {};
    }
    const QByteArray bytes = f.readAll();
    return parseContent(QString::fromUtf8(bytes));
}

} // namespace qcv
