// ============================================================================
// Dialog windows (cache stats, audio diagnostics, transcode, settings, etc.)
// ============================================================================

#include "app/application.h"
#include "app/app_config.h"
#include "app/app_icons.h"
#include "app/app_ui_macros.h"
#include "app/ui_scale.h"
#include "app/shortcut_manager.h"
#include "project/project_manager.h"
#include "utils/debug_utils.h"
#include "utils/system_pressure_monitor.h"
#include "timeline/timeline_view.h"
#include "timeline/timeline_playback_controller.h"
#include "timeline/timeline_cache.h"
#include "player/video_player.h"
#include "player/pipeline_mode.h"
#include "player/direct_exr_cache.h"
#include "ui/timeline_manager.h"
#include "audio/audio_mixer.h"
#include "audio/audio_player.h"
#include "transcode/transcode_settings.h"
#ifdef _WIN32
#include "hdr/hdr_output_manager.h"
#endif
#include <imgui.h>
#include <nfd.h>
#if !defined(QCVIEW_USE_METAL)
#include <GLFW/glfw3.h>
#endif
#include <string>
#include <chrono>
#include <filesystem>
#include <atomic>
#include <mutex>

// Globals defined in main.cpp
extern ImFont* font_regular;
extern ImFont* font_bold;
extern ImFont* font_icons;
extern bool cache_enabled;
extern float g_font_scale;
extern bool show_cache_settings;
extern bool show_font_settings_window;
extern bool show_shortcuts_popup;
extern bool show_cannot_clear_exr_cache_error;
extern bool show_exr_cache_cleared_success;
extern size_t exr_cache_bytes_cleared;
extern bool g_force_reload_pending;
extern std::string stats_bar_notification_message;
extern std::chrono::steady_clock::time_point notification_start_time;
extern float notification_timeout_seconds;
extern bool show_notification_permanent;
extern std::unique_ptr<qcview::TimelineView> timeline_view;

// Transcode globals
extern bool show_transcode_progress;
extern std::atomic<int> transcode_current_frame;
extern std::atomic<int> transcode_total_frames;
extern std::string transcode_status_message;
extern std::mutex transcode_status_mutex;
extern TranscodeSettings transcode_settings;

// Cache config globals
extern CacheSettings cache_settings;
extern std::string g_custom_cache_path;
extern int g_cache_retention_days;
extern int g_transcode_cache_max_gb;
extern bool g_clear_cache_on_exit;
extern int g_exr_read_ahead_frames;
extern int g_read_behind_frames;
extern int g_exr_thread_count;
extern int g_exr_transcode_threads;
extern int g_timeline_read_ahead_frames;
extern int g_timeline_read_behind_frames;
extern int g_timeline_max_textures;
extern int g_timeline_io_threads;
extern int g_video_decode_threads;

// Pressure monitoring globals
extern bool show_pressure_critical_dialog;
extern bool in_emergency_mode;
extern qcview::SystemPressureStatus last_pressure_status;
extern bool exr_cache_was_active;
extern std::string exr_video_path_before_shutdown;

// LUT export globals
extern bool show_lut_export_popup;
extern std::string lut_export_path;
extern std::atomic<float> lut_export_progress;
extern std::atomic<bool> lut_export_cancel;
extern std::atomic<bool> lut_export_complete;
extern std::atomic<bool> lut_export_error;
extern std::string lut_export_error_message;

// Font settings globals
extern float font_settings_temp_scale;

// Font scale presets (compile-time constants)
constexpr float FONT_SCALE_SMALL = 0.75f;
constexpr float FONT_SCALE_MEDIUM = 1.0f;
constexpr float FONT_SCALE_LARGE = 1.25f;
constexpr float FONT_SCALE_XLARGE = 1.5f;

