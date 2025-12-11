#include "timeline_view.h"
#include "timeline_playback_controller.h"
#include "timeline_cache.h"
#include "edl_parser.h"
#include "../utils/debug_utils.h"
#include <imgui.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <filesystem>
#include <ctime>
#include <cmath>

// External font from main.cpp for consistent styling
extern ImFont* font_mono;

// Helper to get Windows accent color (matching main.cpp implementation)
extern ImVec4 GetWindowsAccentColor();

// OTIO includes - only when library is available
#ifdef USE_OPENTIMELINEIO
#include <opentimelineio/timeline.h>
#include <opentimelineio/clip.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/track.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/transition.h>
#include <opentimelineio/externalReference.h>
#include <opentimelineio/imageSequenceReference.h>
#include <opentimelineio/serializableObjectWithMetadata.h>
#include <opentimelineio/deserialization.h>
#include <opentimelineio/errorStatus.h>
namespace otio = opentimelineio::OPENTIMELINEIO_VERSION;
#endif

namespace ump {

// ============================================================================
// TimelineFlattener Implementation
// ============================================================================

void TimelineFlattener::SetTracks(const std::vector<OTIOTrack>& tracks) {
    tracks_ = tracks;
    InvalidateCache();

    // Debug: Log track and clip info
    int linked_count = 0;
    int total_clips = 0;
    for (const auto& track : tracks_) {
        for (const auto& clip : track.clips) {
            if (!clip.is_gap) {
                total_clips++;
                if (clip.is_linked) {
                    linked_count++;
                }
            }
        }
    }
    Debug::Log("Flattener::SetTracks: " + std::to_string(linked_count) + "/" +
               std::to_string(total_clips) + " clips linked");
}

void TimelineFlattener::SetTrackVisibility(const std::string& track_id, bool visible) {
    for (auto& track : tracks_) {
        if (track.id == track_id) {
            track.visible = visible;
            InvalidateCache();
            break;
        }
    }
}

void TimelineFlattener::SetTrackMute(const std::string& track_id, bool muted) {
    for (auto& track : tracks_) {
        if (track.id == track_id) {
            track.muted = muted;
            // Audio doesn't need cache invalidation (separate from video)
            break;
        }
    }
}

const OTIOClip* TimelineFlattener::FindClipInTrack(const OTIOTrack& track, double timestamp) {
    // First pass: look for non-gap clips (real content)
    for (const auto& clip : track.clips) {
        if (clip.is_gap) continue;  // Skip gaps in first pass
        double clip_end = clip.start_time + clip.duration;
        if (timestamp >= clip.start_time && timestamp < clip_end) {
            return &clip;
        }
    }

    // Second pass: if no real clip found, return gap
    for (const auto& clip : track.clips) {
        if (!clip.is_gap) continue;  // Only gaps in second pass
        double clip_end = clip.start_time + clip.duration;
        if (timestamp >= clip.start_time && timestamp < clip_end) {
            return &clip;
        }
    }

    return nullptr;
}

std::string TimelineFlattener::GetVisibleClipPathAtTime(double timestamp) {
    // Check cache first
    if (cache_visible_clips_.count(timestamp)) {
        return cache_visible_clips_[timestamp];
    }

    // Find topmost clip using z_index (higher = on top)
    const OTIOClip* best_clip = nullptr;
    int best_z_index = -1;

    for (const auto& track : tracks_) {
        if (!track.is_video || !track.visible) continue;

        const OTIOClip* clip = FindClipInTrack(track, timestamp);
        if (clip && !clip->is_gap) {
            if (track.z_index > best_z_index) {
                best_z_index = track.z_index;
                best_clip = clip;
            }
        }
    }

    if (best_clip) {
        cache_visible_clips_[timestamp] = best_clip->file_path;
        return best_clip->file_path;
    }

    // No visible clip found
    cache_visible_clips_[timestamp] = "";
    return "";
}

const OTIOClip* TimelineFlattener::GetVisibleClipAtTime(double timestamp) {
    // Find the topmost visible clip using z_index for priority
    // Higher z_index = higher visual priority (V2 above V1)
    const OTIOClip* best_clip = nullptr;
    int best_z_index = -1;

    for (const auto& track : tracks_) {
        if (!track.is_video || !track.visible) continue;

        const OTIOClip* clip = FindClipInTrack(track, timestamp);
        if (clip && !clip->is_gap && clip->is_linked) {
            // Use z_index to determine priority (higher = on top)
            if (track.z_index > best_z_index) {
                best_z_index = track.z_index;
                best_clip = clip;
            }
        }
    }

    return best_clip;
}

std::vector<std::string> TimelineFlattener::GetAudibleClipPathsAtTime(double timestamp) {
    std::vector<std::string> audio_paths;
    
    for (const auto& track : tracks_) {
        if (!track.is_video && !track.muted) {
            const OTIOClip* clip = FindClipInTrack(track, timestamp);
            if (clip && !clip->is_gap) {
                audio_paths.push_back(clip->file_path);
            }
        }
    }
    
    return audio_paths;
}

void TimelineFlattener::InvalidateCache() {
    cache_visible_clips_.clear();
}

// ============================================================================
// TimelineView Implementation
// ============================================================================

TimelineView::TimelineView(::VideoPlayer* player)
    : video_player_(player) {
}

TimelineView::~TimelineView() {
    ShutdownPlayback();
}

bool TimelineView::InitializePlayback(DummyVideoGenerator* dummy_generator) {
    if (!video_player_ || !dummy_generator) {
        Debug::Log("TimelineView::InitializePlayback: Invalid parameters");
        return false;
    }

    if (tracks_.empty()) {
        Debug::Log("TimelineView::InitializePlayback: No tracks loaded");
        return false;
    }

    // Count linked clips
    int linked_count = 0;
    for (const auto& track : tracks_) {
        for (const auto& clip : track.clips) {
            if (clip.is_linked) linked_count++;
        }
    }

    if (linked_count == 0) {
        Debug::Log("TimelineView::InitializePlayback: No linked clips - link media first");
        return false;
    }

    Debug::Log("TimelineView::InitializePlayback: Initializing with " +
               std::to_string(linked_count) + " linked clips");

    // Create and initialize playback controller
    playback_controller_ = std::make_unique<TimelinePlaybackController>();

    if (!playback_controller_->InitializeForTimeline(this, video_player_, dummy_generator)) {
        Debug::Log("TimelineView::InitializePlayback: Failed to initialize controller");
        playback_controller_.reset();
        return false;
    }

    Debug::Log("TimelineView::InitializePlayback: Playback initialized successfully");
    return true;
}

void TimelineView::ShutdownPlayback() {
    if (playback_controller_) {
        Debug::Log("TimelineView::ShutdownPlayback: Shutting down playback controller");
        playback_controller_->Shutdown();
        playback_controller_.reset();
    }
}

//=============================================================================
// Timeline In/Out Points and Loop Mode
//=============================================================================

void TimelineView::SetTimelineInPoint(double time) {
    // Clamp to valid timeline range
    if (time < 0) time = 0;
    if (time > timeline_duration_) time = timeline_duration_;

    // Toggle behavior: if same position, clear it
    if (timeline_in_point_ >= 0 && std::abs(timeline_in_point_ - time) < 0.01) {
        timeline_in_point_ = -1.0;
        Debug::Log("TimelineView: Cleared In point");
    } else {
        timeline_in_point_ = time;
        Debug::Log("TimelineView: Set In point at " + std::to_string(time) + "s");

        // Auto-clear Out point if it's before new In point
        if (timeline_out_point_ >= 0 && timeline_out_point_ < time) {
            timeline_out_point_ = -1.0;
            Debug::Log("TimelineView: Auto-cleared Out point (was before In point)");
        }
    }
}

void TimelineView::SetTimelineOutPoint(double time) {
    // Clamp to valid timeline range
    if (time < 0) time = 0;
    if (time > timeline_duration_) time = timeline_duration_;

    // Toggle behavior: if same position, clear it
    if (timeline_out_point_ >= 0 && std::abs(timeline_out_point_ - time) < 0.01) {
        timeline_out_point_ = -1.0;
        Debug::Log("TimelineView: Cleared Out point");
    } else {
        timeline_out_point_ = time;
        Debug::Log("TimelineView: Set Out point at " + std::to_string(time) + "s");

        // Auto-clear In point if it's after new Out point
        if (timeline_in_point_ >= 0 && timeline_in_point_ > time) {
            timeline_in_point_ = -1.0;
            Debug::Log("TimelineView: Auto-cleared In point (was after Out point)");
        }
    }
}

void TimelineView::ClearTimelineInOutPoints() {
    timeline_in_point_ = -1.0;
    timeline_out_point_ = -1.0;
    Debug::Log("TimelineView: Cleared In/Out points");
}

//=============================================================================
// Zoom/Pan Control
//=============================================================================

void TimelineView::SetZoomLevel(float zoom) {
    // Clamp to reasonable range
    if (zoom < 0.1f) zoom = 0.1f;
    if (zoom > 100.0f) zoom = 100.0f;
    zoom_level_ = zoom;
}

void TimelineView::SetScrollOffset(float offset) {
    float max_offset = GetMaxScrollOffset();
    if (offset < 0) offset = 0;
    if (offset > max_offset) offset = max_offset;
    scroll_offset_x_ = offset;
}

float TimelineView::GetMaxScrollOffset() const {
    // Max scroll is based on timeline duration and zoom level
    // Allow scrolling until the end of timeline is at the left edge
    float timeline_width_pixels = static_cast<float>(timeline_duration_) * zoom_level_;
    float visible_width = ruler_width_ > 0 ? ruler_width_ : 800.0f; // Default fallback
    return std::max(0.0f, timeline_width_pixels - visible_width);
}

//=============================================================================
// Force Cache Refresh (Failsafe)
//=============================================================================

void TimelineView::ForceRefreshCache() {
    Debug::Log("TimelineView::ForceRefreshCache: Starting forced cache refresh");

    // 1. Re-sync flattener with current tracks
    flattener_.SetTracks(tracks_);
    flattener_.InvalidateCache();

    // 2. Recalculate duration
    RecalculateDuration();

    // 3. If we have a playback controller, notify it of the "edit"
    // Use effective controller (external for scratch timelines, internal for file-based)
    TimelinePlaybackController* controller = GetEffectivePlaybackController();
    if (controller) {
        // Update duration
        controller->UpdateDuration(timeline_duration_);

        // Get the cache and force full invalidation
        if (controller->GetCache()) {
            controller->GetCache()->UpdateDuration(timeline_duration_);
            controller->GetCache()->NotifyTracksEdited();
        }

        // Notify playback controller
        controller->NotifyTracksEdited();
    }

    Debug::Log("TimelineView::ForceRefreshCache: Complete");
}

void TimelineView::Render(bool* show_timeline_panel) {
    // Transparent border for docked panel (dock borders remain visible)
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

    if (!ImGui::Begin("Timeline View", show_timeline_panel, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        ImGui::PopStyleColor();
        return;
    }
    
    RenderToolbar();
    
    ImGui::Separator();
    
    // Main timeline area with scrolling
    ImVec2 content_region = ImGui::GetContentRegionAvail();
    
    ImGui::BeginChild("TimelineScrollRegion", ImVec2(0, content_region.y - 100), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    
    RenderTrackList();

    ImGui::EndChild();

    ImGui::Separator();

    // Viewport minimap (shows pan/zoom position when zoomed in)
    RenderViewportMinimap();

    ImGui::Separator();

    RenderTimelineRuler();

    ImGui::End();
    ImGui::PopStyleColor();  // Transparent border
}

void TimelineView::RenderToolbar() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Import OTIO...")) {
                // Open file dialog (integrate with existing file dialog system)
                Debug::Log("Import OTIO dialog");
            }
            if (ImGui::MenuItem("Import AAF...")) {
                Debug::Log("Import AAF dialog");
            }
            if (ImGui::MenuItem("Import EDL...")) {
                Debug::Log("Import EDL dialog");
            }
            if (ImGui::MenuItem("Import FCP XML...")) {
                Debug::Log("Import FCP XML dialog");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Export Flattened EDL...")) {
                Debug::Log("Export EDL dialog");
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Show Thumbnails", nullptr, &show_thumbnails_);
            ImGui::MenuItem("Show Waveforms", nullptr, &show_waveforms_);
            ImGui::Separator();
            if (ImGui::MenuItem("Zoom In")) {
                zoom_level_ *= 1.5f;
            }
            if (ImGui::MenuItem("Zoom Out")) {
                zoom_level_ /= 1.5f;
            }
            if (ImGui::MenuItem("Fit Timeline")) {
                // Calculate zoom to fit entire timeline
                ImVec2 region = ImGui::GetContentRegionAvail();
                zoom_level_ = region.x / timeline_duration_;
            }
            ImGui::EndMenu();
        }
        
        ImGui::EndMenuBar();
    }
    
