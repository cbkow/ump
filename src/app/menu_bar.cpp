// ============================================================================
// Menu bar (File, Edit, View, Timeline, Playback, Color, Tools, Help menus)
// ============================================================================

#include "app/application.h"
#include "app/app_config.h"
#include "app/app_icons.h"
#include "app/app_ui_macros.h"
#include "app/timecode.h"
#include "project/project_manager.h"
#include "utils/debug_utils.h"
#include "timeline/timeline_view.h"
#include "timeline/timeline_playback_controller.h"
#include "player/video_player.h"
#include "player/video_decoder_factory.h"
#include "player/pipeline_mode.h"
#include "nodes/node_manager.h"
#include "annotations/annotation_manager.h"
#include "ui/annotation_panel.h"
#include "ui/timeline_manager.h"
#include "audio/audio_mixer.h"
#include "hdr/hdr_color_utils.h"
#ifdef _WIN32
#include "hdr/hdr_output_manager.h"
#endif
#include "ui/node_editor_theme.h"
#include "timeline/timeline_thumbnail_cache.h"
#include <imgui.h>
#include <chrono>
#include <GLFW/glfw3.h>
#include <string>
#include <algorithm>
#include <filesystem>

// Globals defined in main.cpp
extern ImFont* font_regular;
extern ImFont* font_bold;
extern ImFont* font_icons;
extern bool use_windows_accent_color;
extern int custom_accent_color_index;
extern ImVec4 custom_picker_color;
extern const ImVec4 accent_color_palette[];
extern const int accent_color_palette_count;
extern bool cache_enabled;
extern float g_font_scale;
extern bool otio_timeline_mode;
extern bool otio_dual_view_mode;
extern bool otio_dual_view_split_mode;
extern bool show_delete_prefs_confirm;
extern std::unique_ptr<qcview::TimelineView> timeline_view;
extern std::unique_ptr<qcview::TimelinePlaybackController> scratch_timeline_controller;
extern std::unique_ptr<qcview::TimelineThumbnailCache> timeline_thumbnail_cache;

// Screenshot flash state
struct ScreenshotFlashState {
    std::chrono::steady_clock::time_point clipboard_flash_start;
    std::chrono::steady_clock::time_point desktop_flash_start;
    bool clipboard_flashing = false;
    bool desktop_flashing = false;
    static constexpr float FLASH_DURATION_MS = 400.0f;
};
extern ScreenshotFlashState g_screenshot_flash;

