// ============================================================================
// Timeline panel (OTIO timeline, snap helpers, transport content)
// ============================================================================

#include "app/application.h"
#include "app/app_icons.h"
#include "app/app_ui_macros.h"
#include "app/ui_scale.h"
#include "app/app_config.h"
#include "app/shortcut_manager.h"
#include "app/timecode.h"
#include "project/project_manager.h"
#include "timeline/timeline_view.h"
#include "timeline/timeline_playback_controller.h"
#include "player/video_player.h"
#include "player/pipeline_mode.h"
#include "ui/timeline_manager.h"
#include "ui/annotation_panel.h"
#include "annotations/annotation_manager.h"
#include "annotations/annotation_serializer.h"
#include "nodes/node_manager.h"
#include "audio/audio_mixer.h"
#include "audio/audio_player.h"
#include "timeline/timeline_cache.h"
#include "timeline/timeline_thumbnail_cache.h"
#include "timeline/timeline_commands.h"
#include "timeline/media_linker.h"
#include "utils/frame_indexing.h"
#include <nfd.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <GLFW/glfw3.h>
#include <set>
#include <algorithm>
#include <chrono>

#ifdef QCVIEW_USE_VULKAN
#include "gpu/vulkan_texture_pool.h"
static inline ImTextureID PoolIDToImTexture(GLuint id) {
    if (id == 0) return ImTextureID{};
    return qcview::VulkanTexturePool::Instance().GetImTextureID(static_cast<uint64_t>(id));
}
static inline ImTextureID ThumbnailPoolIDToImTexture(GLuint id) {
    if (id == 0) return ImTextureID{};
    return qcview::VulkanTexturePool::ThumbnailInstance().GetImTextureID(static_cast<uint64_t>(id));
}
#elif defined(QCVIEW_USE_METAL)
#include "gpu/metal_texture_pool.h"
static inline ImTextureID PoolIDToImTexture(GLuint id) {
    if (id == 0) return ImTextureID{};
    return qcview::MetalTexturePool::Instance().GetImTextureID(static_cast<uint64_t>(id));
}
static inline ImTextureID ThumbnailPoolIDToImTexture(GLuint id) {
    if (id == 0) return ImTextureID{};
    return qcview::MetalTexturePool::ThumbnailInstance().GetImTextureID(static_cast<uint64_t>(id));
}
#else
static inline ImTextureID PoolIDToImTexture(GLuint id) {
    return (ImTextureID)(intptr_t)id;
}
static inline ImTextureID ThumbnailPoolIDToImTexture(GLuint id) {
    return (ImTextureID)(intptr_t)id;
}
#endif

// Globals defined in main.cpp
extern ImFont* font_regular;
extern ImFont* font_bold;
extern ImFont* font_icons;
extern ImFont* font_mono;
extern std::unique_ptr<qcview::TimelineView> timeline_view;
extern std::unique_ptr<qcview::TimelinePlaybackController> scratch_timeline_controller;
extern std::unique_ptr<qcview::TimelineThumbnailCache> timeline_thumbnail_cache;
extern bool otio_timeline_mode;
extern bool otio_dual_view_mode;
extern bool otio_dual_view_split_mode;
extern float otio_dual_view_split_pos;
extern CacheSettings cache_settings;
extern float g_font_scale;

// Timeline interaction state (defined in main.cpp)
extern qcview::TimelineClipDragState timeline_clip_drag;
extern qcview::TimelineTrimState timeline_trim_state;
extern qcview::TimelineMediaDropState timeline_media_drop;

// Timeline editing state
extern std::unique_ptr<qcview::TimelineCommandManager> timeline_command_manager;
extern bool timeline_thumbnail_cache_clear_deferred;
extern qcview::TimelineSlipState timeline_slip_state;
extern qcview::TimelineReorderState timeline_reorder_state;

// Helper to ensure command manager exists
inline qcview::TimelineCommandManager* GetCommandManager(qcview::TimelineView* view) {
    if (!timeline_command_manager) {
        timeline_command_manager = std::make_unique<qcview::TimelineCommandManager>();
    }
    timeline_command_manager->SetTimelineView(view);
    return timeline_command_manager.get();
}

// Playlist pending drag state
extern bool playlist_pending_drag_active;
extern std::string playlist_pending_drag_clip_id;
extern int playlist_pending_drag_clip_idx;
extern float playlist_pending_drag_start_x;
extern bool playlist_pending_drag_shift;
extern double playlist_pending_drag_source_in;
extern double playlist_pending_drag_source_out;
extern double playlist_pending_drag_source_duration;
extern double playlist_pending_drag_clip_duration;

// Media linker state
extern std::unique_ptr<qcview::MediaLinker> media_linker;
extern bool show_link_media_popup;
extern std::string link_media_search_path;
extern qcview::LinkSummary last_link_summary;
extern bool show_link_results_popup;
extern bool show_auto_relink_results;
extern std::string auto_relink_timeline_id;
extern std::string current_timeline_path;
static const float kPlaylistDragThreshold = S(15.0f);

// Free functions defined in main.cpp
void SaveLinksToCache(const std::string& timeline_id);

// ScreenshotFlashState (defined in main.cpp)
struct ScreenshotFlashState {
    std::chrono::steady_clock::time_point clipboard_flash_start;
    std::chrono::steady_clock::time_point desktop_flash_start;
    bool clipboard_flashing = false;
    bool desktop_flashing = false;
    static constexpr float FLASH_DURATION_MS = 400.0f;
};
extern ScreenshotFlashState g_screenshot_flash;

// OTIO Timeline UI Constants
namespace OTIOTimeline {
    const float TRACK_HEADER_WIDTH = S(140.0f);
    const float TRACK_LANE_HEIGHT = S(32.0f);
    const float TIMELINE_RULER_HEIGHT = S(48.0f);
    const float TRACK_SEPARATOR_HEIGHT = S(4.0f);
    const float OVERVIEW_TRACK_HEIGHT = S(28.0f);
    const float MIN_PIXELS_PER_SECOND = S(2.5f);
    const float MAX_PIXELS_PER_SECOND = S(1400.0f);
    const float DEFAULT_PIXELS_PER_SECOND = S(50.0f);
}

// Dual view timeline state (moved from main.cpp — only used by timeline panel)
static float g_dv_pixels_per_second = 80.0f;
static float g_dv_scroll_offset_x = 0.0f;
static bool g_dv_rw_active = false;
static bool g_dv_ff_active = false;
static float g_dv_zoom_min = 10.0f;
static float g_dv_zoom_max = 500.0f;

struct DualViewTrimState {
    bool active = false;
    bool is_left_track = true;
    bool is_left_edge = true;
    double original_in = 0.0;
    double original_out = 0.0;
    double original_offset = 0.0;
};
static DualViewTrimState g_dv_trim_state;

struct DualViewTrimMode {
    bool active = false;
    bool is_left_track = true;
    double pending_in = 0.0;
    double pending_out = 0.0;
    double original_in = 0.0;
    double original_out = 0.0;
    bool in_set = false;
    bool out_set = false;
};
static DualViewTrimMode g_dv_trim_mode;

struct DualViewDragState {
    bool active = false;
    bool is_left_track = true;
    double original_offset = 0.0;
    double drag_start_time = 0.0;
};
static DualViewDragState g_dv_drag_state;
static bool g_dv_scrubbing = false;
static bool g_dv_follow_playhead = true;

// Snapping state for dual view mode
static bool g_dv_snap_active = false;
static double g_dv_snap_time = 0.0;
static const double DV_SNAP_THRESHOLD_PX = 10.0;

// Snapping state for clip dragging
static bool snap_active = false;
static double snap_time = 0.0;
static const double SNAP_THRESHOLD_PX = 10.0;

// Right-click context menu state
static std::string right_clicked_clip_id;
static int right_clicked_track_index = -1;
static bool show_clip_context_menu = false;
static int right_clicked_track_header_index = -1;
static bool show_track_context_menu = false;
static bool show_empty_area_context_menu = false;

    // =========================================================================
    // Timeline Clip Snapping Helper Functions
    // =========================================================================

    // Collect all snap points from timeline (clip edges, playhead, boundaries)
    // exclude_clip_ids: clips being dragged (don't snap to self)
    std::vector<double> Application::CollectSnapPoints(
        const std::vector<qcview::OTIOTrack>& tracks,
        const std::set<std::string>& exclude_clip_ids,
        double playhead_time,
        double timeline_duration
    ) {
        std::vector<double> points;
        points.push_back(0.0);  // Timeline start
        points.push_back(timeline_duration);  // Timeline end
        points.push_back(playhead_time);  // Playhead

        for (const auto& track : tracks) {
            for (const auto& clip : track.clips) {
                if (exclude_clip_ids.count(clip.id)) continue;
                points.push_back(clip.start_time);  // Clip start
                points.push_back(clip.start_time + clip.duration);  // Clip end
            }
        }
        return points;
    }

    // Find nearest snap point and return snapped position
    // Returns the adjusted start_time for the clip
    double Application::CalculateSnappedPosition(
        double proposed_start,
        double clip_duration,
        const std::vector<double>& snap_points,
        double pixels_per_second,
        bool& snapped,          // OUT: whether snap occurred
        double& snap_line_time  // OUT: where to draw snap line
    ) {
        double threshold_time = SNAP_THRESHOLD_PX / pixels_per_second;
        double clip_end = proposed_start + clip_duration;
        snapped = false;

        double best_delta = threshold_time;
        double best_start = proposed_start;

        for (double point : snap_points) {
            // Try snapping clip START to this point
            double delta = std::abs(proposed_start - point);
            if (delta < best_delta) {
                best_delta = delta;
                best_start = point;
                snap_line_time = point;
                snapped = true;
            }

            // Try snapping clip END to this point
            delta = std::abs(clip_end - point);
            if (delta < best_delta) {
                best_delta = delta;
                best_start = point - clip_duration;  // Adjust start so end aligns
                snap_line_time = point;
                snapped = true;
            }
        }

        return std::max(0.0, best_start);  // Clamp to timeline start
    }

    // =========================================================================
    // Dual View Timeline Snapping Helper Functions
    // =========================================================================

    // Collect snap points for dual view mode (right track snaps to left track edges + playhead)
    std::vector<double> Application::CollectDualViewSnapPoints(
        const qcview::DualViewClip& left_clip,
        double playhead_time
    ) {
        std::vector<double> points;

        // Left clip defines the timeline (it's locked)
        if (left_clip.IsLoaded()) {
            points.push_back(0.0);  // Left clip start (always at 0, locked)
            points.push_back(left_clip.source_duration);  // Left clip end
        }

        // Playhead position
        points.push_back(playhead_time);

        return points;
    }

    // Calculate snapped position for dual view right track
    // Returns the adjusted position_offset for the right clip
    double Application::CalculateDualViewSnappedPosition(
        double proposed_offset,
        double clip_duration,
        const std::vector<double>& snap_points,
        double pixels_per_second,
        bool& snapped,          // OUT: whether snap occurred
        double& snap_line_time  // OUT: where to draw snap line
    ) {
        double threshold_time = DV_SNAP_THRESHOLD_PX / pixels_per_second;
        double clip_start = proposed_offset;  // position_offset is where clip starts
        double clip_end = proposed_offset + clip_duration;
        snapped = false;

        double best_delta = threshold_time;
        double best_offset = proposed_offset;

        for (double point : snap_points) {
            // Try snapping clip START to this point
            double delta = std::abs(clip_start - point);
            if (delta < best_delta) {
                best_delta = delta;
                best_offset = point;  // position_offset = snap point
                snap_line_time = point;
                snapped = true;
            }

            // Try snapping clip END to this point
            delta = std::abs(clip_end - point);
            if (delta < best_delta) {
                best_delta = delta;
                best_offset = point - clip_duration;  // Adjust offset so end aligns
                snap_line_time = point;
                snapped = true;
            }
        }

        return std::max(0.0, best_offset);  // Clamp to timeline start (no negative offset)
    }

    // Check if proposed multi-clip move would cause overlap with non-moving clips
    bool Application::WouldCauseOverlap(
        const std::vector<qcview::OTIOTrack>& tracks,
        const std::vector<qcview::TimelineClipDragState::DraggedClipInfo>& moving_clips,
        double primary_new_start
    ) {
        // Build set of moving clip IDs
        std::set<std::string> moving_ids;
        for (const auto& mc : moving_clips) {
            moving_ids.insert(mc.clip_id);
        }

        // Check each moving clip against stationary clips on same track
        for (const auto& mc : moving_clips) {
            double new_start = primary_new_start + mc.offset_from_primary;
            double new_end = new_start + mc.duration;

            if (mc.track_index < 0 || mc.track_index >= static_cast<int>(tracks.size())) continue;

            for (const auto& clip : tracks[mc.track_index].clips) {
                if (moving_ids.count(clip.id)) continue;  // Skip other moving clips
                double clip_end = clip.start_time + clip.duration;
                // Check overlap (with small epsilon for floating point)
                if (new_start < clip_end - 0.001 && new_end > clip.start_time + 0.001) {
                    return true;  // Overlap detected
                }
            }
        }
        return false;
    }

    void Application::RenderTimelineContent() {
#ifdef __APPLE__
        // Reduce control rounding inside timeline — dense UI looks better less round
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 4.0f);
#endif
        // OTIO timeline is now the only timeline mode
        RenderOTIOTimelinePanel();
#ifdef __APPLE__
        ImGui::PopStyleVar(3);
