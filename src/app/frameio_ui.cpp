// frameio_ui.cpp — Frame.io token management + thumbnail generation

#include "app/application.h"
#include "annotations/annotation_manager.h"
#include "utils/debug_utils.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

    bool Application::HasSavedFrameioToken() {
        // Check if token exists in frameio_import_state (loaded from settings)
        return strlen(frameio_import_state.token_buffer) > 0;
    }

    void Application::ClearSavedToken() {
        // Clear the token buffer
        frameio_import_state.token_buffer[0] = '\0';
        // Save settings to persist the cleared state
        SaveSettings();
        Debug::Log("Cleared saved Frame.io token from settings");
    }

    // Process Frame.io thumbnail generation (called once per frame)
    void Application::ProcessFrameioThumbnailGeneration() {
        if (!frameio_import_state.generating_thumbnails) return;

        // Wait for seek to complete
        if (frameio_import_state.frames_to_wait_after_seek > 0) {
            frameio_import_state.frames_to_wait_after_seek--;
            return;
        }

        // Capture thumbnail after seek completes
        if (frameio_import_state.waiting_for_seek) {
            frameio_import_state.waiting_for_seek = false;

            // Capture screenshot for current note
            if (frameio_import_state.current_thumbnail_index < frameio_import_state.imported_notes.size()) {
                auto& note = frameio_import_state.imported_notes[frameio_import_state.current_thumbnail_index];

                // Generate filename based on timecode
                std::string filename = "note_" + note.timecode + ".png";
                // Replace colons with underscores for valid filename
                std::replace(filename.begin(), filename.end(), ':', '_');

                // Get the annotations directory for the current project
                if (annotation_manager) {
                    std::filesystem::path annotations_dir = annotation_manager->GetAnnotationsDirectory();
                    std::filesystem::path images_dir = annotations_dir / "images";

                    // Ensure images directory exists
                    std::filesystem::create_directories(images_dir);

                    std::filesystem::path full_path = images_dir / filename;

                    // Capture screenshot
                    if (video_player && video_player->CaptureScreenshotToPath(images_dir.string(), filename)) {
                        // Wait for file to be written to disk (with timeout)
                        int wait_attempts = 0;
                        const int max_attempts = 10;  // 10 frames max
                        while (wait_attempts < max_attempts && !std::filesystem::exists(full_path)) {
                            wait_attempts++;
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }

                        // Verify file exists before updating
                        if (std::filesystem::exists(full_path)) {
                            std::string relative_path = "images/" + filename;
                            annotation_manager->UpdateNoteImagePath(note.timecode, relative_path);
                            Debug::Log("Generated thumbnail for " + note.timecode + ": " + relative_path);
                        } else {
                        // Playback controller already exists - reload dummy video and re-enable timeline mode
                            Debug::Log("Warning: Thumbnail file not found after capture: " + full_path.string());
                        }
                    }
                }

                frameio_import_state.current_thumbnail_index++;
            }

            // Check if done
            if (frameio_import_state.current_thumbnail_index >= frameio_import_state.imported_notes.size()) {
                Debug::Log("Finished generating thumbnails for imported notes");

                // Disable batch mode and do final save (single save instead of one per thumbnail)
                if (annotation_manager) {
                    annotation_manager->SetBatchMode(false);
                    annotation_manager->ForceSave();
                    Debug::Log("Disabled batch mode and saved all thumbnail paths");
                }

                frameio_import_state.generating_thumbnails = false;
                frameio_import_state.imported_notes.clear();
                frameio_import_state.current_thumbnail_index = 0;

                // Update status message in dialog
                frameio_import_state.status_message += "\nThumbnails generated successfully!";
                return;
            }

            // Update status
            frameio_import_state.status_message = "Generating thumbnails... (" +
                std::to_string(frameio_import_state.current_thumbnail_index) + "/" +
                std::to_string(frameio_import_state.imported_notes.size()) + ")";

            // Fall through to process next note
        }

        // Process next note
        if (frameio_import_state.current_thumbnail_index < frameio_import_state.imported_notes.size()) {
            const auto& note = frameio_import_state.imported_notes[frameio_import_state.current_thumbnail_index];

            // Seek to the timestamp
            if (video_player) {
                double current_pos = video_player->GetPosition();
                const double epsilon = 0.001;

                if (std::abs(current_pos - note.timestamp_seconds) > epsilon) {
                    video_player->Seek(note.timestamp_seconds);
                    frameio_import_state.frames_to_wait_after_seek = 3;
                    frameio_import_state.waiting_for_seek = true;
                } else {
                    // Already at position
                    frameio_import_state.frames_to_wait_after_seek = 1;
                    frameio_import_state.waiting_for_seek = true;
                }
            }
        }
    }
