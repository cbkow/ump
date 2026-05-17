// AnnotationExporter implementation. See header for architectural
// notes — this file is the format-specific writers + the common
// note-collection pre-pass.

#include "annotation_exporter.h"

#include "annotation_manager.h"
#include "annotation_note.h"

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QSize>
#include <functional>
#include <QImageReader>
#include <QPdfWriter>
#include <QPainter>
#include <QPageSize>
#include <QPageLayout>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextImageFormat>
#include <QXmlStreamWriter>
#include <QtLogging>

// Qt's QtCore-private OOXML zip writer. DOCX is a plain
// ZIP-with-XML bundle so this is enough to produce files that
// open in Word / Pages / Google Docs.
#include <private/qzipwriter_p.h>

namespace qcv {

namespace {

QString fileStemForTimecode(const QString &timecode) {
    return QString(timecode)
        .replace(QLatin1Char(':'), QLatin1Char('_'))
        .replace(QLatin1Char(';'), QLatin1Char('_'));
}

QString htmlEscape(const QString &s) {
    return QString(s)
        .replace(QStringLiteral("&"), QStringLiteral("&amp;"))
        .replace(QStringLiteral("<"), QStringLiteral("&lt;"))
        .replace(QStringLiteral(">"), QStringLiteral("&gt;"))
        .replace(QStringLiteral("\n"), QStringLiteral("<br/>"));
}

bool readImageDimensions(const QString &path, int &w, int &h) {
    if (path.isEmpty()) return false;
    QImageReader r(path);
    const QSize s = r.size();
    if (!s.isValid()) return false;
    w = s.width();
    h = s.height();
    return true;
}

} // namespace

AnnotationExporter::AnnotationExporter(QObject *parent)
    : QObject(parent)
{
}

QString AnnotationExporter::brandIconPath()
{
    // Search the same fallback chain as the safety SVG resolver:
    // macOS bundle → out-of-tree build → in-tree source. Returns
    // the first path that exists.
    const QString appDir =
        QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/../Resources/assets/icons/qcview_export.png"),
        appDir + QStringLiteral("/assets/icons/qcview_export.png"),
        appDir + QStringLiteral("/../../assets/icons/qcview_export.png"),
        appDir + QStringLiteral("/../../../assets/icons/qcview_export.png"),
    };
    for (const QString &p : candidates) {
        if (QFileInfo::exists(p)) return QFileInfo(p).canonicalFilePath();
    }
    return QString();
}

