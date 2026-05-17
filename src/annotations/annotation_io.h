// AnnotationIO — direct lift from old QCView's
// src/annotations/annotation_io.{h,cpp} per Guide 19 §2.4.
//
// Folder layout + JSON read/write for note metadata. Two layouts:
//
//   1. Per-media (single clip):
//        <media_dir>/.qcview/<media_filename>/notes.json
//        <media_dir>/.qcview/<media_filename>/images/note_<TC>.png
//
//   2. Per-project-timeline (when notes belong to a timeline, not a
//      single clip):
//        <project_dir>/.qcview/<timeline_name>/notes.json
//        <project_dir>/.qcview/<timeline_name>/images/note_<TC>.png
//
// Read-side falls back to legacy `.ump` folder if `.qcview` doesn't
// exist (old "UMP" alpha used .ump). Write-side always creates
// `.qcview`.
//
// Adaptation notes vs old app:
//   - Namespace qcview::AnnotationIO → qcv::annotation_io
//   - std::string → QString throughout
//   - std::filesystem → QDir / QFile / QFileInfo
//   - Async via QtConcurrent::run (was std::thread::detach)
//   - SaveScreenshot dropped — Phase B annotation pass writes PNGs via
//     QImage::save (no separate stb_image_write dep needed)
//   - Per-Windows hidden-attr: keep (AttributesNormal | Hidden via
//     SetFileAttributesA, guarded by Q_OS_WIN)

#pragma once

#include "annotation_note.h"

#include <QString>

#include <functional>
#include <vector>

namespace qcv::annotation_io {

// ---- Per-media path helpers ----
QString getQcViewPath(const QString &mediaPath);
QString getNotesJsonPath(const QString &mediaPath);
QString getImagesFolder(const QString &mediaPath);
QString sanitizeMediaName(const QString &filename);
QString generateImageFilename(const QString &timecode);

// ---- Per-project-timeline path helpers ----
// `projectPath` is the path to the project file (e.g. "foo.qcvproj");
// the project dir is its parent. timelineName is sanitized.
QString getProjectAnnotationPath(const QString &projectPath,
                                 const QString &timelineName);
QString getProjectImagesFolder(const QString &projectPath,
                               const QString &timelineName);
bool createProjectQcViewFolder(const QString &projectPath,
                               const QString &timelineName);

// ---- Folder management ----
bool createQcViewFolder(const QString &mediaPath);
bool ensureImagesFolderExists(const QString &mediaPath);

// ---- JSON I/O (sync) ----
bool saveNotes(const std::vector<AnnotationNote> &notes,
               const QString &mediaPath);
bool loadNotes(std::vector<AnnotationNote> &notes,
               const QString &mediaPath);

// ---- Async wrappers (fire-and-forget, off the calling thread) ----
using LoadCallback =
    std::function<void(bool success, const std::vector<AnnotationNote> &)>;
void loadNotesAsync(const QString &mediaPath, LoadCallback callback);
void saveNotesAsync(const std::vector<AnnotationNote> &notes,
                    const QString &mediaPath);

} // namespace qcv::annotation_io