    // Timeline info
    ImGui::Text("Timeline: %s", timeline_name_.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%.2f fps, %.2fs duration, %d video tracks, %d audio tracks)",
                        frame_rate_, timeline_duration_,
                        GetVideoTrackCount(), GetAudioTrackCount());
    
    // Flatten mode selector
    ImGui::Spacing();
    ImGui::Text("Flatten Mode:");
    ImGui::SameLine();
    
    if (ImGui::RadioButton("Auto", flatten_mode_ == FlattenMode::AUTO_PAINTER_ORDER)) {
        flatten_mode_ = FlattenMode::AUTO_PAINTER_ORDER;
        UpdateFlattenedPlayback();
    }
    ImGui::SameLine();
    
    if (ImGui::RadioButton("Manual Priority", flatten_mode_ == FlattenMode::MANUAL_PRIORITY)) {
        flatten_mode_ = FlattenMode::MANUAL_PRIORITY;
    }
    ImGui::SameLine();
    
    if (ImGui::RadioButton("Single Track", flatten_mode_ == FlattenMode::SINGLE_TRACK_PREVIEW)) {
        flatten_mode_ = FlattenMode::SINGLE_TRACK_PREVIEW;
    }
}

void TimelineView::RenderTrackList() {
    if (tracks_.empty()) {
        ImGui::TextDisabled("No timeline loaded. Import an OTIO, AAF, EDL, or FCP XML file.");
        return;
    }
    
    // Reverse order to show top tracks first (V3, V2, V1)
    for (int i = static_cast<int>(tracks_.size()) - 1; i >= 0; i--) {
        auto& track = tracks_[i];
        
        ImGui::PushID(track.id.c_str());
        
        RenderTrackHeader(track, i);
        ImGui::SameLine();
        
        // Track clips area
        ImGui::BeginChild(("TrackClips_" + track.id).c_str(),
                         ImVec2(0, track_height_), true);
        
        RenderTrackClips(track, track_height_);
        
        ImGui::EndChild();
        
        ImGui::PopID();
    }
}

void TimelineView::RenderTrackHeader(OTIOTrack& track, int track_index) {
    ImGui::BeginGroup();
    
    // Track name and type
    const char* icon = track.is_video ? "🎬" : "🎵";
    ImGui::Text("%s %s", icon, track.name.c_str());
    
    // Visibility/Mute toggle
    if (track.is_video) {
        bool visible = track.visible;
        if (ImGui::Checkbox("👁️##vis", &visible)) {
            HandleTrackVisibilityToggle(track.id);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Toggle track visibility");
        }
    } else {
        bool muted = track.muted;
        if (ImGui::Checkbox("🔇##mute", &muted)) {
            HandleTrackMuteToggle(track.id);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Mute/Unmute track");
        }
    }
    
    // Solo button
    ImGui::SameLine();
    if (ImGui::SmallButton("S")) {
        HandleTrackSolo(track.id);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Solo this track (disable all others)");
    }
    
    ImGui::EndGroup();
    
    // Fixed width for header
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(200 - ImGui::GetItemRectSize().x, 0));
}