// Free functions defined in main.cpp
qcview::DirectEXRCacheConfig GetCurrentEXRCacheConfig();
void AutoConfigureEXRThreading(CacheSettings& settings);

    void Application::CreateCacheStatsWindow() {
        static bool show_cache_stats = false;

        // Toggle with Ctrl+Shift+C
        if (ImGui::IsKeyPressed(ImGuiKey_C) && ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift) {
            show_cache_stats = !show_cache_stats;
        }

        if (!show_cache_stats || !timeline_manager) return;

        if (ImGui::Begin("Frame Cache Statistics", &show_cache_stats)) {
            auto stats = timeline_manager->GetCacheStats();

            ImGui::Text("Cache Status:");
            ImGui::Separator();

            ImGui::Text("Frames Cached: %zu", stats.total_frames_cached);
            ImGui::Text("Cache Hits: %zu", stats.cache_hits);
            ImGui::Text("Cache Misses: %zu", stats.cache_misses);
            ImGui::Text("Hit Ratio: %.1f%%", stats.hit_ratio * 100.0f);

            ImGui::Spacing();

            if (stats.coverage_end > stats.coverage_start) {
                ImGui::Text("Coverage: %.2fs - %.2fs", stats.coverage_start, stats.coverage_end);
                ImGui::Text("Window: %.2fs", stats.coverage_end - stats.coverage_start);
            }
            else {
                ImGui::Text("Coverage: No cached frames");
            }

            ImGui::Spacing();
            ImGui::Separator();

            // Cache configuration
            PushOutlineHeaderStyle();
            if (ImGui::CollapsingHeader("Cache Configuration")) {
                static int cache_size_mb = 12288;
                static float window_seconds = -1.0f;  // Full video
                static int cache_width = 1920;

            }

            ImGui::Spacing();
            ImGui::Separator();

            // Cache visualization legend
            if (ImGui::CollapsingHeader("Cache Visualization##CacheDiag")) {
                ImGui::Text("Timeline Cache Bar Legend:");
                ImGui::Spacing();

                // Green bar sample
                ImDrawList* legend_draw_list = ImGui::GetWindowDrawList();
                ImVec2 cursor_pos = ImGui::GetCursorScreenPos();

                legend_draw_list->AddRectFilled(
                    cursor_pos,
                    ImVec2(cursor_pos.x + 20, cursor_pos.y + 6),
                    IM_COL32(50, 200, 50, 200)
                );
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 25);
                ImGui::Text("RAM Cache (Green) - Frames cached in memory");

                cursor_pos = ImGui::GetCursorScreenPos();
                legend_draw_list->AddRectFilled(
                    cursor_pos,
                    ImVec2(cursor_pos.x + 20, cursor_pos.y + 6),
                    IM_COL32(50, 120, 200, 150)
                );
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 25);
                ImGui::Text("Keyframe Cache (Blue) - Sparse coverage for seeking");

                ImGui::Spacing();

                // White window indicator sample
                cursor_pos = ImGui::GetCursorScreenPos();
                legend_draw_list->AddRect(
                    ImVec2(cursor_pos.x, cursor_pos.y + 1),
                    ImVec2(cursor_pos.x + 20, cursor_pos.y + 5),
                    UI_WHITE_A(80), 0.0f, 0, 1.0f
                );
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 25);
                ImGui::Text("Active Cache Window (White outline) - 60s centered focus");

                ImGui::Spacing();
                ImGui::TextWrapped("Segments closer to current position appear brighter. "
                    "RAM cache focuses around the current playhead position for optimal scrubbing performance. "
                    "The cache bar appears at the bottom of the timeline.");
            }
            PopOutlineHeaderStyle();

            ImGui::Spacing();
            ImGui::Text("Press Ctrl+Shift+C to toggle this window");
        }
        ImGui::End();
    }

    void Application::CreateAudioDiagnosticsWindow() {
        static bool show_audio_diag = false;

        // Toggle with Ctrl+Shift+A
        if (ImGui::IsKeyPressed(ImGuiKey_A) && ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift) {
            show_audio_diag = !show_audio_diag;
        }

        if (!show_audio_diag) return;

        if (ImGui::Begin("Audio Diagnostics", &show_audio_diag)) {
            ImGui::Text("Audio System Status:");
            ImGui::Separator();

            // Dual View Audio (AudioPlayer)
            PushOutlineHeaderStyle();
            if (ImGui::CollapsingHeader("Dual View Audio (AudioPlayer)", ImGuiTreeNodeFlags_DefaultOpen)) {
                qcview::AudioPlayer* audio = video_player ? video_player->GetDualViewAudio() : nullptr;
                if (audio) {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "AudioPlayer: ACTIVE");
                    ImGui::Text("  Initialized: %s", audio->IsInitialized() ? "Yes" : "No");
                    ImGui::Text("  Playing: %s", audio->IsPlaying() ? "Yes" : "No");
                    ImGui::Text("  Volume: %.0f%%", audio->GetVolume() * 100.0f);
                    ImGui::Text("  Muted: %s", audio->IsMuted() ? "Yes" : "No");
                    ImGui::Text("  Has Clip: %s", audio->HasClip() ? "Yes" : "No");
                    ImGui::Text("  Has Audio Track: %s", audio->HasAudio() ? "Yes" : "No");

                    // Diagnostic counters
                    ImGui::Spacing();
                    ImGui::TextDisabled("Diagnostics:");
                    uint64_t callbacks = audio->diag_callback_count_.load();
                    uint64_t frames_out = audio->diag_frames_output_.load();
                    uint64_t silence = audio->diag_silence_frames_.load();
                    uint64_t frames_read = audio->diag_frames_read_.load();
                    ImGui::Text("  Callbacks: %llu", callbacks);
                    ImGui::Text("  Frames Output: %llu (%.1fs)", frames_out, frames_out / 48000.0);
                    ImGui::Text("  Frames Read: %llu", frames_read);
                    ImGui::Text("  Silence Frames: %llu", silence);
                    if (frames_out > 0) {
                        double data_pct = 100.0 * frames_read / frames_out;
                        ImGui::Text("  Data vs Silence: %.1f%% data", data_pct);
                    }
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "AudioPlayer: NOT ACTIVE");
                    ImGui::TextDisabled("  (Only active in Dual View mode)");
                }
            }

            ImGui::Spacing();

            // Timeline Audio (AudioMixer)
            if (ImGui::CollapsingHeader("Timeline Audio (AudioMixer)", ImGuiTreeNodeFlags_DefaultOpen)) {
                qcview::TimelinePlaybackController* ctrl = timeline_view ? timeline_view->GetEffectivePlaybackController() : nullptr;
                qcview::AudioMixer* mixer = ctrl ? ctrl->GetAudioMixer() : nullptr;

                if (mixer) {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "AudioMixer: ACTIVE");
                    ImGui::Text("  Initialized: %s", mixer->IsInitialized() ? "Yes" : "No");
                    ImGui::Text("  Playing: %s", mixer->IsPlaying() ? "Yes" : "No");
                    ImGui::Text("  Volume: %.0f%%", mixer->GetVolume() * 100.0f);
                    ImGui::Text("  Muted: %s", mixer->IsMuted() ? "Yes" : "No");

                    std::string clip_id = mixer->GetCurrentClipId();
                    std::string source_path = mixer->GetCurrentSourcePath();
                    ImGui::Text("  Current Clip: %s", clip_id.empty() ? "(none/gap)" : clip_id.c_str());
                    if (!source_path.empty()) {
                        // Show just filename
                        size_t last_slash = source_path.find_last_of("\\/");
                        std::string filename = (last_slash != std::string::npos) ?
                            source_path.substr(last_slash + 1) : source_path;
                        ImGui::Text("  Source: %s", filename.c_str());
                    }
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "AudioMixer: NOT ACTIVE");
                    ImGui::TextDisabled("  (Only active in Timeline mode with linked clips)");
                }
            }
            PopOutlineHeaderStyle();

            ImGui::Spacing();
            ImGui::Separator();

            // Quick controls
            ImGui::Text("Quick Controls:");
            PushOutlineButtonStyle();
            if (ImGui::Button("Test: Play Audio")) {
                qcview::AudioPlayer* audio = video_player ? video_player->GetDualViewAudio() : nullptr;
                if (audio && audio->IsInitialized()) audio->Play();

                qcview::TimelinePlaybackController* ctrl = timeline_view ? timeline_view->GetEffectivePlaybackController() : nullptr;
                qcview::AudioMixer* mixer = ctrl ? ctrl->GetAudioMixer() : nullptr;
                if (mixer && mixer->IsInitialized()) mixer->Play();
            }
            PopOutlineButtonStyle();

            ImGui::SameLine();

            PushOutlineButtonStyle();
            if (ImGui::Button("Test: Pause Audio")) {
                qcview::AudioPlayer* audio = video_player ? video_player->GetDualViewAudio() : nullptr;
                if (audio) audio->Pause();

                qcview::TimelinePlaybackController* ctrl = timeline_view ? timeline_view->GetEffectivePlaybackController() : nullptr;
                qcview::AudioMixer* mixer = ctrl ? ctrl->GetAudioMixer() : nullptr;
                if (mixer) mixer->Pause();
            }
            PopOutlineButtonStyle();

            ImGui::Spacing();
            ImGui::TextDisabled("Press Ctrl+Shift+A to toggle this window");
        }
        ImGui::End();
    }

    void Application::CreateTranscodeProgressDialog() {
        // Open modal popup when transcode starts
        if (show_transcode_progress) {
            ImGui::OpenPopup("EXR Transcode Progress");
        }

        // Set popup to center on screen
        float scale = ImGui::GetIO().FontGlobalScale;
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(S(500), 0), ImGuiCond_Always);

        if (ImGui::BeginPopupModal("EXR Transcode Progress", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            // Check if transcode completed (progress flag was cleared by completion callback)
            if (!show_transcode_progress) {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return;
            }

            ImGui::Text("Converting multilayer EXR to single-layer sequence...");
            ImGui::Spacing();

            // Get progress values (thread-safe)
            int current = transcode_current_frame.load();
            int total = transcode_total_frames.load();

            std::string status_msg;
            {
                std::lock_guard<std::mutex> lock(transcode_status_mutex);
                status_msg = transcode_status_message;
            }

            // Progress bar with system accent color
            float progress = (total > 0) ? static_cast<float>(current) / static_cast<float>(total) : 0.0f;
            char progress_text[64];
            snprintf(progress_text, sizeof(progress_text), "%d / %d frames (%.0f%%)", current, total, progress * 100.0f);

            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, GetWindowsAccentColor());
            ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f), progress_text);
            ImGui::PopStyleColor();

            ImGui::Spacing();

            // Status message
            if (!status_msg.empty()) {
                if (font_regular) ImGui::PushFont(font_regular);
                ImGui::TextWrapped("%s", status_msg.c_str());
                if (font_regular) ImGui::PopFont();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Estimated time on left, Cancel button flush right
            if (total > 0 && current > 0) {
                static auto start_time = std::chrono::steady_clock::now();
                if (current == 1) {
                    start_time = std::chrono::steady_clock::now();
                }

                auto elapsed = std::chrono::steady_clock::now() - start_time;
                auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

                if (elapsed_sec > 0) {
                    float rate = static_cast<float>(current) / static_cast<float>(elapsed_sec);
                    int remaining_frames = total - current;
                    int eta_sec = (rate > 0) ? static_cast<int>(remaining_frames / rate) : 0;

                    ImGui::Text("ETA: %d:%02d", eta_sec / 60, eta_sec % 60);
                    ImGui::SameLine();
                }
            }

            // Cancel button (flush right)
            float btnPadding = 8.0f * 2;
            float cancelW = ImGui::CalcTextSize("Cancel Transcode").x + btnPadding;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - cancelW);
            PushOutlineButtonStyle();
            if (ImGui::Button("Cancel Transcode")) {
                // Call cancel on project manager's transcoder
                if (project_manager) {
                    project_manager->CancelTranscode();
                    Debug::Log("User requested transcode cancel");
                }
                show_transcode_progress = false;
                ImGui::CloseCurrentPopup();
            }
            PopOutlineButtonStyle();

            ImGui::EndPopup();
        }
    }

    void Application::CreateCacheClearDialogs() {
        // Open error popup when flag is set
        if (show_cannot_clear_exr_cache_error) {
            ImGui::OpenPopup("Cannot Clear EXR Cache");
            show_cannot_clear_exr_cache_error = false;
        }

        // Open success popup when flag is set
        if (show_exr_cache_cleared_success) {
            ImGui::OpenPopup("EXR Cache Cleared");
            show_exr_cache_cleared_success = false;
        }

        // Error popup for clearing image sequence cache while sequences are loaded
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Cannot Clear EXR Cache", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::Text("Cannot clear image sequence cache while sequences are loaded.");
            ImGui::Spacing();
            ImGui::TextWrapped("Ttranscodes are used by loaded image sequences.\nClearing them would corrupt playback.");
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Please remove all EXR sequences from the project first.");
            ImGui::Spacing();

            float btnPadding = 8.0f * 2;
            float okW = ImGui::CalcTextSize("OK").x + btnPadding;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - okW);
            PushOutlineButtonStyle();
            if (ImGui::Button("OK")) {
                ImGui::CloseCurrentPopup();
            }
            PopOutlineButtonStyle();

            ImGui::EndPopup();
        }

        // Success popup showing how much was deleted
        center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("EXR Cache Cleared", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            double mb_cleared = exr_cache_bytes_cleared / 1024.0 / 1024.0;

            ImGui::Text("EXR Cache Cleared Successfully");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextColored(Bright(GetWindowsAccentColor()), "%.2f MB deleted", mb_cleared);
            ImGui::Spacing();
            ImGui::TextWrapped("Cleared EXR transcodes (all locations)");

            ImGui::Spacing();

            float btnPadding = 8.0f * 2;
            float okW = ImGui::CalcTextSize("OK").x + btnPadding;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - okW);
            PushOutlineButtonStyle();
            if (ImGui::Button("OK")) {
                ImGui::CloseCurrentPopup();
            }
            PopOutlineButtonStyle();

            ImGui::EndPopup();
        }
    }

    void Application::HandleCriticalPressure() {
        Debug::Log("SYSTEM CRITICAL: " + std::to_string(last_pressure_status.ram_usage_percent * 100) + "% RAM");

        // Enter emergency mode
        in_emergency_mode = true;

        // CRITICAL: Check if EXR cache is active BEFORE clearing anything
        // (ClearAllCaches will clear EXR cache, setting initialized_=false)
        bool has_exr = video_player && video_player->HasEXRCache();
        Debug::Log("EXR check (before clear): video_player=" + std::string(video_player ? "yes" : "no") +
                   ", HasEXRCache=" + std::string(has_exr ? "yes" : "no"));

        // Save EXR state BEFORE clearing
        if (has_exr) {
            auto before_stats = video_player->GetEXRCacheStats();
            Debug::Log("EXR cache detected (" + std::to_string(before_stats.cached_frames) +
                       " cached frames), saving state for recovery...");

            // Remember that EXR was active so we can restore it on recovery
            exr_cache_was_active = true;
            if (!current_file_path.empty()) {
                exr_video_path_before_shutdown = current_file_path;
                Debug::Log("Saved EXR path for recovery: " + exr_video_path_before_shutdown);
            }

            // Stop playback - without cache, EXR playback won't work
            bool is_playing = video_player->IsPlaying();
            Debug::Log("Playback state: " + std::string(is_playing ? "PLAYING" : "PAUSED"));
            if (is_playing) {
                video_player->Pause();
                Debug::Log("Paused playback (EXR requires cache)");
            }
        } else {
            Debug::Log("No EXR cache to save");
            exr_cache_was_active = false;
        }

        // Priority 1: Clear all caches (NOTE: ClearAllCaches also clears EXR!)
        if (project_manager) {
            size_t before_ram_mb = last_pressure_status.ram_used_mb;
            project_manager->ClearAllCaches();
            Debug::Log("Evicted seek caches + EXR (before: " + std::to_string(before_ram_mb) + " MB RAM)");
        }

        // Disable caching to prevent RAM refill
        cache_enabled = false;
        if (project_manager) {
            project_manager->SetCacheEnabled(false);
        }

        // Show notification in menu bar
        stats_bar_notification_message = "RAM exceeded 92% - caching paused";
        show_notification_permanent = true;  // Keep message visible until recovery
        notification_start_time = std::chrono::steady_clock::now();

        Debug::Log("RAM critical - notification shown in menu bar");
    }

    void Application::RenderPressureCriticalDialog() {
        if (!show_pressure_critical_dialog) return;

        ImGui::OpenPopup("System Critical");

        float scale = ImGui::GetIO().FontGlobalScale;
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(S(600), 0), ImGuiCond_Always);

        if (ImGui::BeginPopupModal("System Critical", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {

            // Show critical warning with Material Icons
            if (font_icons) ImGui::PushFont(font_icons);
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), ICON_WARNING);
            if (font_icons) ImGui::PopFont();

            ImGui::SameLine(0, 8);
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "SYSTEM CRITICAL - EMERGENCY MODE");

            ImGui::Separator();
            ImGui::Spacing();

            // Live RAM status with color coding for recovery
            float ram_percent = last_pressure_status.ram_usage_percent * 100.0f;
            ImVec4 ram_color;
            if (ram_percent >= 90.0f) {
                ram_color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f); // Red - critical
            } else if (ram_percent >= 70.0f) {
                ram_color = ImVec4(1.0f, 0.8f, 0.3f, 1.0f); // Yellow - recovering
            } else {
                ram_color = ImVec4(0.3f, 1.0f, 0.3f, 1.0f); // Green - recovered
            }

            ImGui::TextColored(ram_color, "System RAM: %.0f%% (%.1f GB / %.1f GB)",
                              ram_percent,
                              last_pressure_status.ram_used_mb / 1024.0,
                              last_pressure_status.ram_total_mb / 1024.0);

            // Show recovery status
            ImGui::Spacing();
            if (ram_percent >= 70.0f) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                 "Waiting for recovery... (auto-resumes at < 70%% RAM)");
            } else {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                                 "System recovered! Resuming...");
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextWrapped(
                "Emergency shutdown triggered at 90%% RAM. All caches evicted, "
                "playback paused, and caching disabled."
            );

            ImGui::Spacing();
            ImGui::TextWrapped(
                "This dialog will automatically dismiss when RAM drops below 70%% "
                "and caching will resume."
            );

            ImGui::Spacing();
            ImGui::Text("Recommendations:");
            ImGui::BulletText("Close unused applications to free RAM");
            ImGui::BulletText("Consider reducing cache window (%ds currently)",
                             cache_settings.max_cache_seconds);
            ImGui::BulletText("Work with lower resolution content");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            PushOutlineButtonStyle();
            if (ImGui::Button("Open Settings", ImVec2(S(160), 0))) {
                show_cache_settings = true;
                show_pressure_critical_dialog = false;
                in_emergency_mode = false;
                ImGui::CloseCurrentPopup();
            }
            PopOutlineButtonStyle();

            ImGui::SameLine();

            PushOutlineButtonStyle();
            if (ImGui::Button("Dismiss", ImVec2(S(120), 0))) {
                show_pressure_critical_dialog = false;
                in_emergency_mode = false;
                ImGui::CloseCurrentPopup();
            }
            PopOutlineButtonStyle();

            ImGui::SameLine();

            PushOutlineButtonStyle();
            if (ImGui::Button("Close QCView", ImVec2(S(120), 0))) {
                Debug::Log("User requested app shutdown from critical dialog");
                exit(0);
            }
            PopOutlineButtonStyle();

            ImGui::EndPopup();
        }
    }

    void Application::CreateCacheSettingsWindow() {
        // Open modal popup when flag is set
        if (show_cache_settings) {
            ImGui::OpenPopup("QCView Settings");
            show_cache_settings = false; // Reset flag, modal will handle its own state
        }

        static bool settings_changed = false;

        // Set popup to center on screen — 80% of viewport
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 center = viewport->GetCenter();
        ImVec2 viewport_size = viewport->Size;

        float max_width = viewport_size.x * 0.8f;
        float max_height = viewport_size.y * 0.8f;

        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(max_width, max_height), ImGuiCond_Always);

        bool open = true;

        ImGuiWindowFlags modal_flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
        if (ImGui::BeginPopupModal("QCView Settings", &open, modal_flags)) {

            ImGui::Text("QCView Settings");
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Configure video processing, cache, and memory settings");
            ImGui::Separator();

            // Update pipeline info from current media
            bool is_exr_mode = video_player && video_player->IsInEXRMode();
            bool is_image_sequence = project_manager && project_manager->IsInImageSequenceMode();

            if (is_exr_mode) {
                // EXR sequences always use Float16 pipeline
                cache_settings.current_pipeline_mode = PipelineMode::HDR_RES;  // HDR_RES = half-float for EXR
            } else if (is_image_sequence) {
                // Image sequences use auto-detected pipeline from first frame
                cache_settings.current_pipeline_mode = project_manager->GetImageSequencePipelineMode();
            } else if (video_player) {
                // Regular video pipeline mode
                cache_settings.current_pipeline_mode = video_player->GetPipelineMode();
            }

            // Show current pipeline mode (read-only for EXR/image sequences, changeable for video)
            ImGui::Text("Current Pipeline Mode:");
            auto it = PIPELINE_CONFIGS.find(cache_settings.current_pipeline_mode);
            if (it != PIPELINE_CONFIGS.end()) {
                if (font_regular) ImGui::PushFont(font_regular);
                if (is_exr_mode) {
                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s (EXR - Fixed)", it->second.description.c_str());
                } else if (is_image_sequence) {
                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s (Image Sequence - Auto-detected)", it->second.description.c_str());
                } else {
                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s (Video)", it->second.description.c_str());
                }
                if (font_regular) ImGui::PopFont();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Tabbed layout for organized settings
            if (ImGui::BeginChild("SettingsContent", ImVec2(0, S(-60)), false)) {
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.25f, 0.25f, 0.50f));
                if (ImGui::BeginTabBar("SettingsTabs", ImGuiTabBarFlags_None)) {

                    // === TAB 1: Video Decode (hidden on macOS — VideoToolbox always available on Apple Silicon) ===
#ifndef __APPLE__
                    if (ImGui::BeginTabItem("Video Decode")) {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Hardware acceleration settings for video decode");
                    ImGui::Spacing();

                    if (ImGui::Checkbox("Disable hardware acceleration", &cache_settings.force_software_decode)) {
                        settings_changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Forces software (CPU) video decoding.\n\n"
#ifdef _WIN32
                            "When enabled, D3D11VA and NVDEC hardware decode are bypassed\n"
#elif defined(__APPLE__)
                            "When enabled, VideoToolbox hardware decode is bypassed\n"
#else
                            "When enabled, VA-API hardware decode is bypassed\n"
#endif
                            "and FFmpeg software decode is used for all codecs.\n\n"
                            "Use this if you experience visual artifacts, crashes, or\n"
                            "black frames with hardware-accelerated codecs.\n\n"
                            "Requires reopening the current file to take effect.");
                    }

                    ImGui::Spacing();
                    if (font_regular) ImGui::PushFont(font_regular);
                    if (cache_settings.force_software_decode) {
                        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Software decode active — hardware acceleration disabled");
                    } else {
                        ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "Hardware acceleration enabled (auto-selects HW/SW per codec)");
                    }
                    if (font_regular) ImGui::PopFont();

                        ImGui::EndTabItem();
                    } // End Video Decode tab
