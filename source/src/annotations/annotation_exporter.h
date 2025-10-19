#pragma once

#include "annotation_note.h"
#include <string>
#include <vector>
#include <functional>

namespace ump {
namespace Annotations {

/**
 * AnnotationExporter - Exports annotation notes to various formats
 *
 * Supports exporting to:
 * - Markdown (folder with .md file and images/)
 * - HTML (standalone file with base64-embedded images)
 * - PDF (using libharu with embedded images)
 */
class AnnotationExporter {
public:
    enum class ExportFormat {
        MARKDOWN,
        HTML,
        PDF
    };

    struct ExportOptions {
        std::string media_name;           // Name of the media file
        std::string media_path;           // Full path to media file
        std::string output_directory;     // Where to save exported file(s)
        ExportFormat format;              // Export format
        double frame_rate = 24.0;         // For metadata
        double duration = 0.0;            // For metadata
        int width = 1920;                 // Video width for metadata
        int height = 1080;                // Video height for metadata
    };

    struct ExportProgress {
        int total_notes = 0;
        int current_note = 0;
        std::string current_operation;
        bool is_complete = false;
        bool has_error = false;
        std::string error_message;
    };

    AnnotationExporter();
    ~AnnotationExporter();

    /**
     * Callback for capturing a screenshot with annotation overlay
     * Parameters: timestamp, annotation_data, output_path
     * Returns: true if capture succeeded
     */
    using CaptureCallback = std::function<bool(
        double timestamp,
        const std::string& annotation_data,
        const std::string& output_path
    )>;
    void SetCaptureCallback(CaptureCallback callback);

    /**
     * Callback for progress updates during export
     */
    using ProgressCallback = std::function<void(const ExportProgress& progress)>;
    void SetProgressCallback(ProgressCallback callback);

    /**
     * Export notes to specified format
     * Returns: path to exported file/folder on success, empty string on failure
     */
    std::string ExportNotes(
        const std::vector<AnnotationNote>& notes,
        const ExportOptions& options
    );

    /**
     * Cancel ongoing export operation
     */
    void CancelExport();

    /**
     * Helper functions (public for state machine usage)
     */
    std::string GenerateTimestamp() const;
    std::string SanitizeFilename(const std::string& filename) const;
    static std::string FormatTimecode(double timestamp_seconds, double frame_rate);
    std::string FormatTimecode(const std::string& timecode) const;  // Keep for backward compatibility

private:
    // Format-specific export implementations
    std::string ExportMarkdown(const std::vector<AnnotationNote>& notes, const ExportOptions& options);
    std::string ExportHTML(const std::vector<AnnotationNote>& notes, const ExportOptions& options);
    std::string ExportPDF(const std::vector<AnnotationNote>& notes, const ExportOptions& options);
    std::string EncodeImageToBase64(const std::string& image_path) const;
    std::string GetGitHubMarkdownCSS() const;

    // Capture merged screenshot for a note
    bool CaptureNoteImage(const AnnotationNote& note, const std::string& output_path);

    // Progress tracking
    void UpdateProgress(int current, int total, const std::string& operation);
    void SetError(const std::string& error_message);

    CaptureCallback capture_callback_;
    ProgressCallback progress_callback_;
    ExportProgress current_progress_;
    bool cancel_requested_ = false;
};

} // namespace Annotations
} // namespace ump