void TimelineView::RenderTrackClips(const OTIOTrack& track, float track_height) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();

    const float clip_rounding = 2.0f;  // Match playlist style

    for (const auto& clip : track.clips) {
        float x_start = canvas_pos.x + (float)(clip.start_time * zoom_level_);
        float x_end = canvas_pos.x + (float)((clip.start_time + clip.duration) * zoom_level_);
        float y_top = canvas_pos.y + 2;
        float y_bottom = canvas_pos.y + track_height - 2;
        float clip_width = x_end - x_start;
        float clip_height = y_bottom - y_top;

        // Clip colors - matching playlist style (dark grey base, lighter border)
        ImU32 clip_color, border_color;
        if (clip.is_gap) {
            clip_color = IM_COL32(30, 30, 30, 180);
            border_color = IM_COL32(50, 50, 50, 255);
        } else if (clip.is_linked) {
            // Linked clips use accent color hint
            ImVec4 accent = GetWindowsAccentColor();
            clip_color = IM_COL32(
                (int)(accent.x * 80 + 40),
                (int)(accent.y * 80 + 40),
                (int)(accent.z * 80 + 40), 255);
            border_color = IM_COL32(
                (int)(accent.x * 120 + 60),
                (int)(accent.y * 120 + 60),
                (int)(accent.z * 120 + 60), 255);
        } else {
            // Unlinked clips - dark grey like playlist non-current clips
            clip_color = IM_COL32(60, 60, 60, 255);
            border_color = IM_COL32(90, 90, 90, 255);
        }

        // Draw clip rectangle with rounded corners
        draw_list->AddRectFilled(ImVec2(x_start, y_top), ImVec2(x_end, y_bottom),
                                clip_color, clip_rounding);
        draw_list->AddRect(ImVec2(x_start, y_top), ImVec2(x_end, y_bottom),
                          border_color, clip_rounding, 0, 1.5f);

        // Draw fade indicators
        if (clip.has_fade_in) {
            float fade_width = (float)(clip.fade_in_duration * zoom_level_);
            ImVec2 fade_points[3] = {
                ImVec2(x_start, y_bottom),
                ImVec2(x_start, y_top),
                ImVec2(x_start + fade_width, y_bottom)
            };
            draw_list->AddTriangleFilled(fade_points[0], fade_points[1], fade_points[2],
                                         ImGui::ColorConvertFloat4ToU32(color_transition_));
        }

        if (clip.has_fade_out) {
            float fade_width = (float)(clip.fade_out_duration * zoom_level_);
            ImVec2 fade_points[3] = {
                ImVec2(x_end, y_bottom),
                ImVec2(x_end, y_top),
                ImVec2(x_end - fade_width, y_bottom)
            };
            draw_list->AddTriangleFilled(fade_points[0], fade_points[1], fade_points[2],
                                         ImGui::ColorConvertFloat4ToU32(color_transition_));
        }

        // Clip name label - centered with shadow, using font_mono
        if (clip_width > 30.0f && font_mono) {
            ImGui::PushFont(font_mono);

            // Truncate clip name if needed to fit
            std::string display_name = clip.name;
            ImVec2 text_size = ImGui::CalcTextSize(display_name.c_str());

            float available_text_width = clip_width - 6.0f;  // 3px padding each side
            if (text_size.x > available_text_width) {
                // Truncate with ellipsis
                while (display_name.length() > 3 &&
                       ImGui::CalcTextSize((display_name.substr(0, display_name.length() - 3) + "...").c_str()).x > available_text_width) {
                    display_name.pop_back();
                }
                if (display_name.length() > 3) {
                    display_name = display_name.substr(0, display_name.length() - 3) + "...";
                }
                text_size = ImGui::CalcTextSize(display_name.c_str());
            }

            // Center text vertically and horizontally within clip
            ImVec2 text_pos(
                x_start + (clip_width - text_size.x) * 0.5f,
                y_top + (clip_height - text_size.y) * 0.5f
            );

            // Draw text with shadow for readability
            draw_list->AddText(ImVec2(text_pos.x + 1, text_pos.y + 1), IM_COL32(0, 0, 0, 180), display_name.c_str());
            draw_list->AddText(text_pos, IM_COL32(255, 255, 255, 255), display_name.c_str());

            ImGui::PopFont();
        }

        // Interaction: hover detection
        ImVec2 mouse_pos = ImGui::GetMousePos();
        if (mouse_pos.x >= x_start && mouse_pos.x <= x_end &&
            mouse_pos.y >= y_top && mouse_pos.y <= y_bottom) {
            hovered_clip_id_ = clip.id;
            RenderClipTooltip(clip);

            if (ImGui::IsMouseClicked(0)) {
                HandleClipClick(clip);
            }
        }
    }
}

void TimelineView::RenderTimelineRuler() {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 ruler_pos = ImGui::GetCursorScreenPos();
    ImVec2 ruler_size = ImVec2(ImGui::GetContentRegionAvail().x, 35);  // Extra height for cache bar

    // Save for RenderCacheBar() to use
    ruler_screen_pos_ = ruler_pos;
    ruler_width_ = ruler_size.x;

    // Debug: Log ruler position once
    static bool logged_ruler = false;
    if (!logged_ruler) {
        Debug::Log("RenderTimelineRuler: ruler_pos=(" + std::to_string(ruler_pos.x) + "," +
                   std::to_string(ruler_pos.y) + "), width=" + std::to_string(ruler_size.x));
        logged_ruler = true;
    }

    // Background - matching transport panel style
    draw_list->AddRectFilled(ruler_pos, ImVec2(ruler_pos.x + ruler_size.x, ruler_pos.y + ruler_size.y),
                            IM_COL32(16, 16, 16, 60));

    // Top and bottom border lines - matching transport panel
    ImU32 border_color = IM_COL32(160, 160, 160, 50);
    draw_list->AddLine(ruler_pos, ImVec2(ruler_pos.x + ruler_size.x, ruler_pos.y), border_color, 1.0f);
    draw_list->AddLine(ImVec2(ruler_pos.x, ruler_pos.y + ruler_size.y),
                      ImVec2(ruler_pos.x + ruler_size.x, ruler_pos.y + ruler_size.y), border_color, 1.0f);

    // Time markers every second with major/minor tick distinction
    int second_count = 0;
    for (double t = 0; t <= timeline_duration_; t += 1.0) {
        float x = ruler_pos.x + (float)(t * zoom_level_) - scroll_offset_x_;

        if (x < ruler_pos.x || x > ruler_pos.x + ruler_size.x) {
            second_count++;
            continue;
        }

        // Major tick every 5 seconds, minor tick for others
        bool is_major = (second_count % 5 == 0);
        float tick_height = is_major ? 20.0f : 12.0f;
        ImU32 tick_color = is_major ? IM_COL32(160, 160, 160, 255) : IM_COL32(120, 120, 120, 255);
        float tick_width = is_major ? 1.5f : 1.0f;

        // Draw tick from top
        draw_list->AddLine(ImVec2(x, ruler_pos.y),
                          ImVec2(x, ruler_pos.y + tick_height),
                          tick_color, tick_width);

        // Time label on major ticks only
        if (is_major) {
            int minutes = (int)t / 60;
            int seconds = (int)t % 60;
            char time_label[32];
            snprintf(time_label, sizeof(time_label), "%02d:%02d", minutes, seconds);

            // Use font_mono for consistent styling
            if (font_mono) {
                ImVec2 text_size = ImGui::CalcTextSize(time_label);
                draw_list->AddText(font_mono, 12.0f,
                    ImVec2(x - text_size.x * 0.5f, ruler_pos.y + 22),
                    IM_COL32(180, 180, 180, 255), time_label);
            } else {
                draw_list->AddText(ImVec2(x - 15, ruler_pos.y + 22),
                    IM_COL32(180, 180, 180, 255), time_label);
            }
        }

        second_count++;
    }

    // Cache progress bar (below time markers)
    RenderCacheBar();

    // Playhead indicator
    RenderPlayhead();
}

