#pragma once

#include "annotation_note.h"
#include <vector>
#include <string>
#include <mutex>
#include <functional>

namespace ump {

/**
 * AnnotationManager - Manages annotations for video/image sequences
 *
 * Handles loading, saving, creating, and deleting notes
 * All notes are kept sorted by timecode
 * File I/O is async to avoid blocking playback
 */
class AnnotationManager {
public:
    AnnotationManager();
    ~AnnotationManager();

    // Media management
    void SetMediaPath(const std::string& media_path);
    void LoadNotesForMedia(const std::string& media_path);
    void UnloadNotes();

    // Note operations
    void AddNote(double timestamp_seconds, const std::string& timecode, int frame, const std::string& text = "");
    void UpdateNoteText(const std::string& timecode, const std::string& text);
    void UpdateNoteAnnotationData(const std::string& timecode, const std::string& annotation_data);
    void UpdateNoteImagePath(const std::string& timecode, const std::string& image_path);
    void DeleteNote(const std::string& timecode);

    // Query operations
    const std::vector<AnnotationNote>& GetNotes() const { return notes_; }
    AnnotationNote* GetNoteAtTimecode(const std::string& timecode);
    const AnnotationNote* GetNoteAtTimecode(const std::string& timecode) const;
    bool HasNotes() const { return !notes_.empty(); }
    size_t GetNoteCount() const { return notes_.size(); }
    std::string GetImagesFolder() const;
    std::string GetAnnotationsDirectory() const;

    // Observer pattern for UI updates
    using NotesChangedCallback = std::function<void()>;
    void SetNotesChangedCallback(NotesChangedCallback callback) { notes_changed_callback_ = callback; }

    // Status flags
    bool IsLoading() const { return is_loading_; }
    bool IsSaving() const { return is_saving_; }

    // Batch mode - Skip auto-save during bulk operations
    void SetBatchMode(bool enabled) { batch_mode_ = enabled; }
    void ForceSave();  // Force save when batch mode is done

private:
    std::vector<AnnotationNote> notes_;
    std::string current_media_path_;
    mutable std::mutex notes_mutex_;

    NotesChangedCallback notes_changed_callback_;

    std::atomic<bool> is_loading_{false};
    std::atomic<bool> is_saving_{false};
    std::atomic<bool> batch_mode_{false};  // Skip auto-save when true

    // Internal helpers
    void SortNotesByTimecode();
    void SaveNotesAsync();
    void NotifyNotesChanged();
};

} // namespace ump
