// AnnotationExporter — Phase 3.H.6 Stage D.
//
// Format-agnostic walker that turns the in-memory note list into
// a Markdown folder, a self-contained HTML file, or a PDF. DOCX
// joins the family in a follow-up via QXmlStreamWriter + miniz.
//
// Usage:
//   AnnotationExporter exp;
//   exp.setManager(manager);
//   exp.setMediaName("CAN_LL15_v001");
//   exp.setMediaPath("/.../CAN_LL15_v001.mov");
//   exp.exportPdf("/Users/.../notes.pdf");
//
// The exporter doesn't capture screenshots — it relies on the
// already-on-disk note thumbnails (note_<TC>.png + the optional
// _annotated.png sibling) that AnnotationManager + WindowManager
// produce on note creation / stroke release. Annotated thumbs are
// preferred when present so exported review docs match what the
// user saw at QC time.

#pragma once

#include <QObject>
#include <QString>

#include <vector>

namespace qcv {

class AnnotationManager;

class AnnotationExporter : public QObject {
    Q_OBJECT

public:
    explicit AnnotationExporter(QObject *parent = nullptr);

    void setManager(AnnotationManager *m)         { m_manager   = m; }
    void setMediaName(const QString &n)           { m_mediaName = n; }
    void setMediaPath(const QString &p)           { m_mediaPath = p; }

    // Each returns true on success. Markdown produces a folder
    // (outputDir/{mediaName}-YYYYMMDD-HHMMSS/) with notes.md +
    // images/. HTML and PDF write a single self-contained file.
    // outputBaseName overrides the auto-derived "{mediaName}-{stamp}"
    // for the markdown folder + .md filename. When empty, the
    // exporter falls back to the media-name + timestamp pattern.
    bool exportMarkdown(const QString &outputDir,
                        const QString &outputBaseName = QString());
    bool exportHtml(const QString &outputFile);
    bool exportPdf(const QString &outputFile);
    // Office Open XML (.docx). Hand-rolled OOXML zip via QZipWriter;
    // each note is a Heading2 paragraph + image + body text. Opens
    // cleanly in Microsoft Word, Pages, and Google Docs.
    bool exportDocx(const QString &outputFile);

    // Resolves the bundled branding icon used at the top of every
    // export doc. macOS app bundle → out-of-tree build → in-tree
    // src/assets — same fallback chain as the safety SVG resolver.
    static QString brandIconPath();

private:
    // One row per note, materialized once and reused by every
    // writer so the on-disk read happens exactly once.
    struct ExportNote {
        QString timecode;
        int     frame             = 0;
        double  timestampSeconds  = 0;
        QString text;
        bool    addressed         = false;
        // absolute path on disk; preferred image is the annotated
        // sibling when present, falling back to the clean PNG.
        QString imagePath;
        int     imageWidth        = 0;
        int     imageHeight       = 0;
    };
    std::vector<ExportNote> collectNotes() const;

    // Common HTML body building block — used by both the HTML
    // and PDF paths (QTextDocument's print path takes HTML).
    // imageEmbedMode:
    //   "base64" — inline data URIs (HTML standalone path)
    //   "file"   — file:// URLs (PDF path; QTextDocument loads
    //              the PNG via QImage and bakes it into the
    //              page itself)
    QString buildHtmlBody(const std::vector<ExportNote> &notes,
                          const QString &imageEmbedMode) const;

    AnnotationManager *m_manager = nullptr;
    QString m_mediaName;
    QString m_mediaPath;
};

} // namespace qcv