void TimelineView::RenderPlayhead() {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 ruler_pos = ImGui::GetCursorScreenPos();
    
    float playhead_x = ruler_pos.x + (float)(current_time_ * zoom_level_) - scroll_offset_x_;
    
    // Vertical line
    draw_list->AddLine(ImVec2(playhead_x, ruler_pos.y),
                      ImVec2(playhead_x, ruler_pos.y + 500),  // Extends down entire timeline
                      IM_COL32(255, 100, 100, 200), 2.0f);
    
    // Triangle head
    ImVec2 triangle[3] = {
        ImVec2(playhead_x - 8, ruler_pos.y),
        ImVec2(playhead_x + 8, ruler_pos.y),
        ImVec2(playhead_x, ruler_pos.y + 10)
    };
    draw_list->AddTriangleFilled(triangle[0], triangle[1], triangle[2], IM_COL32(255, 100, 100, 255));
}

void TimelineView::RenderCacheBar() {
    // Debug: Log early returns
    static int call_count = 0;
    call_count++;

    // Skip if no playback controller or cache
    // Use effective controller (external for scratch timelines, internal for file-based)
    TimelinePlaybackController* controller = GetEffectivePlaybackController();
    if (!controller) {
        if (call_count <= 5) Debug::Log("RenderCacheBar: No playback controller");
        return;
    }

    TimelineCache* cache = controller->GetCache();
    if (!cache) {
        if (call_count <= 5) Debug::Log("RenderCacheBar: No cache");
        return;
    }
    if (!cache->IsInitialized()) {
        if (call_count <= 5) Debug::Log("RenderCacheBar: Cache not initialized");
        return;
    }

    // Get cache segments
    auto segments = cache->GetCacheSegments();
    auto stats = cache->GetStats();

    // Debug: Log segment count periodically
    if (call_count <= 5 || call_count % 300 == 0) {
        Debug::Log("RenderCacheBar: " + std::to_string(segments.size()) + " segments, " +
                   std::to_string(stats.cached_frames) + " cached frames, ruler_pos=(" +
                   std::to_string(ruler_screen_pos_.x) + "," + std::to_string(ruler_screen_pos_.y) + ")");
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Use saved ruler position from RenderTimelineRuler, not current cursor
    // The cursor may have moved since the ruler was drawn
    ImVec2 ruler_pos = ruler_screen_pos_;  // Saved during RenderTimelineRuler
    float ruler_width = ruler_width_;

    // Cache bar at bottom of ruler area
    const float cache_bar_height = 6.0f;  // Make it more visible
    const float cache_bar_y = ruler_pos.y + 28.0f;  // Just below time labels

    // Draw background bar (dark gray) - always draw this
    draw_list->AddRectFilled(
        ImVec2(ruler_pos.x, cache_bar_y),
        ImVec2(ruler_pos.x + ruler_width, cache_bar_y + cache_bar_height),
        IM_COL32(50, 50, 50, 255)  // More visible background
    );

    // Draw cached segments (green)
    for (const auto& segment : segments) {
        float start_x = ruler_pos.x + (float)(segment.start_time * zoom_level_) - scroll_offset_x_;
        float end_x = ruler_pos.x + (float)(segment.end_time * zoom_level_) - scroll_offset_x_;

        // Clamp to visible region
        start_x = std::max(start_x, ruler_pos.x);
        end_x = std::min(end_x, ruler_pos.x + ruler_width);

        if (end_x <= start_x) continue;

        // Green color for cached frames (similar to main timeline)
        ImU32 cache_color = IM_COL32(80, 200, 120, 200);

        draw_list->AddRectFilled(
            ImVec2(start_x, cache_bar_y),
            ImVec2(end_x, cache_bar_y + cache_bar_height),
            cache_color
        );
    }

    // Show stats on hover
    ImVec2 mouse_pos = ImGui::GetMousePos();
    if (mouse_pos.y >= cache_bar_y && mouse_pos.y <= cache_bar_y + cache_bar_height &&
        mouse_pos.x >= ruler_pos.x && mouse_pos.x <= ruler_pos.x + ruler_width) {
        auto stats = cache->GetStats();
        ImGui::BeginTooltip();
        ImGui::Text("Timeline Cache");
        ImGui::Separator();
        if (stats.total_timeline_frames > 0) {
            float percent = (float)stats.cached_frames / stats.total_timeline_frames * 100.0f;
            ImGui::Text("Cached: %d / %d frames (%.1f%%)", stats.cached_frames, stats.total_timeline_frames, percent);
        } else {
            ImGui::Text("Cached frames: %d", stats.cached_frames);
        }
        ImGui::Text("Duration: %.2fs", stats.timeline_duration);
        ImGui::Text("Cache size: %.1f MB", stats.cache_bytes / (1024.0 * 1024.0));
        ImGui::Text("Hit ratio: %.1f%%", stats.GetHitRatio() * 100.0);
        ImGui::Text("Pending: %d", stats.pending_requests);
        ImGui::EndTooltip();
    }
}

void TimelineView::RenderClipTooltip(const OTIOClip& clip) {
    // Match playlist tooltip styling
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(50, 50, 50, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(100, 100, 100, 51));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 9.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);

    ImGui::BeginTooltip();

    if (font_mono) ImGui::PushFont(font_mono);

    // Clip name
    ImGui::Text("Clip: %s", clip.name.c_str());

    // Duration formatted as MM:SS:FF
    int minutes = (int)(clip.duration / 60.0);
    int seconds = (int)clip.duration % 60;
    int frames = (int)((clip.duration - (int)clip.duration) * frame_rate_);
    ImGui::Text("Duration: %02d:%02d:%02d", minutes, seconds, frames);

    // File path (truncated if too long)
    std::string filename = clip.file_path;
    size_t last_slash = filename.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        filename = filename.substr(last_slash + 1);
    }
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "File: %s", filename.c_str());

    // Link status
    if (clip.is_linked) {
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "Linked");
    } else {
        ImGui::TextColored(ImVec4(0.8f, 0.5f, 0.5f, 1.0f), "Unlinked");
    }

    if (clip.has_fade_in || clip.has_fade_out) {
        ImGui::Separator();
        if (clip.has_fade_in) {
            ImGui::Text("Fade In: %.2fs", clip.fade_in_duration);
        }
        if (clip.has_fade_out) {
            ImGui::Text("Fade Out: %.2fs", clip.fade_out_duration);
        }
    }

    if (font_mono) ImGui::PopFont();

    ImGui::EndTooltip();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void TimelineView::RenderViewportMinimap() {
    // Skip if timeline is empty or no duration
    if (timeline_duration_ <= 0 || tracks_.empty()) return;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 minimap_pos = ImGui::GetCursorScreenPos();

    // Minimap dimensions
    const float minimap_height = 24.0f;
    float minimap_width = ImGui::GetContentRegionAvail().x;

    // Background
    draw_list->AddRectFilled(
        minimap_pos,
        ImVec2(minimap_pos.x + minimap_width, minimap_pos.y + minimap_height),
        IM_COL32(25, 25, 25, 255)
    );

    // Border
    draw_list->AddRect(
        minimap_pos,
        ImVec2(minimap_pos.x + minimap_width, minimap_pos.y + minimap_height),
        IM_COL32(60, 60, 60, 255),
        0.0f, 0, 1.0f
    );

    // Calculate scale: entire timeline fits in minimap width
    float scale = minimap_width / (float)timeline_duration_;

    // Draw compressed clip representations (just colored bars)
    float track_bar_height = (minimap_height - 4.0f) / std::max((int)tracks_.size(), 1);
    track_bar_height = std::min(track_bar_height, 6.0f);  // Cap individual track height

    float y_offset = minimap_pos.y + 2.0f + (minimap_height - 4.0f - track_bar_height * tracks_.size()) * 0.5f;

    for (const auto& track : tracks_) {
        if (!track.is_video) continue;  // Only show video tracks in minimap

        for (const auto& clip : track.clips) {
            if (clip.is_gap) continue;  // Skip gaps

            float clip_x_start = minimap_pos.x + (float)(clip.start_time * scale);
            float clip_x_end = minimap_pos.x + (float)((clip.start_time + clip.duration) * scale);

            // Clip color - lighter version for visibility
            ImU32 clip_color;
            if (clip.is_linked) {
                ImVec4 accent = GetWindowsAccentColor();
                clip_color = IM_COL32(
                    (int)(accent.x * 150 + 80),
                    (int)(accent.y * 150 + 80),
                    (int)(accent.z * 150 + 80), 200);
            } else {
                clip_color = IM_COL32(100, 100, 100, 200);
            }

            draw_list->AddRectFilled(
                ImVec2(clip_x_start, y_offset),
                ImVec2(clip_x_end, y_offset + track_bar_height),
                clip_color
            );
        }
        y_offset += track_bar_height;
    }

    // Calculate viewport rectangle (visible region)
    float visible_start_time = scroll_offset_x_ / zoom_level_;
    float visible_end_time = (scroll_offset_x_ + ruler_width_) / zoom_level_;

    // Clamp to timeline bounds
    visible_start_time = std::max(0.0f, visible_start_time);
    visible_end_time = std::min((float)timeline_duration_, visible_end_time);

    float viewport_x_start = minimap_pos.x + visible_start_time * scale;
    float viewport_x_end = minimap_pos.x + visible_end_time * scale;

    // Ensure minimum viewport width for visibility
    float min_viewport_width = 8.0f;
    if (viewport_x_end - viewport_x_start < min_viewport_width) {
        float center = (viewport_x_start + viewport_x_end) * 0.5f;
        viewport_x_start = center - min_viewport_width * 0.5f;
        viewport_x_end = center + min_viewport_width * 0.5f;
    }

    // Viewport indicator fill (semi-transparent highlight)
    draw_list->AddRectFilled(
        ImVec2(viewport_x_start, minimap_pos.y + 1),
        ImVec2(viewport_x_end, minimap_pos.y + minimap_height - 1),
        IM_COL32(255, 255, 255, 30)
    );

    // Viewport border (bright, visible)
    ImVec4 accent = GetWindowsAccentColor();
    ImU32 viewport_border_color = IM_COL32(
        (int)(accent.x * 255),
        (int)(accent.y * 255),
        (int)(accent.z * 255), 255);

    draw_list->AddRect(
        ImVec2(viewport_x_start, minimap_pos.y + 1),
        ImVec2(viewport_x_end, minimap_pos.y + minimap_height - 1),
        viewport_border_color,
        2.0f, 0, 2.0f
    );

    // Draw playhead on minimap
    float playhead_x = minimap_pos.x + (float)(current_time_ * scale);
    draw_list->AddLine(
        ImVec2(playhead_x, minimap_pos.y),
        ImVec2(playhead_x, minimap_pos.y + minimap_height),
        IM_COL32(255, 100, 100, 255), 1.5f
    );

    // Interaction: drag viewport to pan
    ImVec2 mouse_pos = ImGui::GetMousePos();
    bool mouse_in_minimap = (mouse_pos.x >= minimap_pos.x &&
                             mouse_pos.x <= minimap_pos.x + minimap_width &&
                             mouse_pos.y >= minimap_pos.y &&
                             mouse_pos.y <= minimap_pos.y + minimap_height);

    // Check if mouse is over the viewport indicator specifically
    bool mouse_in_viewport = (mouse_pos.x >= viewport_x_start &&
                              mouse_pos.x <= viewport_x_end &&
                              mouse_pos.y >= minimap_pos.y &&
                              mouse_pos.y <= minimap_pos.y + minimap_height);

    // Change cursor when hovering viewport
    if (mouse_in_viewport && !is_dragging_viewport_) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    // Start drag when clicking on viewport
    if (mouse_in_viewport && ImGui::IsMouseClicked(0)) {
        is_dragging_viewport_ = true;
        viewport_drag_start_offset_ = scroll_offset_x_;
        viewport_drag_start_mouse_x_ = mouse_pos.x;
    }

    // Handle dragging
    if (is_dragging_viewport_) {
        if (ImGui::IsMouseDown(0)) {
            // Calculate new scroll offset based on mouse delta
            float mouse_delta = mouse_pos.x - viewport_drag_start_mouse_x_;
            float time_delta = mouse_delta / scale;  // Convert pixels to time
            float new_offset = viewport_drag_start_offset_ + time_delta * zoom_level_;
            SetScrollOffset(new_offset);
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        } else {
            is_dragging_viewport_ = false;
        }
    }

    // Click anywhere else in minimap to jump viewport there
    if (mouse_in_minimap && !mouse_in_viewport && !is_dragging_viewport_ && ImGui::IsMouseClicked(0)) {
        // Center viewport on click position
        float clicked_time = (mouse_pos.x - minimap_pos.x) / scale;
        float viewport_half_width = (visible_end_time - visible_start_time) * 0.5f;
        float target_start_time = clicked_time - viewport_half_width;
        SetScrollOffset(target_start_time * zoom_level_);
    }

    // Tooltip
    if (mouse_in_minimap) {
        ImGui::BeginTooltip();
        if (mouse_in_viewport || is_dragging_viewport_) {
            ImGui::Text("Drag to pan viewport");
        } else {
            ImGui::Text("Click to jump viewport");
        }
        float hover_time = (mouse_pos.x - minimap_pos.x) / scale;
        int minutes = (int)(hover_time / 60.0);
        int seconds = (int)hover_time % 60;
        ImGui::Text("Time: %02d:%02d", minutes, seconds);
        ImGui::EndTooltip();
    }

    // Reserve space for minimap
    ImGui::Dummy(ImVec2(minimap_width, minimap_height));
}

void TimelineView::HandleTrackVisibilityToggle(const std::string& track_id) {
    flattener_.SetTrackVisibility(track_id, !GetTrackById(track_id)->visible);
    // Visibility change affects which clip is shown - invalidate cache
    SyncFlattenerAndInvalidate();
}

void TimelineView::HandleTrackMuteToggle(const std::string& track_id) {
    flattener_.SetTrackMute(track_id, !GetTrackById(track_id)->muted);
    // Note: Audio mixing not implemented in MVP
}

void TimelineView::HandleTrackSolo(const std::string& track_id) {
    // Disable all tracks except this one
    for (auto& track : tracks_) {
        if (track.is_video) {
            bool should_be_visible = (track.id == track_id);
            track.visible = should_be_visible;
            flattener_.SetTrackVisibility(track.id, should_be_visible);
        }
    }
    // Visibility change affects which clip is shown - invalidate cache
    SyncFlattenerAndInvalidate();
}

void TimelineView::HandleClipClick(const OTIOClip& clip) {
    Debug::Log("Clicked clip: " + clip.name);
    
    // Future: Add clip selection, editing, etc.
}

void TimelineView::HandleTimelineSeek(double timestamp) {
    current_time_ = timestamp;
    UpdateFlattenedPlayback();
}

void TimelineView::UpdateFlattenedPlayback() {
    // If we have a playback controller (TimelineCache mode), just seek the dummy video
    // The cache will provide the actual frames - don't load individual clips
    if (playback_controller_ && video_player_) {
        // Seek the dummy video to the current timeline position
        video_player_->Seek(current_time_);
        return;
    }

    // Legacy mode: Load individual clips into MPV (no cache)
    const OTIOClip* visible_clip = flattener_.GetVisibleClipAtTime(current_time_);

    if (visible_clip && !visible_clip->is_gap) {
        LoadFlattenedClipIntoMPV(*visible_clip);
    }
}

void TimelineView::LoadFlattenedClipIntoMPV(const OTIOClip& clip) {
    if (!video_player_) return;
    
    // Construct EDL URL with trim points
    std::ostringstream edl;
    edl << "edl://" << clip.file_path
        << ",start=" << clip.source_in
        << ",length=" << (clip.source_out - clip.source_in);
    
    // Add fade filters if present
    if (clip.has_fade_in || clip.has_fade_out) {
        edl << ",lavfi-graph=[";
        
        if (clip.has_fade_in) {
            edl << "fade=t=in:st=0:d=" << clip.fade_in_duration;
            if (clip.has_fade_out) edl << ",";
        }
        
        if (clip.has_fade_out) {
            double fade_start = (clip.source_out - clip.source_in) - clip.fade_out_duration;
            edl << "fade=t=out:st=" << fade_start << ":d=" << clip.fade_out_duration;
        }
        
        edl << "]";
    }
    
    std::string edl_path = edl.str();
    Debug::Log("Loading flattened clip into MPV: " + edl_path);
    
    video_player_->LoadFile(edl_path);
}

int TimelineView::GetVideoTrackCount() const {
    return static_cast<int>(std::count_if(tracks_.begin(), tracks_.end(),
                                          [](const OTIOTrack& t) { return t.is_video; }));
}

int TimelineView::GetAudioTrackCount() const {
    return static_cast<int>(std::count_if(tracks_.begin(), tracks_.end(),
                                          [](const OTIOTrack& t) { return !t.is_video; }));
}

std::string TimelineView::GetSourceDirectory() const {
    if (source_file_path_.empty()) {
        return "";
    }
    std::filesystem::path source_path(source_file_path_);
    return source_path.parent_path().string();
}

// Track management methods (Resolve-style)
void TimelineView::AddVideoTrack(int insert_index) {
    OTIOTrack new_track;
    new_track.is_video = true;

    // Generate unique ID and name
    int video_count = GetVideoTrackCount();
    new_track.id = "V" + std::to_string(video_count + 1) + "_" + std::to_string(std::time(nullptr));
    new_track.name = "V" + std::to_string(video_count + 1);
    new_track.visible = true;
    new_track.muted = false;
    new_track.z_index = video_count + 1;

    if (insert_index < 0 || insert_index > static_cast<int>(tracks_.size())) {
        // Find first video track position (add at top of video tracks)
        auto it = std::find_if(tracks_.begin(), tracks_.end(),
                              [](const OTIOTrack& t) { return t.is_video; });
        if (it != tracks_.end()) {
            tracks_.insert(it, new_track);
        } else {
            // No video tracks, insert at beginning
            tracks_.insert(tracks_.begin(), new_track);
        }
    } else {
        tracks_.insert(tracks_.begin() + insert_index, new_track);
    }

    // Update flattener
    flattener_.SetTracks(tracks_);

    Debug::Log("Added video track: " + new_track.name);
}

void TimelineView::AddAudioTrack(int insert_index) {
    OTIOTrack new_track;
    new_track.is_video = false;

    // Generate unique ID and name
    int audio_count = GetAudioTrackCount();
    new_track.id = "A" + std::to_string(audio_count + 1) + "_" + std::to_string(std::time(nullptr));
    new_track.name = "A" + std::to_string(audio_count + 1);
    new_track.visible = true;
    new_track.muted = false;
    new_track.z_index = audio_count + 1;

    if (insert_index < 0 || insert_index > static_cast<int>(tracks_.size())) {
        // Add at end (bottom of audio tracks)
        tracks_.push_back(new_track);
    } else {
        tracks_.insert(tracks_.begin() + insert_index, new_track);
    }

    // Update flattener
    flattener_.SetTracks(tracks_);

    Debug::Log("Added audio track: " + new_track.name);
}

bool TimelineView::DeleteTrack(int track_index) {
    if (track_index < 0 || track_index >= static_cast<int>(tracks_.size())) {
        return false;
    }

    if (!CanDeleteTrack(track_index)) {
        Debug::Log("Cannot delete track: it's the last of its type");
        return false;
    }

    std::string track_name = tracks_[track_index].name;
    tracks_.erase(tracks_.begin() + track_index);

    // Update flattener
    flattener_.SetTracks(tracks_);

    Debug::Log("Deleted track: " + track_name);
    return true;
}

bool TimelineView::CanDeleteTrack(int track_index) const {
    if (track_index < 0 || track_index >= static_cast<int>(tracks_.size())) {
        return false;
    }

    const OTIOTrack& track = tracks_[track_index];

    // Can't delete if it's the last video or audio track
    if (track.is_video) {
        return GetVideoTrackCount() > 1;
    } else {
        return GetAudioTrackCount() > 1;
    }
}

// Placeholder implementations (full OTIO parsing in separate methods)
bool TimelineView::LoadOTIOFile(const std::string& file_path) {
    Debug::Log("Loading OTIO file: " + file_path);

    // Shutdown existing playback controller before loading new timeline
    // This ensures a fresh dummy video is generated for the new timeline
    ShutdownPlayback();

    // Store source file path for auto-relinking
    source_file_path_ = file_path;

    // TODO: Implement full OTIO parsing
    return ParseOTIOTimeline(file_path);
}

bool TimelineView::LoadEDLFile(const std::string& file_path) {
    Debug::Log("Loading EDL file: " + file_path);

    // Shutdown existing playback controller before loading new timeline
    // This ensures a fresh dummy video is generated for the new timeline
    ShutdownPlayback();

    auto result = EDLParser::Parse(file_path);
    if (!result.success) {
        Debug::Log("EDL parse failed: " + result.error_message);
        return false;
    }

    // Store source file path for auto-relinking
    source_file_path_ = file_path;

    // Apply parsed data
    timeline_name_ = result.timeline_name.empty() ? "Imported EDL" : result.timeline_name;
    frame_rate_ = result.frame_rate;
    tracks_ = std::move(result.tracks);

    // Calculate timeline duration
    timeline_duration_ = 0.0;
    for (const auto& track : tracks_) {
        for (const auto& clip : track.clips) {
            double clip_end = clip.start_time + clip.duration;
            if (clip_end > timeline_duration_) {
                timeline_duration_ = clip_end;
            }
        }
    }

    // Update flattener
    flattener_.SetTracks(tracks_);

    Debug::Log("EDL loaded: " + timeline_name_ +
               " (" + std::to_string(GetVideoTrackCount()) + " video, " +
               std::to_string(GetAudioTrackCount()) + " audio tracks)");

    return true;
}

bool TimelineView::LoadFCPXMLFile(const std::string& file_path) {
    Debug::Log("Loading FCP XML file: " + file_path);
    // TODO: Use OTIO's FCP XML adapter
    return false;
}

void TimelineView::InitializeForScratch(const std::string& name, double duration, double fps,
                                        int width, int height) {
    Debug::Log("Initializing scratch timeline: " + name + " (" +
               std::to_string(duration) + "s @ " + std::to_string(fps) + "fps, " +
               std::to_string(width) + "x" + std::to_string(height) + ")");

    // Clear existing data
    tracks_.clear();
    source_file_path_.clear();  // No source file for scratch timelines

    // Set timeline properties
    timeline_name_ = name.empty() ? "New Timeline" : name;
    timeline_duration_ = duration;
    frame_rate_ = fps;

    // Create one empty video track and one empty audio track
    OTIOTrack video_track;
    video_track.id = "V1";
    video_track.name = "Video 1";
    video_track.is_video = true;
    video_track.visible = true;
    video_track.muted = false;
    video_track.z_index = 1;
    tracks_.push_back(video_track);

    OTIOTrack audio_track;
    audio_track.id = "A1";
    audio_track.name = "Audio 1";
    audio_track.is_video = false;
    audio_track.visible = true;
    audio_track.muted = false;
    audio_track.z_index = 0;
    tracks_.push_back(audio_track);

    // Update flattener with empty tracks
    flattener_.SetTracks(tracks_);

    Debug::Log("Scratch timeline initialized: " + timeline_name_);
}

