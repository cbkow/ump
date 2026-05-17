// AnnotationManager — direct lift from old QCView's
// src/annotations/annotation_manager.{h,cpp} per Guide 19 §2.4.
//
// In-memory list of AnnotationNotes for the currently loaded media.
// Notes are kept sorted by timecode (string compare). All mutations
// trigger an async save to disk; loads run on a worker thread.
//
// Adaptation notes vs old app:
//   - QObject (was POD class with std::function callback)
//   - Q_SIGNALS notesChanged / loadingChanged / savingChanged
//     (was: NotesChangedCallback)
//   - QString throughout (was std::string)
//   - QMutex (was std::mutex)
//   - std::atomic stays (loading_, saving_, batch_mode_)

#pragma once

#include "annotation_note.h"

#include <QMutex>
#include <QObject>
#include <QString>

#include <atomic>
#include <vector>

namespace qcv {

class AnnotationManager : public QObject {
    Q_OBJECT

public:
    explicit AnnotationManager(QObject *parent = nullptr);
    ~AnnotationManager() override;

    // ---- Media lifecycle ----
    void setMediaPath(const QString &mediaPath);
    void loadNotesForMedia(const QString &mediaPath);
    void unloadNotes();
    void clearNotes();   // Drop notes without saving (used when switching to a
                         // disabled state where saving would clobber data).

    // ---- Note CRUD ----
    void addNote(double timestampSeconds,
                 const QString &timecode,
                 int frame,
                 const QString &text = QString());
    void updateNoteText(const QString &timecode, const QString &text);
    void updateNoteAnnotationData(const QString &timecode,
                                  const QString &annotationData);
    void updateNoteImagePath(const QString &timecode,
                             const QString &imagePath);
    void updateNoteAddressed(const QString &timecode, bool addressed);
    void deleteNote(const QString &timecode);

    // ---- Query ----
    std::vector<AnnotationNote> notes() const;          // copy
    AnnotationNote *noteAtTimecode(const QString &tc);
    const AnnotationNote *noteAtTimecode(const QString &tc) const;
    bool   hasNotes()  const;
    qsizetype noteCount() const;
    QString imagesFolder() const;
    QString annotationsDirectory() const;

    bool isLoading() const { return is_loading_.load(); }
    bool isSaving()  const { return is_saving_.load();  }

    // Batch mode skips the autosave on each update; call forceSave() when done.
    void setBatchMode(bool enabled) { batch_mode_.store(enabled); }
    void forceSave();

Q_SIGNALS:
    void notesChanged();
    void loadingChanged(bool loading);
    void savingChanged(bool saving);

private:
    void sortNotesByTimecode();
    void saveNotesAsyncLocked();   // assumes notes_mutex_ NOT held
    void notifyNotesChanged();

    mutable QMutex              notes_mutex_;
    std::vector<AnnotationNote> notes_;
    QString                     current_media_path_;

    std::atomic<bool> is_loading_{false};
    std::atomic<bool> is_saving_{false};
    std::atomic<bool> batch_mode_{false};
};

} // namespace qcv
