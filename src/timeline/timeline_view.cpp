#include "timeline_view.h"
#include "timeline_playback_controller.h"
#include "timeline_cache.h"
#include "nested_timeline_manager.h"
#include "media_linker.h"
#include "../project/media_item.h"
#include "../project/project_manager.h"
#include "../utils/debug_utils.h"
#include <imgui.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <filesystem>
#include <ctime>
#include <cmath>
#include <map>
#include <functional>

// External fonts from main.cpp for consistent styling
extern ImFont* font_regular;
extern ImFont* font_mono;
extern ImFont* font_icons;  // Material Icons font

// Helper to get Windows accent color (matching main.cpp implementation)
extern ImVec4 GetWindowsAccentColor();

// Material Icons (matching main.cpp definitions)
#define ICON_VISIBILITY             u8"\uE8F4"   // Eye open
#define ICON_VISIBILITY_OFF         u8"\uE8F5"   // Eye with slash
#define ICON_VOLUME_UP              u8"\uE050"   // Speaker on
#define ICON_VOLUME_MUTE            u8"\uE04F"   // Speaker muted
#define ICON_VIEW_TIMELINE          u8"\uEB85"   // Video track icon
#define ICON_AUDIO_TRACK            u8"\uE3A1"   // Audio track icon

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

namespace qcview {

// ============================================================================
// Helper to normalize file URLs from AAF/XML imports
// Handles: file:///C:/path/to/file.mov, file://hostname/path, URL encoding
// ============================================================================
static std::string NormalizeMediaPath(const std::string& url) {
    std::string path = url;

    // Strip file:/// or file:// prefix
    if (path.rfind("file:///", 0) == 0) {
        path = path.substr(8);  // Remove "file:///"
    } else if (path.rfind("file://", 0) == 0) {
        path = path.substr(7);  // Remove "file://"
        // May have hostname, find next slash
        size_t slash = path.find('/');
        if (slash != std::string::npos) {
            path = path.substr(slash + 1);
        }
    }

    // URL decode common sequences
    std::string decoded;
    decoded.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size()) {
            // Decode hex
            char hex[3] = { path[i+1], path[i+2], 0 };
            char* end;
            long val = strtol(hex, &end, 16);
            if (end == hex + 2) {
                decoded += static_cast<char>(val);
                i += 2;
                continue;
            }
        }
        decoded += path[i];
    }

    return decoded;
}

// Extract just the filename from a path (handles both / and \ separators)
static std::string ExtractFilename(const std::string& path) {
    size_t last_slash = path.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        return path.substr(last_slash + 1);
    }
    return path;
}

// ============================================================================
// TimelineFlattener Implementation
// ============================================================================

void TimelineFlattener::SetTracks(const std::vector<OTIOTrack>& tracks) {
    // Debug counting (done before lock to minimize lock hold time)
    int linked_count = 0;
    int total_clips = 0;
    int nested_linked = 0;
    int nested_total = 0;

    std::function<void(const std::vector<OTIOTrack>&, bool)> count_clips =
        [&](const std::vector<OTIOTrack>& trks, bool is_nested) {
        for (const auto& track : trks) {
            for (const auto& clip : track.clips) {
                if (clip.is_gap) continue;

                if (is_nested) {
                    nested_total++;
                    if (clip.is_linked) nested_linked++;
                } else {
                    total_clips++;
                    if (clip.is_linked) linked_count++;
                }

                if (clip.is_nested && clip.nested_loaded) {
                    count_clips(clip.nested_tracks, true);
                }
            }
        }
    };

    count_clips(tracks, false);

    // Update tracks with lock (brief)
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        tracks_ = tracks;
        cache_visible_clips_.clear();  // Invalidate cache
    }

    std::string msg = "Flattener::SetTracks: " + std::to_string(linked_count) + "/" +
                      std::to_string(total_clips) + " clips linked";
    if (nested_total > 0) {
        msg += " (nested: " + std::to_string(nested_linked) + "/" + std::to_string(nested_total) + ")";
    }
    Debug::Log(msg);
}

void TimelineFlattener::SetTrackVisibility(const std::string& track_id, bool visible) {
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    for (auto& track : tracks_) {
        if (track.id == track_id) {
            track.visible = visible;
            cache_visible_clips_.clear();  // Invalidate cache
            break;
        }
    }
}

void TimelineFlattener::SetTrackMute(const std::string& track_id, bool muted) {
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    for (auto& track : tracks_) {
        if (track.id == track_id) {
            track.muted = muted;
            // Audio doesn't need cache invalidation (separate from video)
            break;
        }
    }
}

void TimelineFlattener::EnableVisibilityOverrides(bool enable) {
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    use_visibility_overrides_ = enable;
    cache_visible_clips_.clear();  // Invalidate cache
}

void TimelineFlattener::SetVisibilityOverride(const std::string& track_id, bool visible) {
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    visibility_overrides_[track_id] = visible;
    cache_visible_clips_.clear();  // Invalidate cache
}

bool TimelineFlattener::IsTrackVisible(const OTIOTrack& track) const {
    // Must be called with lock held
    if (use_visibility_overrides_) {
        auto it = visibility_overrides_.find(track.id);
        if (it != visibility_overrides_.end()) {
            return it->second;
        }
        // No override for this track - default to false in override mode
        // This ensures unknown tracks are hidden unless explicitly enabled
        return false;
    }
    return track.visible;
}

std::vector<OTIOTrack> TimelineFlattener::GetTracks() const {
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    return tracks_;  // Return copy for thread safety
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
    std::lock_guard<std::mutex> lock(tracks_mutex_);

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
    std::lock_guard<std::mutex> lock(tracks_mutex_);

    // Find the topmost visible clip using z_index for priority
    // Higher z_index = higher visual priority (V2 above V1)
    const OTIOClip* best_clip = nullptr;
    int best_z_index = -1;

    for (const auto& track : tracks_) {
        if (!track.is_video || !IsTrackVisible(track)) continue;

        const OTIOClip* clip = FindClipInTrack(track, timestamp);
        if (clip && !clip->is_gap) {
            // Use z_index to determine priority (higher = on top)
            if (track.z_index > best_z_index) {
                best_z_index = track.z_index;

                // If this is a nested clip, flatten into it to find the actual media
                if (clip->is_nested && clip->nested_loaded && !clip->nested_tracks.empty()) {
                    const OTIOClip* nested_clip = GetVisibleClipInNest(clip, timestamp);
                    if (nested_clip && nested_clip->is_linked) {
                        best_clip = nested_clip;
                    } else {
                        // Nested clip has no linked content at this time - treat as gap
                        best_clip = nullptr;
                    }
                } else if (clip->is_linked) {
                    best_clip = clip;
                }
            }
        }
    }

    return best_clip;
}

const OTIOClip* TimelineFlattener::GetClipFromTrackAtTime(const std::string& track_id, double timestamp) {
    std::lock_guard<std::mutex> lock(tracks_mutex_);

    // Find the specified track
    for (const auto& track : tracks_) {
        if (track.id == track_id) {
            // Found the track, now find clip at this time
            const OTIOClip* clip = FindClipInTrack(track, timestamp);
            if (clip && !clip->is_gap) {
                // Handle nested clips
                if (clip->is_nested && clip->nested_loaded && !clip->nested_tracks.empty()) {
                    const OTIOClip* nested_clip = GetVisibleClipInNest(clip, timestamp);
                    if (nested_clip && nested_clip->is_linked) {
                        return nested_clip;
                    }
                    return nullptr;
                } else if (clip->is_linked) {
                    return clip;
                }
            }
            return nullptr;  // Track found but no clip at this time
        }
    }
    return nullptr;  // Track not found
}

const OTIOClip* TimelineFlattener::GetVisibleClipInNest(const OTIOClip* nest_clip, double timeline_timestamp) {
    // Calculate the relative timestamp within the nested composition
    // timeline_timestamp is absolute, nest_clip->start_time is where the nest starts on timeline
    double relative_time = timeline_timestamp - nest_clip->start_time;

    // Clamp to nested duration
    if (relative_time < 0 || relative_time >= nest_clip->duration) {
        return nullptr;
    }

    // Find the topmost visible clip within the nested tracks at the relative time
    const OTIOClip* best_clip = nullptr;
    int best_z_index = -1;

    for (const auto& track : nest_clip->nested_tracks) {
        if (!track.is_video || !track.visible) continue;

        const OTIOClip* clip = FindClipInTrack(track, relative_time);
        if (clip && !clip->is_gap) {
            if (track.z_index > best_z_index) {
                best_z_index = track.z_index;

                // Handle deeply nested clips (nest within nest)
                if (clip->is_nested && clip->nested_loaded && !clip->nested_tracks.empty()) {
                    // Recursively flatten - calculate time relative to this inner nest
                    const OTIOClip* inner_clip = GetVisibleClipInNest(clip, clip->start_time + relative_time);
                    if (inner_clip && inner_clip->is_linked) {
                        best_clip = inner_clip;
                    }
                } else if (clip->is_linked) {
                    best_clip = clip;
                }
            }
        }
    }

    return best_clip;
}

const OTIOClip* TimelineFlattener::GetAudibleClipAtTime(double timestamp) {
    std::lock_guard<std::mutex> lock(tracks_mutex_);

    // Find the topmost visible clip from a track that is NOT audio_muted
    // This is for audio playback - respects video track audio mute button AND clip-level mute
    const OTIOClip* best_clip = nullptr;
    int best_z_index = -1;

    for (const auto& track : tracks_) {
        // Must be video track, visible, and NOT track-level audio muted
        if (!track.is_video || !track.visible || track.audio_muted) continue;

        const OTIOClip* clip = FindClipInTrack(track, timestamp);
        // Check clip exists, not a gap, AND not clip-level muted
        if (clip && !clip->is_gap && !clip->audio_muted) {
            // Use z_index to determine priority (higher = on top)
            if (track.z_index > best_z_index) {
                best_z_index = track.z_index;

                // If this is a nested clip, flatten into it to find the actual media
                if (clip->is_nested && clip->nested_loaded && !clip->nested_tracks.empty()) {
                    const OTIOClip* nested_clip = GetVisibleClipInNest(clip, timestamp);
                    if (nested_clip && nested_clip->is_linked && !nested_clip->audio_muted) {
                        best_clip = nested_clip;
                    } else {
                        best_clip = nullptr;
                    }
                } else if (clip->is_linked) {
                    best_clip = clip;
                }
            }
        }
    }

    return best_clip;
}