std::vector<AnnotationExporter::ExportNote>
AnnotationExporter::collectNotes() const
{
    std::vector<ExportNote> out;
    if (!m_manager) return out;
    const QString imagesDir = m_manager->imagesFolder();
    const auto src = m_manager->notes();
    out.reserve(src.size());
    for (const auto &n : src) {
        ExportNote en;
        en.timecode         = n.timecode;
        en.frame            = n.frame;
        en.timestampSeconds = n.timestamp_seconds;
        en.text             = n.text;
        en.addressed        = n.addressed;

        // Resolve the on-disk PNG. Prefer the annotated sibling
        // when present so the export shows what the user was
        // looking at; fall back to the clean baseline otherwise.
        if (!imagesDir.isEmpty()) {
            const QString stem = fileStemForTimecode(n.timecode);
            const QString clean =
                imagesDir + QLatin1Char('/')
                + QStringLiteral("note_%1.png").arg(stem);
            const QString annotated =
                imagesDir + QLatin1Char('/')
                + QStringLiteral("note_%1_annotated.png").arg(stem);
            if (QFileInfo::exists(annotated)) en.imagePath = annotated;
            else if (QFileInfo::exists(clean)) en.imagePath = clean;
        }
        readImageDimensions(en.imagePath, en.imageWidth, en.imageHeight);
        out.push_back(std::move(en));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Markdown
// ---------------------------------------------------------------------------

bool AnnotationExporter::exportMarkdown(const QString &outputDir,
                                         const QString &outputBaseName)
{
    if (outputDir.isEmpty()) return false;
    if (!m_manager) return false;
    const auto notes = collectNotes();

    // Filesystem-safe basename. When the caller supplies one
    // (typically the user-typed name from the save dialog), use
    // it for both the folder and the .md file. Otherwise fall
    // back to the auto-generated "{mediaName}-{stamp}".
    auto sanitize = [](const QString &s) -> QString {
        QString out = s;
        for (QChar &c : out) {
            const ushort u = c.unicode();
            const bool ok = (u >= 'A' && u <= 'Z')
                          || (u >= 'a' && u <= 'z')
                          || (u >= '0' && u <= '9')
                          || u == '_' || u == '-' || u == '.'
                          || u == ' ';
            if (!ok) c = QLatin1Char('_');
        }
        return out.trimmed();
    };
    QString safeName;
    if (!outputBaseName.isEmpty()) {
        safeName = sanitize(outputBaseName);
    } else {
        const QString fallback = m_mediaName.isEmpty()
                                  ? QStringLiteral("notes")
                                  : m_mediaName;
        const QString stamp = QDateTime::currentDateTime()
                                 .toString(QStringLiteral("yyyyMMdd-HHmmss"));
        safeName = sanitize(QStringLiteral("%1-%2").arg(fallback, stamp));
    }
    if (safeName.isEmpty()) safeName = QStringLiteral("notes");

    const QString folder = outputDir + QLatin1Char('/') + safeName;
    QDir().mkpath(folder + QStringLiteral("/images"));

    // Copy the branding icon into the export's images folder so
    // the markdown can reference it via a relative path. Quietly
    // skipped when the icon can't be found (e.g. dev build with
    // assets unbundled).
    QString brandRel;
    {
        const QString brandSrc = AnnotationExporter::brandIconPath();
        if (!brandSrc.isEmpty()) {
            const QString destAbs = folder
                + QStringLiteral("/images/qcview_export.png");
            QFile::remove(destAbs);
            if (QFile::copy(brandSrc, destAbs)) {
                brandRel = QStringLiteral("images/qcview_export.png");
            }
        }
    }

    // Copy each referenced PNG into the export's images/ subfolder
    // so the markdown is self-contained — the user can zip the
    // folder, drop it in Notion / GitHub / wherever and the
    // images travel with it.
    QStringList md;
    if (!brandRel.isEmpty()) {
        md << QStringLiteral("![](%1)").arg(brandRel);
        md << QString();
    }
    md << QStringLiteral("# %1").arg(m_mediaName.isEmpty()
                                       ? QStringLiteral("Notes")
                                       : m_mediaName);
    md << QString();
    md << QStringLiteral("Exported: %1").arg(
            QDateTime::currentDateTime().toString(Qt::ISODate));
    if (!m_mediaPath.isEmpty()) {
        md << QStringLiteral("Source: `%1`").arg(m_mediaPath);
    }
    md << QString();

    for (const auto &n : notes) {
        QString relImage;
        if (!n.imagePath.isEmpty()) {
            const QFileInfo fi(n.imagePath);
            const QString destName = fi.fileName();
            const QString destAbs  =
                folder + QStringLiteral("/images/") + destName;
            // Best-effort; remove existing dest then copy.
            QFile::remove(destAbs);
            if (QFile::copy(n.imagePath, destAbs)) {
                relImage = QStringLiteral("images/") + destName;
            }
        }

        md << QStringLiteral("## %1 (frame %2)%3")
                .arg(n.timecode,
                     QString::number(n.frame),
                     n.addressed
                       ? QStringLiteral(" ✅")
                       : QString());
        md << QString();
        if (!relImage.isEmpty()) {
            md << QStringLiteral("![%1](%2)")
                  .arg(n.timecode, relImage);
            md << QString();
        }
        if (!n.text.trimmed().isEmpty()) {
            md << n.text;
            md << QString();
        }
        md << QString();
    }

    const QString path = folder + QLatin1Char('/')
                       + safeName + QStringLiteral(".md");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning("AnnotationExporter: cannot write %s",
                 qPrintable(path));
        return false;
    }
    f.write(md.join(QLatin1Char('\n')).toUtf8());
    f.close();
    return true;
}

// ---------------------------------------------------------------------------
// Shared HTML body
// ---------------------------------------------------------------------------

QString AnnotationExporter::buildHtmlBody(
    const std::vector<ExportNote> &notes,
    const QString &imageEmbedMode) const
{
    QString out;
    out.reserve(8 * 1024);

    // Branding icon at the top — base64-embedded for HTML so the
    // file stays self-contained, file:// for the PDF path so
    // QTextDocument's image cache picks it up.
    const QString brandPath = AnnotationExporter::brandIconPath();
    if (!brandPath.isEmpty()) {
        QString src;
        if (imageEmbedMode == QStringLiteral("base64")) {
            QFile f(brandPath);
            if (f.open(QIODevice::ReadOnly)) {
                src = QStringLiteral("data:image/png;base64,")
                    + QString::fromLatin1(f.readAll().toBase64());
                f.close();
            }
        } else {
            src = QStringLiteral("file://") + brandPath;
        }
        if (!src.isEmpty()) {
            out += QStringLiteral(
                "<img class='brand' src='%1' alt='qcview' />").arg(src);
        }
    }

    auto title = m_mediaName.isEmpty() ? QStringLiteral("Notes")
                                          : m_mediaName;
    out += QStringLiteral("<h1>%1</h1>").arg(htmlEscape(title));
    out += QStringLiteral("<p style='color:#666;font-size:11px;'>"
                          "Exported %1</p>")
              .arg(QDateTime::currentDateTime().toString(Qt::ISODate));
    if (!m_mediaPath.isEmpty()) {
        out += QStringLiteral("<p style='color:#666;font-size:11px;'>"
                              "Source: %1</p>")
                  .arg(htmlEscape(m_mediaPath));
    }
    out += QStringLiteral("<hr/>");

    for (const auto &n : notes) {
        out += QStringLiteral("<h2>%1 <span style='color:#888;"
                              "font-size:12px;font-weight:normal;'>"
                              "(frame %2)%3</span></h2>")
                  .arg(htmlEscape(n.timecode),
                       QString::number(n.frame),
                       n.addressed
                         ? QStringLiteral(" ✅ addressed")
                         : QString());

        if (!n.imagePath.isEmpty()) {
            QString src;
            if (imageEmbedMode == QStringLiteral("base64")) {
                QFile f(n.imagePath);
                if (f.open(QIODevice::ReadOnly)) {
                    const QByteArray b64 =
                        f.readAll().toBase64();
                    src = QStringLiteral("data:image/png;base64,")
                        + QString::fromLatin1(b64);
                    f.close();
                }
            } else {
                src = QStringLiteral("file://") + n.imagePath;
            }
            if (!src.isEmpty()) {
                // <figure> wrapper instead of <p> — the global
                // p {max-width:780px} rule was inheriting onto
                // image-wrapping paragraphs, capping note frames
                // at the text-column width. <figure> bypasses
                // that and lets .frame's max-width:100% resolve
                // against the full body cap.
                out += QStringLiteral(
                    "<figure><img class='frame' src='%1' /></figure>")
                       .arg(src);
            }
        }

        if (!n.text.trimmed().isEmpty()) {
            out += QStringLiteral("<p>%1</p>").arg(htmlEscape(n.text));
        }
        out += QStringLiteral("<hr style='border:0;border-top:1px "
                              "solid #ddd;'/>");
    }
    return out;
}

// ---------------------------------------------------------------------------
// HTML
// ---------------------------------------------------------------------------

bool AnnotationExporter::exportHtml(const QString &outputFile)
{
    if (outputFile.isEmpty()) return false;
    if (!m_manager) return false;
    const auto notes = collectNotes();

    const QString title = m_mediaName.isEmpty()
                             ? QStringLiteral("Notes")
                             : m_mediaName;
    // Body has a 1600 px max-width and is centered so the page
    // reads consistently regardless of monitor width. Images
    // render at native pixel size up to body-width — anything
    // wider is auto-scaled (no cropping). Text elements get a
    // narrower 780-px column so headlines and paragraphs don't
    // sprawl on a wide page.
    // Body is centered at 1600 px so text columns read
    // consistently; images render at native pixel size (no
    // max-width on .frame) so users see full-resolution review
    // frames even when wider than the body's text column. The
    // text column itself caps at 780 px so headlines / paragraphs
    // don't sprawl. The brand icon overrides the frame's
    // 1px border + radius so it reads as a logo, not a screenshot.
    const QString css = QStringLiteral(
        "body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,"
        "Roboto,sans-serif;max-width:1600px;margin:24px auto;"
        "padding:0 24px;color:#222;line-height:1.5;}"
        "h1{font-size:22px;margin:0 0 4px 0;max-width:780px;}"
        "h2{font-size:15px;margin:24px 0 8px 0;max-width:780px;}"
        "p{margin:6px 0;max-width:780px;}"
        "figure{margin:8px 0;}"
        ".frame{display:block;max-width:100%;height:auto;"
        "border:1px solid #e0e0e0;border-radius:3px;}"
        ".brand{display:block;width:64px;height:64px;border:none;"
        "border-radius:0;margin:0 0 8px 0;}");

    QString html;
    html += QStringLiteral("<!DOCTYPE html><html><head>");
    html += QStringLiteral("<meta charset='utf-8'/>");
    html += QStringLiteral("<title>%1</title>").arg(htmlEscape(title));
    html += QStringLiteral("<style>%1</style>").arg(css);
    html += QStringLiteral("</head><body>");
    html += buildHtmlBody(notes, QStringLiteral("base64"));
    html += QStringLiteral("</body></html>");

    QFile f(outputFile);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning("AnnotationExporter: cannot write %s",
                 qPrintable(outputFile));
        return false;
    }
    f.write(html.toUtf8());
    f.close();
    return true;
}

