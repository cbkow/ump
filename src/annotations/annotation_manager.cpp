#include "annotation_manager.h"

#include "annotation_io.h"

#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>

#include <algorithm>

namespace qcv {

AnnotationManager::AnnotationManager(QObject *parent)
    : QObject(parent)
{
}

AnnotationManager::~AnnotationManager() = default;

void AnnotationManager::setMediaPath(const QString &mediaPath)
{
    QMutexLocker lk(&notes_mutex_);
    current_media_path_ = mediaPath;
}

void AnnotationManager::loadNotesForMedia(const QString &mediaPath)
{
    is_loading_.store(true);
    Q_EMIT loadingChanged(true);

    setMediaPath(mediaPath);

    std::vector<AnnotationNote> loaded;
    const bool ok = annotation_io::loadNotes(loaded, mediaPath);

    if (ok) {
        QMutexLocker lk(&notes_mutex_);
        notes_ = std::move(loaded);
        sortNotesByTimecode();
    }

    is_loading_.store(false);
    Q_EMIT loadingChanged(false);
    notifyNotesChanged();
}

void AnnotationManager::unloadNotes()
{
    {
        QMutexLocker lk(&notes_mutex_);
        notes_.clear();
        current_media_path_.clear();
    }
    notifyNotesChanged();
}

void AnnotationManager::clearNotes()
{
    {
        QMutexLocker lk(&notes_mutex_);
        notes_.clear();
        current_media_path_.clear();
    }
    notifyNotesChanged();
}

void AnnotationManager::addNote(double timestampSeconds,
                                const QString &timecode,
                                int frame,
                                const QString &text)
{
    {
        QMutexLocker lk(&notes_mutex_);
        const QString filename = annotation_io::generateImageFilename(timecode);
        const QString imagePath = QStringLiteral("images/") + filename;
        notes_.emplace_back(timecode, timestampSeconds, frame, imagePath, text);
        sortNotesByTimecode();
    }
    saveNotesAsyncLocked();
    notifyNotesChanged();
}

void AnnotationManager::updateNoteText(const QString &timecode,
                                       const QString &text)
{
    {
        QMutexLocker lk(&notes_mutex_);
        auto it = std::find_if(notes_.begin(), notes_.end(),
            [&](const AnnotationNote &n) { return n.timecode == timecode; });
        if (it != notes_.end()) it->text = text;
    }
    if (!batch_mode_.load()) {
        saveNotesAsyncLocked();
        notifyNotesChanged();
    }
}

void AnnotationManager::updateNoteAnnotationData(const QString &timecode,
                                                 const QString &annotationData)
{
    {
        QMutexLocker lk(&notes_mutex_);
        auto it = std::find_if(notes_.begin(), notes_.end(),
            [&](const AnnotationNote &n) { return n.timecode == timecode; });
        if (it != notes_.end()) it->annotation_data = annotationData;
    }
    if (!batch_mode_.load()) {
        saveNotesAsyncLocked();
        notifyNotesChanged();
    }
}

void AnnotationManager::updateNoteImagePath(const QString &timecode,
                                            const QString &imagePath)
{
    {
        QMutexLocker lk(&notes_mutex_);
        auto it = std::find_if(notes_.begin(), notes_.end(),
            [&](const AnnotationNote &n) { return n.timecode == timecode; });
        if (it != notes_.end()) it->image_path = imagePath;
    }
    if (!batch_mode_.load()) {
        saveNotesAsyncLocked();
        notifyNotesChanged();
    }
}

void AnnotationManager::updateNoteAddressed(const QString &timecode, bool addressed)
{
    {
        QMutexLocker lk(&notes_mutex_);
        auto it = std::find_if(notes_.begin(), notes_.end(),
            [&](const AnnotationNote &n) { return n.timecode == timecode; });
        if (it != notes_.end()) it->addressed = addressed;
    }
    if (!batch_mode_.load()) {
        saveNotesAsyncLocked();
        notifyNotesChanged();
    }
}

void AnnotationManager::deleteNote(const QString &timecode)
{
    {
        QMutexLocker lk(&notes_mutex_);
        auto it = std::find_if(notes_.begin(), notes_.end(),
            [&](const AnnotationNote &n) { return n.timecode == timecode; });
        // We deliberately do NOT delete the on-disk thumbnails
        // (note_<TC>.png + _annotated.png) here. This method is
        // called from BOTH the user-click delete path AND the
        // annotation-undo path (when undoing the stroke that
        // created a fresh note). The undo case must preserve the
        // images so a subsequent redo can restore them.
        //
        // PNG file deletion is intentionally tied to an explicit
        // user click only — handled in
        // WindowManager::deleteAnnotationNote, which fires this
        // method after wiping the files.
        if (it != notes_.end()) notes_.erase(it);
    }
    saveNotesAsyncLocked();
    notifyNotesChanged();
}

std::vector<AnnotationNote> AnnotationManager::notes() const
{
    QMutexLocker lk(&notes_mutex_);
    return notes_;   // copy
}

AnnotationNote *AnnotationManager::noteAtTimecode(const QString &tc)
{
    QMutexLocker lk(&notes_mutex_);
    auto it = std::find_if(notes_.begin(), notes_.end(),
        [&](const AnnotationNote &n) { return n.timecode == tc; });
    return it == notes_.end() ? nullptr : &(*it);
}

const AnnotationNote *AnnotationManager::noteAtTimecode(const QString &tc) const
{
    QMutexLocker lk(&notes_mutex_);
    auto it = std::find_if(notes_.cbegin(), notes_.cend(),
        [&](const AnnotationNote &n) { return n.timecode == tc; });
    return it == notes_.cend() ? nullptr : &(*it);
}

bool AnnotationManager::hasNotes() const
{
    QMutexLocker lk(&notes_mutex_);
    return !notes_.empty();
}

qsizetype AnnotationManager::noteCount() const
{
    QMutexLocker lk(&notes_mutex_);
    return static_cast<qsizetype>(notes_.size());
}

QString AnnotationManager::imagesFolder() const
{
    QMutexLocker lk(&notes_mutex_);
    return annotation_io::getImagesFolder(current_media_path_);
}

QString AnnotationManager::annotationsDirectory() const
{
    QMutexLocker lk(&notes_mutex_);
    const QString images = annotation_io::getImagesFolder(current_media_path_);
    if (images.isEmpty()) return {};
    return QFileInfo(images).absolutePath();
}

void AnnotationManager::forceSave()
{
    saveNotesAsyncLocked();
    notifyNotesChanged();
}

void AnnotationManager::sortNotesByTimecode()
{
    // Caller holds notes_mutex_.
    std::sort(notes_.begin(), notes_.end());
}

void AnnotationManager::saveNotesAsyncLocked()
{
    is_saving_.store(true);
    Q_EMIT savingChanged(true);

    QString mediaPath;
    std::vector<AnnotationNote> copy;
    {
        QMutexLocker lk(&notes_mutex_);
        mediaPath = current_media_path_;
        copy      = notes_;
    }

    // annotation_io::saveNotesAsync already runs off-thread via
    // QtConcurrent. Fire-and-forget — we don't observe completion.
    annotation_io::saveNotesAsync(copy, mediaPath);

    is_saving_.store(false);
    Q_EMIT savingChanged(false);
}

void AnnotationManager::notifyNotesChanged()
{
    Q_EMIT notesChanged();
}

} // namespace qcv
