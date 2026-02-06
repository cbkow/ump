#pragma once

#include "../annotations/annotation_manager.h"
#include <string>
#include <functional>
#include <map>
#include <glad/gl.h>
#include <imgui.h>

namespace ump {

// Annotation availability states for different modes
enum class AnnotationAvailability {
    AVAILABLE,           // Normal operation
    NO_PROJECT_SAVED,    // Multi-track timeline but no project saved
    DUAL_VIEW_DISABLED,  // Dual view mode - annotations not supported
    PLAYLIST_DISABLED    // Playlist mode - annotations not supported
};

/**
 * AnnotationPanel - UI panel for displaying and editing annotations
 *
 * Right-side panel showing list of notes with thumbnails
 * Notes are displayed in timecode order
 * Each note is clickable to navigate to that frame
 */
class AnnotationPanel {
public:
    AnnotationPanel();
    ~AnnotationPanel();

    // Render the panel
    void Render(bool* p_open, ImVec4 accent_regular = ImVec4(0.3f, 0.6f, 1.0f, 1.0f), ImVec4 accent_muted_dark = ImVec4(0.5f, 0.5f, 0.5f, 1.0f));

    // Set the annotation manager
    void SetAnnotationManager(AnnotationManager* manager) { annotation_manager_ = manager; }

    // Callbacks for navigation
    using SeekToTimestampCallback = std::function<void(double timestamp)>;
    void SetSeekCallback(SeekToTimestampCallback callback) { seek_callback_ = callback; }

    // Callbacks for screenshot capture
    using CaptureScreenshotCallback = std::function<bool(const std::string& directory_path, const std::string& filename)>;
    void SetCaptureScreenshotCallback(CaptureScreenshotCallback callback) { capture_callback_ = callback; }

    // Callbacks for getting current video state
    using GetCurrentStateCallback = std::function<void(double& timestamp, std::string& timecode, int& frame)>;
    void SetGetCurrentStateCallback(GetCurrentStateCallback callback) { get_state_callback_ = callback; }

    // Callback for getting bright accent color
    using GetBrightAccentColorCallback = std::function<ImVec4()>;
    void SetGetBrightAccentColorCallback(GetBrightAccentColorCallback callback) { get_bright_accent_color_callback_ = callback; }

    // Callback for entering edit mode
    using EnterEditModeCallback = std::function<void(const std::string& timecode, double timestamp, int frame, const std::string& annotation_data)>;
    void SetEnterEditModeCallback(EnterEditModeCallback callback) { enter_edit_mode_callback_ = callback; }

    // Callback for exiting edit mode (saves and exits)
    using ExitEditModeCallback = std::function<void()>;
    void SetExitEditModeCallback(ExitEditModeCallback callback) { exit_edit_mode_callback_ = callback; }

    // Callback to check if a timecode is currently being edited
    using IsEditingCallback = std::function<bool(const std::string& timecode)>;
    void SetIsEditingCallback(IsEditingCallback callback) { is_editing_callback_ = callback; }

    // Callback for export
    using ExportCallback = std::function<void(const std::string& format)>;
    void SetExportCallback(ExportCallback callback) { export_callback_ = callback; }
    void TriggerExport(const std::string& format) {
        if (export_callback_) {
            export_callback_(format);
        }
    }

    // Callback for Frame.io import
    using FrameioImportCallback = std::function<void()>;
    void SetFrameioImportCallback(FrameioImportCallback callback) { frameio_import_callback_ = callback; }

    // Annotations enabled/disabled state
    void SetAnnotationsEnabled(bool* enabled_ptr) { annotations_enabled_ptr_ = enabled_ptr; }

    // Availability state (for disabled modes like DUAL_VIEW or unsaved project)
    void SetAvailability(AnnotationAvailability availability) { availability_ = availability; }
    AnnotationAvailability GetAvailability() const { return availability_; }

    // Video aspect ratio for proper thumbnail display
    void SetVideoAspectRatio(float aspect_ratio) { video_aspect_ratio_ = aspect_ratio; }
    float GetVideoAspectRatio() const { return video_aspect_ratio_; }

    // Selection state
    void SetSelectedNote(const std::string& timecode);
    const std::string& GetSelectedNote() const { return selected_timecode_; }

private:
    AnnotationManager* annotation_manager_;
    SeekToTimestampCallback seek_callback_;
    CaptureScreenshotCallback capture_callback_;
    GetCurrentStateCallback get_state_callback_;
    GetBrightAccentColorCallback get_bright_accent_color_callback_;
    EnterEditModeCallback enter_edit_mode_callback_;
    ExitEditModeCallback exit_edit_mode_callback_;
    IsEditingCallback is_editing_callback_;
    ExportCallback export_callback_;
    FrameioImportCallback frameio_import_callback_;

    std::string selected_timecode_;
    std::string edit_buffer_;
    std::string right_clicked_note_timecode_;  // Set when user right-clicks on a note
    bool is_editing_;
    bool* annotations_enabled_ptr_;
    float video_aspect_ratio_ = 16.0f / 9.0f;  // Default to 16:9, updated when media loads
    AnnotationAvailability availability_ = AnnotationAvailability::AVAILABLE;

    // UI helpers
    void RenderHeader();
    void RenderNotesList();
    void RenderFooter(ImVec4 accent_regular);
    void RenderNote(AnnotationNote& note);
    void HandleAddNote();
    void HandleDeleteNote(const std::string& timecode);

    // Thumbnail loading
    GLuint LoadThumbnail(const std::string& image_path);
    void CleanupThumbnails();

    // Thumbnail cache: image_path -> texture_id
    std::map<std::string, GLuint> thumbnail_cache_;
    // Thumbnail aspect ratio cache: image_path -> aspect_ratio (width/height)
    std::map<std::string, float> thumbnail_aspect_cache_;
};

} // namespace ump