// ---------------------------------------------------------------------------
// PDF
// ---------------------------------------------------------------------------

bool AnnotationExporter::exportPdf(const QString &outputFile)
{
    if (outputFile.isEmpty()) return false;
    if (!m_manager) return false;
    const auto notes = collectNotes();

    QPdfWriter writer(outputFile);
    writer.setPageSize(QPageSize(QPageSize::Letter));
    writer.setPageMargins(QMarginsF(15, 15, 15, 15), QPageLayout::Millimeter);
    writer.setResolution(150);
    writer.setTitle(m_mediaName.isEmpty() ? QStringLiteral("Notes")
                                            : m_mediaName);

    // Built programmatically with QTextCursor + page-break blocks
    // so we get one note per page (cover page first). The HTML
    // path was scaling images poorly — a 1920×1080 frame ended
    // up bigger than the page and getting cropped. Here we
    // pre-scale each image to fit the page's content rect.
    QTextDocument doc;
    // Match the writer's content rect so QTextDocument paginates
    // at the same size QPdfWriter renders.
    const QSizeF pageRectPx =
        writer.pageLayout().paintRectPixels(writer.resolution()).size();
    doc.setPageSize(pageRectPx);
    const int contentWPx = static_cast<int>(pageRectPx.width());
    // Cap image height aggressively — leave room for timecode
    // header (~6%) AND a chunk of body text (~25%) on the same
    // page. Without this the text overflows past the page break
    // and lands on a fresh page, splitting the note.
    const int contentHPx =
        static_cast<int>(pageRectPx.height() * 0.6);

    QTextCursor cur(&doc);

    // ---- Cover page -----------------------------------------
    // Branding icon (sized to 64×64 px regardless of source res)
    // sits above the title. addResource registers the QImage so
    // QTextDocument's HTML / image rendering can find it via its
    // pseudo-URL.
    bool brandInserted = false;
    {
        const QString brandPath = AnnotationExporter::brandIconPath();
        if (!brandPath.isEmpty()) {
            QImage brandImg(brandPath);
            if (!brandImg.isNull()) {
                const QString resName =
                    QStringLiteral("qcv_brand");
                doc.addResource(QTextDocument::ImageResource,
                                 QUrl(resName),
                                 QVariant::fromValue(brandImg));
                QTextImageFormat brandFmt;
                brandFmt.setName(resName);
                brandFmt.setWidth(64);
                brandFmt.setHeight(64);
                // Apply the bottom-margin to THIS block (the
                // one that will hold the icon) before inserting
                // the image. setTopMargin on the next block ate
                // into the title's line-height instead of acting
                // like a paragraph gap.
                QTextBlockFormat brandBlockFmt;
                brandBlockFmt.setBottomMargin(36);
                cur.setBlockFormat(brandBlockFmt);
                cur.insertImage(brandFmt);
                cur.insertBlock(QTextBlockFormat()); // start title block
                brandInserted = true;
            }
        }
    }

    QTextCharFormat titleFmt;
    titleFmt.setFontPointSize(22);
    titleFmt.setFontWeight(QFont::Bold);
    cur.insertText(m_mediaName.isEmpty()
                     ? QStringLiteral("Notes")
                     : m_mediaName, titleFmt);
    cur.insertBlock();
    QTextCharFormat metaFmt;
    metaFmt.setFontPointSize(10);
    metaFmt.setForeground(QColor(0x66, 0x66, 0x66));
    cur.insertText(QStringLiteral("Exported %1").arg(
        QDateTime::currentDateTime().toString(Qt::ISODate)), metaFmt);
    if (!m_mediaPath.isEmpty()) {
        cur.insertBlock();
        cur.insertText(QStringLiteral("Source: %1").arg(m_mediaPath),
                        metaFmt);
    }
    cur.insertBlock();
    cur.insertText(QStringLiteral("%1 %2").arg(
        QString::number(notes.size()),
        notes.size() == 1 ? QStringLiteral("note")
                          : QStringLiteral("notes")), metaFmt);

    // ---- One page per note -----------------------------------
    // Two block formats: pageBreak forces a fresh page before the
    // note's first block; defaultBlock is plain. WITHOUT the
    // explicit "no break" format, insertBlock() inherits the
    // current cursor's format (page-break-before), so every
    // block in the note would land on its own page — that was
    // the visible bug where frame, image, and text each got
    // their own page.
    QTextBlockFormat pageBreak;
    pageBreak.setPageBreakPolicy(QTextFormat::PageBreak_Auto);
    pageBreak.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysBefore);

    QTextBlockFormat defaultBlock;
    defaultBlock.setPageBreakPolicy(QTextFormat::PageBreak_Auto);

    QTextCharFormat noteHeaderFmt;
    noteHeaderFmt.setFontPointSize(14);
    noteHeaderFmt.setFontWeight(QFont::Bold);

    QTextCharFormat noteSubFmt;
    noteSubFmt.setFontPointSize(10);
    noteSubFmt.setForeground(QColor(0x77, 0x77, 0x77));

    QTextCharFormat noteBodyFmt;
    noteBodyFmt.setFontPointSize(11);

    for (const auto &n : notes) {
        // Header block: page-break-before + bottom margin so the
        // following image isn't glued to the timecode line.
        QTextBlockFormat headerBlock = pageBreak;
        headerBlock.setBottomMargin(18);
        cur.insertBlock(headerBlock);
        cur.insertText(n.timecode, noteHeaderFmt);
        cur.insertText(QStringLiteral("   frame %1%2").arg(
            QString::number(n.frame),
            n.addressed ? QStringLiteral("   ✅ addressed")
                        : QString()), noteSubFmt);

        // Image block: pre-scale to fit the page, then apply a
        // bottom margin so body text below has breathing room.
        if (!n.imagePath.isEmpty()) {
            QImage img(n.imagePath);
            if (!img.isNull()) {
                if (img.width() > contentWPx
                    || img.height() > contentHPx) {
                    img = img.scaled(contentWPx, contentHPx,
                                      Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation);
                }
                const QString resName =
                    QStringLiteral("note_img_%1").arg(n.timecode);
                doc.addResource(QTextDocument::ImageResource,
                                 QUrl(resName),
                                 QVariant::fromValue(img));
                QTextImageFormat imgFmt;
                imgFmt.setName(resName);
                imgFmt.setWidth(img.width());
                imgFmt.setHeight(img.height());

                QTextBlockFormat imgBlockFmt = defaultBlock;
                imgBlockFmt.setBottomMargin(18);
                cur.insertBlock(imgBlockFmt);
                cur.insertImage(imgFmt);
            }
        }

        if (!n.text.trimmed().isEmpty()) {
            cur.insertBlock(defaultBlock);
            cur.insertText(n.text, noteBodyFmt);
        }
    }

    doc.print(&writer);
    return true;
}