bool TimelineView::ParseOTIOTimeline(const std::string& file_path) {
    // If empty file path, create mock data for testing
    if (file_path.empty()) {
        CreateMockTimeline();
        return true;
    }

#ifdef USE_OPENTIMELINEIO
    Debug::Log("Loading OTIO file: " + file_path);

    otio::ErrorStatus error_status;

    // Load the timeline from file
    auto timeline = dynamic_cast<otio::Timeline*>(
        otio::Timeline::from_json_file(file_path, &error_status)
    );

    if (!timeline || otio::is_error(error_status)) {
        Debug::Log("Failed to load OTIO file: " + error_status.full_description);
        return false;
    }

    // Extract timeline metadata
    timeline_name_ = timeline->name();
    if (timeline_name_.empty()) {
        timeline_name_ = "Untitled Timeline";
    }

    // Get global start time and rate
    auto global_start = timeline->global_start_time();
    if (global_start.has_value()) {
        frame_rate_ = global_start->rate();
    } else {
        frame_rate_ = 24.0; // Default
    }

    // Extract tracks from timeline
    ExtractTracksFromOTIO(timeline);

    // Calculate timeline duration from tracks
    timeline_duration_ = 0.0;
    for (const auto& track : tracks_) {
        for (const auto& clip : track.clips) {
            double clip_end = clip.start_time + clip.duration;
            if (clip_end > timeline_duration_) {
                timeline_duration_ = clip_end;
            }
        }
    }

    // Update flattener with new tracks
    flattener_.SetTracks(tracks_);

    Debug::Log("Loaded timeline: " + timeline_name_ +
               " (" + std::to_string(GetVideoTrackCount()) + " video, " +
               std::to_string(GetAudioTrackCount()) + " audio tracks)");

    return true;
#else
    // Fallback when OTIO is not available
    Debug::Log("OTIO not available, creating mock timeline");
    CreateMockTimeline();
    return true;
#endif
}

#ifdef USE_OPENTIMELINEIO
void TimelineView::ExtractTracksFromOTIO(otio::Timeline* timeline) {
    tracks_.clear();

    auto* stack = timeline->tracks();
    if (!stack) {
        Debug::Log("Timeline has no tracks stack");
        return;
    }

    int video_track_num = 0;
    int audio_track_num = 0;

    for (auto& child : stack->children()) {
        auto* track = dynamic_cast<otio::Track*>(child.value);
        if (!track) continue;

        OTIOTrack our_track;
        our_track.id = track->name().empty() ?
            ("track_" + std::to_string(video_track_num + audio_track_num)) : track->name();

        // Determine track type
        std::string kind = track->kind();
        our_track.is_video = (kind == otio::Track::Kind::video);

        // Set track name
        if (our_track.is_video) {
            video_track_num++;
            our_track.name = our_track.id.empty() ?
                ("V" + std::to_string(video_track_num)) : our_track.id;
            our_track.z_index = video_track_num;
        } else {
            audio_track_num++;
            our_track.name = our_track.id.empty() ?
                ("A" + std::to_string(audio_track_num)) : our_track.id;
            our_track.z_index = 0;
        }

        our_track.visible = true;
        our_track.muted = false;

        // Track timeline position as we process clips
        double track_position = 0.0;

        // Process track children (clips, gaps, transitions)
        for (auto& item : track->children()) {
            if (auto* clip = dynamic_cast<otio::Clip*>(item.value)) {
                OTIOClip our_clip = ConvertOTIOClip(clip, track_position);
                our_track.clips.push_back(our_clip);
                track_position += our_clip.duration;
            }
            else if (auto* gap = dynamic_cast<otio::Gap*>(item.value)) {
                // Create a gap clip
                OTIOClip gap_clip;
                gap_clip.id = "gap_" + std::to_string(our_track.clips.size());
                gap_clip.name = "Gap";
                gap_clip.is_gap = true;
                gap_clip.start_time = track_position;

                // Get gap duration
                otio::ErrorStatus err;
                auto duration_rt = gap->duration(&err);
                if (!otio::is_error(err)) {
                    gap_clip.duration = duration_rt.to_seconds();
                } else {
                    gap_clip.duration = 1.0; // Default 1 second
                }

                gap_clip.source_in = 0.0;
                gap_clip.source_out = gap_clip.duration;

                our_track.clips.push_back(gap_clip);
                track_position += gap_clip.duration;
            }
            else if (auto* transition = dynamic_cast<otio::Transition*>(item.value)) {
                // Mark adjacent clips as having fades
                auto in_offset = transition->in_offset();
                auto out_offset = transition->out_offset();

                // Apply fade to previous clip (fade out)
                if (!our_track.clips.empty()) {
                    auto& prev_clip = our_track.clips.back();
                    prev_clip.has_fade_out = true;
                    prev_clip.fade_out_duration = in_offset.to_seconds();
                }

                // Note: fade in will be applied to next clip when it's processed
                // We'd need more complex logic to handle this properly
            }
        }

        tracks_.push_back(our_track);
    }

    Debug::Log("Extracted " + std::to_string(video_track_num) + " video and " +
               std::to_string(audio_track_num) + " audio tracks");
}

OTIOClip TimelineView::ConvertOTIOClip(otio::Clip* otio_clip, double global_offset) {
    OTIOClip clip;

    clip.id = otio_clip->name().empty() ?
        ("clip_" + std::to_string(global_offset)) : otio_clip->name();
    clip.name = otio_clip->name();
    clip.start_time = global_offset;
    clip.is_gap = false;

    // Get clip duration
    otio::ErrorStatus err;
    auto duration_rt = otio_clip->duration(&err);
    if (!otio::is_error(err)) {
        clip.duration = duration_rt.to_seconds();
    } else {
        clip.duration = 1.0;
    }

    // Get source range (trim points)
    auto source_range = otio_clip->source_range();
    if (source_range.has_value()) {
        clip.source_in = source_range.value().start_time().to_seconds();
        clip.source_out = clip.source_in + source_range.value().duration().to_seconds();
    } else {
        clip.source_in = 0.0;
        clip.source_out = clip.duration;
    }

    // Get media reference for file path
    auto* media_ref = otio_clip->media_reference();
    if (media_ref) {
        if (auto* ext_ref = dynamic_cast<otio::ExternalReference*>(media_ref)) {
            clip.file_path = ext_ref->target_url();

            // If name was empty, use filename from path
            if (clip.name.empty()) {
                size_t last_slash = clip.file_path.find_last_of("/\\");
                if (last_slash != std::string::npos) {
                    clip.name = clip.file_path.substr(last_slash + 1);
                } else {
                    clip.name = clip.file_path;
                }
            }
        }
        else if (auto* img_seq = dynamic_cast<otio::ImageSequenceReference*>(media_ref)) {
            // Image sequence - construct pattern
            clip.file_path = img_seq->target_url_base() + "/" +
                            img_seq->name_prefix() + "####" + img_seq->name_suffix();

            if (clip.name.empty()) {
                clip.name = img_seq->name_prefix() + "[sequence]";
            }
        }
    }

    return clip;
}
#endif

void TimelineView::ExportFlattenedEDL(const std::string& output_path) {
    Debug::Log("Exporting flattened EDL to: " + output_path);
    // TODO: Generate CMX 3600 EDL from flattened result
}

void TimelineView::ExportFlattenedOTIO(const std::string& output_path) {
    Debug::Log("Exporting flattened OTIO to: " + output_path);
    // TODO: Create new OTIO timeline with single V1 track
}

OTIOTrack* TimelineView::GetTrackById(const std::string& track_id) {
    for (auto& track : tracks_) {
        if (track.id == track_id) {
            return &track;
        }
    }
    return nullptr;
}

// ============================================================================
// Clip Query and Editing Helpers
// ============================================================================

OTIOClip* TimelineView::FindClipById(const std::string& clip_id) {
    for (auto& track : tracks_) {
        for (auto& clip : track.clips) {
            if (clip.id == clip_id) {
                return &clip;
            }
        }
    }
    return nullptr;
}

OTIOClip* TimelineView::FindClipById(const std::string& clip_id, int* out_track_index) {
    for (size_t ti = 0; ti < tracks_.size(); ++ti) {
        for (auto& clip : tracks_[ti].clips) {
            if (clip.id == clip_id) {
                if (out_track_index) {
                    *out_track_index = static_cast<int>(ti);
                }
                return &clip;
            }
        }
    }
    return nullptr;
}