#endif

                    // === TAB 2: Image Playback ===
                    if (ImGui::BeginTabItem("Image Playback")) {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Cache window settings for image sequences (EXR/TIFF/PNG/JPEG)");
                    ImGui::Spacing();

                    // Read-Ahead Frames
                    ImGui::Text("Read-Ahead Frames:");
                    if (ImGui::SliderInt("##ImageSeqReadAhead", &g_exr_read_ahead_frames, 24, 600)) {
                        settings_changed = true;
                        // Live update DirectEXRCache if active
                        if (timeline_view && timeline_view->GetPlaybackController()) {
                            if (auto* cache = timeline_view->GetPlaybackController()->GetCache()) {
                                cache->UpdateDirectEXRCacheConfig(g_exr_read_ahead_frames, g_read_behind_frames, g_exr_thread_count);
                            }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Number of frames to cache ahead of playhead.\n\n"
                            "More frames = smoother playback, higher RAM usage\n"
                            "Fewer frames = lower RAM usage, may stutter on slow storage\n\n"
                            "Examples @ 24fps:\n"
                            "  - 72 frames = 3 seconds (default)\n"
                            "  - 180 frames = 7.5 seconds\n"
                            "  - 360 frames = 15 seconds\n"
                            "  - 600 frames = 25 seconds\n\n"
                            "RAM usage depends on resolution:\n"
                            "  - 4K EXR: ~63 MB/frame\n"
                            "  - 4K TIFF 16-bit: ~33 MB/frame");
                    }

                    // Calculate and display time/RAM estimate
                    double estimated_seconds = g_exr_read_ahead_frames / 24.0;
                    double estimated_ram_gb = (g_exr_read_ahead_frames * 63.0) / 1024.0;  // 4K EXR estimate
                    ImGui::Spacing();
                    if (font_regular) ImGui::PushFont(font_regular);
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                        "Cache: %.1f seconds @ 24fps (~%.1f GB for 4K EXR)",
                        estimated_seconds, estimated_ram_gb);
                    if (font_regular) ImGui::PopFont();

                    // Read-Behind Frames
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::Text("Read-Behind Frames:");
                    if (ImGui::SliderInt("##ReadBehind", &g_read_behind_frames, 0, 120)) {
                        settings_changed = true;
                        // Live update DirectEXRCache if active
                        if (timeline_view && timeline_view->GetPlaybackController()) {
                            if (auto* cache = timeline_view->GetPlaybackController()->GetCache()) {
                                cache->UpdateDirectEXRCacheConfig(g_exr_read_ahead_frames, g_read_behind_frames, g_exr_thread_count);
                            }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Frames to keep cached behind current frame.\nUseful for smooth reverse scrubbing.\n\n12 frames = ~0.5s @ 24fps");
                    }

                        ImGui::EndTabItem();
                    } // End Playback tab

                    // === TAB 3: Image Threading ===
                    if (ImGui::BeginTabItem("Image Threading")) {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Thread settings for I/O and transcoding");
                    ImGui::Spacing();

                    // OpenEXR Threading Info
                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "OpenEXR Multi-Threading");
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Automatic multi-threaded DWAB/ZIP decompression");
                    ImGui::Spacing();

                    int hw_threads = std::thread::hardware_concurrency();
                    if (font_regular) ImGui::PushFont(font_regular);
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                        "OpenEXR Threads: %d (auto-detected from CPU)", hw_threads);
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                        "Thread Pool: std::async (reuses threads efficiently)");
                    if (font_regular) ImGui::PopFont();

                    // === I/O Threading Settings ===
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "I/O Threading");
                    ImGui::Spacing();

                    // Image Sequence Thread Count
                    ImGui::Text("Parallel I/O Threads:");
                    if (ImGui::SliderInt("##ImageSeqThreadCountIS", &g_exr_thread_count, 1, 32)) {
                        settings_changed = true;
                        // Live update DirectEXRCache if active
                        if (timeline_view && timeline_view->GetPlaybackController()) {
                            if (auto* cache = timeline_view->GetPlaybackController()->GetCache()) {
                                cache->UpdateDirectEXRCacheConfig(g_exr_read_ahead_frames, g_read_behind_frames, g_exr_thread_count);
                            }
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Number of parallel threads for image sequence loading.\n\n"
                            "More threads = faster cache filling\n"
                            "Works for all formats: EXR, TIFF, PNG, JPEG\n\n"
                            "Recommended:\n"
                            "  - 8-16 threads for fast SSDs\n"
                            "  - 4-8 threads for HDDs\n"
                            "  - 16-32 threads for network storage");
                    }

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                        "Current: %d parallel loaders", g_exr_thread_count);

                    // === Transcode Threading Settings ===
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "Transcode Threading");
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Parallel transcoding for EXR single-layer conversion");
                    ImGui::Spacing();

                    // Transcode Thread Count
                    ImGui::Text("Parallel Transcode Threads:");
                    if (ImGui::SliderInt("##TranscodeThreadCountIS", &g_exr_transcode_threads, 1, 16)) {
                        settings_changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Number of frames transcoded in parallel.\n\n"
                            "When converting multilayer EXR to single-layer:\n"
                            "  - Read EXR: Parallel\n"
                            "  - Resize (if enabled): Parallel using stb_image_resize2\n"
                            "  - Write EXR: Parallel\n\n"
                            "Recommended: 4-8 threads for balanced performance");
                    }

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                        "Current: %d parallel transcoders", g_exr_transcode_threads);

                        ImGui::EndTabItem();
                    } // End Threading tab

                    // === TAB 4: Disk Cache ===
                    if (ImGui::BeginTabItem("Image Transcode Cache")) {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Storage settings for EXR transcode files");
                    ImGui::Spacing();

                    // Custom Cache Path
                    ImGui::Text("Cache Location:");
                    ImGui::PushItemWidth(-1);
                    if (font_regular) ImGui::PushFont(font_regular);
                    if (g_custom_cache_path.empty()) {
#ifdef _WIN32
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Default: %%LOCALAPPDATA%%\\qcview\\");
#elif defined(__APPLE__)
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Default: ~/Library/Application Support/qcview/");
#else
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Default: ~/.cache/qcview/");
#endif
                    } else {
                        ImGui::TextColored(MutedLight(GetWindowsAccentColor()), "%s", g_custom_cache_path.c_str());
                    }
                    if (font_regular) ImGui::PopFont();
                    ImGui::PopItemWidth();

                    PushOutlineButtonStyle();
                    if (ImGui::Button("Change Cache Folder...", ImVec2(-1, 0))) {
                        nfdchar_t* out_path = nullptr;
                        nfdresult_t result = NFD_PickFolder(&out_path, nullptr);
                        if (result == NFD_OKAY) {
                            g_custom_cache_path = out_path;
                            settings_changed = true;
                            NFD_FreePath(out_path);
                        }
                    }
                    PopOutlineButtonStyle();

                    PushOutlineButtonStyle();
                    if (ImGui::Button("Reset to Default", ImVec2(-1, 0))) {
                        g_custom_cache_path = "";
                        settings_changed = true;
                    }
                    PopOutlineButtonStyle();

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Cache Retention Days
                    ImGui::Text("Cache Retention:");
                    if (ImGui::SliderInt("##CacheRetentionDays", &g_cache_retention_days, 1, 30, "%d days")) {
                        settings_changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Auto-delete cache files older than N days on startup.\n\n"
                            "Files are only deleted when app launches.\n"
                            "Default: 7 days");
                    }

                    // Transcode Cache Size Limit
                    ImGui::Spacing();
                    ImGui::Text("Transcode Cache Limit:");
                    if (ImGui::SliderInt("##TranscodeCacheMaxGB", &g_transcode_cache_max_gb, 5, 100, "%d GB")) {
                        settings_changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Maximum disk space for EXR transcode cache.\n\n"
                            "When exceeded on startup, oldest 50%% of transcodes are deleted.\n"
                            "Default: 10 GB");
                    }

                    // Clear Cache on Exit
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    if (ImGui::Checkbox("Clear transcode cache on exit", &g_clear_cache_on_exit)) {
                        settings_changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(?)");
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Delete all EXR transcodes on app close.\n\n"
                            "WARNING: This will block app shutdown until cleanup completes.\n"
                            "May take several seconds if you have large caches.\n\n"
                            "Default: OFF (lazy cleanup on next launch is recommended)");
                    }

                        ImGui::EndTabItem();
                    } // End Disk Cache tab

                    // === TAB 6: Audio Sync ===
                    if (ImGui::BeginTabItem("Audio Sync")) {
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                            "A/V sync compensation for Float/HDR video mode (AudioMixer)");
                        ImGui::Spacing();

                        // Display Latency Preset
                        ImGui::TextColored(Bright(GetWindowsAccentColor()), "Display Latency");
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                            "Select your monitor's refresh rate for automatic latency compensation");
                        ImGui::Spacing();

                        // Preset dropdown
                        const char* display_presets[] = {
                            "60 Hz (~33ms)", "75 Hz (~27ms)", "120 Hz (~17ms)",
                            "144 Hz (~14ms)", "240 Hz (~8ms)", "Custom"
                        };
                        const float preset_latencies_ms[] = { 33.0f, 27.0f, 17.0f, 14.0f, 8.0f, 0.0f };

                        ImGui::Text("Refresh Rate:");
                        if (ImGui::Combo("##DisplayPreset", &cache_settings.display_latency_preset,
                                         display_presets, IM_ARRAYSIZE(display_presets))) {
                            settings_changed = true;

                            // Apply to AudioMixer if timeline is active
                            if (timeline_view && timeline_view->GetPlaybackController() && timeline_view->GetPlaybackController()->GetAudioMixer()) {
                                float latency_ms = (cache_settings.display_latency_preset == 5)
                                    ? cache_settings.custom_display_latency_ms
                                    : preset_latencies_ms[cache_settings.display_latency_preset];
                                timeline_view->GetPlaybackController()->GetAudioMixer()->SetDisplayLatency(latency_ms / 1000.0);
                            }
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("(?)");
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip(
                                "Compensates for video display latency.\n\n"
                                "Modern displays have ~2 frame latency between\n"
                                "receiving a frame and showing it on screen.\n\n"
                                "Select your monitor's refresh rate for automatic\n"
                                "latency compensation. Use 'Custom' for fine control.");
                        }

                        // Custom latency slider (only visible when Custom is selected)
                        if (cache_settings.display_latency_preset == 5) {
                            ImGui::Text("Custom Latency (ms):");
                            if (ImGui::SliderFloat("##CustomLatency", &cache_settings.custom_display_latency_ms,
                                                   0.0f, 100.0f, "%.1f ms")) {
                                settings_changed = true;
                                if (timeline_view && timeline_view->GetPlaybackController() && timeline_view->GetPlaybackController()->GetAudioMixer()) {
                                    timeline_view->GetPlaybackController()->GetAudioMixer()->SetDisplayLatency(
                                        cache_settings.custom_display_latency_ms / 1000.0);
                                }
                            }
                        }

                        // Show current effective latency
                        float current_latency_ms = (cache_settings.display_latency_preset == 5)
                            ? cache_settings.custom_display_latency_ms
                            : preset_latencies_ms[cache_settings.display_latency_preset];
                        if (font_regular) ImGui::PushFont(font_regular);
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                            "Display latency: %.1f ms", current_latency_ms);
                        if (font_regular) ImGui::PopFont();

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        // Fine-Tune Offset
                        ImGui::TextColored(Bright(GetWindowsAccentColor()), "Fine-Tune Offset");
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                            "Manual adjustment for systems with unusual latency");
                        ImGui::Spacing();

                        ImGui::Text("Audio Offset (ms):");
                        if (ImGui::SliderFloat("##AudioFineTune", &cache_settings.audio_fine_tune_ms,
                                               -50.0f, 50.0f, "%.1f ms")) {
                            settings_changed = true;
                            if (timeline_view && timeline_view->GetPlaybackController() && timeline_view->GetPlaybackController()->GetAudioMixer()) {
                                timeline_view->GetPlaybackController()->GetAudioMixer()->SetFineTuneOffset(
                                    cache_settings.audio_fine_tune_ms / 1000.0);
                            }
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("(?)");
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip(
                                "Fine-tune audio timing manually.\n\n"
                                "Positive values delay audio (audio plays later)\n"
                                "Negative values advance audio (audio plays earlier)\n\n"
                                "Tip: Use a sync test video with a clapper/beep+flash\n"
                                "to dial in the perfect offset for your system.");
                        }

                        // Reset button
                        PushOutlineButtonStyle();
                        if (ImGui::Button("Reset Fine-Tune", ImVec2(-1, 0))) {
                            cache_settings.audio_fine_tune_ms = 0.0f;
                            settings_changed = true;
                            if (timeline_view && timeline_view->GetPlaybackController() && timeline_view->GetPlaybackController()->GetAudioMixer()) {
                                timeline_view->GetPlaybackController()->GetAudioMixer()->SetFineTuneOffset(0.0);
                            }
                        }
                        PopOutlineButtonStyle();

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        // Live Status (if playing)
                        ImGui::TextColored(Bright(GetWindowsAccentColor()), "Live Status");
                        if (timeline_view && timeline_view->GetPlaybackController() && timeline_view->GetPlaybackController()->GetAudioMixer()) {
                            auto* mixer = timeline_view->GetPlaybackController()->GetAudioMixer();
                            double effective_offset = mixer->GetEffectiveOffset();
                            double wasapi_latency = mixer->GetWasapiLatency();
                            double tempo = mixer->GetPlaybackTempo();
                            bool time_stretch_active = mixer->IsTimeStretchAvailable() && std::abs(tempo - 1.0) > 0.001;

                            if (font_regular) ImGui::PushFont(font_regular);
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                "Effective offset: %.1f ms", effective_offset * 1000.0);
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
#ifdef _WIN32
                                "WASAPI buffer: %.1f ms", wasapi_latency * 1000.0);