// ---------------------------------------------------------------------------
// DOCX (Office Open XML)
// ---------------------------------------------------------------------------

namespace {

// EMU = English Metric Unit. OOXML's drawingml uses these for
// extent dimensions: 914400 EMU == 1 inch == 96 px at 96 DPI.
constexpr int kEmuPerInch = 914400;

// Letter page minus 1" margins → 6.5" content; clamp display
// width slightly under at 6 inches for breathing room.
constexpr int kImageDisplayWidthInches = 6;

QByteArray xmlString(const std::function<void(QXmlStreamWriter &)> &fn) {
    QByteArray ba;
    QXmlStreamWriter w(&ba);
    w.setAutoFormatting(false);
    w.writeStartDocument(QStringLiteral("1.0"), true);
    fn(w);
    w.writeEndDocument();
    return ba;
}

QByteArray contentTypesXml() {
    return xmlString([](QXmlStreamWriter &w) {
        w.writeStartElement(QStringLiteral("Types"));
        w.writeAttribute(QStringLiteral("xmlns"),
            QStringLiteral("http://schemas.openxmlformats.org/"
                           "package/2006/content-types"));
        auto def = [&](const QString &ext, const QString &type) {
            w.writeStartElement(QStringLiteral("Default"));
            w.writeAttribute(QStringLiteral("Extension"), ext);
            w.writeAttribute(QStringLiteral("ContentType"), type);
            w.writeEndElement();
        };
        def("rels",
            "application/vnd.openxmlformats-package.relationships+xml");
        def("xml", "application/xml");
        def("png", "image/png");
        w.writeStartElement(QStringLiteral("Override"));
        w.writeAttribute(QStringLiteral("PartName"),
                          QStringLiteral("/word/document.xml"));
        w.writeAttribute(QStringLiteral("ContentType"),
            QStringLiteral("application/vnd.openxmlformats-officedocument."
                           "wordprocessingml.document.main+xml"));
        w.writeEndElement();
        w.writeEndElement();
    });
}

QByteArray packageRelsXml() {
    return xmlString([](QXmlStreamWriter &w) {
        w.writeStartElement(QStringLiteral("Relationships"));
        w.writeAttribute(QStringLiteral("xmlns"),
            QStringLiteral("http://schemas.openxmlformats.org/"
                           "package/2006/relationships"));
        w.writeStartElement(QStringLiteral("Relationship"));
        w.writeAttribute(QStringLiteral("Id"), QStringLiteral("rId1"));
        w.writeAttribute(QStringLiteral("Type"),
            QStringLiteral("http://schemas.openxmlformats.org/"
                           "officeDocument/2006/relationships/officeDocument"));
        w.writeAttribute(QStringLiteral("Target"),
                          QStringLiteral("word/document.xml"));
        w.writeEndElement();
        w.writeEndElement();
    });
}

QByteArray documentRelsXml(int imageCount, bool hasBrand) {
    return xmlString([&](QXmlStreamWriter &w) {
        w.writeStartElement(QStringLiteral("Relationships"));
        w.writeAttribute(QStringLiteral("xmlns"),
            QStringLiteral("http://schemas.openxmlformats.org/"
                           "package/2006/relationships"));
        const int total = imageCount + (hasBrand ? 1 : 0);
        for (int i = 0; i < total; ++i) {
            w.writeStartElement(QStringLiteral("Relationship"));
            w.writeAttribute(QStringLiteral("Id"),
                              QStringLiteral("rIdImg%1").arg(i + 1));
            w.writeAttribute(QStringLiteral("Type"),
                QStringLiteral("http://schemas.openxmlformats.org/"
                               "officeDocument/2006/relationships/image"));
            w.writeAttribute(QStringLiteral("Target"),
                              QStringLiteral("media/image%1.png").arg(i + 1));
            w.writeEndElement();
        }
        w.writeEndElement();
    });
}

// Inline drawing XML for an embedded image. relId == "rIdImg1"
// etc.; widthEmu / heightEmu are display sizes.
void writeDrawing(QXmlStreamWriter &w, const QString &relId,
                  int widthEmu, int heightEmu, int picId)
{
    w.writeStartElement(QStringLiteral("w:drawing"));
    w.writeStartElement(QStringLiteral("wp:inline"));
    w.writeAttribute(QStringLiteral("distT"), QStringLiteral("0"));
    w.writeAttribute(QStringLiteral("distB"), QStringLiteral("0"));
    w.writeAttribute(QStringLiteral("distL"), QStringLiteral("0"));
    w.writeAttribute(QStringLiteral("distR"), QStringLiteral("0"));

    w.writeStartElement(QStringLiteral("wp:extent"));
    w.writeAttribute(QStringLiteral("cx"), QString::number(widthEmu));
    w.writeAttribute(QStringLiteral("cy"), QString::number(heightEmu));
    w.writeEndElement();

    w.writeStartElement(QStringLiteral("wp:docPr"));
    w.writeAttribute(QStringLiteral("id"), QString::number(picId));
    w.writeAttribute(QStringLiteral("name"),
                      QStringLiteral("Picture %1").arg(picId));
    w.writeEndElement();

    w.writeStartElement(QStringLiteral("a:graphic"));
    w.writeAttribute(QStringLiteral("xmlns:a"),
        QStringLiteral("http://schemas.openxmlformats.org/"
                       "drawingml/2006/main"));
    w.writeStartElement(QStringLiteral("a:graphicData"));
    w.writeAttribute(QStringLiteral("uri"),
        QStringLiteral("http://schemas.openxmlformats.org/"
                       "drawingml/2006/picture"));

    w.writeStartElement(QStringLiteral("pic:pic"));
    w.writeAttribute(QStringLiteral("xmlns:pic"),
        QStringLiteral("http://schemas.openxmlformats.org/"
                       "drawingml/2006/picture"));

    w.writeStartElement(QStringLiteral("pic:nvPicPr"));
    w.writeStartElement(QStringLiteral("pic:cNvPr"));
    w.writeAttribute(QStringLiteral("id"), QString::number(picId));
    w.writeAttribute(QStringLiteral("name"),
                      QStringLiteral("image%1.png").arg(picId));
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("pic:cNvPicPr"));
    w.writeEndElement();
    w.writeEndElement();