std::vector<std::string> TimelineFlattener::GetAudibleClipPathsAtTime(double timestamp) {
    std::lock_guard<std::mutex> lock(tracks_mutex_);

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

std::vector<OTIOClip> TimelineFlattener::GetAllAudibleClipsAtTime(double timestamp) {
    std::lock_guard<std::mutex> lock(tracks_mutex_);

    std::vector<OTIOClip> audible_clips;

    for (const auto& track : tracks_) {
        // For VIDEO tracks: include if visible and NOT audio_muted
        // For AUDIO tracks: include if NOT muted
        bool include_track = false;
        if (track.is_video) {
            include_track = track.visible && !track.audio_muted;
        } else {
            include_track = !track.muted;
        }

        if (!include_track) continue;

        const OTIOClip* clip = FindClipInTrack(track, timestamp);
        if (!clip || clip->is_gap) continue;

        // Check clip-level audio mute
        if (clip->audio_muted) continue;

        // Handle nested clips - get the actual media clip inside
        if (clip->is_nested && clip->nested_loaded && !clip->nested_tracks.empty()) {
            const OTIOClip* nested_clip = GetVisibleClipInNest(clip, timestamp);
            // For nested clips, check if it has any usable path
            if (nested_clip && !nested_clip->audio_muted &&
                (!nested_clip->linked_path.empty() || !nested_clip->file_path.empty())) {
                // Return a COPY - pointer becomes invalid after mutex is released
                audible_clips.push_back(*nested_clip);
            }
        } else {
            // For regular clips, include if it has any usable path (linked or file_path)
            // The audio mixer will handle the actual path resolution
            if (!clip->linked_path.empty() || !clip->file_path.empty()) {
                // Return a COPY - pointer becomes invalid after mutex is released
                audible_clips.push_back(*clip);
            }
        }
    }

    return audible_clips;
}

void TimelineFlattener::InvalidateCache() {
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    cache_visible_clips_.clear();
}

// ============================================================================
// TimelineView Implementation
// ============================================================================

TimelineView::TimelineView(::VideoPlayer* player)
    : video_player_(player)
    , nested_manager_(std::make_unique<NestedTimelineManager>()) {
}

TimelineView::~TimelineView() {
    ShutdownPlayback();
}

bool TimelineView::InitializePlayback() {
    if (!video_player_) {
        Debug::Log("TimelineView::InitializePlayback: Invalid video player");
        return false;
    }

    if (tracks_.empty()) {
        Debug::Log("TimelineView::InitializePlayback: No tracks loaded");
        return false;
    }

    // Count clips with usable media paths (linked or file_path)
    // This allows playback to work for imported timelines (AAF/XML/OTIO)
    // that have file_path set even before explicit media linking
    int usable_count = 0;
    for (const auto& track : tracks_) {
        for (const auto& clip : track.clips) {
            if (!clip.is_gap && (!clip.linked_path.empty() || !clip.file_path.empty())) {
                usable_count++;
            }
        }
    }

    if (usable_count == 0) {
        Debug::Log("TimelineView::InitializePlayback: No clips with media paths - link media first");
        return false;
    }

    Debug::Log("TimelineView::InitializePlayback: Initializing with " +
               std::to_string(usable_count) + " clips with media paths" +
               ", pipeline_mode=" + std::string(PipelineModeToString(pending_pipeline_mode_)));

    // Create and initialize playback controller
    playback_controller_ = std::make_unique<TimelinePlaybackController>();

    // Set pipeline mode and video codec from pending (set by caller before InitializePlayback)
    TimelinePlaybackConfig config = playback_controller_->GetConfig();
    config.pipeline_mode = pending_pipeline_mode_;
    config.video_codec = pending_video_codec_;  // For MF HDR eligibility check (HEVC only)
    config.timecode_start_frame = pending_timecode_start_frame_;  // For MF HDR frame alignment
    playback_controller_->SetConfig(config);

    // Use virtual timeline mode (no dummy video required)
    // Pass canvas dimensions for consistent output sizing (prevents flickering with mixed resolutions)
    if (!playback_controller_->InitializeForVirtualTimeline(this, video_player_,
                                                             canvas_width_, canvas_height_)) {
        Debug::Log("TimelineView::InitializePlayback: Failed to initialize virtual timeline controller");
        playback_controller_.reset();
        return false;
    }

    Debug::Log("TimelineView::InitializePlayback: Virtual timeline playback initialized successfully");
    return true;
}

void TimelineView::ShutdownPlayback() {
    // Clear external controller reference first (prevents stale pointer issues)
    // This is critical: when switching modes, the external controller may point to
    // scratch_timeline_controller which is about to be destroyed
    if (external_playback_controller_) {
        Debug::Log("TimelineView::ShutdownPlayback: Clearing external playback controller");
        external_playback_controller_ = nullptr;
    }

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

void TimelineView::SetTimelineLooping(bool enabled) {
    timeline_loop_enabled_ = enabled;

    // Also tell the playback controller so it can update loop state
    auto* controller = GetEffectivePlaybackController();
    if (controller) {
        controller->SetLooping(enabled);
        Debug::Log("TimelineView: Set loop " + std::string(enabled ? "ON" : "OFF") +
                   " (propagated to playback controller)");
    } else {
        Debug::Log("TimelineView: Set loop " + std::string(enabled ? "ON" : "OFF") +
                   " (no playback controller)");
    }
}

//=============================================================================
// Zoom/Pan Control
//=============================================================================

void TimelineView::SetZoomLevel(float zoom) {
    // Calculate minimum zoom so timeline fills the visible width exactly
    // This prevents zooming out past the full timeline range
    float min_zoom = 2.5f;
    if (timeline_duration_ > 0 && last_visible_width_ > 0) {
        // Minimum zoom = visible_width / duration (exactly fills the viewport)
        min_zoom = last_visible_width_ / static_cast<float>(timeline_duration_);
    } else if (timeline_duration_ > 0) {
        // Fallback: ensure at least 200 pixels wide
        min_zoom = std::max(min_zoom, 200.0f / static_cast<float>(timeline_duration_));
    }

    // Clamp to reasonable range
    if (zoom < min_zoom) zoom = min_zoom;
    if (zoom > 1400.0f) zoom = 1400.0f;
    zoom_level_ = zoom;

    // Clamp scroll offset to new valid range (prevents view from going outside timeline bounds)
    float max_offset = GetMaxScrollOffset();
    if (scroll_offset_x_ > max_offset) {
        scroll_offset_x_ = max_offset;
    }
}

void TimelineView::SetZoomLevelAroundTime(float zoom, double time) {
    // Calculate minimum zoom so timeline fills the visible width exactly
    float min_zoom = 2.5f;
    if (timeline_duration_ > 0 && last_visible_width_ > 0) {
        min_zoom = last_visible_width_ / static_cast<float>(timeline_duration_);
    } else if (timeline_duration_ > 0) {
        min_zoom = std::max(min_zoom, 200.0f / static_cast<float>(timeline_duration_));
    }

    // Clamp to reasonable range
    if (zoom < min_zoom) zoom = min_zoom;
    if (zoom > 1400.0f) zoom = 1400.0f;

    float old_zoom = zoom_level_;
    if (old_zoom <= 0) old_zoom = 50.0f;

    // Calculate where the time position appears on screen before zoom
    float time_in_timeline_old = static_cast<float>(time) * old_zoom;
    float time_screen_x = time_in_timeline_old - scroll_offset_x_;

    // Apply new zoom
    zoom_level_ = zoom;

    // Calculate where time position should be in new zoomed timeline
    float time_in_timeline_new = static_cast<float>(time) * zoom_level_;

    // Adjust scroll to keep time at same screen position
    scroll_offset_x_ = time_in_timeline_new - time_screen_x;

    // Clamp scroll offset to valid range
    float max_offset = GetMaxScrollOffset();
    if (scroll_offset_x_ < 0) scroll_offset_x_ = 0;
    if (scroll_offset_x_ > max_offset) scroll_offset_x_ = max_offset;
}

void TimelineView::FitZoomToWidth(float visible_width) {
    if (timeline_duration_ <= 0 || visible_width <= 0) {
        return;
    }

    // Store visible width for zoom limit calculations
    last_visible_width_ = visible_width;

    // Calculate zoom so timeline duration exactly fills visible width
    // No padding - viewport indicator should align with minimap clip bar
    float new_zoom = visible_width / static_cast<float>(timeline_duration_);

    // Clamp to reasonable range (min zoom = fit to width, can't zoom out further)
    if (new_zoom > 1400.0f) new_zoom = 1400.0f;

    zoom_level_ = new_zoom;
    scroll_offset_x_ = 0.0f;  // Reset scroll to beginning

    Debug::Log("FitZoomToWidth: duration=" + std::to_string(timeline_duration_) +
               "s, width=" + std::to_string(visible_width) +
               ", zoom=" + std::to_string(new_zoom) + " px/s");
}

void TimelineView::SetInitialZoomForDuration() {
    // Set a reasonable initial zoom based on timeline duration
    // Assumes ~1000px panel width - will be refined by FitZoomToWidth later
    if (timeline_duration_ > 0) {
        float estimated_width = 1000.0f * 0.95f;  // 5% padding like FitZoomToWidth
        zoom_level_ = estimated_width / static_cast<float>(timeline_duration_);
        if (zoom_level_ < 2.5f) zoom_level_ = 2.5f;
        if (zoom_level_ > 1400.0f) zoom_level_ = 1400.0f;
        scroll_offset_x_ = 0.0f;
    }
}

void TimelineView::UpdateVisibleWidth(float width) {
    // Handle proportional zoom adjustment when window resizes
    // This keeps the same "view" - if you were seeing 10% of the timeline, you still see 10%

    // Skip if this is the first update or width is invalid
    if (last_visible_width_ <= 0 || width <= 0) {
        last_visible_width_ = width;
        return;
    }

    // Calculate width change ratio
    float width_ratio = width / last_visible_width_;

    // Only adjust if the change is significant (>1% change to avoid jitter)
    // This also handles DPI changes smoothly since they cause proportional width changes
    if (std::abs(width_ratio - 1.0f) > 0.01f) {
        // Calculate the center time of the current view (what we want to keep centered)
        float old_visible_duration = last_visible_width_ / zoom_level_;
        float center_time = (scroll_offset_x_ / zoom_level_) + (old_visible_duration * 0.5f);

        // Proportionally adjust zoom level to maintain the same visible time range
        float new_zoom = zoom_level_ * width_ratio;

        // Calculate minimum zoom (can't zoom out past full timeline)
        float min_zoom = 2.5f;
        if (timeline_duration_ > 0) {
            min_zoom = width / static_cast<float>(timeline_duration_);
        }

        // Clamp to valid range
        if (new_zoom < min_zoom) new_zoom = min_zoom;
        if (new_zoom > 1400.0f) new_zoom = 1400.0f;

        // Calculate new scroll to keep the center time in the center
        float new_visible_duration = width / new_zoom;
        float new_scroll = (center_time - new_visible_duration * 0.5f) * new_zoom;

        // Clamp scroll to valid range
        float max_scroll = std::max(0.0f, static_cast<float>(timeline_duration_) * new_zoom - width);
        if (new_scroll < 0) new_scroll = 0;
        if (new_scroll > max_scroll) new_scroll = max_scroll;

        // Apply the new zoom and scroll
        zoom_level_ = new_zoom;
        scroll_offset_x_ = new_scroll;
    }

    // Always update the cached width
    last_visible_width_ = width;
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

    // Update any in-progress playlist edits (trim/slip/move)
    UpdatePlaylistEdit();

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
            // Hide thumbnails option for PLAYLIST mode (not applicable)
            if (source_mode_ != TimelineSourceMode::PLAYLIST) {
                ImGui::MenuItem("Show Thumbnails", nullptr, &show_thumbnails_);
            }
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
    
    // Timeline info - prefix based on source mode (hide when no media loaded)
    if (!timeline_name_.empty()) {
        const char* prefix = "Timeline";
        switch (source_mode_) {
            case TimelineSourceMode::IMAGE_SEQUENCE: prefix = "Image Sequence"; break;
            case TimelineSourceMode::VIDEO_FILE:
                // Check if audio-only (no video tracks)
                prefix = (GetVideoTrackCount() > 0) ? "Video" : "Audio";
                break;
            case TimelineSourceMode::DUAL_VIEW: prefix = "Comparison"; break;
            default: prefix = "Timeline"; break;
        }
        ImGui::Text("%s: %s", prefix, timeline_name_.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%.2f fps, %.2fs duration, %d video tracks, %d audio tracks)",
                            frame_rate_, timeline_duration_,
                            GetVideoTrackCount(), GetAudioTrackCount());
    }
    
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

        // Hide audio tracks in Audio-only mode (no video tracks, just audio file)
        // The A1 track is not needed for audio-only playback
        if (source_mode_ == TimelineSourceMode::VIDEO_FILE &&
            GetVideoTrackCount() == 0 &&
            !track.is_video) {
            continue;  // Skip this audio track
        }

        ImGui::PushID(track.id.c_str());

        RenderTrackHeader(track, i);
        ImGui::SameLine();
        
        // Track clips area
        ImGui::BeginChild(("TrackClips_" + track.id).c_str(),
                         ImVec2(0, track_height_), true);

        RenderTrackClips(track, track_height_);

        // Drag-drop target for PLAYLIST mode - accept media items dropped onto video track
        if (source_mode_ == TimelineSourceMode::PLAYLIST && track.is_video) {
            if (ImGui::BeginDragDropTarget()) {
                // Accept single media item
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEM")) {
                    std::string media_id((const char*)payload->Data);
                    Debug::Log("Timeline drop: single item " + media_id);
                    if (drop_media_callback_) {
                        drop_media_callback_({media_id});
                    }
                }
                // Accept multiple media items
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEMS_MULTI")) {
                    std::string payload_str((const char*)payload->Data);
                    std::vector<std::string> media_ids;
                    size_t pos = 0;
                    while ((pos = payload_str.find(';')) != std::string::npos) {
                        media_ids.push_back(payload_str.substr(0, pos));
                        payload_str.erase(0, pos + 1);
                    }
                    if (!payload_str.empty()) {
                        media_ids.push_back(payload_str);
                    }
                    Debug::Log("Timeline drop: " + std::to_string(media_ids.size()) + " items");
                    if (drop_media_callback_) {
                        drop_media_callback_(media_ids);
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }

        ImGui::EndChild();

        ImGui::PopID();
    }
}

void TimelineView::RenderTrackHeader(OTIOTrack& track, int track_index) {
    ImGui::BeginGroup();

    // DEBUG: Check if font_icons is available (log once per session)
    static bool logged_font_status = false;
    if (!logged_font_status) {
        Debug::Log("TimelineView: font_icons = " + std::string(font_icons ? "valid" : "NULL"));
        logged_font_status = true;
    }

    // Track name with Material Icon (need icon font for the icon part)
    if (font_icons) {
        ImGui::PushFont(font_icons);
        const char* track_icon = track.is_video ? ICON_VIEW_TIMELINE : ICON_AUDIO_TRACK;
        ImGui::Text("%s", track_icon);
        ImGui::PopFont();
        ImGui::SameLine(0, 4);
    }
    ImGui::Text("%s", track.name.c_str());

    if (track.is_video) {
        // Video track: Eye icon for visibility
        ImGui::SameLine();
        const char* vis_icon = track.visible ? ICON_VISIBILITY : ICON_VISIBILITY_OFF;
        ImGui::PushID("vis");
        if (font_icons) ImGui::PushFont(font_icons);
        if (ImGui::SmallButton(vis_icon)) {
            HandleTrackVisibilityToggle(track.id);
        }
        if (font_icons) ImGui::PopFont();
        ImGui::PopID();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(track.visible ? "Hide track" : "Show track");
        }

        // Video track: Speaker icon for audio mute
        {
            ImGui::SameLine();
            const char* audio_icon = track.audio_muted ? ICON_VOLUME_MUTE : ICON_VOLUME_UP;
            ImGui::PushID("audio");
            if (font_icons) ImGui::PushFont(font_icons);
            if (ImGui::SmallButton(audio_icon)) {
                track.audio_muted = !track.audio_muted;
                // Sync to flattener so AudioMixer picks up the change
                SyncFlattenerOnly();
                Debug::Log("Track " + track.name + " audio " + (track.audio_muted ? "muted" : "unmuted"));
            }
            if (font_icons) ImGui::PopFont();
            ImGui::PopID();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(track.audio_muted ? "Unmute track audio" : "Mute track audio");
            }
        }
    } else {
        // Audio track: Speaker icon for mute
        {
            ImGui::SameLine();
            const char* mute_icon = track.muted ? ICON_VOLUME_MUTE : ICON_VOLUME_UP;
            ImGui::PushID("mute");
            if (font_icons) ImGui::PushFont(font_icons);
            if (ImGui::SmallButton(mute_icon)) {
                HandleTrackMuteToggle(track.id);
            }
            if (font_icons) ImGui::PopFont();
            ImGui::PopID();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(track.muted ? "Unmute track" : "Mute track");
            }
        }
    }

    // Solo button (uses regular text font)
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
    const float trim_handle_width = 8.0f;  // Width of trim handles in pixels

    int clip_index = 0;
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

            // PLAYLIST mode: Trim/Slip/Move editing
            if (source_mode_ == TimelineSourceMode::PLAYLIST && track.is_video && !clip.is_gap) {
                // Define regions
                bool in_left_handle = (mouse_pos.x >= x_start && mouse_pos.x <= x_start + trim_handle_width);
                bool in_right_handle = (mouse_pos.x >= x_end - trim_handle_width && mouse_pos.x <= x_end);
                bool in_clip_body = (mouse_pos.x > x_start + trim_handle_width &&
                                     mouse_pos.x < x_end - trim_handle_width);

                // Visual feedback for trim handles when hovering (only if no edit in progress)
                if (playlist_edit_op_ == PlaylistEditOp::NONE) {
                    if (in_left_handle || in_right_handle) {
                        ImU32 handle_color = IM_COL32(255, 200, 100, 180);
                        if (in_left_handle) {
                            draw_list->AddRectFilled(ImVec2(x_start, y_top),
                                                     ImVec2(x_start + trim_handle_width, y_bottom),
                                                     handle_color, 2.0f);
                        }
                        if (in_right_handle) {
                            draw_list->AddRectFilled(ImVec2(x_end - trim_handle_width, y_top),
                                                     ImVec2(x_end, y_bottom),
                                                     handle_color, 2.0f);
                        }
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    }
                }

                // Start edit on mouse down (only if no edit is in progress)
                if (playlist_edit_op_ == PlaylistEditOp::NONE && !playlist_pending_edit_ && ImGui::IsMouseClicked(0)) {
                    bool shift_held = ImGui::GetIO().KeyShift;

                    // Select the clip on any click (PLAYLIST mode uses single track 0)
                    selection_.SelectClip(clip.id, 0, ImGui::GetIO().KeyCtrl);

                    if (in_left_handle) {
                        // Set up pending TRIM_START - will start after drag threshold
                        playlist_pending_edit_ = true;
                        playlist_pending_op_ = PlaylistEditOp::TRIM_START;
                        playlist_edit_clip_id_ = clip.id;
                        playlist_edit_clip_index_ = clip_index;
                        playlist_edit_start_mouse_x_ = mouse_pos.x;
                        playlist_edit_original_in_ = clip.source_in;
                        playlist_edit_original_out_ = clip.source_out;
                        playlist_edit_original_start_ = clip.start_time;
                    } else if (in_right_handle) {
                        // Set up pending TRIM_END - will start after drag threshold
                        playlist_pending_edit_ = true;
                        playlist_pending_op_ = PlaylistEditOp::TRIM_END;
                        playlist_edit_clip_id_ = clip.id;
                        playlist_edit_clip_index_ = clip_index;
                        playlist_edit_start_mouse_x_ = mouse_pos.x;
                        playlist_edit_original_in_ = clip.source_in;
                        playlist_edit_original_out_ = clip.source_out;
                        playlist_edit_original_start_ = clip.start_time;
                    } else if (in_clip_body) {
                        // Clicking on clip body just selects - no edit setup
                        // Shift+drag for SLIP and regular drag for MOVE are disabled
                        // to prevent accidental edits. Use trim handles for editing.
                        // Selection was already done above.
                    }
                }
            } else if (ImGui::IsMouseClicked(0)) {
                // Non-PLAYLIST mode: standard click handling
                HandleClipClick(clip);
            }
        }

        clip_index++;
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

            // Use font_regular for consistent styling
            if (font_regular) {
                ImVec2 text_size = ImGui::CalcTextSize(time_label);
                draw_list->AddText(font_regular, 12.0f,
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
    // Skip if no playback controller
    TimelinePlaybackController* controller = GetEffectivePlaybackController();
    if (!controller) {
        return;
    }

    // Get both caches (LEFT is always available, RIGHT only in DUAL_VIEW)
    TimelineCache* left_cache = controller->GetCache();
    TimelineCache* right_cache = controller->GetRightCache();  // nullptr if not DUAL_VIEW

    // Check which caches have image content to display
    bool left_has_content = left_cache && left_cache->IsInitialized() && left_cache->HasImageContent();
    bool right_has_content = right_cache && right_cache->IsInitialized() && right_cache->HasImageContent();

    if (!left_has_content && !right_has_content) {
        return;
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Use saved ruler position from RenderTimelineRuler, not current cursor
    ImVec2 ruler_pos = ruler_screen_pos_;
    float ruler_width = ruler_width_;

    // Cache bar dimensions
    const float cache_bar_height = 6.0f;
    float cache_bar_y = ruler_pos.y + 28.0f;  // Just below time labels

    // In DUAL_VIEW with both caches, stack them vertically
    const float bar_spacing = 2.0f;
    float left_bar_y = cache_bar_y;
    float right_bar_y = cache_bar_y + cache_bar_height + bar_spacing;

    // Helper lambda to render a single cache bar
    auto renderCacheSegments = [&](TimelineCache* cache, float bar_y, ImU32 color, const char* label) {
        if (!cache) return;

        auto segments = cache->GetCacheSegments();
        auto stats = cache->GetStats();

        // Draw background bar (dark gray)
        draw_list->AddRectFilled(
            ImVec2(ruler_pos.x, bar_y),
            ImVec2(ruler_pos.x + ruler_width, bar_y + cache_bar_height),
            IM_COL32(50, 50, 50, 255)
        );

        // Draw cached segments
        for (const auto& segment : segments) {
            float start_x = ruler_pos.x + (float)(segment.start_time * zoom_level_) - scroll_offset_x_;
            float end_x = ruler_pos.x + (float)(segment.end_time * zoom_level_) - scroll_offset_x_;

            // Clamp to visible region
            start_x = std::max(start_x, ruler_pos.x);
            end_x = std::min(end_x, ruler_pos.x + ruler_width);

            if (end_x <= start_x) continue;

            draw_list->AddRectFilled(
                ImVec2(start_x, bar_y),
                ImVec2(end_x, bar_y + cache_bar_height),
                color
            );
        }

        // Show stats on hover
        ImVec2 mouse_pos = ImGui::GetMousePos();
        if (mouse_pos.y >= bar_y && mouse_pos.y <= bar_y + cache_bar_height &&
            mouse_pos.x >= ruler_pos.x && mouse_pos.x <= ruler_pos.x + ruler_width) {
            ImGui::BeginTooltip();
            ImGui::Text("%s Cache", label);
            ImGui::Separator();
            if (stats.total_timeline_frames > 0) {
                float percent = (float)stats.cached_frames / stats.total_timeline_frames * 100.0f;
                ImGui::Text("Cached: %d / %d frames (%.1f%%)",
                            stats.cached_frames, stats.total_timeline_frames, percent);
            } else {
                ImGui::Text("Cached frames: %d", stats.cached_frames);
            }
            ImGui::Text("Duration: %.2fs", stats.timeline_duration);
            ImGui::Text("Cache size: %.1f MB", stats.cache_bytes / (1024.0 * 1024.0));
            ImGui::Text("Hit ratio: %.1f%%", stats.GetHitRatio() * 100.0);
            ImGui::Text("Pending: %d", stats.pending_requests);
            ImGui::EndTooltip();
        }
    };

    // Render LEFT cache bar (green)
    if (left_has_content) {
        ImU32 left_color = IM_COL32(80, 200, 120, 204);  // Green
        renderCacheSegments(left_cache, left_bar_y, left_color, "LEFT");
    }

    // Render RIGHT cache bar (cyan) - only in DUAL_VIEW with image content
    if (right_has_content) {
        ImU32 right_color = IM_COL32(80, 180, 220, 204);  // Cyan to distinguish from LEFT
        renderCacheSegments(right_cache, right_bar_y, right_color, "RIGHT");
    }
}

void TimelineView::RenderClipTooltip(const OTIOClip& clip) {
    // Match playlist tooltip styling
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(50, 50, 50, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(100, 100, 100, 51));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 9.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);

    ImGui::BeginTooltip();

    if (font_regular) ImGui::PushFont(font_regular);

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

    if (font_regular) ImGui::PopFont();

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

    // Legacy mode: Load individual clips for playback (no cache)
    const OTIOClip* visible_clip = flattener_.GetVisibleClipAtTime(current_time_);

    if (visible_clip && !visible_clip->is_gap) {
        LoadFlattenedClipForPlayback(*visible_clip);
    }
}

void TimelineView::LoadFlattenedClipForPlayback(const OTIOClip& clip) {
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
    Debug::Log("Loading flattened clip for playback: " + edl_path);
    
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

void TimelineView::SetFrameRate(double fps) {
    if (fps > 0) {
        frame_rate_ = fps;
        Debug::Log("TimelineView: Frame rate updated to " + std::to_string(fps));
    }
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

    // Use full sync and invalidation path (same as other edits)
    // This ensures playback controller, caches, and dual view flatteners are all updated
    SyncFlattenerAndInvalidate();

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

void TimelineView::AutoMuteVideoClipsWithAudio() {
    // Auto-mute video clips that have embedded audio on video tracks
    // This is useful for imported timelines that likely have their own audio track layout
    int muted_count = 0;

    for (auto& track : tracks_) {
        if (!track.is_video) continue;  // Only process video tracks

        for (auto& clip : track.clips) {
            if (clip.is_gap) continue;

            // If the clip has audio (detected during probe/link), auto-mute it
            if (clip.has_audio && !clip.audio_muted) {
                clip.audio_muted = true;
                muted_count++;
            }
        }
    }

    if (muted_count > 0) {
        Debug::Log("AutoMuteVideoClipsWithAudio: Muted " + std::to_string(muted_count) +
                   " video clips with embedded audio");
        flattener_.SetTracks(tracks_);
    }
}

void TimelineView::InitializeForScratch(const std::string& name, double duration, double fps,
                                        int width, int height) {
    Debug::Log("Initializing scratch timeline: " + name + " (" +
               std::to_string(duration) + "s @ " + std::to_string(fps) + "fps, " +
               std::to_string(width) + "x" + std::to_string(height) + ")");

    // Reset source mode to VIDEO_FILE (default) for scratch timelines
    ResetSourceMode();

    // Clear existing data
    tracks_.clear();
    source_file_path_.clear();  // No source file for scratch timelines

    // Set timeline properties
    timeline_name_ = name.empty() ? "New Timeline" : name;
    timeline_duration_ = duration;
    frame_rate_ = fps;
    canvas_width_ = width;
    canvas_height_ = height;

    // Set initial zoom based on duration (will be refined by FitZoomToWidth later)
    SetInitialZoomForDuration();

    // Create one empty video track and one empty audio track
    OTIOTrack video_track;
    video_track.id = "V1";
    video_track.name = "V1";
    video_track.is_video = true;
    video_track.visible = true;
    video_track.muted = false;
    video_track.z_index = 1;
    tracks_.push_back(video_track);

    OTIOTrack audio_track;
    audio_track.id = "A1";
    audio_track.name = "A1";
    audio_track.is_video = false;
    audio_track.visible = true;
    audio_track.muted = false;
    audio_track.z_index = 0;
    tracks_.push_back(audio_track);

    // Update flattener with empty tracks
    flattener_.SetTracks(tracks_);

    Debug::Log("Scratch timeline initialized: " + timeline_name_);
}

// ============================================================================
// Image Sequence as Timeline
// ============================================================================

bool TimelineView::LoadImageSequenceAsTimeline(MediaItem* item) {
    if (!item) {
        Debug::Log("LoadImageSequenceAsTimeline: null MediaItem");
        return false;
    }

    Debug::Log("Loading image sequence as timeline: " + item->name);

    // Set source mode
    source_mode_ = TimelineSourceMode::IMAGE_SEQUENCE;
    source_media_item_ = item;
    source_media_item_id_ = item ? item->id : "";

    // Clear existing data
    tracks_.clear();
    source_file_path_.clear();  // No OTIO source file

    // Calculate timeline properties from sequence (use image_seq with legacy fallbacks)
    double fps = item->image_seq.frame_rate;
    if (fps <= 0) fps = item->frame_rate;  // Legacy fallback
    if (fps <= 0) fps = 24.0;  // Default fallback

    int start_frame = item->image_seq.start_frame;
    if (start_frame <= 0) start_frame = item->start_frame;  // Legacy fallback
    if (start_frame <= 0) start_frame = 1;

    int end_frame = item->image_seq.end_frame;
    if (end_frame <= 0) end_frame = item->end_frame;  // Legacy fallback

    int total_frames = item->image_seq.frame_count;
    if (total_frames <= 0) total_frames = item->frame_count;  // Legacy fallback
    if (total_frames <= 0 && end_frame > 0) {
        total_frames = end_frame - start_frame + 1;
    }
    if (total_frames <= 0) total_frames = 1;  // Safety

    double duration = static_cast<double>(total_frames) / fps;

    // Get dimensions (use image_seq with legacy fallbacks)
    int width = item->image_seq.width;
    if (width <= 0) width = item->sequence_width;  // Legacy fallback
    if (width <= 0) width = 1920;

    int height = item->image_seq.height;
    if (height <= 0) height = item->sequence_height;  // Legacy fallback
    if (height <= 0) height = 1080;

    // Set timeline properties
    timeline_name_ = item->name;
    // Use extended duration if audio clips extended past sequence
    double effective_duration = duration;
    if (item->cached_timeline_duration > duration) {
        effective_duration = item->cached_timeline_duration;
        Debug::Log("  Using extended timeline duration: " + std::to_string(effective_duration) +
                   "s (sequence was " + std::to_string(duration) + "s)");
    }
    timeline_duration_ = effective_duration;
    frame_rate_ = fps;
    canvas_width_ = width;
    canvas_height_ = height;

    Debug::Log("  fps=" + std::to_string(fps) + ", frames=" + std::to_string(total_frames) +
               ", duration=" + std::to_string(duration) + "s, " +
               std::to_string(width) + "x" + std::to_string(height));

    // Create video track with single clip - LOCKED
    OTIOTrack video_track;
    video_track.id = "V1";
    video_track.name = "V1";
    video_track.is_video = true;
    video_track.visible = true;
    video_track.muted = false;
    video_track.locked = true;  // Video track is locked in IMAGE_SEQUENCE mode
    video_track.z_index = 1;

    // Get sequence directory and pattern (with legacy fallbacks)
    std::string seq_directory = item->image_seq.directory;
    if (seq_directory.empty()) {
        // Extract from path
        std::string path = item->path;
        if (path.substr(0, 5) == "mf://") path = path.substr(5);
        else if (path.substr(0, 6) == "exr://") {
            size_t query_pos = path.find("?layer=");
            if (query_pos != std::string::npos) path = path.substr(6, query_pos - 6);
            else path = path.substr(6);
        }
        size_t last_slash = path.find_last_of("/\\");
        if (last_slash != std::string::npos) {
            seq_directory = path.substr(0, last_slash);
        }
    }

    std::string seq_pattern = item->image_seq.pattern;
    if (seq_pattern.empty()) seq_pattern = item->sequence_pattern;  // Legacy fallback

    std::string exr_layer = item->image_seq.layer;
    if (exr_layer.empty()) exr_layer = item->exr_layer;  // Legacy fallback

    // Create the sequence clip
    OTIOClip clip;
    clip.id = "seq_clip_1";
    clip.name = item->name;
    clip.file_path = item->path;  // mf:// or exr:// URL
    clip.is_sequence = true;
    clip.sequence_directory = seq_directory;
    clip.sequence_pattern = seq_pattern;
    clip.sequence_start_frame = start_frame;
    clip.sequence_end_frame = end_frame;
    clip.sequence_exr_layer = exr_layer;
    clip.start_time = 0.0;
    clip.duration = duration;
    clip.source_in = 0.0;
    clip.source_out = duration;
    clip.source_duration = duration;
    clip.source_fps = fps;
    clip.source_width = width;
    clip.source_height = height;
    clip.is_linked = true;
    clip.linked_path = item->path;
    clip.is_gap = false;

    video_track.clips.push_back(clip);
    tracks_.push_back(video_track);

    // Create audio track - EDITABLE
    // Check if there are cached audio tracks from a previous session
    OTIOTrack audio_track;
    bool restored_audio = false;
    if (item->has_cached_edits && !item->cached_tracks.empty()) {
        // Find the audio track in cached_tracks
        for (const auto& cached_track : item->cached_tracks) {
            if (!cached_track.is_video) {
                audio_track = cached_track;
                audio_track.locked = false;  // Ensure it's editable
                restored_audio = true;
                Debug::Log("LoadImageSequenceAsTimeline: Restored audio track with " +
                           std::to_string(audio_track.clips.size()) + " clips");
                break;
            }
        }
    }

    // If no cached audio track, create an empty one
    if (!restored_audio) {
        audio_track.id = "A1";
        audio_track.name = "A1";
        audio_track.is_video = false;
        audio_track.visible = true;
        audio_track.muted = false;
        audio_track.locked = false;  // Audio track is editable
        audio_track.z_index = 0;
    }
    tracks_.push_back(audio_track);

    // Update flattener with tracks
    flattener_.SetTracks(tracks_);

    Debug::Log("LoadImageSequenceAsTimeline: Created " + std::to_string(tracks_.size()) + " tracks");
    for (size_t i = 0; i < tracks_.size(); i++) {
        Debug::Log("  Track[" + std::to_string(i) + "]: " + tracks_[i].name +
                   ", is_video=" + std::to_string(tracks_[i].is_video) +
                   ", clips=" + std::to_string(tracks_[i].clips.size()));
        for (const auto& c : tracks_[i].clips) {
            Debug::Log("    Clip: " + c.name + ", duration=" + std::to_string(c.duration) +
                       ", linked=" + std::to_string(c.is_linked) +
                       ", path=" + c.linked_path.substr(0, 50));
        }
    }

    // Reset view state
    current_time_ = 0.0;
    scroll_offset_x_ = 0.0f;
    timeline_in_point_ = -1.0;
    timeline_out_point_ = -1.0;

    // Set initial zoom based on duration (will be refined by FitZoomToWidth later)
    SetInitialZoomForDuration();

    Debug::Log("Image sequence timeline created: " + timeline_name_ +
               " (" + std::to_string(total_frames) + " frames @ " +
               std::to_string(fps) + " fps = " + std::to_string(duration) + "s)" +
               ", initial_zoom=" + std::to_string(zoom_level_) + " px/s");

    return true;
}

bool TimelineView::LoadVideoFileAsTimeline(MediaItem* item) {
    if (!item) {
        Debug::Log("LoadVideoFileAsTimeline: null MediaItem");
        return false;
    }

    Debug::Log("Loading video file as timeline: " + item->name);

    // Set source mode
    source_mode_ = TimelineSourceMode::VIDEO_FILE;
    source_media_item_ = item;
    source_media_item_id_ = item ? item->id : "";

    // Clear existing data
    tracks_.clear();
    source_file_path_.clear();  // No OTIO source file

    // Get timeline properties from MediaItem (populated by FFmpegMetadataExtractor)
    double fps = item->frame_rate;
    if (fps <= 0) fps = 24.0;  // Default fallback

    double duration = item->duration;
    if (duration <= 0) duration = 1.0;  // Safety

    // Get dimensions from MediaItem (populated when file was added to project)
    int width = item->timeline_width;
    int height = item->timeline_height;
    if (width <= 0) width = 1920;   // Default fallback
    if (height <= 0) height = 1080;

    // Set timeline properties
    timeline_name_ = item->name;
    timeline_duration_ = duration;
    frame_rate_ = fps;
    canvas_width_ = width;
    canvas_height_ = height;

    Debug::Log("  fps=" + std::to_string(fps) + ", duration=" + std::to_string(duration) +
               "s, " + std::to_string(width) + "x" + std::to_string(height));

    // Create video track with single clip - LOCKED
    OTIOTrack video_track;
    video_track.id = "V1";
    video_track.name = "V1";
    video_track.is_video = true;
    video_track.visible = true;
    video_track.muted = false;
    video_track.locked = true;  // Video track is locked in VIDEO_FILE mode
    video_track.z_index = 1;

    // Create the video clip
    OTIOClip video_clip;
    video_clip.id = "video_clip_1";
    video_clip.name = item->name;
    video_clip.file_path = item->path;
    video_clip.start_time = 0.0;
    video_clip.duration = duration;
    video_clip.source_in = 0.0;
    video_clip.source_out = duration;
    video_clip.source_duration = duration;
    video_clip.source_fps = fps;
    video_clip.source_width = width;
    video_clip.source_height = height;
    video_clip.is_linked = true;
    video_clip.linked_path = item->path;
    video_clip.is_gap = false;
    video_clip.is_sequence = false;  // Not an image sequence
    video_clip.has_audio = item->has_audio;  // Show speaker icon only if video has audio

    video_track.clips.push_back(video_clip);
    tracks_.push_back(video_track);

    // No separate audio track for VIDEO_FILE mode - audio comes from the video clip itself
    // The speaker icon on the clip allows muting if needed

    // Update flattener with tracks
    flattener_.SetTracks(tracks_);

    Debug::Log("LoadVideoFileAsTimeline: Created " + std::to_string(tracks_.size()) + " tracks");
    for (size_t i = 0; i < tracks_.size(); i++) {
        Debug::Log("  Track[" + std::to_string(i) + "]: " + tracks_[i].name +
                   ", is_video=" + std::to_string(tracks_[i].is_video) +
                   ", clips=" + std::to_string(tracks_[i].clips.size()));
    }

    // Reset view state
    current_time_ = 0.0;
    scroll_offset_x_ = 0.0f;
    timeline_in_point_ = -1.0;
    timeline_out_point_ = -1.0;

    // Set initial zoom based on duration (will be refined by FitZoomToWidth later)
    SetInitialZoomForDuration();

    Debug::Log("Video file timeline created: " + timeline_name_ +
               " (duration=" + std::to_string(duration) + "s @ " +
               std::to_string(fps) + " fps)" +
               ", initial_zoom=" + std::to_string(zoom_level_) + " px/s");

    return true;
}

bool TimelineView::LoadAudioFileAsTimeline(MediaItem* item) {
    if (!item) {
        Debug::Log("LoadAudioFileAsTimeline: null MediaItem");
        return false;
    }

    Debug::Log("Loading audio file as timeline: " + item->name);

    // Set source mode to VIDEO_FILE for direct playback
    // Audio-only files won't produce video frames (black display)
    source_mode_ = TimelineSourceMode::VIDEO_FILE;
    source_media_item_ = item;
    source_media_item_id_ = item ? item->id : "";

    // Clear existing data
    tracks_.clear();
    source_file_path_.clear();  // No OTIO source file

    // Get timeline properties from MediaItem
    // Audio files don't have a natural frame rate, use a default for timeline display
    double fps = 24.0;  // Default for timeline visualization
    if (item->frame_rate > 0) fps = item->frame_rate;  // Use if available

    double duration = item->duration;
    if (duration <= 0) duration = 1.0;  // Safety

    // Set timeline properties (dummy canvas for audio-only files)
    timeline_name_ = item->name;
    timeline_duration_ = duration;
    frame_rate_ = fps;
    canvas_width_ = 1920;   // Dummy canvas for audio-only
    canvas_height_ = 1080;

    Debug::Log("  fps=" + std::to_string(fps) + " (display), duration=" + std::to_string(duration) + "s");

    // Create audio track with single clip - LOCKED
    OTIOTrack audio_track;
    audio_track.id = "A1";
    audio_track.name = "A1";
    audio_track.is_video = false;  // Audio track
    audio_track.visible = true;
    audio_track.muted = false;
    audio_track.locked = true;  // Audio track is locked in AUDIO_FILE mode
    audio_track.z_index = 0;    // Audio tracks have lower z_index

    // Create the audio clip
    OTIOClip audio_clip;
    audio_clip.id = "audio_clip_1";
    audio_clip.name = item->name;
    audio_clip.file_path = item->path;
    audio_clip.start_time = 0.0;
    audio_clip.duration = duration;
    audio_clip.source_in = 0.0;
    audio_clip.source_out = duration;
    audio_clip.source_duration = duration;
    audio_clip.source_fps = fps;
    audio_clip.source_width = 0;   // No video
    audio_clip.source_height = 0;
    audio_clip.is_linked = true;
    audio_clip.linked_path = item->path;
    audio_clip.is_gap = false;
    audio_clip.is_sequence = false;
    audio_clip.has_audio = true;  // It's an audio file

    audio_track.clips.push_back(audio_clip);
    tracks_.push_back(audio_track);

    // Update flattener with tracks
    flattener_.SetTracks(tracks_);

    Debug::Log("LoadAudioFileAsTimeline: Created " + std::to_string(tracks_.size()) + " tracks");
    for (size_t i = 0; i < tracks_.size(); i++) {
        Debug::Log("  Track[" + std::to_string(i) + "]: " + tracks_[i].name +
                   ", is_video=" + std::to_string(tracks_[i].is_video) +
                   ", clips=" + std::to_string(tracks_[i].clips.size()));
    }

    // Reset view state
    current_time_ = 0.0;
    scroll_offset_x_ = 0.0f;
    timeline_in_point_ = -1.0;
    timeline_out_point_ = -1.0;

    // Set initial zoom based on duration
    SetInitialZoomForDuration();

    Debug::Log("Audio file timeline created: " + timeline_name_ +
               " (duration=" + std::to_string(duration) + "s)" +
               ", initial_zoom=" + std::to_string(zoom_level_) + " px/s");

    return true;
}

// ============================================================================
// PLAYLIST AS TIMELINE - Unified single-track playlist with ripple editing
// ============================================================================

bool TimelineView::LoadPlaylistAsTimeline(MediaItem* item) {
    if (!item) {
        Debug::Log("LoadPlaylistAsTimeline: null MediaItem");
        return false;
    }

    if (item->type != qcview::MediaType::PLAYLIST) {
        Debug::Log("LoadPlaylistAsTimeline: MediaItem is not a playlist");
        return false;
    }

    Debug::Log("Loading playlist as timeline: " + item->name +
               " with " + std::to_string(item->playlist_items.size()) + " items");

    // Set source mode to PLAYLIST for ripple editing behavior
    source_mode_ = TimelineSourceMode::PLAYLIST;
    source_media_item_ = item;
    source_media_item_id_ = item ? item->id : "";

    // Clear existing data
    tracks_.clear();
    source_file_path_.clear();  // No OTIO source file

    // Get timeline properties - will be updated as we process items
    double fps = 23.976;  // Default, will be updated from first media item
    double total_duration = 0.0;
    int max_width = 1920;
    int max_height = 1080;
    bool fps_set = false;

    // Default gap duration (10 frames at default fps)
    const int gap_frames = 10;
    double gap_duration = gap_frames / fps;

    // Create video track
    OTIOTrack video_track;
    video_track.id = "V1";
    video_track.name = "Playlist";
    video_track.is_video = true;
    video_track.visible = true;
    video_track.muted = false;
    video_track.locked = false;  // Allow editing in PLAYLIST mode
    video_track.z_index = 1;

    // No initial gap - first clip starts at 0
    total_duration = 0.0;

    // Process each playlist item and create clips
    for (size_t i = 0; i < item->playlist_items.size(); i++) {
        const auto& entry = item->playlist_items[i];

        OTIOClip clip;
        clip.id = "playlist_clip_" + std::to_string(i);

        // Try to resolve the referenced MediaItem via ProjectManager
        MediaItem* media_item = nullptr;
        if (project_manager_) {
            media_item = project_manager_->GetMediaItem(entry.media_id);
            Debug::Log("  Looking up media_id='" + entry.media_id + "' -> " +
                       (media_item ? ("found: " + media_item->name) : "NOT FOUND"));
        } else {
            Debug::Log("  project_manager_ is NULL!");
        }

        if (media_item) {
            // Successfully resolved - populate clip from media item
            clip.name = media_item->name;
            clip.file_path = media_item->path;
            clip.linked_path = media_item->path;
            clip.is_linked = true;
            clip.has_audio = media_item->has_audio;

            // Get duration and fps from media item
            double media_duration = media_item->duration;
            double media_fps = media_item->frame_rate;

            // Handle image sequences
            if (media_item->type == MediaType::IMAGE_SEQUENCE ||
                media_item->type == MediaType::EXR_SEQUENCE) {
                clip.is_sequence = true;
                clip.sequence_directory = media_item->image_seq.directory;
                clip.sequence_pattern = media_item->image_seq.pattern;
                clip.sequence_start_frame = media_item->image_seq.start_frame;
                clip.sequence_end_frame = media_item->image_seq.end_frame;
                clip.sequence_exr_layer = media_item->image_seq.layer;
                clip.source_width = media_item->image_seq.width;
                clip.source_height = media_item->image_seq.height;
                media_duration = media_item->image_seq.duration;
                media_fps = media_item->image_seq.frame_rate;

                // Use ffmpeg pattern for playback
                if (!media_item->image_seq.ffmpeg_pattern.empty()) {
                    clip.file_path = media_item->image_seq.ffmpeg_pattern;
                    clip.linked_path = media_item->image_seq.ffmpeg_pattern;
                }
            } else {
                // Video file - use timeline dimensions if available
                clip.source_width = media_item->timeline_width > 0 ? media_item->timeline_width : 1920;
                clip.source_height = media_item->timeline_height > 0 ? media_item->timeline_height : 1080;
            }

            clip.source_fps = media_fps > 0 ? media_fps : 23.976;
            clip.source_duration = media_duration > 0 ? media_duration : 10.0;

            // Use first valid fps for timeline
            if (!fps_set && media_fps > 0) {
                fps = media_fps;
                gap_duration = gap_frames / fps;
                fps_set = true;
            }

            // Track max dimensions
            if (clip.source_width > max_width) max_width = clip.source_width;
            if (clip.source_height > max_height) max_height = clip.source_height;

            // Apply in/out points if specified
            clip.source_in = (entry.in_point >= 0) ? entry.in_point : 0.0;
            clip.source_out = (entry.out_point >= 0) ? entry.out_point : clip.source_duration;
            clip.duration = clip.source_out - clip.source_in;

            Debug::Log("  Resolved clip " + std::to_string(i) + ": " + clip.name +
                       " (" + std::to_string(clip.duration) + "s)");
        } else {
            // Could not resolve - create placeholder
            clip.name = "Unlinked Clip " + std::to_string(i + 1);
            clip.file_path = "playlist_item://" + entry.media_id;
            clip.is_linked = false;

            double clip_duration = 5.0;  // Placeholder duration
            clip.source_in = (entry.in_point >= 0) ? entry.in_point : 0.0;
            clip.source_out = (entry.out_point >= 0) ? entry.out_point : clip_duration;
            clip.duration = clip.source_out - clip.source_in;
            clip.source_duration = clip_duration;

            Debug::Log("  Unresolved clip " + std::to_string(i) + ": media_id=" + entry.media_id);
        }

        clip.start_time = total_duration;
        clip.is_gap = false;

        video_track.clips.push_back(clip);
        total_duration = clip.start_time + clip.duration;

        // Add gap between clips (except after last clip)
        if (i < item->playlist_items.size() - 1) {
            OTIOClip gap;
            gap.id = "gap_" + std::to_string(i);
            gap.name = "Gap";
            gap.start_time = total_duration;
            gap.duration = gap_duration;
            gap.is_gap = true;
            video_track.clips.push_back(gap);
            total_duration += gap_duration;
        }
    }

    tracks_.push_back(video_track);

    // Set timeline properties
    timeline_name_ = item->name;
    timeline_duration_ = total_duration;
    frame_rate_ = fps;
    canvas_width_ = max_width;
    canvas_height_ = max_height;

    // Update flattener with tracks
    flattener_.SetTracks(tracks_);

    Debug::Log("LoadPlaylistAsTimeline: Created " + std::to_string(video_track.clips.size()) +
               " clips, duration=" + std::to_string(total_duration) + "s");

    // Reset view state
    current_time_ = 0.0;
    scroll_offset_x_ = 0.0f;
    timeline_in_point_ = -1.0;
    timeline_out_point_ = -1.0;

    // Set initial zoom based on duration
    SetInitialZoomForDuration();

    Debug::Log("Playlist timeline created: " + timeline_name_ +
               " (duration=" + std::to_string(total_duration) + "s)" +
               ", initial_zoom=" + std::to_string(zoom_level_) + " px/s");

    return true;
}

// ============================================================================
// DUAL VIEW MODE - Side-by-side comparison timeline
// ============================================================================

void TimelineView::InitializeForDualView(const std::string& name, double fps) {
    Debug::Log("InitializeForDualView: " + name + " at " + std::to_string(fps) + " fps");

    // Set source mode
    source_mode_ = TimelineSourceMode::DUAL_VIEW;
    source_media_item_ = nullptr;
    source_media_item_id_.clear();

    // Clear existing data
    tracks_.clear();
    source_file_path_.clear();

    // Set timeline properties
    timeline_name_ = name;
    timeline_duration_ = 0.0;  // Will be set when media is loaded
    frame_rate_ = fps;
    canvas_width_ = 1920;   // Default HD
    canvas_height_ = 1080;

    // Create LEFT video track
    OTIOTrack left_track;
    left_track.id = "left";
    left_track.name = "LEFT";
    left_track.is_video = true;
    left_track.visible = true;
    left_track.muted = false;
    left_track.locked = false;
    left_track.z_index = 1;
    tracks_.push_back(left_track);

    // Create RIGHT video track
    OTIOTrack right_track;
    right_track.id = "right";
    right_track.name = "RIGHT";
    right_track.is_video = true;
    right_track.visible = true;
    right_track.muted = false;
    right_track.locked = false;
    right_track.audio_muted = true;  // Mute RIGHT track audio by default (user can enable)
    right_track.z_index = 0;
    tracks_.push_back(right_track);

    // Update flattener with tracks
    flattener_.SetTracks(tracks_);

    // Reset view state
    current_time_ = 0.0;
    scroll_offset_x_ = 0.0f;
    timeline_in_point_ = -1.0;
    timeline_out_point_ = -1.0;

    Debug::Log("InitializeForDualView: Created LEFT and RIGHT tracks");
}

bool TimelineView::LoadMediaToLeftTrack(MediaItem* item) {
    if (!item) {
        Debug::Log("LoadMediaToLeftTrack: null MediaItem");
        return false;
    }

    OTIOTrack* left = GetLeftTrack();
    if (!left) {
        Debug::Log("LoadMediaToLeftTrack: LEFT track not found");
        return false;
    }

    Debug::Log("LoadMediaToLeftTrack: " + item->name);

    // Clear existing clips on LEFT track
    left->clips.clear();

    // Determine duration and source info based on media type
    double duration = item->duration;
    double fps = item->frame_rate > 0 ? item->frame_rate : frame_rate_;
    int width = item->sequence_width;
    int height = item->sequence_height;
    std::string linked_path = item->path;
    bool is_sequence = false;
    std::string seq_dir, seq_pattern;
    int seq_start = 0, seq_end = 0;

    if (item->type == MediaType::IMAGE_SEQUENCE || item->type == MediaType::EXR_SEQUENCE) {
        is_sequence = true;
        if (item->image_seq.IsValid()) {
            duration = item->image_seq.duration;
            fps = item->image_seq.frame_rate;
            width = item->image_seq.width;
            height = item->image_seq.height;
            seq_dir = item->image_seq.directory;
            seq_pattern = item->image_seq.pattern;
            seq_start = item->image_seq.start_frame;
            seq_end = item->image_seq.end_frame;
            // Use path as-is (already contains mf:// or exr:// URL)
            linked_path = item->path;
        }
    }

    if (duration <= 0) duration = 1.0;

    // Create clip for LEFT track
    OTIOClip clip;
    clip.id = "left_clip_1";
    clip.name = item->name;
    clip.file_path = item->path;
    clip.start_time = 0.0;
    clip.duration = duration;
    clip.source_in = 0.0;
    clip.source_out = duration;
    clip.source_duration = duration;
    clip.source_fps = fps;
    clip.source_width = width;
    clip.source_height = height;
    clip.is_linked = true;
    clip.linked_path = linked_path;
    clip.is_gap = false;
    clip.is_sequence = is_sequence;
    clip.sequence_directory = seq_dir;
    clip.sequence_pattern = seq_pattern;
    clip.sequence_start_frame = seq_start;
    clip.sequence_end_frame = seq_end;
    clip.has_audio = item->has_audio;

    left->clips.push_back(clip);

    // Update canvas dimensions from first loaded media
    if (width > 0 && height > 0) {
        canvas_width_ = width;
        canvas_height_ = height;
    }

    // Sync flattener and notify playback controller (also syncs dual flatteners)
    SyncFlattenerAndInvalidate();

    // Request fit zoom to update for new duration
    RequestFitZoomOnNextRender();

    Debug::Log("LoadMediaToLeftTrack: Added clip, duration=" + std::to_string(duration));
    return true;
}

bool TimelineView::LoadMediaToRightTrack(MediaItem* item) {
    if (!item) {
        Debug::Log("LoadMediaToRightTrack: null MediaItem");
        return false;
    }

    OTIOTrack* right = GetRightTrack();
    if (!right) {
        Debug::Log("LoadMediaToRightTrack: RIGHT track not found");
        return false;
    }

    Debug::Log("LoadMediaToRightTrack: " + item->name);

    // Clear existing clips on RIGHT track
    right->clips.clear();

    // Determine duration and source info based on media type
    double duration = item->duration;
    double fps = item->frame_rate > 0 ? item->frame_rate : frame_rate_;
    int width = item->sequence_width;
    int height = item->sequence_height;
    std::string linked_path = item->path;
    bool is_sequence = false;
    std::string seq_dir, seq_pattern;
    int seq_start = 0, seq_end = 0;

    if (item->type == MediaType::IMAGE_SEQUENCE || item->type == MediaType::EXR_SEQUENCE) {
        is_sequence = true;
        if (item->image_seq.IsValid()) {
            duration = item->image_seq.duration;
            fps = item->image_seq.frame_rate;
            width = item->image_seq.width;
            height = item->image_seq.height;
            seq_dir = item->image_seq.directory;
            seq_pattern = item->image_seq.pattern;
            seq_start = item->image_seq.start_frame;
            seq_end = item->image_seq.end_frame;
            // Use path as-is (already contains mf:// or exr:// URL)
            linked_path = item->path;
        }
    }

    if (duration <= 0) duration = 1.0;

    // Create clip for RIGHT track
    OTIOClip clip;
    clip.id = "right_clip_1";
    clip.name = item->name;
    clip.file_path = item->path;
    clip.start_time = 0.0;
    clip.duration = duration;
    clip.source_in = 0.0;
    clip.source_out = duration;
    clip.source_duration = duration;
    clip.source_fps = fps;
    clip.source_width = width;
    clip.source_height = height;
    clip.is_linked = true;
    clip.linked_path = linked_path;
    clip.is_gap = false;
    clip.is_sequence = is_sequence;
    clip.sequence_directory = seq_dir;
    clip.sequence_pattern = seq_pattern;
    clip.sequence_start_frame = seq_start;
    clip.sequence_end_frame = seq_end;
    clip.has_audio = item->has_audio;

    right->clips.push_back(clip);

    // Sync flattener and notify playback controller (also syncs dual flatteners)
    SyncFlattenerAndInvalidate();

    // Request fit zoom to update for new duration
    RequestFitZoomOnNextRender();

    Debug::Log("LoadMediaToRightTrack: Added clip, duration=" + std::to_string(duration));
    return true;
}

OTIOTrack* TimelineView::GetLeftTrack() {
    for (auto& track : tracks_) {
        if (track.id == "left" || track.name == "LEFT") {
            return &track;
        }
    }
    return nullptr;
}

OTIOTrack* TimelineView::GetRightTrack() {
    for (auto& track : tracks_) {
        if (track.id == "right" || track.name == "RIGHT") {
            return &track;
        }
    }
    return nullptr;
}

void TimelineView::ResetSourceMode() {
    source_mode_ = TimelineSourceMode::VIDEO_FILE;
    source_media_item_ = nullptr;
    source_media_item_id_.clear();

    // Unlock all tracks
    for (auto& track : tracks_) {
        track.locked = false;
    }
}

MediaItem* TimelineView::GetSourceMediaItem() {
    // If we have an ID and a project manager, refresh the pointer to prevent stale references
    // This is necessary because media_pool is a vector and may reallocate when items are added
    if (!source_media_item_id_.empty() && project_manager_) {
        MediaItem* old_ptr = source_media_item_;
        source_media_item_ = project_manager_->GetMediaItem(source_media_item_id_);
        if (old_ptr != source_media_item_) {
            Debug::Log("[GetSourceMediaItem] Pointer refreshed: old=" +
                       std::to_string(reinterpret_cast<uintptr_t>(old_ptr)) + " new=" +
                       std::to_string(reinterpret_cast<uintptr_t>(source_media_item_)) +
                       " id=" + source_media_item_id_);
        }
    }
    return source_media_item_;
}

bool TimelineView::IsVideoTrackLocked() const {
    return source_mode_ == TimelineSourceMode::IMAGE_SEQUENCE ||
           source_mode_ == TimelineSourceMode::VIDEO_FILE;
}

bool TimelineView::CanAddVideoClips() const {
    return source_mode_ != TimelineSourceMode::IMAGE_SEQUENCE &&
           source_mode_ != TimelineSourceMode::VIDEO_FILE;
}

bool TimelineView::CanRemoveVideoClips() const {
    return source_mode_ != TimelineSourceMode::IMAGE_SEQUENCE &&
           source_mode_ != TimelineSourceMode::VIDEO_FILE;
}

bool TimelineView::CanAddAudioClips() const {
    // Audio clips can be added in PLAYLIST and IMAGE_SEQUENCE modes
    // In VIDEO_FILE mode, audio track is locked (video has its own embedded audio)
    return source_mode_ != TimelineSourceMode::VIDEO_FILE;
}

bool TimelineView::CanEditClip(const OTIOClip& clip, const OTIOTrack& track) const {
    // In IMAGE_SEQUENCE or VIDEO_FILE mode, video track clips cannot be edited
    if ((source_mode_ == TimelineSourceMode::IMAGE_SEQUENCE ||
         source_mode_ == TimelineSourceMode::VIDEO_FILE) && track.is_video) {
        return false;
    }
    // In VIDEO_FILE mode, audio track clips also cannot be edited
    if (source_mode_ == TimelineSourceMode::VIDEO_FILE && !track.is_video) {
        return false;
    }
    // Also check track-level lock
    if (track.locked) {
        return false;
    }
    return true;
}

#ifdef USE_OPENTIMELINEIO
OTIOClip TimelineView::ConvertOTIOClip(otio::Clip* otio_clip, double global_offset) {
    OTIOClip clip;

    // Generate unique ID using clip pointer address to ensure uniqueness
    // even when multiple clips have the same name (e.g., linked video+audio from same source)
    clip.id = "clip_" + std::to_string(reinterpret_cast<uintptr_t>(otio_clip)) + "_" +
              std::to_string(global_offset);
    clip.name = otio_clip->name();
    clip.start_time = global_offset;
    clip.is_gap = false;

    // Extract AAF metadata from clip (needed for MXF file matching)
    auto& clip_metadata = otio_clip->metadata();
    if (clip_metadata.has_key("AAF")) {
        try {
            auto aaf_any = clip_metadata["AAF"];
            if (aaf_any.type() == typeid(otio::AnyDictionary)) {
                auto aaf_dict = std::any_cast<otio::AnyDictionary>(aaf_any);
                for (auto ait = aaf_dict.begin(); ait != aaf_dict.end(); ++ait) {
                    // Log ALL string AAF fields for debugging
                    if (ait->second.type() == typeid(std::string)) {
                        std::string val = std::any_cast<std::string>(ait->second);
                        Debug::Log("  Clip AAF." + ait->first + " = " + val);
                        if (ait->first == "MobID") {
                            clip.aaf_mob_id = val;
                        }
                    }
                }
            }
        } catch (...) {}
    }

    // Get clip duration
    otio::ErrorStatus err;
    auto duration_rt = otio_clip->duration(&err);
    if (!otio::is_error(err)) {
        clip.duration = duration_rt.to_seconds();
        // Extract source frame rate from the duration's rate (this is the source rate)
        // Note: This is a hint - actual source fps will be confirmed when media is probed
        if (duration_rt.rate() > 0) {
            clip.source_fps = duration_rt.rate();
        }
    } else {
        clip.duration = 1.0;
    }

    // Get source range (trim points)
    auto source_range = otio_clip->source_range();
    if (source_range.has_value()) {
        clip.source_in = source_range.value().start_time().to_seconds();
        clip.source_out = clip.source_in + source_range.value().duration().to_seconds();
        // Also try to get source fps from source range if not already set
        if (clip.source_fps <= 0 && source_range.value().start_time().rate() > 0) {
            clip.source_fps = source_range.value().start_time().rate();
        }
    } else {
        clip.source_in = 0.0;
        clip.source_out = clip.duration;
    }

    // Get media reference for file path
    auto* media_ref = otio_clip->media_reference();
    if (media_ref) {
        // Log all metadata on the media reference (AAF stores source info here)
        auto& ref_metadata = media_ref->metadata();
        for (auto it = ref_metadata.begin(); it != ref_metadata.end(); ++it) {
            Debug::Log("  MediaRef metadata key: " + it->first);
            // Try to log string values
            if (it->second.type() == typeid(std::string)) {
                try {
                    Debug::Log("    = " + std::any_cast<std::string>(it->second));
                } catch (...) {}
            }
        }

        if (auto* ext_ref = dynamic_cast<otio::ExternalReference*>(media_ref)) {
            // Get the raw URL and normalize it (handle file:/// prefix, URL encoding)
            std::string raw_url = ext_ref->target_url();
            clip.file_path = NormalizeMediaPath(raw_url);

            // Extract just the filename for matching purposes
            std::string filename = ExtractFilename(clip.file_path);

            // Check for AAF source clip name in metadata
            // AAF adapter often stores original source info in metadata
            if (ref_metadata.has_key("AAF")) {
                try {
                    auto aaf_any = ref_metadata["AAF"];
                    if (aaf_any.type() == typeid(otio::AnyDictionary)) {
                        auto aaf_dict = std::any_cast<otio::AnyDictionary>(aaf_any);
                        for (auto it = aaf_dict.begin(); it != aaf_dict.end(); ++it) {
                            // Log ALL AAF fields for debugging
                            if (it->second.type() == typeid(std::string)) {
                                std::string val = std::any_cast<std::string>(it->second);
                                Debug::Log("  MediaRef AAF." + it->first + " = " + val);
                                // Capture MobID for linking
                                if (it->first == "MobID") {
                                    clip.aaf_mob_id = val;
                                }
                            }
                        }
                    }
                } catch (...) {}
            }

            // If name was empty or looks like a full path, use extracted filename
            // This helps AAF/XML imports where clip names may be paths
            if (clip.name.empty()) {
                clip.name = filename;
            } else if (clip.name.find('/') != std::string::npos ||
                       clip.name.find('\\') != std::string::npos ||
                       clip.name.find("file:") == 0) {
                // Name looks like a path, normalize it too
                clip.name = ExtractFilename(NormalizeMediaPath(clip.name));
            }

            Debug::Log("ConvertOTIOClip: name='" + clip.name +
                       "' file_path='" + clip.file_path + "'");
        }
        else if (auto* img_seq = dynamic_cast<otio::ImageSequenceReference*>(media_ref)) {
            // Image sequence - construct pattern
            std::string base = NormalizeMediaPath(img_seq->target_url_base());
            clip.file_path = base + "/" +
                            img_seq->name_prefix() + "####" + img_seq->name_suffix();

            if (clip.name.empty()) {
                clip.name = img_seq->name_prefix() + "[sequence]";
            }
        }
    }

    return clip;
}

// Recursively parse nested stack into nested_tracks and link media
// This is called eagerly during import so all nested clips are linked upfront
void TimelineView::ParseNestedStack(OTIOClip& nest_clip, otio::Stack* nested_stack, const std::string& source_dir) {
    if (!nested_stack) return;

    Debug::Log("ParseNestedStack: Parsing '" + nest_clip.name + "' eagerly");

    int video_idx = 0, audio_idx = 0;

    for (auto& child : nested_stack->children()) {
        auto* track = dynamic_cast<otio::Track*>(child.value);
        if (!track) continue;

        OTIOTrack our_track;
        std::string kind = track->kind();
        our_track.is_video = (kind == otio::Track::Kind::video);

        if (our_track.is_video) {
            video_idx++;
            our_track.name = "V" + std::to_string(video_idx);
            our_track.id = "nested_" + nest_clip.id + "_V" + std::to_string(video_idx);
            our_track.z_index = video_idx;
        } else {
            audio_idx++;
            our_track.name = "A" + std::to_string(audio_idx);
            our_track.id = "nested_" + nest_clip.id + "_A" + std::to_string(audio_idx);
            our_track.z_index = 0;
        }

        our_track.visible = true;
        our_track.muted = false;

        // Process track clips
        double track_position = 0.0;
        for (auto& item : track->children()) {
            if (auto* otio_clip = dynamic_cast<otio::Clip*>(item.value)) {
                OTIOClip our_clip = ConvertOTIOClip(otio_clip, track_position);
                our_track.clips.push_back(our_clip);
                track_position += our_clip.duration;
            }
            else if (auto* inner_stack = dynamic_cast<otio::Stack*>(item.value)) {
                // Nested within nested! Create clip and parse recursively
                OTIOClip inner_nest;
                inner_nest.id = "nested_" + std::to_string(our_track.clips.size()) + "_" +
                               std::to_string(reinterpret_cast<uintptr_t>(inner_stack));
                inner_nest.is_nested = true;
                inner_nest.nested_name = inner_stack->name().empty() ?
                                        "Nested Sequence" : inner_stack->name();
                inner_nest.name = inner_nest.nested_name;
                inner_nest.start_time = track_position;
                inner_nest.is_gap = false;

                otio::ErrorStatus nest_err;
                auto nest_dur = inner_stack->duration(&nest_err);
                if (!otio::is_error(nest_err)) {
                    inner_nest.duration = nest_dur.to_seconds();
                    inner_nest.nested_fps = nest_dur.rate();
                } else {
                    inner_nest.duration = 1.0;
                }

                inner_nest.source_in = 0.0;
                inner_nest.source_out = inner_nest.duration;

                // Recursively parse this inner nested stack
                ParseNestedStack(inner_nest, inner_stack, source_dir);

                our_track.clips.push_back(inner_nest);
                track_position += inner_nest.duration;

                Debug::Log("ParseNestedStack: Found nested-within-nested: " + inner_nest.name);
            }
            else if (auto* gap = dynamic_cast<otio::Gap*>(item.value)) {
                OTIOClip gap_clip;
                gap_clip.is_gap = true;
                gap_clip.name = "Gap";
                gap_clip.start_time = track_position;

                otio::ErrorStatus gap_err;
                auto gap_dur = gap->duration(&gap_err);
                if (!otio::is_error(gap_err)) {
                    gap_clip.duration = gap_dur.to_seconds();
                }

                our_track.clips.push_back(gap_clip);
                track_position += gap_clip.duration;
            }
        }

        nest_clip.nested_tracks.push_back(our_track);
    }

    // Reorder: reverse video tracks for NLE-style display
    std::vector<OTIOTrack> video_tracks, audio_tracks;
    for (auto& t : nest_clip.nested_tracks) {
        if (t.is_video) video_tracks.push_back(std::move(t));
        else audio_tracks.push_back(std::move(t));
    }
    std::reverse(video_tracks.begin(), video_tracks.end());

    nest_clip.nested_tracks.clear();
    for (auto& t : video_tracks) nest_clip.nested_tracks.push_back(std::move(t));
    for (auto& t : audio_tracks) nest_clip.nested_tracks.push_back(std::move(t));

    // Get frame rate from nested clips if available
    for (const auto& track : nest_clip.nested_tracks) {
        for (const auto& clip : track.clips) {
            if (!clip.is_gap && clip.source_fps > 0) {
                nest_clip.nested_fps = clip.source_fps;
                break;
            }
        }
        if (nest_clip.nested_fps > 0) break;
    }

    // Link media in the nested clips using MediaLinker
    if (!source_dir.empty()) {
        qcview::MediaLinker linker;
        qcview::LinkOptions options;
        options.recursive = true;
        options.fuzzy_match = true;
        options.max_depth = 10;

        auto summary = linker.LinkMediaInDirectory(nest_clip.nested_tracks, source_dir, options);
        Debug::Log("ParseNestedStack '" + nest_clip.name + "': Linked " +
                   std::to_string(summary.linked_count) + "/" +
                   std::to_string(summary.total_clips) + " clips");
    }

    nest_clip.nested_loaded = true;

    Debug::Log("ParseNestedStack: '" + nest_clip.name + "' complete - " +
               std::to_string(video_idx) + " video, " + std::to_string(audio_idx) +
               " audio tracks, fps=" + std::to_string(nest_clip.nested_fps));
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
    double max_end = 0.0;
    for (const auto& track : tracks_) {
        for (const auto& clip : track.clips) {
            if (clip.is_gap) continue;  // Don't count gaps
            double clip_end = clip.start_time + clip.duration;
            if (clip_end > max_end) {
                max_end = clip_end;
            }
        }
    }

    // For dual view mode and video-like modes: exact duration, no padding
    // For multi-track timelines: small padding to prevent immediate end
    if (IsDualViewMode() || source_mode_ == TimelineSourceMode::VIDEO_FILE ||
        source_mode_ == TimelineSourceMode::IMAGE_SEQUENCE) {
        timeline_duration_ = max_end > 0.0 ? max_end : 1.0;  // Minimum 1s for empty
    } else {
        // Multi-track timelines get small padding
        timeline_duration_ = max_end > 0.0 ? max_end + 0.5 : 1.0;
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

        // For dual view mode, sync the separate LEFT/RIGHT flatteners BEFORE NotifyTracksEdited
        // CRITICAL: In dual view, cache_ uses left_flattener_ (not flattener_), so we must
        // update left_flattener_ FIRST, then call NotifyTracksEdited.
        // Otherwise, NotifyTracksEdited would use stale flattener data and crash.
        if (IsDualViewMode()) {
            controller->SyncDualFlatteners();
            // SyncDualFlatteners already calls NotifyTracksEdited on both caches
        } else {
            // Non-dual-view mode: notify the playback controller about the edit
            // This clears the stale current texture AND the cache
            controller->NotifyTracksEdited();
        }
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

//=============================================================================
// Edit Safety (Thread-Safe Track Modifications)
//=============================================================================

bool TimelineView::BeginEdit() {
    if (edit_mode_active_) {
        // Already in edit mode - nested edits are OK
        return false;
    }

    // Get the effective playback controller (internal or external)
    auto* controller = GetEffectivePlaybackController();

    // Check if playback is active
    was_playing_before_edit_ = controller && controller->IsPlaying();

    // Pause playback to prevent race conditions with background threads
    // (CacheManagementThread, AudioMixer background thread, etc.)
    if (was_playing_before_edit_) {
        controller->Pause();
        Debug::Log("TimelineView::BeginEdit: Paused playback for safe editing");
    }

    edit_mode_active_ = true;
    return was_playing_before_edit_;
}

void TimelineView::EndEdit(bool resume) {
    if (!edit_mode_active_) {
        return;
    }

    edit_mode_active_ = false;

    // Resume playback if it was playing before the edit
    if (resume && was_playing_before_edit_) {
        auto* controller = GetEffectivePlaybackController();
        if (controller) {
            controller->Play();
            Debug::Log("TimelineView::EndEdit: Resumed playback after edit");
        }
    }

    was_playing_before_edit_ = false;
}

void TimelineView::SetTracks(const std::vector<OTIOTrack>& tracks,
                              double frame_rate,
                              const std::string& timeline_name,
                              const std::string& source_file_path) {
    // Restore tracks from cached edits (when re-entering a previously edited timeline)
    // Shutdown existing playback so it can be properly reinitialized with the restored tracks

    // Reset source mode to VIDEO_FILE (default) for restored timelines
    ResetSourceMode();

    // Shutdown existing playback controller first
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

// ============================================================================
// Nested Timeline Navigation
// ============================================================================

bool TimelineView::EnterNestedClip(const std::string& clip_id) {
    if (!nested_manager_) {
        Debug::Log("EnterNestedClip: No nested timeline manager");
        return false;
    }

    // Find the clip
    OTIOClip* clip = FindClipById(clip_id);
    if (!clip) {
        Debug::Log("EnterNestedClip: Clip not found: " + clip_id);
        return false;
    }

    if (!clip->is_nested) {
        Debug::Log("EnterNestedClip: Clip is not a nested composition: " + clip_id);
        return false;
    }

    // Nested tracks should already be parsed and linked during import (eager loading)
    // This fallback handles legacy data that might have JSON but no parsed tracks
    if (!clip->nested_loaded && !clip->nested_timeline_json.empty()) {
#ifdef USE_OPENTIMELINEIO
        Debug::Log("EnterNestedClip: Fallback parsing for " + clip->name +
                   " (should have been parsed during import)");

        otio::ErrorStatus err;
        auto* obj = otio::SerializableObject::from_json_string(clip->nested_timeline_json, &err);
        if (obj && !otio::is_error(err)) {
            if (auto* nested_stack = dynamic_cast<otio::Stack*>(obj)) {
                std::string source_dir = GetSourceDirectory();
                ParseNestedStack(*clip, nested_stack, source_dir);
            }
        } else {
            Debug::Log("Failed to parse nested JSON: " + std::string(err.details.c_str()));
        }
#endif
    }

    // Verify we have nested tracks to enter
    if (clip->nested_tracks.empty()) {
        Debug::Log("EnterNestedClip: No nested tracks in " + clip->name);
        return false;
    }

    // Save current context and enter nest
    bool success = nested_manager_->EnterNest(
        *clip,
        tracks_,
        timeline_name_,
        frame_rate_,
        current_time_,
        zoom_level_,
        scroll_offset_x_
    );

    if (!success) {
        return false;
    }

    // Replace current tracks with nested tracks
    tracks_ = clip->nested_tracks;
    timeline_name_ = clip->nested_name.empty() ? clip->name : clip->nested_name;
    if (clip->nested_fps > 0) {
        frame_rate_ = clip->nested_fps;
    }

    // Reset view state for nested timeline
    current_time_ = 0.0;
    zoom_level_ = 50.0f;
    scroll_offset_x_ = 0.0f;

    // Recalculate duration and sync flattener
    RecalculateDuration();
    flattener_.SetTracks(tracks_);

    Debug::Log("Entered nested timeline: " + timeline_name_ +
               " (depth: " + std::to_string(GetNestedDepth()) + ")");

    return true;
}

bool TimelineView::ExitNestedTimeline() {
    if (!nested_manager_ || !nested_manager_->IsNested()) {
        Debug::Log("ExitNestedTimeline: Not in a nested timeline");
        return false;
    }

    std::vector<OTIOTrack> parent_tracks;
    std::string parent_name;
    double parent_fps;
    double parent_playhead;
    float parent_zoom;
    float parent_scroll;

    bool success = nested_manager_->ExitNest(
        parent_tracks,
        parent_name,
        parent_fps,
        parent_playhead,
        parent_zoom,
        parent_scroll
    );

    if (!success) {
        return false;
    }

    // Restore parent context
    tracks_ = std::move(parent_tracks);
    timeline_name_ = parent_name;
    frame_rate_ = parent_fps;
    current_time_ = parent_playhead;
    zoom_level_ = parent_zoom;
    scroll_offset_x_ = parent_scroll;

    // Recalculate duration and sync flattener
    RecalculateDuration();
    flattener_.SetTracks(tracks_);

    Debug::Log("Exited to timeline: " + timeline_name_ +
               " (depth: " + std::to_string(GetNestedDepth()) +
               ", tracks: " + std::to_string(tracks_.size()) +
               ", duration: " + std::to_string(timeline_duration_) + "s)");

    return true;
}

bool TimelineView::IsViewingNestedTimeline() const {
    return nested_manager_ && nested_manager_->IsNested();
}

int TimelineView::GetNestedDepth() const {
    return nested_manager_ ? nested_manager_->GetDepth() : 0;
}

std::vector<std::string> TimelineView::GetBreadcrumbPath() const {
    if (!nested_manager_) {
        return {timeline_name_};
    }

    auto path = nested_manager_->GetBreadcrumbPath();

    // If at root, return just the timeline name
    if (path.empty()) {
        return {timeline_name_};
    }

    return path;
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

// ============================================================================
// PLAYLIST MODE EDITING - Trim, Slip, and Reorder with Ripple
// ============================================================================

void TimelineView::UpdatePlaylistEdit() {
    if (source_mode_ != TimelineSourceMode::PLAYLIST) return;
    if (tracks_.empty() || tracks_[0].clips.empty()) return;

    // Handle pending edit state - check if drag threshold exceeded
    if (playlist_pending_edit_) {
        ImVec2 mouse_pos = ImGui::GetMousePos();
        float delta = std::abs(mouse_pos.x - playlist_edit_start_mouse_x_);

        // Cancel pending on Escape
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            playlist_pending_edit_ = false;
            playlist_pending_op_ = PlaylistEditOp::NONE;
            return;
        }

        if (!ImGui::IsMouseDown(0)) {
            // Mouse released before threshold - just a click, no edit
            playlist_pending_edit_ = false;
            playlist_pending_op_ = PlaylistEditOp::NONE;
            return;
        }

        if (delta >= kDragThreshold) {
            // Threshold exceeded - start the actual edit
            playlist_edit_op_ = playlist_pending_op_;
            playlist_pending_edit_ = false;
            playlist_pending_op_ = PlaylistEditOp::NONE;
            BeginEdit();  // Pause playback for safe editing
            Debug::Log("[Playlist Edit] Started " +
                       std::string(playlist_edit_op_ == PlaylistEditOp::TRIM_START ? "TRIM_START" :
                                  playlist_edit_op_ == PlaylistEditOp::TRIM_END ? "TRIM_END" :
                                  playlist_edit_op_ == PlaylistEditOp::SLIP ? "SLIP" : "MOVE") +
                       " after drag threshold (delta=" + std::to_string(delta) + "px)");
        }
        return;  // Still pending or just started - don't process edit yet
    }

    // Called each frame during active edit
    if (playlist_edit_op_ == PlaylistEditOp::NONE) return;

    // Check for valid clip index
    if (playlist_edit_clip_index_ < 0 ||
        playlist_edit_clip_index_ >= static_cast<int>(tracks_[0].clips.size())) {
        CancelPlaylistEdit();
        return;
    }

    OTIOClip& clip = tracks_[0].clips[playlist_edit_clip_index_];
    ImVec2 mouse_pos = ImGui::GetMousePos();
    float delta_x = mouse_pos.x - playlist_edit_start_mouse_x_;
    double delta_time = delta_x / zoom_level_;  // Convert pixels to seconds

    // Handle escape to cancel
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        CancelPlaylistEdit();
        return;
    }

    // Handle mouse release to commit
    if (!ImGui::IsMouseDown(0)) {
        CommitPlaylistEdit();
        return;
    }

    switch (playlist_edit_op_) {
        case PlaylistEditOp::TRIM_START: {
            // Adjust source_in, keeping source_out fixed
            // This changes the clip duration and timeline position of downstream clips
            double new_source_in = playlist_edit_original_in_ + delta_time;

            // Clamp to valid range (can't go past source_out or below 0)
            new_source_in = std::max(0.0, std::min(new_source_in, clip.source_out - 0.04)); // Min 1 frame at 24fps

            clip.source_in = new_source_in;
            clip.duration = clip.source_out - clip.source_in;

            // Ripple: recalculate all positions
            RippleRecalculatePositions(0);
            break;
        }

        case PlaylistEditOp::TRIM_END: {
            // Adjust source_out, keeping source_in fixed
            double new_source_out = playlist_edit_original_out_ + delta_time;

            // Clamp to valid range (can't go below source_in or past source_duration)
            new_source_out = std::max(clip.source_in + 0.04, std::min(new_source_out, clip.source_duration));

            clip.source_out = new_source_out;
            clip.duration = clip.source_out - clip.source_in;

            // Ripple: recalculate all positions
            RippleRecalculatePositions(0);
            break;
        }

        case PlaylistEditOp::SLIP: {
            // Shift source window without changing timeline duration
            // Both source_in and source_out move together
            double new_source_in = playlist_edit_original_in_ + delta_time;
            double new_source_out = playlist_edit_original_out_ + delta_time;

            // Clamp to source boundaries
            if (new_source_in < 0.0) {
                double shift = -new_source_in;
                new_source_in = 0.0;
                new_source_out = playlist_edit_original_out_ - playlist_edit_original_in_;  // Keep same duration
            }
            if (new_source_out > clip.source_duration) {
                double duration = playlist_edit_original_out_ - playlist_edit_original_in_;
                new_source_out = clip.source_duration;
                new_source_in = clip.source_duration - duration;
            }

            clip.source_in = new_source_in;
            clip.source_out = new_source_out;
            // Duration stays the same - no ripple needed for slip
            break;
        }

        case PlaylistEditOp::MOVE: {
            // Reorder clip to new position based on mouse X
            // Find which position the mouse is over
            double mouse_time = (mouse_pos.x - ruler_screen_pos_.x + scroll_offset_x_) / zoom_level_;

            // Find target index based on mouse position
            int target_index = -1;
            for (size_t i = 0; i < tracks_[0].clips.size(); i++) {
                const auto& c = tracks_[0].clips[i];
                if (c.is_gap) continue;

                double clip_center = c.start_time + c.duration / 2.0;
                if (mouse_time < clip_center) {
                    target_index = static_cast<int>(i);
                    break;
                }
            }

            // If past all clips, insert at end
            if (target_index < 0) {
                target_index = static_cast<int>(tracks_[0].clips.size()) - 1;
            }

            // Only move if target is different from current
            if (target_index != playlist_edit_clip_index_ && target_index >= 0) {
                // Extract the clip
                OTIOClip moving_clip = clip;

                // Remove from current position (and adjacent gap)
                tracks_[0].clips.erase(tracks_[0].clips.begin() + playlist_edit_clip_index_);

                // Check for and remove adjacent gap
                if (playlist_edit_clip_index_ < static_cast<int>(tracks_[0].clips.size()) &&
                    tracks_[0].clips[playlist_edit_clip_index_].is_gap) {
                    tracks_[0].clips.erase(tracks_[0].clips.begin() + playlist_edit_clip_index_);
                } else if (playlist_edit_clip_index_ > 0 &&
                           tracks_[0].clips[playlist_edit_clip_index_ - 1].is_gap) {
                    tracks_[0].clips.erase(tracks_[0].clips.begin() + playlist_edit_clip_index_ - 1);
                    target_index--;  // Adjust target since we removed before it
                }

                // Adjust target index after removal
                if (target_index > playlist_edit_clip_index_) {
                    target_index -= 2;  // Account for clip and gap removal
                    if (target_index < 0) target_index = 0;
                }

                // Insert at new position with gap
                const int gap_frames = 10;
                double gap_duration = gap_frames / frame_rate_;

                // Insert gap before if not at start
                if (target_index > 0 && !tracks_[0].clips.empty()) {
                    OTIOClip gap;
                    gap.id = "gap_" + std::to_string(target_index);
                    gap.name = "Gap";
                    gap.is_gap = true;
                    gap.duration = gap_duration;
                    tracks_[0].clips.insert(tracks_[0].clips.begin() + target_index, gap);
                    target_index++;
                }

                // Insert the clip
                tracks_[0].clips.insert(tracks_[0].clips.begin() + target_index, moving_clip);

                // Update our tracking index
                playlist_edit_clip_index_ = target_index;

                // Ripple recalculate
                RippleRecalculatePositions(0);
            }
            break;
        }

        default:
            break;
    }
}

void TimelineView::CommitPlaylistEdit() {
    if (playlist_edit_op_ == PlaylistEditOp::NONE) return;

    Debug::Log("[Playlist Edit] Commit " +
               std::string(playlist_edit_op_ == PlaylistEditOp::TRIM_START ? "TRIM_START" :
                          playlist_edit_op_ == PlaylistEditOp::TRIM_END ? "TRIM_END" :
                          playlist_edit_op_ == PlaylistEditOp::SLIP ? "SLIP" : "MOVE"));

    // Final ripple calculation
    RippleRecalculatePositions(0);

    // Sync to flattener and cache
    SyncFlattenerAndInvalidate();

    // Save edits back to source MediaItem
    SyncPlaylistToMediaItem();

    // Clear edit state
    playlist_edit_op_ = PlaylistEditOp::NONE;
    playlist_edit_clip_id_.clear();
    playlist_edit_clip_index_ = -1;

    // End edit mode (resumes playback if it was playing)
    EndEdit(true);
}

void TimelineView::CancelPlaylistEdit() {
    if (playlist_edit_op_ == PlaylistEditOp::NONE) return;

    Debug::Log("[Playlist Edit] Cancel - reverting changes");

    // Revert the edited clip to original values
    if (playlist_edit_clip_index_ >= 0 &&
        playlist_edit_clip_index_ < static_cast<int>(tracks_[0].clips.size())) {
        OTIOClip& clip = tracks_[0].clips[playlist_edit_clip_index_];
        clip.source_in = playlist_edit_original_in_;
        clip.source_out = playlist_edit_original_out_;
        clip.duration = clip.source_out - clip.source_in;

        // Ripple recalculate to restore positions
        RippleRecalculatePositions(0);
    }

    // Clear edit state
    playlist_edit_op_ = PlaylistEditOp::NONE;
    playlist_edit_clip_id_.clear();
    playlist_edit_clip_index_ = -1;

    // Also clear pending state
    playlist_pending_edit_ = false;
    playlist_pending_op_ = PlaylistEditOp::NONE;

    // End edit mode without resuming (user explicitly cancelled)
    EndEdit(false);
}

void TimelineView::RippleRecalculatePositions(int start_index) {
    // Recalculate timeline start positions for all clips
    // This enforces ripple behavior: clips are sequential with gaps
    if (tracks_.empty() || tracks_[0].clips.empty()) return;

    const int gap_frames = 10;
    double gap_duration = gap_frames / frame_rate_;

    double current_time = 0.0;

    for (size_t i = 0; i < tracks_[0].clips.size(); i++) {
        auto& clip = tracks_[0].clips[i];

        // Set position
        clip.start_time = current_time;

        // Advance by clip duration
        current_time += clip.duration;

        // If this is NOT a gap and next clip is NOT a gap, we need gap time
        // (Gaps are explicit clip objects, so we don't double-add)
    }

    // Update timeline duration
    RecalculateDuration();
}

void TimelineView::SyncPlaylistToMediaItem() {
    // Save the current timeline state back to the source MediaItem
    if (!source_media_item_ || source_mode_ != TimelineSourceMode::PLAYLIST) return;
    if (tracks_.empty()) return;

    Debug::Log("[Playlist Edit] Syncing to MediaItem: " + source_media_item_->name);

    // Update existing playlist items in-place (preserve media_ids, update in/out points)
    // This is safer than rebuilding - we don't lose the media_id associations
    int playlist_index = 0;
    for (const auto& clip : tracks_[0].clips) {
        if (clip.is_gap) continue;  // Skip gaps

        if (playlist_index < static_cast<int>(source_media_item_->playlist_items.size())) {
            // Update existing entry's in/out points
            source_media_item_->playlist_items[playlist_index].in_point = clip.source_in;
            source_media_item_->playlist_items[playlist_index].out_point = clip.source_out;
        }
        playlist_index++;
    }

    Debug::Log("[Playlist Edit] Synced " + std::to_string(playlist_index) + " items");
}

bool TimelineView::DeleteClipFromPlaylist(const std::string& clip_id) {
    if (source_mode_ != TimelineSourceMode::PLAYLIST || !source_media_item_) {
        Debug::Log("DeleteClipFromPlaylist: Not in playlist mode or no source item");
        return false;
    }

    if (tracks_.empty()) {
        Debug::Log("DeleteClipFromPlaylist: No tracks");
        return false;
    }

    // Find and remove the clip from the timeline tracks
    auto& video_track = tracks_[0];
    int clip_index = -1;
    for (size_t i = 0; i < video_track.clips.size(); i++) {
        if (video_track.clips[i].id == clip_id && !video_track.clips[i].is_gap) {
            clip_index = static_cast<int>(i);
            break;
        }
    }

    if (clip_index < 0) {
        Debug::Log("DeleteClipFromPlaylist: Clip not found: " + clip_id);
        return false;
    }

    // Remove from timeline tracks
    std::string clip_name = video_track.clips[clip_index].name;
    video_track.clips.erase(video_track.clips.begin() + clip_index);

    // Also remove the adjacent gap (if any)
    // After erasing the clip, the gap that was after it is now at clip_index
    // Check if there's still elements and if the current element at clip_index is a gap
    if (clip_index < static_cast<int>(video_track.clips.size()) &&
        video_track.clips[clip_index].is_gap) {
        video_track.clips.erase(video_track.clips.begin() + clip_index);
        Debug::Log("DeleteClipFromPlaylist: Also removed adjacent gap");
    }
    // If we deleted the last clip, check if there's now a trailing gap to remove
    else if (clip_index > 0 && clip_index == static_cast<int>(video_track.clips.size()) &&
             video_track.clips[clip_index - 1].is_gap) {
        video_track.clips.erase(video_track.clips.begin() + clip_index - 1);
        Debug::Log("DeleteClipFromPlaylist: Removed trailing gap");
    }

    // Remove from source MediaItem's playlist_items
    // Match by index (clips and playlist_items should be in same order)
    // But we need to account for gaps - count non-gap clips up to clip_index
    int playlist_index = 0;
    for (int i = 0; i < clip_index; i++) {
        if (!tracks_[0].clips[i].is_gap) {
            playlist_index++;
        }
    }

    if (playlist_index < static_cast<int>(source_media_item_->playlist_items.size())) {
        source_media_item_->playlist_items.erase(
            source_media_item_->playlist_items.begin() + playlist_index);
        Debug::Log("DeleteClipFromPlaylist: Removed '" + clip_name + "' from playlist (index " +
                   std::to_string(playlist_index) + ")");
    }

    // Recalculate positions (ripple downstream clips)
    RippleRecalculatePositions(clip_index);

    // Recalculate duration
    RecalculateDuration();

    // Sync flattener and invalidate cache
    SyncFlattenerAndInvalidate();

    return true;
}

} // namespace qcview