std::vector<OTIOClip*> TimelineView::GetClipsAtTime(double time) {
    std::vector<OTIOClip*> result;

    for (auto& track : tracks_) {
        if (!track.is_video) continue;  // Only video tracks for now

        for (auto& clip : track.clips) {
            if (clip.is_gap) continue;

            double clip_end = clip.start_time + clip.duration;
            if (time >= clip.start_time && time < clip_end) {
                result.push_back(&clip);
            }
        }
    }

    return result;
}

int TimelineView::GetTrackIndexForClip(const std::string& clip_id) const {
    for (size_t ti = 0; ti < tracks_.size(); ++ti) {
        for (const auto& clip : tracks_[ti].clips) {
            if (clip.id == clip_id) {
                return static_cast<int>(ti);
            }
        }
    }
    return -1;
}

void TimelineView::RecalculateDuration() {
    timeline_duration_ = 0.0;
    for (const auto& track : tracks_) {
        for (const auto& clip : track.clips) {
            double clip_end = clip.start_time + clip.duration;
            if (clip_end > timeline_duration_) {
                timeline_duration_ = clip_end;
            }
        }
    }
}

void TimelineView::SyncFlattenerAndInvalidate() {
    // Sync the flattener's track copy with the current master tracks
    // This ensures edits (move, trim, cut, delete) are reflected in playback
    // Note: SetTracks() internally calls InvalidateCache()
    flattener_.SetTracks(tracks_);

    // Recalculate timeline duration (edits may have changed it)
    RecalculateDuration();

    // Get the effective playback controller (internal or external for scratch timelines)
    TimelinePlaybackController* controller = GetEffectivePlaybackController();

    // Update durations in playback controller and cache
    if (controller) {
        // Update playback controller's duration (also extends dummy if needed)
        controller->UpdateDuration(timeline_duration_);

        // For scratch timelines, initialize or update the cache when clips are added
        // The cache may not exist yet if this is the first clip
        if (!controller->GetCache()) {
            // First clip added to scratch timeline - initialize the cache
            controller->InitializeCacheForScratchTimeline(this);
        }

        // Update the cache's duration so it knows the new frame count
        if (controller->GetCache()) {
            controller->GetCache()->UpdateDuration(timeline_duration_);
        }

        // Notify the playback controller about the edit
        // This clears the stale current texture AND the cache
        controller->NotifyTracksEdited();
    }
}

void TimelineView::SetExternalPlaybackController(TimelinePlaybackController* controller) {
    external_playback_controller_ = controller;
    Debug::Log("TimelineView: External playback controller " +
               std::string(controller ? "set" : "cleared"));
}

TimelinePlaybackController* TimelineView::GetEffectivePlaybackController() const {
    // Prefer external controller (for scratch timelines) over internal
    if (external_playback_controller_) {
        return external_playback_controller_;
    }
    return playback_controller_.get();
}

void TimelineView::SyncFlattenerOnly() {
    // Lightweight sync for cut operations - updates flattener track data
    // but does NOT invalidate cache since cutting a clip doesn't change
    // the timeline-to-source frame mappings (same frames, just split metadata)
    flattener_.SetTracks(tracks_);
    // No cache invalidation, no duration recalc needed for cuts
}

void TimelineView::SetTracks(const std::vector<OTIOTrack>& tracks,
                              double frame_rate,
                              const std::string& timeline_name,
                              const std::string& source_file_path) {
    // Restore tracks from cached edits (when re-entering a previously edited timeline)
    // This should behave similarly to LoadEDLFile - shutdown existing playback
    // so it can be properly reinitialized with the restored tracks

    // Shutdown existing playback controller first (like LoadEDLFile does)
    // This ensures a fresh dummy video is generated for the restored timeline
    ShutdownPlayback();

    // Restore tracks
    tracks_ = tracks;

    // Restore timeline properties if provided (non-zero/non-empty means restore)
    if (frame_rate > 0.0) {
        frame_rate_ = frame_rate;
    }
    if (!timeline_name.empty()) {
        timeline_name_ = timeline_name;
    }
    if (!source_file_path.empty()) {
        source_file_path_ = source_file_path;
    }

    // Recalculate duration from restored tracks
    RecalculateDuration();

    // Sync flattener with restored tracks
    flattener_.SetTracks(tracks_);

    // Note: InitializePlayback() will be called later by the caller (main.cpp)
    // after verifying linked clips exist, similar to the normal EDL load flow

    Debug::Log("TimelineView::SetTracks: Restored " + std::to_string(tracks_.size()) +
               " tracks, fps=" + std::to_string(frame_rate_) +
               ", duration=" + std::to_string(timeline_duration_) + "s");
}

void TimelineView::CreateMockTimeline() {
    tracks_.clear();

    // Create 3 video tracks
    OTIOTrack v3;
    v3.id = "V3";
    v3.name = "Graphics";
    v3.is_video = true;
    v3.visible = true;
    v3.z_index = 3;

    OTIOClip v3_clip;
    v3_clip.id = "v3_clip1";
    v3_clip.name = "Lower Third";
    v3_clip.file_path = "";
    v3_clip.start_time = 2.0;
    v3_clip.duration = 8.0;
    v3_clip.source_in = 0.0;
    v3_clip.source_out = 8.0;
    v3.clips.push_back(v3_clip);
    tracks_.push_back(v3);

    OTIOTrack v2;
    v2.id = "V2";
    v2.name = "Titles";
    v2.is_video = true;
    v2.visible = true;
    v2.z_index = 2;

    OTIOClip v2_clip;
    v2_clip.id = "v2_clip1";
    v2_clip.name = "Title Card";
    v2_clip.file_path = "";
    v2_clip.start_time = 5.0;
    v2_clip.duration = 15.0;
    v2_clip.source_in = 0.0;
    v2_clip.source_out = 15.0;
    v2.clips.push_back(v2_clip);
    tracks_.push_back(v2);

    OTIOTrack v1;
    v1.id = "V1";
    v1.name = "Main";
    v1.is_video = true;
    v1.visible = true;
    v1.z_index = 1;

    OTIOClip v1_clip;
    v1_clip.id = "v1_clip1";
    v1_clip.name = "Background.mov";
    v1_clip.file_path = "";
    v1_clip.start_time = 0.0;
    v1_clip.duration = 30.0;
    v1_clip.source_in = 0.0;
    v1_clip.source_out = 30.0;
    v1.clips.push_back(v1_clip);
    tracks_.push_back(v1);

    // Create 2 audio tracks
    OTIOTrack a1;
    a1.id = "A1";
    a1.name = "Music";
    a1.is_video = false;
    a1.visible = true;
    a1.muted = false;
    a1.z_index = 0;

    OTIOClip a1_clip;
    a1_clip.id = "a1_clip1";
    a1_clip.name = "Background Music";
    a1_clip.file_path = "";
    a1_clip.start_time = 0.0;
    a1_clip.duration = 25.0;
    a1_clip.source_in = 0.0;
    a1_clip.source_out = 25.0;
    a1.clips.push_back(a1_clip);
    tracks_.push_back(a1);

    OTIOTrack a2;
    a2.id = "A2";
    a2.name = "SFX";
    a2.is_video = false;
    a2.visible = true;
    a2.muted = false;
    a2.z_index = 0;

    OTIOClip a2_clip;
    a2_clip.id = "a2_clip1";
    a2_clip.name = "Sound Effect";
    a2_clip.file_path = "";
    a2_clip.start_time = 8.0;
    a2_clip.duration = 5.0;
    a2_clip.source_in = 0.0;
    a2_clip.source_out = 5.0;
    a2.clips.push_back(a2_clip);
    tracks_.push_back(a2);

    timeline_name_ = "Mock Timeline";
    timeline_duration_ = 30.0;
    frame_rate_ = 24.0;

    flattener_.SetTracks(tracks_);
}

} // namespace ump