    w.writeStartElement(QStringLiteral("pic:blipFill"));
    w.writeStartElement(QStringLiteral("a:blip"));
    w.writeAttribute(QStringLiteral("xmlns:r"),
        QStringLiteral("http://schemas.openxmlformats.org/"
                       "officeDocument/2006/relationships"));
    w.writeAttribute(QStringLiteral("r:embed"), relId);
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("a:stretch"));
    w.writeStartElement(QStringLiteral("a:fillRect"));
    w.writeEndElement();
    w.writeEndElement();
    w.writeEndElement();

    w.writeStartElement(QStringLiteral("pic:spPr"));
    w.writeStartElement(QStringLiteral("a:xfrm"));
    w.writeStartElement(QStringLiteral("a:off"));
    w.writeAttribute(QStringLiteral("x"), QStringLiteral("0"));
    w.writeAttribute(QStringLiteral("y"), QStringLiteral("0"));
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("a:ext"));
    w.writeAttribute(QStringLiteral("cx"), QString::number(widthEmu));
    w.writeAttribute(QStringLiteral("cy"), QString::number(heightEmu));
    w.writeEndElement();
    w.writeEndElement();
    w.writeStartElement(QStringLiteral("a:prstGeom"));
    w.writeAttribute(QStringLiteral("prst"), QStringLiteral("rect"));
    w.writeStartElement(QStringLiteral("a:avLst"));
    w.writeEndElement();
    w.writeEndElement();
    w.writeEndElement();

    w.writeEndElement();   // pic:pic
    w.writeEndElement();   // a:graphicData
    w.writeEndElement();   // a:graphic
    w.writeEndElement();   // wp:inline
    w.writeEndElement();   // w:drawing
}

// Plain-text paragraph with optional pStyle (Heading1/2/etc.).
void writePara(QXmlStreamWriter &w, const QString &text,
               const QString &style = QString())
{
    w.writeStartElement(QStringLiteral("w:p"));
    if (!style.isEmpty()) {
        w.writeStartElement(QStringLiteral("w:pPr"));
        w.writeStartElement(QStringLiteral("w:pStyle"));
        w.writeAttribute(QStringLiteral("w:val"), style);
        w.writeEndElement();
        w.writeEndElement();
    }
    if (!text.isEmpty()) {
        w.writeStartElement(QStringLiteral("w:r"));
        w.writeStartElement(QStringLiteral("w:t"));
        w.writeAttribute(QStringLiteral("xml:space"),
                          QStringLiteral("preserve"));
        w.writeCharacters(text);
        w.writeEndElement();
        w.writeEndElement();
    }
    w.writeEndElement();
}

} // namespace

bool AnnotationExporter::exportDocx(const QString &outputFile)
{
    if (outputFile.isEmpty()) return false;
    if (!m_manager) return false;
    const auto notes = collectNotes();

    // Read the brand icon up front so we can include it in the
    // relationships count + drop it into media/image1.png.
    QByteArray brandBytes;
    QSize brandSize;
    {
        const QString p = AnnotationExporter::brandIconPath();
        if (!p.isEmpty()) {
            QFile f(p);
            if (f.open(QIODevice::ReadOnly)) {
                brandBytes = f.readAll();
                f.close();
                QImage tmp = QImage::fromData(brandBytes);
                if (!tmp.isNull()) brandSize = tmp.size();
                else brandBytes.clear();
            }
        }
    }
    const bool hasBrand = !brandBytes.isEmpty();

    // Read all note PNGs + capture display sizes (EMU). Skip
    // notes whose image is missing rather than aborting — same
    // graceful-degrade behavior as the other exporters.
    struct DocxImage {
        QByteArray bytes;
        int        widthEmu  = 0;
        int        heightEmu = 0;
    };
    std::vector<DocxImage> noteImages;
    noteImages.reserve(notes.size());
    for (const auto &n : notes) {
        DocxImage di;
        if (!n.imagePath.isEmpty()) {
            QFile f(n.imagePath);
            if (f.open(QIODevice::ReadOnly)) {
                di.bytes = f.readAll();
                f.close();
                QImage tmp = QImage::fromData(di.bytes);
                if (!tmp.isNull() && tmp.width() > 0) {
                    const double aspect = double(tmp.height())
                                          / tmp.width();
                    di.widthEmu  = kImageDisplayWidthInches * kEmuPerInch;
                    di.heightEmu = static_cast<int>(di.widthEmu * aspect);
                } else {
                    di.bytes.clear();
                }
            }
        }
        noteImages.push_back(di);
    }

    // Build word/document.xml. Each note is heading2 + image +
    // body text + page break (sectPr-less; w:br type="page").
    const QByteArray documentXml = xmlString([&](QXmlStreamWriter &w) {
        w.writeStartElement(QStringLiteral("w:document"));
        w.writeAttribute(QStringLiteral("xmlns:w"),
            QStringLiteral("http://schemas.openxmlformats.org/"
                           "wordprocessingml/2006/main"));
        w.writeAttribute(QStringLiteral("xmlns:r"),
            QStringLiteral("http://schemas.openxmlformats.org/"
                           "officeDocument/2006/relationships"));
        w.writeAttribute(QStringLiteral("xmlns:wp"),
            QStringLiteral("http://schemas.openxmlformats.org/"
                           "drawingml/2006/wordprocessingDrawing"));
        w.writeStartElement(QStringLiteral("w:body"));

        int picId = 1;

        // Cover page: brand icon + title + meta.
        if (hasBrand && brandSize.isValid()) {
            // Display brand at 64×64 px regardless of source res.
            const int sizeEmu = 64 * (kEmuPerInch / 96);
            w.writeStartElement(QStringLiteral("w:p"));
            w.writeStartElement(QStringLiteral("w:r"));
            writeDrawing(w, QStringLiteral("rIdImg1"),
                          sizeEmu, sizeEmu, picId++);
            w.writeEndElement();
            w.writeEndElement();
        }
        writePara(w, m_mediaName.isEmpty() ? QStringLiteral("Notes")
                                              : m_mediaName,
                  QStringLiteral("Title"));
        writePara(w, QStringLiteral("Exported %1").arg(
            QDateTime::currentDateTime().toString(Qt::ISODate)));
        if (!m_mediaPath.isEmpty()) {
            writePara(w, QStringLiteral("Source: %1").arg(m_mediaPath));
        }
        writePara(w, QStringLiteral("%1 %2").arg(
            QString::number(notes.size()),
            notes.size() == 1 ? QStringLiteral("note")
                              : QStringLiteral("notes")));

        // One page per note.
        for (std::size_t i = 0; i < notes.size(); ++i) {
            const auto &n = notes[i];
            // Page break at the start of each note.
            w.writeStartElement(QStringLiteral("w:p"));
            w.writeStartElement(QStringLiteral("w:r"));
            w.writeStartElement(QStringLiteral("w:br"));
            w.writeAttribute(QStringLiteral("w:type"),
                              QStringLiteral("page"));
            w.writeEndElement();
            w.writeEndElement();
            w.writeEndElement();

            writePara(w,
                QStringLiteral("%1   frame %2%3").arg(
                    n.timecode,
                    QString::number(n.frame),
                    n.addressed ? QStringLiteral("   ✅ addressed")
                                : QString()),
                QStringLiteral("Heading2"));

            const DocxImage &di = noteImages[i];
            if (!di.bytes.isEmpty()) {
                const int relIdNum = (hasBrand ? 2 : 1)
                                   + static_cast<int>(i);
                w.writeStartElement(QStringLiteral("w:p"));
                w.writeStartElement(QStringLiteral("w:r"));
                writeDrawing(w,
                    QStringLiteral("rIdImg%1").arg(relIdNum),
                    di.widthEmu, di.heightEmu, picId++);
                w.writeEndElement();
                w.writeEndElement();
            }

            if (!n.text.trimmed().isEmpty()) {
                writePara(w, n.text);
            }
        }

        w.writeEndElement();   // w:body
        w.writeEndElement();   // w:document
    });

    // Compose the zip.
    QZipWriter zip(outputFile);
    zip.setCompressionPolicy(QZipWriter::AlwaysCompress);
    if (zip.status() != QZipWriter::NoError) {
        qWarning("AnnotationExporter: cannot open zip for write at %s",
                 qPrintable(outputFile));
        return false;
    }
    zip.addFile(QStringLiteral("[Content_Types].xml"), contentTypesXml());
    zip.addFile(QStringLiteral("_rels/.rels"),         packageRelsXml());
    zip.addFile(QStringLiteral("word/document.xml"),    documentXml);
    zip.addFile(QStringLiteral("word/_rels/document.xml.rels"),
                 documentRelsXml(static_cast<int>(notes.size()), hasBrand));
    int mediaIdx = 1;
    if (hasBrand) {
        zip.addFile(QStringLiteral("word/media/image%1.png").arg(mediaIdx++),
                     brandBytes);
    }
    for (const auto &di : noteImages) {
        if (di.bytes.isEmpty()) continue;
        zip.addFile(QStringLiteral("word/media/image%1.png").arg(mediaIdx++),
                     di.bytes);
    }
    zip.close();
    return zip.status() == QZipWriter::NoError;
}

} // namespace qcv
