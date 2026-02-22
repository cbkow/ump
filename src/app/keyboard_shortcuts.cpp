// ============================================================================
// Keyboard shortcut handling
// ============================================================================

#include "app/application.h"
#include "app/shortcut_manager.h"
#include "project/project_manager.h"
#include "utils/debug_utils.h"
#include "annotations/annotation_serializer.h"
#include "annotations/annotation_manager.h"
#include "ui/annotation_panel.h"
#include "timeline/timeline_playback_controller.h"
#include "timeline/timeline_view.h"
#include "ui/timeline_manager.h"
#include "nodes/node_manager.h"
#include "audio/audio_mixer.h"
#include <imgui.h>
#include <imnodes.h>
#include <GLFW/glfw3.h>
#include <chrono>
#include <string>

// Globals defined in main.cpp
struct ScreenshotFlashState {
    std::chrono::steady_clock::time_point clipboard_flash_start;
    std::chrono::steady_clock::time_point desktop_flash_start;
    bool clipboard_flashing = false;
    bool desktop_flashing = false;
    static constexpr float FLASH_DURATION_MS = 400.0f;
};
extern ScreenshotFlashState g_screenshot_flash;
extern std::unique_ptr<qcview::TimelineView> timeline_view;

    void Application::HandleKeyboardShortcuts() {
        ImGuiIO& io = ImGui::GetIO();

        // Don't process shortcuts when typing in text fields
        if (io.WantTextInput) return;

        // Fullscreen toggle
        if (IsShortcutPressed("fullscreen", false)) {
            pending_fullscreen_toggle = true;
        }

        // ==============================================
        // PROJECT PANEL SHORTCUTS
        // ==============================================

        // Delete key - Remove selected items from project OR delete nodes/connections
        // NOTE: Timeline clip deletion is handled separately in the OTIO timeline section
        // to avoid conflicts when both timeline clips and project items are selected
        if (IsShortcutPressed("delete_item")) {
            // Check if timeline has selected clips - if so, skip project deletion here
            // Timeline deletion is handled in the OTIO timeline keyboard section
            bool timeline_has_selection = timeline_view && timeline_view->GetSelection().HasSelection();

            // First check if we're in color panels mode and have nodes/connections selected
            if (show_color_panels && node_manager) {
                bool handled_node_deletion = false;

                // Delete selected nodes
                if (node_manager->HasSelectedNode()) {
                    node_manager->DeleteSelectedNodes();
                    Debug::Log("Delete key: Deleted selected nodes");
                    UpdateColorPipeline();
                    handled_node_deletion = true;
                }

                // Delete selected connections
                int num_selected_links = ImNodes::NumSelectedLinks();
                if (num_selected_links > 0) {
                    std::vector<int> selected_links(num_selected_links);
                    ImNodes::GetSelectedLinks(selected_links.data());

                    for (int link_id : selected_links) {
                        node_manager->DeleteConnection(link_id);
                    }

                    Debug::Log("Delete key: Deleted " + std::to_string(num_selected_links) + " selected connections");
                    UpdateColorPipeline();
                    handled_node_deletion = true;
                }

                // If we didn't handle node deletion, fall back to project deletion
                // BUT only if timeline doesn't have selected clips
                if (!handled_node_deletion && !timeline_has_selection &&
                    project_manager && project_manager->HasSelectedItems()) {
                    project_manager->DeleteSelectedItems();
                    Debug::Log("Delete key: Removed selected items from project");
                }
            }
            // If not in color panels mode, handle project deletion
            // BUT only if timeline doesn't have selected clips
            else if (!timeline_has_selection && project_manager && project_manager->HasSelectedItems()) {
                project_manager->DeleteSelectedItems();
                Debug::Log("Delete key: Removed selected items from project");
            }
        }

        // X key - Alternative delete for nodes/connections (only in color panels mode)
        if (IsShortcutPressed("delete_node")) {
            if (show_color_panels && node_manager) {
                // Delete selected nodes
                if (node_manager->HasSelectedNode()) {
                    node_manager->DeleteSelectedNodes();
                    Debug::Log("X key: Deleted selected nodes");
                    UpdateColorPipeline();
                }

                // Delete selected connections
                int num_selected_links = ImNodes::NumSelectedLinks();
                if (num_selected_links > 0) {
                    std::vector<int> selected_links(num_selected_links);
                    ImNodes::GetSelectedLinks(selected_links.data());

                    for (int link_id : selected_links) {
                        node_manager->DeleteConnection(link_id);
                    }

                    Debug::Log("X key: Deleted " + std::to_string(num_selected_links) + " selected connections");
                    UpdateColorPipeline();
                }
            }
        }

        // F2 key - Rename (if single item selected)
        if (IsShortcutPressed("rename_item")) {
            if (project_manager && project_manager->GetSelectedItemsCount() == 1) {
                project_manager->StartRenamingSelected();
                Debug::Log("F2 key: Start renaming selected item");
            }
        }

        // ==============================================
        // PLAYBACK CONTROLS
        // ==============================================

        // Play/Pause (Space/W - no repeat to prevent multiple triggers per keypress)
        if (IsShortcutPressed("play_pause", false)) {
            bool is_playing = false;
            if (timeline_view && timeline_view->HasPlaybackController()) {
                auto* effective_ctrl = timeline_view->GetEffectivePlaybackController();
                is_playing = effective_ctrl && effective_ctrl->IsActuallyPlaying();
            } else if (video_player) {
                is_playing = video_player->IsPlaying();
            }

            Debug::Log("Keyboard Play/Pause pressed, is_playing=" + std::to_string(is_playing));

            if (is_playing) {
                video_player->Pause();
                if (project_manager) {
                    project_manager->NotifyPlaybackState(false);
                }
            } else {
                AutoSaveAnnotationOnSeek();
                video_player->Play();
                if (project_manager) {
                    project_manager->NotifyPlaybackState(true);
                }
            }
        }

        // Toggle Loop
        if (IsShortcutPressed("toggle_loop")) {
            ToggleLoop();
        }

        // Quick Background Cycling
        if (IsShortcutPressed("cycle_background")) {
            // Cycle through backgrounds in same order as popup menu
            static const VideoBackgroundType cycle_order[] = {
                VideoBackgroundType::BLACK,
                VideoBackgroundType::DEFAULT,
                VideoBackgroundType::DARK_CHECKERBOARD,
                VideoBackgroundType::LIGHT_CHECKERBOARD
            };

            // Find current position in cycle
            int current_index = 0;
            for (int i = 0; i < 4; i++) {
                if (cycle_order[i] == video_background_type) {
                    current_index = i;
                    break;
                }
            }

            // Move to next in cycle
            current_index = (current_index + 1) % 4;
            video_background_type = cycle_order[current_index];

            const char* names[] = { "Black", "None", "Dark Checkerboard", "Light Checkerboard" };
            Debug::Log("Background: Quick-switched to " + std::string(names[current_index]));
        }

        // Set In Point
        if (IsShortcutPressed("set_in_point")) {
            if (video_player && project_manager) {
                double current_time = video_player->GetPosition();
                double current_in = project_manager->GetInPoint();

                // Toggle behavior: If In point already at this position (within 0.01s), clear it
                if (current_in >= 0 && std::abs(current_in - current_time) < 0.01) {
                    project_manager->SetInPoint(-1.0);
                    Debug::Log("I - Cleared In point");
                } else {
                    project_manager->SetInPoint(current_time);
                    Debug::Log("I - Set In point at " + std::to_string(current_time) + "s");

                    // Auto-clear Out point if it's before new In point
                    double current_out = project_manager->GetOutPoint();
                    if (current_out >= 0 && current_out < current_time) {
                        project_manager->SetOutPoint(-1.0);
                        Debug::Log("Auto-cleared Out point (was before In point)");
                    }
                }
            }
        }

        // Set Out Point
        if (IsShortcutPressed("set_out_point")) {
            if (video_player && project_manager) {
                double current_time = video_player->GetPosition();
                double current_out = project_manager->GetOutPoint();

                // Toggle behavior: If Out point already at this position (within 0.01s), clear it
                if (current_out >= 0 && std::abs(current_out - current_time) < 0.01) {
                    project_manager->SetOutPoint(-1.0);
                    Debug::Log("O - Cleared Out point");
                } else {
                    project_manager->SetOutPoint(current_time);
                    Debug::Log("O - Set Out point at " + std::to_string(current_time) + "s");

                    // Auto-clear In point if it's after new Out point
                    double current_in = project_manager->GetInPoint();
                    if (current_in >= 0 && current_in > current_time) {
                        project_manager->SetInPoint(-1.0);
                        Debug::Log("Auto-cleared In point (was after Out point)");
                    }
                }
            }
        }

        // ==============================================
        // FRAME STEPPING
        // ==============================================

        // Frame Back
        if (IsShortcutPressed("frame_back")) {
            AutoSaveAnnotationOnSeek();
            video_player->StepFrame(-1);
            timeline_manager->ScheduleFrameStepSync();
            Debug::Log("Frame back");
        }

        // Frame Forward
        if (IsShortcutPressed("frame_forward")) {
            AutoSaveAnnotationOnSeek();
            video_player->StepFrame(1);
            timeline_manager->ScheduleFrameStepSync();
            Debug::Log("Frame forward");
        }

        // ==============================================
        // FAST SEEK CONTROLS
        // ==============================================

        // Rewind (held for continuous)
        bool a_pressed = IsShortcutDown("rewind");
        if (a_pressed && !rewind_a_held) {
            AutoSaveAnnotationOnSeek();
            rewind_a_held = true;
            video_player->StartRewind();
            Debug::Log("Start rewind");
        }
        else if (!a_pressed && rewind_a_held) {
            rewind_a_held = false;
            video_player->StopFastSeek();
            Debug::Log("Stop rewind");
        }

        // Fast Forward (held for continuous)
        bool d_pressed = IsShortcutDown("fast_forward");
        if (d_pressed && !fastforward_d_held) {
            AutoSaveAnnotationOnSeek();
            fastforward_d_held = true;
            video_player->StartFastForward();
            Debug::Log("Start fast forward");
        }
        else if (!d_pressed && fastforward_d_held) {
            fastforward_d_held = false;
            video_player->StopFastSeek();
            Debug::Log("Stop fast forward");
        }

        // ==============================================
        // VOLUME CONTROLS
        // ==============================================

        // Volume Up
        if (IsShortcutPressed("volume_up")) {
            int new_volume = current_volume + 5;
            if (new_volume > 100) new_volume = 100;
            current_volume = new_volume;
            // Set timeline audio mixer volume
            if (timeline_view) {
                if (auto* ctrl = timeline_view->GetEffectivePlaybackController()) {
                    if (auto* mixer = ctrl->GetAudioMixer()) {
                        mixer->SetVolume(new_volume / 100.0f);
                    }
                }
            }
            Debug::Log("Volume Up: " + std::to_string(new_volume) + "%");
        }

        // Volume Down
        if (IsShortcutPressed("volume_down")) {
            int new_volume = current_volume - 5;
            if (new_volume < 0) new_volume = 0;
            current_volume = new_volume;
            // Set timeline audio mixer volume
            if (timeline_view) {
                if (auto* ctrl = timeline_view->GetEffectivePlaybackController()) {
                    if (auto* mixer = ctrl->GetAudioMixer()) {
                        mixer->SetVolume(new_volume / 100.0f);
                    }
                }
            }
            Debug::Log("Volume Down: " + std::to_string(new_volume) + "%");
        }

        // Toggle Mute
        if (IsShortcutPressed("toggle_mute")) {
            ToggleMute();
        }

        // ==============================================
        // VIEW PANEL TOGGLES
        // ==============================================

        // Project Panel
        if (IsShortcutPressed("toggle_project")) {
            show_project_panel = !show_project_panel;
            if (show_project_panel) minimal_view_mode = false;
            Debug::Log("Toggle Project Panel: " + std::string(show_project_panel ? "ON" : "OFF"));
        }

        // Inspector Panel
        if (IsShortcutPressed("toggle_inspector")) {
            show_inspector_panel = !show_inspector_panel;
            if (show_inspector_panel) minimal_view_mode = false;
            Debug::Log("Toggle Inspector Panel: " + std::string(show_inspector_panel ? "ON" : "OFF"));
        }

        // Timeline & Transport Panel
        if (IsShortcutPressed("toggle_timeline")) {
            show_timeline_panel = !show_timeline_panel;
            if (show_timeline_panel) minimal_view_mode = false;
            first_time_setup = true;
            Debug::Log("Toggle Timeline Panel: " + std::string(show_timeline_panel ? "ON" : "OFF"));
        }

        // Annotations Panel
        if (IsShortcutPressed("toggle_annotations")) {
            show_annotation_panel = !show_annotation_panel;
            if (show_annotation_panel) minimal_view_mode = false;
            Debug::Log("Toggle Annotations Panel: " + std::string(show_annotation_panel ? "ON" : "OFF"));
        }

        // Annotation Toolbar
        if (IsShortcutPressed("toggle_ann_toolbar")) {
            show_annotation_toolbar = !show_annotation_toolbar;
            annotation_toolbar->SetVisible(show_annotation_toolbar);
            Debug::Log("Toggle Annotation Toolbar: " + std::string(show_annotation_toolbar ? "ON" : "OFF"));
        }

        // Sidebar Panel
        if (IsShortcutPressed("toggle_sidebar")) {
            show_sidebar_panel = !show_sidebar_panel;
            first_time_setup = true;
            Debug::Log("Toggle Sidebar Panel: " + std::string(show_sidebar_panel ? "ON" : "OFF"));
        }

        // New Playlist
        if (IsShortcutPressed("new_playlist")) {
            if (project_manager) {
                project_manager->ShowNewPlaylistDialog();
                Debug::Log("New Playlist dialog");
            }
        }

        // Transcode Queue Window
        if (IsShortcutPressed("transcode_window")) {
            if (project_manager) {
                project_manager->ToggleTranscodeQueueWindow();
                Debug::Log("Toggle Transcode Queue Window: " +
                          std::string(project_manager->IsTranscodeQueueWindowOpen() ? "ON" : "OFF"));
            }
        }

        // Add Selected to Transcode Queue
        if (IsShortcutPressed("add_to_transcode")) {
            if (project_manager && project_manager->HasSelectedItems()) {
                project_manager->AddSelectedItemsToTranscodeQueue();
                Debug::Log("Add selected items to transcode queue");
            }
        }

        // Escape - Exit fullscreen mode (if in fullscreen)
        // Note: ESC only exits fullscreen, it does NOT enter fullscreen (use F for that)
        if (IsShortcutPressed("escape_action") && is_fullscreen) {
            Debug::Log("Escape: Exiting fullscreen mode");
            ToggleFullscreen();
        }
        // Escape - Cancel annotation mode (if active and not in fullscreen, and not in edit modal)
        else if (IsShortcutPressed("escape_action") && viewport_annotator && viewport_annotator->IsAnnotationMode()
                 && !(annotation_panel && annotation_panel->IsEditModalOpen())) {
            Debug::Log("Escape: Canceling annotation mode");
            viewport_annotator->ClearActiveStroke();
            current_annotation_strokes_.clear();
            current_editing_timecode_.clear();
            viewport_annotator->SetMode(qcview::Annotations::ViewportMode::PLAYBACK);
            if (annotation_toolbar) {
                annotation_toolbar->SetVisible(false);
            }
        }

        // Enter - Done with annotation (if in annotation mode, not in edit modal)
        if (IsShortcutPressed("annotation_save") && viewport_annotator && viewport_annotator->IsAnnotationMode()
            && !(annotation_panel && annotation_panel->IsEditModalOpen())) {
            Debug::Log("Enter: Saving annotation and exiting mode");

            // Finalize any active stroke being drawn
            auto active_stroke = viewport_annotator->FinalizeStroke();
            if (active_stroke) {
                current_annotation_strokes_.push_back(*active_stroke);
                Debug::Log("Added final stroke to annotation");
            }

            // Serialize all strokes to JSON
            std::string json_data = qcview::Annotations::AnnotationSerializer::StrokesToJsonString(current_annotation_strokes_);

            // Save to annotation manager
            if (annotation_manager && !current_editing_timecode_.empty()) {
                annotation_manager->UpdateNoteAnnotationData(current_editing_timecode_, json_data);
                Debug::Log("Saved " + std::to_string(current_annotation_strokes_.size()) + " strokes to annotation");
            }

            // Clear editing state
            current_annotation_strokes_.clear();
            current_editing_timecode_.clear();

            viewport_annotator->SetMode(qcview::Annotations::ViewportMode::PLAYBACK);
            if (annotation_toolbar) {
                annotation_toolbar->SetVisible(false);
            }
        }

        // Default View
        if (IsShortcutPressed("default_view")) {
            minimal_view_mode = false;
            SetDefaultView();
        }

        // Show All Panels
        if (IsShortcutPressed("show_all_panels")) {
            minimal_view_mode = false;
            ShowAllPanels();
        }

        // Toggle Color Panels
        if (IsShortcutPressed("toggle_color")) {
            show_color_panels = !show_color_panels;
            first_time_setup = true;
        }

        // ==============================================
        // VIEW TOGGLES
        // ==============================================

        // Toggle Minimal View (keeps sidebar visible)
        if (IsShortcutPressed("toggle_minimal")) {
            minimal_view_mode = !minimal_view_mode;
            if (minimal_view_mode) {
                // Save current state before entering minimal view
                saved_show_project_panel = show_project_panel;
                saved_show_inspector_panel = show_inspector_panel;
                saved_show_color_panels = show_color_panels;
                saved_show_annotation_panel = show_annotation_panel;
                saved_show_annotation_toolbar = show_annotation_toolbar;

                // Enable minimal view - only video, timeline, and sidebar
                show_project_panel = false;
                show_inspector_panel = false;
                show_timeline_panel = true;
                show_color_panels = false;
                show_annotation_panel = false;
                show_annotation_toolbar = false;
                if (annotation_toolbar) annotation_toolbar->SetVisible(false);

                first_time_setup = true;
                Debug::Log("Minimal View: ON");
            }
            else {
                // Restore previous state when exiting minimal view
                show_project_panel = saved_show_project_panel;
                show_inspector_panel = saved_show_inspector_panel;
                show_timeline_panel = true;
                show_color_panels = saved_show_color_panels;
                show_annotation_panel = saved_show_annotation_panel;
                show_annotation_toolbar = saved_show_annotation_toolbar;
                if (annotation_toolbar) annotation_toolbar->SetVisible(saved_show_annotation_toolbar);

                first_time_setup = true;
                Debug::Log("Minimal View: OFF");
            }
        }

        // Toggle Colorspace Presets Panel
        if (IsShortcutPressed("toggle_colorspace")) {
            show_colorspace_panel = !show_colorspace_panel;
            Debug::Log("Toggle colorspace presets panel");
        }

        // Toggle Safety Overlay Panel
        if (IsShortcutPressed("toggle_safety")) {
            show_safety_overlay_panel = !show_safety_overlay_panel;
            Debug::Log("Toggle safety overlay panel");
        }

        // Toggle Background Panel
        if (IsShortcutPressed("toggle_bg_panel")) {
            show_background_panel = !show_background_panel;
            Debug::Log("Toggle background panel");
        }

        // ==============================================
        // FILE OPERATIONS
        // ==============================================

        // Open File
        if (IsShortcutPressed("open_file")) {
            OpenFileDialog();
            Debug::Log("Open file dialog");
        }

        // Save Project As (check before Save to avoid both firing)
        if (IsShortcutPressed("save_project_as")) {
            if (project_manager) {
                project_manager->SaveProjectAs();
                Debug::Log("Save project as");
            }
        }
        // Save Project
        else if (IsShortcutPressed("save_project")) {
            if (project_manager) {
                project_manager->SaveProject();
                Debug::Log("Save project");
            }
        }

        // Open Project
        if (IsShortcutPressed("open_project")) {
            if (project_manager) {
                project_manager->LoadProject();
                Debug::Log("Open project dialog");
                // Show project panels for context
                show_project_panel = true;
                show_inspector_panel = true;
            }
        }

        // Reset Layout
        if (IsShortcutPressed("reset_layout")) {
            first_time_setup = true;
            Debug::Log("Reset layout");
        }

        // ==============================================
        // SCREENSHOT OPERATIONS
        // ==============================================

        // Screenshot to Clipboard
        if (IsShortcutPressed("screenshot_clip")) {
            if (video_player && video_player->HasValidTexture()) {
                if (video_player->CaptureScreenshotToClipboard()) {
                    g_screenshot_flash.clipboard_flashing = true;
                    g_screenshot_flash.clipboard_flash_start = std::chrono::steady_clock::now();
                }
                Debug::Log("Screenshot to clipboard");
            }
        }

        // Screenshot to Desktop
        if (IsShortcutPressed("screenshot_desk")) {
            if (video_player && video_player->HasValidTexture()) {
                if (video_player->CaptureScreenshotToDesktop()) {
                    g_screenshot_flash.desktop_flashing = true;
                    g_screenshot_flash.desktop_flash_start = std::chrono::steady_clock::now();
                }
                Debug::Log("Screenshot to desktop");
            }
        }
    }

    void Application::HandleInput() {
        // Note: Primary keyboard shortcuts are handled in ProcessKeyboardShortcuts()
        // This function is kept for backwards compatibility but shortcuts moved there
        // to properly handle modifier key conflicts (e.g., Ctrl+O vs Ctrl+Shift+O)
    }