#endif
    }

    // =========================================================================
    // OTIO TIMELINE PANEL - Multi-track timeline editor view
    // =========================================================================
    void Application::RenderOTIOTimelinePanel() {
        // Get timeline info - prefer TimelineView's cached duration (survives mode switches)
        double duration = 0.0;
        if (timeline_view && timeline_view->GetDuration() > 0) {
            duration = timeline_view->GetDuration();  // Use timeline's actual duration
        } else if (timeline_manager) {
            duration = timeline_manager->GetUIDuration();  // Fallback
        }
        double position = timeline_manager ? timeline_manager->GetUIPosition() : 0.0;
        if (duration <= 0) duration = 30.0;  // Default duration for mock data

        // UI state for OTIO timeline - sync with TimelineView
        static float pixels_per_second = OTIOTimeline::DEFAULT_PIXELS_PER_SECOND;
        static float scroll_offset_x = 0.0f;
        static bool follow_playhead = true;  // Auto-scroll to keep playhead visible during playback

        // Calculate layout dimensions
        ImVec2 content_region = ImGui::GetContentRegionAvail();

        // Track which timeline we've fit-to-width for (to avoid re-fitting every frame)
        static std::string last_fit_timeline_id;

        // Fit zoom to actual panel width on first render of a new timeline
        if (timeline_view) {
            // Use timeline name as identifier (works for both EDL timelines and image sequences)
            std::string current_timeline_id = timeline_view->GetTimelineName();
            if (current_timeline_id.empty()) {
                current_timeline_id = timeline_view->GetSourceFilePath();  // Fallback for EDL timelines
            }
            float visible_width = content_region.x - OTIOTimeline::TRACK_HEADER_WIDTH;

            // Fit to width if:
            // 1. This is a new timeline (ID changed), OR
            // 2. A fit zoom was explicitly requested (e.g., after loading clips)
            bool should_fit = false;
            if (!current_timeline_id.empty() && current_timeline_id != last_fit_timeline_id && visible_width > 100.0f) {
                should_fit = true;
                last_fit_timeline_id = current_timeline_id;
            } else if (timeline_view->HasPendingFitZoom() && visible_width > 100.0f) {
                should_fit = true;
                timeline_view->ClearPendingFitZoom();
            }

            if (should_fit) {
                timeline_view->FitZoomToWidth(visible_width);
                Debug::Log("OTIO Panel: Fit zoom to width " + std::to_string(visible_width) + "px for: " + current_timeline_id);
            }

            // Update visible width for zoom limit calculations (prevents zooming past full timeline)
            timeline_view->UpdateVisibleWidth(visible_width);

            pixels_per_second = timeline_view->GetZoomLevel();
            scroll_offset_x = timeline_view->GetScrollOffset();
        }

        // Follow playhead: auto-scroll to keep playhead visible during playback
        if (follow_playhead && video_player && video_player->IsPlaying()) {
            float visible_width = content_region.x - OTIOTimeline::TRACK_HEADER_WIDTH;

            // Clamp position to valid timeline range to prevent scroll jump at loop point
            double clamped_position = std::min(position, duration > 0 ? duration - 0.001 : 0.0);
            float playhead_screen_x = static_cast<float>(clamped_position) * pixels_per_second - scroll_offset_x;

            // Calculate max scroll (where timeline end aligns with right edge of visible area)
            float timeline_width_px = static_cast<float>(duration) * pixels_per_second;
            float max_scroll = std::max(0.0f, timeline_width_px - visible_width);

            // Define margins (keep playhead within 20px of edges)
            float margin_px = S(20.0f);
            float left_margin = margin_px;
            float right_margin = visible_width - margin_px;

            // If playhead goes past right margin, scroll to put it at left margin
            if (playhead_screen_x > right_margin) {
                scroll_offset_x = static_cast<float>(clamped_position) * pixels_per_second - left_margin;
                scroll_offset_x = std::max(0.0f, scroll_offset_x);
                // CRITICAL: Cap scroll to max - don't scroll past timeline end
                scroll_offset_x = std::min(scroll_offset_x, max_scroll);
                if (timeline_view) {
                    timeline_view->SetScrollOffset(scroll_offset_x);
                }
            }
            // If playhead goes before left margin (e.g., after loop), scroll to show it
            else if (playhead_screen_x < left_margin && scroll_offset_x > 0) {
                scroll_offset_x = static_cast<float>(clamped_position) * pixels_per_second - left_margin;
                scroll_offset_x = std::max(0.0f, scroll_offset_x);
                if (timeline_view) {
                    timeline_view->SetScrollOffset(scroll_offset_x);
                }
            }
        }
        // Calculate visible track count (hide audio tracks in audio-only mode)
        float track_count = 0.0f;
        if (timeline_view) {
            bool is_audio_only = timeline_view->GetSourceMode() == qcview::TimelineSourceMode::VIDEO_FILE &&
                                 timeline_view->GetVideoTrackCount() == 0;
            if (is_audio_only) {
                track_count = 0.0f;  // Hide all tracks in audio-only mode
            } else {
                track_count = (float)(timeline_view->GetVideoTrackCount() + timeline_view->GetAudioTrackCount());
            }
        } else {
            track_count = 5.0f;  // Default when no timeline
        }

        // Calculate tracks area height
        float tracks_height = OTIOTimeline::OVERVIEW_TRACK_HEIGHT +  // Overview minimap track
                             OTIOTimeline::TRACK_SEPARATOR_HEIGHT;   // Separator below overview
        if (track_count > 0) {
            tracks_height += track_count * OTIOTimeline::TRACK_LANE_HEIGHT +
                            OTIOTimeline::TRACK_SEPARATOR_HEIGHT;    // Separator between video/audio
        }

        // Add breadcrumb bar height when viewing nested timeline
        float breadcrumb_bar_height = 0.0f;
        if (timeline_view && timeline_view->IsViewingNestedTimeline()) {
            breadcrumb_bar_height = S(58.0f);  // Match breadcrumb_height in panel calculations
        }

        // =====================================================================
        // ROW 1: Transport Controls (matching standard timeline style)
        // =====================================================================
        // Add top padding to transport row
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(3.7f));

        // Transport button sizes - use GetFrameHeight() which already scales with font
        float button_size = ImGui::GetFrameHeight() * 1.5f;
        float small_button = button_size;
        float icon_scale = 1.6f;
        float utility_icon_scale = 1.3f;
        float item_spacing = ImGui::GetStyle().ItemSpacing.x;
        float spacer_width = S(10.0f);
        ImVec4 disabled_color = ImVec4(0.29f, 0.29f, 0.29f, 1.0f);

        // Calculate total width for centering
        // Playback box: 7 buttons + padding (+ 2 for prev/next clip in PLAYLIST mode)
        bool is_playlist_mode = timeline_view &&
            timeline_view->GetSourceMode() == qcview::TimelineSourceMode::PLAYLIST;
        int playback_button_count = is_playlist_mode ? 9 : 7;
        float playback_box_padding = S(5.0f);
        float playback_box_width = (playback_button_count * small_button) + ((playback_button_count - 1) * item_spacing) + (playback_box_padding * 2 + S(12));

        // Utility buttons after playback: 3 + 2 + 1 + 2 = 8 buttons
        int utility_button_count = 8;
        // Separators: 5 separators with spacers on each side (2 spacers each)
        int separator_count = 5;
        float separator_spacing = spacer_width * 2 + S(2.0f);  // Two spacers + separator width

        // Resolution button at far left: 1 button + 1 separator + extra padding
        int resolution_button_count = 1;
        int resolution_separator_count = 1;

        float total_width = (resolution_button_count * small_button) +
                           (resolution_separator_count * separator_spacing) +
                           (resolution_button_count * item_spacing) +
                           button_size + spacer_width +  // Extra button + spacer for alignment
                           playback_box_width +
                           (utility_button_count * small_button) +
                           (separator_count * separator_spacing) +
                           ((utility_button_count + separator_count) * item_spacing);

        float available_width = content_region.x;
        float center_offset = (available_width - total_width) * 0.5f;

        // Apply centering
        if (center_offset > 0.0f) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + center_offset);
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));  // No padding - icon fills button
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));  // Center icon
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));

        // Track hover states for tooltips
        bool hover_start = false, hover_rw = false, hover_prev = false, hover_play = false;
        bool hover_next = false, hover_ff = false, hover_end = false;
        bool hover_link = false;
        bool hover_prev_clip = false, hover_next_clip = false;

        bool is_playing = false;
        if (timeline_view && timeline_view->HasPlaybackController()) {
            auto* controller = timeline_view->GetEffectivePlaybackController();
            is_playing = controller && controller->IsActuallyPlaying();
        } else if (video_player) {
            is_playing = video_player->IsActuallyPlaying();
        }

        // Static states for rewind/fast-forward
        static bool otio_rw_active = false;
        static bool otio_ff_active = false;

        float box_y_offset = S(1.1f);  // Vertical offset for playback box and buttons

        // Store draw list for background box
        ImDrawList* transport_draw_list = ImGui::GetWindowDrawList();

        float playback_box_height = button_size;

        // Get start position for playback box
        ImVec2 playback_box_start = ImGui::GetCursorScreenPos();
        playback_box_start.y += box_y_offset;  // Offset the box down

        // Draw background box with rounded corners for playback controls
        float transport_box_rounding = S(10.0f);
        transport_draw_list->AddRectFilled(
            playback_box_start,
            ImVec2(playback_box_start.x + playback_box_width, playback_box_start.y + playback_box_height),
            IM_COL32(16, 16, 16, 60), transport_box_rounding);
        transport_draw_list->AddRect(
            playback_box_start,
            ImVec2(playback_box_start.x + playback_box_width, playback_box_start.y + playback_box_height),
            UI_WHITE_A(15), transport_box_rounding, 0, S(1.0f));

        // Offset buttons to match the box
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + box_y_offset);

        // Padding before playback buttons
        ImGui::Dummy(ImVec2(playback_box_padding, 0));
        ImGui::SameLine();

        if (font_icons) {
            ImGui::PushFont(font_icons);
            ImGui::SetWindowFontScale(icon_scale);

            // === PLAYBACK CONTROLS ===
            // Previous clip (PLAYLIST mode only)
            if (is_playlist_mode) {
                if (ImGui::Button(ICON_FIRST_PAGE "##prev_clip", ImVec2(small_button, button_size))) {
                    AutoSaveAnnotationOnSeek();
                    // Seek to previous clip start
                    if (timeline_view) {
                        auto& tracks = timeline_view->GetTracks();
                        if (!tracks.empty() && !tracks[0].clips.empty()) {
                            double pos = video_player ? video_player->GetPosition() : 0.0;
                            const auto& clips = tracks[0].clips;
                            // Find previous non-gap clip
                            for (int i = clips.size() - 1; i >= 0; --i) {
                                if (!clips[i].is_gap && clips[i].start_time < pos - 0.1) {
                                    video_player->Seek(clips[i].start_time);
                                    break;
                                }
                            }
                        }
                    }
                }
                hover_prev_clip = ImGui::IsItemHovered();
                ImGui::SameLine();
            }

            // Skip to start
            if (ImGui::Button(ICON_SKIP_PREVIOUS "##otio_start", ImVec2(small_button, button_size))) {
                AutoSaveAnnotationOnSeek();
                if (video_player) {
                    video_player->GoToStart();
                    // Scroll timeline view to show playhead at start
                    if (timeline_view) {
                        timeline_view->SetScrollOffset(0.0f);
                    }
                }
            }
            hover_start = ImGui::IsItemHovered();
            ImGui::SameLine();

            // Rewind (press and hold)
            if (otio_rw_active) {
                ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
            }
            ImGui::Button(ICON_FAST_REWIND "##otio_rw", ImVec2(small_button, button_size));
            bool otio_rw_held = ImGui::IsItemActive();  // IsItemActive stays true while button is held
            if (otio_rw_active) ImGui::PopStyleColor();
            hover_rw = ImGui::IsItemHovered();

            if (otio_rw_held && !otio_rw_active) {
                // Just pressed
                otio_rw_active = true;
                otio_ff_active = false;
                if (video_player) video_player->StartRewind();
            } else if (!otio_rw_held && otio_rw_active) {
                // Just released
                otio_rw_active = false;
                if (video_player) video_player->StopFastSeek();
            }
            ImGui::SameLine();

            // Step back
            if (ImGui::Button(ICON_ARROW_LEFT "##otio_prev", ImVec2(small_button, button_size))) {
                if (video_player) video_player->StepFrame(-1);
            }
            hover_prev = ImGui::IsItemHovered();
            ImGui::SameLine();

            // Play/Pause - use stable ID to prevent click issues when state changes
            bool otio_style_pushed = false;
            if (is_playing && !otio_rw_active && !otio_ff_active) {
                ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                otio_style_pushed = true;
            }
            static int otio_playpause_click_count = 0;
            static int otio_playpause_frame_count = 0;
            int current_frame = ImGui::GetFrameCount();
            if (current_frame != otio_playpause_frame_count) {
                if (otio_playpause_click_count > 0) {
                    Debug::Log("OTIO Play/Pause: Frame " + std::to_string(otio_playpause_frame_count) +
                               " had " + std::to_string(otio_playpause_click_count) + " clicks!");
                }
                otio_playpause_click_count = 0;
                otio_playpause_frame_count = current_frame;
            }
            if (ImGui::Button(is_playing ? ICON_PAUSE "##otio_playpause" : ICON_PLAY_ARROW "##otio_playpause",
                             ImVec2(small_button, button_size))) {
                otio_playpause_click_count++;
                if (video_player) {
                    Debug::Log("OTIO Play/Pause button clicked #" + std::to_string(otio_playpause_click_count) +
                               " in frame " + std::to_string(current_frame) + ", is_playing=" + std::to_string(is_playing));
                    otio_rw_active = false;
                    otio_ff_active = false;
                    if (is_playing) {
                        video_player->Pause();
                    } else {
                        AutoSaveAnnotationOnSeek();
                        video_player->Play();
                    }
                }
            }
            if (otio_style_pushed) ImGui::PopStyleColor();
            hover_play = ImGui::IsItemHovered();
            ImGui::SameLine();

            // Step forward
            if (ImGui::Button(ICON_ARROW_RIGHT "##otio_next", ImVec2(small_button, button_size))) {
                if (video_player) video_player->StepFrame(1);
            }
            hover_next = ImGui::IsItemHovered();
            ImGui::SameLine();

            // Fast forward (press and hold)
            if (otio_ff_active) {
                ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
            }
            ImGui::Button(ICON_FAST_FORWARD "##otio_ff", ImVec2(small_button, button_size));
            bool otio_ff_held = ImGui::IsItemActive();  // IsItemActive stays true while button is held
            if (otio_ff_active) ImGui::PopStyleColor();
            hover_ff = ImGui::IsItemHovered();

            if (otio_ff_held && !otio_ff_active) {
                // Just pressed
                otio_ff_active = true;
                otio_rw_active = false;
                if (video_player) video_player->StartFastForward();
            } else if (!otio_ff_held && otio_ff_active) {
                // Just released
                otio_ff_active = false;
                if (video_player) video_player->StopFastSeek();
            }
            ImGui::SameLine();

            // Skip to end
            if (ImGui::Button(ICON_SKIP_NEXT "##otio_end", ImVec2(small_button, button_size))) {
                AutoSaveAnnotationOnSeek();
                if (video_player) {
                    video_player->GoToEnd();
                    // Scroll timeline view to show playhead at end
                    if (timeline_view) {
                        double duration = timeline_view->GetDuration();
                        float end_offset = static_cast<float>(duration) * pixels_per_second;
                        float visible_width = content_region.x - OTIOTimeline::TRACK_HEADER_WIDTH;
                        // Position end at right edge of view (no margin - prevents visual extension)
                        float target_offset = end_offset - visible_width;
                        timeline_view->SetScrollOffset(std::max(0.0f, target_offset));
                    }
                }
            }
            hover_end = ImGui::IsItemHovered();

            // Next clip (PLAYLIST mode only)
            if (is_playlist_mode) {
                ImGui::SameLine();
                if (ImGui::Button(ICON_LAST_PAGE "##next_clip", ImVec2(small_button, button_size))) {
                    AutoSaveAnnotationOnSeek();
                    // Seek to next clip start
                    if (timeline_view) {
                        auto& tracks = timeline_view->GetTracks();
                        if (!tracks.empty() && !tracks[0].clips.empty()) {
                            double pos = video_player ? video_player->GetPosition() : 0.0;
                            const auto& clips = tracks[0].clips;
                            // Find next non-gap clip
                            for (const auto& clip : clips) {
                                if (!clip.is_gap && clip.start_time > pos + 0.1) {
                                    video_player->Seek(clip.start_time);
                                    break;
                                }
                            }
                        }
                    }
                }
                hover_next_clip = ImGui::IsItemHovered();
            }

            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopFont();
        }

        // Padding after playback buttons
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(playback_box_padding, 0));
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(spacer_width, 0));
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(spacer_width, 0));
        ImGui::SameLine();

        // === VIEW TOGGLE BUTTONS (Colorspace to Minimal View) ===
        // Track hover states for tooltips (shown after font is popped)
        bool hover_colorspace = false, hover_safety = false, hover_background = false;
        bool hover_ss_clip = false, hover_ss_desk = false;
        bool hover_dual = false, hover_fullscreen = false;
        bool hover_all = false, hover_minimal = false;
        bool dual_view_enabled = false;

        if (font_icons) {
            ImGui::PushFont(font_icons);
            ImGui::SetWindowFontScale(utility_icon_scale);

            // Toggle Colorspace
            bool shader_applied = video_player && video_player->HasActiveColorTransform();
            if (shader_applied) {
                ImGui::PushStyleColor(ImGuiCol_Text, MutedLight(GetWindowsAccentColor()));
            }
            if (ImGui::Button(ICON_TONALITY "##otio_colorspace", ImVec2(small_button, button_size))) {
                if (shader_applied) {
                    // Remove shader and return to passthrough
                    video_player->ClearColorPipeline();
                    Debug::Log("Colorspace button: Removed color transform");
                } else {
                    // Show preset picker
                    show_colorspace_panel = !show_colorspace_panel;
                }
            }
            if (shader_applied) {
                ImGui::PopStyleColor();
            }
            hover_colorspace = ImGui::IsItemHovered();
            ImGui::SameLine();

            // Safety Guides
            bool overlays_enabled = video_player && video_player->IsSafetyOverlaysEnabled();
            if (overlays_enabled) {
                ImGui::PushStyleColor(ImGuiCol_Text, MutedLight(GetWindowsAccentColor()));
            }
            if (ImGui::Button(ICON_MASK "##otio_safety", ImVec2(small_button, button_size))) {
                if (overlays_enabled) {
                    // Disable overlays
                    video_player->EnableSafetyOverlays(false);
                    if (auto* svg_renderer = video_player->GetSVGRenderer()) {
                        svg_renderer->ClearSVG();
                    }
                    Debug::Log("Safety button: Disabled overlays");
                } else {
                    // Show panel to select overlay
                    show_safety_overlay_panel = !show_safety_overlay_panel;
                }
            }
            if (overlays_enabled) ImGui::PopStyleColor();
            hover_safety = ImGui::IsItemHovered();
            ImGui::SameLine();

            // Background
            if (ImGui::Button(ICON_BACKGROUND "##otio_bg", ImVec2(small_button, button_size))) {
                show_background_panel = !show_background_panel;
            }
            hover_background = ImGui::IsItemHovered();
            ImGui::SameLine();

            ImGui::Dummy(ImVec2(spacer_width, 0));
            ImGui::SameLine();
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(spacer_width, 0));
            ImGui::SameLine();

            // Screenshot to clipboard (with flash feedback)
            {
                bool is_flashing = false;
                if (g_screenshot_flash.clipboard_flashing) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - g_screenshot_flash.clipboard_flash_start).count();
                    if (elapsed < ScreenshotFlashState::FLASH_DURATION_MS) {
                        is_flashing = true;
                        ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                    } else {
                        g_screenshot_flash.clipboard_flashing = false;
                    }
                }
                if (ImGui::Button(ICON_SCREENSHOT_CLIPBOARD "##otio_ss_clip", ImVec2(small_button, button_size))) {
                    if (video_player && video_player->HasValidTexture()) {
                        if (video_player->CaptureScreenshotToClipboard()) {
                            g_screenshot_flash.clipboard_flashing = true;
                            g_screenshot_flash.clipboard_flash_start = std::chrono::steady_clock::now();
                        }
                    }
                }
                if (is_flashing) ImGui::PopStyleColor();
            }
            hover_ss_clip = ImGui::IsItemHovered();
            ImGui::SameLine();

            // Screenshot to desktop (with flash feedback)
            {
                bool is_flashing = false;
                if (g_screenshot_flash.desktop_flashing) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - g_screenshot_flash.desktop_flash_start).count();
                    if (elapsed < ScreenshotFlashState::FLASH_DURATION_MS) {
                        is_flashing = true;
                        ImGui::PushStyleColor(ImGuiCol_Button, GetWindowsAccentColor());
                    } else {
                        g_screenshot_flash.desktop_flashing = false;
                    }
                }
                if (ImGui::Button(ICON_SCREENSHOT_DESKTOP "##otio_ss_desk", ImVec2(small_button, button_size))) {
                    if (video_player && video_player->HasValidTexture()) {
                        if (video_player->CaptureScreenshotToDesktop()) {
                            g_screenshot_flash.desktop_flashing = true;
                            g_screenshot_flash.desktop_flash_start = std::chrono::steady_clock::now();
                        }
                    }
                }
                if (is_flashing) ImGui::PopStyleColor();
            }
            hover_ss_desk = ImGui::IsItemHovered();
            ImGui::SameLine();

            ImGui::Dummy(ImVec2(spacer_width, 0));
            ImGui::SameLine();
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(spacer_width, 0));
            ImGui::SameLine();

            // Fullscreen
            if (ImGui::Button(ICON_FULLSCREEN "##otio_fullscreen", ImVec2(small_button, button_size))) {
                ToggleFullscreen();
            }
            hover_fullscreen = ImGui::IsItemHovered();
            ImGui::SameLine();

            ImGui::Dummy(ImVec2(spacer_width, 0));
            ImGui::SameLine();
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine();
            ImGui::Dummy(ImVec2(spacer_width, 0));
            ImGui::SameLine();

            // === ALL PANELS BUTTON ===
            // Left-click: Show all panels
            bool all_panels_shown_local = show_project_panel && show_inspector_panel && show_color_panels && !minimal_view_mode;
            ImGui::PushStyleColor(ImGuiCol_Text, all_panels_shown_local ? UI_WHITE_VEC4 : UI_GRAY_VEC4);
            if (ImGui::Button(ICON_GRID_VIEW "##otio_all_panels", ImVec2(small_button, button_size))) {
                minimal_view_mode = false;
                ShowAllPanels();
            }
            ImGui::PopStyleColor();
            hover_all = ImGui::IsItemHovered();
            ImGui::SameLine();

            // Minimal view (keeps sidebar visible)
            ImGui::PushStyleColor(ImGuiCol_Text, minimal_view_mode ? UI_WHITE_VEC4 : UI_GRAY_VEC4);
            if (ImGui::Button(ICON_HOME_MAX "##otio_minimal", ImVec2(small_button, button_size))) {
                minimal_view_mode = !minimal_view_mode;
                if (minimal_view_mode) {
                    saved_show_project_panel = show_project_panel;
                    saved_show_inspector_panel = show_inspector_panel;
                    saved_show_color_panels = show_color_panels;
                    saved_show_annotation_panel = show_annotation_panel;
                    saved_show_annotation_toolbar = show_annotation_toolbar;
                    show_project_panel = false;
                    show_inspector_panel = false;
                    show_timeline_panel = true;
                    show_color_panels = false;
                    show_annotation_panel = false;
                    show_annotation_toolbar = false;
                    if (annotation_toolbar) annotation_toolbar->SetVisible(false);
                    first_time_setup = true;
                } else {
                    show_project_panel = saved_show_project_panel;
                    show_inspector_panel = saved_show_inspector_panel;
                    show_timeline_panel = true;
                    show_color_panels = saved_show_color_panels;
                    show_annotation_panel = saved_show_annotation_panel;
                    show_annotation_toolbar = saved_show_annotation_toolbar;
                    if (annotation_toolbar) annotation_toolbar->SetVisible(saved_show_annotation_toolbar);
                    first_time_setup = true;
                }
            }
            ImGui::PopStyleColor();
            hover_minimal = ImGui::IsItemHovered();

            // In dual view mode: show Dual/Split toggle
            // In single-file modes (Video/Audio/Image/EXR): show Thumbnail toggle
            // In PLAYLIST mode: nothing shown, so hide separator too
            bool is_otio_dual_view_active = otio_dual_view_mode && timeline_view && timeline_view->IsDualViewMode();
            bool show_last_button = is_otio_dual_view_active || !is_playlist_mode;

            if (show_last_button) {
                ImGui::SameLine();
                ImGui::Dummy(ImVec2(spacer_width, 0));
                ImGui::SameLine();
                ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                ImGui::SameLine();
                ImGui::Dummy(ImVec2(spacer_width, 0));
                ImGui::SameLine();
            }
            static bool hover_dual_split = false;
            static bool hover_thumbnails = false;

            if (is_otio_dual_view_active) {
                // Dual/Split toggle button
                // Split mode = vertical wipe, Dual mode = side-by-side
                const char* toggle_icon = otio_dual_view_split_mode ? ICON_SPLIT_SCENE : ICON_SPLIT_SCREEN_LEFT;
                ImGui::PushStyleColor(ImGuiCol_Text, UI_WHITE_VEC4);
                if (ImGui::Button(toggle_icon, ImVec2(small_button, button_size))) {
                    otio_dual_view_split_mode = !otio_dual_view_split_mode;
                }
                ImGui::PopStyleColor();
                hover_dual_split = ImGui::IsItemHovered();
                hover_link = false;
                hover_thumbnails = false;
            } else if (!is_playlist_mode) {
                // Thumbnail toggle for single-file modes (Video/Audio/Image/EXR)
                // Hidden in PLAYLIST mode (thumbnails not applicable)
                const char* thumb_icon = cache_settings.enable_thumbnails ? ICON_IMAGE : ICON_HIDE_IMAGE;
                ImVec4 thumb_color = cache_settings.enable_thumbnails ? MutedLight(GetWindowsAccentColor()) : UI_WHITE_VEC4;
                ImGui::PushStyleColor(ImGuiCol_Text, thumb_color);
                if (ImGui::Button(thumb_icon, ImVec2(small_button, button_size))) {
                    cache_settings.enable_thumbnails = !cache_settings.enable_thumbnails;
                    SaveSettings();
                    if (!cache_settings.enable_thumbnails && timeline_thumbnail_cache) {
                        timeline_thumbnail_cache->Clear();
                    }
                    Debug::Log(cache_settings.enable_thumbnails ? "Timeline thumbnails enabled" : "Timeline thumbnails disabled");
                }
                ImGui::PopStyleColor();
                hover_thumbnails = ImGui::IsItemHovered();
                hover_link = false;
                hover_dual_split = false;
            } else {
                // PLAYLIST mode - no thumbnail button
                hover_thumbnails = false;
                hover_link = false;
                hover_dual_split = false;
            }

            // HDR status indicator moved to sidebar panel

            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopFont();

            // Dual/Split toggle tooltip (outside icon font scope)
            if (hover_dual_split) {
                ImGui::SetTooltip(otio_dual_view_split_mode ? "Switch to Side-by-Side" : "Switch to Split View");
            }
            // Thumbnail toggle tooltip
            if (hover_thumbnails) {
                ImGui::SetTooltip(cache_settings.enable_thumbnails ? "Thumbnails: ON" : "Thumbnails: OFF");
            }
        }

        // Show tooltips with regular font (after icon font is popped)
        if (hover_start) ImGui::SetTooltip("Go to Start");
        if (hover_rw) ImGui::SetTooltip(otio_rw_active ? "Stop Rewind" : "Rewind (4x)");
        if (hover_prev) ImGui::SetTooltip("Previous Frame");
        if (hover_play) ImGui::SetTooltip(is_playing ? "Pause" : "Play");
        if (hover_next) ImGui::SetTooltip("Next Frame");
        if (hover_ff) ImGui::SetTooltip(otio_ff_active ? "Stop Fast Forward" : "Fast Forward (4x)");
        if (hover_end) ImGui::SetTooltip("Go to End");
        if (hover_prev_clip) ImGui::SetTooltip("Previous Clip");
        if (hover_next_clip) ImGui::SetTooltip("Next Clip");
        if (hover_colorspace) {
            bool has_shader = video_player && video_player->HasActiveColorTransform();
            ImGui::SetTooltip(has_shader ? "Remove colorspace (active)" : "Toggle colorspace presets");
        }
        if (hover_safety) ImGui::SetTooltip("Toggle safety overlay guides");
        if (hover_background) ImGui::SetTooltip("Change video panel background");
        if (hover_ss_clip) ImGui::SetTooltip("Screenshot to clipboard");
        if (hover_ss_desk) ImGui::SetTooltip("Screenshot to desktop");
        if (hover_dual) ImGui::SetTooltip(dual_view_enabled ? "Dual View ON" : "Dual View OFF");
        if (hover_fullscreen) ImGui::SetTooltip("Fullscreen");
        if (hover_all) ImGui::SetTooltip("Show All Panels (Ctrl+9)");
        if (hover_minimal) ImGui::SetTooltip(minimal_view_mode ? "Exit Minimal View (Ctrl+-)" : "Minimal View (Ctrl+-)");
        if (hover_link) ImGui::SetTooltip("Link Media to Clips");

        ImGui::PopStyleVar(2);    // Pop FramePadding, ButtonTextAlign
        ImGui::PopStyleColor(3);

        ImGui::Spacing();

        // =====================================================================
        // NESTED TIMELINE BREADCRUMB NAVIGATION
        // =====================================================================
        if (timeline_view && timeline_view->IsViewingNestedTimeline()) {
            // Draw breadcrumb bar for nested timeline navigation
            ImVec2 breadcrumb_pos = ImGui::GetCursorScreenPos();
            float breadcrumb_height = S(24.0f);  // Actual breadcrumb bar height (panel adds 52px total for this + spacing)
            ImDrawList* bc_draw = ImGui::GetWindowDrawList();

            // Breadcrumb background
            bc_draw->AddRectFilled(
                breadcrumb_pos,
                ImVec2(breadcrumb_pos.x + content_region.x, breadcrumb_pos.y + breadcrumb_height),
                IM_COL32(35, 35, 35, 255)
            );

            // Draw breadcrumb path: Root > Nested1 > Nested2 > [Current]
            auto path = timeline_view->GetBreadcrumbPath();
            float bc_x = breadcrumb_pos.x + S(8.0f);
            float bc_y = breadcrumb_pos.y + S(3.0f);

            if (font_regular) ImGui::PushFont(font_regular);

            // Back button to exit current nested timeline
            if (font_icons) {
                ImGui::SetCursorScreenPos(ImVec2(bc_x, bc_y));
                ImGui::PushFont(font_icons);
                if (ImGui::SmallButton(ICON_BACK "##exit_nest")) {
                    timeline_view->ExitNestedTimeline();
                }
                ImGui::PopFont();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Exit to parent timeline");
                }
                bc_x += S(28.0f);
            }

            // Draw path segments
            for (size_t pi = 0; pi < path.size(); pi++) {
                if (pi > 0) {
                    bc_draw->AddText(ImVec2(bc_x, bc_y), IM_COL32(100, 100, 100, 255), ">");
                    bc_x += S(12.0f);
                }

                bool is_current = (pi == path.size() - 1);
                ImU32 text_color = is_current ?
                    UI_WHITE : IM_COL32(160, 160, 255, 255);

                if (!is_current) {
                    // Clickable - navigate to this level
                    ImGui::SetCursorScreenPos(ImVec2(bc_x, bc_y));
                    std::string btn_id = "##bc_" + std::to_string(pi);
                    if (ImGui::SmallButton((path[pi] + btn_id).c_str())) {
                        // Navigate back to this level
                        int levels_to_exit = static_cast<int>(path.size() - 1 - pi);
                        for (int lv = 0; lv < levels_to_exit; lv++) {
                            timeline_view->ExitNestedTimeline();
                        }
                    }
                    bc_x += ImGui::GetItemRectSize().x + S(4.0f);
                } else {
                    // Current level - not clickable
                    bc_draw->AddText(ImVec2(bc_x, bc_y), text_color, path[pi].c_str());
                    bc_x += ImGui::CalcTextSize(path[pi].c_str()).x + S(4.0f);
                }
            }

            // Show depth indicator
            int depth = timeline_view->GetNestedDepth();
            if (depth > 0) {
                std::string depth_str = "(Depth: " + std::to_string(depth) + ")";
                float depth_x = breadcrumb_pos.x + content_region.x - ImGui::CalcTextSize(depth_str.c_str()).x - S(8.0f);
                bc_draw->AddText(ImVec2(depth_x, bc_y), IM_COL32(100, 100, 100, 255), depth_str.c_str());
            }

            if (font_regular) ImGui::PopFont();

            // Advance cursor past breadcrumb bar
            ImGui::Dummy(ImVec2(content_region.x, breadcrumb_height));
        }

        // =====================================================================
        // ROW 2: Track Headers + Track Lanes
        // =====================================================================
        ImVec2 tracks_start_pos = ImGui::GetCursorScreenPos();
        ImDrawList* draw_list = ImGui::GetWindowDrawList();

        // Background for tracks area
        draw_list->AddRectFilled(
            tracks_start_pos,
            ImVec2(tracks_start_pos.x + content_region.x, tracks_start_pos.y + tracks_height),
            IM_COL32(20, 20, 20, 255)
        );

        // Track header column background
        draw_list->AddRectFilled(
            tracks_start_pos,
            ImVec2(tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH,
                   tracks_start_pos.y + tracks_height),
            IM_COL32(30, 30, 30, 255)
        );

        // =====================================================================
        // TIMELINE OVERVIEW MINIMAP TRACK (shows full timeline with viewport indicator)
        // =====================================================================
        {
            float overview_y = tracks_start_pos.y;
            float overview_height = OTIOTimeline::OVERVIEW_TRACK_HEIGHT;
            float overview_lane_left = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH;
            float overview_lane_width = content_region.x - OTIOTimeline::TRACK_HEADER_WIDTH;

            // Overview track lane background
            draw_list->AddRectFilled(
                ImVec2(overview_lane_left, overview_y),
                ImVec2(overview_lane_left + overview_lane_width, overview_y + overview_height),
                IM_COL32(22, 22, 22, 255)
            );

            // Header icon and label (scaled like other tracks)
            const float hdr_ui_scale = ImGui::GetIO().FontGlobalScale;
            const float hdr_scale_factor = 1.0f + (hdr_ui_scale - 1.0f) * 0.5f;
            const float track_icon_size = S(16.0f) * hdr_scale_factor;
            const float icon_v_offset = (overview_height - track_icon_size) * 0.5f;

            // Draw filmstrip/movie icon (no label - icon speaks for itself)
            ImVec2 icon_pos(tracks_start_pos.x + S(6), overview_y + icon_v_offset);
            if (font_icons) {
                draw_list->AddText(font_icons, track_icon_size, icon_pos, IM_COL32(180, 180, 180, 255), ICON_MOVIE);
            }

            // Header separator line
            draw_list->AddLine(
                ImVec2(overview_lane_left, overview_y),
                ImVec2(overview_lane_left, overview_y + overview_height),
                IM_COL32(50, 50, 50, 255), S(1.0f)
            );

            // Timeline "clip" visual - represents full timeline duration
            if (duration > 0) {
                float clip_padding = S(2.0f);
                float clip_y_top = overview_y + clip_padding;
                float clip_y_bottom = overview_y + overview_height - clip_padding;
                float clip_x_start = overview_lane_left + clip_padding;
                float clip_x_end = overview_lane_left + overview_lane_width - clip_padding;
                float clip_width = clip_x_end - clip_x_start;
                float clip_rounding = S(1.0f);  // Flat style with minimal rounding

                // Clip color - slightly desaturated accent for distinction
                ImVec4 accent = GetWindowsAccentColor();
                ImU32 clip_color = IM_COL32(
                    (int)(accent.x * 50 + 35),
                    (int)(accent.y * 50 + 35),
                    (int)(accent.z * 50 + 40), 255);

                // Draw clip rectangle (no border for flat look)
                draw_list->AddRectFilled(
                    ImVec2(clip_x_start, clip_y_top),
                    ImVec2(clip_x_end, clip_y_bottom),
                    clip_color, clip_rounding);

                // Timeline name label (centered in clip)
                // Prefix based on source mode: Video, Audio, Image Sequence, Comparison, Playlist, or Timeline
                // Hide when no media loaded (empty timeline name and no tracks)
                std::string tl_name;
                if (timeline_view && !timeline_view->GetTimelineName().empty()) {
                    auto mode = timeline_view->GetSourceMode();
                    std::string prefix;
                    if (mode == qcview::TimelineSourceMode::IMAGE_SEQUENCE) {
                        prefix = "Image Sequence: ";
                    } else if (mode == qcview::TimelineSourceMode::DUAL_VIEW) {
                        prefix = "Comparison: ";
                    } else if (mode == qcview::TimelineSourceMode::PLAYLIST) {
                        prefix = "Playlist: ";
                    } else if (mode == qcview::TimelineSourceMode::VIDEO_FILE) {
                        // Check if this is actually an audio-only file (no video tracks)
                        bool has_video_track = false;
                        for (const auto& track : timeline_view->GetTracks()) {
                            if (track.is_video) { has_video_track = true; break; }
                        }
                        prefix = has_video_track ? "Video: " : "Audio: ";
                    } else {
                        prefix = "Timeline: ";
                    }
                    tl_name = prefix + timeline_view->GetTimelineName();
                }
                if (!tl_name.empty() && font_mono) {
                    ImGui::PushFont(font_mono);
                    float available_text_width = clip_width - S(10.0f);
                    std::string display_name = tl_name;
                    ImVec2 text_size = ImGui::CalcTextSize(display_name.c_str());

                    // Truncate with ellipsis if needed
                    if (text_size.x > available_text_width) {
                        while (display_name.length() > 3 &&
                               ImGui::CalcTextSize((display_name.substr(0, display_name.length() - 3) + "...").c_str()).x > available_text_width) {
                            display_name.pop_back();
                        }
                        if (display_name.length() > 3) {
                            display_name = display_name.substr(0, display_name.length() - 3) + "...";
                        }
                        text_size = ImGui::CalcTextSize(display_name.c_str());
                    }

                    float clip_height = clip_y_bottom - clip_y_top;
                    ImVec2 text_pos(
                        clip_x_start + (clip_width - text_size.x) * 0.5f,
                        clip_y_top + (clip_height - text_size.y) * 0.5f
                    );

                    // Draw with shadow for readability
                    draw_list->AddText(ImVec2(text_pos.x + S(1), text_pos.y + S(1)), IM_COL32(0, 0, 0, 160), display_name.c_str());
                    draw_list->AddText(text_pos, IM_COL32(200, 200, 200, 255), display_name.c_str());
                    ImGui::PopFont();
                }

                // === VIEWPORT INDICATOR (shows visible region) ===
                // Always show viewport indicator (even at 1x zoom to allow navigation)
                float total_timeline_width = static_cast<float>(duration) * pixels_per_second;
                float visible_width_px = content_region.x - OTIOTimeline::TRACK_HEADER_WIDTH;

                // Calculate visible portion as fraction of total timeline
                float visible_start_fraction = scroll_offset_x / total_timeline_width;
                float visible_end_fraction = (scroll_offset_x + visible_width_px) / total_timeline_width;

                // Clamp to valid range
                visible_start_fraction = std::clamp(visible_start_fraction, 0.0f, 1.0f);
                visible_end_fraction = std::clamp(visible_end_fraction, 0.0f, 1.0f);

                // Map to clip coordinates
                float viewport_x_start = clip_x_start + visible_start_fraction * clip_width;
                float viewport_x_end = clip_x_start + visible_end_fraction * clip_width;

                // Ensure minimum width for usability (never disappear)
                float min_viewport_width = S(12.0f);
                if (viewport_x_end - viewport_x_start < min_viewport_width) {
                    float center = (viewport_x_start + viewport_x_end) * 0.5f;
                    viewport_x_start = center - min_viewport_width * 0.5f;
                    viewport_x_end = center + min_viewport_width * 0.5f;
                    // Keep within clip bounds
                    if (viewport_x_start < clip_x_start) {
                        viewport_x_start = clip_x_start;
                        viewport_x_end = clip_x_start + min_viewport_width;
                    }
                    if (viewport_x_end > clip_x_end) {
                        viewport_x_end = clip_x_end;
                        viewport_x_start = clip_x_end - min_viewport_width;
                    }
                }

                // Draw viewport indicator fill (flat style, slightly more visible)
                draw_list->AddRectFilled(
                    ImVec2(viewport_x_start, clip_y_top),
                    ImVec2(viewport_x_end, clip_y_bottom),
                    UI_WHITE_A(70),
                    clip_rounding
                );

                // === VIEWPORT DRAG/CLICK/RESIZE INTERACTION ===
                ImVec2 mouse_pos = ImGui::GetMousePos();

                // Edge detection zones for resize handles
                const float edge_zone = S(8.0f);
                bool mouse_in_left_edge = (mouse_pos.x >= viewport_x_start - S(2) &&
                                           mouse_pos.x <= viewport_x_start + edge_zone &&
                                           mouse_pos.y >= clip_y_top &&
                                           mouse_pos.y <= clip_y_bottom);
                bool mouse_in_right_edge = (mouse_pos.x >= viewport_x_end - edge_zone &&
                                            mouse_pos.x <= viewport_x_end + S(2) &&
                                            mouse_pos.y >= clip_y_top &&
                                            mouse_pos.y <= clip_y_bottom);
                bool mouse_in_viewport_middle = (mouse_pos.x > viewport_x_start + edge_zone &&
                                                 mouse_pos.x < viewport_x_end - edge_zone &&
                                                 mouse_pos.y >= clip_y_top &&
                                                 mouse_pos.y <= clip_y_bottom);
                bool mouse_in_viewport = (mouse_pos.x >= viewport_x_start &&
                                          mouse_pos.x <= viewport_x_end &&
                                          mouse_pos.y >= clip_y_top &&
                                          mouse_pos.y <= clip_y_bottom);
                bool mouse_in_clip = (mouse_pos.x >= clip_x_start &&
                                      mouse_pos.x <= clip_x_end &&
                                      mouse_pos.y >= clip_y_top &&
                                      mouse_pos.y <= clip_y_bottom);

                // Static drag state
                static bool is_dragging_overview_viewport = false;
                static bool is_dragging_left_edge = false;
                static bool is_dragging_right_edge = false;
                static float overview_drag_start_scroll = 0.0f;
                static float overview_drag_start_mouse_x = 0.0f;
                static float overview_drag_start_zoom = 0.0f;

                bool any_dragging = is_dragging_overview_viewport || is_dragging_left_edge || is_dragging_right_edge;

                // Change cursor based on hover zone
                if (!any_dragging) {
                    if (mouse_in_left_edge || mouse_in_right_edge) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    } else if (mouse_in_viewport_middle) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    }
                }

                // Start appropriate drag when clicking
                if (ImGui::IsMouseClicked(0)) {
                    if (mouse_in_left_edge) {
                        is_dragging_left_edge = true;
                        overview_drag_start_scroll = scroll_offset_x;
                        overview_drag_start_mouse_x = mouse_pos.x;
                        overview_drag_start_zoom = pixels_per_second;
                    } else if (mouse_in_right_edge) {
                        is_dragging_right_edge = true;
                        overview_drag_start_scroll = scroll_offset_x;
                        overview_drag_start_mouse_x = mouse_pos.x;
                        overview_drag_start_zoom = pixels_per_second;
                    } else if (mouse_in_viewport_middle) {
                        is_dragging_overview_viewport = true;
                        overview_drag_start_scroll = scroll_offset_x;
                        overview_drag_start_mouse_x = mouse_pos.x;
                    }
                }

                // Handle left edge dragging (zoom + scroll)
                if (is_dragging_left_edge) {
                    if (ImGui::IsMouseDown(0)) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

                        // Calculate new left edge position in clip coordinates
                        float new_left_x = std::clamp(mouse_pos.x, clip_x_start, viewport_x_end - min_viewport_width);
                        float new_start_fraction = (new_left_x - clip_x_start) / clip_width;

                        // Keep right edge fixed - calculate visible duration from that
                        float right_time = (scroll_offset_x + visible_width_px) / pixels_per_second;
                        float new_left_time = new_start_fraction * static_cast<float>(duration);
                        float new_visible_duration = right_time - new_left_time;

                        // Calculate new zoom level
                        if (new_visible_duration > 0.01f) {
                            float new_pps = visible_width_px / new_visible_duration;
                            new_pps = std::clamp(new_pps, OTIOTimeline::MIN_PIXELS_PER_SECOND,
                                                 OTIOTimeline::MAX_PIXELS_PER_SECOND);

                            // Calculate new scroll to keep right edge fixed
                            float new_scroll = new_left_time * new_pps;
                            new_scroll = std::max(0.0f, new_scroll);

                            if (timeline_view) {
                                timeline_view->SetZoomLevel(new_pps);
                                timeline_view->SetScrollOffset(new_scroll);
                            }
                        }
                    } else {
                        is_dragging_left_edge = false;
                    }
                }

                // Handle right edge dragging (zoom only, left edge stays fixed)
                if (is_dragging_right_edge) {
                    if (ImGui::IsMouseDown(0)) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

                        // Calculate new right edge position in clip coordinates
                        float new_right_x = std::clamp(mouse_pos.x, viewport_x_start + min_viewport_width, clip_x_end);
                        float new_end_fraction = (new_right_x - clip_x_start) / clip_width;

                        // Keep left edge (scroll position) fixed - calculate visible duration
                        float left_time = scroll_offset_x / pixels_per_second;
                        float new_right_time = new_end_fraction * static_cast<float>(duration);
                        float new_visible_duration = new_right_time - left_time;

                        // Calculate new zoom level
                        if (new_visible_duration > 0.01f) {
                            float new_pps = visible_width_px / new_visible_duration;
                            new_pps = std::clamp(new_pps, OTIOTimeline::MIN_PIXELS_PER_SECOND,
                                                 OTIOTimeline::MAX_PIXELS_PER_SECOND);

                            // Recalculate scroll to keep left time position fixed
                            float new_scroll = left_time * new_pps;
                            float max_scroll = std::max(0.0f, static_cast<float>(duration) * new_pps - visible_width_px);
                            new_scroll = std::clamp(new_scroll, 0.0f, max_scroll);

                            if (timeline_view) {
                                timeline_view->SetZoomLevel(new_pps);
                                timeline_view->SetScrollOffset(new_scroll);
                            }
                        }
                    } else {
                        is_dragging_right_edge = false;
                    }
                }

                // Handle middle dragging (pan only)
                if (is_dragging_overview_viewport) {
                    if (ImGui::IsMouseDown(0)) {
                        float mouse_delta = mouse_pos.x - overview_drag_start_mouse_x;
                        // Convert clip pixel delta to scroll delta
                        float scroll_delta = (mouse_delta / clip_width) * total_timeline_width;
                        float new_scroll = overview_drag_start_scroll + scroll_delta;
                        float max_scroll = std::max(0.0f, total_timeline_width - visible_width_px);
                        new_scroll = std::clamp(new_scroll, 0.0f, max_scroll);

                        if (timeline_view) {
                            timeline_view->SetScrollOffset(new_scroll);
                        }
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    } else {
                        is_dragging_overview_viewport = false;
                    }
                }

                // Click outside viewport but inside clip to jump there
                if (mouse_in_clip && !mouse_in_viewport && !any_dragging && ImGui::IsMouseClicked(0)) {
                    // Center viewport on clicked position
                    float clicked_fraction = (mouse_pos.x - clip_x_start) / clip_width;
                    float viewport_width_fraction = visible_width_px / total_timeline_width;
                    float target_start_fraction = clicked_fraction - viewport_width_fraction * 0.5f;
                    float new_scroll = target_start_fraction * total_timeline_width;
                    float max_scroll = std::max(0.0f, total_timeline_width - visible_width_px);
                    new_scroll = std::clamp(new_scroll, 0.0f, max_scroll);

                    if (timeline_view) {
                        timeline_view->SetScrollOffset(new_scroll);
                    }
                }
            }

            // Separator line below overview track
            float sep_y = overview_y + overview_height;
            draw_list->AddLine(
                ImVec2(tracks_start_pos.x, sep_y),
                ImVec2(tracks_start_pos.x + content_region.x, sep_y),
                IM_COL32(60, 60, 60, 255), S(2.0f)
            );
        }

        // Offset for regular tracks (after overview track + separator)
        float overview_offset = OTIOTimeline::OVERVIEW_TRACK_HEIGHT + OTIOTimeline::TRACK_SEPARATOR_HEIGHT;

        // Get actual tracks from timeline_view (if available)
        std::vector<qcview::OTIOTrack>* tracks_ptr = nullptr;
        if (timeline_view) {
            tracks_ptr = &timeline_view->GetTracks();
        }

        // Track colors - matching playlist style (grey base)
        // Linked clips get accent-tinted colors, unlinked get plain grey
        // Image sequences get a cooler/purple tint to distinguish from video
        auto GetTrackColor = [this](bool is_video, int index, bool is_linked, bool is_sequence) -> ImU32 {
            if (is_linked) {
                // Linked clips: subtle accent-tinted colors
                ImVec4 accent = GetWindowsAccentColor();
                if (is_video) {
                    if (is_sequence) {
                        // IMAGE SEQUENCE: cooler/purple tint to distinguish from video
                        return IM_COL32(
                            (int)(accent.x * 55 + 40),   // Less red
                            (int)(accent.y * 50 + 35),   // Less green
                            (int)(accent.z * 85 + 55), 255);  // More blue/purple
                    } else {
                        // Video clips: standard accent color
                        return IM_COL32(
                            (int)(accent.x * 80 + 40),
                            (int)(accent.y * 80 + 40),
                            (int)(accent.z * 80 + 40), 255);
                    }
                } else {
                    // Audio track clips: warmer/orange tint to distinguish from video
                    return IM_COL32(
                        (int)(accent.x * 70 + 55),   // More warm/orange
                        (int)(accent.y * 55 + 40),   // Less green
                        (int)(accent.z * 50 + 35), 255);  // Less blue
                }
            } else {
                // Unlinked clips: dark grey like playlist non-current clips
                return IM_COL32(60, 60, 60, 255);
            }
        };

        // Render each track (starting after the overview track)
        float current_y = tracks_start_pos.y + overview_offset;
        int num_tracks = tracks_ptr ? static_cast<int>(tracks_ptr->size()) : 0;

        // Empty state - no message needed, just show empty tracks area

        for (int i = 0; i < num_tracks; i++) {
            qcview::OTIOTrack& track = (*tracks_ptr)[i];

            // Hide audio tracks in Audio-only mode (no video tracks, just audio file)
            // The A1 track is not needed since video track handles everything
            if (timeline_view &&
                timeline_view->GetSourceMode() == qcview::TimelineSourceMode::VIDEO_FILE &&
                timeline_view->GetVideoTrackCount() == 0 &&
                !track.is_video) {
                continue;  // Skip this audio track
            }

            float track_y = current_y;

            // Add separator before audio tracks
            if (i > 0 && !track.is_video && (*tracks_ptr)[i-1].is_video) {
                draw_list->AddLine(
                    ImVec2(tracks_start_pos.x, track_y),
                    ImVec2(tracks_start_pos.x + content_region.x, track_y),
                    IM_COL32(60, 60, 60, 255), S(2.0f)
                );
                current_y += OTIOTimeline::TRACK_SEPARATOR_HEIGHT;
                track_y = current_y;
            }

            // Track header icon button position (icons scale with font)
            // NOTE: Icon button must come FIRST so it can receive clicks before the header area
            const float hdr_ui_scale = ImGui::GetIO().FontGlobalScale;
            const float hdr_scale_factor = 1.0f + (hdr_ui_scale - 1.0f) * 0.5f;
            const float track_icon_size = S(18.0f) * hdr_scale_factor;  // Icon font size (scaled)
            const float icon_v_offset = (OTIOTimeline::TRACK_LANE_HEIGHT - track_icon_size) * 0.5f;
            ImVec2 icon_button_pos(tracks_start_pos.x + S(4), track_y + icon_v_offset);
            ImVec2 icon_button_size(S(24) * hdr_scale_factor, track_icon_size);

            ImGui::SetCursorScreenPos(icon_button_pos);
            char icon_btn_id[32];
            snprintf(icon_btn_id, sizeof(icon_btn_id), "##track_icon_%d", i);

            if (track.is_video) {
                // VIDEO TRACK: Eye icon for visibility toggle
                if (ImGui::InvisibleButton(icon_btn_id, icon_button_size)) {
                    track.visible = !track.visible;
                    // Sync TimelineView tracks to flattener for actual playback effect
                    if (timeline_view) {
                        timeline_view->SyncFlattenerAndInvalidate();
                    }
                    Debug::Log("Track " + track.name + " visibility: " +
                              (track.visible ? "ON" : "OFF"));
                }
                bool icon_hovered = ImGui::IsItemHovered();

                // Visual toggle state: bright when ON, dim when OFF, highlight on hover
                const char* vis_icon = track.visible ? ICON_VISIBILITY : ICON_VISIBILITY_OFF;
                ImU32 icon_color;
                if (track.visible) {
                    // ON state: bright white/accent
                    icon_color = icon_hovered ? UI_WHITE : UI_LIGHT_GRAY;
                } else {
                    // OFF state: dim/red tint to show disabled
                    icon_color = icon_hovered ? IM_COL32(180, 100, 100, 255) : IM_COL32(120, 70, 70, 255);
                }

                ImVec2 icon_pos(tracks_start_pos.x + S(6), track_y + icon_v_offset);
                if (font_icons) {
                    draw_list->AddText(font_icons, track_icon_size, icon_pos, icon_color, vis_icon);
                } else {
                    draw_list->AddText(icon_pos, icon_color, track.visible ? "V" : "-");
                }

                // Hide speaker icon in Video, Audio, and Image Sequence modes
                bool show_track_mute = timeline_view &&
                    timeline_view->GetSourceMode() != qcview::TimelineSourceMode::VIDEO_FILE &&
                    timeline_view->GetSourceMode() != qcview::TimelineSourceMode::IMAGE_SEQUENCE;

                if (show_track_mute) {
                    // VIDEO TRACK: Speaker icon for audio mute toggle (second icon)
                    char audio_btn_id[32];
                    snprintf(audio_btn_id, sizeof(audio_btn_id), "##track_audio_%d", i);
                    ImVec2 audio_icon_pos(tracks_start_pos.x + S(6) + icon_button_size.x, track_y + icon_v_offset);
                    ImGui::SetCursorScreenPos(audio_icon_pos);
                    if (ImGui::InvisibleButton(audio_btn_id, icon_button_size)) {
                        track.audio_muted = !track.audio_muted;
                        // Sync TimelineView tracks to flattener for actual audio effect
                        if (timeline_view) {
                            timeline_view->SyncFlattenerAndInvalidate();
                        }
                        Debug::Log("Track " + track.name + " audio: " +
                                  (track.audio_muted ? "MUTED" : "ON"));
                    }
                    bool audio_icon_hovered = ImGui::IsItemHovered();

                    const char* audio_icon = track.audio_muted ? ICON_VOLUME_MUTE : ICON_VOLUME_UP;
                    ImU32 audio_icon_color;
                    if (!track.audio_muted) {
                        audio_icon_color = audio_icon_hovered ? UI_WHITE : UI_LIGHT_GRAY;
                    } else {
                        audio_icon_color = audio_icon_hovered ? IM_COL32(180, 100, 100, 255) : IM_COL32(120, 70, 70, 255);
                    }

                    ImVec2 audio_draw_pos(tracks_start_pos.x + S(6) + icon_button_size.x + S(2), track_y + icon_v_offset);
                    if (font_icons) {
                        draw_list->AddText(font_icons, track_icon_size, audio_draw_pos, audio_icon_color, audio_icon);
                    } else {
                        draw_list->AddText(audio_draw_pos, audio_icon_color, track.audio_muted ? "M" : "S");
                    }
                }
            } else {
                // Hide speaker icon in Video/Audio modes 
                // Keep speaker for Image Sequence mode (audio tracks are user-added)
                bool show_track_mute = timeline_view &&
                    timeline_view->GetSourceMode() != qcview::TimelineSourceMode::VIDEO_FILE;

                if (show_track_mute) {
                    // AUDIO TRACK: Speaker icon for mute toggle
                    if (ImGui::InvisibleButton(icon_btn_id, icon_button_size)) {
                        track.muted = !track.muted;
                        // Sync TimelineView tracks to flattener for actual audio effect
                        if (timeline_view) {
                            timeline_view->SyncFlattenerAndInvalidate();
                        }
                        Debug::Log("Track " + track.name + " audio: " +
                                  (track.muted ? "MUTED" : "ON"));
                    }
                    bool icon_hovered = ImGui::IsItemHovered();

                    // Visual toggle state: bright when unmuted, dim/red when muted
                    const char* mute_icon = track.muted ? ICON_VOLUME_MUTE : ICON_VOLUME_UP;
                    ImU32 icon_color;
                    if (!track.muted) {
                        // Unmuted: bright
                        icon_color = icon_hovered ? UI_WHITE : UI_LIGHT_GRAY;
                    } else {
                        // Muted: dim/red tint
                        icon_color = icon_hovered ? IM_COL32(180, 100, 100, 255) : IM_COL32(120, 70, 70, 255);
                    }

                    ImVec2 icon_pos(tracks_start_pos.x + S(6), track_y + icon_v_offset);
                    if (font_icons) {
                        draw_list->AddText(font_icons, track_icon_size, icon_pos, icon_color, mute_icon);
                    } else {
                        draw_list->AddText(icon_pos, icon_color, track.muted ? "M" : "S");
                    }
                }
            }

            // Track name offset depends on number of visible icons
            // Video tracks: 2 icons normally, 1 icon (eye only) in Video/Image/Audio mode
            // Audio tracks: 1 icon normally, 0 icons in Video/Audio mode (but keep icon in Image Sequence)
            float name_h_offset;
            if (track.is_video) {
                bool video_icons_hidden = timeline_view &&
                    (timeline_view->GetSourceMode() == qcview::TimelineSourceMode::VIDEO_FILE ||
                     timeline_view->GetSourceMode() == qcview::TimelineSourceMode::IMAGE_SEQUENCE);
                name_h_offset = video_icons_hidden ? (S(32.0f) * hdr_scale_factor) : (S(56.0f) * hdr_scale_factor);
            } else {
                bool audio_icons_hidden = timeline_view &&
                    timeline_view->GetSourceMode() == qcview::TimelineSourceMode::VIDEO_FILE;
                name_h_offset = audio_icons_hidden ? (S(8.0f) * hdr_scale_factor) : (S(32.0f) * hdr_scale_factor);
            }
            const float text_v_offset = (OTIOTimeline::TRACK_LANE_HEIGHT - ImGui::GetTextLineHeight()) * 0.5f;
            ImVec2 name_pos(tracks_start_pos.x + name_h_offset, track_y + text_v_offset);
            ImU32 name_color = (track.is_video ? (track.visible && !track.audio_muted) : !track.muted) ?
                IM_COL32(180, 180, 180, 255) : IM_COL32(100, 100, 100, 255);
            draw_list->AddText(name_pos, name_color, track.name.c_str());

            // Track header - right-click detection for context menu
            // This comes AFTER the icon button so it doesn't block icon clicks
            ImVec2 header_pos(tracks_start_pos.x, track_y);
            ImVec2 header_size(OTIOTimeline::TRACK_HEADER_WIDTH, OTIOTimeline::TRACK_LANE_HEIGHT);
            ImGui::SetCursorScreenPos(header_pos);
            char header_btn_id[32];
            snprintf(header_btn_id, sizeof(header_btn_id), "##header_%d", i);
            ImGui::InvisibleButton(header_btn_id, header_size);

            // Right-click on track header opens context menu
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {
                right_clicked_track_header_index = i;
                show_track_context_menu = true;
            }

            // Track lane separator
            draw_list->AddLine(
                ImVec2(tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH, track_y),
                ImVec2(tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH,
                       track_y + OTIOTimeline::TRACK_LANE_HEIGHT),
                IM_COL32(50, 50, 50, 255), S(1.0f)
            );

            // Track lane background (alternate colors)
            ImU32 lane_bg = (i % 2 == 0) ? IM_COL32(25, 25, 25, 255) : IM_COL32(30, 30, 30, 255);
            draw_list->AddRectFilled(
                ImVec2(tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH, track_y),
                ImVec2(tracks_start_pos.x + content_region.x,
                       track_y + OTIOTimeline::TRACK_LANE_HEIGHT),
                lane_bg
            );

            // Render actual clips on this track (fade if track is hidden)
            {
                const float clip_rounding = S(1.0f);  // Flat style with minimal rounding
                const float fade_alpha = track.visible ? 1.0f : 0.35f;  // Fade hidden tracks
                int clip_index = 0;

                for (const auto& clip : track.clips) {
                    if (clip.is_gap) {
                        clip_index++;
                        continue;  // Don't render gaps
                    }

                    float clip_x = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH +
                                   static_cast<float>(clip.start_time) * pixels_per_second - scroll_offset_x;
                    float clip_w = static_cast<float>(clip.duration) * pixels_per_second;

                    // Clamp to visible area
                    float visible_left = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH;
                    float visible_right = tracks_start_pos.x + content_region.x;

                    // Only render if visible in viewport
                    if (clip_x + clip_w > visible_left && clip_x < visible_right) {
                        // Clamp clip rendering to visible area
                        float render_left = std::max(clip_x + S(2), visible_left);
                        float render_right = std::min(clip_x + clip_w - S(2), visible_right);

                        if (render_right > render_left) {
                            // Check selection state
                            bool is_selected = timeline_view && timeline_view->GetSelection().IsSelected(clip.id);
                            ImVec4 accent = GetWindowsAccentColor();

                            // Get clip color based on link status and sequence type
                            ImU32 fill_color = GetTrackColor(track.is_video, i, clip.is_linked, clip.is_sequence);

                            // Apply fade for hidden tracks
                            if (!track.visible) {
                                int r = (fill_color >> 0) & 0xFF;
                                int g = (fill_color >> 8) & 0xFF;
                                int b = (fill_color >> 16) & 0xFF;
                                fill_color = IM_COL32(r, g, b, (int)(255 * fade_alpha));
                            }

                            ImU32 border_color;
                            if (clip.is_linked) {
                                // Linked: lighter border matching accent
                                border_color = IM_COL32(
                                    (int)(accent.x * 120 + 60),
                                    (int)(accent.y * 120 + 60),
                                    (int)(accent.z * 120 + 60), (int)(255 * fade_alpha));
                            } else {
                                // Unlinked: grey border like playlist
                                border_color = IM_COL32(90, 90, 90, (int)(255 * fade_alpha));
                            }

                            float clip_top = track_y + S(2);
                            float clip_bottom = track_y + OTIOTimeline::TRACK_LANE_HEIGHT - S(2);
                            float clip_height = clip_bottom - clip_top;
                            float clip_render_width = render_right - render_left;

                            // Selection highlight - brighter version of clip type color
                            if (is_selected && track.visible) {
                                // Maintain clip type color differentiation while making it brighter
                                if (track.is_video) {
                                    if (clip.is_sequence) {
                                        // IMAGE SEQUENCE: brighter cooler/purple tint
                                        fill_color = IM_COL32(
                                            (int)(accent.x * 110 + 60),
                                            (int)(accent.y * 100 + 55),
                                            (int)(accent.z * 170 + 80), 255);
                                    } else {
                                        // Video clips: brighter standard accent
                                        fill_color = IM_COL32(
                                            (int)(accent.x * 180 + 40),
                                            (int)(accent.y * 180 + 40),
                                            (int)(accent.z * 180 + 40), 255);
                                    }
                                } else {
                                    // Audio track clips: brighter warm/orange tint
                                    fill_color = IM_COL32(
                                        (int)(accent.x * 160 + 80),
                                        (int)(accent.y * 130 + 60),
                                        (int)(accent.z * 100 + 50), 255);
                                }
                                border_color = IM_COL32(
                                    (int)(accent.x * 255),
                                    (int)(accent.y * 255),
                                    (int)(accent.z * 255), 255);
                            }

                            // Clip rectangle with playlist-style rounding
                            draw_list->AddRectFilled(
                                ImVec2(render_left, clip_top),
                                ImVec2(render_right, clip_bottom),
                                fill_color, clip_rounding
                            );

                            // Thumbnail rendering for VIDEO_FILE and IMAGE_SEQUENCE modes
                            // Show tiled thumbnails across the clip width for visual preview
                            // Respects "Enable Timeline Thumbnails" setting from cache settings
                            if (cache_settings.enable_thumbnails &&
                                timeline_view && track.is_video && clip.is_linked && !clip.linked_path.empty() &&
                                clip_render_width > 50.0f && timeline_thumbnail_cache) {

                                auto source_mode = timeline_view->GetSourceMode();
                                // Enable thumbnails only for single-file modes (not DUAL_VIEW or PLAYLIST)
                                // Complex timelines with mixed clips would make thumbnails confusing/expensive
                                bool show_clip_thumbnails = (source_mode == qcview::TimelineSourceMode::VIDEO_FILE ||
                                                            source_mode == qcview::TimelineSourceMode::IMAGE_SEQUENCE);

                                if (show_clip_thumbnails) {
                                    // Calculate thumbnail dimensions to fit track height
                                    float thumb_height = clip_height - S(4.0f);  // Leave 2px padding top/bottom
                                    float aspect_ratio = 16.0f / 9.0f;  // Default aspect ratio
                                    float thumb_width = thumb_height * aspect_ratio;

                                    // Calculate how many thumbnails can fit in VISIBLE area (cap at 30)
                                    float available_width = clip_render_width - S(4.0f);  // 2px padding each side
                                    int num_thumbnails = std::max(1, static_cast<int>(available_width / thumb_width));
                                    num_thumbnails = std::min(num_thumbnails, 30);  // Cap BEFORE calculating width
                                    float actual_thumb_width = available_width / static_cast<float>(num_thumbnails);

                                    // Get frame rate for time-to-frame conversion
                                    double source_fps = (clip.source_fps > 0) ? clip.source_fps : timeline_view->GetFrameRate();
                                    if (source_fps <= 0) source_fps = 24.0;  // Fallback

                                    // Render thumbnails - position-based sampling so they stick with zoom/pan
                                    float thumb_x = render_left + S(2.0f);
                                    float thumb_y = clip_top + S(2.0f);

                                    for (int t = 0; t < num_thumbnails; ++t) {
                                        // Calculate the CENTER of this thumbnail slot
                                        float thumb_center_x = thumb_x + actual_thumb_width * 0.5f;

                                        // Convert pixel position to fraction of FULL clip (not just visible)
                                        // clip_x is the full clip start, clip_w is full clip width
                                        float clip_fraction = (thumb_center_x - clip_x) / clip_w;
                                        clip_fraction = std::clamp(clip_fraction, 0.0f, 1.0f);

                                        // Convert to source time, accounting for source_in offset
                                        double time_in_clip = clip_fraction * clip.duration;
                                        double source_time = clip.source_in + time_in_clip;
                                        int source_frame = static_cast<int>(source_time * source_fps);
                                        source_frame = std::max(0, source_frame);

                                        // Get thumbnail from cache
                                        // For sequences, construct the actual file path
                                        std::string thumb_path;
                                        int thumb_frame = source_frame;
                                        if (clip.is_sequence && !clip.sequence_directory.empty() && !clip.sequence_pattern.empty()) {
                                            // Construct actual file path: directory + pattern with frame number
                                            int file_frame = clip.sequence_start_frame + source_frame;
                                            char frame_file[512];
                                            snprintf(frame_file, sizeof(frame_file), clip.sequence_pattern.c_str(), file_frame);
                                            thumb_path = clip.sequence_directory + "/" + frame_file;
                                            // For EXR with layer, append layer info so cache can use it
                                            if (!clip.sequence_exr_layer.empty()) {
                                                thumb_path += "?layer=" + clip.sequence_exr_layer;
                                            }
                                            thumb_frame = 0;  // Path already identifies the specific file
                                        } else {
                                            thumb_path = clip.linked_path;
                                        }

                                        int thumb_w = 0, thumb_h = 0;
                                        GLuint thumb_texture = timeline_thumbnail_cache->GetThumbnail(
                                            thumb_path, thumb_frame, thumb_w, thumb_h, true);

                                        if (thumb_texture != 0 && thumb_w > 0 && thumb_h > 0) {
                                            // Calculate actual aspect ratio from thumbnail
                                            float actual_aspect = static_cast<float>(thumb_w) / static_cast<float>(thumb_h);
                                            float draw_width = actual_thumb_width;
                                            float draw_height = thumb_height;

                                            // Fit to height, maintain aspect ratio
                                            if (draw_width / draw_height > actual_aspect) {
                                                draw_width = draw_height * actual_aspect;
                                            } else {
                                                draw_height = draw_width / actual_aspect;
                                            }

                                            // Center thumbnail in its slot
                                            float offset_x = (actual_thumb_width - draw_width) * 0.5f;
                                            float offset_y = (thumb_height - draw_height) * 0.5f;

                                            // Draw dark background for letterbox/pillarbox effect
                                            // This makes the aspect ratio handling visible
                                            draw_list->AddRectFilled(
                                                ImVec2(thumb_x, thumb_y),
                                                ImVec2(thumb_x + actual_thumb_width, thumb_y + thumb_height),
                                                IM_COL32(15, 15, 15, 255)  // Dark letterbox bars
                                            );

                                            // Draw thumbnail (full opacity now that we have letterbox background)
                                            draw_list->AddImage(
                                                ThumbnailPoolIDToImTexture(thumb_texture),
                                                ImVec2(thumb_x + offset_x, thumb_y + offset_y),
                                                ImVec2(thumb_x + offset_x + draw_width, thumb_y + offset_y + draw_height),
                                                ImVec2(0, 0), ImVec2(1, 1),
                                                UI_WHITE
                                            );
                                        }

                                        thumb_x += actual_thumb_width;
                                    }
                                    // NOTE: ProcessPendingUploads moved to before ImGui::NewFrame()
                                }
                            }

                            // Selection highlight - border only when selected (flat look otherwise)
                            if (is_selected && track.visible) {
                                ImU32 selection_color = IM_COL32(
                                    (int)(accent.x * 255),
                                    (int)(accent.y * 255),
                                    (int)(accent.z * 255), 200);
                                draw_list->AddRect(
                                    ImVec2(render_left, clip_top),
                                    ImVec2(render_right, clip_bottom),
                                    selection_color, clip_rounding, 0, S(2.0f)
                                );
                            }

                            // Clip name - centered with shadow, matching playlist style
                            // Skip for VIDEO_FILE mode (thumbnails + minimap name is enough)
                            // For IMAGE_SEQUENCE: skip video track names, but show audio track clip names
                            // DUAL_VIEW shows clip names since there's no thumbnails
                            bool skip_clip_name = false;
                            if (timeline_view) {
                                auto source_mode = timeline_view->GetSourceMode();
                                if (source_mode == qcview::TimelineSourceMode::VIDEO_FILE) {
                                    skip_clip_name = true;
                                } else if (source_mode == qcview::TimelineSourceMode::IMAGE_SEQUENCE) {
                                    // Only skip names on video track; show names on audio track
                                    skip_clip_name = track.is_video;
                                }
                            }

                            if (!skip_clip_name && clip_render_width > 30.0f && font_mono) {
                                ImGui::PushFont(font_mono);

                                // Truncate clip name if needed
                                std::string display_name = clip.name;
                                ImVec2 text_size = ImGui::CalcTextSize(display_name.c_str());

                                float available_text_width = clip_render_width - S(6.0f);
                                if (text_size.x > available_text_width) {
                                    while (display_name.length() > 3 &&
                                           ImGui::CalcTextSize((display_name.substr(0, display_name.length() - 3) + "...").c_str()).x > available_text_width) {
                                        display_name.pop_back();
                                    }
                                    if (display_name.length() > 3) {
                                        display_name = display_name.substr(0, display_name.length() - 3) + "...";
                                    }
                                    text_size = ImGui::CalcTextSize(display_name.c_str());
                                }

                                // Center text vertically and horizontally
                                ImVec2 text_pos(
                                    render_left + (clip_render_width - text_size.x) * 0.5f,
                                    clip_top + (clip_height - text_size.y) * 0.5f
                                );

                                // Draw text with shadow for readability
                                draw_list->AddText(ImVec2(text_pos.x + S(1), text_pos.y + S(1)), IM_COL32(0, 0, 0, 180), display_name.c_str());
                                draw_list->AddText(text_pos, UI_WHITE, display_name.c_str());

                                ImGui::PopFont();
                            }

                            // Nested composition indicator (top-left corner)
                            if (clip.is_nested && clip_render_width > 30.0f && font_icons) {
                                const float nest_icon_size = S(16.0f);
                                const float nest_padding = S(3.0f);
                                ImVec2 nest_icon_pos(render_left + nest_padding, clip_top + nest_padding);

                                // Draw layers icon to indicate nested composition
                                ImU32 nest_icon_color = UI_WHITE_A(220);
                                draw_list->AddText(font_icons, nest_icon_size,
                                    ImVec2(nest_icon_pos.x + S(1), nest_icon_pos.y + S(1)),
                                    IM_COL32(0, 0, 0, 180), ICON_LAYERS);
                                draw_list->AddText(font_icons, nest_icon_size,
                                    nest_icon_pos, nest_icon_color, ICON_LAYERS);
                            }

                            // Clip interaction detection
                            ImVec2 clip_min(render_left, clip_top);
                            ImVec2 clip_max(render_right, clip_bottom);
                            ImVec2 current_mouse = ImGui::GetMousePos();

                            if (ImGui::IsMouseHoveringRect(clip_min, clip_max)) {
                                // Detect if mouse is near clip edges (for trim cursor)
                                const float edge_zone = S(8.0f);  // 8 pixels from edge
                                bool near_left_edge = (current_mouse.x >= render_left && current_mouse.x <= render_left + edge_zone);
                                bool near_right_edge = (current_mouse.x >= render_right - edge_zone && current_mouse.x <= render_right);

                                // IMAGE_SEQUENCE mode: video track is locked (no drag, no trim)
                                bool track_is_locked = false;
                                if (timeline_view && timeline_view->GetSourceMode() == qcview::TimelineSourceMode::IMAGE_SEQUENCE) {
                                    track_is_locked = track.is_video;
                                }

                                // Set cursor based on position (but not for locked tracks)
                                if (!track_is_locked && (near_left_edge || near_right_edge)) {
                                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                                }

                                if (!track_is_locked && ImGui::IsMouseClicked(0) && timeline_view && !timeline_clip_drag.active && !timeline_trim_state.active) {
                                    // Calculate mouse time for drag offset
                                    float clip_relative_x = current_mouse.x - (tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH);
                                    double click_time = (clip_relative_x + scroll_offset_x) / pixels_per_second;

                                    Debug::Log("Clip click: near_left=" + std::to_string(near_left_edge) +
                                               ", near_right=" + std::to_string(near_right_edge) +
                                               ", clip=" + clip.id);

                                    if (near_left_edge) {
                                        // Start left edge trim
                                        Debug::Log("Starting LEFT edge trim for clip: " + clip.id);
                                        // Pause playback during trim for thread safety
                                        if (timeline_view) timeline_view->BeginEdit();
                                        timeline_trim_state.active = true;
                                        timeline_trim_state.clip_id = clip.id;
                                        timeline_trim_state.track_index = i;
                                        timeline_trim_state.is_left_edge = true;
                                        timeline_trim_state.original_value = clip.start_time;
                                        timeline_trim_state.original_duration = clip.duration;
                                        timeline_trim_state.original_source_in = clip.source_in;

                                        // Start precaching thumbnails for trim preview
                                        if (timeline_thumbnail_cache && clip.is_linked && !clip.linked_path.empty()) {
                                            double source_fps = (clip.source_fps > 0) ? clip.source_fps : 24.0;
                                            int start_frame = 0;
                                            int end_frame = static_cast<int>(clip.source_duration * source_fps);

                                            // Handle tape timecode - source_in may be absolute
                                            bool source_in_is_tape_tc = (clip.source_duration > 0) ?
                                                (clip.source_in > clip.source_duration) : (clip.source_in > 3600.0);
                                            int current_frame = source_in_is_tape_tc ? 0 :
                                                static_cast<int>(clip.source_in * source_fps);

                                            timeline_thumbnail_cache->PrecacheRange(clip.linked_path, start_frame, end_frame, current_frame, 12);
                                        }
                                    } else if (near_right_edge) {
                                        // Start right edge trim
                                        Debug::Log("Starting RIGHT edge trim for clip: " + clip.id);
                                        // Pause playback during trim for thread safety
                                        if (timeline_view) timeline_view->BeginEdit();
                                        timeline_trim_state.active = true;
                                        timeline_trim_state.clip_id = clip.id;
                                        timeline_trim_state.track_index = i;
                                        timeline_trim_state.is_left_edge = false;
                                        timeline_trim_state.original_value = clip.start_time + clip.duration;
                                        timeline_trim_state.original_duration = clip.duration;
                                        timeline_trim_state.original_source_in = clip.source_in;

                                        // Start precaching thumbnails for trim preview
                                        if (timeline_thumbnail_cache && clip.is_linked && !clip.linked_path.empty()) {
                                            double source_fps = (clip.source_fps > 0) ? clip.source_fps : 24.0;
                                            int start_frame = 0;
                                            int end_frame = static_cast<int>(clip.source_duration * source_fps);

                                            // Handle tape timecode - source_in may be absolute
                                            bool source_in_is_tape_tc = (clip.source_duration > 0) ?
                                                (clip.source_in > clip.source_duration) : (clip.source_in > 3600.0);
                                            int current_frame = source_in_is_tape_tc ?
                                                static_cast<int>(clip.duration * source_fps) :
                                                static_cast<int>((clip.source_in + clip.duration) * source_fps);

                                            timeline_thumbnail_cache->PrecacheRange(clip.linked_path, start_frame, end_frame, current_frame, 12);
                                        }
                                    } else {
                                        // PLAYLIST mode: Shift+drag = slip, regular drag = reorder
                                        // But require drag threshold before starting edit
                                        bool is_playlist_mode = timeline_view->GetSourceMode() == qcview::TimelineSourceMode::PLAYLIST;
                                        bool shift_held = ImGui::GetIO().KeyShift;

                                        if (is_playlist_mode && !clip.is_gap) {
                                            // Select the clip immediately
                                            timeline_view->GetSelection().SelectClip(clip.id, 0, ImGui::GetIO().KeyCtrl);

                                            // Find clip index in tracks[0]
                                            int clip_idx = -1;
                                            auto& tracks = timeline_view->GetTracks();
                                            if (!tracks.empty()) {
                                                for (size_t ci = 0; ci < tracks[0].clips.size(); ci++) {
                                                    if (tracks[0].clips[ci].id == clip.id) {
                                                        clip_idx = static_cast<int>(ci);
                                                        break;
                                                    }
                                                }
                                            }

                                            // Set up PENDING state - don't start edit until drag threshold exceeded
                                            // Store info needed to start the edit later
                                            playlist_pending_drag_clip_id = clip.id;
                                            playlist_pending_drag_clip_idx = clip_idx;
                                            playlist_pending_drag_start_x = current_mouse.x;
                                            playlist_pending_drag_shift = shift_held;
                                            playlist_pending_drag_source_in = clip.source_in;
                                            playlist_pending_drag_source_out = clip.source_out;
                                            playlist_pending_drag_source_duration = clip.source_duration;
                                            playlist_pending_drag_clip_duration = clip.duration;
                                            playlist_pending_drag_active = true;
                                            Debug::Log("[Playlist] Click on clip: " + clip.id + " (pending drag)");
                                        } else if (!is_playlist_mode) {
                                            // Standard multi-track mode: Select clip and prepare for potential drag
                                            Debug::Log("Starting DRAG for clip: " + clip.id);
                                            bool add_to_selection = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
                                            timeline_view->GetSelection().SelectClip(clip.id, i, add_to_selection);

                                            // Pause playback during drag for thread safety
                                            // This prevents race conditions with CacheManagementThread and AudioMixer
                                            timeline_view->BeginEdit();

                                            // Start drag
                                            timeline_clip_drag.active = true;
                                            timeline_clip_drag.clip_id = clip.id;
                                            timeline_clip_drag.original_track_index = i;
                                            timeline_clip_drag.original_start_time = clip.start_time;
                                            timeline_clip_drag.drag_offset = click_time - clip.start_time;

                                            // Capture all selected clips for multi-clip drag
                                            timeline_clip_drag.dragged_clips.clear();
                                            const auto& selection = timeline_view->GetSelection();
                                            if (selection.selected_clip_ids.size() > 0) {
                                                const auto& all_tracks = timeline_view->GetTracks();
                                                for (const auto& sel_id : selection.selected_clip_ids) {
                                                    // Find this clip in tracks
                                                    for (int ti = 0; ti < static_cast<int>(all_tracks.size()); ++ti) {
                                                        for (const auto& c : all_tracks[ti].clips) {
                                                            if (c.id == sel_id) {
                                                                qcview::TimelineClipDragState::DraggedClipInfo info;
                                                                info.clip_id = c.id;
                                                                info.track_index = ti;
                                                                info.original_start_time = c.start_time;
                                                                info.duration = c.duration;
                                                                info.offset_from_primary = c.start_time - clip.start_time;
                                                                timeline_clip_drag.dragged_clips.push_back(info);
                                                                break;
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                // Right-click = context menu
                                if (ImGui::IsMouseClicked(1)) {
                                    right_clicked_clip_id = clip.id;
                                    right_clicked_track_index = i;
                                    show_clip_context_menu = true;
                                    // Also select the right-clicked clip if not already selected
                                    if (timeline_view && !timeline_view->GetSelection().IsSelected(clip.id)) {
                                        timeline_view->GetSelection().SelectClip(clip.id, i, false);
                                    }
                                }

                                // Double-click on nested clip = enter nested timeline
                                if (ImGui::IsMouseDoubleClicked(0) && clip.is_nested && timeline_view) {
                                    Debug::Log("Double-click on nested clip: " + clip.name);
                                    if (timeline_view->EnterNestedClip(clip.id)) {
                                        Debug::Log("Entered nested timeline: " + clip.name);
                                    }
                                }

                                // Hover tooltip with thumbnail preview (only when not dragging/trimming)
                                // DISABLED: Using ruler hover thumbnail instead to avoid duplicate hover states
                                if (false && !ImGui::IsMouseDown(0) && !timeline_clip_drag.active && !timeline_trim_state.active &&
                                    cache_settings.enable_thumbnails && clip.is_linked) {

                                    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(35, 35, 35, 245));
                                    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(100, 100, 100, 100));
                                    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
                                    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
                                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(8)));

                                    ImGui::BeginTooltip();

                                    // Thumbnail preview (if available)
                                    if (timeline_thumbnail_cache && !clip.linked_path.empty()) {
                                        // Calculate frame for thumbnail based on clip midpoint
                                        double source_fps = (clip.source_fps > 0) ? clip.source_fps : timeline_view->GetFrameRate();
                                        if (source_fps <= 0) source_fps = 24.0;
                                        double mid_time = clip.source_in + clip.duration * 0.5;
                                        int thumb_frame = static_cast<int>(mid_time * source_fps);
                                        thumb_frame = std::max(0, thumb_frame);

                                        // Construct thumbnail path
                                        std::string thumb_path;
                                        int actual_thumb_frame = thumb_frame;
                                        if (clip.is_sequence && !clip.sequence_directory.empty() && !clip.sequence_pattern.empty()) {
                                            int file_frame = clip.sequence_start_frame + thumb_frame;
                                            char frame_file[512];
                                            snprintf(frame_file, sizeof(frame_file), clip.sequence_pattern.c_str(), file_frame);
                                            thumb_path = clip.sequence_directory + "/" + frame_file;
                                            if (!clip.sequence_exr_layer.empty()) {
                                                thumb_path += "?layer=" + clip.sequence_exr_layer;
                                            }
                                            actual_thumb_frame = 0;
                                        } else {
                                            thumb_path = clip.linked_path;
                                        }

                                        int frame_width = 0, frame_height = 0;
                                        GLuint preview_texture = timeline_thumbnail_cache->GetThumbnail(
                                            thumb_path, actual_thumb_frame, frame_width, frame_height, true);

                                        // Fixed preview size with aspect ratio preservation
                                        const float preview_max_w = S(200.0f);
                                        const float preview_max_h = S(120.0f);

                                        if (preview_texture != 0 && frame_width > 0 && frame_height > 0) {
                                            float aspect = static_cast<float>(frame_width) / frame_height;
                                            float draw_w = preview_max_w;
                                            float draw_h = preview_max_w / aspect;
                                            if (draw_h > preview_max_h) {
                                                draw_h = preview_max_h;
                                                draw_w = preview_max_h * aspect;
                                            }

                                            // Center thumbnail in preview area
                                            float offset_x = (preview_max_w - draw_w) * 0.5f;
                                            float offset_y = (preview_max_h - draw_h) * 0.5f;

                                            ImVec2 cursor = ImGui::GetCursorPos();
                                            // Dark background for letterbox/pillarbox
                                            ImGui::GetWindowDrawList()->AddRectFilled(
                                                ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y),
                                                ImVec2(ImGui::GetCursorScreenPos().x + preview_max_w, ImGui::GetCursorScreenPos().y + preview_max_h),
                                                IM_COL32(15, 15, 15, 255), S(4.0f));

                                            ImGui::SetCursorPos(ImVec2(cursor.x + offset_x, cursor.y + offset_y));
                                            ImGui::Image(ThumbnailPoolIDToImTexture(preview_texture), ImVec2(draw_w, draw_h));
                                            ImGui::SetCursorPosY(cursor.y + preview_max_h + S(6.0f));
                                        } else {
                                            // Placeholder for loading thumbnail
                                            ImGui::GetWindowDrawList()->AddRectFilled(
                                                ImGui::GetCursorScreenPos(),
                                                ImVec2(ImGui::GetCursorScreenPos().x + preview_max_w, ImGui::GetCursorScreenPos().y + preview_max_h),
                                                IM_COL32(30, 30, 30, 255), S(4.0f));
                                            ImGui::Dummy(ImVec2(preview_max_w, preview_max_h));
                                        }

                                        ImGui::Spacing();
                                    }

                                    // Clip info text
                                    if (font_regular) ImGui::PushFont(font_regular);

                                    // Clip name
                                    ImGui::TextColored(UI_WHITE_VEC4, "%s", clip.name.c_str());

                                    // Duration formatted as MM:SS:FF
                                    double duration_sec = clip.duration;
                                    int minutes = static_cast<int>(duration_sec / 60.0);
                                    int seconds = static_cast<int>(duration_sec) % 60;
                                    int frames_part = static_cast<int>((duration_sec - static_cast<int>(duration_sec)) * timeline_view->GetFrameRate());
                                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Duration: %02d:%02d:%02d", minutes, seconds, frames_part);

                                    // File name (truncated)
                                    std::string filename = clip.linked_path.empty() ? clip.file_path : clip.linked_path;
                                    size_t last_slash = filename.find_last_of("/\\");
                                    if (last_slash != std::string::npos) {
                                        filename = filename.substr(last_slash + 1);
                                    }
                                    if (filename.length() > 40) {
                                        filename = filename.substr(0, 37) + "...";
                                    }
                                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", filename.c_str());

                                    if (font_regular) ImGui::PopFont();

                                    ImGui::EndTooltip();

                                    ImGui::PopStyleVar(3);
                                    ImGui::PopStyleColor(2);
                                }
                            }
                        }
                    }
                    clip_index++;
                }
            }

            // Track bottom border
            draw_list->AddLine(
                ImVec2(tracks_start_pos.x, track_y + OTIOTimeline::TRACK_LANE_HEIGHT),
                ImVec2(tracks_start_pos.x + content_region.x,
                       track_y + OTIOTimeline::TRACK_LANE_HEIGHT),
                IM_COL32(40, 40, 40, 255), 1.0f
            );

            current_y += OTIOTimeline::TRACK_LANE_HEIGHT;
        }

        // Right-click on empty area (track header column, below all tracks)
        // Add a small empty area at the bottom for adding tracks when timeline is empty
        float empty_area_height = std::max(S(30.0f), content_region.y - tracks_height - OTIOTimeline::TIMELINE_RULER_HEIGHT - S(100));
        if (empty_area_height > S(10.0f)) {
            ImVec2 empty_header_pos(tracks_start_pos.x, current_y);
            ImVec2 empty_header_size(OTIOTimeline::TRACK_HEADER_WIDTH, empty_area_height);

            ImGui::SetCursorScreenPos(empty_header_pos);
            if (ImGui::InvisibleButton("##empty_track_area", empty_header_size)) {
                // Left click - do nothing for now
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {
                show_empty_area_context_menu = true;
            }
        }

        // =====================================================================
        // ROW 3: Timeline Ruler - matching transport panel style
        // =====================================================================
        float ruler_y = tracks_start_pos.y + tracks_height;
        float ruler_left = tracks_start_pos.x;
        float ruler_right = tracks_start_pos.x + content_region.x;

        // Ruler background - semi-transparent like transport panel
        draw_list->AddRectFilled(
            ImVec2(ruler_left, ruler_y),
            ImVec2(ruler_right, ruler_y + OTIOTimeline::TIMELINE_RULER_HEIGHT),
            IM_COL32(16, 16, 16, 60)
        );

        // Top and bottom border lines - matching transport panel
        ImU32 ruler_border_color = IM_COL32(160, 160, 160, 50);
        draw_list->AddLine(
            ImVec2(ruler_left, ruler_y),
            ImVec2(ruler_right, ruler_y),
            ruler_border_color, S(1.0f)
        );
        draw_list->AddLine(
            ImVec2(ruler_left, ruler_y + OTIOTimeline::TIMELINE_RULER_HEIGHT),
            ImVec2(ruler_right, ruler_y + OTIOTimeline::TIMELINE_RULER_HEIGHT),
            ruler_border_color, S(1.0f)
        );

        // Loop region highlight - draw colored background between In/Out points
        // Using Bright accent color for markers
        if (timeline_view && (timeline_view->HasTimelineInPoint() || timeline_view->HasTimelineOutPoint())) {
            float track_lane_left = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH;
            ImVec4 io_accent = Bright(GetWindowsAccentColor());
            ImU32 io_marker_color = ToImU32(io_accent);

            // Calculate In point position if set
            float loop_start_x = -1;
            if (timeline_view->HasTimelineInPoint()) {
                double tl_in = timeline_view->GetTimelineInPoint();
                loop_start_x = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH +
                              static_cast<float>(tl_in) * pixels_per_second - scroll_offset_x;

                // Draw In point (accent color line with triangle) - if in visible area
                if (loop_start_x >= track_lane_left && loop_start_x <= ruler_right) {
                    draw_list->AddLine(
                        ImVec2(loop_start_x, ruler_y),
                        ImVec2(loop_start_x, ruler_y + OTIOTimeline::TIMELINE_RULER_HEIGHT),
                        io_marker_color, S(2.0f)
                    );
                    // Triangle pointing right (In point)
                    draw_list->AddTriangleFilled(
                        ImVec2(loop_start_x, ruler_y),
                        ImVec2(loop_start_x + S(8), ruler_y + S(8)),
                        ImVec2(loop_start_x, ruler_y + S(8)),
                        io_marker_color
                    );
                }
            }

            // Calculate Out point position if set
            float loop_end_x = -1;
            if (timeline_view->HasTimelineOutPoint()) {
                double tl_out = timeline_view->GetTimelineOutPoint();
                loop_end_x = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH +
                            static_cast<float>(tl_out) * pixels_per_second - scroll_offset_x;

                // Draw Out point (accent color line with triangle) - if in visible area
                if (loop_end_x >= track_lane_left && loop_end_x <= ruler_right) {
                    draw_list->AddLine(
                        ImVec2(loop_end_x, ruler_y),
                        ImVec2(loop_end_x, ruler_y + OTIOTimeline::TIMELINE_RULER_HEIGHT),
                        io_marker_color, S(2.0f)
                    );
                    // Triangle pointing left (Out point)
                    draw_list->AddTriangleFilled(
                        ImVec2(loop_end_x, ruler_y),
                        ImVec2(loop_end_x - S(8), ruler_y + S(8)),
                        ImVec2(loop_end_x, ruler_y + S(8)),
                        io_marker_color
                    );
                }
            }

            // Draw filled region only when BOTH points are set and loop is enabled
            if (timeline_view->HasTimelineInOutPoints() && timeline_view->IsTimelineLooping()) {
                // Clamp to visible area
                float clamped_start = std::max(loop_start_x, track_lane_left);
                float clamped_end = std::min(loop_end_x, ruler_right);

                if (clamped_start < clamped_end) {
                    // White highlight matching dual view trim mode style
                    ImU32 loop_region_color = UI_WHITE_A(30);

                    // Draw on ruler area
                    draw_list->AddRectFilled(
                        ImVec2(clamped_start, ruler_y),
                        ImVec2(clamped_end, ruler_y + OTIOTimeline::TIMELINE_RULER_HEIGHT),
                        loop_region_color
                    );
                }
            }
        }

        // Time markers - with major/minor distinction like transport panel
        // Calculate visible time range based on scroll and pixels_per_second
        float visible_width = ruler_right - (tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH);
        float visible_start_time = scroll_offset_x / pixels_per_second;
        float visible_end_time = (scroll_offset_x + visible_width) / pixels_per_second;

        // Determine marker interval based on zoom level
        float marker_interval = 1.0f;  // 1 second intervals
        if (pixels_per_second < 20) marker_interval = 5.0f;
        if (pixels_per_second > 100) marker_interval = 0.5f;

        // Start from first marker at or before visible start, aligned to interval
        float start_t = std::floor(visible_start_time / marker_interval) * marker_interval;
        if (start_t < 0) start_t = 0;

        int marker_count = (int)(start_t / marker_interval);  // Preserve major/minor pattern

        for (float t = start_t; t <= visible_end_time + marker_interval; t += marker_interval) {
            float x = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH +
                      t * pixels_per_second - scroll_offset_x;

            if (x < tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH) {
                marker_count++;
                continue;
            }
            if (x > ruler_right) break;

            // Major tick every 5 markers, minor for others
            bool is_major = (marker_count % 5 == 0);
            float tick_height = is_major ? S(12.0f) : S(8.0f);  // Ticks from bottom
            ImU32 tick_color = is_major ? IM_COL32(160, 160, 160, 255) : IM_COL32(120, 120, 120, 255);
            float tick_width = is_major ? S(1.5f) : S(1.0f);

            // Draw tick from bottom of ruler
            float ruler_bottom = ruler_y + OTIOTimeline::TIMELINE_RULER_HEIGHT;
            draw_list->AddLine(
                ImVec2(x, ruler_bottom - tick_height),
                ImVec2(x, ruler_bottom),
                tick_color, tick_width
            );

            // Time label on major ticks only (centered vertically in ruler)
            if (is_major) {
                int secs = (int)t;
                int mins = secs / 60;
                secs = secs % 60;
                char time_str[16];
                snprintf(time_str, sizeof(time_str), "%d:%02d", mins, secs);

                if (font_regular) {
                    ImVec2 text_size = ImGui::CalcTextSize(time_str);
                    draw_list->AddText(font_regular, S(12.0f),
                        ImVec2(x - text_size.x * 0.5f, ruler_y + S(8)),
                        IM_COL32(180, 180, 180, 255), time_str);
                } else {
                    draw_list->AddText(
                        ImVec2(x - S(12), ruler_y + S(8)),
                        IM_COL32(180, 180, 180, 255), time_str);
                }
            }
            marker_count++;
        }

        // =====================================================================
        // CACHE BAR - Shows cached frames in timeline (at top of ruler, above time markers)
        // For dual view mode: shows two bars (LEFT above, RIGHT below)
        // Skip for video-only content - D3D11 streaming decoders handle buffering internally
        // =====================================================================

        // Check if we should show cache bar (only for image/EXR content)
        bool show_cache_bar = false;
        if (timeline_view && timeline_view->HasPlaybackController()) {
            auto* controller = timeline_view->GetEffectivePlaybackController();
            if (controller && controller->IsInitialized()) {
                // Check LEFT cache for image content
                qcview::TimelineCache* cache = controller->GetCache();
                if (cache && cache->HasImageContent()) {
                    show_cache_bar = true;
                }
                // Also check RIGHT cache in dual view mode
                if (!show_cache_bar) {
                    qcview::TimelineCache* right_cache = controller->GetRightCache();
                    if (right_cache && right_cache->HasImageContent()) {
                        show_cache_bar = true;
                    }
                }
            }
        }

        if (show_cache_bar) {
        const float cache_bar_height = S(4.0f);
        float cache_bar_y = ruler_y + S(1.0f);  // Top of ruler area, above time markers
        float cache_bar_left = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH;
        float cache_bar_right = tracks_start_pos.x + content_region.x;

        // Check if we're in dual view mode
        bool is_dual_view_cache = timeline_view && timeline_view->IsDualViewMode() &&
                                   scratch_timeline_controller && scratch_timeline_controller->IsDualViewMode();

        // For dual view: each bar is full height (stacked, not halved)
        float single_bar_height = cache_bar_height;

        // Helper to shift hue of a color (for RIGHT cache differentiation)
        auto ShiftHue = [](ImVec4 color, float shift) -> ImU32 {
            // Convert RGB to HSV
            float r = color.x, g = color.y, b = color.z;
            float max_c = std::max({r, g, b});
            float min_c = std::min({r, g, b});
            float h = 0, s = 0, v = max_c;

            float d = max_c - min_c;
            s = max_c == 0 ? 0 : d / max_c;

            if (max_c != min_c) {
                if (max_c == r) h = (g - b) / d + (g < b ? 6 : 0);
                else if (max_c == g) h = (b - r) / d + 2;
                else h = (r - g) / d + 4;
                h /= 6;
            }

            // Shift hue
            h = fmodf(h + shift, 1.0f);
            if (h < 0) h += 1.0f;

            // Convert HSV back to RGB
            int i = static_cast<int>(h * 6);
            float f = h * 6 - i;
            float p = v * (1 - s);
            float q = v * (1 - f * s);
            float t = v * (1 - (1 - f) * s);

            float ro, go, bo;
            switch (i % 6) {
                case 0: ro = v; go = t; bo = p; break;
                case 1: ro = q; go = v; bo = p; break;
                case 2: ro = p; go = v; bo = t; break;
                case 3: ro = p; go = q; bo = v; break;
                case 4: ro = t; go = p; bo = v; break;
                default: ro = v; go = p; bo = q; break;
            }

            return IM_COL32(
                static_cast<int>(ro * 255),
                static_cast<int>(go * 255),
                static_cast<int>(bo * 255),
                static_cast<int>(color.w * 255)  // Preserve input alpha
            );
        };

        // Cache bar background (dark gray) - double height for dual view (two stacked bars)
        float total_cache_bar_height = is_dual_view_cache ? (cache_bar_height * 2.0f) : cache_bar_height;
        draw_list->AddRectFilled(
            ImVec2(cache_bar_left, cache_bar_y),
            ImVec2(cache_bar_right, cache_bar_y + total_cache_bar_height),
            IM_COL32(40, 40, 40, 255)
        );

        // Draw cached segments with full boundary-based visualization
        // Layering order: 1) Boundary region (background), 2) Target window (outline), 3) Cached frames (filled)
        if (timeline_view && timeline_view->HasPlaybackController()) {
            auto* controller = timeline_view->GetEffectivePlaybackController();
            if (controller && controller->IsInitialized()) {
                ImVec4 accent = GetWindowsAccentColor();
                accent.w = 0.8f;  // 80% opacity for visibility
                ImU32 left_cache_color = ToImU32(accent);  // System accent color for LEFT
                ImU32 right_cache_color = ShiftHue(accent, 0.5f);  // Complementary hue for RIGHT

                // Dimmed versions for target window (what cache is trying to fill)
                ImU32 left_target_color = IM_COL32(
                    static_cast<int>(accent.x * 255 * 0.4f),
                    static_cast<int>(accent.y * 255 * 0.4f),
                    static_cast<int>(accent.z * 255 * 0.4f),
                    180
                );
                ImU32 right_target_color = ShiftHue(ImVec4(accent.x * 0.4f, accent.y * 0.4f, accent.z * 0.4f, 0.7f), 0.5f);

                // Boundary region color (subtle background tint)
                ImU32 boundary_color = IM_COL32(80, 80, 100, 60);

                // Get current playhead frame for target window calculation
                int current_frame = static_cast<int>(position * controller->GetFPS());

                // Helper lambda to render segments for a cache
                auto RenderCacheSegments = [&](qcview::TimelineCache* cache, float bar_y, float bar_height,
                                               ImU32 cache_color, ImU32 target_color) {
                    if (!cache || !cache->IsInitialized()) return;

                    // Skip progress bar for video-only content - D3D11 streaming decoders handle
                    // their own buffering internally. Progress bar only makes sense for image sequences.
                    if (cache->IsVideoOnly()) return;

                    // Layer 1: Boundary region (if custom boundaries set)
                    auto boundary_segments = cache->GetBoundarySegments();
                    for (const auto& segment : boundary_segments) {
                        float start_x = cache_bar_left + static_cast<float>(segment.start_time) * pixels_per_second - scroll_offset_x;
                        float end_x = cache_bar_left + static_cast<float>(segment.end_time) * pixels_per_second - scroll_offset_x;
                        start_x = std::max(start_x, cache_bar_left);
                        end_x = std::min(end_x, cache_bar_right);
                        if (end_x > start_x) {
                            draw_list->AddRectFilled(
                                ImVec2(start_x, bar_y),
                                ImVec2(end_x, bar_y + bar_height),
                                boundary_color
                            );
                        }
                    }

                    // Layer 2: Target window (what cache is trying to fill)
                    auto target_segments = cache->GetTargetWindowSegments(current_frame);
                    for (const auto& segment : target_segments) {
                        float start_x = cache_bar_left + static_cast<float>(segment.start_time) * pixels_per_second - scroll_offset_x;
                        float end_x = cache_bar_left + static_cast<float>(segment.end_time) * pixels_per_second - scroll_offset_x;
                        start_x = std::max(start_x, cache_bar_left);
                        end_x = std::min(end_x, cache_bar_right);
                        if (end_x > start_x) {
                            draw_list->AddRectFilled(
                                ImVec2(start_x, bar_y),
                                ImVec2(end_x, bar_y + bar_height),
                                target_color
                            );
                        }
                    }

                    // Layer 3: Actually cached/buffered frames (solid fill)
                    // Only image sequences reach here (video-only returns early above)
                    auto cached_segments = cache->GetCacheSegments();
                    for (const auto& segment : cached_segments) {
                        float start_x = cache_bar_left + static_cast<float>(segment.start_time) * pixels_per_second - scroll_offset_x;
                        float end_x = cache_bar_left + static_cast<float>(segment.end_time) * pixels_per_second - scroll_offset_x;
                        start_x = std::max(start_x, cache_bar_left);
                        end_x = std::min(end_x, cache_bar_right);
                        if (end_x > start_x) {
                            draw_list->AddRectFilled(
                                ImVec2(start_x, bar_y),
                                ImVec2(end_x, bar_y + bar_height),
                                cache_color
                            );
                        }
                    }
                };

                // LEFT cache (or single cache in non-dual mode)
                qcview::TimelineCache* left_cache = controller->GetCache();
                RenderCacheSegments(left_cache, cache_bar_y, single_bar_height, left_cache_color, left_target_color);

                // RIGHT cache (dual view mode only)
                if (is_dual_view_cache) {
                    qcview::TimelineCache* right_cache = controller->GetRightCache();
                    float right_bar_y = cache_bar_y + single_bar_height;
                    RenderCacheSegments(right_cache, right_bar_y, single_bar_height, right_cache_color, right_target_color);
                }
            }
        }
        }  // end if (show_cache_bar)

        // =====================================================================
        // ANNOTATION MARKERS - Triangle at ruler with dotted line through tracks
        // =====================================================================
        if (annotation_manager && annotation_manager->GetNoteCount() > 0) {
            const auto& notes = annotation_manager->GetNotes();
            ImVec4 accent = GetWindowsAccentColor();
            ImU32 marker_color = ImGui::GetColorU32(accent);  // Use regular accent color

            float marker_triangle_height = S(8.0f);
            float marker_triangle_width = S(8.0f);
            float track_lane_left = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH;
            float track_lane_right = tracks_start_pos.x + content_region.x;

            // Calculate line extent - from bottom of triangle to bottom of ruler
            float line_top = ruler_y + marker_triangle_height;
            float line_bottom = ruler_y + OTIOTimeline::TIMELINE_RULER_HEIGHT;

            for (const auto& note : notes) {
                // Calculate X position from timestamp
                float marker_x = track_lane_left + (float)note.timestamp_seconds * pixels_per_second - scroll_offset_x;

                // Skip if outside visible area
                if (marker_x < track_lane_left - S(10.0f) || marker_x > track_lane_right + S(10.0f)) continue;

                // Draw downward-pointing triangle at top of ruler
                draw_list->AddTriangleFilled(
                    ImVec2(marker_x - marker_triangle_width / 2, ruler_y),  // Left point
                    ImVec2(marker_x + marker_triangle_width / 2, ruler_y),  // Right point
                    ImVec2(marker_x, ruler_y + marker_triangle_height),     // Bottom point
                    marker_color
                );

                // Draw dotted line from triangle to bottom of ruler
                float dash_length = S(3.0f);
                float gap_length = S(3.0f);
                float y = line_top;
                while (y < line_bottom) {
                    float dash_end = std::min(y + dash_length, line_bottom);
                    draw_list->AddLine(
                        ImVec2(marker_x, y),
                        ImVec2(marker_x, dash_end),
                        marker_color, S(1.5f)
                    );
                    y += dash_length + gap_length;
                }
            }
        }

        // =====================================================================
        // PLAYHEAD - Vertical line through all tracks
        // =====================================================================
        float playhead_x = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH +
                          (float)position * pixels_per_second - scroll_offset_x;

        if (playhead_x >= tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH &&
            playhead_x <= tracks_start_pos.x + content_region.x) {

            // Playhead line
            draw_list->AddLine(
                ImVec2(playhead_x, tracks_start_pos.y),
                ImVec2(playhead_x, ruler_y + OTIOTimeline::TIMELINE_RULER_HEIGHT),
                IM_COL32(255, 80, 80, 255), S(2.0f)
            );

            // Playhead triangle
            ImVec2 tri[3] = {
                ImVec2(playhead_x - S(6), ruler_y),
                ImVec2(playhead_x + S(6), ruler_y),
                ImVec2(playhead_x, ruler_y + S(8))
            };
            draw_list->AddTriangleFilled(tri[0], tri[1], tri[2], IM_COL32(255, 80, 80, 255));
        }

        // =====================================================================
        // GHOST/PREVIEW RENDERING - Visual feedback during drag/trim
        // =====================================================================
        ImVec2 preview_mouse = ImGui::GetMousePos();
        float preview_relative_x = preview_mouse.x - (tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH);
        double preview_mouse_time = (preview_relative_x + scroll_offset_x) / pixels_per_second;

        // Calculate which track the mouse is over for preview
        // Subtract overview_offset because tracks_start_pos.y includes the overview track area
        float preview_relative_y = preview_mouse.y - tracks_start_pos.y - overview_offset;
        int preview_track_index = -1;
        float preview_track_y = 0.0f;
        if (preview_relative_y >= 0 && preview_relative_y < tracks_height && tracks_ptr) {
            float y_accum = 0.0f;
            for (int ti = 0; ti < num_tracks; ++ti) {
                if (ti > 0 && !(*tracks_ptr)[ti].is_video && (*tracks_ptr)[ti-1].is_video) {
                    y_accum += OTIOTimeline::TRACK_SEPARATOR_HEIGHT;
                }
                if (preview_relative_y >= y_accum && preview_relative_y < y_accum + OTIOTimeline::TRACK_LANE_HEIGHT) {
                    preview_track_index = ti;
                    preview_track_y = tracks_start_pos.y + overview_offset + y_accum;
                    break;
                }
                y_accum += OTIOTimeline::TRACK_LANE_HEIGHT;
            }
        }

        // Ghost rectangle during clip drag
        if (timeline_clip_drag.active && timeline_view) {
            // Find the clip being dragged to get its duration
            qcview::OTIOClip* drag_clip = timeline_view->FindClipById(timeline_clip_drag.clip_id);
            if (drag_clip) {
                // Calculate ghost position with snapping
                double proposed_start = preview_mouse_time - timeline_clip_drag.drag_offset;

                // Collect snap points (exclude dragged clip)
                std::set<std::string> exclude_ids;
                exclude_ids.insert(timeline_clip_drag.clip_id);
                // Also exclude any other clips in multi-selection
                for (const auto& dc : timeline_clip_drag.dragged_clips) {
                    exclude_ids.insert(dc.clip_id);
                }

                double playhead_time = timeline_manager ? timeline_manager->GetUIPosition() : 0.0;
                auto snap_points = CollectSnapPoints(
                    *tracks_ptr,
                    exclude_ids,
                    playhead_time,
                    timeline_view->GetDuration()
                );

                // Apply snapping
                double ghost_start = CalculateSnappedPosition(
                    proposed_start,
                    drag_clip->duration,
                    snap_points,
                    pixels_per_second,
                    snap_active,
                    snap_time
                );

                float ghost_duration = static_cast<float>(drag_clip->duration);

                // Determine which track to show ghost on
                int target_track = (preview_track_index >= 0) ? preview_track_index : timeline_clip_drag.original_track_index;

                // Calculate ghost track Y position (add overview_offset to skip overview track)
                float ghost_track_y = tracks_start_pos.y + overview_offset;
                if (tracks_ptr) {
                    float y_accum = 0.0f;
                    for (int ti = 0; ti < target_track && ti < num_tracks; ++ti) {
                        if (ti > 0 && !(*tracks_ptr)[ti].is_video && (*tracks_ptr)[ti-1].is_video) {
                            y_accum += OTIOTimeline::TRACK_SEPARATOR_HEIGHT;
                        }
                        y_accum += OTIOTimeline::TRACK_LANE_HEIGHT;
                    }
                    // Check for separator before target track
                    if (target_track > 0 && !(*tracks_ptr)[target_track].is_video && (*tracks_ptr)[target_track-1].is_video) {
                        y_accum += OTIOTimeline::TRACK_SEPARATOR_HEIGHT;
                    }
                    ghost_track_y = tracks_start_pos.y + overview_offset + y_accum;
                }

                // Calculate ghost screen coordinates
                float ghost_x = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH +
                               static_cast<float>(ghost_start) * pixels_per_second - scroll_offset_x;
                float ghost_w = ghost_duration * pixels_per_second;
                float ghost_top = ghost_track_y + S(2);
                float ghost_bottom = ghost_track_y + OTIOTimeline::TRACK_LANE_HEIGHT - S(2);

                // Clamp to visible area
                float visible_left = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH;
                float visible_right = tracks_start_pos.x + content_region.x;
                float render_left = std::max(ghost_x, visible_left);
                float render_right = std::min(ghost_x + ghost_w, visible_right);

                if (render_right > render_left) {
                    // Get accent color for ghost
                    ImVec4 accent = GetWindowsAccentColor();
                    ImU32 ghost_fill = IM_COL32(
                        (int)(accent.x * 255),
                        (int)(accent.y * 255),
                        (int)(accent.z * 255), 80);  // Semi-transparent
                    ImU32 ghost_border = IM_COL32(
                        (int)(accent.x * 255),
                        (int)(accent.y * 255),
                        (int)(accent.z * 255), 200);

                    // Draw ghost rectangle
                    draw_list->AddRectFilled(
                        ImVec2(render_left, ghost_top),
                        ImVec2(render_right, ghost_bottom),
                        ghost_fill, S(2.0f));
                    draw_list->AddRect(
                        ImVec2(render_left, ghost_top),
                        ImVec2(render_right, ghost_bottom),
                        ghost_border, S(2.0f), 0, S(2.0f));

                    // Draw time indicator tooltip
                    char time_buf[32];
                    int mins = (int)(ghost_start / 60.0);
                    int secs = (int)ghost_start % 60;
                    int frames = (int)((ghost_start - (int)ghost_start) * 24.0);
                    snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", mins, secs, frames);

                    ImVec2 text_size = ImGui::CalcTextSize(time_buf);
                    float tooltip_x = render_left + (render_right - render_left - text_size.x) * 0.5f;
                    float tooltip_y = ghost_top - text_size.y - S(4);

                    // Background for time indicator
                    draw_list->AddRectFilled(
                        ImVec2(tooltip_x - S(4), tooltip_y - S(2)),
                        ImVec2(tooltip_x + text_size.x + S(4), tooltip_y + text_size.y + S(2)),
                        IM_COL32(0, 0, 0, 200), S(3.0f));
                    draw_list->AddText(ImVec2(tooltip_x, tooltip_y), UI_WHITE, time_buf);

                    // =============================================================
                    // MULTI-CLIP GHOSTS - Draw ghost for each additional selected clip
                    // =============================================================
                    if (timeline_clip_drag.dragged_clips.size() > 1) {
                        // Check for overlap with stationary clips
                        bool would_overlap = WouldCauseOverlap(*tracks_ptr, timeline_clip_drag.dragged_clips, ghost_start);

                        // Choose ghost color based on overlap status
                        ImU32 multi_ghost_fill = would_overlap ?
                            IM_COL32(255, 80, 80, 60) :  // Red if overlap
                            ghost_fill;
                        ImU32 multi_ghost_border = would_overlap ?
                            IM_COL32(255, 80, 80, 180) :
                            ghost_border;

                        for (const auto& dc : timeline_clip_drag.dragged_clips) {
                            // Skip the primary clip (already drawn above)
                            if (dc.clip_id == timeline_clip_drag.clip_id) continue;

                            // Calculate this clip's ghost position
                            double clip_ghost_start = ghost_start + dc.offset_from_primary;
                            if (clip_ghost_start < 0) clip_ghost_start = 0;

                            // Calculate track Y for this clip (add overview_offset to skip overview track)
                            float clip_ghost_y = tracks_start_pos.y + overview_offset;
                            if (tracks_ptr && dc.track_index >= 0 && dc.track_index < num_tracks) {
                                float y_accum = 0.0f;
                                for (int ti = 0; ti < dc.track_index; ++ti) {
                                    if (ti > 0 && !(*tracks_ptr)[ti].is_video && (*tracks_ptr)[ti-1].is_video) {
                                        y_accum += OTIOTimeline::TRACK_SEPARATOR_HEIGHT;
                                    }
                                    y_accum += OTIOTimeline::TRACK_LANE_HEIGHT;
                                }
                                if (dc.track_index > 0 && !(*tracks_ptr)[dc.track_index].is_video && (*tracks_ptr)[dc.track_index-1].is_video) {
                                    y_accum += OTIOTimeline::TRACK_SEPARATOR_HEIGHT;
                                }
                                clip_ghost_y = tracks_start_pos.y + overview_offset + y_accum;
                            }

                            // Calculate screen coordinates
                            float cg_x = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH +
                                        static_cast<float>(clip_ghost_start) * pixels_per_second - scroll_offset_x;
                            float cg_w = static_cast<float>(dc.duration) * pixels_per_second;
                            float cg_top = clip_ghost_y + S(2);
                            float cg_bottom = clip_ghost_y + OTIOTimeline::TRACK_LANE_HEIGHT - S(2);

                            float cg_left = std::max(cg_x, visible_left);
                            float cg_right = std::min(cg_x + cg_w, visible_right);

                            if (cg_right > cg_left) {
                                draw_list->AddRectFilled(
                                    ImVec2(cg_left, cg_top),
                                    ImVec2(cg_right, cg_bottom),
                                    multi_ghost_fill, S(2.0f));
                                draw_list->AddRect(
                                    ImVec2(cg_left, cg_top),
                                    ImVec2(cg_right, cg_bottom),
                                    multi_ghost_border, S(2.0f), 0, S(2.0f));
                            }
                        }
                    }

                    // =============================================================
                    // DRAG PREVIEW THUMBNAIL
                    // Show the source frame at the playhead position within clip
                    // =============================================================
                    if (drag_clip->is_linked && !drag_clip->linked_path.empty()) {
                        auto* playback_ctrl = timeline_view->GetEffectivePlaybackController();
                        if (playback_ctrl && playback_ctrl->IsInitialized()) {
                            auto* cache = playback_ctrl->GetCache();
                            if (cache && timeline_manager) {
                                // Get current playhead position
                                double playhead_time = timeline_manager->GetUIPosition();

                                // Check if playhead would be within the dragged clip at new position
                                double clip_end = ghost_start + drag_clip->duration;
                                if (playhead_time >= ghost_start && playhead_time < clip_end) {
                                    // Calculate the source frame at playhead
                                    double source_fps = (drag_clip->source_fps > 0) ? drag_clip->source_fps : 24.0;
                                    double offset_in_clip = playhead_time - ghost_start;

                                    // Handle Avid MXF subclips: source_in may contain absolute tape timecode
                                    // If source_in exceeds the file's actual duration, it's a tape timecode
                                    // and the MXF file itself starts at frame 0
                                    double source_time;
                                    bool source_in_is_tape_timecode = false;
                                    if (drag_clip->source_duration > 0) {
                                        source_in_is_tape_timecode = (drag_clip->source_in > drag_clip->source_duration);
                                    } else {
                                        // Fallback heuristic: source_in > 1 hour likely means tape timecode
                                        source_in_is_tape_timecode = (drag_clip->source_in > 3600.0);
                                    }

                                    if (source_in_is_tape_timecode) {
                                        source_time = offset_in_clip;  // MXF starts at frame 0
                                    } else {
                                        source_time = drag_clip->source_in + offset_in_clip;
                                    }

                                    int preview_source_frame = static_cast<int>(source_time * source_fps);

                                    // Request and display the preview frame using dedicated thumbnail cache
                                    int frame_width = 0, frame_height = 0;
                                    GLuint preview_texture = 0;
                                    if (timeline_thumbnail_cache) {
                                        preview_texture = timeline_thumbnail_cache->GetThumbnail(
                                            drag_clip->linked_path, preview_source_frame,
                                            frame_width, frame_height);
                                    }

                                    if (preview_texture != 0 && frame_width > 0 && frame_height > 0) {
                                        // Fixed size matching dual view style (200x120)
                                        const float container_width = S(200.0f);
                                        const float container_height = S(120.0f);
                                        float aspect = static_cast<float>(frame_width) / frame_height;

                                        // Calculate display size maintaining aspect ratio
                                        float thumb_w, thumb_h;
                                        if (aspect > (container_width / container_height)) {
                                            thumb_w = container_width;
                                            thumb_h = thumb_w / aspect;
                                        } else {
                                            thumb_h = container_height;
                                            thumb_w = thumb_h * aspect;
                                        }

                                        // Prepare frame label
                                        char frame_label[64];
                                        double tl_fps = timeline_view->GetFrameRate();
                                        double frame_time = preview_source_frame / source_fps;
                                        int secs = static_cast<int>(frame_time);
                                        int mins = secs / 60;
                                        int sec_rem = secs % 60;
                                        int frame_in_sec = static_cast<int>((frame_time - secs) * tl_fps);
                                        snprintf(frame_label, sizeof(frame_label), "@ Playhead: Frame %d - %02d:%02d:%02d",
                                                 preview_source_frame, mins, sec_rem, frame_in_sec);
                                        ImVec2 label_size = ImGui::CalcTextSize(frame_label);

                                        // Calculate total tooltip size (matching dual view style)
                                        const float padding = S(4.0f);
                                        const float text_height = label_size.y + S(4.0f);
                                        const float tooltip_width = container_width + padding * 2;
                                        const float tooltip_height = container_height + text_height + padding * 2;

                                        // Position tooltip ABOVE mouse, centered (matching dual view style)
                                        ImVec2 mouse_pos = ImGui::GetMousePos();
                                        float tooltip_x = mouse_pos.x - tooltip_width * 0.5f;
                                        float tooltip_y = mouse_pos.y - tooltip_height - S(30.0f);

                                        // Keep within main viewport (window bounds, not screen bounds)
                                        ImGuiViewport* viewport = ImGui::GetMainViewport();
                                        float vp_min_x = viewport->Pos.x;
                                        float vp_max_x = viewport->Pos.x + viewport->Size.x;
                                        float vp_min_y = viewport->Pos.y;
                                        tooltip_x = std::max(vp_min_x + S(10.0f), std::min(tooltip_x, vp_max_x - tooltip_width - S(10.0f)));
                                        tooltip_y = std::max(vp_min_y + S(10.0f), tooltip_y);

                                        // Use foreground draw list
                                        ImDrawList* fg_draw = ImGui::GetForegroundDrawList();

                                        // Get accent color for border (matching dual view style)
                                        ImVec4 accent = GetWindowsAccentColor();
                                        ImU32 border_color = IM_COL32(
                                            (int)(accent.x * 180), (int)(accent.y * 180), (int)(accent.z * 180), 255);

                                        // Draw shadow (matching dual view style)
                                        fg_draw->AddRectFilled(
                                            ImVec2(tooltip_x + S(4), tooltip_y + S(4)),
                                            ImVec2(tooltip_x + tooltip_width + S(4), tooltip_y + tooltip_height + S(4)),
                                            IM_COL32(0, 0, 0, 100), S(6.0f));

                                        // Draw tooltip background (matching dual view style)
                                        fg_draw->AddRectFilled(
                                            ImVec2(tooltip_x, tooltip_y),
                                            ImVec2(tooltip_x + tooltip_width, tooltip_y + tooltip_height),
                                            IM_COL32(30, 30, 30, 240), S(6.0f));

                                        // Draw colored border (matching dual view style)
                                        fg_draw->AddRect(
                                            ImVec2(tooltip_x, tooltip_y),
                                            ImVec2(tooltip_x + tooltip_width, tooltip_y + tooltip_height),
                                            border_color, S(6.0f), 0, S(2.0f));

                                        // Draw thumbnail (centered within container)
                                        float img_x = tooltip_x + padding + (container_width - thumb_w) * 0.5f;
                                        float img_y = tooltip_y + padding + (container_height - thumb_h) * 0.5f;
                                        fg_draw->AddImage(
                                            ThumbnailPoolIDToImTexture(preview_texture),
                                            ImVec2(img_x, img_y),
                                            ImVec2(img_x + thumb_w, img_y + thumb_h));

                                        // Draw frame label centered below thumbnail
                                        float label_x = tooltip_x + (tooltip_width - label_size.x) * 0.5f;
                                        float label_y = tooltip_y + padding + container_height + S(4.0f);
                                        fg_draw->AddText(ImVec2(label_x, label_y),
                                            UI_WHITE, frame_label);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Draw snap indicator line when snapping is active
            if (snap_active) {
                float snap_x = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH +
                               static_cast<float>(snap_time * pixels_per_second - scroll_offset_x);
                float visible_left = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH;
                float visible_right = tracks_start_pos.x + content_region.x;

                if (snap_x >= visible_left && snap_x <= visible_right) {
                    ImU32 snap_color = IM_COL32(255, 200, 0, 220);  // Yellow/gold
                    draw_list->AddLine(
                        ImVec2(snap_x, tracks_start_pos.y),
                        ImVec2(snap_x, tracks_start_pos.y + tracks_height),
                        snap_color, S(2.0f)
                    );
                }
            }
        } else {
            // Reset snap state when not dragging
            snap_active = false;
        }

        // Ghost preview for media drop from project panel
        if (timeline_media_drop.active && timeline_media_drop.preview_duration > 0 && timeline_media_drop.target_track_index >= 0) {
            ImVec4 accent = GetWindowsAccentColor();

            // Calculate ghost track Y position (add overview_offset to skip overview track)
            float ghost_track_y = tracks_start_pos.y + overview_offset;
            if (tracks_ptr) {
                float y_accum = 0.0f;
                for (int ti = 0; ti < timeline_media_drop.target_track_index && ti < num_tracks; ++ti) {
                    if (ti > 0 && !(*tracks_ptr)[ti].is_video && (*tracks_ptr)[ti-1].is_video) {
                        y_accum += OTIOTimeline::TRACK_SEPARATOR_HEIGHT;
                    }
                    y_accum += OTIOTimeline::TRACK_LANE_HEIGHT;
                }
                // Check for separator before target track
                if (timeline_media_drop.target_track_index > 0 &&
                    timeline_media_drop.target_track_index < num_tracks &&
                    !(*tracks_ptr)[timeline_media_drop.target_track_index].is_video &&
                    (*tracks_ptr)[timeline_media_drop.target_track_index-1].is_video) {
                    y_accum += OTIOTimeline::TRACK_SEPARATOR_HEIGHT;
                }
                ghost_track_y = tracks_start_pos.y + overview_offset + y_accum;
            }

            // Calculate ghost screen coordinates
            float ghost_x = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH +
                           static_cast<float>(timeline_media_drop.drop_time) * pixels_per_second - scroll_offset_x;
            float ghost_w = static_cast<float>(timeline_media_drop.preview_duration) * pixels_per_second;
            float ghost_top = ghost_track_y + S(2);
            float ghost_bottom = ghost_track_y + OTIOTimeline::TRACK_LANE_HEIGHT - S(2);

            // Clamp to visible area
            float visible_left = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH;
            float visible_right = tracks_start_pos.x + content_region.x;
            float render_left = std::max(ghost_x, visible_left);
            float render_right = std::min(ghost_x + ghost_w, visible_right);

            if (render_right > render_left) {
                // Ghost colors (same style as clip drag)
                ImU32 ghost_fill = IM_COL32(
                    (int)(accent.x * 255),
                    (int)(accent.y * 255),
                    (int)(accent.z * 255), 80);
                ImU32 ghost_border = IM_COL32(
                    (int)(accent.x * 255),
                    (int)(accent.y * 255),
                    (int)(accent.z * 255), 200);

                // Draw ghost rectangle
                draw_list->AddRectFilled(
                    ImVec2(render_left, ghost_top),
                    ImVec2(render_right, ghost_bottom),
                    ghost_fill, S(2.0f));
                draw_list->AddRect(
                    ImVec2(render_left, ghost_top),
                    ImVec2(render_right, ghost_bottom),
                    ghost_border, S(2.0f), 0, S(2.0f));

                // Draw clip name in ghost
                if (!timeline_media_drop.preview_name.empty()) {
                    ImVec2 text_pos(render_left + S(6), ghost_top + S(4));
                    draw_list->AddText(text_pos, UI_WHITE_A(200),
                                      timeline_media_drop.preview_name.c_str());
                }

                // Draw time indicator tooltip (same style as clip drag)
                char time_buf[32];
                int mins = (int)(timeline_media_drop.drop_time / 60.0);
                int secs = (int)timeline_media_drop.drop_time % 60;
                int frames = (int)((timeline_media_drop.drop_time - (int)timeline_media_drop.drop_time) * 24.0);
                snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", mins, secs, frames);

                ImVec2 text_size = ImGui::CalcTextSize(time_buf);
                float tooltip_x = render_left + (render_right - render_left - text_size.x) * 0.5f;
                float tooltip_y = ghost_top - text_size.y - S(4);

                // Background for time indicator
                draw_list->AddRectFilled(
                    ImVec2(tooltip_x - S(4), tooltip_y - S(2)),
                    ImVec2(tooltip_x + text_size.x + S(4), tooltip_y + text_size.y + S(2)),
                    IM_COL32(0, 0, 0, 200), S(3.0f));
                draw_list->AddText(ImVec2(tooltip_x, tooltip_y), UI_WHITE, time_buf);
            }
        }

        // Ghost edges during trim
        if (timeline_trim_state.active && timeline_view) {
            qcview::OTIOClip* trim_clip = timeline_view->FindClipById(timeline_trim_state.clip_id);
            if (trim_clip) {
                // Calculate original clip bounds
                float orig_clip_x = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH +
                                   static_cast<float>(trim_clip->start_time) * pixels_per_second - scroll_offset_x;
                float orig_clip_w = static_cast<float>(trim_clip->duration) * pixels_per_second;

                // Calculate track Y for this clip (add overview_offset to skip overview track)
                float trim_track_y = tracks_start_pos.y + overview_offset;
                if (tracks_ptr) {
                    float y_accum = 0.0f;
                    for (int ti = 0; ti < timeline_trim_state.track_index && ti < num_tracks; ++ti) {
                        if (ti > 0 && !(*tracks_ptr)[ti].is_video && (*tracks_ptr)[ti-1].is_video) {
                            y_accum += OTIOTimeline::TRACK_SEPARATOR_HEIGHT;
                        }
                        y_accum += OTIOTimeline::TRACK_LANE_HEIGHT;
                    }
                    if (timeline_trim_state.track_index > 0 &&
                        !(*tracks_ptr)[timeline_trim_state.track_index].is_video &&
                        (*tracks_ptr)[timeline_trim_state.track_index-1].is_video) {
                        y_accum += OTIOTimeline::TRACK_SEPARATOR_HEIGHT;
                    }
                    trim_track_y = tracks_start_pos.y + overview_offset + y_accum;
                }

                float clip_top = trim_track_y + S(2);
                float clip_bottom = trim_track_y + OTIOTimeline::TRACK_LANE_HEIGHT - S(2);
                float visible_left = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH;
                float visible_right = tracks_start_pos.x + content_region.x;

                // Calculate new edge position based on mouse
                double new_edge_time = std::max(0.0, preview_mouse_time);
                float new_edge_x = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH +
                                  static_cast<float>(new_edge_time) * pixels_per_second - scroll_offset_x;

                ImVec4 accent = GetWindowsAccentColor();
                ImU32 trim_color = IM_COL32(
                    (int)(accent.x * 255),
                    (int)(accent.y * 255),
                    (int)(accent.z * 255), 255);

                if (timeline_trim_state.is_left_edge) {
                    // Trimming left edge - show new start position
                    float new_start_x = new_edge_x;
                    float orig_end_x = orig_clip_x + orig_clip_w;

                    // Clamp within valid range
                    new_start_x = std::min(new_start_x, orig_end_x - S(10));  // Min 10px clip width
                    new_start_x = std::max(new_start_x, visible_left);

                    // Draw preview edge line
                    if (new_start_x >= visible_left && new_start_x <= visible_right) {
                        draw_list->AddLine(
                            ImVec2(new_start_x, clip_top - S(4)),
                            ImVec2(new_start_x, clip_bottom + S(4)),
                            trim_color, S(3.0f));

                        // Draw filled area showing trim region
                        float trim_right = std::min(orig_clip_x, visible_right);
                        if (new_start_x < trim_right) {
                            // Trimming inward - show what's being trimmed off
                            draw_list->AddRectFilled(
                                ImVec2(new_start_x, clip_top),
                                ImVec2(trim_right, clip_bottom),
                                IM_COL32(255, 100, 100, 60), S(2.0f));
                        } else if (new_start_x > orig_clip_x) {
                            // Show the area that will be removed
                            draw_list->AddRectFilled(
                                ImVec2(std::max(orig_clip_x, visible_left), clip_top),
                                ImVec2(new_start_x, clip_bottom),
                                IM_COL32(255, 50, 50, 100), S(2.0f));
                        }
                    }

                    // Time indicator
                    char time_buf[32];
                    int mins = (int)(new_edge_time / 60.0);
                    int secs = (int)new_edge_time % 60;
                    int frames = (int)((new_edge_time - (int)new_edge_time) * 24.0);
                    snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", mins, secs, frames);

                    ImVec2 text_size = ImGui::CalcTextSize(time_buf);
                    float tooltip_x = new_start_x - text_size.x * 0.5f;
                    float tooltip_y = clip_top - text_size.y - S(6);

                    draw_list->AddRectFilled(
                        ImVec2(tooltip_x - S(4), tooltip_y - S(2)),
                        ImVec2(tooltip_x + text_size.x + S(4), tooltip_y + text_size.y + S(2)),
                        IM_COL32(0, 0, 0, 200), S(3.0f));
                    draw_list->AddText(ImVec2(tooltip_x, tooltip_y), UI_WHITE, time_buf);

                } else {
                    // Trimming right edge - show new end position
                    float new_end_x = new_edge_x;

                    // Clamp within valid range
                    new_end_x = std::max(new_end_x, orig_clip_x + S(10));  // Min 10px clip width
                    new_end_x = std::min(new_end_x, visible_right);

                    // Draw preview edge line
                    if (new_end_x >= visible_left && new_end_x <= visible_right) {
                        draw_list->AddLine(
                            ImVec2(new_end_x, clip_top - S(4)),
                            ImVec2(new_end_x, clip_bottom + S(4)),
                            trim_color, S(3.0f));

                        // Show the area being trimmed
                        float orig_end_x = orig_clip_x + orig_clip_w;
                        if (new_end_x < orig_end_x) {
                            // Trimming inward - show what's being removed
                            draw_list->AddRectFilled(
                                ImVec2(new_end_x, clip_top),
                                ImVec2(std::min(orig_end_x, visible_right), clip_bottom),
                                IM_COL32(255, 50, 50, 100), S(2.0f));
                        } else if (new_end_x > orig_end_x) {
                            // Extending - show new area
                            draw_list->AddRectFilled(
                                ImVec2(std::max(orig_end_x, visible_left), clip_top),
                                ImVec2(new_end_x, clip_bottom),
                                IM_COL32(100, 255, 100, 60), S(2.0f));
                        }
                    }

                    // Time indicator
                    char time_buf[32];
                    int mins = (int)(new_edge_time / 60.0);
                    int secs = (int)new_edge_time % 60;
                    int frames = (int)((new_edge_time - (int)new_edge_time) * 24.0);
                    snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", mins, secs, frames);

                    ImVec2 text_size = ImGui::CalcTextSize(time_buf);
                    float tooltip_x = new_end_x - text_size.x * 0.5f;
                    float tooltip_y = clip_top - text_size.y - S(6);

                    draw_list->AddRectFilled(
                        ImVec2(tooltip_x - S(4), tooltip_y - S(2)),
                        ImVec2(tooltip_x + text_size.x + S(4), tooltip_y + text_size.y + S(2)),
                        IM_COL32(0, 0, 0, 200), S(3.0f));
                    draw_list->AddText(ImVec2(tooltip_x, tooltip_y), UI_WHITE, time_buf);
                }

                // =============================================================
                // TRIM PREVIEW THUMBNAIL
                // Show the source frame that will be at the trim point
                // =============================================================
                if (trim_clip->is_linked && !trim_clip->linked_path.empty()) {
                    auto* playback_ctrl = timeline_view->GetEffectivePlaybackController();
                    if (playback_ctrl && playback_ctrl->IsInitialized()) {
                        auto* cache = playback_ctrl->GetCache();
                        if (cache) {
                            // Calculate the source frame at the trim point
                            double source_fps = (trim_clip->source_fps > 0) ? trim_clip->source_fps : 24.0;
                            int preview_source_frame = 0;

                            // Handle Avid MXF subclips: source_in may contain absolute tape timecode
                            bool source_in_is_tape_timecode = false;
                            if (trim_clip->source_duration > 0) {
                                source_in_is_tape_timecode = (trim_clip->source_in > trim_clip->source_duration);
                            } else {
                                source_in_is_tape_timecode = (trim_clip->source_in > 3600.0);
                            }

                            if (timeline_trim_state.is_left_edge) {
                                // Left edge trim: show frame at new IN point
                                double delta_time = new_edge_time - timeline_trim_state.original_value;

                                if (source_in_is_tape_timecode) {
                                    // Tape timecode - calculate relative to clip offset
                                    double original_offset = 0.0;  // MXF starts at 0
                                    double new_offset = original_offset + delta_time;
                                    new_offset = std::max(0.0, new_offset);
                                    preview_source_frame = static_cast<int>(new_offset * source_fps);
                                } else {
                                    double new_source_in = timeline_trim_state.original_source_in + delta_time;
                                    new_source_in = std::max(0.0, new_source_in);
                                    preview_source_frame = static_cast<int>(new_source_in * source_fps);
                                }
                            } else {
                                // Right edge trim: show frame at new OUT point
                                double new_duration = new_edge_time - trim_clip->start_time;
                                new_duration = std::max(0.0, new_duration);

                                if (source_in_is_tape_timecode) {
                                    // Tape timecode - duration directly gives us the frame
                                    preview_source_frame = static_cast<int>(new_duration * source_fps);
                                } else {
                                    double source_out_time = trim_clip->source_in + new_duration;
                                    preview_source_frame = static_cast<int>(source_out_time * source_fps);
                                }
                                // Show frame just before the OUT point
                                if (preview_source_frame > 0) preview_source_frame--;
                            }

                            // Request and display the preview frame using dedicated thumbnail cache
                            int frame_width = 0, frame_height = 0;
                            GLuint preview_texture = 0;
                            if (timeline_thumbnail_cache) {
                                preview_texture = timeline_thumbnail_cache->GetThumbnail(
                                    trim_clip->linked_path, preview_source_frame,
                                    frame_width, frame_height);
                            }

                            if (preview_texture != 0 && frame_width > 0 && frame_height > 0) {
                                // Match existing thumbnail tooltip sizing
                                // Fixed size matching dual view style (200x120)
                                const float container_width = S(200.0f);
                                const float container_height = S(120.0f);
                                float aspect = static_cast<float>(frame_width) / frame_height;

                                // Calculate display size maintaining aspect ratio
                                float thumb_w, thumb_h;
                                if (aspect > (container_width / container_height)) {
                                    thumb_w = container_width;
                                    thumb_h = thumb_w / aspect;
                                } else {
                                    thumb_h = container_height;
                                    thumb_w = thumb_h * aspect;
                                }

                                // Prepare frame label text
                                char frame_label[64];
                                double source_fps = (trim_clip->source_fps > 0) ? trim_clip->source_fps : timeline_view->GetFrameRate();
                                double frame_time = preview_source_frame / source_fps;
                                int secs = static_cast<int>(frame_time);
                                int mins = secs / 60;
                                int sec_rem = secs % 60;
                                int frame_in_sec = static_cast<int>((frame_time - secs) * source_fps);
                                snprintf(frame_label, sizeof(frame_label), "Frame %d - %02d:%02d:%02d",
                                         preview_source_frame, mins, sec_rem, frame_in_sec);
                                ImVec2 label_size = ImGui::CalcTextSize(frame_label);

                                // Calculate total tooltip size (matching dual view style)
                                const float padding = S(4.0f);
                                const float text_height = label_size.y + S(4.0f);
                                const float tooltip_width = container_width + padding * 2;
                                const float tooltip_height = container_height + text_height + padding * 2;

                                // Position tooltip ABOVE mouse, centered (matching dual view style)
                                ImVec2 mouse_pos = ImGui::GetMousePos();
                                float tooltip_x = mouse_pos.x - tooltip_width * 0.5f;
                                float tooltip_y = mouse_pos.y - tooltip_height - S(30.0f);

                                // Keep within main viewport (window bounds, not screen bounds)
                                ImGuiViewport* viewport = ImGui::GetMainViewport();
                                float vp_min_x = viewport->Pos.x;
                                float vp_max_x = viewport->Pos.x + viewport->Size.x;
                                float vp_min_y = viewport->Pos.y;
                                tooltip_x = std::max(vp_min_x + S(10.0f), std::min(tooltip_x, vp_max_x - tooltip_width - S(10.0f)));
                                tooltip_y = std::max(vp_min_y + S(10.0f), tooltip_y);

                                // Use foreground draw list so it renders on top of everything
                                ImDrawList* fg_draw = ImGui::GetForegroundDrawList();

                                // Get accent color for border (matching dual view style)
                                ImVec4 accent = GetWindowsAccentColor();
                                ImU32 border_color = IM_COL32(
                                    (int)(accent.x * 180), (int)(accent.y * 180), (int)(accent.z * 180), 255);

                                // Draw shadow (matching dual view style)
                                fg_draw->AddRectFilled(
                                    ImVec2(tooltip_x + S(4), tooltip_y + S(4)),
                                    ImVec2(tooltip_x + tooltip_width + S(4), tooltip_y + tooltip_height + S(4)),
                                    IM_COL32(0, 0, 0, 100), S(6.0f));

                                // Draw tooltip background (matching dual view style)
                                fg_draw->AddRectFilled(
                                    ImVec2(tooltip_x, tooltip_y),
                                    ImVec2(tooltip_x + tooltip_width, tooltip_y + tooltip_height),
                                    IM_COL32(30, 30, 30, 240), S(6.0f));

                                // Draw colored border (matching dual view style)
                                fg_draw->AddRect(
                                    ImVec2(tooltip_x, tooltip_y),
                                    ImVec2(tooltip_x + tooltip_width, tooltip_y + tooltip_height),
                                    border_color, S(6.0f), 0, S(2.0f));

                                // Draw thumbnail (centered within container)
                                float img_x = tooltip_x + padding + (container_width - thumb_w) * 0.5f;
                                float img_y = tooltip_y + padding + (container_height - thumb_h) * 0.5f;
                                fg_draw->AddImage(
                                    ThumbnailPoolIDToImTexture(preview_texture),
                                    ImVec2(img_x, img_y),
                                    ImVec2(img_x + thumb_w, img_y + thumb_h));

                                // Draw frame label centered below thumbnail
                                float label_x = tooltip_x + (tooltip_width - label_size.x) * 0.5f;
                                float label_y = tooltip_y + padding + container_height + S(4.0f);
                                fg_draw->AddText(ImVec2(label_x, label_y),
                                    UI_WHITE, frame_label);
                            }
                        }
                    }
                }
            }
        }

        // Ghost clip and insertion indicator during PLAYLIST reorder
        if (timeline_reorder_state.active && timeline_view) {
            auto& tracks = timeline_view->GetTracks();
            if (!tracks.empty()) {
                ImVec4 accent = GetWindowsAccentColor();
                ImU32 accent_color = IM_COL32(
                    (int)(accent.x * 255),
                    (int)(accent.y * 255),
                    (int)(accent.z * 255), 255);
                ImU32 ghost_fill = IM_COL32(
                    (int)(accent.x * 100),
                    (int)(accent.y * 100),
                    (int)(accent.z * 100), 120);

                // Track Y position (PLAYLIST uses only track 0 / V1)
                float track_y = tracks_start_pos.y + overview_offset;
                float clip_top = track_y + S(2);
                float clip_bottom = track_y + OTIOTimeline::TRACK_LANE_HEIGHT - S(2);
                float track_content_x = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH;

                // Draw ghost rectangle at mouse position
                float ghost_w = static_cast<float>(timeline_reorder_state.clip_duration) * pixels_per_second;
                float ghost_x = timeline_reorder_state.ghost_x - ghost_w * 0.5f;
                draw_list->AddRectFilled(
                    ImVec2(ghost_x, clip_top),
                    ImVec2(ghost_x + ghost_w, clip_bottom),
                    ghost_fill, S(4.0f));
                draw_list->AddRect(
                    ImVec2(ghost_x, clip_top),
                    ImVec2(ghost_x + ghost_w, clip_bottom),
                    accent_color, S(4.0f), 0, S(2.0f));

                // Find insertion position and draw insertion line
                int target_idx = timeline_reorder_state.target_insert_index;
                double insert_time = 0.0;

                // Calculate insert time based on target position
                int clip_count = 0;
                for (const auto& c : tracks[0].clips) {
                    if (c.is_gap) continue;
                    if (clip_count == target_idx) {
                        insert_time = c.start_time;
                        break;
                    }
                    clip_count++;
                    insert_time = c.start_time + c.duration;  // After last clip
                }

                // Draw insertion line
                float insert_x = track_content_x + static_cast<float>(insert_time) * pixels_per_second - scroll_offset_x;
                draw_list->AddLine(
                    ImVec2(insert_x, clip_top - S(8)),
                    ImVec2(insert_x, clip_bottom + S(8)),
                    accent_color, S(3.0f));

                // Draw small triangles at top and bottom of insertion line
                float tri_size = S(6.0f);
                // Top triangle (pointing down)
                draw_list->AddTriangleFilled(
                    ImVec2(insert_x - tri_size, clip_top - S(8)),
                    ImVec2(insert_x + tri_size, clip_top - S(8)),
                    ImVec2(insert_x, clip_top - S(2)),
                    accent_color);
                // Bottom triangle (pointing up)
                draw_list->AddTriangleFilled(
                    ImVec2(insert_x - tri_size, clip_bottom + S(8)),
                    ImVec2(insert_x + tri_size, clip_bottom + S(8)),
                    ImVec2(insert_x, clip_bottom + S(2)),
                    accent_color);

                // Position label
                char pos_buf[16];
                snprintf(pos_buf, sizeof(pos_buf), "Pos: %d", target_idx + 1);
                ImVec2 label_size = ImGui::CalcTextSize(pos_buf);
                float label_x = insert_x - label_size.x * 0.5f;
                float label_y = clip_top - S(24);
                draw_list->AddRectFilled(
                    ImVec2(label_x - S(4), label_y - S(2)),
                    ImVec2(label_x + label_size.x + S(4), label_y + label_size.y + S(2)),
                    IM_COL32(0, 0, 0, 200), S(3.0f));
                draw_list->AddText(ImVec2(label_x, label_y), UI_WHITE, pos_buf);
            }
        }

        // Advance cursor past the drawn content (include breadcrumb height when nested)
        ImGui::Dummy(ImVec2(content_region.x,
                           tracks_height + OTIOTimeline::TIMELINE_RULER_HEIGHT + breadcrumb_bar_height + S(10)));

        // =====================================================================
        // INTERACTION ZONE 1: RULER - Scrubbing/Seeking Only
        // =====================================================================
        float track_lanes_left = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH;
        float track_lanes_width = content_region.x - OTIOTimeline::TRACK_HEADER_WIDTH;

        ImGui::SetCursorScreenPos(ImVec2(track_lanes_left, ruler_y));
        ImGui::InvisibleButton("##otio_ruler_scrub",
            ImVec2(track_lanes_width, OTIOTimeline::TIMELINE_RULER_HEIGHT));

        bool ruler_hovered = ImGui::IsItemHovered();
        bool ruler_active = ImGui::IsItemActive();

        if (ruler_hovered || ruler_active) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

            ImVec2 mouse = ImGui::GetMousePos();
            float relative_x = mouse.x - track_lanes_left;
            double mouse_time = (relative_x + scroll_offset_x) / pixels_per_second;

            // Left click/drag on ruler = scrub
            if (ImGui::IsMouseDown(0)) {
                // Clamp to just before duration to stay on the last valid frame
                // (time == duration maps to frame N which is past the last frame N-1)
                double max_time = duration > 0 ? duration - 0.001 : 0.0;
                double new_time = std::max(0.0, std::min(mouse_time, max_time));
                if (video_player) {
                    if (!timeline_manager->IsScrubbing()) {
                        AutoSaveAnnotationOnSeek();
                        timeline_manager->StartScrubbing(video_player.get());
                    }
                    timeline_manager->UpdateScrubbing(new_time, video_player.get());
                }
            }

            // =================================================================
            // HOVER THUMBNAIL POPUP for VIDEO_FILE and IMAGE_SEQUENCE modes
            // Shows preview thumbnail when hovering over the ruler
            // Respects "Enable Timeline Thumbnails" setting from cache settings
            // =================================================================
            if (cache_settings.enable_thumbnails &&
                timeline_view && timeline_thumbnail_cache && !ImGui::IsMouseDown(0)) {
                auto source_mode = timeline_view->GetSourceMode();
                bool show_hover_thumbnail = (source_mode == qcview::TimelineSourceMode::VIDEO_FILE ||
                                             source_mode == qcview::TimelineSourceMode::IMAGE_SEQUENCE);

                if (show_hover_thumbnail && duration > 0) {
                    // Get the linked path and sequence info from the first video clip
                    std::string hover_media_path;
                    std::string seq_directory;
                    std::string seq_pattern;
                    std::string seq_exr_layer;
                    int seq_start_frame = 0;
                    double source_fps = 24.0;
                    int sequence_start_frame = 0;
                    bool is_sequence = (source_mode == qcview::TimelineSourceMode::IMAGE_SEQUENCE);
                    bool clip_is_sequence = false;

                    const auto& tracks_ref = timeline_view->GetTracks();
                    for (const auto& track : tracks_ref) {
                        if (track.is_video && !track.clips.empty()) {
                            const auto& clip = track.clips[0];
                            if (clip.is_linked && !clip.linked_path.empty()) {
                                hover_media_path = clip.linked_path;
                                source_fps = (clip.source_fps > 0) ? clip.source_fps : timeline_view->GetFrameRate();
                                // Get sequence info from clip
                                clip_is_sequence = clip.is_sequence;
                                seq_directory = clip.sequence_directory;
                                seq_pattern = clip.sequence_pattern;
                                seq_start_frame = clip.sequence_start_frame;
                                seq_exr_layer = clip.sequence_exr_layer;
                                // Get sequence start frame from source MediaItem for display
                                if (is_sequence) {
                                    auto* src_item = timeline_view->GetSourceMediaItem();
                                    if (src_item && src_item->image_seq.IsValid()) {
                                        sequence_start_frame = src_item->image_seq.start_frame;
                                    } else if (src_item) {
                                        sequence_start_frame = src_item->start_frame;
                                    }
                                }
                                break;
                            }
                        }
                    }

                    if (!hover_media_path.empty() && source_fps > 0) {
                        // Calculate hover frame from mouse position
                        double clamped_time = std::max(0.0, std::min(mouse_time, duration));
                        int hover_frame = static_cast<int>(clamped_time * source_fps);

                        // For sequences, construct the actual file path
                        std::string thumb_path;
                        int thumb_frame = hover_frame;
                        if (clip_is_sequence && !seq_directory.empty() && !seq_pattern.empty()) {
                            int file_frame = seq_start_frame + hover_frame;
                            char frame_file[512];
                            snprintf(frame_file, sizeof(frame_file), seq_pattern.c_str(), file_frame);
                            thumb_path = seq_directory + "/" + frame_file;
                            // For EXR with layer, append layer info so cache can use it
                            if (!seq_exr_layer.empty()) {
                                thumb_path += "?layer=" + seq_exr_layer;
                            }
                            thumb_frame = 0;  // Path already identifies the specific file
                        } else {
                            thumb_path = hover_media_path;
                        }

                        // Request thumbnail
                        int frame_width = 0, frame_height = 0;
                        GLuint hover_thumbnail = timeline_thumbnail_cache->GetThumbnail(
                            thumb_path, thumb_frame, frame_width, frame_height, true);
                        // NOTE: ProcessPendingUploads moved to before ImGui::NewFrame()

                        // Draw tooltip popup
                        const float container_width = S(200.0f);
                        const float container_height = S(120.0f);
                        float thumb_w = container_width;
                        float thumb_h = container_height;

                        if (hover_thumbnail != 0 && frame_width > 0 && frame_height > 0) {
                            float aspect = static_cast<float>(frame_width) / frame_height;
                            if (aspect > (container_width / container_height)) {
                                thumb_w = container_width;
                                thumb_h = thumb_w / aspect;
                            } else {
                                thumb_h = container_height;
                                thumb_w = thumb_h * aspect;
                            }
                        }

                        // Prepare frame label
                        int display_frame = is_sequence
                            ? qcview::FrameIndexing::InternalToSequenceDisplay(hover_frame, sequence_start_frame)
                            : qcview::FrameIndexing::InternalToDisplay(hover_frame);

                        double frame_time = hover_frame / source_fps;
                        int total_secs = static_cast<int>(frame_time);
                        int mins = total_secs / 60;
                        int secs = total_secs % 60;
                        int frame_in_sec = static_cast<int>((frame_time - total_secs) * source_fps);

                        char frame_label[128];
                        snprintf(frame_label, sizeof(frame_label), "Frame %d - %02d:%02d:%02d",
                                 display_frame, mins, secs, frame_in_sec);
                        ImVec2 label_size = ImGui::CalcTextSize(frame_label);

                        // Calculate tooltip size
                        const float padding = S(4.0f);
                        const float text_height = label_size.y + S(4.0f);
                        const float tooltip_width = container_width + padding * 2;
                        const float tooltip_height = container_height + text_height + padding * 2;

                        // Position tooltip above mouse, centered
                        float tooltip_x = mouse.x - tooltip_width * 0.5f;
                        float tooltip_y = ruler_y - tooltip_height - S(10.0f);

                        // Keep within viewport
                        ImGuiViewport* viewport = ImGui::GetMainViewport();
                        float vp_min_x = viewport->Pos.x;
                        float vp_max_x = viewport->Pos.x + viewport->Size.x;
                        float vp_min_y = viewport->Pos.y;
                        tooltip_x = std::max(vp_min_x + S(10.0f), std::min(tooltip_x, vp_max_x - tooltip_width - S(10.0f)));
                        tooltip_y = std::max(vp_min_y + S(10.0f), tooltip_y);

                        // Draw using foreground draw list
                        ImDrawList* fg_draw = ImGui::GetForegroundDrawList();

                        // Get accent color for border
                        ImVec4 accent = GetWindowsAccentColor();
                        ImU32 border_color = IM_COL32(
                            (int)(accent.x * 180), (int)(accent.y * 180), (int)(accent.z * 180), 255);

                        // Draw shadow
                        fg_draw->AddRectFilled(
                            ImVec2(tooltip_x + S(4), tooltip_y + S(4)),
                            ImVec2(tooltip_x + tooltip_width + S(4), tooltip_y + tooltip_height + S(4)),
                            IM_COL32(0, 0, 0, 100), S(6.0f));

                        // Draw background
                        fg_draw->AddRectFilled(
                            ImVec2(tooltip_x, tooltip_y),
                            ImVec2(tooltip_x + tooltip_width, tooltip_y + tooltip_height),
                            IM_COL32(30, 30, 30, 240), S(6.0f));

                        // Draw border
                        fg_draw->AddRect(
                            ImVec2(tooltip_x, tooltip_y),
                            ImVec2(tooltip_x + tooltip_width, tooltip_y + tooltip_height),
                            border_color, S(6.0f), 0, S(2.0f));

                        if (hover_thumbnail != 0) {
                            // Draw thumbnail centered
                            float img_x = tooltip_x + padding + (container_width - thumb_w) * 0.5f;
                            float img_y = tooltip_y + padding + (container_height - thumb_h) * 0.5f;
                            fg_draw->AddImage(
                                ThumbnailPoolIDToImTexture(hover_thumbnail),
                                ImVec2(img_x, img_y),
                                ImVec2(img_x + thumb_w, img_y + thumb_h));
                        } else {
                            // Show loading placeholder
                            const char* loading_text = "Loading...";
                            ImVec2 loading_size = ImGui::CalcTextSize(loading_text);
                            float loading_x = tooltip_x + (tooltip_width - loading_size.x) * 0.5f;
                            float loading_y = tooltip_y + padding + (container_height - loading_size.y) * 0.5f;
                            fg_draw->AddText(ImVec2(loading_x, loading_y), IM_COL32(180, 180, 180, 255), loading_text);
                        }

                        // Draw frame label centered below thumbnail
                        float label_x = tooltip_x + (tooltip_width - label_size.x) * 0.5f;
                        float label_y = tooltip_y + padding + container_height + S(4.0f);
                        fg_draw->AddText(ImVec2(label_x, label_y), UI_WHITE, frame_label);
                    }
                }
            }
        }

        // Stop scrubbing when mouse released (anywhere)
        if (timeline_manager && timeline_manager->IsScrubbing() && !ImGui::IsMouseDown(0)) {
            timeline_manager->StopScrubbing(video_player.get());
        }

        // =====================================================================
        // INTERACTION ZONE 2: TRACK LANES - Editing (selection, drag, trim)
        // =====================================================================
        ImGui::SetCursorScreenPos(ImVec2(track_lanes_left, tracks_start_pos.y));
        ImGui::InvisibleButton("##otio_track_lanes",
            ImVec2(track_lanes_width, tracks_height));

        bool tracks_hovered = ImGui::IsItemHovered();
        bool tracks_active = ImGui::IsItemActive();

        // Track mouse position for editing operations
        ImVec2 mouse = ImGui::GetMousePos();
        float relative_x = mouse.x - track_lanes_left;
        double mouse_time = (relative_x + scroll_offset_x) / pixels_per_second;
        // Subtract overview_offset because tracks_start_pos.y includes the overview track area
        float relative_y = mouse.y - tracks_start_pos.y - overview_offset;

        // Determine which track the mouse is over
        int hover_track_index = -1;
        // Calculate actual tracks height (excluding overview area)
        float actual_tracks_height = num_tracks * OTIOTimeline::TRACK_LANE_HEIGHT +
                                     OTIOTimeline::TRACK_SEPARATOR_HEIGHT;  // One separator between video/audio
        if (relative_y >= 0 && relative_y < actual_tracks_height && tracks_ptr) {
            float y_accum = 0.0f;
            for (int ti = 0; ti < num_tracks; ++ti) {
                // Account for separator before audio tracks
                if (ti > 0 && !(*tracks_ptr)[ti].is_video && (*tracks_ptr)[ti-1].is_video) {
                    y_accum += OTIOTimeline::TRACK_SEPARATOR_HEIGHT;
                }
                if (relative_y >= y_accum && relative_y < y_accum + OTIOTimeline::TRACK_LANE_HEIGHT) {
                    hover_track_index = ti;
                    break;
                }
                y_accum += OTIOTimeline::TRACK_LANE_HEIGHT;
            }
        }

        // =====================================================================
        // DROP TARGET FOR MEDIA FROM PROJECT PANEL
        // =====================================================================
        if (ImGui::BeginDragDropTarget()) {
            // Preview during hover (before accepting)
            if (const ImGuiPayload* payload = ImGui::GetDragDropPayload()) {
                if (payload->IsDataType("MEDIA_ITEM") || payload->IsDataType("MEDIA_ITEMS_MULTI")) {
                    // Get media info for ghost preview
                    std::string media_id;
                    if (payload->IsDataType("MEDIA_ITEM")) {
                        media_id = std::string(static_cast<const char*>(payload->Data), payload->DataSize - 1);
                    } else {
                        // Multi-select: use first item for preview
                        std::string payload_str(static_cast<const char*>(payload->Data), payload->DataSize - 1);
                        size_t sep = payload_str.find(';');
                        media_id = (sep != std::string::npos) ? payload_str.substr(0, sep) : payload_str;
                    }

                    if (project_manager) {
                        qcview::MediaItem* item = project_manager->GetMediaItem(media_id);
                        if (item) {
                            // IMAGE_SEQUENCE mode: show not-allowed cursor for locked video track
                            bool drop_target_locked = false;
                            if (timeline_view && timeline_view->GetSourceMode() == qcview::TimelineSourceMode::IMAGE_SEQUENCE) {
                                if (hover_track_index >= 0) {
                                    auto& tracks = timeline_view->GetTracks();
                                    if (hover_track_index < static_cast<int>(tracks.size()) && tracks[hover_track_index].is_video) {
                                        drop_target_locked = true;
                                        ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
                                    }
                                }
                            }

                            // Activate drop preview state (only if not blocked)
                            if (!drop_target_locked) {
                                timeline_media_drop.active = true;
                                timeline_media_drop.target_track_index = hover_track_index;
                                timeline_media_drop.drop_time = std::max(0.0, mouse_time);
                                timeline_media_drop.media_id = media_id;
                                timeline_media_drop.preview_duration = item->duration;
                                timeline_media_drop.preview_name = item->name;
                            }
                        }
                    }
                }
            }

            // Accept single media item
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEM")) {
                std::string media_id(static_cast<const char*>(payload->Data), payload->DataSize - 1);

                Debug::Log("[DROP DEBUG] Payload accepted, hover_track_index=" + std::to_string(hover_track_index) +
                           ", media_id=" + media_id);

                // PLAYLIST mode: use simplified drop handling via callback
                if (timeline_view && timeline_view->GetSourceMode() == qcview::TimelineSourceMode::PLAYLIST) {
                    Debug::Log("[DROP] PLAYLIST mode - using callback for single item");
                    // The drop callback was set up when playlist was loaded
                    // It will handle adding the item and reloading the timeline
                    // Trigger it by calling the timeline view's internal callback mechanism
                    if (project_manager) {
                        qcview::MediaItem* item = project_manager->GetMediaItem(media_id);
                        // Skip non-playable types
                        if (item && item->type != qcview::MediaType::PLAYLIST &&
                            item->type != qcview::MediaType::DUAL_VIEW &&
                            item->type != qcview::MediaType::IMAGE) {

                            // Get the source playlist
                            qcview::MediaItem* playlist = timeline_view->GetSourceMediaItem();
                            if (playlist && playlist->type == qcview::MediaType::PLAYLIST) {
                                // Add entry to playlist
                                qcview::PlaylistItemEntry entry;
                                entry.media_id = media_id;
                                entry.in_point = -1.0;
                                entry.out_point = -1.0;
                                playlist->playlist_items.push_back(entry);

                                Debug::Log("Added '" + item->name + "' to playlist via drop");

                                // Reload playlist timeline
                                // Ensure project manager is set for media resolution
                                timeline_view->SetProjectManager(project_manager.get());
                                timeline_view->LoadPlaylistAsTimeline(playlist);
                                timeline_view->InitializePlayback();

                                otio_timeline_mode = true;
                                if (video_player) {
                                    video_player->SetTimelineMode(true, timeline_view->GetPlaybackController());
                                }

                                if (auto* controller = timeline_view->GetPlaybackController()) {
                                    controller->Pause();
                                                }
                                timeline_view->RequestFitZoomOnNextRender();
                            }
                        }
                    }
                    timeline_media_drop.active = false;
                }
                // Create and insert clip from media (multi-track mode)
                else if (project_manager && timeline_view && timeline_command_manager && hover_track_index >= 0) {
                    // IMAGE_SEQUENCE mode: block drops onto video track (it's locked)
                    bool drop_blocked_by_lock = false;
                    if (timeline_view->GetSourceMode() == qcview::TimelineSourceMode::IMAGE_SEQUENCE) {
                        auto& tracks = timeline_view->GetTracks();
                        if (hover_track_index < static_cast<int>(tracks.size()) && tracks[hover_track_index].is_video) {
                            drop_blocked_by_lock = true;
                            Debug::Log("[DROP] Blocked: video track is locked in IMAGE_SEQUENCE mode");
                        }
                    }

                    qcview::MediaItem* item = project_manager->GetMediaItem(media_id);
                    if (!drop_blocked_by_lock && item) {
                        // Create OTIOClip from MediaItem
                        qcview::OTIOClip clip;
                        clip.id = "";  // Will be generated by InsertClipCommand
                        clip.name = item->name;

                        // Determine file path based on media type
                        if (item->type == qcview::MediaType::IMAGE_SEQUENCE || item->type == qcview::MediaType::EXR_SEQUENCE) {
                            clip.file_path = item->ffmpeg_pattern;  // Use FFmpeg pattern for sequences

                            // Mark as sequence and populate sequence metadata
                            clip.is_sequence = true;
                            clip.sequence_directory = item->image_seq.directory;
                            clip.sequence_pattern = item->image_seq.pattern;
                            clip.sequence_start_frame = item->image_seq.start_frame;
                            clip.sequence_end_frame = item->image_seq.end_frame;

                            if (item->type == qcview::MediaType::EXR_SEQUENCE) {
                                clip.sequence_exr_layer = item->image_seq.layer;
                            }
                        } else {
                            clip.file_path = item->path;
                        }

                        // In dual view mode, always align clips at beginning (start_time = 0)
                        // In normal mode, use mouse position
                        double insert_time = (timeline_view->IsDualViewMode()) ? 0.0 : std::max(0.0, mouse_time);
                        clip.start_time = insert_time;
                        clip.duration = item->duration;
                        clip.source_in = 0.0;
                        clip.source_out = item->duration;
                        clip.is_gap = false;

                        // Link immediately since we have the full path
                        clip.linked_path = clip.file_path;
                        clip.is_linked = true;
                        clip.source_fps = item->frame_rate > 0 ? item->frame_rate : timeline_view->GetFrameRate();
                        clip.source_width = (item->sequence_width > 0) ? item->sequence_width : item->timeline_width;
                        clip.source_height = (item->sequence_height > 0) ? item->sequence_height : item->timeline_height;
                        clip.source_duration = item->duration;

                        // Register sequence metadata with timeline cache for ImageSequenceDecoder
                        if (clip.is_sequence) {
                            // Use GetEffectivePlaybackController for scratch timelines
                            // (which use external_playback_controller_ instead of playback_controller_)
                            auto* ctrl = timeline_view->GetEffectivePlaybackController();
                            if (ctrl && ctrl->GetCache()) {
                                qcview::SequenceMetadata seq_meta;
                                seq_meta.directory = clip.sequence_directory;
                                seq_meta.pattern = clip.sequence_pattern;
                                seq_meta.start_frame = clip.sequence_start_frame;
                                seq_meta.end_frame = clip.sequence_end_frame;
                                seq_meta.exr_layer = clip.sequence_exr_layer;
                                seq_meta.pipeline_mode = item->image_seq.pipeline_mode;
                                seq_meta.valid = true;
                                ctrl->GetCache()->RegisterSequenceMetadata(clip.linked_path, seq_meta);
                                Debug::Log("Registered sequence metadata for: " + clip.linked_path);
                                Debug::Log("  start_frame: " + std::to_string(seq_meta.start_frame) +
                                           ", end_frame: " + std::to_string(seq_meta.end_frame) +
                                           " (frame count: " + std::to_string(seq_meta.end_frame - seq_meta.start_frame + 1) + ")");
                            } else {
                                Debug::Log("WARNING: Could not register sequence metadata - ctrl=" +
                                           std::string(ctrl ? "valid" : "null") +
                                           ", cache=" + std::string((ctrl && ctrl->GetCache()) ? "valid" : "null"));
                            }
                        }

                        // Probe for audio presence (for UI indicator on video clips)
                        if (!clip.is_sequence && qcview::MediaLinker::IsVideoFile(clip.file_path)) {
                            qcview::VideoProbeResult probe = qcview::MediaLinker::ProbeVideoFile(clip.file_path);
                            if (probe.valid) {
                                clip.has_audio = probe.has_audio;
                            }
                        }

                        Debug::Log("Clip drop: item->frame_rate=" + std::to_string(item->frame_rate) +
                                   ", clip.source_fps=" + std::to_string(clip.source_fps) +
                                   ", timeline_fps=" + std::to_string(timeline_view->GetFrameRate()));

                        // In dual view mode, REPLACE the track content (only one clip per track allowed)
                        // Clear the track first, then add the new clip
                        if (timeline_view->IsDualViewMode()) {
                            auto& tracks = timeline_view->GetTracks();
                            if (hover_track_index >= 0 && hover_track_index < static_cast<int>(tracks.size())) {
                                tracks[hover_track_index].clips.clear();
                                Debug::Log("[DUAL VIEW DROP] Cleared track " + std::to_string(hover_track_index) + " for replacement");
                            }
                        }

                        // Execute via command for undo/redo (overwrite overlapping clips)
                        auto cmd = std::make_unique<qcview::OverwriteEditCommand>(
                            timeline_view.get(), hover_track_index, clip);
                        timeline_command_manager->Execute(std::move(cmd));

                        // Recalculate duration in case clip extends past current timeline end
                        timeline_view->RecalculateDuration();

                        // In dual view mode, auto-fit zoom to show entire timeline after drop
                        if (timeline_view->IsDualViewMode()) {
                            timeline_view->RequestFitZoomOnNextRender();
                        }

                        Debug::Log("Inserted clip '" + clip.name + "' at " +
                                   std::to_string(insert_time) + "s on track " +
                                   std::to_string(hover_track_index));
                    } else {
                        Debug::Log("[DROP DEBUG] Drop blocked: item=" + std::string(item ? item->name : "null") +
                                   ", type=" + (item ? std::to_string(static_cast<int>(item->type)) : "N/A"));
                    }
                } else {
                    Debug::Log("[DROP DEBUG] Drop conditions not met: project_manager=" +
                               std::string(project_manager ? "OK" : "null") +
                               ", timeline_view=" + std::string(timeline_view ? "OK" : "null") +
                               ", cmd_manager=" + std::string(timeline_command_manager ? "OK" : "null") +
                               ", hover_track_index=" + std::to_string(hover_track_index));
                }
                timeline_media_drop.active = false;
            }

            // Accept multiple media items
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEMS_MULTI")) {
                std::string payload_str(static_cast<const char*>(payload->Data), payload->DataSize - 1);

                // PLAYLIST mode: use simplified drop handling
                if (timeline_view && timeline_view->GetSourceMode() == qcview::TimelineSourceMode::PLAYLIST && project_manager) {
                    Debug::Log("[DROP] PLAYLIST mode - using callback for multiple items");

                    qcview::MediaItem* playlist = timeline_view->GetSourceMediaItem();
                    if (playlist && playlist->type == qcview::MediaType::PLAYLIST) {
                        std::istringstream ss(payload_str);
                        std::string media_id;
                        int added_count = 0;

                        while (std::getline(ss, media_id, ';')) {
                            if (media_id.empty()) continue;

                            qcview::MediaItem* item = project_manager->GetMediaItem(media_id);
                            // Skip non-playable types
                            if (item && item->type != qcview::MediaType::PLAYLIST &&
                                item->type != qcview::MediaType::DUAL_VIEW &&
                                item->type != qcview::MediaType::IMAGE) {

                                qcview::PlaylistItemEntry entry;
                                entry.media_id = media_id;
                                entry.in_point = -1.0;
                                entry.out_point = -1.0;
                                playlist->playlist_items.push_back(entry);
                                added_count++;

                                Debug::Log("Added '" + item->name + "' to playlist via multi-drop");
                            }
                        }

                        if (added_count > 0) {
                            // Reload playlist timeline
                            // Ensure project manager is set for media resolution
                            timeline_view->SetProjectManager(project_manager.get());
                            timeline_view->LoadPlaylistAsTimeline(playlist);
                            timeline_view->InitializePlayback();

                            otio_timeline_mode = true;
                            if (video_player) {
                                video_player->SetTimelineMode(true, timeline_view->GetPlaybackController());
                            }

                            if (auto* controller = timeline_view->GetPlaybackController()) {
                                controller->Pause();
                                        }
                            timeline_view->RequestFitZoomOnNextRender();
                        }
                    }
                    timeline_media_drop.active = false;
                }
                // Parse semicolon-separated IDs and insert sequentially (multi-track mode)
                else if (project_manager && timeline_view && timeline_command_manager && hover_track_index >= 0) {
                    // IMAGE_SEQUENCE mode: block drops onto video track (it's locked)
                    bool drop_blocked_by_lock = false;
                    if (timeline_view->GetSourceMode() == qcview::TimelineSourceMode::IMAGE_SEQUENCE) {
                        auto& tracks = timeline_view->GetTracks();
                        if (hover_track_index < static_cast<int>(tracks.size()) && tracks[hover_track_index].is_video) {
                            drop_blocked_by_lock = true;
                            Debug::Log("[DROP] Blocked multi-drop: video track is locked in IMAGE_SEQUENCE mode");
                        }
                    }

                    if (!drop_blocked_by_lock) {
                        // In dual view mode, always align clips at beginning (start_time = 0)
                        double current_insert_time = (timeline_view && timeline_view->IsDualViewMode()) ? 0.0 : std::max(0.0, mouse_time);
                        std::istringstream ss(payload_str);
                        std::string media_id;

                        while (std::getline(ss, media_id, ';')) {
                        if (media_id.empty()) continue;

                        qcview::MediaItem* item = project_manager->GetMediaItem(media_id);
                        if (item) {
                            qcview::OTIOClip clip;
                            clip.id = "";
                            clip.name = item->name;

                            if (item->type == qcview::MediaType::IMAGE_SEQUENCE || item->type == qcview::MediaType::EXR_SEQUENCE) {
                                clip.file_path = item->ffmpeg_pattern;

                                // Mark as sequence and populate sequence metadata
                                clip.is_sequence = true;
                                clip.sequence_directory = item->image_seq.directory;
                                clip.sequence_pattern = item->image_seq.pattern;
                                clip.sequence_start_frame = item->image_seq.start_frame;
                                clip.sequence_end_frame = item->image_seq.end_frame;

                                if (item->type == qcview::MediaType::EXR_SEQUENCE) {
                                    clip.sequence_exr_layer = item->image_seq.layer;
                                }
                            } else {
                                clip.file_path = item->path;
                            }

                            clip.start_time = current_insert_time;
                            clip.duration = item->duration;
                            clip.source_in = 0.0;
                            clip.source_out = item->duration;
                            clip.is_gap = false;

                            clip.linked_path = clip.file_path;
                            clip.is_linked = true;
                            clip.source_fps = item->frame_rate > 0 ? item->frame_rate : timeline_view->GetFrameRate();
                            clip.source_width = (item->sequence_width > 0) ? item->sequence_width : item->timeline_width;
                            clip.source_height = (item->sequence_height > 0) ? item->sequence_height : item->timeline_height;
                            clip.source_duration = item->duration;

                            // Register sequence metadata with timeline cache for ImageSequenceDecoder
                            if (clip.is_sequence) {
                                // Use GetEffectivePlaybackController for scratch timelines
                                auto* ctrl = timeline_view->GetEffectivePlaybackController();
                                if (ctrl && ctrl->GetCache()) {
                                    qcview::SequenceMetadata seq_meta;
                                    seq_meta.directory = clip.sequence_directory;
                                    seq_meta.pattern = clip.sequence_pattern;
                                    seq_meta.start_frame = clip.sequence_start_frame;
                                    seq_meta.end_frame = clip.sequence_end_frame;
                                    seq_meta.exr_layer = clip.sequence_exr_layer;
                                    seq_meta.pipeline_mode = item->image_seq.pipeline_mode;
                                    seq_meta.valid = true;
                                    ctrl->GetCache()->RegisterSequenceMetadata(clip.linked_path, seq_meta);
                                    Debug::Log("Registered sequence metadata (multi) for: " + clip.linked_path);
                                    Debug::Log("  start_frame: " + std::to_string(seq_meta.start_frame) +
                                               ", end_frame: " + std::to_string(seq_meta.end_frame) +
                                               " (frame count: " + std::to_string(seq_meta.end_frame - seq_meta.start_frame + 1) + ")");
                                } else {
                                    Debug::Log("WARNING: Could not register sequence metadata (multi) - ctrl=" +
                                               std::string(ctrl ? "valid" : "null"));
                                }
                            }

                            // Probe for audio presence (for UI indicator on video clips)
                            if (!clip.is_sequence && qcview::MediaLinker::IsVideoFile(clip.file_path)) {
                                qcview::VideoProbeResult probe = qcview::MediaLinker::ProbeVideoFile(clip.file_path);
                                if (probe.valid) {
                                    clip.has_audio = probe.has_audio;
                                }
                            }

                            // Execute via command for undo/redo (overwrite overlapping clips)
                            auto cmd = std::make_unique<qcview::OverwriteEditCommand>(
                                timeline_view.get(), hover_track_index, clip);
                            timeline_command_manager->Execute(std::move(cmd));

                            Debug::Log("Inserted clip '" + clip.name + "' at " +
                                       std::to_string(current_insert_time) + "s on track " +
                                       std::to_string(hover_track_index));

                            // Next clip starts after this one
                            current_insert_time += item->duration;
                        }
                    }
                    // Recalculate duration in case clips extend past current timeline end
                    timeline_view->RecalculateDuration();

                    // In dual view mode, auto-fit zoom to show entire timeline after drop
                    if (timeline_view->IsDualViewMode()) {
                        timeline_view->RequestFitZoomOnNextRender();
                    }
                    }  // end if (!drop_blocked_by_lock)
                }
                timeline_media_drop.active = false;
            }

            ImGui::EndDragDropTarget();
        } else {
            // Clear drop state when not over drop target
            timeline_media_drop.active = false;
        }

        // =====================================================================
        // CLIP DRAGGING
        // =====================================================================
        if (timeline_clip_drag.active) {
            // Update drag position
            double new_start = mouse_time - timeline_clip_drag.drag_offset;
            new_start = std::max(0.0, new_start);

            // Visual feedback: show ghost of clip at new position
            // (The actual clip move happens on mouse release)

            // IMAGE_SEQUENCE mode: show not-allowed cursor when hovering over locked video track
            if (timeline_view && timeline_view->GetSourceMode() == qcview::TimelineSourceMode::IMAGE_SEQUENCE) {
                if (hover_track_index >= 0 && hover_track_index != timeline_clip_drag.original_track_index) {
                    auto& tracks = timeline_view->GetTracks();
                    if (hover_track_index < static_cast<int>(tracks.size()) && tracks[hover_track_index].is_video) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
                    }
                }
            }

            if (!ImGui::IsMouseDown(0)) {
                // Mouse released - commit the move
                if (timeline_view && timeline_command_manager) {
                    // Find the clip to get its duration for snapping
                    qcview::OTIOClip* drop_clip = timeline_view->FindClipById(timeline_clip_drag.clip_id);
                    double clip_duration = drop_clip ? drop_clip->duration : 1.0;

                    // Collect snap points (exclude dragged clip)
                    std::set<std::string> exclude_ids;
                    exclude_ids.insert(timeline_clip_drag.clip_id);
                    for (const auto& dc : timeline_clip_drag.dragged_clips) {
                        exclude_ids.insert(dc.clip_id);
                    }

                    double playhead_time = timeline_manager ? timeline_manager->GetUIPosition() : 0.0;
                    auto snap_points = CollectSnapPoints(
                        timeline_view->GetTracks(),
                        exclude_ids,
                        playhead_time,
                        timeline_view->GetDuration()
                    );

                    // Calculate final position with snapping
                    double proposed_start = mouse_time - timeline_clip_drag.drag_offset;
                    bool snapped_on_drop = false;
                    double snap_line_unused = 0.0;
                    double final_start = CalculateSnappedPosition(
                        proposed_start,
                        clip_duration,
                        snap_points,
                        pixels_per_second,
                        snapped_on_drop,
                        snap_line_unused
                    );

                    // Check for multi-clip drag
                    if (timeline_clip_drag.dragged_clips.size() > 1) {
                        // Multi-clip move: check for overlap first
                        bool would_overlap = WouldCauseOverlap(
                            timeline_view->GetTracks(),
                            timeline_clip_drag.dragged_clips,
                            final_start
                        );

                        if (!would_overlap) {
                            // Build move list for all clips
                            std::vector<qcview::MoveMultipleClipsCommand::ClipMoveInfo> moves;
                            for (const auto& dc : timeline_clip_drag.dragged_clips) {
                                qcview::MoveMultipleClipsCommand::ClipMoveInfo info;
                                info.clip_id = dc.clip_id;
                                info.track_index = dc.track_index;
                                info.old_start_time = dc.original_start_time;
                                info.new_start_time = final_start + dc.offset_from_primary;
                                moves.push_back(info);
                            }

                            auto cmd = std::make_unique<qcview::MoveMultipleClipsCommand>(
                                timeline_view.get(), std::move(moves));
                            timeline_command_manager->Execute(std::move(cmd));
                            Debug::Log("Moved " + std::to_string(timeline_clip_drag.dragged_clips.size()) +
                                       " clips to time " + std::to_string(final_start));
                        } else {
                            Debug::Log("Multi-clip move blocked: would cause overlap");
                        }
                    } else if (hover_track_index >= 0 && hover_track_index != timeline_clip_drag.original_track_index) {
                        // IMAGE_SEQUENCE mode: block moves to video track (it's locked)
                        bool target_track_locked = false;
                        if (timeline_view->GetSourceMode() == qcview::TimelineSourceMode::IMAGE_SEQUENCE) {
                            auto& tracks = timeline_view->GetTracks();
                            if (hover_track_index < static_cast<int>(tracks.size()) && tracks[hover_track_index].is_video) {
                                target_track_locked = true;
                                Debug::Log("[DRAG] Blocked: cannot move clip to locked video track in IMAGE_SEQUENCE mode");
                            }
                        }

                        // Single clip: move to different track with overwrite on destination
                        qcview::OTIOClip* drag_clip = timeline_view->FindClipById(timeline_clip_drag.clip_id);
                        if (!target_track_locked && drag_clip) {
                            // Create clip data for insertion into new track
                            qcview::OTIOClip moved_clip = *drag_clip;
                            moved_clip.start_time = final_start;

                            // Composite: delete from old track, then overwrite-insert into new track
                            auto composite = std::make_unique<qcview::CompositeCommand>("Move Clip to Track (Overwrite)");
                            composite->AddCommand(std::make_unique<qcview::DeleteClipCommand>(
                                timeline_view.get(), timeline_clip_drag.clip_id, timeline_clip_drag.original_track_index));
                            composite->AddCommand(std::make_unique<qcview::OverwriteEditCommand>(
                                timeline_view.get(), hover_track_index, moved_clip));
                            timeline_command_manager->Execute(std::move(composite));
                            Debug::Log("Moved clip to track " + std::to_string(hover_track_index) + " (with overwrite)");
                        }
                    } else if (std::abs(final_start - timeline_clip_drag.original_start_time) > 0.001) {
                        // Single clip: move within same track (horizontal) with overwrite
                        qcview::OTIOClip* drag_clip = timeline_view->FindClipById(timeline_clip_drag.clip_id);
                        if (drag_clip) {
                            // Create clip data with new position for overwrite calculation
                            qcview::OTIOClip moved_clip = *drag_clip;
                            moved_clip.start_time = final_start;

                            auto cmd = std::make_unique<qcview::OverwriteEditCommand>(
                                timeline_view.get(),
                                timeline_clip_drag.original_track_index,
                                moved_clip,
                                timeline_clip_drag.clip_id);  // Exclude this clip from overlap checks
                            timeline_command_manager->Execute(std::move(cmd));
                            Debug::Log("Moved clip to time " + std::to_string(final_start) + " (with overwrite)");
                        }
                    }
                    timeline_view->RecalculateDuration();

                    // Trigger cache refill around current playhead for viewport update
                    auto* playback_ctrl = timeline_view->GetEffectivePlaybackController();
                    if (playback_ctrl && playback_ctrl->IsInitialized()) {
                        auto* cache = playback_ctrl->GetCache();
                        if (cache && timeline_manager) {
                            double playhead_time = timeline_manager->GetUIPosition();
                            double fps = playback_ctrl->GetFPS();
                            int playhead_frame = static_cast<int>(playhead_time * fps + 0.5);
                            // UpdatePlayhead triggers cache thread to refill around this position
                            cache->UpdatePlayhead(playhead_frame, false);
                            // Also request the frame directly for immediate load
                            cache->RequestFrame(playhead_frame);
                        }
                    }
                }
                timeline_clip_drag.active = false;

                // Resume playback after drag operation completes
                if (timeline_view) {
                    timeline_view->EndEdit(true);  // Resume if was playing
                }

                // Defer thumbnail cache clear until next frame start
                // This prevents deleting textures that ImGui may still reference in current frame's draw list
                timeline_thumbnail_cache_clear_deferred = true;
            }
        }

        // =====================================================================
        // CLIP EDGE TRIMMING
        // =====================================================================
        if (timeline_trim_state.active) {
            if (!ImGui::IsMouseDown(0)) {
                // Mouse released - commit the trim
                bool trim_handled = false;

                // PLAYLIST mode: Direct trim with ripple (no command manager)
                if (timeline_view && timeline_view->GetSourceMode() == qcview::TimelineSourceMode::PLAYLIST) {
                    auto& tracks = timeline_view->GetTracks();
                    if (!tracks.empty()) {
                        // Find the clip being trimmed
                        qcview::OTIOClip* clip = nullptr;
                        int clip_index = -1;
                        for (size_t i = 0; i < tracks[0].clips.size(); i++) {
                            if (tracks[0].clips[i].id == timeline_trim_state.clip_id) {
                                clip = &tracks[0].clips[i];
                                clip_index = static_cast<int>(i);
                                break;
                            }
                        }

                        if (clip && !clip->is_gap) {
                            double delta_time = mouse_time - (timeline_trim_state.is_left_edge ?
                                timeline_trim_state.original_value :
                                timeline_trim_state.original_value);

                            if (timeline_trim_state.is_left_edge) {
                                // Trim start: adjust source_in
                                double new_source_in = timeline_trim_state.original_source_in + delta_time;
                                new_source_in = std::max(0.0, std::min(new_source_in, clip->source_out - 0.04));
                                clip->source_in = new_source_in;
                                clip->duration = clip->source_out - clip->source_in;
                                Debug::Log("[Playlist Trim] Trimmed start to source_in=" + std::to_string(new_source_in));
                            } else {
                                // Trim end: adjust source_out
                                double new_source_out = clip->source_in + timeline_trim_state.original_duration + delta_time;
                                new_source_out = std::max(clip->source_in + 0.04, std::min(new_source_out, clip->source_duration));
                                clip->source_out = new_source_out;
                                clip->duration = clip->source_out - clip->source_in;
                                Debug::Log("[Playlist Trim] Trimmed end to source_out=" + std::to_string(new_source_out));
                            }

                            // Ripple: recalculate all clip positions
                            double fps = timeline_view->GetFrameRate();
                            const int gap_frames = 10;
                            double gap_duration = gap_frames / fps;
                            double current_time = 0.0;

                            for (auto& c : tracks[0].clips) {
                                c.start_time = current_time;
                                current_time += c.duration;
                            }

                            // Sync flattener and cache
                            timeline_view->SyncFlattenerAndInvalidate();

                            // Save trim edits to MediaItem for persistence
                            qcview::MediaItem* playlist_item = timeline_view->GetSourceMediaItem();
                            if (playlist_item && project_manager) {
                                // Update the playlist item's in/out points to match the edited clips
                                int playlist_index = 0;
                                for (const auto& c : tracks[0].clips) {
                                    if (c.is_gap) continue;
                                    if (playlist_index < static_cast<int>(playlist_item->playlist_items.size())) {
                                        playlist_item->playlist_items[playlist_index].in_point = c.source_in;
                                        playlist_item->playlist_items[playlist_index].out_point = c.source_out;
                                    }
                                    playlist_index++;
                                }
                                Debug::Log("[Playlist Trim] Saved in/out points to MediaItem");
                            }

                            // Push to undo stack for undo/redo support
                            double old_source_in, old_source_out;
                            if (timeline_trim_state.is_left_edge) {
                                old_source_in = timeline_trim_state.original_source_in;
                                old_source_out = clip->source_out;  // Unchanged by left edge trim
                            } else {
                                old_source_in = clip->source_in;  // Unchanged by right edge trim
                                old_source_out = clip->source_in + timeline_trim_state.original_duration;
                            }

                            auto cmd = std::make_unique<qcview::TrimPlaylistClipCommand>(
                                timeline_view.get(),
                                timeline_trim_state.clip_id,
                                old_source_in, old_source_out, timeline_trim_state.original_duration,
                                clip->source_in, clip->source_out, clip->duration
                            );
                            // Call Execute to set executed_ flag (values already applied, this is idempotent)
                            cmd->Execute();
                            // Push to undo stack
                            auto* cmd_mgr = GetCommandManager(timeline_view.get());
                            if (cmd_mgr) {
                                cmd_mgr->PushExecuted(std::move(cmd));
                            }

                            trim_handled = true;
                        }
                    }
                }

                // Standard multi-track mode: use command manager
                if (!trim_handled && timeline_view && timeline_command_manager) {
                    if (timeline_trim_state.is_left_edge) {
                        auto cmd = std::make_unique<qcview::TrimClipStartCommand>(
                            timeline_view.get(),
                            timeline_trim_state.clip_id,
                            timeline_trim_state.track_index,
                            mouse_time);
                        timeline_command_manager->Execute(std::move(cmd));
                        Debug::Log("Trimmed clip start to " + std::to_string(mouse_time));
                    } else {
                        auto cmd = std::make_unique<qcview::TrimClipEndCommand>(
                            timeline_view.get(),
                            timeline_trim_state.clip_id,
                            timeline_trim_state.track_index,
                            mouse_time);
                        timeline_command_manager->Execute(std::move(cmd));
                        Debug::Log("Trimmed clip end to " + std::to_string(mouse_time));
                    }
                    timeline_view->RecalculateDuration();

                    // Trigger cache refill around current playhead for viewport update
                    auto* playback_ctrl = timeline_view->GetEffectivePlaybackController();
                    if (playback_ctrl && playback_ctrl->IsInitialized()) {
                        auto* cache = playback_ctrl->GetCache();
                        if (cache && timeline_manager) {
                            double playhead_time = timeline_manager->GetUIPosition();
                            double fps = playback_ctrl->GetFPS();
                            int playhead_frame = static_cast<int>(playhead_time * fps + 0.5);
                            // UpdatePlayhead triggers cache thread to refill around this position
                            cache->UpdatePlayhead(playhead_frame, false);
                            // Also request the frame directly for immediate load
                            cache->RequestFrame(playhead_frame);
                        }
                    }
                }
                timeline_trim_state.active = false;

                // Resume playback after trim operation completes
                if (timeline_view) {
                    timeline_view->EndEdit(true);  // Resume if was playing
                }

                // Defer thumbnail cache clear until next frame start
                // This prevents deleting textures that ImGui may still reference in current frame's draw list
                timeline_thumbnail_cache_clear_deferred = true;
            }
        }

        // =====================================================================
        // PLAYLIST PENDING DRAG - Check threshold before starting slip/reorder
        // =====================================================================
        if (playlist_pending_drag_active && timeline_view) {
            ImVec2 current_mouse = ImGui::GetMousePos();
            float delta = std::abs(current_mouse.x - playlist_pending_drag_start_x);

            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                // Escape pressed - cancel pending drag
                playlist_pending_drag_active = false;
                Debug::Log("[Playlist] Pending drag cancelled with Escape");
            } else if (!ImGui::IsMouseDown(0)) {
                // Mouse released before threshold - just a click, no edit
                playlist_pending_drag_active = false;
                Debug::Log("[Playlist] Click without drag - selection only");
            } else if (delta >= kPlaylistDragThreshold) {
                // Threshold exceeded - start the actual edit
                playlist_pending_drag_active = false;
                timeline_view->BeginEdit();

                if (playlist_pending_drag_shift) {
                    // Start SLIP edit
                    Debug::Log("[Playlist Slip] Starting slip after drag threshold (" + std::to_string(delta) + "px)");
                    timeline_slip_state.active = true;
                    timeline_slip_state.clip_id = playlist_pending_drag_clip_id;
                    timeline_slip_state.clip_index = playlist_pending_drag_clip_idx;
                    timeline_slip_state.original_source_in = playlist_pending_drag_source_in;
                    timeline_slip_state.original_source_out = playlist_pending_drag_source_out;
                    timeline_slip_state.source_duration = playlist_pending_drag_source_duration;
                    timeline_slip_state.start_mouse_x = playlist_pending_drag_start_x;
                } else {
                    // Start REORDER
                    Debug::Log("[Playlist Reorder] Starting reorder after drag threshold (" + std::to_string(delta) + "px)");
                    timeline_reorder_state.active = true;
                    timeline_reorder_state.clip_id = playlist_pending_drag_clip_id;
                    timeline_reorder_state.source_clip_index = playlist_pending_drag_clip_idx;
                    timeline_reorder_state.target_insert_index = playlist_pending_drag_clip_idx;
                    timeline_reorder_state.clip_duration = playlist_pending_drag_clip_duration;
                    timeline_reorder_state.start_mouse_x = playlist_pending_drag_start_x;
                    timeline_reorder_state.ghost_x = current_mouse.x;
                }
            }
        }

        // =====================================================================
        // SLIP EDITING (PLAYLIST mode - Shift+drag on clip body)
        // =====================================================================
        if (timeline_slip_state.active && timeline_view) {
            ImVec2 current_mouse = ImGui::GetMousePos();
            float delta_x = current_mouse.x - timeline_slip_state.start_mouse_x;
            double delta_time = delta_x / pixels_per_second;

            // Find the clip and update its source_in/source_out
            auto& tracks = timeline_view->GetTracks();
            if (!tracks.empty() && timeline_slip_state.clip_index >= 0 &&
                timeline_slip_state.clip_index < static_cast<int>(tracks[0].clips.size())) {

                auto& clip = tracks[0].clips[timeline_slip_state.clip_index];
                double clip_duration = timeline_slip_state.original_source_out - timeline_slip_state.original_source_in;

                // Calculate new source_in/source_out (shifted together)
                double new_source_in = timeline_slip_state.original_source_in + delta_time;
                double new_source_out = timeline_slip_state.original_source_out + delta_time;

                // Clamp to source boundaries
                if (new_source_in < 0.0) {
                    new_source_in = 0.0;
                    new_source_out = clip_duration;
                }
                if (new_source_out > timeline_slip_state.source_duration) {
                    new_source_out = timeline_slip_state.source_duration;
                    new_source_in = timeline_slip_state.source_duration - clip_duration;
                    if (new_source_in < 0.0) new_source_in = 0.0;
                }

                // Update clip (live preview)
                clip.source_in = new_source_in;
                clip.source_out = new_source_out;
                // Duration stays the same - slip doesn't change timeline position

                // Sync flattener for live preview
                timeline_view->GetFlattener().SetTracks(tracks);
            }

            // Cancel on Escape - revert to original values
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                Debug::Log("[Playlist Slip] Cancelled with Escape - reverting to original");

                auto& tracks = timeline_view->GetTracks();
                if (!tracks.empty() && timeline_slip_state.clip_index >= 0 &&
                    timeline_slip_state.clip_index < static_cast<int>(tracks[0].clips.size())) {
                    auto& clip = tracks[0].clips[timeline_slip_state.clip_index];
                    clip.source_in = timeline_slip_state.original_source_in;
                    clip.source_out = timeline_slip_state.original_source_out;
                    timeline_view->GetFlattener().SetTracks(tracks);
                    timeline_view->SyncFlattenerAndInvalidate();
                }

                timeline_slip_state.active = false;
                timeline_view->EndEdit(false);  // End edit without committing
            }
            // Commit on mouse release
            else if (!ImGui::IsMouseDown(0)) {
                Debug::Log("[Playlist Slip] Committed slip for clip: " + timeline_slip_state.clip_id);

                auto& tracks = timeline_view->GetTracks();
                if (!tracks.empty() && timeline_slip_state.clip_index >= 0 &&
                    timeline_slip_state.clip_index < static_cast<int>(tracks[0].clips.size())) {

                    auto& clip = tracks[0].clips[timeline_slip_state.clip_index];

                    // Capture final values before reverting
                    double final_source_in = clip.source_in;
                    double final_source_out = clip.source_out;

                    // Revert to original values so command can capture them
                    clip.source_in = timeline_slip_state.original_source_in;
                    clip.source_out = timeline_slip_state.original_source_out;

                    // Create and execute slip command (captures old values, applies new)
                    auto cmd = std::make_unique<qcview::SlipClipCommand>(
                        timeline_view.get(),
                        timeline_slip_state.clip_id,
                        0,  // track_index (PLAYLIST mode only has one track)
                        final_source_in,
                        final_source_out
                    );
                    cmd->Execute();

                    // Push to undo stack (without calling Execute again)
                    // Note: SlipClipCommand also updates MediaItem for persistence
                    auto* cmd_mgr = GetCommandManager(timeline_view.get());
                    if (cmd_mgr) {
                        cmd_mgr->PushExecuted(std::move(cmd));
                    }
                }

                timeline_slip_state.active = false;
                timeline_view->EndEdit(true);
            }
        }

        // =====================================================================
        // REORDER (PLAYLIST mode - drag clip to new position)
        // =====================================================================
        if (timeline_reorder_state.active && timeline_view) {
            ImVec2 current_mouse = ImGui::GetMousePos();
            auto& tracks = timeline_view->GetTracks();

            // Update ghost position
            timeline_reorder_state.ghost_x = current_mouse.x;

            // Calculate mouse time position
            float track_content_x = tracks_start_pos.x + OTIOTimeline::TRACK_HEADER_WIDTH;
            double mouse_time = (current_mouse.x - track_content_x + scroll_offset_x) / pixels_per_second;
            mouse_time = std::max(0.0, mouse_time);

            // Find target insert position based on mouse position
            // We look at clip boundaries and find where the mouse is
            int target_index = 0;
            if (!tracks.empty()) {
                int clip_count = 0;  // Count only non-gap clips
                for (size_t ci = 0; ci < tracks[0].clips.size(); ci++) {
                    const auto& c = tracks[0].clips[ci];
                    if (c.is_gap) continue;

                    double clip_center = c.start_time + c.duration / 2.0;
                    if (mouse_time < clip_center) {
                        target_index = clip_count;
                        break;
                    }
                    clip_count++;
                    target_index = clip_count;  // After all clips
                }
            }
            timeline_reorder_state.target_insert_index = target_index;

            // Cancel on Escape
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                Debug::Log("[Playlist Reorder] Cancelled with Escape");
                timeline_reorder_state.active = false;
                timeline_view->EndEdit(false);  // End edit without committing
            }
            // Commit on mouse release
            else if (!ImGui::IsMouseDown(0)) {
                // Get source and target indices (counting only non-gap clips)
                int source_idx = 0;
                int source_array_idx = timeline_reorder_state.source_clip_index;
                for (int ci = 0; ci < source_array_idx && !tracks.empty(); ci++) {
                    if (!tracks[0].clips[ci].is_gap) source_idx++;
                }

                int target_idx = timeline_reorder_state.target_insert_index;

                if (source_idx != target_idx && !tracks.empty()) {
                    Debug::Log("[Playlist Reorder] Moving clip from position " +
                               std::to_string(source_idx) + " to " + std::to_string(target_idx));

                    // Create and execute reorder command
                    auto cmd = std::make_unique<qcview::ReorderPlaylistCommand>(
                        timeline_view.get(),
                        source_idx,
                        target_idx
                    );
                    cmd->Execute();

                    // Push to undo stack
                    auto* cmd_mgr = GetCommandManager(timeline_view.get());
                    if (cmd_mgr) {
                        cmd_mgr->PushExecuted(std::move(cmd));
                    }
                } else {
                    Debug::Log("[Playlist Reorder] No change - same position");
                }

                timeline_reorder_state.active = false;
                timeline_view->EndEdit(true);
            }
        }

        // =====================================================================
        // NAVIGATION (works over both ruler and tracks)
        // =====================================================================
        bool timeline_hovered = ruler_hovered || tracks_hovered;

        if (timeline_hovered) {
            // Calculate max scroll to prevent scrolling past timeline end
            float visible_width = content_region.x - OTIOTimeline::TRACK_HEADER_WIDTH;
            float max_scroll = std::max(0.0f, static_cast<float>(duration) * pixels_per_second - visible_width);

            // Middle mouse drag for panning
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
                scroll_offset_x -= delta.x;
                scroll_offset_x = std::clamp(scroll_offset_x, 0.0f, max_scroll);
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);

                // Sync back to TimelineView so sliders stay in sync
                if (timeline_view) {
                    timeline_view->SetScrollOffset(scroll_offset_x);
                }
            }

            // Mouse wheel: regular wheel = zoom, Ctrl+wheel = pan (Blender style)
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0) {
                if (ImGui::GetIO().KeyCtrl) {
                    // Ctrl + mouse wheel = horizontal pan
                    scroll_offset_x -= wheel * S(50.0f);
                    scroll_offset_x = std::clamp(scroll_offset_x, 0.0f, max_scroll);
                } else {
                    // Regular scroll (no modifier) = zoom centered on playhead
                    float zoom_factor = 1.0f + wheel * 0.1f;
                    float old_pps = pixels_per_second;
                    pixels_per_second *= zoom_factor;
                    pixels_per_second = std::clamp(pixels_per_second,
                        OTIOTimeline::MIN_PIXELS_PER_SECOND,
                        OTIOTimeline::MAX_PIXELS_PER_SECOND);

                    // Keep PLAYHEAD position stable when zooming (not mouse position)
                    if (old_pps != pixels_per_second && timeline_manager) {
                        double playhead_time = timeline_manager->GetUIPosition();
                        // Calculate where playhead appears on screen before zoom
                        float playhead_screen_x = static_cast<float>(playhead_time) * old_pps - scroll_offset_x;
                        // Calculate new scroll to keep playhead at same screen position
                        scroll_offset_x = static_cast<float>(playhead_time) * pixels_per_second - playhead_screen_x;
                        // Recalculate max_scroll with new zoom level
                        max_scroll = std::max(0.0f, static_cast<float>(duration) * pixels_per_second - visible_width);
                        scroll_offset_x = std::clamp(scroll_offset_x, 0.0f, max_scroll);
                    }
                }

                // Sync back to TimelineView so sliders stay in sync
                if (timeline_view) {
                    timeline_view->SetZoomLevel(pixels_per_second);
                    timeline_view->SetScrollOffset(scroll_offset_x);
                }
            }
        }

        // Click on empty track area = deselect all
        if (tracks_hovered && ImGui::IsMouseClicked(0) && !timeline_clip_drag.active && !timeline_trim_state.active) {
            // Check if we clicked on empty space (not on a clip)
            bool clicked_on_clip = false;
            if (tracks_ptr && hover_track_index >= 0) {
                auto& track = (*tracks_ptr)[hover_track_index];
                for (const auto& clip : track.clips) {
                    if (clip.is_gap) continue;
                    double clip_end = clip.start_time + clip.duration;
                    if (mouse_time >= clip.start_time && mouse_time < clip_end) {
                        clicked_on_clip = true;
                        break;
                    }
                }
            }
            if (!clicked_on_clip && timeline_view) {
                timeline_view->GetSelection().Clear();
            }
        }

        // =====================================================================
        // KEYBOARD SHORTCUTS (when timeline panel is focused)
        // =====================================================================
        if (timeline_hovered && timeline_view && timeline_command_manager) {
            // Cut clips at playhead (split)
            if (IsShortcutPressed("tl_cut_clips")) {
                double playhead_time = timeline_manager ? timeline_manager->GetUIPosition() : 0.0;
                auto& selection = timeline_view->GetSelection();

                if (!selection.selected_clip_ids.empty()) {
                    // Cut only selected clips
                    auto composite = std::make_unique<qcview::CompositeCommand>("Cut Selected Clips at Playhead");
                    for (const auto& clip_id : selection.selected_clip_ids) {
                        int track_idx = timeline_view->GetTrackIndexForClip(clip_id);
                        qcview::OTIOClip* clip = timeline_view->FindClipById(clip_id);
                        if (clip && track_idx >= 0) {
                            double clip_end = clip->start_time + clip->duration;
                            if (playhead_time > clip->start_time && playhead_time < clip_end) {
                                composite->AddCommand(std::make_unique<qcview::CutClipCommand>(
                                    timeline_view.get(), clip_id, track_idx, playhead_time));
                            }
                        }
                    }
                    if (!composite->IsEmpty()) {
                        timeline_command_manager->Execute(std::move(composite));
                        Debug::Log("Cut selected clips at playhead: " + std::to_string(playhead_time));
                    }
                } else {
                    // No selection - cut all clips at playhead
                    auto clips_at_time = timeline_view->GetClipsAtTime(playhead_time);

                    if (!clips_at_time.empty()) {
                        auto composite = std::make_unique<qcview::CompositeCommand>("Cut Clips at Playhead");
                        for (auto* clip : clips_at_time) {
                            int track_idx = timeline_view->GetTrackIndexForClip(clip->id);
                            if (track_idx >= 0) {
                                composite->AddCommand(std::make_unique<qcview::CutClipCommand>(
                                    timeline_view.get(), clip->id, track_idx, playhead_time));
                            }
                        }
                        if (!composite->IsEmpty()) {
                            timeline_command_manager->Execute(std::move(composite));
                            Debug::Log("Cut clips at playhead: " + std::to_string(playhead_time));
                        }
                    }
                }
            }

            // Delete selected clips (also support Backspace as hardcoded fallback)
            if (IsShortcutPressed("tl_delete_clips") || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
                auto& selection = timeline_view->GetSelection();
                if (selection.HasSelection()) {
                    qcview::TimelineSourceMode source_mode = timeline_view->GetSourceMode();

                    if (source_mode == qcview::TimelineSourceMode::PLAYLIST) {
                        // PLAYLIST mode: use direct deletion (modifies source MediaItem)
                        std::vector<std::string> clips_to_delete(selection.selected_clip_ids.begin(),
                                                                  selection.selected_clip_ids.end());
                        for (const auto& clip_id : clips_to_delete) {
                            timeline_view->DeleteClipFromPlaylist(clip_id);
                        }
                        selection.Clear();
                        Debug::Log("Deleted " + std::to_string(clips_to_delete.size()) + " clips from playlist");
                    } else {
                        // OTIO/DUAL_VIEW mode: use command system for undo support
                        auto composite = std::make_unique<qcview::CompositeCommand>("Delete Selected Clips");
                        for (const auto& clip_id : selection.selected_clip_ids) {
                            int track_idx = timeline_view->GetTrackIndexForClip(clip_id);
                            if (track_idx >= 0) {
                                composite->AddCommand(std::make_unique<qcview::DeleteClipCommand>(
                                    timeline_view.get(), clip_id, track_idx));
                            }
                        }
                        if (!composite->IsEmpty()) {
                            timeline_command_manager->Execute(std::move(composite));
                            selection.Clear();
                            timeline_view->RecalculateDuration();
                            Debug::Log("Deleted selected clips");
                        }
                    }
                }
            }

            // Undo
            if (IsShortcutPressed("tl_undo")) {
                if (timeline_command_manager->CanUndo()) {
                    std::string desc = timeline_command_manager->GetUndoDescription();
                    timeline_command_manager->Undo();
                    timeline_view->RecalculateDuration();
                    Debug::Log("Undo: " + desc);
                }
            }

            // Redo
            if (IsShortcutPressed("tl_redo")) {
                if (timeline_command_manager->CanRedo()) {
                    std::string desc = timeline_command_manager->GetRedoDescription();
                    timeline_command_manager->Redo();
                    timeline_view->RecalculateDuration();
                    Debug::Log("Redo: " + desc);
                }
            }

            // Deselect all
            if (IsShortcutPressed("tl_deselect")) {
                timeline_view->GetSelection().Clear();
            }

            // NOTE: I, O, L shortcuts moved to GLOBAL OTIO SHORTCUTS section below

            // Toggle Follow Playhead
            if (IsShortcutPressed("tl_follow_playhead")) {
                follow_playhead = !follow_playhead;
                Debug::Log("Follow playhead: " + std::string(follow_playhead ? "enabled" : "disabled"));
            }

            // Zoom In (also support numpad + as hardcoded fallback)
            if (IsShortcutPressed("tl_zoom_in") ||
                (ImGui::IsKeyPressed(ImGuiKey_KeypadAdd) && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyAlt)) {
                float old_pps = pixels_per_second;
                pixels_per_second *= 1.25f;
                pixels_per_second = std::clamp(pixels_per_second,
                    OTIOTimeline::MIN_PIXELS_PER_SECOND,
                    OTIOTimeline::MAX_PIXELS_PER_SECOND);
                // Keep playhead centered when zooming
                if (old_pps != pixels_per_second) {
                    float zoom_ratio = pixels_per_second / old_pps;
                    float playhead_offset = static_cast<float>(position) * old_pps - scroll_offset_x;
                    scroll_offset_x = static_cast<float>(position) * pixels_per_second - playhead_offset;
                    scroll_offset_x = std::max(0.0f, scroll_offset_x);
                }
                timeline_view->SetZoomLevel(pixels_per_second);
                timeline_view->SetScrollOffset(scroll_offset_x);
            }

            // Zoom Out (also support numpad - as hardcoded fallback)
            if (IsShortcutPressed("tl_zoom_out") ||
                (ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract) && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyAlt)) {
                float old_pps = pixels_per_second;
                pixels_per_second /= 1.25f;
                pixels_per_second = std::clamp(pixels_per_second,
                    OTIOTimeline::MIN_PIXELS_PER_SECOND,
                    OTIOTimeline::MAX_PIXELS_PER_SECOND);
                // Keep playhead centered when zooming
                if (old_pps != pixels_per_second) {
                    float zoom_ratio = pixels_per_second / old_pps;
                    float playhead_offset = static_cast<float>(position) * old_pps - scroll_offset_x;
                    scroll_offset_x = static_cast<float>(position) * pixels_per_second - playhead_offset;
                    scroll_offset_x = std::max(0.0f, scroll_offset_x);
                }
                timeline_view->SetZoomLevel(pixels_per_second);
                timeline_view->SetScrollOffset(scroll_offset_x);
            }

            // Pan left/right
            if (IsShortcutPressed("tl_pan_left")) {
                scroll_offset_x -= S(100.0f);
                scroll_offset_x = std::max(0.0f, scroll_offset_x);
                timeline_view->SetScrollOffset(scroll_offset_x);
                follow_playhead = false;
            }
            if (IsShortcutPressed("tl_pan_right")) {
                scroll_offset_x += S(100.0f);
                timeline_view->SetScrollOffset(scroll_offset_x);
                follow_playhead = false;
            }
        }

        // =====================================================================
        // GLOBAL OTIO SHORTCUTS (work anywhere in OTIO mode, no hover required)
        // =====================================================================
        if (timeline_view && !ImGui::GetIO().WantTextInput) {
            auto* playback_ctrl = timeline_view->GetEffectivePlaybackController();

            // Set Timeline In Point (reuses same binding as main "set_in_point")
            if (IsShortcutPressed("set_in_point")) {
                double playhead_time = timeline_manager ? timeline_manager->GetUIPosition() : 0.0;
                timeline_view->SetTimelineInPoint(playhead_time);
                Debug::Log("Set In point: " + std::to_string(playhead_time) + "s");
            }

            // Set Timeline Out Point (reuses same binding as main "set_out_point")
            if (IsShortcutPressed("set_out_point")) {
                double playhead_time = timeline_manager ? timeline_manager->GetUIPosition() : 0.0;
                timeline_view->SetTimelineOutPoint(playhead_time);
                Debug::Log("Set Out point: " + std::to_string(playhead_time) + "s");
            }

            // Toggle Timeline Loop (reuses same binding as main "toggle_loop")
            if (IsShortcutPressed("toggle_loop")) {
                bool new_loop_state = !timeline_view->IsTimelineLooping();
                timeline_view->SetTimelineLooping(new_loop_state);
                Debug::Log("Timeline loop: " + std::string(new_loop_state ? "enabled" : "disabled"));
                // Save the setting
                cache_settings.loop_enabled = new_loop_state;
                SaveSettings();
            }
        }

        // Restore cursor position after interaction zones (which use SetCursorScreenPos)
        // Position cursor below the ruler which is at the bottom of the timeline
        ImGui::SetCursorScreenPos(ImVec2(
            tracks_start_pos.x,
            ruler_y + OTIOTimeline::TIMELINE_RULER_HEIGHT));

        // Add spacing between timeline and bottom controls
        ImGui::Spacing();

        // =====================================================================
        // VOLUME/MUTE/LOOP ROW (matching standard timeline panel)
        // =====================================================================
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(1.5f));
        ImGui::AlignTextToFramePadding();

        // Volume control on left
        if (font_regular) ImGui::PushFont(font_regular);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(1.7f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::Text("Volume:");
        ImGui::PopStyleColor();
        if (font_regular) ImGui::PopFont();
        ImGui::SameLine();

        // Mute button
        if (font_icons) {
            ImGui::PushFont(font_icons);
            const char* volume_icon = is_muted ? ICON_VOLUME_MUTE : ICON_VOLUME_UP;
            ImVec4 active_color = is_muted ? MutedLight(GetWindowsAccentColor()) : UI_WHITE_VEC4;

            float btn_size = ImGui::GetFrameHeight();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
            ImGui::PushStyleColor(ImGuiCol_Text, active_color);
            bool clicked = ImGui::Button(volume_icon, ImVec2(btn_size, btn_size));
            bool hovered = ImGui::IsItemHovered();
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar(2);

            ImGui::PopFont();

            if (clicked) {
                ToggleMute();
            }
            if (hovered) {
                ImGui::SetTooltip(is_muted ? "Unmute (M)" : "Mute (M)");
            }
        }

        ImGui::SameLine();

        // Volume slider - disabled when muted
        ImGui::SetNextItemWidth(S(120));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y + S(1)));
        ImGui::SetWindowFontScale(0.85f);
        ImGui::BeginDisabled(is_muted);
        if (ImGui::SliderInt("##otio_volume_slider", &current_volume, 0, 100, "%d%%")) {
            if (!is_muted) {
                // Set volume (routes to video or AudioMixer depending on mode)
                if (timeline_view) {
                    if (auto* ctrl = timeline_view->GetEffectivePlaybackController()) {
                        ctrl->SetVolume(current_volume / 100.0);
                    }
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleVar();

        // Show muted indicator
        if (is_muted) {
            ImGui::SameLine();
            if (font_regular) ImGui::PushFont(font_regular);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(1.7f));
            ImGui::TextColored(MutedLight(GetWindowsAccentColor()), "Muted");
            if (font_regular) ImGui::PopFont();
        }

        // Vertical divider between volume and loop
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(S(2), 0));
        ImGui::SameLine();
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float h = ImGui::GetFrameHeight();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + S(2)), ImVec2(p.x, p.y + h - S(2)), ImGui::GetColorU32(ImGuiCol_Separator));
            ImGui::Dummy(ImVec2(S(1), 0));
            ImGui::SameLine(0, S(12));
        }

        ImGui::Dummy(ImVec2(S(1), 0));
        ImGui::SameLine();

        // Loop toggle
        if (font_regular) ImGui::PushFont(font_regular);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(1.7f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::Text("Loop:");
        ImGui::PopStyleColor();
        if (font_regular) ImGui::PopFont();
        ImGui::SameLine();

        // Get loop state - use timeline_view in OTIO mode, else video_player
        bool loop_enabled = false;
        if (otio_timeline_mode && timeline_view) {
            loop_enabled = timeline_view->IsTimelineLooping();
        } else if (video_player) {
            loop_enabled = video_player->IsLooping();
        }

        // Simple tooltip without mode prefix
        const char* loop_tooltip = loop_enabled ? "Loop: ON (L)" : "Loop: OFF (L)";

        if (font_icons) {
            ImGui::PushFont(font_icons);
            const char* loop_icon = loop_enabled ? ICON_LOOP : ICON_LOOP_OFF;
            ImVec4 active_color = loop_enabled ? MutedLight(GetWindowsAccentColor()) : UI_WHITE_VEC4;

            float btn_size = ImGui::GetFrameHeight();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
            ImGui::PushStyleColor(ImGuiCol_Text, active_color);
            bool clicked = ImGui::Button(loop_icon, ImVec2(btn_size, btn_size));
            bool hovered = ImGui::IsItemHovered();
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar(2);

            ImGui::PopFont();

            if (clicked) {
                // In OTIO mode, toggle timeline loop directly
                // In other modes, use legacy ToggleLoop
                if (otio_timeline_mode && timeline_view) {
                    bool new_state = !timeline_view->IsTimelineLooping();
                    timeline_view->SetTimelineLooping(new_state);
                    Debug::Log("Timeline loop: " + std::string(new_state ? "enabled" : "disabled"));
                    // Save the setting
                    cache_settings.loop_enabled = new_state;
                    SaveSettings();
                } else {
                    ToggleLoop();
                    // Save the setting for legacy mode too
                    cache_settings.loop_enabled = video_player->IsLooping();
                    SaveSettings();
                }
            }
            if (hovered) {
                ImGui::SetTooltip("%s", loop_tooltip);
            }
        }

        // Spacer
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(S(1), 0));
        ImGui::SameLine();
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float h = ImGui::GetFrameHeight();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + S(2)), ImVec2(p.x, p.y + h - S(2)), ImGui::GetColorU32(ImGuiCol_Separator));
            ImGui::Dummy(ImVec2(S(1), 0));
            ImGui::SameLine(0, S(12));
        }

        ImGui::Dummy(ImVec2(S(1), 0));
        ImGui::SameLine();

        // Follow Playhead toggle button (F key shortcut) - next to Loop button
        ImGui::SameLine();
        if (font_regular) ImGui::PushFont(font_regular);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(1.7f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::Text("Options:");
        ImGui::PopStyleColor();
        if (font_regular) ImGui::PopFont();
        ImGui::SameLine();
        {
            ImVec4 active_color = follow_playhead ? MutedLight(GetWindowsAccentColor()) : UI_WHITE_VEC4;
            const char* follow_text = follow_playhead ? "F*" : "F";

            if (font_regular) ImGui::PushFont(font_regular);

            float btn_size = ImGui::GetFrameHeight();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
            ImGui::PushStyleColor(ImGuiCol_Text, active_color);
            bool clicked = ImGui::Button(follow_text, ImVec2(btn_size, btn_size));
            bool hovered = ImGui::IsItemHovered();
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar(2);

            if (font_regular) ImGui::PopFont();

            if (clicked) {
                follow_playhead = !follow_playhead;
                Debug::Log("Follow playhead: " + std::string(follow_playhead ? "enabled" : "disabled"));
            }
            if (hovered) {
                ImGui::SetTooltip("Follow Playhead (F)\nAuto-scroll to keep playhead visible during playback");
            }
        }

        // Frame Skip stride button (image sequences only) - next to F*
        {
            bool is_image_seq = false;
            int current_stride = 1;
            if (timeline_view && timeline_view->HasPlaybackController()) {
                auto* ctrl = timeline_view->GetEffectivePlaybackController();
                if (ctrl) {
                    auto* cache = ctrl->GetCache();
                    auto* right_cache = ctrl->GetRightCache();
                    is_image_seq = (cache && cache->IsDirectEXRCacheMode()) ||
                                   (right_cache && right_cache->IsDirectEXRCacheMode()) ||
                                   (timeline_view->GetSourceMode() == qcview::TimelineSourceMode::PLAYLIST);
                    current_stride = ctrl->GetPlaybackStride();
                }
            }

            if (is_image_seq) {
                ImGui::SameLine();
                ImGui::Dummy(ImVec2(S(1), 0));
                ImGui::SameLine();

                bool stride_active = (current_stride > 1);
                ImVec4 stride_color = stride_active ? MutedLight(GetWindowsAccentColor()) : UI_WHITE_VEC4;
                std::string stride_text = std::to_string(current_stride) + "x";

                if (font_regular) ImGui::PushFont(font_regular);

                float btn_size = ImGui::GetFrameHeight();
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
                ImGui::PushStyleColor(ImGuiCol_Text, stride_color);
                bool stride_clicked = ImGui::Button(stride_text.c_str(), ImVec2(btn_size, btn_size));
                bool stride_hovered = ImGui::IsItemHovered();
                ImGui::PopStyleColor(4);
                ImGui::PopStyleVar(2);

                // Right-click popup menu
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    ImGui::OpenPopup("FrameSkipPopup");
                }

                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
                ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0x12/255.0f, 0x12/255.0f, 0x12/255.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.1f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));

                if (ImGui::BeginPopup("FrameSkipPopup")) {
                    ImGui::TextDisabled("Playback Frame Skip");
                    ImGui::Separator();
                    auto* ctrl2 = timeline_view->GetEffectivePlaybackController();
                    const char* labels[] = { "Every Frame (Full)", "Every 2nd Frame", "Every 3rd Frame", "Every 4th Frame" };
                    for (int s = 1; s <= 4; s++) {
                        bool selected = (current_stride == s);
                        if (ImGui::MenuItem(labels[s - 1], nullptr, selected)) {
                            if (ctrl2) ctrl2->SetPlaybackStride(s);
                        }
                    }
                    ImGui::EndPopup();
                }

                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar(2);

                if (font_regular) ImGui::PopFont();

                if (stride_clicked) {
                    // Left click: cycle 1 -> 2 -> 3 -> 4 -> 1
                    int next = (current_stride % 4) + 1;
                    if (auto* ctrl = timeline_view->GetEffectivePlaybackController()) {
                        ctrl->SetPlaybackStride(next);
                    }
                }
                if (stride_hovered) {
                    std::string stride_label = current_stride == 1 ? "Full" : ("Every " + std::to_string(current_stride) + " frames");
                    ImGui::SetTooltip("Frame Skip: %s\nRight-click for options", stride_label.c_str());
                }
            }
        }

        // Spacer + separator before editing controls
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(S(1), 0));
        ImGui::SameLine();
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float h = ImGui::GetFrameHeight();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + S(2)), ImVec2(p.x, p.y + h - S(2)), ImGui::GetColorU32(ImGuiCol_Separator));
            ImGui::Dummy(ImVec2(S(1), 0));
            ImGui::SameLine(0, S(12));
        }

        ImGui::Dummy(ImVec2(S(1), 0));
        ImGui::SameLine();

        // === TIMELINE EDITING CONTROLS (same row, after F*) ===
        if (timeline_view) {
            double tl_current_time = timeline_manager ? timeline_manager->GetUIPosition() : 0.0;
            ImVec4 accent_color = GetWindowsAccentColor();

            ImGui::SameLine();
            if (font_regular) ImGui::PushFont(font_regular);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(1.7f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::Text("Set Range:");
            ImGui::PopStyleColor();
            if (font_regular) ImGui::PopFont();
            ImGui::SameLine();

            // In Point button
            if (font_icons) {
                ImGui::PushFont(font_icons);
                bool has_tl_in = timeline_view->HasTimelineInPoint();
                ImVec4 in_active_color = has_tl_in ? MutedLight(accent_color) : UI_WHITE_VEC4;

                float btn_size = ImGui::GetFrameHeight();
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
                ImGui::PushStyleColor(ImGuiCol_Text, in_active_color);
                bool in_clicked = ImGui::Button(ICON_LABEL, ImVec2(btn_size, btn_size));
                bool in_hovered = ImGui::IsItemHovered();
                ImGui::PopStyleColor(4);
                ImGui::PopStyleVar(2);

                if (in_clicked) {
                    timeline_view->SetTimelineInPoint(tl_current_time);
                }
                ImGui::PopFont();
                if (in_hovered) {
                    ImGui::SetTooltip("Set In Point (I)");
                }
            }

            ImGui::SameLine();

            // Out Point button
            if (font_icons) {
                ImGui::PushFont(font_icons);
                bool has_tl_out = timeline_view->HasTimelineOutPoint();
                ImVec4 out_active_color = has_tl_out ? MutedLight(accent_color) : UI_WHITE_VEC4;

                float btn_size = ImGui::GetFrameHeight();
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
                ImGui::PushStyleColor(ImGuiCol_Text, out_active_color);
                bool out_clicked = ImGui::Button(ICON_LABEL_IMPORTANT, ImVec2(btn_size, btn_size));
                bool out_hovered = ImGui::IsItemHovered();
                ImGui::PopStyleColor(4);
                ImGui::PopStyleVar(2);

                if (out_clicked) {
                    timeline_view->SetTimelineOutPoint(tl_current_time);
                }
                ImGui::PopFont();
                if (out_hovered) {
                    ImGui::SetTooltip("Set Out Point (O)");
                }
            }

            // Clear In/Out button (between Out and Cut, only show when at least one is set)
            if (timeline_view->HasTimelineInPoint() || timeline_view->HasTimelineOutPoint()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear##tl_in_out")) {
                    timeline_view->ClearTimelineInOutPoints();
                    Debug::Log("Cleared timeline In/Out points");
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Clear In/Out points");
                }
            }

            // Refresh section: only show for IMAGE_SEQUENCE mode (useful for reloading changed frames)
            bool is_image_sequence_mode = timeline_view && timeline_view->GetSourceMode() == qcview::TimelineSourceMode::IMAGE_SEQUENCE;
            if (is_image_sequence_mode) {
                ImGui::SameLine();

                // Spacer
                ImGui::Dummy(ImVec2(S(1), 0));
                ImGui::SameLine();
                {
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    float h = ImGui::GetFrameHeight();
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + S(2)), ImVec2(p.x, p.y + h - S(2)), ImGui::GetColorU32(ImGuiCol_Separator));
                    ImGui::Dummy(ImVec2(S(1), 0));
                    ImGui::SameLine(0, S(12));
                }

                // Refresh
                if (font_regular) ImGui::PushFont(font_regular);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(1.7f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::Text("Refresh:");
                ImGui::PopStyleColor();
                if (font_regular) ImGui::PopFont();
                ImGui::SameLine();

                // Refresh Cache button
                if (font_icons) {
                    ImGui::PushFont(font_icons);

                    float btn_size = ImGui::GetFrameHeight();
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
                    bool refresh_clicked = ImGui::Button(ICON_REFRESH, ImVec2(btn_size, btn_size));
                    bool refresh_hovered = ImGui::IsItemHovered();
                    ImGui::PopStyleColor(3);
                    ImGui::PopStyleVar(2);

                    if (refresh_clicked) {
                        timeline_view->ForceRefreshCache();
                    }
                    ImGui::PopFont();
                    if (refresh_hovered) {
                        ImGui::SetTooltip("Refresh Cache (reload changed frames)");
                    }
                }
            }

            // === FRAME/TIMECODE COUNTER (RIGHT ALIGNED) ===
            ImGui::SameLine();

            // Calculate current frame from timeline position
            double fps = timeline_view->GetFrameRate();
            if (fps <= 0) fps = 24.0;
            int current_frame = static_cast<int>(std::round(position * fps));
            int total_frames = static_cast<int>(std::round(duration * fps));
            // Clamp to valid frame range (0 to total_frames-1)
            if (current_frame >= total_frames) current_frame = total_frames - 1;
            if (current_frame < 0) current_frame = 0;

            // Format timecode and frame string
            std::string time_display = FormatCurrentTimecodeWithOffset(position);
            // Calculate display frame - use offset frame when in timecode mode
            int display_frame = current_frame + 1;  // 1-based frame numbering for display
            bool tc_in_timecode_mode = timecode_mode_enabled && timecode_state == AVAILABLE;
            if (tc_in_timecode_mode) {
                // Calculate frame with embedded timecode offset
                double start_offset = ParseTimecodeToSeconds(cached_start_timecode, fps);
                display_frame = static_cast<int>(std::round((position + start_offset) * fps)) + 1;
            }
            char frame_buf[16];
            snprintf(frame_buf, sizeof(frame_buf), "%04d", display_frame);
            std::string frame_str = time_display + " Frame " + frame_buf;

            // Calculate position for right-aligned text (with button)
            if (font_mono) ImGui::PushFont(font_mono);
            ImVec2 text_size = ImGui::CalcTextSize(frame_str.c_str());
            float button_width = S(25.0f);
            // Check if this is a video file (only videos can have embedded timecode)
            bool is_video_source = timeline_view &&
                timeline_view->GetSourceMode() == qcview::TimelineSourceMode::VIDEO_FILE;
            // Account for Go To button, plus Timecode Mode button if video
            float total_width = text_size.x + button_width + (is_video_source ? button_width : 0.0f) + S(5.0f);
            float vertical_padding = S(1.1f);

            // Right-align the frame counter
            float window_width = ImGui::GetWindowSize().x;
            float current_x = ImGui::GetCursorPosX();
            float target_x = window_width - total_width - S(15.0f);
            if (target_x > current_x) {
                ImGui::SetCursorPosX(target_x);
            }
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + vertical_padding);

            // Show in accent color when in timecode mode
            if (tc_in_timecode_mode) {
                ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s", frame_str.c_str());
            } else {
                ImGui::Text("%s", frame_str.c_str());
            }
            if (font_mono) ImGui::PopFont();

            // Go To button next to timecode
            ImGui::SameLine();

            if (font_icons) {
                ImGui::PushFont(font_icons);
                float btn_size = ImGui::GetFrameHeight();
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
                if (ImGui::Button("\uf50b##otio_goto", ImVec2(btn_size, btn_size))) {  // "play_for_work" icon
                    OpenGotoTimecodeModal();
                }
                bool hovered = ImGui::IsItemHovered();
                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar(2);
                ImGui::PopFont();

                if (hovered) {
                    ImGui::SetTooltip("Go to timecode/frame");
                }
            }

            // Timecode Mode button (only for video files that can have embedded timecode)
            if (is_video_source) {
                ImGui::SameLine();
                if (font_icons) {
                    ImGui::PushFont(font_icons);
                    float btn_size = ImGui::GetFrameHeight();
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
                    // Accent color text when timecode mode is active
                    bool tc_active = timecode_mode_enabled && timecode_state == AVAILABLE;
                    if (tc_active) {
                        ImGui::PushStyleColor(ImGuiCol_Text, Bright(GetWindowsAccentColor()));
                    }

                    if (ImGui::Button(ICON_TIMECODE "##otio_main_tcmode", ImVec2(btn_size, btn_size))) {
                        ImGui::OpenPopup("##main_timecode_mode_popup");
                    }
                    bool tcmode_hovered = ImGui::IsItemHovered();

                    if (tc_active) {
                        ImGui::PopStyleColor();
                    }
                    ImGui::PopStyleColor(3);
                    ImGui::PopStyleVar(2);
                    ImGui::PopFont();

                    // Timecode mode popup menu - style with padding, dark background, and border
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
                    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
                    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0x1A/255.0f, 0x1A/255.0f, 0x1A/255.0f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.1f));
                    if (ImGui::BeginPopup("##main_timecode_mode_popup")) {
                        // Check timecode availability
                        if (timecode_state == NOT_CHECKED) {
                            CheckStartTimecodeAvailability();
                        }

                        // Calculate both frame numbers for display
                        int playback_frame = current_frame + 1;  // Regular 1-based frame
                        double start_offset = ParseTimecodeToSeconds(cached_start_timecode, fps);
                        int embedded_frame = static_cast<int>(std::round((position + start_offset) * fps)) + 1;

                        // Regular timecode section
                        ImGui::TextDisabled("Playback Time");
                        std::string regular_tc = FormatRegularTimecode(position);
                        ImGui::Text("  %s  Frame %04d", regular_tc.c_str(), playback_frame);

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        // Embedded timecode section
                        ImGui::TextDisabled("Embedded Timecode");
                        if (timecode_state == AVAILABLE) {
                            std::string offset_tc = FormatOffsetTimecode(position);
                            ImGui::Text("  %s  Frame %04d", offset_tc.c_str(), embedded_frame);
                        } else if (timecode_state == CHECKING) {
                            ImGui::TextDisabled("  Checking...");
                        } else {
                            ImGui::TextDisabled("  Not available");
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        // Toggle option
                        if (timecode_state == AVAILABLE) {
                            bool mode_on = timecode_mode_enabled;
                            if (ImGui::Checkbox("Show Embedded Timecode", &mode_on)) {
                                timecode_mode_enabled = mode_on;
                            }
                        } else {
                            ImGui::BeginDisabled();
                            bool disabled_mode = false;
                            ImGui::Checkbox("Show Embedded Timecode", &disabled_mode);
                            ImGui::EndDisabled();
                        }

                        ImGui::EndPopup();
                    }
                    ImGui::PopStyleColor(2);
                    ImGui::PopStyleVar(2);

                    if (tcmode_hovered) {
                        ImGui::SetTooltip("Timecode Mode\nSwitch between playback time and embedded timecode");
                    }
                }
            }
        }

        // Bottom padding below the controls row
        ImGui::Dummy(ImVec2(0, S(30.0f)));

        // =====================================================================
        // LINK MEDIA POPUP
        // =====================================================================
        if (show_link_media_popup) {
            ImGui::OpenPopup("Link Media##popup");
            show_link_media_popup = false;
        }

        // Center the modal
        float scale = ImGui::GetIO().FontGlobalScale;
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(S(520), 0), ImGuiCond_Always);

        // Push consistent window padding for modal
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16.0f), S(16.0f)));

        if (ImGui::BeginPopupModal("Link Media##popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Link timeline clips to media files by searching a directory.");
            ImGui::Separator();
            ImGui::Spacing();

            // Search directory input
            ImGui::Text("Search Directory:");
            ImGui::Spacing();

            // Calculate browse button width
            float browse_width = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2 + 8;
            float input_width = ImGui::GetContentRegionAvail().x - browse_width - ImGui::GetStyle().ItemSpacing.x;

            char path_buf[512];
            strncpy(path_buf, link_media_search_path.c_str(), sizeof(path_buf) - 1);
            path_buf[sizeof(path_buf) - 1] = '\0';

            ImGui::SetNextItemWidth(input_width);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            if (ImGui::InputText("##link_path", path_buf, sizeof(path_buf))) {
                link_media_search_path = path_buf;
            }
            ImGui::SameLine();
            PushOutlineButtonStyle();
            if (ImGui::Button("Browse...", ImVec2(browse_width, 0))) {
                extern bool g_nfd_dialog_open;
                if (!g_nfd_dialog_open) {
                    g_nfd_dialog_open = true;
                    nfdu8char_t* out_path = nullptr;
                    nfdresult_t result = NFD_PickFolderU8(&out_path, nullptr);
                    ImGui::GetIO().ClearInputKeys();
                    g_nfd_dialog_open = false;
                    if (result == NFD_OKAY && out_path) {
                        link_media_search_path = out_path;
                        NFD_FreePathU8(out_path);
                    }
                }
            }
            PopOutlineButtonStyle();

            ImGui::Spacing();
            ImGui::Spacing();

            // Linking options
            ImGui::Text("Options:");
            ImGui::Spacing();
            static qcview::LinkOptions link_options;
            ImGui::Checkbox("Recursive search (include subdirectories)", &link_options.recursive);
            ImGui::Checkbox("Fuzzy matching (ignore .new.##, _v001, etc.)", &link_options.fuzzy_match);

            ImGui::Spacing();
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Action buttons (flush right)
            float btnPadding = S(8.0f) * 2;
            float linkW = ImGui::CalcTextSize("Link Media").x + btnPadding;
            float cancelW = ImGui::CalcTextSize("Cancel").x + btnPadding;
            float btnSpacing = ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - linkW - cancelW - btnSpacing);

            bool can_link = !link_media_search_path.empty() && timeline_view;
            PushOutlineButtonStyle();
            if (!can_link) ImGui::BeginDisabled();
            if (ImGui::Button("Link Media")) {
                // Create media linker if needed
                if (!media_linker) {
                    media_linker = std::make_unique<qcview::MediaLinker>();
                }

                // Perform linking
                auto& tracks = timeline_view->GetTracks();
                last_link_summary = media_linker->LinkMediaInDirectory(tracks, link_media_search_path, link_options);

                // Update flattener and invalidate cache (media changed)
                timeline_view->SyncFlattenerAndInvalidate();

                // Show results
                show_link_results_popup = true;
                ImGui::CloseCurrentPopup();
            }
            if (!can_link) ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }
            PopOutlineButtonStyle();

            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();  // WindowPadding

        // Link Results popup
        if (show_link_results_popup) {
            ImGui::OpenPopup("Link Results##popup");
            show_link_results_popup = false;
        }

        // Center the results modal
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        // Push consistent window padding for modal
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16.0f), S(16.0f)));

        if (ImGui::BeginPopupModal("Link Results##popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Media Linking Complete");
            ImGui::Separator();
            ImGui::Spacing();

            // Summary with icons/colors
            ImGui::Text("Total clips:  %d", last_link_summary.total_clips);
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Linked:       %d", last_link_summary.linked_count);
            if (last_link_summary.unlinked_count > 0) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Unlinked:     %d", last_link_summary.unlinked_count);
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Unlinked:     0");
            }

            ImGui::Spacing();

            // Show unlinked clips if any
            if (last_link_summary.unlinked_count > 0) {
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::Text("Unlinked clips:");
                ImGui::Spacing();

                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.1f, 0.1f, 0.1f, 1.0f));
                ImGui::BeginChild("UnlinkedList", ImVec2(S(420), S(150)), true);
                for (const auto& result : last_link_summary.results) {
                    if (!result.success) {
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "  %s", result.clip_name.c_str());
                    }
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // OK button (flush right)
            float btnPadding = S(8.0f) * 2;
            float okW = ImGui::CalcTextSize("OK").x + btnPadding;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - okW);
            PushOutlineButtonStyle();
            if (ImGui::Button("OK")) {
                // Save links to cache for persistence across timeline reloads
                if (last_link_summary.linked_count > 0) {
                    SaveLinksToCache(auto_relink_timeline_id);
                }

                // Initialize timeline playback if linking was successful
                if (last_link_summary.linked_count > 0 && timeline_view && video_player) {
                    // Update flattener with linked tracks (it has its own copy)
                    timeline_view->GetFlattener().SetTracks(timeline_view->GetTracks());

                    if (!timeline_view->HasPlaybackController()) {
                        if (timeline_view->InitializePlayback()) {
                            Debug::Log("Timeline playback initialized after media linking");
                            // Enable timeline mode in VideoPlayer for smooth EXR-style frame injection
                            video_player->SetTimelineMode(true, timeline_view->GetPlaybackController());
                            // Ensure timeline starts paused (don't auto-play when switching)
                            timeline_view->GetPlaybackController()->Pause();
                            timeline_view->SetTimelineLooping(cache_settings.loop_enabled);

                            // Apply audio sync settings
                            if (auto* mixer = timeline_view->GetPlaybackController()->GetAudioMixer()) {
                                const float preset_latencies_ms[] = { 33.0f, 27.0f, 17.0f, 14.0f, 8.0f, 0.0f };
                                float latency_ms = (cache_settings.display_latency_preset == 5)
                                    ? cache_settings.custom_display_latency_ms
                                    : preset_latencies_ms[cache_settings.display_latency_preset];
                                mixer->SetDisplayLatency(latency_ms / 1000.0);
                                mixer->SetFineTuneOffset(cache_settings.audio_fine_tune_ms / 1000.0);

                                // Set pipeline latency based on source mode (D3D11 video decoding adds ~28ms)
                                qcview::TimelineSourceMode source_mode = timeline_view->GetSourceMode();
                                if (source_mode == qcview::TimelineSourceMode::VIDEO_FILE ||
                                    source_mode == qcview::TimelineSourceMode::PLAYLIST) {
                                    mixer->SetPipelineLatency(0.028);  // D3D11 decoder buffering
                                } else {
                                    mixer->SetPipelineLatency(0.0);
                                }
                            }
                        } else {
                            Debug::Log("Failed to initialize timeline playback");
                        }
                    } else {
                        // Playback controller already exists - ensure timeline mode is enabled
                        video_player->SetTimelineMode(true, timeline_view->GetPlaybackController());
                    }
                }
                ImGui::CloseCurrentPopup();
            }
            PopOutlineButtonStyle();

            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();  // WindowPadding


        // =====================================================================
        // AUTO-RELINK RESULTS POPUP (shown after automatic relink on timeline load)
        // =====================================================================
        if (show_auto_relink_results) {
            ImGui::OpenPopup("Auto-Relink Results##popup");
            show_auto_relink_results = false;
        }

        // Center the results modal
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16.0f), S(16.0f)));

        if (ImGui::BeginPopupModal("Auto-Relink Results##popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Automatic Media Linking Complete");
            ImGui::TextDisabled("(Searched 6 folders deep from timeline location)");
            ImGui::Separator();
            ImGui::Spacing();

            // Summary with icons/colors
            ImGui::Text("Total clips:  %d", last_link_summary.total_clips);
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Linked:       %d", last_link_summary.linked_count);
            if (last_link_summary.unlinked_count > 0) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Unlinked:     %d", last_link_summary.unlinked_count);
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Unlinked:     0");
            }

            ImGui::Spacing();

            // Show unlinked clips if any
            if (last_link_summary.unlinked_count > 0) {
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::Text("Unlinked clips:");
                ImGui::Spacing();

                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.1f, 0.1f, 0.1f, 1.0f));
                ImGui::BeginChild("UnlinkedListAuto", ImVec2(S(420), S(150)), true);
                for (const auto& result : last_link_summary.results) {
                    if (!result.success) {
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "  %s", result.clip_name.c_str());
                    }
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Links will be saved with your project.");
            ImGui::Separator();
            ImGui::Spacing();

            // OK button (flush right)
            float btnPadding = S(8.0f) * 2;
            float okW = ImGui::CalcTextSize("OK").x + btnPadding;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - okW);

            PushOutlineButtonStyle();
            if (ImGui::Button("OK")) {
                // Initialize timeline playback if linking was successful
                if (last_link_summary.linked_count > 0 && timeline_view && video_player) {
                    if (!timeline_view->HasPlaybackController()) {
                        if (timeline_view->InitializePlayback()) {
                            Debug::Log("Timeline playback initialized after auto-relink");
                            video_player->SetTimelineMode(true, timeline_view->GetPlaybackController());
                            // Ensure timeline starts paused (don't auto-play when switching)
                            timeline_view->GetPlaybackController()->Pause();
                            timeline_view->SetTimelineLooping(cache_settings.loop_enabled);

                            // Apply audio sync settings
                            if (auto* mixer = timeline_view->GetPlaybackController()->GetAudioMixer()) {
                                const float preset_latencies_ms[] = { 33.0f, 27.0f, 17.0f, 14.0f, 8.0f, 0.0f };
                                float latency_ms = (cache_settings.display_latency_preset == 5)
                                    ? cache_settings.custom_display_latency_ms
                                    : preset_latencies_ms[cache_settings.display_latency_preset];
                                mixer->SetDisplayLatency(latency_ms / 1000.0);
                                mixer->SetFineTuneOffset(cache_settings.audio_fine_tune_ms / 1000.0);

                                // Set pipeline latency based on source mode (D3D11 video decoding adds ~28ms)
                                qcview::TimelineSourceMode source_mode = timeline_view->GetSourceMode();
                                if (source_mode == qcview::TimelineSourceMode::VIDEO_FILE ||
                                    source_mode == qcview::TimelineSourceMode::PLAYLIST) {
                                    mixer->SetPipelineLatency(0.028);  // D3D11 decoder buffering
                                } else {
                                    mixer->SetPipelineLatency(0.0);
                                }
                            }
                        }
                    } else {
                        video_player->SetTimelineMode(true, timeline_view->GetPlaybackController());
                    }
                }
                ImGui::CloseCurrentPopup();
            }
            PopOutlineButtonStyle();

            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();  // WindowPadding

        // =====================================================================
        // CLIP CONTEXT MENU (right-click)
        // =====================================================================
        if (show_clip_context_menu) {
            ImGui::OpenPopup("ClipContextMenu##popup");
            show_clip_context_menu = false;
        }

        // Style the context menu with dark background (matching playlist context menu)
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.065f, 0.065f, 0.065f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8.0f), S(8.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8.0f), S(6.0f)));

        if (ImGui::BeginPopup("ClipContextMenu##popup")) {
            // Find the clicked clip
            qcview::OTIOClip* clicked_clip = nullptr;
            if (tracks_ptr && right_clicked_track_index >= 0 &&
                right_clicked_track_index < static_cast<int>(tracks_ptr->size())) {
                auto& track = (*tracks_ptr)[right_clicked_track_index];
                for (auto& clip : track.clips) {
                    if (clip.id == right_clicked_clip_id) {
                        clicked_clip = &clip;
                        break;
                    }
                }
            }

            if (clicked_clip) {
                // Truncate long clip names for header (disabled/dimmed style)
                std::string display_name = clicked_clip->name;
                if (display_name.length() > 40) {
                    display_name = display_name.substr(0, 37) + "...";
                }
                ImGui::TextDisabled("%s", display_name.c_str());
                ImGui::Separator();

                // Check if we're in PLAYLIST or DUAL_VIEW mode (simplified menu)
                qcview::TimelineSourceMode source_mode = timeline_view ? timeline_view->GetSourceMode() : qcview::TimelineSourceMode::VIDEO_FILE;
                bool is_playlist_or_dual = (source_mode == qcview::TimelineSourceMode::PLAYLIST) ||
                                           (source_mode == qcview::TimelineSourceMode::DUAL_VIEW);

                if (is_playlist_or_dual) {
                    // === SIMPLIFIED MENU FOR PLAYLIST/DUAL_VIEW ===
                    // These modes have pre-linked media from project manager

                    // Reveal in file manager
                    if (!clicked_clip->linked_path.empty()) {
                        const char* reveal_label =
#ifdef _WIN32
                            "Reveal in Explorer";
#elif defined(__APPLE__)
                            "Reveal in Finder";
#else
                            "Reveal in File Browser";
#endif
                        if (ImGui::MenuItem(reveal_label)) {
                            if (project_manager) {
                                project_manager->ShowInExplorer(clicked_clip->linked_path);
                            }
                        }
                    }

                    // Reveal in Project - find and select the media item in project manager
                    if (ImGui::MenuItem("Reveal in Project")) {
                        if (project_manager && !clicked_clip->linked_path.empty()) {
                            // Find the MediaItem by path and select it
                            qcview::MediaItem* media_item = project_manager->FindMediaItemByPath(clicked_clip->linked_path);
                            if (media_item) {
                                project_manager->SelectMediaItem(media_item->id, false, false);
                                Debug::Log("Revealed clip in project: " + media_item->name);
                            } else {
                                Debug::Log("Could not find media item for path: " + clicked_clip->linked_path);
                            }
                        }
                    }

                    ImGui::Separator();

                    // Delete
                    if (ImGui::MenuItem("Delete")) {
                        if (timeline_view && right_clicked_track_index >= 0) {
                            // For PLAYLIST mode, use the playlist-specific delete
                            if (source_mode == qcview::TimelineSourceMode::PLAYLIST) {
                                timeline_view->DeleteClipFromPlaylist(right_clicked_clip_id);
                                Debug::Log("Deleted clip from playlist");
                            } else {
                                // For DUAL_VIEW, use the command system
                                if (timeline_command_manager) {
                                    auto cmd = std::make_unique<qcview::DeleteClipCommand>(
                                        timeline_view.get(), right_clicked_clip_id, right_clicked_track_index);
                                    timeline_command_manager->Execute(std::move(cmd));
                                }
                                Debug::Log("Deleted clip from dual view");
                            }

                            // Clear selection if deleted clip was selected
                            auto& selection = timeline_view->GetSelection();
                            if (selection.IsSelected(right_clicked_clip_id)) {
                                selection.DeselectClip(right_clicked_clip_id);
                            }
                            timeline_view->RecalculateDuration();
                        }
                    }
                } else {
                    // === FULL MENU FOR OTIO TIMELINE EDITING ===

                    // Link/Relink option
                    const char* link_label = clicked_clip->is_linked ? "Relink to File..." : "Link to File...";
                    if (ImGui::MenuItem(link_label)) {
                        extern bool g_nfd_dialog_open;
                        if (!g_nfd_dialog_open) {
                        g_nfd_dialog_open = true;
                        nfdu8char_t* out_path = nullptr;
                        nfdfilteritem_t filters[2] = {
                            { "Media Files", "mov,mp4,mxf,avi,mkv,exr,dpx,tif,tiff,png,jpg,jpeg" },
                            { "All Files", "*" }
                        };
                        nfdresult_t result = NFD_OpenDialogU8(&out_path, filters, 2, nullptr);
                        ImGui::GetIO().ClearInputKeys();
                        g_nfd_dialog_open = false;
                        if (result == NFD_OKAY && out_path) {
                            if (!media_linker) {
                                media_linker = std::make_unique<qcview::MediaLinker>();
                            }
                            if (media_linker->LinkClip(*clicked_clip, out_path)) {
                                Debug::Log("Manually linked clip '" + clicked_clip->name + "' to " + std::string(out_path));
                                SaveLinksToCache(auto_relink_timeline_id);
                                if (timeline_view) {
                                    timeline_view->SyncFlattenerAndInvalidate();
                                }
                            }
                            NFD_FreePathU8(out_path);
                        }
                        } // g_nfd_dialog_open guard
                    }

                    if (clicked_clip->is_linked) {
                        if (ImGui::MenuItem("Unlink Media")) {
                            if (!media_linker) {
                                media_linker = std::make_unique<qcview::MediaLinker>();
                            }
                            media_linker->UnlinkClip(*clicked_clip);
                            Debug::Log("Unlinked clip '" + clicked_clip->name + "'");
                            if (timeline_view) {
                                timeline_view->SyncFlattenerAndInvalidate();
                            }
                        }

                        ImGui::Separator();

                        if (ImGui::MenuItem(
#ifdef _WIN32
                            "Reveal in Explorer"
#elif defined(__APPLE__)
                            "Reveal in Finder"
#else
                            "Reveal in File Browser"
#endif
                        )) {
                            if (project_manager) {
                                project_manager->ShowInExplorer(clicked_clip->linked_path);
                            }
                        }

                        ImGui::Separator();
                        std::string path_display = clicked_clip->linked_path;
                        if (path_display.length() > 50) {
                            path_display = "..." + path_display.substr(path_display.length() - 47);
                        }
                        ImGui::TextDisabled("%s", path_display.c_str());
                    }

                    // Open in Project option (only for linked clips)
                    if (clicked_clip->is_linked && !clicked_clip->linked_path.empty()) {
                        ImGui::Separator();
                        if (ImGui::MenuItem("Open in Project")) {
                            if (project_manager) {
                                std::string path_to_open = clicked_clip->linked_path;

                                if (path_to_open.length() > 5 && path_to_open.substr(0, 5) == "mf://") {
                                    std::string path_part = path_to_open.substr(5);
                                    size_t fps_pos = path_part.find(":fps=");
                                    if (fps_pos != std::string::npos) {
                                        path_part = path_part.substr(0, fps_pos);
                                    }
                                    path_to_open = path_part;
                                } else if (path_to_open.length() > 6 && path_to_open.substr(0, 6) == "exr://") {
                                    std::string path_part = path_to_open.substr(6);
                                    size_t query_pos = path_part.find('?');
                                    if (query_pos != std::string::npos) {
                                        path_part = path_part.substr(0, query_pos);
                                    }
                                    path_to_open = path_part;
                                }

                                Debug::Log("Opening clip in project: " + path_to_open);
                                project_manager->LoadSingleFileFromDrop(path_to_open);
                            }
                        }
                    }

                    // Delete option (always available)
                    ImGui::Separator();
                    if (ImGui::MenuItem("Delete")) {
                        if (timeline_view && timeline_command_manager && right_clicked_track_index >= 0) {
                            auto cmd = std::make_unique<qcview::DeleteClipCommand>(
                                timeline_view.get(), right_clicked_clip_id, right_clicked_track_index);
                            timeline_command_manager->Execute(std::move(cmd));

                            auto& selection = timeline_view->GetSelection();
                            if (selection.IsSelected(right_clicked_clip_id)) {
                                selection.DeselectClip(right_clicked_clip_id);
                            }

                            timeline_view->RecalculateDuration();
                            Debug::Log("Deleted clip from context menu");
                        }
                    }
                }
            }

            ImGui::EndPopup();
        }

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        // =====================================================================
        // TRACK HEADER CONTEXT MENU (right-click on track header)
        // =====================================================================
        if (show_track_context_menu) {
            ImGui::OpenPopup("TrackContextMenu##popup");
            show_track_context_menu = false;
        }

        // Style the context menu with dark background
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.065f, 0.065f, 0.065f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8.0f), S(8.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8.0f), S(6.0f)));

        if (ImGui::BeginPopup("TrackContextMenu##popup")) {
            if (tracks_ptr && right_clicked_track_header_index >= 0 &&
                right_clicked_track_header_index < static_cast<int>(tracks_ptr->size())) {
                qcview::OTIOTrack& clicked_track = (*tracks_ptr)[right_clicked_track_header_index];
                bool is_video_track = clicked_track.is_video;

                // Header showing track name
                ImGui::Text("%s", clicked_track.name.c_str());
                ImGui::Separator();

                // Add track options
                if (is_video_track) {
                    if (ImGui::MenuItem("Add Video Track Above")) {
                        if (timeline_view) {
                            timeline_view->AddVideoTrack(right_clicked_track_header_index);
                        }
                    }
                    if (ImGui::MenuItem("Add Video Track Below")) {
                        if (timeline_view) {
                            timeline_view->AddVideoTrack(right_clicked_track_header_index + 1);
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Add Audio Track")) {
                        if (timeline_view) {
                            timeline_view->AddAudioTrack();
                        }
                    }
                } else {
                    if (ImGui::MenuItem("Add Audio Track Above")) {
                        if (timeline_view) {
                            timeline_view->AddAudioTrack(right_clicked_track_header_index);
                        }
                    }
                    if (ImGui::MenuItem("Add Audio Track Below")) {
                        if (timeline_view) {
                            timeline_view->AddAudioTrack(right_clicked_track_header_index + 1);
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Add Video Track")) {
                        if (timeline_view) {
                            timeline_view->AddVideoTrack();
                        }
                    }
                }

                ImGui::Separator();

                // Delete track option (disabled if last of type)
                bool can_delete = timeline_view && timeline_view->CanDeleteTrack(right_clicked_track_header_index);
                if (ImGui::MenuItem("Delete Track", nullptr, false, can_delete)) {
                    if (timeline_view) {
                        timeline_view->DeleteTrack(right_clicked_track_header_index);
                    }
                }
                if (!can_delete && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("Cannot delete the last %s track", is_video_track ? "video" : "audio");
                }
            }

            ImGui::EndPopup();
        }

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        // =====================================================================
        // EMPTY AREA CONTEXT MENU (right-click below all tracks)
        // =====================================================================
        if (show_empty_area_context_menu) {
            ImGui::OpenPopup("EmptyAreaContextMenu##popup");
            show_empty_area_context_menu = false;
        }

        // Style the context menu with dark background
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.065f, 0.065f, 0.065f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8.0f), S(8.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8.0f), S(6.0f)));

        if (ImGui::BeginPopup("EmptyAreaContextMenu##popup")) {
            ImGui::Text("Add Track");
            ImGui::Separator();

            if (ImGui::MenuItem("Add Video Track")) {
                if (timeline_view) {
                    timeline_view->AddVideoTrack();
                }
            }

            if (ImGui::MenuItem("Add Audio Track")) {
                if (timeline_view) {
                    timeline_view->AddAudioTrack();
                }
            }

            ImGui::EndPopup();
        }

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }
