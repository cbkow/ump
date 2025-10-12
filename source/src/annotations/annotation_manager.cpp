#include "annotation_manager.h"
#include "annotation_io.h"
#include "../utils/debug_utils.h"
#include <algorithm>
#include <filesystem>

namespace ump {

AnnotationManager::AnnotationManager() {
}

AnnotationManager::~AnnotationManager() {
}

void AnnotationManager::SetMediaPath(const std::string& media_path) {
    std::lock_guard<std::mutex> lock(notes_mutex_);
    current_media_path_ = media_path;
}

void AnnotationManager::LoadNotesForMedia(const std::string& media_path) {
    is_loading_ = true;

    // Set current media path
    SetMediaPath(media_path);

    // Load notes from disk
    std::vector<AnnotationNote> loaded_notes;
    bool success = AnnotationIO::LoadNotes(loaded_notes, media_path);

    if (success) {
        std::lock_guard<std::mutex> lock(notes_mutex_);
        notes_ = std::move(loaded_notes);
        SortNotesByTimecode();
        Debug::Log("Loaded " + std::to_string(notes_.size()) + " annotations for: " + media_path);
    } else {
        Debug::Log("Failed to load annotations for: " + media_path);
    }

    is_loading_ = false;
    NotifyNotesChanged();
}

void AnnotationManager::UnloadNotes() {
    std::lock_guard<std::mutex> lock(notes_mutex_);
    notes_.clear();
    current_media_path_.clear();
    NotifyNotesChanged();
}

void AnnotationManager::AddNote(double timestamp_seconds, const std::string& timecode, int frame, const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(notes_mutex_);

        // Generate image filename
        std::string image_filename = AnnotationIO::GenerateImageFilename(timecode);
        std::string image_path = "images/" + image_filename;

        // Create note
        AnnotationNote note(timecode, timestamp_seconds, frame, image_path, text);
        notes_.push_back(note);

        // Keep sorted by timecode
        SortNotesByTimecode();

        Debug::Log("Added annotation at timecode: " + timecode);
    }

    // Save to disk
    SaveNotesAsync();
    NotifyNotesChanged();
}

void AnnotationManager::UpdateNoteText(const std::string& timecode, const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(notes_mutex_);

        auto it = std::find_if(notes_.begin(), notes_.end(),
            [&timecode](const AnnotationNote& note) {
                return note.timecode == timecode;
            });

        if (it != notes_.end()) {
            it->text = text;
            Debug::Log("Updated annotation text at timecode: " + timecode);
        }
    }

    // Save to disk (unless in batch mode)
    if (!batch_mode_) {
        SaveNotesAsync();
        NotifyNotesChanged();
    }
}

void AnnotationManager::UpdateNoteAnnotationData(const std::string& timecode, const std::string& annotation_data) {
    {
        std::lock_guard<std::mutex> lock(notes_mutex_);

        auto it = std::find_if(notes_.begin(), notes_.end(),
            [&timecode](const AnnotationNote& note) {
                return note.timecode == timecode;
            });

        if (it != notes_.end()) {
            it->annotation_data = annotation_data;
            Debug::Log("Updated annotation data at timecode: " + timecode + " (" + std::to_string(annotation_data.length()) + " bytes)");
        }
    }

    // Save to disk (unless in batch mode)
    if (!batch_mode_) {
        SaveNotesAsync();
        NotifyNotesChanged();
    }
}

void AnnotationManager::UpdateNoteImagePath(const std::string& timecode, const std::string& image_path) {
    {
        std::lock_guard<std::mutex> lock(notes_mutex_);

        auto it = std::find_if(notes_.begin(), notes_.end(),
            [&timecode](const AnnotationNote& note) {
                return note.timecode == timecode;
            });

        if (it != notes_.end()) {
            it->image_path = image_path;
            Debug::Log("Updated image path at timecode: " + timecode + " -> " + image_path);
        }
    }

    // Save to disk (unless in batch mode)
    if (!batch_mode_) {
        SaveNotesAsync();
        NotifyNotesChanged();
    }
}

void AnnotationManager::DeleteNote(const std::string& timecode) {
    {
        std::lock_guard<std::mutex> lock(notes_mutex_);

        auto it = std::find_if(notes_.begin(), notes_.end(),
            [&timecode](const AnnotationNote& note) {
                return note.timecode == timecode;
            });

        if (it != notes_.end()) {
            // TODO: Delete screenshot file
            notes_.erase(it);
            Debug::Log("Deleted annotation at timecode: " + timecode);
        }
    }

    // Save to disk
    SaveNotesAsync();
    NotifyNotesChanged();
}

AnnotationNote* AnnotationManager::GetNoteAtTimecode(const std::string& timecode) {
    std::lock_guard<std::mutex> lock(notes_mutex_);

    auto it = std::find_if(notes_.begin(), notes_.end(),
        [&timecode](const AnnotationNote& note) {
            return note.timecode == timecode;
        });

    if (it != notes_.end()) {
        return &(*it);
    }

    return nullptr;
}

const AnnotationNote* AnnotationManager::GetNoteAtTimecode(const std::string& timecode) const {
    std::lock_guard<std::mutex> lock(notes_mutex_);

    auto it = std::find_if(notes_.begin(), notes_.end(),
        [&timecode](const AnnotationNote& note) {
            return note.timecode == timecode;
        });

    if (it != notes_.end()) {
        return &(*it);
    }

    return nullptr;
}

std::string AnnotationManager::GetImagesFolder() const {
    std::lock_guard<std::mutex> lock(notes_mutex_);
    return AnnotationIO::GetImagesFolder(current_media_path_);
}

std::string AnnotationManager::GetAnnotationsDirectory() const {
    std::lock_guard<std::mutex> lock(notes_mutex_);
    // Get the annotations folder path (parent of images folder)
    std::string images_folder = AnnotationIO::GetImagesFolder(current_media_path_);
    if (!images_folder.empty()) {
        std::filesystem::path images_path(images_folder);
        return images_path.parent_path().string();
    }
    return "";
}

void AnnotationManager::SortNotesByTimecode() {
    // Notes are already sorted by timecode via operator<
    std::sort(notes_.begin(), notes_.end());
}

void AnnotationManager::SaveNotesAsync() {
    is_saving_ = true;

    std::string media_path;
    std::vector<AnnotationNote> notes_copy;

    {
        std::lock_guard<std::mutex> lock(notes_mutex_);
        media_path = current_media_path_;
        notes_copy = notes_;
    }

    // Save (sync for now, will be async later)
    AnnotationIO::SaveNotes(notes_copy, media_path);

    is_saving_ = false;
}

void AnnotationManager::NotifyNotesChanged() {
    if (notes_changed_callback_) {
        notes_changed_callback_();
    }
}

void AnnotationManager::ForceSave() {
    SaveNotesAsync();
    NotifyNotesChanged();
}

} // namespace ump