#else
                                "Audio buffer: %.1f ms", wasapi_latency * 1000.0);
#endif
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                "Playback tempo: %.0f%%", tempo * 100.0);
                            ImGui::TextColored(time_stretch_active ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                "Time stretch: %s", time_stretch_active ? "Active" : "Inactive");
                            if (font_regular) ImGui::PopFont();
                        } else {
                            if (font_regular) ImGui::PushFont(font_regular);
                            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                "No active AudioMixer (load a timeline to see live stats)");
                            if (font_regular) ImGui::PopFont();
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        // Info box
                        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
#ifdef _WIN32
                            "Note: These settings only affect Float/HDR video mode\n"
                            "(D3D11/D3D11VA decoders with AudioMixer).\n"
                            "Video mode has its own internal A/V sync.");
#else
                            "Note: These settings affect video playback\n"
                            "with the AudioMixer audio backend.");
#endif

                        ImGui::EndTabItem();
                    } // End Audio Sync tab

                    // === HDR Display tab ===
                    if (ImGui::BeginTabItem("HDR Display")) {
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "HDR output and display settings");
                        ImGui::Spacing();

#ifdef QCVIEW_USE_VULKAN
                        if (vulkan_swapchain_) {
                            bool hdr_supported = vulkan_swapchain_->IsHDRSupported();
                            bool hdr_active = vulkan_swapchain_->IsHDRActive();

                            // HDR status
                            ImGui::TextColored(Bright(GetWindowsAccentColor()), "Status");
                            if (font_regular) ImGui::PushFont(font_regular);
                            if (hdr_supported) {
                                ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f),
                                    "HDR10 (ST.2084 PQ) supported");
                            } else {
                                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                                    "HDR not supported on this display/compositor");
                            }

                            auto& info = vulkan_swapchain_->GetHDRInfo();
                            if (hdr_supported) {
                                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                    "Display: %.0f - %.0f nits (%.0f avg)",
                                    info.min_luminance, info.max_luminance, info.max_full_frame_luminance);
                            }
                            if (font_regular) ImGui::PopFont();

                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();

                            // HDR toggle
                            ImGui::TextColored(Bright(GetWindowsAccentColor()), "HDR Output");
                            ImGui::Spacing();
                            if (!hdr_supported) ImGui::BeginDisabled();
                            bool toggle_hdr = hdr_active;
                            if (ImGui::Checkbox("Enable HDR output", &toggle_hdr)) {
                                vulkan_swapchain_->SetHDREnabled(toggle_hdr);
                                settings_changed = true;
                            }
                            if (!hdr_supported) ImGui::EndDisabled();

                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();

                            // UI brightness slider (only meaningful when HDR active)
                            ImGui::TextColored(Bright(GetWindowsAccentColor()), "UI Brightness");
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                "Controls the brightness of UI elements in HDR mode");
                            ImGui::Spacing();

                            if (!hdr_active) ImGui::BeginDisabled();
                            ImGui::SetNextItemWidth(-1);
                            if (ImGui::SliderFloat("##HDRUIBrightness", &hdr_ui_nits_, 80.0f, 1000.0f, "%.0f nits")) {
                                vulkan_swapchain_->SetUIBrightness(hdr_ui_nits_);
                                qcview::SetHDRUIBrightness(hdr_ui_nits_);
                                settings_changed = true;
                            }
                            if (!hdr_active) ImGui::EndDisabled();

                            ImGui::Spacing();
                            if (font_regular) ImGui::PushFont(font_regular);
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                "Recommended: Match your display's typical brightness.\n"
                                "400 nits is a good default for most HDR displays.");
                            if (font_regular) ImGui::PopFont();

                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();

                            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                "Note: For true HDR output, ensure HDR is also enabled\n"
                                "in your system display settings (e.g. KDE Display\n"
                                "Configuration, GNOME Settings, etc).");
                        } else {
                            if (font_regular) ImGui::PushFont(font_regular);
                            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                "Vulkan swapchain not initialized");
                            if (font_regular) ImGui::PopFont();
                        }