// Other globals
extern bool g_force_reload_pending;
extern bool show_cannot_clear_exr_cache_error;
extern bool show_exr_cache_cleared_success;
extern size_t exr_cache_bytes_cleared;
extern bool show_cache_settings;
extern bool show_font_settings_window;
extern bool show_shortcuts_popup;
extern std::string stats_bar_notification_message;
extern std::chrono::steady_clock::time_point notification_start_time;
extern float notification_timeout_seconds;
extern bool show_notification_permanent;

    void Application::CreateMenuBar() {
        if (ImGui::BeginMenuBar()) {
            // Custom submenu background color
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.065f, 0.065f, 0.065f, 1.0f));

            if (ImGui::BeginMenu("File")) {
                ImGui::TextDisabled("Media:");
                if (ImGui::MenuItem("Open Media...", "Ctrl+O")) {
                    OpenFileDialog();
                }

                ImGui::Separator();

                if (!recent_files.empty()) {
                    if (ImGui::BeginMenu("Recent Files")) {
                        for (const auto& file : recent_files) {
                            if (ImGui::MenuItem(GetFileName(file).c_str())) {
                                Debug::Log("*** Loading recent file: " + file);

                                // Route through project manager for proper cache eviction
                                if (project_manager) {
                                    project_manager->LoadSingleFileFromDrop(file);
                                } else {
                        // Playback controller already exists - reload dummy video and re-enable timeline mode
                                    // Fallback to direct loading if project manager unavailable
                                    current_file_path = file;
                                    video_player->LoadFile(current_file_path);
                                    if (timeline_manager) {
                                        Debug::Log("*** Calling timeline_manager->SetVideoFile() for recent file");
                                        timeline_manager->SetVideoFile(current_file_path);
                                    }
                                    else {
                                        Debug::Log("*** ERROR: timeline_manager is null!");
                                    }

                                    // Auto-seek to first frame so user sees the video immediately
                                    video_player->Seek(0.0);
                                }
                                Debug::Log("Auto-seeked to first frame after recent file load");
                            }
                        }
                        ImGui::EndMenu();
                    }
                }

                ImGui::Separator();

                // Project management
                ImGui::TextDisabled("Project:");
                if (ImGui::MenuItem("New Project")) {
                    if (project_manager) {
                        project_manager->CreateNewProjectFromMenu();
                    }
                }
                if (ImGui::MenuItem("Open Project...", "Ctrl+Shift+O")) {
                    if (project_manager) {
                        project_manager->LoadProject();  // Empty path triggers file dialog
                        // Show project panels for context
                        show_project_panel = true;
                        show_inspector_panel = true;
                    }
                }

                // Save Project is always available - SaveProject() shows save dialog if no path exists
                bool has_project = project_manager && !project_manager->GetProjectPath().empty();
                if (ImGui::MenuItem("Save Project", "Ctrl+S")) {
                    if (project_manager) {
                        project_manager->SaveProject();
                    }
                }

                if (ImGui::MenuItem("Save Project As...", "Ctrl+Shift+S")) {
                    if (project_manager) {
                        project_manager->SaveProjectAs();
                    }
                }

                ImGui::Separator();

                // Timeline management
                ImGui::TextDisabled("Timeline:");
                if (ImGui::MenuItem("New Dual View")) {
                    if (project_manager) {
                        project_manager->ShowNewDualViewDialog();
                    }
                }
                if (ImGui::MenuItem("New Playlist", "Ctrl+Shift+P")) {
                    if (project_manager) {
                        project_manager->ShowNewPlaylistDialog();
                    }
                }

                ImGui::Separator();

                // Screenshot options
                ImGui::TextDisabled("Clipboard:");
                bool has_video = video_player && video_player->HasValidTexture();
                if (ImGui::MenuItem("Screenshot to Clipboard", "Ctrl+[", false, has_video)) {
                    if (has_video) {
                        if (video_player->CaptureScreenshotToClipboard()) {
                            g_screenshot_flash.clipboard_flashing = true;
                            g_screenshot_flash.clipboard_flash_start = std::chrono::steady_clock::now();
                        }
                    }
                }
                if (ImGui::MenuItem("Screenshot to Desktop", "Ctrl+]", false, has_video)) {
                    if (has_video) {
                        if (video_player->CaptureScreenshotToDesktop()) {
                            g_screenshot_flash.desktop_flashing = true;
                            g_screenshot_flash.desktop_flash_start = std::chrono::steady_clock::now();
                        }
                    }
                }

                ImGui::Separator();
                if (ImGui::MenuItem("Bake Annotations on Screenshots", nullptr, &cache_settings.bake_annotations_on_screenshots)) {
                    // Toggled - will be persisted on app close
                }

                ImGui::Separator();

                // Project sharing
                ImGui::TextDisabled("Share:");
                if (ImGui::MenuItem("Copy Project Link...", nullptr, false, has_project)) {
                    ShareProject();
                }

                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Alt+F4")) {
                    glfwSetWindowShouldClose(window, true);
                }

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("View")) {

                ImGui::TextDisabled("Layout Controls:");
                // Layout controls
                if (ImGui::MenuItem("Reset Layout", "Ctrl+R")) {
                    first_time_setup = true;
                    Debug::Log("Layout reset requested");
                }

                ImGui::Separator();

                // View modes
                if (ImGui::MenuItem("Default View", "Ctrl+0")) {
                    minimal_view_mode = false;
                    SetDefaultView();
                }

                if (ImGui::MenuItem("Minimal View", "Ctrl+-", minimal_view_mode)) {
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

                if (ImGui::MenuItem("Fullscreen", "F", is_fullscreen)) {
                    pending_fullscreen_toggle = true;
                }

                ImGui::Separator();

                // Panel visibility controls
                if (ImGui::MenuItem("Show All Panels", "Ctrl+9")) {
                    minimal_view_mode = false;
                    ShowAllPanels();
                }

                ImGui::Separator();
                ImGui::TextDisabled("Panels:");
                // Individual panel toggles
                if (ImGui::MenuItem("Project Panel", "Ctrl+1", show_project_panel)) {
                    show_project_panel = !show_project_panel;
                    if (show_project_panel) minimal_view_mode = false; // Exit minimal mode when opening panel
                }

                if (ImGui::MenuItem("Inspector Panel", "Ctrl+2", show_inspector_panel)) {
                    show_inspector_panel = !show_inspector_panel;
                    if (show_inspector_panel) minimal_view_mode = false; // Exit minimal mode when opening panel
                }

                if (ImGui::MenuItem("Timeline Panel", "Ctrl+3", show_timeline_panel)) {
                    show_timeline_panel = !show_timeline_panel;
                    if (show_timeline_panel) minimal_view_mode = false; // Exit minimal mode when opening panel
                    first_time_setup = true; // Trigger layout rebuild when timeline visibility changes
                }

                if (ImGui::MenuItem("Color Panels", "Ctrl+4", &show_color_panels)) {
                    first_time_setup = true;
                }

                if (ImGui::MenuItem("Annotations", "Ctrl+5", show_annotation_panel)) {
                    show_annotation_panel = !show_annotation_panel;
                    if (show_annotation_panel) minimal_view_mode = false; // Exit minimal mode when opening panel
                }

                if (ImGui::MenuItem("Annotation Toolbar", "Ctrl+6", show_annotation_toolbar)) {
                    show_annotation_toolbar = !show_annotation_toolbar;
                    if (annotation_toolbar) annotation_toolbar->SetVisible(show_annotation_toolbar);
                }

                if (ImGui::MenuItem("Sidebar", "Ctrl+7", show_sidebar_panel)) {
                    show_sidebar_panel = !show_sidebar_panel;
                    first_time_setup = true;  // Trigger dock rebuild
                }

                ImGui::Separator();
                ImGui::TextDisabled("Background & Overlays:");

                if (ImGui::MenuItem("Video Background", "Ctrl+Shift+B, B")) {
                    show_background_panel = !show_background_panel;
                }

                if (ImGui::MenuItem("Safety Overlays", "Ctrl+/")) {
                    show_safety_overlay_panel = !show_safety_overlay_panel;
                }

                if (ImGui::MenuItem("Colorspace Presets", "Ctrl+C")) {
                    show_colorspace_panel = !show_colorspace_panel;
                }

                ImGui::Separator();
                ImGui::TextDisabled("Settings:");

                if (ImGui::MenuItem("System Accent Color", nullptr, use_windows_accent_color)) {
                    use_windows_accent_color = !use_windows_accent_color;
                    if (use_windows_accent_color) {
                        custom_accent_color_index = -1;  // Clear custom color when using system
                    }
                    SaveSettings();
                    SetupImGuiStyle();
                    NodeEditorTheme::ApplyDarkTheme();
                }

                // Accent color picker
                if (ImGui::BeginMenu("Accent Color")) {
                    if (ImGui::ColorPicker3("##AccentPicker", (float*)&custom_picker_color,
                            ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview)) {
                        custom_accent_color_index = -2;
                        use_windows_accent_color = false;
                        SaveSettings();
                        SetupImGuiStyle();
                        NodeEditorTheme::ApplyDarkTheme();
                    }
                    ImGui::EndMenu();
                }

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Annotations")) {
                ImGui::TextDisabled("Annotation Display:");

                if (ImGui::MenuItem("Enable Annotations", nullptr, annotations_enabled)) {
                    annotations_enabled = !annotations_enabled;
                    Debug::Log(annotations_enabled ? "Annotations enabled for playback" : "Annotations disabled for playback");
                }

                ImGui::Separator();

                ImGui::TextDisabled("Panels:");

                if (ImGui::MenuItem("Annotations Panel", "Ctrl+5", show_annotation_panel)) {
                    show_annotation_panel = !show_annotation_panel;
                    if (show_annotation_panel) minimal_view_mode = false;
                }

                if (ImGui::MenuItem("Annotation Toolbar", "Ctrl+6", show_annotation_toolbar)) {
                    show_annotation_toolbar = !show_annotation_toolbar;
                    if (annotation_toolbar) annotation_toolbar->SetVisible(show_annotation_toolbar);
                }

                ImGui::Separator();

                // Export submenu
                bool has_notes = annotation_manager && annotation_manager->GetNoteCount() > 0;

                if (ImGui::BeginMenu("Export Notes", has_notes)) {
                    if (ImGui::MenuItem("Markdown")) {
                        if (annotation_panel) {
                            annotation_panel->TriggerExport("markdown");
                        }
                    }
                    if (ImGui::MenuItem("HTML")) {
                        if (annotation_panel) {
                            annotation_panel->TriggerExport("html");
                        }
                    }
                    if (ImGui::MenuItem("PDF")) {
                        if (annotation_panel) {
                            annotation_panel->TriggerExport("pdf");
                        }
                    }
                    ImGui::EndMenu();
                }

                if (!has_notes) {
                    ImGui::TextDisabled("(No notes to export)");
                }

                ImGui::Separator();

                // Import submenu (requires media to be loaded)
                bool has_media_loaded = !current_file_path.empty();
                if (!has_media_loaded) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::BeginMenu("Import Notes")) {
                    if (ImGui::MenuItem("From Frame.io...")) {
                        frameio_import_state.show_dialog = true;
                    }
                    ImGui::EndMenu();
                }
                if (!has_media_loaded) {
                    ImGui::EndDisabled();
                }

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Transcode")) {
                bool has_selected = project_manager && project_manager->HasSelectedItems();
                bool can_transcode = false;

                // Check if any selected item is transcodable (sequence or video)
                if (has_selected && project_manager) {
                    auto selected_items = project_manager->GetSelectedItems();
                    for (const auto& item : selected_items) {
                        if (item.type == qcview::MediaType::IMAGE_SEQUENCE ||
                            item.type == qcview::MediaType::EXR_SEQUENCE ||
                            item.type == qcview::MediaType::VIDEO) {
                            can_transcode = true;
                            break;
                        }
                    }
                }

                ImGui::TextDisabled("Queue:");

                if (ImGui::MenuItem("Show Queue Manager", "Ctrl+Shift+T")) {
                    if (project_manager) {
                        project_manager->ShowTranscodeQueueWindow();
                    }
                }

                ImGui::Separator();
                ImGui::TextDisabled("Add to Queue:");

                if (ImGui::MenuItem("Add Selected Items", "Ctrl+Shift+Q", false, can_transcode)) {
                    if (project_manager) {
                        project_manager->AddSelectedItemsToTranscodeQueue();
                    }
                }

                if (!can_transcode && has_selected) {
                    ImGui::TextDisabled("(Only image sequences can be transcoded)");
                } else if (!has_selected) {
                    ImGui::TextDisabled("(No items selected)");
                }

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Playback")) {
                // ==============================================
                // PRIMARY CONTROLS
                // ==============================================

                ImGui::TextDisabled("Playback:");

                if (ImGui::MenuItem("Play/Pause", "Space, W")) {
                    if (video_player->IsPlaying()) {
                        video_player->Pause();
                        if (project_manager) {
                            project_manager->NotifyPlaybackState(false);
                        }
                    }
                    else {
                        video_player->Play();
                        if (project_manager) {
                            project_manager->NotifyPlaybackState(true);
                        }
                    }
                }

                bool loop_enabled = video_player->IsLooping();
                const char* loop_status = loop_enabled ? " (ON)" : " (OFF)";
                std::string full_loop_text = std::string("Toggle Loop") + loop_status;

                if (ImGui::MenuItem(full_loop_text.c_str(), "L")) {
                    ToggleLoop();
                }

                ImGui::Separator();


                // ==============================================
                // FRAME CONTROLS
                // ==============================================

                ImGui::TextDisabled("Frame Navigation:");

                if (ImGui::MenuItem("Previous Frame", "Q")) {
                    video_player->StepFrame(-1);
                    timeline_manager->ScheduleFrameStepSync();
                }

                if (ImGui::MenuItem("Next Frame", "E")) {
                    video_player->StepFrame(1);
                    timeline_manager->ScheduleFrameStepSync();
                }

                ImGui::Separator();

                // ==============================================
                // FAST SEEK CONTROLS (Legacy A/D system)
                // ==============================================

                ImGui::TextDisabled("Fast Seek (Legacy):");

                if (ImGui::MenuItem("Rewind", "A (hold)")) {
                    video_player->StartRewind();
                }

                if (ImGui::MenuItem("Fast Forward", "D (hold)")) {
                    video_player->StartFastForward();
                }

                ImGui::Separator();

                // ==============================================
                // VOLUME CONTROLS
                // ==============================================

                ImGui::TextDisabled("Volume:");

                if (ImGui::MenuItem("Volume Up", "Up Arrow")) {
                    int new_volume = current_volume + 5;
                    if (new_volume > 100) new_volume = 100;
                    current_volume = new_volume;
                    if (timeline_view) {
                        if (auto* ctrl = timeline_view->GetEffectivePlaybackController()) {
                            if (auto* mixer = ctrl->GetAudioMixer()) {
                                mixer->SetVolume(new_volume / 100.0f);
                            }
                        }
                    }
                }

                if (ImGui::MenuItem("Volume Down", "Down Arrow")) {
                    int new_volume = current_volume - 5;
                    if (new_volume < 0) new_volume = 0;
                    current_volume = new_volume;
                    if (timeline_view) {
                        if (auto* ctrl = timeline_view->GetEffectivePlaybackController()) {
                            if (auto* mixer = ctrl->GetAudioMixer()) {
                                mixer->SetVolume(new_volume / 100.0f);
                            }
                        }
                    }
                }

                if (ImGui::MenuItem("Mute/Unmute", "M")) {
                    ToggleMute();
                }

                ImGui::Separator();

                // ==============================================
                // PLAYBACK OPTIONS
                // ==============================================

                ImGui::TextDisabled("Options:");

                if (ImGui::MenuItem("Auto-Play on Load", nullptr, cache_settings.auto_play_on_load)) {
                    cache_settings.auto_play_on_load = !cache_settings.auto_play_on_load;
                    SaveSettings();
                    Debug::Log(cache_settings.auto_play_on_load ? "Auto-play on load enabled" : "Auto-play on load disabled");
                }

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Pipeline")) {

                ImGui::TextDisabled("Pipeline Modes:");

                // Determine current state for pipeline mode selection
                PipelineMode current_mode = PipelineMode::NORMAL;
                bool is_audio_only = false;
                bool is_image_sequence = false;
                bool is_exr = false;
                bool is_16bit_image = false;  // TIFF/PNG detected as 16-bit
                bool has_media = false;

                // Check OTIO timeline mode first
                if (otio_timeline_mode && timeline_view) {
                    qcview::TimelineSourceMode source_mode = timeline_view->GetSourceMode();
                    auto* controller = timeline_view->GetEffectivePlaybackController();

                    if (source_mode == qcview::TimelineSourceMode::AUDIO_FILE) {
                        is_audio_only = true;
                    } else {
                        has_media = true;
                        current_mode = controller ? controller->GetCurrentPipelineMode() : PipelineMode::NORMAL;

                        // Get source path for format detection
                        std::string source_path = controller ? controller->GetCurrentSourcePath() : "";
                        if (!source_path.empty()) {
                            std::string ext = "";
                            size_t dot_pos = source_path.rfind('.');
                            if (dot_pos != std::string::npos) {
                                ext = source_path.substr(dot_pos);
                                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                            }

                            bool is_tiff = (ext == ".tiff" || ext == ".tif");
                            bool is_png = (ext == ".png");

                            // Check if it's an image sequence
                            is_image_sequence = (source_mode == qcview::TimelineSourceMode::IMAGE_SEQUENCE) ||
                                                (ext == ".exr") || is_tiff || is_png || (ext == ".jpg" || ext == ".jpeg");

                            // Use pipeline mode to determine format (more reliable than extension)
                            // ULTRA_HIGH_RES/HDR_RES = EXR/float content
                            // HIGH_RES = 16-bit content
                            // NORMAL = 8-bit content
                            is_exr = (current_mode == PipelineMode::ULTRA_HIGH_RES || current_mode == PipelineMode::HDR_RES);
                            is_16bit_image = (current_mode == PipelineMode::HIGH_RES);
                        }
                    }
                } else if (video_player) {
                    has_media = true;
                    current_mode = video_player->GetPipelineMode();

                    // Check for image sequence mode
                    is_image_sequence = video_player->IsInEXRMode() || video_player->IsImageSequence();

                    // Use pipeline mode to determine format (more reliable)
                    is_exr = (current_mode == PipelineMode::ULTRA_HIGH_RES || current_mode == PipelineMode::HDR_RES);
                    is_16bit_image = (current_mode == PipelineMode::HIGH_RES);
                }

                // Pipeline Mode menu items - standardized labels
                const char* mode_labels[] = {
                    "Normal (8-bit)",
                    "High-Res (12/16-bit)",
                    "Ultra-High-Res (Float/HDR)"
                };

                const char* mode_tooltips[] = {
                    "8-bit integer pipeline (GL_RGBA8)\nFor standard video and 8-bit images",
                    "12/16-bit integer pipeline (GL_RGBA16)\nFor ProRes 4444, DNxHR 444, 16-bit TIFF/PNG",
                    "16-bit float pipeline (GL_RGBA16F)\nFor HDR video, EXR sequences, and complex OCIO transforms"
                };

                if (is_audio_only) {
                    // All options disabled for audio-only
                    ImGui::BeginDisabled();
                    for (int i = 0; i < 3; i++) {
                        ImGui::MenuItem(mode_labels[i], nullptr, false);
                    }
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::SetTooltip("No video pipeline for audio-only content");
                    }
                } else if (!has_media) {
                    // No media loaded - all disabled
                    ImGui::BeginDisabled();
                    for (int i = 0; i < 3; i++) {
                        ImGui::MenuItem(mode_labels[i], nullptr, false);
                    }
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::SetTooltip("No media loaded");
                    }
                } else if (is_image_sequence) {
                    // Image sequence - show all options, disable unavailable ones
                    for (int i = 0; i < 3; i++) {
                        PipelineMode mode = static_cast<PipelineMode>(i);
                        bool is_selected = (current_mode == mode);
                        bool should_disable = false;
                        const char* disable_reason = nullptr;

                        if (is_exr) {
                            // EXR: only Ultra-High-Res available
                            if (mode != PipelineMode::ULTRA_HIGH_RES) {
                                should_disable = true;
                                disable_reason = "EXR sequences use Float pipeline";
                            }
                        } else if (is_16bit_image) {
                            // 16-bit TIFF/PNG: only High-Res available
                            if (mode != PipelineMode::HIGH_RES) {
                                should_disable = true;
                                disable_reason = "16-bit images use High-Res pipeline";
                            }
                        } else {
                            // 8-bit images (JPEG, 8-bit PNG/TIFF): only Normal available
                            if (mode != PipelineMode::NORMAL) {
                                should_disable = true;
                                disable_reason = "8-bit images use Normal pipeline";
                            }
                        }

                        if (should_disable) ImGui::BeginDisabled();

                        if (ImGui::MenuItem(mode_labels[i], nullptr, is_selected)) {
                            // Image sequences are locked - no action needed
                        }

                        if (should_disable) {
                            ImGui::EndDisabled();
                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && disable_reason) {
                                ImGui::SetTooltip("%s", disable_reason);
                            }
                        } else if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", mode_tooltips[i]);
                        }
                    }
                } else {
                    // Video content - all modes available
                    for (int i = 0; i < 3; i++) {
                        PipelineMode mode = static_cast<PipelineMode>(i);
                        bool is_selected = (current_mode == mode);

                        if (ImGui::MenuItem(mode_labels[i], nullptr, is_selected)) {
                            if (!is_selected) {
                                // Set project-level pipeline mode - reload will pick this up
                                if (project_manager) {
                                    project_manager->SetProjectPipelineMode(mode);
                                }
                                Debug::Log("Pipeline mode changed to: " + std::string(PipelineModeToString(mode)) + " - forcing full reload");
                                g_force_reload_pending = true;  // Full reload will apply new mode
                            }
                        }

                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", mode_tooltips[i]);
                        }
                    }
                }

                ImGui::Separator();

                // Video Range Override (for D3D11 YUV decoding)
                ImGui::TextDisabled("Video Range:");

                if (ImGui::MenuItem("Auto (Detect)", nullptr, cache_settings.video_range_mode == 0)) {
                    if (cache_settings.video_range_mode != 0) {
                        cache_settings.video_range_mode = 0;
                        qcview::g_video_range_override = VideoRangeMode::AUTO;
                        SaveSettings();
                        Debug::Log("Video range mode set to AUTO - deferring reload");
                        g_force_reload_pending = true;  // Defer to next frame start
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Use range detected from video metadata");
                }
                if (ImGui::MenuItem("Full Range", nullptr, cache_settings.video_range_mode == 1)) {
                    if (cache_settings.video_range_mode != 1) {
                        cache_settings.video_range_mode = 1;
                        qcview::g_video_range_override = VideoRangeMode::FULL;
                        SaveSettings();
                        Debug::Log("Video range mode set to FULL - deferring reload");
                        g_force_reload_pending = true;  // Defer to next frame start
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Force full range (0-255 / 0-1023)\nUse if blacks appear crushed");
                }
                if (ImGui::MenuItem("Limited Range", nullptr, cache_settings.video_range_mode == 2)) {
                    if (cache_settings.video_range_mode != 2) {
                        cache_settings.video_range_mode = 2;
                        qcview::g_video_range_override = VideoRangeMode::LIMITED;
                        SaveSettings();
                        Debug::Log("Video range mode set to LIMITED - deferring reload");
                        g_force_reload_pending = true;  // Defer to next frame start
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Force limited/legal range (16-235 / 64-940)\nUse if blacks appear washed out");
                }

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Settings")) {
                // === CACHE SETTINGS ===
                ImGui::TextDisabled("Cache:");

                // Clear Image Sequence Disk Cache
                if (ImGui::MenuItem("Clear Image Sequence Disk Cache")) {
                    if (project_manager && project_manager->HasLoadedEXRSequences()) {
                        show_cannot_clear_exr_cache_error = true;
                    } else if (video_player) {
                        exr_cache_bytes_cleared = video_player->ClearEXRDiskCache();
                        show_exr_cache_cleared_success = true;
                    }
                }

                if (ImGui::MenuItem("Enable Timeline Thumbnails", nullptr, cache_settings.enable_thumbnails)) {
                    cache_settings.enable_thumbnails = !cache_settings.enable_thumbnails;
                    SaveSettings();
                    if (!cache_settings.enable_thumbnails && timeline_thumbnail_cache) {
                        timeline_thumbnail_cache->Clear();
                        Debug::Log("Timeline thumbnail cache cleared");
                    }
                    Debug::Log(cache_settings.enable_thumbnails ? "Timeline thumbnails enabled" : "Timeline thumbnails disabled");
                }

                // === AUTO SETTINGS ===
                ImGui::Separator();
                ImGui::TextDisabled("Auto:");

                bool hdr_active = qcview::IsHDRDisplayActive();
                if (hdr_active) ImGui::BeginDisabled();
                if (ImGui::MenuItem("Auto 1-2-1", nullptr, &cache_settings.auto_121_enabled)) {
                    Debug::Log(cache_settings.auto_121_enabled ?
                        "Auto 1-2-1 enabled: Will auto-apply Rec.709 -> sRGB for 1-2-1 videos" :
                        "Auto 1-2-1 disabled");
                    SaveSettings();
                }
                if (hdr_active) ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (hdr_active) {
                        ImGui::SetTooltip("Disabled in HDR mode - color conversion handled by HDR pipeline");
                    } else {
                        ImGui::SetTooltip("Auto-apply Rec.709 -> sRGB OCIO preset when 1-2-1 NCLC tag detected\n"
                                         "(BT.709 primaries with Unspecified transfer)");
                    }
                }

                // === USER INTERFACE ===
                ImGui::Separator();
                ImGui::TextDisabled("User Interface:");

                if (ImGui::MenuItem("Keyboard Shortcuts...")) {
                    show_shortcuts_popup = true;
                }

                if (ImGui::MenuItem("Font Settings...")) {
                    show_font_settings_window = true;
                }

                // === ADVANCED SETTINGS ===
                ImGui::Separator();
                ImGui::TextDisabled("Advanced:");

                if (ImGui::MenuItem("QCView Settings...")) {
                    show_cache_settings = true;
                }

                // === DANGER ZONE ===
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.8f, 0.3f, 0.3f, 1.0f), "Danger Zone:");

                if (ImGui::MenuItem("Delete All Preferences...")) {
                    show_delete_prefs_confirm = true;
                }

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Help")) {

                ImGui::TextDisabled("About QCView v1.0.4");

                if (ImGui::MenuItem("Manual")) {
#ifdef _WIN32
                    ShellExecuteA(NULL, "open", "https://qcview.app/", NULL, NULL, SW_SHOWNORMAL);
#else
                    system("xdg-open https://qcview.app/ &");
#endif
                }

                if (ImGui::MenuItem("License")) {
#ifdef _WIN32
                    ShellExecuteA(NULL, "open", "https://github.com/cbkow/QCView-Player/blob/main/LICENSE", NULL, NULL, SW_SHOWNORMAL);
#else
                    system("xdg-open https://github.com/cbkow/QCView-Player/blob/main/LICENSE &");
#endif
                }

                if (ImGui::MenuItem("Check for Updates")) {
#ifdef _WIN32
                    ShellExecuteA(NULL, "open", "https://github.com/cbkow/QCView-Player/releases", NULL, NULL, SW_SHOWNORMAL);
#else
                    system("xdg-open https://github.com/cbkow/QCView-Player/releases &");
#endif
                }

                ImGui::EndMenu();
            }

            ImGui::PopStyleColor(); // Pop custom submenu background

            // ========================================================================
            // SHADER STATUS INDICATOR IN MENU BAR
            // ========================================================================
            bool shader_applied = video_player && video_player->HasActiveColorTransform();
            float shader_text_width = 0.0f;
            if (shader_applied) {
                const char* shader_text = "Shader Applied";
                shader_text_width = ImGui::CalcTextSize(shader_text).x + 20.0f; // Add spacing
            }

            // ========================================================================
            // SYSTEM NOTIFICATION IN MENU BAR
            // ========================================================================
            if (!stats_bar_notification_message.empty()) {
                // Check timeout for non-permanent messages
                bool should_show = true;
                if (!show_notification_permanent) {
                    auto now = std::chrono::steady_clock::now();
                    float elapsed = std::chrono::duration<float>(now - notification_start_time).count();
                    if (elapsed > notification_timeout_seconds) {
                        stats_bar_notification_message.clear();
                        should_show = false;
                    }
                }

                if (should_show) {
                    // Measure text width to position on right side (account for shader text)
                    std::string display_text = stats_bar_notification_message;
                    float text_width = ImGui::CalcTextSize(display_text.c_str()).x;
                    float padding = 20.0f;

                    // Position on right side of menu bar, leaving room for shader text
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - text_width - shader_text_width + ImGui::GetCursorPosX() - padding);

                    // Color based on message type
                    ImVec4 bg_color = show_notification_permanent ?
                        ImVec4(0.8f, 0.5f, 0.0f, 0.3f) :  // Orange for critical
                        ImVec4(0.0f, 0.6f, 0.2f, 0.3f);   // Green for recovery
                    ImVec4 text_color = show_notification_permanent ?
                        ImVec4(1.0f, 0.8f, 0.0f, 1.0f) :  // Yellow text for critical
                        ImVec4(0.0f, 1.0f, 0.4f, 1.0f);   // Light green text for recovery

                    // Render notification with background
                    ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
                    ImVec2 text_size = ImGui::CalcTextSize(display_text.c_str());
                    ImVec2 rect_min = ImVec2(cursor_pos.x - 8.0f, cursor_pos.y - 4.0f);
                    ImVec2 rect_max = ImVec2(cursor_pos.x + text_size.x + 8.0f, cursor_pos.y + text_size.y + 4.0f);

                    ImGui::GetWindowDrawList()->AddRectFilled(rect_min, rect_max, ImGui::GetColorU32(bg_color), 4.0f);

                    ImGui::PushStyleColor(ImGuiCol_Text, text_color);
                    ImGui::Text("%s", display_text.c_str());
                    ImGui::PopStyleColor();
                }
            }

            // Render shader status (after notification, so it appears to the right)
            if (shader_applied) {
                const char* shader_text = "Shader Applied";
                float padding = 20.0f;

                // If no notification, position from right edge
                if (stats_bar_notification_message.empty()) {
                    float text_width = ImGui::CalcTextSize(shader_text).x;
                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - text_width + ImGui::GetCursorPosX() - padding);
                } else {
                    // Already positioned after notification, just add some spacing
                    ImGui::SameLine();
                }

                ImGui::PushStyleColor(ImGuiCol_Text, GetWindowsAccentColor());
                ImGui::Text("%s", shader_text);
                ImGui::PopStyleColor();
            }

            ImGui::EndMenuBar();
        }
    }