#elif defined(QCVIEW_USE_METAL)
                        if (metal_swapchain_) {
                            bool hdr_supported = metal_swapchain_->IsHDRSupported();
                            bool hdr_active = metal_swapchain_->IsHDRActive();
                            float max_headroom = metal_swapchain_->GetMaxEDRHeadroom();

                            // EDR status
                            ImGui::TextColored(Bright(GetWindowsAccentColor()), "Status");
                            if (font_regular) ImGui::PushFont(font_regular);
                            if (hdr_supported) {
                                ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f),
                                    "EDR (Extended Dynamic Range) supported");
                                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                    "Max EDR headroom: %.1fx", max_headroom);
                            } else {
                                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                                    "EDR not supported on this display");
                            }
                            if (font_regular) ImGui::PopFont();

                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();

                            // HDR toggle
                            ImGui::TextColored(Bright(GetWindowsAccentColor()), "EDR Output");
                            ImGui::Spacing();
                            if (!hdr_supported) ImGui::BeginDisabled();
                            bool toggle_hdr = hdr_active;
                            if (ImGui::Checkbox("Enable EDR output", &toggle_hdr)) {
                                metal_swapchain_->SetHDREnabled(toggle_hdr);
                                settings_changed = true;
                            }
                            if (!hdr_supported) ImGui::EndDisabled();

                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();

                            // EDR colorspace selector
                            ImGui::TextColored(Bright(GetWindowsAccentColor()), "EDR Colorspace");
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                "Determines the color encoding of the framebuffer.\n"
                                "Match this to your OCIO output transform.");
                            ImGui::Spacing();

                            if (!hdr_active) ImGui::BeginDisabled();
                            auto current_cs = metal_swapchain_->GetEDRColorspace();
                            ImGui::SetNextItemWidth(-1);
                            if (ImGui::BeginCombo("##EDRColorspace", qcview::EDRColorspaceName(current_cs))) {
                                for (int i = 0; i < static_cast<int>(qcview::EDRColorspace::Count); i++) {
                                    auto cs = static_cast<qcview::EDRColorspace>(i);
                                    bool selected = (cs == current_cs);
                                    if (ImGui::Selectable(qcview::EDRColorspaceName(cs), selected)) {
                                        metal_swapchain_->SetEDRColorspace(cs);
                                        settings_changed = true;
                                    }
                                    if (selected) ImGui::SetItemDefaultFocus();
                                }
                                ImGui::EndCombo();
                            }
                            if (!hdr_active) ImGui::EndDisabled();

                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();

                            // UI brightness slider (EDR scale: 1.0 = SDR white)
                            ImGui::TextColored(Bright(GetWindowsAccentColor()), "UI Brightness");
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                "Controls the brightness of UI elements in EDR mode.\n"
                                "1.0 = SDR white (normal brightness).");
                            ImGui::Spacing();

                            if (!hdr_active) ImGui::BeginDisabled();
                            static float edr_ui_scale = metal_swapchain_->GetUIBrightness();
                            ImGui::SetNextItemWidth(-1);
                            if (ImGui::SliderFloat("##EDRUIScale", &edr_ui_scale, 0.5f, 3.0f, "%.2fx")) {
                                metal_swapchain_->SetUIBrightness(edr_ui_scale);
                                settings_changed = true;
                            }
                            if (!hdr_active) ImGui::EndDisabled();

                            ImGui::Spacing();
                            if (font_regular) ImGui::PushFont(font_regular);
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                "1.0x = SDR white (recommended default).\n"
                                "macOS maps SDR white to display brightness automatically.");
                            if (font_regular) ImGui::PopFont();
                        } else {
                            if (font_regular) ImGui::PushFont(font_regular);
                            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                "Metal HDR swapchain not initialized");
                            if (font_regular) ImGui::PopFont();
                        }
#elif defined(_WIN32)
                        {
                            bool hdr_supported = qcview::HDROutputManager::Instance().IsHDRSupported();
                            bool hdr_active = qcview::HDROutputManager::Instance().IsHDRActive();

                            // HDR status
                            ImGui::TextColored(Bright(GetWindowsAccentColor()), "Status");
                            if (font_regular) ImGui::PushFont(font_regular);
                            if (hdr_supported) {
                                ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f),
                                    "HDR supported (Windows HDR enabled)");
                            } else {
                                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                                    "HDR not available — enable HDR in Windows Display Settings");
                            }
                            if (font_regular) ImGui::PopFont();

                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();

                            // UI brightness slider
                            ImGui::TextColored(Bright(GetWindowsAccentColor()), "UI Brightness");
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                "Controls the brightness of UI elements in HDR mode");
                            ImGui::Spacing();

                            if (!hdr_active) ImGui::BeginDisabled();
                            ImGui::SetNextItemWidth(-1);
                            if (ImGui::SliderFloat("##HDRUIBrightness", &hdr_ui_nits_, 80.0f, 1000.0f, "%.0f nits")) {
                                qcview::SetHDRUIBrightness(hdr_ui_nits_);
                                settings_changed = true;
                            }
                            if (!hdr_active) ImGui::EndDisabled();

                            ImGui::Spacing();
                            if (font_regular) ImGui::PushFont(font_regular);
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                "Recommended: Match your display's typical brightness.\n"
                                "400 nits is a good default for most HDR displays.");
                            if (font_regular) ImGui::PopFont();

                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();

                            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                "Note: HDR output requires Windows HDR to be enabled\n"
                                "in Settings > System > Display > HDR.");
                        }
#endif

                        ImGui::EndTabItem();
                    } // End HDR Display tab

                    // === TAB: General ===
                    if (ImGui::BeginTabItem("General")) {
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "General application settings");
                        ImGui::Spacing();

                        ImGui::Text("Screenshots:");
                        ImGui::Spacing();
                        if (ImGui::Checkbox("Bake annotations on screenshots", &cache_settings.bake_annotations_on_screenshots)) {
                            settings_changed = true;
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("(?)");
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip(
                                "When enabled, screenshots (Ctrl+[ and Ctrl+]) will\n"
                                "include any annotation drawings on the current frame.\n\n"
                                "When disabled, screenshots capture the clean video only.");
                        }

                        ImGui::EndTabItem();
                    } // End General tab

                    ImGui::EndTabBar();
                } // End tab bar
                ImGui::PopStyleColor();  // Border
                ImGui::EndChild();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Buttons (flush right)
            float btnPadding = 8.0f * 2;
            float applyW = ImGui::CalcTextSize("Apply Settings").x + btnPadding;
            float resetW = ImGui::CalcTextSize("Reset to Defaults").x + btnPadding;
            float closeW = ImGui::CalcTextSize("Close").x + btnPadding;
            float btnSpacing = ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - applyW - resetW - closeW - btnSpacing * 2);

            PushOutlineButtonStyle();
            if (ImGui::Button("Apply Settings") && settings_changed) {
                // Apply pipeline mode change (only for regular video, not EXR/image sequences)
                bool is_exr_mode = video_player && video_player->IsInEXRMode();
                bool is_image_sequence = project_manager && project_manager->IsInImageSequenceMode();

                if (!is_exr_mode && !is_image_sequence) {
                    // Regular video: allow pipeline mode changes
                    if (video_player && cache_settings.current_pipeline_mode != video_player->GetPipelineMode()) {
                        // Brute-force reload: set mode and force full reload
                        if (project_manager) {
                            project_manager->SetProjectPipelineMode(cache_settings.current_pipeline_mode);
                        }
                        video_player->SetPipelineMode(cache_settings.current_pipeline_mode);
                        Debug::Log("Applied pipeline mode: " + std::string(PipelineModeToString(cache_settings.current_pipeline_mode)) + " - deferring full reload");
                        g_force_reload_pending = true;  // Defer to next frame start
                    }
                } else {
                    // EXR/image sequences: pipeline mode is locked, just log
                    if (is_exr_mode) {
                        Debug::Log("Pipeline mode locked for EXR sequences (Float16 fixed)");
                    } else if (is_image_sequence) {
                        Debug::Log("Pipeline mode locked for image sequences (auto-detected from bit depth)");
                    }
                }

                // Apply cache configuration
                if (project_manager) {
                    // Create FrameCache::CacheConfig for seconds-based cache
                    FrameCache::CacheConfig config;
                    config.max_cache_seconds = cache_settings.max_cache_seconds; // NEW: Seconds-based limit

                    // Set pipeline mode for texture format matching
                    config.pipeline_mode = cache_settings.current_pipeline_mode;

                    // Set RAM cache defaults
                    config.use_centered_caching = true;         // Focus on current position
                    config.cache_width = 1920;                  // Standard resolution for consistent quality
                    config.cache_height = -1;                   // Calculate from aspect ratio
                    config.adaptive_threading = true;           // Slow down during playback
                    config.max_extractions_per_second = 100;    // Reasonable limit for background caching
                    config.enable_nvidia_decode = cache_settings.enable_nvidia_decode;  // Hardware decode setting

                    // Apply threading settings
                    config.max_batch_size = cache_settings.max_batch_size;
                    config.max_concurrent_batches = cache_settings.max_concurrent_batches;

                    project_manager->SetCacheConfig(config);

                    Debug::Log("Applied cache settings: " + std::to_string(cache_settings.max_cache_seconds) + "s cache window for " +
                               std::string(PipelineModeToString(cache_settings.current_pipeline_mode)) + " mode");
                }

                // Apply EXR-specific settings (NEW: DirectEXRCacheConfig)
                if (video_player) {
                    qcview::DirectEXRCacheConfig exr_config = GetCurrentEXRCacheConfig();

                    // Apply config through VideoPlayer
                    if (video_player->HasEXRCache()) {
                        video_player->SetEXRCacheConfig(exr_config);

                        Debug::Log("Applied image sequence cache settings: " +
                                   std::to_string(exr_config.readAheadFrames) + " frames ahead, " +
                                   std::to_string(exr_config.readBehindFrames) + " frames behind, " +
                                   std::to_string(exr_config.threadCount) + " threads");
                    } else {
                        Debug::Log("Image sequence cache settings configured (will apply when sequence is loaded)");
                    }

                    // Apply disk cache settings to EXRTranscoder
                    video_player->SetCacheSettings(g_custom_cache_path, g_cache_retention_days,
                                                   g_transcode_cache_max_gb, g_clear_cache_on_exit);
                    Debug::Log("Applied disk cache settings: retention=" + std::to_string(g_cache_retention_days) +
                              " days, transcode limit=" + std::to_string(g_transcode_cache_max_gb) + " GB" +
                              ", clear on exit=" + std::string(g_clear_cache_on_exit ? "ON" : "OFF"));

                    // Disk cache settings applied above
                }

                // Save settings to disk
                SaveSettings();

                settings_changed = false;

                // Close the settings window
                ImGui::CloseCurrentPopup();
            }
            PopOutlineButtonStyle();

            ImGui::SameLine();

            PushOutlineButtonStyle();
            if (ImGui::Button("Reset to Defaults")) {
                // Video decode settings
                cache_settings.force_software_decode = false;

                // Image Sequence settings - Auto-configure based on CPU
                AutoConfigureEXRThreading(cache_settings);
                g_exr_read_ahead_frames = 72;   // ~3s @ 24fps
                g_read_behind_frames = 12;      // ~0.5s @ 24fps

                // HDR settings
                hdr_ui_nits_ = 400.0f;
                qcview::SetHDRUIBrightness(hdr_ui_nits_);
#ifdef QCVIEW_USE_VULKAN
                if (vulkan_swapchain_ && vulkan_swapchain_->IsHDRActive()) {
                    vulkan_swapchain_->SetUIBrightness(hdr_ui_nits_);
                }
#endif

                // Screenshot settings
                cache_settings.bake_annotations_on_screenshots = true;

                // Disk cache settings
                g_custom_cache_path = "";
                g_cache_retention_days = 7;
                g_transcode_cache_max_gb = 10;
                g_clear_cache_on_exit = false;

                // Timeline cache settings
                g_timeline_read_ahead_frames = 16;       // Frames ahead
                g_timeline_read_behind_frames = 4;       // Frames behind
                g_timeline_max_textures = 120;           // Max GPU textures
                g_timeline_io_threads = 8;               // Background I/O threads
                g_video_decode_threads = 4;              // FFmpeg decode threads

                // Apply timeline defaults to active cache if present
                if (timeline_view && timeline_view->HasPlaybackController()) {
                    auto* active_cache = timeline_view->GetEffectivePlaybackController()->GetCache();
                    if (active_cache) {
                        auto config = active_cache->GetConfig();
                        config.readAheadFrames = g_timeline_read_ahead_frames;
                        config.readBehindFrames = g_timeline_read_behind_frames;
                        config.max_textures = g_timeline_max_textures;
                        config.decodeThreads = g_video_decode_threads;
                        active_cache->SetConfig(config);
                    }
                }

                settings_changed = true;

                // Save the defaults immediately
                SaveSettings();
                Debug::Log("Settings reset to defaults and saved");
            }
            PopOutlineButtonStyle();

            ImGui::SameLine();

            PushOutlineButtonStyle();
            if (ImGui::Button("Close")) {
                ImGui::CloseCurrentPopup();
            }
            PopOutlineButtonStyle();

            // Handle X button click
            if (!open) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    void Application::CreateFontSettingsWindow() {
        // Open modal popup when flag is set
        if (show_font_settings_window) {
            ImGui::OpenPopup("UI Scale");
            show_font_settings_window = false;
        }

        // Centered, compact modal
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 center = viewport->GetCenter();

        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(S(400), 0), ImGuiCond_Always);

        bool open = true;

        ImGuiWindowFlags modal_flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
        if (ImGui::BeginPopupModal("UI Scale", &open, modal_flags)) {

            ImGui::Text("UI Scale");
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Adjust the user interface size (requires restart)");
            ImGui::Separator();
            ImGui::Spacing();

            // UI Scale slider
            static float ui_scale_temp = g_ui_scale;

            // Reset temp to current on first open
            static bool first_open = true;
            if (first_open) {
                ui_scale_temp = g_ui_scale;
                first_open = false;
            }

            ImGui::Text("Scale:");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##uiscale", &ui_scale_temp, 0.5f, 1.5f, "%.2fx");

            ImGui::Spacing();

            // Preset buttons
            PushOutlineButtonStyle();
            if (ImGui::Button("Small")) { ui_scale_temp = 0.75f; }
            PopOutlineButtonStyle();
            ImGui::SameLine();
            PushOutlineButtonStyle();
            if (ImGui::Button("Default")) { ui_scale_temp = 1.0f; }
            PopOutlineButtonStyle();
            ImGui::SameLine();
            PushOutlineButtonStyle();
            if (ImGui::Button("Large")) { ui_scale_temp = 1.25f; }
            PopOutlineButtonStyle();

            ImGui::Spacing();

            if (ui_scale_temp != g_ui_scale) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Restart required to apply changes");
            } else {
                ImGui::TextDisabled("Current: %.2fx", g_ui_scale);
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Bottom buttons (flush right)
            float btnPadding = S(8.0f) * 2;
            float saveW = ImGui::CalcTextSize("Save").x + btnPadding;
            float cancelW = ImGui::CalcTextSize("Cancel").x + btnPadding;
            float btnSpacing = ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - saveW - cancelW - btnSpacing);

            PushOutlineButtonStyle();
            if (ImGui::Button("Save")) {
                g_ui_scale = ui_scale_temp;
                SaveSettings();
                Debug::Log("UI scale changed to " + std::to_string(g_ui_scale) + "x");
                ImGui::CloseCurrentPopup();
                first_open = true;
            }
            PopOutlineButtonStyle();

            ImGui::SameLine();

            PushOutlineButtonStyle();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
                first_open = true;
            }
            PopOutlineButtonStyle();

            // Handle X button click
            if (!open) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    // ============================================================================
    // KEYBOARD SHORTCUTS POPUP
    // ============================================================================
    void Application::CreateKeyboardShortcutsPopup() {
        // Open modal popup when flag is set
        if (show_shortcuts_popup) {
            ImGui::OpenPopup("Keyboard Shortcuts");
            show_shortcuts_popup = false;
        }

        // Center popup on screen — 80% of viewport
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 center = viewport->GetCenter();
        ImVec2 viewport_size = viewport->Size;

        float max_width = viewport_size.x * 0.8f;
        float max_height = viewport_size.y * 0.8f;

        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(max_width, max_height), ImGuiCond_Always);

        // State for press-to-capture
        static std::string capturing_id;        // Which shortcut is being captured ("" = none)
        static bool capturing_is_alt = false;    // Capturing primary or alt binding
        static std::string conflict_warning;     // Conflict warning text
        static std::string conflict_for_id;      // Which binding the conflict is about
        // Snapshot of registry for cancel support
        static std::vector<ShortcutEntry> snapshot;
        static bool has_snapshot = false;

        bool open = true;
        ImGuiWindowFlags modal_flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
        if (ImGui::BeginPopupModal("Keyboard Shortcuts", &open, modal_flags)) {

            // Take snapshot on first frame for cancel support
            if (!has_snapshot) {
                snapshot = GetShortcutRegistry();
                has_snapshot = true;
            }

            // Get accent color
            ImVec4 accent_color = Bright(GetWindowsAccentColor());
            ImVec4 dim_color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
            ImVec4 warn_color = ImVec4(1.0f, 0.85f, 0.2f, 1.0f);

            // Handle capture input (process before UI so we catch the key this frame)
            if (!capturing_id.empty()) {
                // Escape cancels capture
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                    capturing_id.clear();
                    conflict_warning.clear();
                } else {
                    KeyCombo captured = CaptureKeyCombo();
                    if (captured.IsSet()) {
                        // Apply the captured combo
                        auto& registry = GetShortcutRegistry();
                        for (auto& entry : registry) {
                            if (entry.id == capturing_id) {
                                if (capturing_is_alt) {
                                    entry.user_alt = captured;
                                    entry.has_user_alt = true;
                                } else {
                                    entry.user_primary = captured;
                                    entry.has_user_primary = true;
                                }
                                break;
                            }
                        }

                        // Check for conflicts
                        conflict_warning.clear();
                        conflict_for_id = capturing_id;
                        for (auto& other : registry) {
                            if (other.id == capturing_id) continue;
                            KeyCombo op = other.GetActivePrimary();
                            KeyCombo oa = other.GetActiveAlt();
                            if ((op.IsSet() && op == captured) || (oa.IsSet() && oa == captured)) {
                                conflict_warning = "Also used by: " + other.label;
                                break;
                            }
                        }

                        capturing_id.clear();
                    }
                }
            }

            ImGui::BeginChild("ShortcutsContent", ImVec2(0, S(-60)), false);

            // Helper lambda for rendering one editable shortcut row (call between BeginTable/EndTable)
            auto RenderEditableRow = [&](ShortcutEntry& entry) {
                ImGui::TableNextRow();

                // Column 0: Action label
                ImGui::TableSetColumnIndex(0);
                ImGui::TextWrapped("%s", entry.label.c_str());

                // Column 1: Default binding (dimmed)
                ImGui::TableSetColumnIndex(1);
                {
                    std::string def_text = entry.default_primary.ToString();
                    if (entry.default_alt.IsSet()) {
                        def_text += " / " + entry.default_alt.ToString();
                    }
                    if (entry.is_hold) def_text += " (hold)";
                    ImGui::TextColored(dim_color, "%s", def_text.c_str());
                }

                // Column 2: Editable binding button
                ImGui::TableSetColumnIndex(2);
                {
                    KeyCombo active_primary = entry.GetActivePrimary();
                    std::string btn_text;
                    bool is_capturing = (capturing_id == entry.id && !capturing_is_alt);

                    if (is_capturing) {
                        btn_text = "Press a key...";
                    } else {
                        btn_text = active_primary.IsSet() ? active_primary.ToString() : "(none)";
                        KeyCombo active_alt = entry.GetActiveAlt();
                        if (active_alt.IsSet()) {
                            btn_text += " / " + active_alt.ToString();
                        }
                    }

                    // Highlight modified bindings with accent color
                    bool modified = entry.has_user_primary || entry.has_user_alt;
                    if (modified) {
                        ImGui::PushStyleColor(ImGuiCol_Text, accent_color);
                    }

                    ImGui::PushID(entry.id.c_str());
                    float avail = ImGui::GetContentRegionAvail().x;
                    if (is_capturing) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.1f, 1.0f));
                        ImGui::Button(btn_text.c_str(), ImVec2(avail, 0));
                        ImGui::PopStyleColor();
                    } else {
                        PushOutlineButtonStyle();
                        if (ImGui::Button(btn_text.c_str(), ImVec2(avail, 0))) {
                            capturing_id = entry.id;
                            capturing_is_alt = false;
                            conflict_warning.clear();
                        }
                        PopOutlineButtonStyle();
                    }

                    if (conflict_for_id == entry.id && !conflict_warning.empty()) {
                        ImGui::TextColored(warn_color, "%s", conflict_warning.c_str());
                    }

                    ImGui::PopID();

                    if (modified) {
                        ImGui::PopStyleColor();
                    }
                }

                // Column 3: Reset button (only if user has override)
                ImGui::TableSetColumnIndex(3);
                if (entry.has_user_primary || entry.has_user_alt) {
                    ImGui::PushID((entry.id + "_reset").c_str());
                    if (ImGui::SmallButton("x")) {
                        ResetShortcut(entry.id);
                        if (conflict_for_id == entry.id) conflict_warning.clear();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Reset to default");
                    }
                    ImGui::PopID();
                }
            };

            // Helper lambda for rendering one category tab with editable bindings
            auto RenderCategoryTab = [&](const char* category_name) {
                auto& registry = GetShortcutRegistry();

                if (ImGui::BeginTabItem(category_name)) {
                    ImGui::Spacing();

                    ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH;
                    if (ImGui::BeginTable(category_name, 4, table_flags)) {
                        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                        ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                        ImGui::TableSetupColumn("##reset", ImGuiTableColumnFlags_WidthFixed, 30.0f);
                        ImGui::TableHeadersRow();

                        for (auto& entry : registry) {
                            if (entry.category != category_name) continue;
                            RenderEditableRow(entry);
                        }

                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
            };

            // Helper to find a shortcut entry by ID
            auto FindEntry = [&](const std::string& id) -> ShortcutEntry* {
                auto& registry = GetShortcutRegistry();
                for (auto& entry : registry) {
                    if (entry.id == id) return &entry;
                }
                return nullptr;
            };

            // Tabbed sections
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.25f, 0.25f, 0.50f));
            if (ImGui::BeginTabBar("ShortcutTabs")) {

                // Editable tabs from registry
                RenderCategoryTab("Playback");
                RenderCategoryTab("File / Project");
                RenderCategoryTab("View / Panels");
                RenderCategoryTab("Annotations");

                // Editable Timeline tab
                RenderCategoryTab("Timeline");

                // Color Panels tab: editable keyboard shortcuts + display-only mouse controls
                if (ImGui::BeginTabItem("Color Panels")) {
                    ImGui::Spacing();

                    // Editable keyboard shortcuts (these are cross-references to entries in other categories)
                    ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH;
                    if (ImGui::BeginTable("ColorShortcuts", 4, table_flags)) {
                        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                        ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                        ImGui::TableSetupColumn("##reset", ImGuiTableColumnFlags_WidthFixed, 30.0f);
                        ImGui::TableHeadersRow();

                        const char* color_panel_ids[] = { "delete_item", "delete_node", "tl_undo", "tl_redo" };
                        for (const char* id : color_panel_ids) {
                            ShortcutEntry* entry = FindEntry(id);
                            if (entry) RenderEditableRow(*entry);
                        }

                        ImGui::EndTable();
                    }

                    ImGui::Spacing();
                    ImGui::TextDisabled("Node Editor Mouse Controls:");
                    if (ImGui::BeginTable("NodeMouseControls", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);

                        auto MouseRow = [](const char* action, const char* description) {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", action);
                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextWrapped("%s", description);
                        };

                        MouseRow("Click + Drag", "Move selected nodes");
                        MouseRow("Click Pin + Drag", "Create connection");
                        MouseRow("Middle Mouse Drag", "Pan node editor");
                        MouseRow("Scroll", "Zoom node editor");
                        MouseRow("Box Select", "Select multiple nodes");

                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
            ImGui::PopStyleColor();  // Border

            ImGui::EndChild();  // End ShortcutsContent scrollable area

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Bottom buttons
            float btnPadding = 8.0f * 2;

            // Reset All (left-aligned)
            PushOutlineButtonStyle();
            if (ImGui::Button("Reset All Shortcuts")) {
                ResetAllShortcuts();
                conflict_warning.clear();
            }
            PopOutlineButtonStyle();

            // Save and Cancel (right-aligned)
            ImGui::SameLine();
            float saveW = ImGui::CalcTextSize("Save").x + btnPadding;
            float cancelW = ImGui::CalcTextSize("Cancel").x + btnPadding;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - saveW - cancelW - spacing);

            PushOutlineButtonStyle();
            if (ImGui::Button("Cancel")) {
                // Restore snapshot
                if (has_snapshot) {
                    GetShortcutRegistry() = snapshot;
                }
                capturing_id.clear();
                conflict_warning.clear();
                has_snapshot = false;
                ImGui::CloseCurrentPopup();
            }
            PopOutlineButtonStyle();

            ImGui::SameLine();

            PushOutlineButtonStyle();
            if (ImGui::Button("Save")) {
                SaveShortcuts();
                capturing_id.clear();
                conflict_warning.clear();
                has_snapshot = false;
                ImGui::CloseCurrentPopup();
            }
            PopOutlineButtonStyle();

            if (!open) {
                // X button clicked — treat as cancel
                if (has_snapshot) {
                    GetShortcutRegistry() = snapshot;
                }
                capturing_id.clear();
                conflict_warning.clear();
                has_snapshot = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    // ============================================================================
    // LUT EXPORT POPUP
    // ============================================================================
    void Application::CreateLutExportPopup() {
        // Open modal popup when flag is set
        if (show_lut_export_popup && !ImGui::IsPopupOpen("Exporting LUT")) {
            ImGui::OpenPopup("Exporting LUT");
        }

        // Center popup on screen with auto-height
        float scale = ImGui::GetIO().FontGlobalScale;
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(S(400), 0), ImGuiCond_Always);

        bool open = true;
        if (ImGui::BeginPopupModal("Exporting LUT", &open, ImGuiWindowFlags_AlwaysAutoResize)) {

            // Check for completion or error
            if (lut_export_complete.load()) {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "LUT exported successfully!");
                ImGui::Spacing();
                ImGui::Text("Saved to:");
                ImGui::TextWrapped("%s", lut_export_path.c_str());

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                float btnPadding = 8.0f * 2;
                float closeW = ImGui::CalcTextSize("Close").x + btnPadding;
                ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - closeW);
                PushOutlineButtonStyle();
                if (ImGui::Button("Close")) {
                    show_lut_export_popup = false;
                    ImGui::CloseCurrentPopup();
                }
                PopOutlineButtonStyle();
            }
            else if (lut_export_error.load()) {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Export failed!");
                ImGui::Text("%s", lut_export_error_message.c_str());

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                float btnPadding = 8.0f * 2;
                float closeW = ImGui::CalcTextSize("Close").x + btnPadding;
                ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - closeW);
                PushOutlineButtonStyle();
                if (ImGui::Button("Close")) {
                    show_lut_export_popup = false;
                    ImGui::CloseCurrentPopup();
                }
                PopOutlineButtonStyle();
            }
            else {
                // In progress
                ImGui::Text("Generating 65x65x65 3D LUT...");
                ImGui::Spacing();

                float progress = lut_export_progress.load();
                ImGui::ProgressBar(progress, ImVec2(-1, 0));
                ImGui::Text("%.0f%%", progress * 100.0f);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                float btnPadding = 8.0f * 2;
                float cancelW = ImGui::CalcTextSize("Cancel").x + btnPadding;
                ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - cancelW);
                PushOutlineButtonStyle();
                if (ImGui::Button("Cancel")) {
                    lut_export_cancel = true;
                    show_lut_export_popup = false;
                    ImGui::CloseCurrentPopup();
                }
                PopOutlineButtonStyle();
            }

            // Handle X button click
            if (!open) {
                lut_export_cancel = true;
                show_lut_export_popup = false;
            }

            ImGui::EndPopup();
        }
    }

