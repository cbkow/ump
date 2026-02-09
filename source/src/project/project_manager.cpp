#include "project_manager.h"
#include "../player/video_player.h"
#include "../player/exr_transcoder.h"
#include "../player/image_loaders.h"
#include "../utils/exr_layer_detector.h"
#include "../metadata/ffmpeg_metadata_extractor.h"
#include "../timeline/timeline_view.h"
#include "../annotations/annotation_io.h"
#include <imgui.h>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <set>
#include <regex>
#include <random>
#include "../utils/debug_utils.h"
#include <nfd.h>
#include <nlohmann/json.hpp>
#include <fstream>

#ifdef _WIN32
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

// External function from main.cpp to schedule import with overlay
extern void ScheduleImport(const std::string& path, const std::string& message);

#define ICON_MOVIE                  u8"\uE02C"
#define ICON_AUDIO_TRACK           u8"\uE3A1"
#define ICON_IMAGE                 u8"\uE1A6"
#define ICON_VIDEO_LIBRARY         u8"\uE02C"
#define ICON_PLAY_CIRCLE          u8"\uE1C4"
#define ICON_MOVIE_CREATION       u8"\uE404"
#define ICON_PLAYLIST_ADD         u8"\uE03B"
#define ICON_FOLDER_OPEN          u8"\uE2C8"
#define ICON_CONTENT_COPY         u8"\xE14D"
#define ICON_ARTICLE              u8"\uEF42"
#define ICON_CLOSE                 u8"\uE5CD"
#define ICON_TOPIC                 u8"\uEB13"

extern ImFont* font_icons;

// EXR settings globals from main.cpp
extern int g_exr_transcode_threads;

// Disk cache settings globals from main.cpp
extern std::string g_custom_cache_path;
extern int g_cache_retention_days;
extern int g_transcode_cache_max_gb;
extern bool g_clear_cache_on_exit;

// Shared EXRTranscoder instance for all transcode operations
// This ensures consistent cache paths and proper cancellation support
static ump::EXRTranscoder& GetSharedTranscoder() {
    static ump::EXRTranscoder s_transcoder;
    // Always apply current cache config (in case settings changed)
    s_transcoder.SetCacheConfig(g_custom_cache_path, g_cache_retention_days,
                                g_transcode_cache_max_gb, g_clear_cache_on_exit);
    return s_transcoder;
}

extern ImFont* font_regular;
extern ImFont* font_mono;

// External variables from main.cpp
extern bool use_windows_accent_color;
extern int custom_accent_color_index;
extern ImVec4 custom_picker_color;
extern const ImVec4 accent_color_palette[];
extern const int accent_color_palette_count;

ImVec4 GetDefaultAccentColor() {
    return accent_color_palette[0];  // 69797e - Default gray-blue
}

ImVec4 GetCustomAccentColor() {
    if (custom_accent_color_index == -2) {
        return custom_picker_color;
    }
    if (custom_accent_color_index >= 0 && custom_accent_color_index < accent_color_palette_count) {
        return accent_color_palette[custom_accent_color_index];
    }
    return GetDefaultAccentColor();
}

#ifdef _WIN32
ImVec4 GetWindowsAccentColor() {
    if (use_windows_accent_color) {
        DWORD colorization_color;
        BOOL opaque_blend;
        if (SUCCEEDED(DwmGetColorizationColor(&colorization_color, &opaque_blend))) {
            float r = ((colorization_color >> 16) & 0xff) / 255.0f;
            float g = ((colorization_color >> 8) & 0xff) / 255.0f;
            float b = (colorization_color & 0xff) / 255.0f;
            return ImVec4(r, g, b, 1.0f);
        }
        return ImVec4(0.26f, 0.59f, 0.98f, 1.0f); // Fallback blue if API fails
    }
    return GetCustomAccentColor();
}
#else
ImVec4 GetWindowsAccentColor() {
    if (use_windows_accent_color) {
        return GetDefaultAccentColor();
    }
    return GetCustomAccentColor();
}
#endif

ImVec4 TintColor(const ImVec4& color, float brightness, float saturation = 1.0f) {
    ImVec4 result = color;
    result.x *= brightness;
    result.y *= brightness;
    result.z *= brightness;

    if (saturation < 1.0f) {
        float gray = result.x * 0.299f + result.y * 0.587f + result.z * 0.114f;
        result.x = gray + (result.x - gray) * saturation;
        result.y = gray + (result.y - gray) * saturation;
        result.z = gray + (result.z - gray) * saturation;
    }

    return result;
}

ImVec4 MutedLight(const ImVec4& accent) { return TintColor(accent, 1.5f, 0.8f); }
ImVec4 Bright(const ImVec4& accent) { return TintColor(accent, 2.2f, 0.5f); }

// External transcode performance settings (defined in main.cpp)
extern struct {
    int encoder_thread_count;
    bool prefer_hardware_encoding;
    std::string hardware_encoder;
    int default_worker_count;
    int max_worker_count;
    int prefetch_buffer_size;
    int prefetch_ahead_count;
} transcode_settings;

namespace ump {

    // ============================================================================
    // CODEC DETECTION UTILITY
    // ============================================================================

    bool ProjectManager::IsInterFrameCodec(const std::string& codec) {
        // Check if codec is H.264/H.265 (inter-frame codec with poor random access)
        // Convert to lowercase for case-insensitive comparison
        std::string codec_lower = codec;
        std::transform(codec_lower.begin(), codec_lower.end(), codec_lower.begin(), ::tolower);

        return (codec_lower.find("h264") != std::string::npos ||
                codec_lower.find("h.264") != std::string::npos ||
                codec_lower.find("h265") != std::string::npos ||
                codec_lower.find("h.265") != std::string::npos ||
                codec_lower.find("hevc") != std::string::npos ||
                codec_lower.find("avc") != std::string::npos);
    }

    // ============================================================================
    // CONSTRUCTION / DESTRUCTION
    // ============================================================================

    ProjectManager::ProjectManager(VideoPlayer* player, std::string* current_file, bool* inspector_panel_flag, bool cache_preference)
        : video_player(player), current_file_path(current_file), show_inspector_panel(inspector_panel_flag),
          user_cache_preference(cache_preference), cache_enabled(cache_preference) {

        // Initialize video cache manager
        video_cache_manager = std::make_unique<VideoCache>();

        // Initialize transcode queue
        transcode_queue_ = std::make_unique<TranscodeQueue>();
        transcode_worker_pool_ = std::make_unique<TranscodeWorkerPool>(transcode_queue_.get(), transcode_settings.default_worker_count);
        transcode_queue_window_ = std::make_unique<TranscodeQueueWindow>(
            transcode_queue_.get(),
            transcode_worker_pool_.get()
        );

        // Set auto-save path (using %LOCALAPPDATA%\ump\ for consistency with other settings)
        #ifdef _WIN32
        const char* localappdata = std::getenv("LOCALAPPDATA");
        std::string queue_save_path;
        if (localappdata) {
            std::string base_path = std::string(localappdata) + "\\ump";
            std::filesystem::create_directories(base_path);
            queue_save_path = base_path + "\\transcode_queue.json";
        } else {
            queue_save_path = "transcode_queue.json";  // Fallback
        }
        #else
        const char* home = std::getenv("HOME");
        std::string queue_save_path;
        if (home) {
            std::string base_path = std::string(home) + "/.config/ump";
            std::filesystem::create_directories(base_path);
            queue_save_path = base_path + "/transcode_queue.json";
        } else {
            queue_save_path = "transcode_queue.json";  // Fallback
        }
        #endif

        transcode_queue_->SetAutoSavePath(queue_save_path);

        // Clear old queue file on startup (async, fire-and-forget)
        std::thread([queue_save_path]() {
            try {
                if (std::filesystem::exists(queue_save_path)) {
                    std::filesystem::remove(queue_save_path);
                    Debug::Log("ProjectManager: Cleared old transcode queue file");
                }
            } catch (...) {
                // Ignore errors - not critical
            }
        }).detach();

        // Start worker pool
        transcode_worker_pool_->Start();

        Debug::Log("ProjectManager: Transcode queue initialized");

        CreateNewBin("Videos");
        CreateNewBin("Audio");
        CreateNewBin("Images");
        CreateNewBin("Dual Views");
        CreateNewBin("Playlists");

        if (video_player) {
            // Set metadata callback to provide cached FFmpeg metadata
            video_player->SetMetadataCallback([this](const std::string& path) -> VideoMetadata {
                const CombinedMetadata* cached = GetCachedMetadata(path);
                if (cached && cached->video_meta && cached->video_meta->is_loaded) {
                    //Debug::Log("MetadataCallback: Returning cached FFmpeg metadata for: " + path);
                    return *cached->video_meta;
                }
                //Debug::Log("MetadataCallback: No cached metadata found for: " + path);
                return VideoMetadata();  // Return empty if not found
            });
        }

        StartAdobeWorkerThread();
        StartVideoMetadataWorkerThread();
    }

    ProjectManager::~ProjectManager() {
        // Stop transcode workers
        if (transcode_worker_pool_) {
            transcode_worker_pool_->Stop();
        }

        // Save queue (using %LOCALAPPDATA%\ump\ for consistency with other settings)
        if (transcode_queue_) {
            #ifdef _WIN32
            const char* localappdata = std::getenv("LOCALAPPDATA");
            std::string queue_save_path;
            if (localappdata) {
                std::string base_path = std::string(localappdata) + "\\ump";
                std::filesystem::create_directories(base_path);
                queue_save_path = base_path + "\\transcode_queue.json";
            } else {
                queue_save_path = "transcode_queue.json";  // Fallback
            }
            #else
            const char* home = std::getenv("HOME");
            std::string queue_save_path;
            if (home) {
                std::string base_path = std::string(home) + "/.config/ump";
                std::filesystem::create_directories(base_path);
                queue_save_path = base_path + "/transcode_queue.json";
            } else {
                queue_save_path = "transcode_queue.json";  // Fallback
            }
            #endif
            transcode_queue_->SaveQueue(queue_save_path);
        }

        StopVideoMetadataWorkerThread();
        StopAdobeWorkerThread();
    }

    // ============================================================================
    // PROJECT MANAGEMENT
    // ============================================================================

    void ProjectManager::CreateNewProject(const std::string& name, const std::string& path) {
        bins.clear();
        media_pool.clear();

        current_project_path = path + "/" + name + ".uproj";

        CreateNewBin("Videos");
        CreateNewBin("Audio");
        CreateNewBin("Images");
        CreateNewBin("Dual Views");
        CreateNewBin("Playlists");
    }

    void ProjectManager::SaveProject() {
        using json = nlohmann::json;

        Debug::Log("SaveProject: Called - current_project_path = " + (current_project_path.empty() ? "(empty)" : current_project_path));
        Debug::Log("SaveProject: media_pool.size() = " + std::to_string(media_pool.size()));

        // CRITICAL: Flush current timeline edits to MediaItem before saving
        // Without this, edits made to the active timeline won't be captured
        if (flush_timeline_edits_callback) {
            flush_timeline_edits_callback();
        }

        // Show save dialog if no project path exists
        std::string save_path = current_project_path;
        if (save_path.empty()) {
            nfdu8char_t* out_path = nullptr;
            nfdfilteritem_t filter[1] = { { "Union Player Project", "umproj" } };
            nfdresult_t result = NFD_SaveDialogU8(&out_path, filter, 1, nullptr, "project.umproj");

            if (result != NFD_OKAY) {
                if (result == NFD_ERROR) {
                    Debug::Log("SaveProject: File dialog error");
                }
                return;
            }

            save_path = out_path;
            NFD_FreePathU8(out_path);

            // Ensure .umproj extension
            if (save_path.find(".umproj") == std::string::npos) {
                save_path += ".umproj";
            }
        }

        Debug::Log("SaveProject: Saving to path: " + save_path);

        try {
            json project_data;

            // Project metadata
            project_data["version"] = "1.0";
            project_data["project_name"] = GetProjectName(save_path);

            // Project-level settings
            project_data["pipeline_mode"] = PipelineModeToString(project_pipeline_mode_);

            // Serialize bins
            json bins_array = json::array();
            for (const auto& bin : bins) {
                json bin_obj;
                bin_obj["name"] = bin.name;
                bin_obj["is_open"] = bin.is_open;

                json bin_items = json::array();
                for (const auto& item : bin.items) {
                    bin_items.push_back(item.id);  // Store only IDs, full items in media_pool
                }
                bin_obj["items"] = bin_items;
                bins_array.push_back(bin_obj);
            }
            project_data["bins"] = bins_array;

            // Serialize media_pool
            json media_pool_array = json::array();
            for (const auto& item : media_pool) {
                json item_obj;
                item_obj["id"] = item.id;
                item_obj["name"] = item.name;
                item_obj["path"] = item.path;
                item_obj["type"] = static_cast<int>(item.type);
                item_obj["duration"] = item.duration;
                item_obj["sequence_id"] = item.sequence_id;
                item_obj["clip_count"] = item.clip_count;
                item_obj["is_active"] = item.is_active;

                // Image sequence fields
                item_obj["sequence_pattern"] = item.sequence_pattern;
                item_obj["ffmpeg_pattern"] = item.ffmpeg_pattern;
                item_obj["frame_count"] = item.frame_count;
                item_obj["start_frame"] = item.start_frame;
                item_obj["end_frame"] = item.end_frame;
                item_obj["frame_rate"] = item.frame_rate;
                item_obj["pipeline_mode"] = PipelineModeToString(item.pipeline_mode);

                // NEW: Save cached sequence dimensions (for instant loading)
                if ((item.type == MediaType::IMAGE_SEQUENCE || item.type == MediaType::EXR_SEQUENCE) &&
                    item.sequence_width > 0 && item.sequence_height > 0) {
                    item_obj["sequence_width"] = item.sequence_width;
                    item_obj["sequence_height"] = item.sequence_height;
                }

                // EXR fields
                item_obj["exr_layer"] = item.exr_layer;
                item_obj["exr_layer_display"] = item.exr_layer_display;

                // NEW: Save ImageSequenceData for reliable reload
                if ((item.type == MediaType::IMAGE_SEQUENCE || item.type == MediaType::EXR_SEQUENCE) &&
                    item.image_seq.IsValid()) {
                    json image_seq_obj;
                    image_seq_obj["pattern"] = item.image_seq.pattern;
                    image_seq_obj["ffmpeg_pattern"] = item.image_seq.ffmpeg_pattern;
                    image_seq_obj["directory"] = item.image_seq.directory;
                    image_seq_obj["frame_count"] = item.image_seq.frame_count;
                    image_seq_obj["start_frame"] = item.image_seq.start_frame;
                    image_seq_obj["end_frame"] = item.image_seq.end_frame;
                    image_seq_obj["frame_rate"] = item.image_seq.frame_rate;
                    image_seq_obj["duration"] = item.image_seq.duration;
                    image_seq_obj["width"] = item.image_seq.width;
                    image_seq_obj["height"] = item.image_seq.height;
                    image_seq_obj["pipeline_mode"] = PipelineModeToString(item.image_seq.pipeline_mode);
                    image_seq_obj["format"] = item.image_seq.format;
                    image_seq_obj["layer"] = item.image_seq.layer;
                    image_seq_obj["layer_display"] = item.image_seq.layer_display;
                    image_seq_obj["audio_file"] = item.image_seq.audio_file;
                    item_obj["image_seq"] = image_seq_obj;
                    Debug::Log("SaveProject: Saved ImageSequenceData for " + item.name +
                               " (duration=" + std::to_string(item.image_seq.duration) + "s)");
                }

                // In/Out points (per-video range markers)
                item_obj["in_point"] = item.in_point;
                item_obj["out_point"] = item.out_point;

                // View state for persistent zoom/pan/playhead per-media
                item_obj["view_zoom"] = item.view_state.zoom_level;
                item_obj["view_scroll"] = item.view_state.scroll_offset;
                item_obj["view_playhead"] = item.view_state.playhead_position;

                // Video-specific fields: dimensions and frame rate (for VIDEO and AUDIO types)
                if (item.type == MediaType::VIDEO || item.type == MediaType::AUDIO) {
                    item_obj["timeline_width"] = item.timeline_width;
                    item_obj["timeline_height"] = item.timeline_height;
                    item_obj["frame_rate"] = item.frame_rate;
                }

                // Dual view-specific fields (for MediaType::DUAL_VIEW)
                if (item.type == MediaType::DUAL_VIEW) {
                    item_obj["timeline_id"] = item.timeline_id;
                    item_obj["timeline_format"] = item.timeline_format;
                    item_obj["video_track_count"] = item.video_track_count;
                    item_obj["audio_track_count"] = item.audio_track_count;
                    item_obj["timeline_width"] = item.timeline_width;
                    item_obj["timeline_height"] = item.timeline_height;

                    // OTIO timeline view state (zoom/scroll/playhead/in-out points)
                    item_obj["timeline_view_zoom"] = item.view_state.timeline_zoom;
                    item_obj["timeline_view_scroll"] = item.view_state.timeline_scroll;
                    item_obj["timeline_view_playhead"] = item.view_state.timeline_playhead;
                    item_obj["timeline_view_in"] = item.view_state.timeline_in_point;
                    item_obj["timeline_view_out"] = item.view_state.timeline_out_point;

                    // Save cached edited tracks (full timeline edits)
                    if (item.has_cached_edits && !item.cached_tracks.empty()) {
                        json edited_tracks_array = json::array();
                        for (const auto& track : item.cached_tracks) {
                            json track_obj;
                            track_obj["id"] = track.id;
                            track_obj["name"] = track.name;
                            track_obj["is_video"] = track.is_video;
                            track_obj["visible"] = track.visible;
                            track_obj["muted"] = track.muted;
                            track_obj["audio_muted"] = track.audio_muted;
                            track_obj["z_index"] = track.z_index;

                            json clips_array = json::array();
                            for (const auto& clip : track.clips) {
                                json clip_obj;
                                clip_obj["id"] = clip.id;
                                clip_obj["name"] = clip.name;
                                clip_obj["file_path"] = clip.file_path;
                                clip_obj["start_time"] = clip.start_time;
                                clip_obj["duration"] = clip.duration;
                                clip_obj["source_in"] = clip.source_in;
                                clip_obj["source_out"] = clip.source_out;
                                clip_obj["is_gap"] = clip.is_gap;
                                clip_obj["linked_path"] = clip.linked_path;
                                clip_obj["is_linked"] = clip.is_linked;
                                clip_obj["source_fps"] = clip.source_fps;
                                clip_obj["source_width"] = clip.source_width;
                                clip_obj["source_height"] = clip.source_height;
                                clip_obj["source_duration"] = clip.source_duration;
                                clip_obj["has_audio"] = clip.has_audio;
                                clip_obj["has_fade_in"] = clip.has_fade_in;
                                clip_obj["has_fade_out"] = clip.has_fade_out;
                                clip_obj["fade_in_duration"] = clip.fade_in_duration;
                                clip_obj["fade_out_duration"] = clip.fade_out_duration;
                                clip_obj["audio_muted"] = clip.audio_muted;
                                // Image sequence fields
                                if (clip.is_sequence) {
                                    clip_obj["is_sequence"] = clip.is_sequence;
                                    clip_obj["sequence_directory"] = clip.sequence_directory;
                                    clip_obj["sequence_pattern"] = clip.sequence_pattern;
                                    clip_obj["sequence_start_frame"] = clip.sequence_start_frame;
                                    clip_obj["sequence_end_frame"] = clip.sequence_end_frame;
                                    if (!clip.sequence_exr_layer.empty()) {
                                        clip_obj["sequence_exr_layer"] = clip.sequence_exr_layer;
                                    }
                                }
                                // AAF-specific fields
                                if (!clip.aaf_mob_id.empty()) {
                                    clip_obj["aaf_mob_id"] = clip.aaf_mob_id;
                                }
                                // Nested timeline support
                                if (clip.is_nested) {
                                    clip_obj["is_nested"] = clip.is_nested;
                                    clip_obj["nested_fps"] = clip.nested_fps;
                                    clip_obj["nested_name"] = clip.nested_name;
                                    clip_obj["nested_loaded"] = clip.nested_loaded;
                                    if (!clip.nested_timeline_json.empty()) {
                                        clip_obj["nested_timeline_json"] = clip.nested_timeline_json;
                                    }
                                    // Recursively save nested tracks if loaded
                                    if (clip.nested_loaded && !clip.nested_tracks.empty()) {
                                        json nested_tracks_array = json::array();
                                        for (const auto& nested_track : clip.nested_tracks) {
                                            json nested_track_obj;
                                            nested_track_obj["id"] = nested_track.id;
                                            nested_track_obj["name"] = nested_track.name;
                                            nested_track_obj["is_video"] = nested_track.is_video;
                                            nested_track_obj["visible"] = nested_track.visible;
                                            nested_track_obj["muted"] = nested_track.muted;
                                            nested_track_obj["audio_muted"] = nested_track.audio_muted;
                                            nested_track_obj["z_index"] = nested_track.z_index;
                                            // Note: Nested clips serialization is simplified - nested_timeline_json provides recovery
                                            json nested_clips_array = json::array();
                                            for (const auto& nested_clip : nested_track.clips) {
                                                json nested_clip_obj;
                                                nested_clip_obj["id"] = nested_clip.id;
                                                nested_clip_obj["name"] = nested_clip.name;
                                                nested_clip_obj["file_path"] = nested_clip.file_path;
                                                nested_clip_obj["start_time"] = nested_clip.start_time;
                                                nested_clip_obj["duration"] = nested_clip.duration;
                                                nested_clip_obj["source_in"] = nested_clip.source_in;
                                                nested_clip_obj["source_out"] = nested_clip.source_out;
                                                nested_clip_obj["is_gap"] = nested_clip.is_gap;
                                                nested_clip_obj["linked_path"] = nested_clip.linked_path;
                                                nested_clip_obj["is_linked"] = nested_clip.is_linked;
                                                if (!nested_clip.aaf_mob_id.empty()) {
                                                    nested_clip_obj["aaf_mob_id"] = nested_clip.aaf_mob_id;
                                                }
                                                nested_clips_array.push_back(nested_clip_obj);
                                            }
                                            nested_track_obj["clips"] = nested_clips_array;
                                            nested_tracks_array.push_back(nested_track_obj);
                                        }
                                        clip_obj["nested_tracks"] = nested_tracks_array;
                                    }
                                }
                                clips_array.push_back(clip_obj);
                            }
                            track_obj["clips"] = clips_array;
                            edited_tracks_array.push_back(track_obj);
                        }
                        item_obj["edited_tracks"] = edited_tracks_array;
                        item_obj["has_cached_edits"] = true;
                        Debug::Log("SaveProject: Saved " + std::to_string(item.cached_tracks.size()) +
                                   " edited tracks for timeline '" + item.name + "'");
                    }
                }

                // Playlist-specific fields (for MediaType::PLAYLIST)
                if (item.type == MediaType::PLAYLIST) {
                    item_obj["playlist_loop"] = item.playlist_loop;
                    // current_playlist_index is runtime state, NOT saved (starts at 0 on load)

                    Debug::Log("SaveProject: Playlist '" + item.name + "' ptr=" +
                               std::to_string(reinterpret_cast<uintptr_t>(&item)) +
                               " has " + std::to_string(item.playlist_items.size()) + " items to save");

                    json playlist_items_array = json::array();
                    for (const auto& entry : item.playlist_items) {
                        json entry_obj;
                        entry_obj["media_id"] = entry.media_id;
                        entry_obj["in_point"] = entry.in_point;
                        entry_obj["out_point"] = entry.out_point;
                        playlist_items_array.push_back(entry_obj);
                        Debug::Log("  - Saving playlist item: media_id=" + entry.media_id);
                    }
                    item_obj["playlist_items"] = playlist_items_array;

                    Debug::Log("SaveProject: Saved playlist '" + item.name + "' with " +
                               std::to_string(item.playlist_items.size()) + " items");
                }

                // Save cached audio tracks for IMAGE_SEQUENCE and EXR_SEQUENCE items
                // This preserves audio clips dropped onto the audio track
                if ((item.type == MediaType::IMAGE_SEQUENCE || item.type == MediaType::EXR_SEQUENCE) &&
                    item.has_cached_edits && !item.cached_tracks.empty()) {
                    json edited_tracks_array = json::array();
                    for (const auto& track : item.cached_tracks) {
                        // Only save audio tracks (video track is reconstructed from sequence metadata)
                        if (track.is_video) continue;

                        json track_obj;
                        track_obj["id"] = track.id;
                        track_obj["name"] = track.name;
                        track_obj["is_video"] = track.is_video;
                        track_obj["visible"] = track.visible;
                        track_obj["muted"] = track.muted;
                        track_obj["audio_muted"] = track.audio_muted;
                        track_obj["z_index"] = track.z_index;

                        json clips_array = json::array();
                        for (const auto& clip : track.clips) {
                            if (clip.is_gap) continue;  // Don't save gaps
                            json clip_obj;
                            clip_obj["id"] = clip.id;
                            clip_obj["name"] = clip.name;
                            clip_obj["file_path"] = clip.file_path;
                            clip_obj["start_time"] = clip.start_time;
                            clip_obj["duration"] = clip.duration;
                            clip_obj["source_in"] = clip.source_in;
                            clip_obj["source_out"] = clip.source_out;
                            clip_obj["is_gap"] = clip.is_gap;
                            clip_obj["linked_path"] = clip.linked_path;
                            clip_obj["is_linked"] = clip.is_linked;
                            clip_obj["source_fps"] = clip.source_fps;
                            clip_obj["source_width"] = clip.source_width;
                            clip_obj["source_height"] = clip.source_height;
                            clip_obj["source_duration"] = clip.source_duration;
                            clip_obj["has_audio"] = clip.has_audio;
                            clip_obj["audio_muted"] = clip.audio_muted;
                            clips_array.push_back(clip_obj);
                        }
                        track_obj["clips"] = clips_array;
                        edited_tracks_array.push_back(track_obj);
                    }
                    if (!edited_tracks_array.empty()) {
                        item_obj["audio_tracks"] = edited_tracks_array;
                        item_obj["has_audio_edits"] = true;
                        // Save extended timeline duration if set
                        if (item.cached_timeline_duration > 0) {
                            item_obj["cached_timeline_duration"] = item.cached_timeline_duration;
                        }
                        Debug::Log("SaveProject: Saved audio tracks for sequence '" + item.name + "'");
                    }
                }

                // Save cached metadata if available (ONLY for regular videos, NOT image sequences)
                const CombinedMetadata* cached_meta = GetCachedMetadata(item.path);

                // Save VideoMetadata (for regular videos only - image sequences use late-binding)
                if (cached_meta && cached_meta->video_meta && cached_meta->video_meta->is_loaded) {
                    json metadata_obj;
                    // File information
                    metadata_obj["file_name"] = cached_meta->video_meta->file_name;
                    metadata_obj["file_path"] = cached_meta->video_meta->file_path;
                    metadata_obj["file_size"] = cached_meta->video_meta->file_size;
                    // Video properties
                    metadata_obj["width"] = cached_meta->video_meta->width;
                    metadata_obj["height"] = cached_meta->video_meta->height;
                    metadata_obj["frame_rate"] = cached_meta->video_meta->frame_rate;
                    metadata_obj["total_frames"] = cached_meta->video_meta->total_frames;
                    metadata_obj["pixel_format"] = cached_meta->video_meta->pixel_format;
                    metadata_obj["video_codec"] = cached_meta->video_meta->video_codec;
                    metadata_obj["colorspace"] = cached_meta->video_meta->colorspace;
                    metadata_obj["color_primaries"] = cached_meta->video_meta->color_primaries;
                    metadata_obj["color_transfer"] = cached_meta->video_meta->color_transfer;
                    metadata_obj["range_type"] = cached_meta->video_meta->range_type;

                    item_obj["video_metadata"] = metadata_obj;
                }

                // Image sequences use late-binding metadata (extracted on-demand via QueueVideoMetadataExtraction)
                // No persistence needed - metadata is re-extracted when sequences are loaded

                media_pool_array.push_back(item_obj);
            }
            project_data["media_pool"] = media_pool_array;
            project_data["current_timeline_id"] = current_timeline_id;

            // Write to file
            std::ofstream file(save_path);
            if (!file.is_open()) {
                Debug::Log("SaveProject: Failed to open file for writing: " + save_path);
                return;
            }

            file << project_data.dump(2);  // Pretty print with 2-space indent
            file.close();

            current_project_path = save_path;
            Debug::Log("SaveProject: Project saved successfully to " + save_path);

            // Notify listeners that project was saved (used to refresh annotation availability)
            if (project_saved_callback) {
                project_saved_callback();
            }

        } catch (const std::exception& e) {
            Debug::Log("SaveProject: Error - " + std::string(e.what()));
        }
    }

    void ProjectManager::SaveProjectAs() {
        // Always show save dialog (even if project already has a path)
        nfdu8char_t* out_path = nullptr;
        nfdfilteritem_t filter[1] = { { "Union Player Project", "umproj" } };

        // Use current project name as default if available
        std::string default_name = "project.umproj";
        if (!current_project_path.empty()) {
            std::filesystem::path path(current_project_path);
            default_name = path.filename().string();
        }

        nfdresult_t result = NFD_SaveDialogU8(&out_path, filter, 1, nullptr, default_name.c_str());

        if (result != NFD_OKAY) {
            if (result == NFD_ERROR) {
                Debug::Log("SaveProjectAs: File dialog error");
            }
            return;
        }

        std::string save_path = out_path;
        NFD_FreePathU8(out_path);

        // Ensure .umproj extension
        if (save_path.find(".umproj") == std::string::npos) {
            save_path += ".umproj";
        }

        // Temporarily clear project path to force SaveProject to use new path
        std::string original_path = current_project_path;
        current_project_path = save_path;

        // Use existing SaveProject logic
        SaveProject();

        Debug::Log("SaveProjectAs: Project saved to new location: " + save_path);
    }

    void ProjectManager::LoadProject(const std::string& file_path) {
        using json = nlohmann::json;

        // Pause playback before loading project (non-blocking)
        if (video_player && video_player->IsPlaying()) {
            video_player->Pause();
            Debug::Log("LoadProject: Paused playback before loading project");
        }

        // ========================================================================
        // COMPREHENSIVE MODE CLEANUP
        // Exit all active modes before loading new project to prevent
        // stale state from previous media/timeline/playlist sessions
        // ========================================================================

        // 1. Exit timeline mode (if active)
        if (exit_timeline_mode_callback) {
            exit_timeline_mode_callback();
            Debug::Log("LoadProject: Exited timeline mode");
        }

        // 2. Stop any current playback
        if (video_player) {
            video_player->Stop();
            Debug::Log("LoadProject: Stopped video player");
        }

        // Show open dialog if no path provided
        std::string load_path = file_path;
        if (load_path.empty()) {
            nfdu8char_t* out_path = nullptr;
            nfdfilteritem_t filter[1] = { { "Union Player Project", "umproj" } };
            nfdresult_t result = NFD_OpenDialogU8(&out_path, filter, 1, nullptr);

            if (result != NFD_OKAY) {
                if (result == NFD_ERROR) {
                    Debug::Log("LoadProject: File dialog error");
                }
                return;
            }

            load_path = out_path;
            NFD_FreePathU8(out_path);
        }

        try {
            // Read file
            std::ifstream file(load_path);
            if (!file.is_open()) {
                Debug::Log("LoadProject: Failed to open file: " + load_path);
                return;
            }

            json project_data = json::parse(file);
            file.close();

            // Validate version
            std::string version = project_data.value("version", "");
            if (version != "1.0") {
                Debug::Log("LoadProject: Unsupported project version: " + version);
                return;
            }

            // Clear existing project state
            bins.clear();
            media_pool.clear();
            current_timeline_id.clear();
            selected_media_items.clear();

            // Load project-level settings
            project_pipeline_mode_ = StringToPipelineMode(project_data.value("pipeline_mode", "Normal"));
            Debug::Log("LoadProject: Project pipeline mode = " + std::string(PipelineModeToString(project_pipeline_mode_)));

            // Load media_pool first (needed for bins)
            if (project_data.contains("media_pool")) {
                for (const auto& item_json : project_data["media_pool"]) {
                    MediaItem item;
                    item.id = item_json.value("id", "");
                    item.name = item_json.value("name", "");
                    item.path = item_json.value("path", "");
                    item.type = static_cast<MediaType>(item_json.value("type", 0));
                    item.duration = item_json.value("duration", 0.0);
                    item.sequence_id = item_json.value("sequence_id", "");
                    item.clip_count = item_json.value("clip_count", 0);
                    item.is_active = item_json.value("is_active", false);

                    // Image sequence fields
                    item.sequence_pattern = item_json.value("sequence_pattern", "");
                    item.ffmpeg_pattern = item_json.value("ffmpeg_pattern", "");
                    item.frame_count = item_json.value("frame_count", 0);
                    item.start_frame = item_json.value("start_frame", 1);
                    item.end_frame = item_json.value("end_frame", 1);
                    item.frame_rate = item_json.value("frame_rate", 24.0);
                    item.pipeline_mode = StringToPipelineMode(item_json.value("pipeline_mode", "Normal"));

                    // NEW: Restore cached sequence dimensions (for instant loading)
                    if (item.type == MediaType::IMAGE_SEQUENCE || item.type == MediaType::EXR_SEQUENCE) {
                        item.sequence_width = item_json.value("sequence_width", 0);
                        item.sequence_height = item_json.value("sequence_height", 0);

                        if (item.sequence_width > 0 && item.sequence_height > 0) {
                            Debug::Log("LoadProject: Restored sequence dimensions for " + item.name + ": " +
                                      std::to_string(item.sequence_width) + "x" + std::to_string(item.sequence_height));
                        }
                    }

                    // EXR fields
                    item.exr_layer = item_json.value("exr_layer", "");
                    item.exr_layer_display = item_json.value("exr_layer_display", "");

                    // NEW: Restore ImageSequenceData for reliable reload
                    if (item_json.contains("image_seq")) {
                        const auto& seq_json = item_json["image_seq"];
                        item.image_seq.pattern = seq_json.value("pattern", "");
                        item.image_seq.ffmpeg_pattern = seq_json.value("ffmpeg_pattern", "");
                        item.image_seq.directory = seq_json.value("directory", "");
                        item.image_seq.frame_count = seq_json.value("frame_count", 0);
                        item.image_seq.start_frame = seq_json.value("start_frame", 1);
                        item.image_seq.end_frame = seq_json.value("end_frame", 1);
                        item.image_seq.frame_rate = seq_json.value("frame_rate", 24.0);
                        item.image_seq.duration = seq_json.value("duration", 0.0);
                        item.image_seq.width = seq_json.value("width", 0);
                        item.image_seq.height = seq_json.value("height", 0);
                        item.image_seq.pipeline_mode = StringToPipelineMode(seq_json.value("pipeline_mode", "Normal"));
                        item.image_seq.format = seq_json.value("format", "");
                        item.image_seq.layer = seq_json.value("layer", "");
                        item.image_seq.layer_display = seq_json.value("layer_display", "");
                        item.image_seq.audio_file = seq_json.value("audio_file", "");

                        Debug::Log("LoadProject: Restored ImageSequenceData for " + item.name +
                                   " (duration=" + std::to_string(item.image_seq.duration) + "s, " +
                                   std::to_string(item.image_seq.frame_count) + " frames)");
                    } else if (item.type == MediaType::IMAGE_SEQUENCE || item.type == MediaType::EXR_SEQUENCE) {
                        // Fallback: Populate image_seq from legacy fields for backward compatibility
                        item.image_seq.pattern = item.sequence_pattern;
                        item.image_seq.ffmpeg_pattern = item.ffmpeg_pattern;
                        item.image_seq.frame_count = item.frame_count;
                        item.image_seq.start_frame = item.start_frame;
                        item.image_seq.end_frame = item.end_frame;
                        item.image_seq.frame_rate = item.frame_rate;
                        item.image_seq.duration = item.duration;
                        item.image_seq.width = item.sequence_width;
                        item.image_seq.height = item.sequence_height;
                        item.image_seq.pipeline_mode = item.pipeline_mode;
                        item.image_seq.layer = item.exr_layer;
                        item.image_seq.layer_display = item.exr_layer_display;

                        Debug::Log("LoadProject: Populated ImageSequenceData from legacy fields for " + item.name);
                    }

                    // Restore audio tracks for IMAGE_SEQUENCE and EXR_SEQUENCE items
                    if ((item.type == MediaType::IMAGE_SEQUENCE || item.type == MediaType::EXR_SEQUENCE) &&
                        item_json.contains("audio_tracks") && item_json.value("has_audio_edits", false)) {
                        item.has_cached_edits = true;
                        item.cached_tracks.clear();

                        for (const auto& track_json : item_json["audio_tracks"]) {
                            OTIOTrack track;
                            track.id = track_json.value("id", "");
                            track.name = track_json.value("name", "");
                            track.is_video = track_json.value("is_video", false);
                            track.visible = track_json.value("visible", true);
                            track.muted = track_json.value("muted", false);
                            track.audio_muted = track_json.value("audio_muted", false);
                            track.z_index = track_json.value("z_index", 0);

                            if (track_json.contains("clips")) {
                                for (const auto& clip_json : track_json["clips"]) {
                                    OTIOClip clip;
                                    clip.id = clip_json.value("id", "");
                                    clip.name = clip_json.value("name", "");
                                    clip.file_path = clip_json.value("file_path", "");
                                    clip.start_time = clip_json.value("start_time", 0.0);
                                    clip.duration = clip_json.value("duration", 0.0);
                                    clip.source_in = clip_json.value("source_in", 0.0);
                                    clip.source_out = clip_json.value("source_out", 0.0);
                                    clip.is_gap = clip_json.value("is_gap", false);
                                    clip.linked_path = clip_json.value("linked_path", "");
                                    clip.is_linked = clip_json.value("is_linked", false);
                                    clip.source_fps = clip_json.value("source_fps", 0.0);
                                    clip.source_width = clip_json.value("source_width", 0);
                                    clip.source_height = clip_json.value("source_height", 0);
                                    clip.source_duration = clip_json.value("source_duration", 0.0);
                                    clip.has_audio = clip_json.value("has_audio", false);
                                    clip.audio_muted = clip_json.value("audio_muted", false);
                                    track.clips.push_back(clip);
                                }
                            }
                            item.cached_tracks.push_back(track);
                        }
                        // Restore extended timeline duration if saved
                        item.cached_timeline_duration = item_json.value("cached_timeline_duration", 0.0);
                        Debug::Log("LoadProject: Restored " + std::to_string(item.cached_tracks.size()) +
                                   " audio tracks for sequence '" + item.name + "'" +
                                   (item.cached_timeline_duration > 0 ? ", extended duration=" + std::to_string(item.cached_timeline_duration) + "s" : ""));
                    }

                    // In/Out points (per-video range markers)
                    item.in_point = item_json.value("in_point", -1.0);
                    item.out_point = item_json.value("out_point", -1.0);

                    // View state for persistent zoom/pan/playhead per-media
                    item.view_state.zoom_level = item_json.value("view_zoom", 1.0f);
                    item.view_state.scroll_offset = item_json.value("view_scroll", 0.0f);
                    item.view_state.playhead_position = item_json.value("view_playhead", 0.0);

                    // Video-specific fields: dimensions and frame rate (for VIDEO and AUDIO types)
                    if (item.type == MediaType::VIDEO || item.type == MediaType::AUDIO) {
                        item.timeline_width = item_json.value("timeline_width", 1920);
                        item.timeline_height = item_json.value("timeline_height", 1080);
                        item.frame_rate = item_json.value("frame_rate", 0.0);
                    }

                    // Dual view-specific fields (for MediaType::DUAL_VIEW)
                    if (item.type == MediaType::DUAL_VIEW) {
                        item.timeline_id = item_json.value("timeline_id", "");
                        item.timeline_format = item_json.value("timeline_format", "");
                        item.video_track_count = item_json.value("video_track_count", 0);
                        item.audio_track_count = item_json.value("audio_track_count", 0);
                        item.timeline_width = item_json.value("timeline_width", 1920);
                        item.timeline_height = item_json.value("timeline_height", 1080);

                        // OTIO timeline view state (zoom/scroll/playhead/in-out points)
                        item.view_state.timeline_zoom = item_json.value("timeline_view_zoom", 50.0f);
                        item.view_state.timeline_scroll = item_json.value("timeline_view_scroll", 0.0f);
                        item.view_state.timeline_playhead = item_json.value("timeline_view_playhead", 0.0);
                        item.view_state.timeline_in_point = item_json.value("timeline_view_in", -1.0);
                        item.view_state.timeline_out_point = item_json.value("timeline_view_out", -1.0);

                        // NOTE: clip_links and track_metadata loading removed with TIMELINE mode cleanup
                        // Legacy project files with these fields will be ignored

                        // Restore cached edited tracks (full timeline edits)
                        if (item_json.contains("edited_tracks") && item_json.value("has_cached_edits", false)) {
                            item.has_cached_edits = true;
                            item.cached_tracks.clear();
                            for (const auto& track_json : item_json["edited_tracks"]) {
                                OTIOTrack track;
                                track.id = track_json.value("id", "");
                                track.name = track_json.value("name", "");
                                track.is_video = track_json.value("is_video", true);
                                track.visible = track_json.value("visible", true);
                                track.muted = track_json.value("muted", false);
                                track.audio_muted = track_json.value("audio_muted", false);
                                track.z_index = track_json.value("z_index", 0);

                                if (track_json.contains("clips")) {
                                    for (const auto& clip_json : track_json["clips"]) {
                                        OTIOClip clip;
                                        clip.id = clip_json.value("id", "");
                                        clip.name = clip_json.value("name", "");
                                        clip.file_path = clip_json.value("file_path", "");
                                        clip.start_time = clip_json.value("start_time", 0.0);
                                        clip.duration = clip_json.value("duration", 0.0);
                                        clip.source_in = clip_json.value("source_in", 0.0);
                                        clip.source_out = clip_json.value("source_out", 0.0);
                                        clip.is_gap = clip_json.value("is_gap", false);
                                        clip.linked_path = clip_json.value("linked_path", "");
                                        clip.is_linked = clip_json.value("is_linked", false);
                                        clip.source_fps = clip_json.value("source_fps", 0.0);
                                        clip.source_width = clip_json.value("source_width", 0);
                                        clip.source_height = clip_json.value("source_height", 0);
                                        clip.source_duration = clip_json.value("source_duration", 0.0);
                                        clip.has_audio = clip_json.value("has_audio", false);
                                        clip.has_fade_in = clip_json.value("has_fade_in", false);
                                        clip.has_fade_out = clip_json.value("has_fade_out", false);
                                        clip.fade_in_duration = clip_json.value("fade_in_duration", 0.0);
                                        clip.fade_out_duration = clip_json.value("fade_out_duration", 0.0);
                                        clip.audio_muted = clip_json.value("audio_muted", false);
                                        // Image sequence fields
                                        clip.is_sequence = clip_json.value("is_sequence", false);
                                        clip.sequence_directory = clip_json.value("sequence_directory", "");
                                        clip.sequence_pattern = clip_json.value("sequence_pattern", "");
                                        clip.sequence_start_frame = clip_json.value("sequence_start_frame", 1);
                                        clip.sequence_end_frame = clip_json.value("sequence_end_frame", 1);
                                        clip.sequence_exr_layer = clip_json.value("sequence_exr_layer", "");
                                        // AAF-specific fields
                                        clip.aaf_mob_id = clip_json.value("aaf_mob_id", "");
                                        // Nested timeline support
                                        clip.is_nested = clip_json.value("is_nested", false);
                                        clip.nested_fps = clip_json.value("nested_fps", 0.0);
                                        clip.nested_name = clip_json.value("nested_name", "");
                                        clip.nested_loaded = clip_json.value("nested_loaded", false);
                                        clip.nested_timeline_json = clip_json.value("nested_timeline_json", "");
                                        // Load nested tracks if present
                                        if (clip_json.contains("nested_tracks")) {
                                            for (const auto& nested_track_json : clip_json["nested_tracks"]) {
                                                OTIOTrack nested_track;
                                                nested_track.id = nested_track_json.value("id", "");
                                                nested_track.name = nested_track_json.value("name", "");
                                                nested_track.is_video = nested_track_json.value("is_video", true);
                                                nested_track.visible = nested_track_json.value("visible", true);
                                                nested_track.muted = nested_track_json.value("muted", false);
                                                nested_track.audio_muted = nested_track_json.value("audio_muted", false);
                                                nested_track.z_index = nested_track_json.value("z_index", 0);
                                                if (nested_track_json.contains("clips")) {
                                                    for (const auto& nested_clip_json : nested_track_json["clips"]) {
                                                        OTIOClip nested_clip;
                                                        nested_clip.id = nested_clip_json.value("id", "");
                                                        nested_clip.name = nested_clip_json.value("name", "");
                                                        nested_clip.file_path = nested_clip_json.value("file_path", "");
                                                        nested_clip.start_time = nested_clip_json.value("start_time", 0.0);
                                                        nested_clip.duration = nested_clip_json.value("duration", 0.0);
                                                        nested_clip.source_in = nested_clip_json.value("source_in", 0.0);
                                                        nested_clip.source_out = nested_clip_json.value("source_out", 0.0);
                                                        nested_clip.is_gap = nested_clip_json.value("is_gap", false);
                                                        nested_clip.linked_path = nested_clip_json.value("linked_path", "");
                                                        nested_clip.is_linked = nested_clip_json.value("is_linked", false);
                                                        nested_clip.aaf_mob_id = nested_clip_json.value("aaf_mob_id", "");
                                                        nested_track.clips.push_back(nested_clip);
                                                    }
                                                }
                                                clip.nested_tracks.push_back(nested_track);
                                            }
                                        }
                                        track.clips.push_back(clip);
                                    }
                                }
                                item.cached_tracks.push_back(track);
                            }
                            Debug::Log("LoadProject: Restored " + std::to_string(item.cached_tracks.size()) +
                                       " edited tracks for timeline '" + item.name + "'");
                        }

                        Debug::Log("LoadProject: Restored timeline '" + item.name + "' with dimensions " +
                                  std::to_string(item.timeline_width) + "x" + std::to_string(item.timeline_height));
                    }

                    // Playlist-specific fields (for MediaType::PLAYLIST)
                    if (item.type == MediaType::PLAYLIST) {
                        item.playlist_loop = item_json.value("playlist_loop", false);
                        item.current_playlist_index = 0;  // Always start at 0 on load

                        if (item_json.contains("playlist_items")) {
                            for (const auto& entry_json : item_json["playlist_items"]) {
                                PlaylistItemEntry entry;
                                entry.media_id = entry_json.value("media_id", "");
                                entry.in_point = entry_json.value("in_point", -1.0);
                                entry.out_point = entry_json.value("out_point", -1.0);

                                // Add playlist item without validation during load
                                // (Referenced items may appear later in the media_pool array)
                                // Validation happens at runtime when playlist is opened
                                item.playlist_items.push_back(entry);
                                Debug::Log("LoadProject: Playlist item media_id=" + entry.media_id + " added");
                            }
                        }

                        Debug::Log("LoadProject: Restored playlist '" + item.name + "' with " +
                                   std::to_string(item.playlist_items.size()) + " items");
                    }

                    // Load cached video metadata if available
                    if (item_json.contains("video_metadata")) {
                        VideoMetadata metadata;
                        auto meta_obj = item_json["video_metadata"];

                        // File information - derive from item.path if not saved (backwards compatibility)
                        metadata.file_name = meta_obj.value("file_name", "");
                        metadata.file_path = meta_obj.value("file_path", "");
                        metadata.file_size = meta_obj.value("file_size", static_cast<uint64_t>(0));

                        // If file info is empty, derive from item.path (older project files)
                        if (metadata.file_name.empty() && !item.path.empty()) {
                            metadata.file_path = item.path;
                            metadata.file_name = std::filesystem::path(item.path).filename().string();
                            // Try to get file size from disk
                            try {
                                if (std::filesystem::exists(item.path)) {
                                    metadata.file_size = std::filesystem::file_size(item.path);
                                }
                            } catch (...) {}
                        }

                        // Video properties
                        metadata.width = meta_obj.value("width", 0);
                        metadata.height = meta_obj.value("height", 0);
                        metadata.frame_rate = meta_obj.value("frame_rate", 24.0);
                        metadata.total_frames = meta_obj.value("total_frames", 0);
                        metadata.pixel_format = meta_obj.value("pixel_format", "");
                        metadata.video_codec = meta_obj.value("video_codec", "");
                        metadata.colorspace = meta_obj.value("colorspace", "");
                        metadata.color_primaries = meta_obj.value("color_primaries", "");
                        metadata.color_transfer = meta_obj.value("color_transfer", "");
                        metadata.range_type = meta_obj.value("range_type", "");
                        metadata.is_loaded = true;

                        // Cache in memory
                        std::lock_guard<std::mutex> lock(queue_mutex);
                        auto& cached = metadata_cache[item.path];
                        cached.video_meta = std::make_unique<VideoMetadata>(metadata);
                        cached.state = MetadataState::VIDEO_READY;

                        Debug::Log("LoadProject: Restored video metadata for: " + item.path);
                    }

                    // Image sequences use late-binding metadata (extracted when loaded via QueueVideoMetadataExtraction)
                    // No restoration needed - metadata will be re-extracted on first load

                    media_pool.push_back(item);
                }
            }

            // Load bins (references media_pool by ID)
            if (project_data.contains("bins")) {
                for (const auto& bin_json : project_data["bins"]) {
                    ProjectBin bin;
                    bin.name = bin_json.value("name", "");
                    bin.is_open = bin_json.value("is_open", true);

                    // Populate bin items from media_pool
                    if (bin_json.contains("items")) {
                        for (const auto& item_id : bin_json["items"]) {
                            std::string id = item_id.get<std::string>();
                            for (const auto& media_item : media_pool) {
                                if (media_item.id == id) {
                                    bin.items.push_back(media_item);
                                    break;
                                }
                            }
                        }
                    }

                    bins.push_back(bin);
                    Debug::Log("LoadProject: Loaded bin '" + bin.name + "' with " + std::to_string(bin.items.size()) + " items");
                }
            }

            // Ensure all required bins exist (for projects created before new bins were added)
            std::vector<std::string> required_bins = {"Videos", "Audio", "Images", "Dual Views", "Playlists"};
            for (size_t i = 0; i < required_bins.size(); i++) {
                if (bins.size() <= i) {
                    ProjectBin new_bin;
                    new_bin.name = required_bins[i];
                    new_bin.is_open = true;
                    bins.push_back(new_bin);
                    Debug::Log("LoadProject: Created missing bin '" + required_bins[i] + "'");
                }
            }

            // Restore current timeline
            current_timeline_id = project_data.value("current_timeline_id", "");

            // Update project path
            current_project_path = load_path;

            Debug::Log("LoadProject: Project loaded successfully from " + load_path);
            Debug::Log("  - " + std::to_string(media_pool.size()) + " media items");
            Debug::Log("  - " + std::to_string(bins.size()) + " bins");

            // Update ID counter to prevent duplicate IDs when adding new items
            UpdateIDCounter();

            // Clear all runtime "active" states - nothing should be active when loading a project
            ClearDualViewActiveStates();

        } catch (const std::exception& e) {
            Debug::Log("LoadProject: Error - " + std::string(e.what()));
        }
    }

    void ProjectManager::OnVideoLoaded(const std::string& file_path) {
        if (file_path.empty()) return;

        // === NOTIFY MAIN ABOUT VIDEO CHANGE ===
        if (video_change_callback) {
            video_change_callback(file_path);
        }

        // === CHECK IF CACHE WAS AUTO-DISABLED FOR PREVIOUS CODEC ===
        // If cache was disabled for H.264/H.265, check if new video has a safe codec and restore cache
        if (!cache_enabled && cache_auto_disabled_for_codec) {
            std::thread([this, file_path]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));

                // CRITICAL: Verify this video is still the current one before initializing cache
                // User might have loaded a different video during the 200ms delay
                if (!current_file_path || *current_file_path != file_path) {
                    Debug::Log("OnVideoLoaded: Video changed before cache restoration, aborting (was: " + file_path + ", now: " +
                               (current_file_path ? *current_file_path : "(null)") + ")");
                    return;
                }

                Debug::Log("OnVideoLoaded: Cache was auto-disabled, checking new video codec: " + file_path);

                // Check if this is a GIF (should keep cache disabled)
                std::string extension = std::filesystem::path(file_path).extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
                bool is_gif = (extension == ".gif");

                const CombinedMetadata* cached_meta = GetCachedMetadata(file_path);

                if (cached_meta && cached_meta->video_meta && cached_meta->video_meta->is_loaded) {
                    std::string codec = cached_meta->video_meta->video_codec;
                    Debug::Log("OnVideoLoaded: Found cached metadata, codec = '" + codec + "'");
                    std::transform(codec.begin(), codec.end(), codec.begin(), ::tolower);

                    bool is_h264_h265 = (codec.find("h264") != std::string::npos ||
                                        codec.find("h.264") != std::string::npos ||
                                        codec.find("h265") != std::string::npos ||
                                        codec.find("h.265") != std::string::npos ||
                                        codec.find("hevc") != std::string::npos);

                    if (!is_h264_h265 && !is_gif) {
                        Debug::Log("OnVideoLoaded: Safe codec detected, restoring cache and initializing");
                        SetCacheEnabled(user_cache_preference);
                        cache_auto_disabled_for_codec = false;
                        current_video_codec = "";

                        // Double-check video is still current before initializing cache
                        if (current_file_path && *current_file_path == file_path && user_cache_preference && video_cache_manager) {
                            NotifyVideoChanged(file_path);
                        } else {
                            Debug::Log("OnVideoLoaded: Video changed before NotifyVideoChanged, aborting cache init");
                        }
                    } else {
                        Debug::Log("OnVideoLoaded: H.264/H.265/GIF codec detected, cache remains disabled");
                        current_video_codec = cached_meta->video_meta->video_codec;
                    }
                } else {
                    Debug::Log("OnVideoLoaded: No cached metadata yet, will check when metadata arrives");
                }
            }).detach();
        }
        // === DEFER CACHE INITIALIZATION TO BACKGROUND ===
        // Previously blocked for 300ms before starting cache - this delayed viewport updates
        // Now we defer cache init to background thread, allowing immediate playback
        else if (cache_enabled) {
            std::thread([this, file_path]() {
                // Brief delay to let MPV start playback first
                std::this_thread::sleep_for(std::chrono::milliseconds(200));

                // Initialize cache in background without blocking viewport
                NotifyVideoChanged(file_path);
            }).detach();
        }

        // === METADATA NOW EXTRACTED BY FFMPEG BEFORE MPV LOAD ===
        // Previously queued MPV metadata extraction here - no longer needed
        // FFmpeg extracts all video metadata upfront in LoadSingleMediaItem()
        // Only queue Adobe metadata (timecode, project links) if needed
        if (video_player && video_player->HasVideo()) {
            QueueAdobeMetadata(file_path);  // Optional: timecode and project links
        }

        // Check if this file is already in the project
        for (auto& item : media_pool) {
            if (item.path == file_path) {
                // Update duration now that video is loaded
                if (video_player && video_player->HasVideo()) {
                    double new_duration = video_player->GetDuration();
                    if (new_duration > 0) {
                        item.duration = new_duration;

                        // Also update in bins
                        for (auto& bin : bins) {
                            for (auto& bin_item : bin.items) {
                                if (bin_item.path == file_path) {
                                    bin_item.duration = new_duration;
                                    break;
                                }
                            }
                        }
                    }
                }
                return;
            }
        }

        AddCurrentVideoToProject();
    }

    // ============================================================================
    // UI RENDERING
    // ============================================================================

    void ProjectManager::CreateProjectPanel(bool* show_project_panel) {
        if (!show_project_panel || !*show_project_panel) return;

        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));  // Transparent border
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.07f, 0.07f, 1.0f));  // #121212 background
        if (ImGui::Begin("Project", show_project_panel)) {
            // Header with icon
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImGui::Text(ICON_TOPIC);
                ImGui::PopFont();
                ImGui::SameLine();
            }
            ImGui::Text("Project Manager");
            ImGui::PopStyleColor();

            // Close button on the right
            float button_size = ImGui::GetFontSize() + 4.0f;  // Compact size
            ImGui::SameLine(ImGui::GetWindowWidth() - button_size - ImGui::GetStyle().WindowPadding.x);
            ImVec2 button_pos = ImGui::GetCursorScreenPos();
            bool clicked = ImGui::InvisibleButton("##CloseProject", ImVec2(button_size, button_size));
            bool hovered = ImGui::IsItemHovered();
            // Draw icon centered - disabled color by default, regular on hover
            if (font_icons) {
                ImGui::PushFont(font_icons);
                ImVec2 icon_size = ImGui::CalcTextSize(ICON_CLOSE);
                ImVec2 icon_pos = ImVec2(button_pos.x + (button_size - icon_size.x) / 2,
                                         button_pos.y + (button_size - icon_size.y) / 2 - 1.0f);
                ImU32 icon_col = hovered ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
                ImGui::GetWindowDrawList()->AddText(icon_pos, icon_col, ICON_CLOSE);
                ImGui::PopFont();
            }
            if (clicked) {
                *show_project_panel = false;
            }
            if (hovered) {
                ImGui::SetTooltip("Close Project (Ctrl+1)");
            }

            CreateProjectInfo();
            ImGui::Separator();
            CreateMediaPool();
        }
        ImGui::End();
        ImGui::PopStyleColor(2);  // Transparent border + #1b1b1b background
    }

    void ProjectManager::CreateNewProjectFromMenu() {
        // Fresh start - clear everything
        bins.clear();
        media_pool.clear();
        current_timeline_id.clear();
        selected_media_items.clear();
        current_project_path.clear();
        project_pipeline_mode_ = PipelineMode::NORMAL;  // Reset to default

        // Stop playback and clear current media
        if (video_player) {
            video_player->Stop();
        }
        if (current_file_path) {
            current_file_path->clear();
        }

        // Recreate default bins
        CreateNewBin("Videos");
        CreateNewBin("Audio");
        CreateNewBin("Images");
        CreateNewBin("Dual Views");
        CreateNewBin("Playlists");

        Debug::Log("New Project: Fresh start - all project data cleared and media stopped");
    }

    void ProjectManager::CreateProjectInfo() {
        if (current_project_path.empty()) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No project loaded");
            return;
        }

        std::string project_name = GetProjectName(current_project_path);
        ImGui::Text("Project: %s", project_name.c_str());
        ImGui::SameLine();
        if (font_regular) ImGui::PushFont(font_regular);
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(%zu items)", media_pool.size());
        if (font_regular) ImGui::PopFont();
    }

    void ProjectManager::CreateMediaPool() {
        for (auto& bin : bins) {
            CreateBinUI(bin);
            // Inline toolbars removed - use menu bar instead
        }

        // Context menu for empty space below the tree
        // Use remaining space as an invisible button for right-click detection
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.y > 10.0f) {  // Only if there's meaningful empty space
            ImGui::InvisibleButton("##EmptySpaceContextArea", ImVec2(avail.x, avail.y));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                ImGui::OpenPopup("##EmptySpaceContextMenu");
            }
        }

        // Context menu popup
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.065f, 0.065f, 0.065f, 1.0f));
        if (ImGui::BeginPopup("##EmptySpaceContextMenu")) {
            if (ImGui::MenuItem("Open Media...")) {
                // Trigger file open dialog (will be handled by main.cpp)
                pending_open_media_dialog = true;
            }
            if (ImGui::MenuItem("Open Project...")) {
                pending_open_project_dialog = true;
            }
            ImGui::Separator();
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
    }

    void ProjectManager::CreateBinUI(ProjectBin& bin) {
        // Build header text with item count
        char header_text[128];
        size_t item_count = bin.items.size();
        if (item_count > 0) {
            snprintf(header_text, sizeof(header_text), "%s (%zu)", bin.name.c_str(), item_count);
        } else {
            snprintf(header_text, sizeof(header_text), "%s", bin.name.c_str());
        }

        // Unique ID for state persistence
        std::string header_id = header_text + std::string("##bin_") + bin.name;

        // Set open state flag
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_None;
        if (bin.is_open) {
            flags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        // Style the collapsing header
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));

        bool node_open = ImGui::CollapsingHeader(header_id.c_str(), flags);
        ImGui::PopStyleColor(3);

        // Right-click context menu for bin headers
        std::string bin_context_id = "bin_context_" + bin.name;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            ImGui::OpenPopup(bin_context_id.c_str());
        }

        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.07f, 0.07f, 1.0f));
        if (ImGui::BeginPopup(bin_context_id.c_str())) {
            if (bin.name == "Dual Views") {
                if (ImGui::MenuItem("New Dual View...")) {
                    show_new_dual_view_dialog = true;
                }
            }
            else if (bin.name == "Playlists") {
                if (ImGui::MenuItem("New Playlist...")) {
                    show_new_playlist_dialog = true;
                }
            }
            else {
                // Empty bins (Videos, Audio, Images) - show disabled header
                ImGui::TextDisabled("%s Bin", bin.name.c_str());
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();

        if (node_open) {
            // Add slight indent for bin contents
            ImGui::Indent(8.0f);
            for (const auto& item : bin.items) {
                CreateMediaItemUI(item);
            }
            ImGui::Unindent(8.0f);
        }

        bin.is_open = node_open;
    }

    void ProjectManager::CreateMediaItemUI(const MediaItem& item) {
        ImGui::PushID(item.id.c_str());

        bool is_selected = IsItemSelected(item.id);
        std::string display_name = item.name;
        ImVec4 text_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

        if (item.type == MediaType::SEQUENCE) {
            if (item.clip_count == 0) {
                display_name += " (empty)";
                text_color = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
            }
            else {
                display_name += " (" + std::to_string(item.clip_count) + " clips)";
                text_color = Bright(GetWindowsAccentColor());  // Use bright accent color for playlists
            }

            if (item.is_active) {
                display_name += " [ACTIVE]";
                text_color = Bright(GetWindowsAccentColor());
            }
        }
        else if (item.type == MediaType::DUAL_VIEW) {
            // Dual view display - show LEFT/RIGHT tracks
            display_name += " [L/R]";

            // Check if this dual view is active
            if (item.is_active) {
                display_name += " [ACTIVE]";
                text_color = Bright(GetWindowsAccentColor());
            }
        }
        else if (item.type == MediaType::PLAYLIST) {
            // Playlist display - show item count
            display_name += " [" + std::to_string(item.playlist_items.size()) + " items]";

            // Check if this playlist is active
            if (item.is_active) {
                display_name += " [PLAYING]";
                text_color = Bright(GetWindowsAccentColor());
            }
        }
        else {
            if (current_file_path && !current_file_path->empty() && item.path == *current_file_path) {
                display_name += " [ACTIVE]";
                text_color = Bright(GetWindowsAccentColor());
            }
        }

        if (is_selected) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
        }

        if (font_regular) ImGui::PushFont(font_regular);
        ImGui::PushStyleColor(ImGuiCol_Text, text_color);
        bool clicked = ImGui::Selectable(display_name.c_str(), is_selected, ImGuiSelectableFlags_AllowDoubleClick);
        ImGui::PopStyleColor();
        if (font_regular) ImGui::PopFont();

        if (is_selected) {
            ImGui::PopStyleColor(3);
        }

        // Handle click events
        if (clicked) {
            HandleMediaItemClick(item);
        }

        // Handle right-click context menu
        HandleMediaItemRightClick(item);

        // Handle drag & drop
        HandleMediaItemDragDrop(item, is_selected);

        // Show duration info for media files
        if (item.type != MediaType::SEQUENCE) {
            ImGui::SameLine();
            std::string type_str;
            switch (item.type) {
            case MediaType::VIDEO: type_str = "video"; break;
            case MediaType::AUDIO: type_str = "audio"; break;
            case MediaType::IMAGE: type_str = "image"; break;
            case MediaType::IMAGE_SEQUENCE:
                type_str = "sequence [" + std::to_string(item.frame_count) + " frames @ " + std::to_string(static_cast<int>(item.frame_rate)) + "fps]";
                break;
            case MediaType::EXR_SEQUENCE:
                type_str = "EXR sequence [" + std::to_string(item.frame_count) + " frames @ " + std::to_string(static_cast<int>(item.frame_rate)) + "fps]";
                if (!item.exr_layer_display.empty()) {
                    type_str += " - " + item.exr_layer_display;
                }
                break;
            case MediaType::DUAL_VIEW:
                type_str = "comparison [" + std::to_string(item.timeline_width) + "x" + std::to_string(item.timeline_height) + "]";
                break;
            case MediaType::PLAYLIST:
                type_str = "playlist [" + std::to_string(item.playlist_items.size()) + " items]";
                break;
            default: type_str = "unknown"; break;
            }

            if (item.type == MediaType::DUAL_VIEW || item.type == MediaType::PLAYLIST) {
                // Dual views and playlists show type info, no duration
                ImGui::TextDisabled("[%s]", type_str.c_str());
            } else if (item.duration > 0) {
                ImGui::TextDisabled("[%s] %.2fs", type_str.c_str(), item.duration);
            }
        }

        ImGui::PopID();
    }

    void ProjectManager::HandleProjectDialogs() {
        // New Project Dialog
        if (show_new_project_dialog) {
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImVec2 center = viewport->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::OpenPopup("New Project");
            show_new_project_dialog = false;
        }

        float project_scale = ImGui::GetIO().FontGlobalScale;
        ImGui::SetNextWindowSize(ImVec2(400 * project_scale + 50, 0), ImGuiCond_Always);
        if (ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char project_name[256] = "Untitled Project";
            static char project_path[512] = "";

            ImGui::Text("Create New Project");
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("Project Name:");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##ProjectNameInput", project_name, sizeof(project_name));
            ImGui::Spacing();
            ImGui::Text("Location:");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##ProjectPathInput", project_path, sizeof(project_path));
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Create and Cancel buttons (flush right)
            float btnPadding = 8.0f * 2;
            float createW = ImGui::CalcTextSize("Create").x + btnPadding;
            float cancelW = ImGui::CalcTextSize("Cancel").x + btnPadding;
            float btnSpacing = ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - createW - cancelW - btnSpacing);

            if (ImGui::Button("Create")) {
                CreateNewProject(project_name, project_path);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        // Rename Dialog
        if (show_rename_dialog) {
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImVec2 center = viewport->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::OpenPopup("Rename Item");
            show_rename_dialog = false;
        }

        float rename_scale = ImGui::GetIO().FontGlobalScale;
        ImGui::SetNextWindowSize(ImVec2(350 * rename_scale + 50, 0), ImGuiCond_Always);
        if (ImGui::BeginPopupModal("Rename Item", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Enter new name:");
            ImGui::Spacing();
            ImGui::SetNextItemWidth(-1);
            bool enter_pressed = ImGui::InputText("##RenameInput", rename_buffer, sizeof(rename_buffer), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // OK and Cancel buttons (flush right)
            float btnPadding = 8.0f * 2;
            float okW = ImGui::CalcTextSize("OK").x + btnPadding;
            float cancelW = ImGui::CalcTextSize("Cancel").x + btnPadding;
            float btnSpacing = ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - okW - cancelW - btnSpacing);

            if (ImGui::Button("OK") || enter_pressed) {
                ProcessRenameItem();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        // New Dual View Dialog
        if (show_new_dual_view_dialog) {
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImVec2 center = viewport->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::OpenPopup("New Dual View");
            show_new_dual_view_dialog = false;
            // Reset to defaults when opening
            memset(new_dual_view_name_buffer, 0, sizeof(new_dual_view_name_buffer));
            new_dual_view_resolution_preset = 0;  // 1080p
            new_dual_view_width = 1920;
            new_dual_view_height = 1080;
            new_dual_view_fps = 23.976;
        }

        float dv_scale = ImGui::GetIO().FontGlobalScale;
        ImGui::SetNextWindowSize(ImVec2(350 * dv_scale, 0), ImGuiCond_Always);
        if (ImGui::BeginPopupModal("New Dual View", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Create a new dual view comparison");
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextDisabled("Dual View Name:");
            ImGui::SetNextItemWidth(280 * dv_scale);
            ImGui::InputText("##DualViewNameInput", new_dual_view_name_buffer, sizeof(new_dual_view_name_buffer));

            ImGui::Spacing();

            // Resolution preset dropdown
            ImGui::TextDisabled("Resolution:");
            ImGui::SetNextItemWidth(280 * dv_scale);
            const char* resolution_presets[] = { "1920x1080 (1080p)", "3840x2160 (4K)", "1080x1080 (Square)", "1080x1920 (Portrait)", "Custom" };
            if (ImGui::Combo("##DualViewResolution", &new_dual_view_resolution_preset, resolution_presets, IM_ARRAYSIZE(resolution_presets))) {
                switch (new_dual_view_resolution_preset) {
                    case 0: new_dual_view_width = 1920; new_dual_view_height = 1080; break;
                    case 1: new_dual_view_width = 3840; new_dual_view_height = 2160; break;
                    case 2: new_dual_view_width = 1080; new_dual_view_height = 1080; break;
                    case 3: new_dual_view_width = 1080; new_dual_view_height = 1920; break;
                    // case 4: Custom - keep existing values
                }
            }

            // Custom resolution inputs (only shown when Custom is selected)
            if (new_dual_view_resolution_preset == 4) {
                ImGui::SetNextItemWidth(135 * dv_scale);
                ImGui::InputInt("##DualViewWidth", &new_dual_view_width);
                ImGui::SameLine();
                ImGui::Text("x");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(135 * dv_scale);
                ImGui::InputInt("##DualViewHeight", &new_dual_view_height);
            }

            ImGui::Spacing();

            // Frame rate
            ImGui::TextDisabled("Frame Rate:");
            ImGui::SetNextItemWidth(280 * dv_scale);
            const char* fps_presets[] = { "23.976", "24", "25", "29.97", "30", "48", "50", "59.94", "60" };
            double fps_values[] = { 23.976, 24.0, 25.0, 29.97, 30.0, 48.0, 50.0, 59.94, 60.0 };
            int current_fps_idx = 0;  // Default to 23.976
            for (int i = 0; i < IM_ARRAYSIZE(fps_values); i++) {
                if (std::abs(new_dual_view_fps - fps_values[i]) < 0.01) {
                    current_fps_idx = i;
                    break;
                }
            }
            if (ImGui::Combo("##DualViewFPS", &current_fps_idx, fps_presets, IM_ARRAYSIZE(fps_presets))) {
                new_dual_view_fps = fps_values[current_fps_idx];
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Drag media to LEFT and RIGHT tracks after creation.");

            ImGui::Spacing();
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Button("Create")) {
                std::string name = new_dual_view_name_buffer;
                if (name.empty()) {
                    name = "";  // CreateDualView will auto-generate name
                }
                std::string dual_view_id = CreateDualView(name, new_dual_view_width, new_dual_view_height, new_dual_view_fps);
                // Open the dual view immediately
                if (!dual_view_id.empty() && dual_view_editor_callback) {
                    dual_view_editor_callback(dual_view_id);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        // New Playlist Dialog
        if (show_new_playlist_dialog) {
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImVec2 center = viewport->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::OpenPopup("New Playlist");
            show_new_playlist_dialog = false;
            memset(new_playlist_name_buffer, 0, sizeof(new_playlist_name_buffer));
        }

        float playlist_scale = ImGui::GetIO().FontGlobalScale;
        ImGui::SetNextWindowSize(ImVec2(350 * playlist_scale, 0), ImGuiCond_Always);
        if (ImGui::BeginPopupModal("New Playlist", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Create a new playlist to queue media for sequential playback.");
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextDisabled("Playlist Name:");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##PlaylistName", new_playlist_name_buffer, sizeof(new_playlist_name_buffer));

            ImGui::Spacing();
            ImGui::TextDisabled("Drag media from the Project panel to add items.");
            ImGui::TextDisabled("Click items to jump and play.");

            ImGui::Spacing();
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Button("Create")) {
                std::string name = new_playlist_name_buffer;
                if (name.empty()) {
                    name = "";  // CreateNewPlaylist will auto-generate name
                }
                std::string playlist_id = CreateNewPlaylist(name);
                // Open the playlist in unified timeline editor
                if (!playlist_id.empty()) {
                    OpenPlaylistInTimelineEditor(playlist_id);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        // Process pending transcode load (deferred from worker thread)
        if (pending_transcode_load.load()) {
            pending_transcode_load.store(false);

            std::string first_file;
            double frame_rate;
            {
                std::lock_guard<std::mutex> lock(pending_transcode_mutex);
                first_file = pending_transcode_first_file;
                frame_rate = pending_transcode_frame_rate;
                pending_transcode_first_file.clear();
            }

            if (!first_file.empty()) {
                Debug::Log("ProjectManager: Loading transcoded sequence from main thread: " + first_file);
                ProcessImageSequence(first_file, frame_rate, "");  // Empty layer = transcoded single-layer
            }
        }

        // Frame Rate Selection Dialog for Image Sequences
        if (show_frame_rate_dialog && !frame_rate_dialog_opened) {
            Debug::Log("Opening frame rate popup");
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImVec2 center = viewport->GetCenter();
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::OpenPopup("Select Frame Rate");
            frame_rate_dialog_opened = true;
        }

        if (ImGui::BeginPopupModal("Select Frame Rate", &show_frame_rate_dialog, ImGuiWindowFlags_AlwaysAutoResize)) {
            // Show appropriate title based on single image vs sequence
            if (is_single_image) {
                ImGui::Text("Single image detected:");
            } else {
                ImGui::Text("Image sequence detected:");
            }

            // Safety check for valid path
            if (!pending_sequence_path.empty()) {
                try {
                    ImGui::Text("%s", std::filesystem::path(pending_sequence_path).filename().string().c_str());
                } catch (const std::exception&) {
                    ImGui::Text("Invalid sequence path");
                }
            } else {
                ImGui::Text("No sequence selected");
            }
            ImGui::Separator();

            // EXR Layer Selection (if applicable)
            // Thread-safe access to layer data
            std::vector<std::string> layer_names_copy;
            std::vector<std::string> layer_display_names_copy;
            int current_layer_index = 0;
            {
                std::lock_guard<std::mutex> lock(exr_layers_mutex);
                layer_names_copy = exr_layer_names;
                layer_display_names_copy = exr_layer_display_names;
                current_layer_index = selected_exr_layer_index;
            }

            if (is_exr_sequence && !layer_display_names_copy.empty()) {
                ImGui::Text("Select EXR Layer:");

                // Bounds check
                if (current_layer_index >= layer_display_names_copy.size()) {
                    current_layer_index = 0;
                }

                // Show layer selection combo box
                ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.07f, 0.07f, 1.00f));
                if (ImGui::BeginCombo("##exr_layer", layer_display_names_copy[current_layer_index].c_str())) {
                    for (int i = 0; i < layer_display_names_copy.size(); i++) {
                        bool is_selected = (current_layer_index == i);
                        if (ImGui::Selectable(layer_display_names_copy[i].c_str(), is_selected)) {
                            std::lock_guard<std::mutex> lock(exr_layers_mutex);
                            selected_exr_layer_index = i;
                            current_layer_index = i;
                        }
                        if (is_selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopStyleColor();

                if (current_layer_index < layer_names_copy.size()) {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Layer: %s", layer_names_copy[current_layer_index].c_str());
                }

                // Show hidden Cryptomatte layers feedback
                if (hidden_cryptomatte_count > 0) {
                        ImGui::TextDisabled("%d Cryptomatte layer%s hidden",
                        hidden_cryptomatte_count,
                        hidden_cryptomatte_count == 1 ? "" : "s");
                    }

                ImGui::Separator();
            }

            // Transcode Options (for both EXR and TIFF/PNG sequences)
            if (is_exr_sequence || is_tiff_png_sequence) {
                ImGui::Text("Performance Optimization:");

                std::string transcode_label = is_exr_sequence ?
                    "Transcode EXR (optimize for playback)" :
                    "Transcode to EXR (optimize for playback)";

                if (ImGui::Checkbox(transcode_label.c_str(), &exr_transcode_enabled)) {
                    // Reset to defaults when enabling
                    if (exr_transcode_enabled) {
                        exr_transcode_max_width = 1920;  // 1920 default
                        exr_transcode_compression = 7;   // B44A (lossy, 32:1 ratio, fast)
                    }
                }

                if (exr_transcode_enabled) {
                    ImGui::Indent();

                    std::string transcode_help = is_exr_sequence ?
                        "Create optimized single-layer copy for smooth playback" :
                        "Convert to EXR with B44A compression for smooth playback";

                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", transcode_help.c_str());

                    // Max Width dropdown
                    const char* width_options[] = { "Native", "1920", "2560", "3840", "Custom" };
                    int width_index = (exr_transcode_max_width == 0) ? 0 :
                                     (exr_transcode_max_width == 1920) ? 1 :
                                     (exr_transcode_max_width == 2560) ? 2 :
                                     (exr_transcode_max_width == 3840) ? 3 : 4;

                    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.07f, 0.07f, 1.00f));
                    if (ImGui::Combo("Max Width", &width_index, width_options, IM_ARRAYSIZE(width_options))) {
                        switch (width_index) {
                            case 0: exr_transcode_max_width = 0; break;     // Native
                            case 1: exr_transcode_max_width = 1920; break;
                            case 2: exr_transcode_max_width = 2560; break;
                            case 3: exr_transcode_max_width = 3840; break;
                            case 4: exr_transcode_max_width = 1920; break;  // Custom - default to 1920
                        }
                    }
                    ImGui::PopStyleColor();

                    // Custom width input
                    if (width_index == 4) {
                        ImGui::InputInt("Custom Width", &exr_transcode_max_width, 64, 256);
                        if (exr_transcode_max_width < 64) exr_transcode_max_width = 64;
                        if (exr_transcode_max_width > 16384) exr_transcode_max_width = 16384;
                    }

                    // Compression dropdown
                    const char* compression_options[] = { "B44A (Lossy, 32:1, Fast)", "B44 (Lossy, 44:1)", "DWAA (Lossy)", "DWAB (Lossy)", "PIZ (Lossless)", "ZIP (Lossless)" };
                    int compression_index = (exr_transcode_compression == 7) ? 0 :  // B44A (default)
                                           (exr_transcode_compression == 6) ? 1 :  // B44
                                           (exr_transcode_compression == 8) ? 2 :  // DWAA
                                           (exr_transcode_compression == 9) ? 3 :  // DWAB
                                           (exr_transcode_compression == 4) ? 4 :  // PIZ
                                           (exr_transcode_compression == 3) ? 5 : 0;  // ZIP

                    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.07f, 0.07f, 1.00f));
                    if (ImGui::Combo("Compression", &compression_index, compression_options, IM_ARRAYSIZE(compression_options))) {
                        switch (compression_index) {
                            case 0: exr_transcode_compression = 7; break;  // B44A
                            case 1: exr_transcode_compression = 6; break;  // B44
                            case 2: exr_transcode_compression = 8; break;  // DWAA
                            case 3: exr_transcode_compression = 9; break;  // DWAB
                            case 4: exr_transcode_compression = 4; break;  // PIZ
                            case 5: exr_transcode_compression = 3; break;  // ZIP
                        }
                    }
                    ImGui::PopStyleColor();

                    ImGui::Unindent();
                }

                ImGui::Separator();
            }

            // Disable FPS controls for single images
            if (is_single_image) {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Frame rate: Not applicable (single image)");
                selected_frame_rate = 23.976; // Use default FPS for single images
            } else {
                ImGui::Text("Please select a frame rate:");

                // Common frame rate buttons in a simple grid layout
                if (ImGui::Button("23.976")) selected_frame_rate = 23.976;
                ImGui::SameLine();
                if (ImGui::Button("24")) selected_frame_rate = 24.0;
                ImGui::SameLine();
                if (ImGui::Button("25")) selected_frame_rate = 25.0;
                ImGui::SameLine();
                if (ImGui::Button("29.97")) selected_frame_rate = 29.97;

                if (ImGui::Button("30")) selected_frame_rate = 30.0;
                ImGui::SameLine();
                if (ImGui::Button("50")) selected_frame_rate = 50.0;
                ImGui::SameLine();
                if (ImGui::Button("59.94")) selected_frame_rate = 59.94;
                ImGui::SameLine();
                if (ImGui::Button("60")) selected_frame_rate = 60.0;

                ImGui::Separator();
                ImGui::Text("Custom frame rate:");
                ImGui::InputDouble("##fps", &selected_frame_rate, 0.1, 1.0, "%.3f");
            }

            ImGui::Separator();
            ImGui::Spacing();

            // OK and Cancel buttons (flush right)
            float btnPadding = 8.0f * 2;
            float okW = ImGui::CalcTextSize("OK").x + btnPadding;
            float cancelW = ImGui::CalcTextSize("Cancel").x + btnPadding;
            float btnSpacing = ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - okW - cancelW - btnSpacing);

            if (ImGui::Button("OK")) {
                Debug::Log("OK button pressed");
                if (!pending_sequence_path.empty() && selected_frame_rate > 0.0) {
                    Debug::Log("Processing sequence...");

                    // Pass selected EXR layer if applicable
                    std::string selected_layer = "";
                    int selected_part_index = 0;
                    if (is_exr_sequence) {
                        std::lock_guard<std::mutex> lock(exr_layers_mutex);
                        if (selected_exr_layer_index < exr_layer_names.size()) {
                            selected_layer = exr_layer_names[selected_exr_layer_index];
                        }
                        if (selected_exr_layer_index < exr_layer_part_indices.size()) {
                            selected_part_index = exr_layer_part_indices[selected_exr_layer_index];
                        }
                    }

                    // Check if transcode is requested (for both EXR and TIFF/PNG)
                    if ((is_exr_sequence || is_tiff_png_sequence) && exr_transcode_enabled) {
                        ProcessImageSequenceWithTranscode(pending_sequence_path, selected_frame_rate,
                                                         selected_layer, selected_part_index,
                                                         exr_transcode_max_width, exr_transcode_compression);
                    } else {
                        ProcessImageSequence(pending_sequence_path, selected_frame_rate, selected_layer);
                    }

                    Debug::Log("ProcessImageSequence completed");
                }
                Debug::Log("Closing frame rate dialog");
                show_frame_rate_dialog = false;
                frame_rate_dialog_opened = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                show_frame_rate_dialog = false;
                frame_rate_dialog_opened = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        } else if (frame_rate_dialog_opened) {
            // Dialog was closed by X button or Escape
            Debug::Log("Frame rate dialog closed by user");
            show_frame_rate_dialog = false;
            frame_rate_dialog_opened = false;
        }

        // Transcode Settings Dialog
        RenderTranscodeSettingsDialog();
    }

    void ProjectManager::CreatePropertiesSection() {
        if (!video_player || current_file_path->empty()) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No video loaded");
            return;
        }

        // Check if this is a single image file to prevent metadata spam
        bool is_single_image = false;
        if (!current_file_path->empty() && current_file_path->find("mf://") != 0) {
            // Not an MF:// sequence, check if it's a single image
            MediaType media_type = GetMediaType(*current_file_path);
            if (media_type == MediaType::IMAGE) {
                // This is a single image - don't spam metadata extraction
                is_single_image = true;
            }
        }

        if (is_single_image) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Single image loaded");
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Image properties not available for single files");
            return;
        }

        // Check if this is an image sequence or EXR sequence
        bool is_image_sequence = current_file_path->substr(0, 5) == "mf://";
        bool is_exr_sequence = current_file_path->substr(0, 6) == "exr://";

        if (is_image_sequence || is_exr_sequence) {
            // For EXR sequences, use the new EXR metadata structure
            if (is_exr_sequence) {
                // Try to get cached EXR metadata
                const CombinedMetadata* cached_meta = GetCachedMetadata(*current_file_path);
                if (cached_meta && cached_meta->exr_meta) {
                    DisplayEXRMetadata(cached_meta->exr_meta.get());
                } else {
                    // Create basic EXR metadata from video player if available
                    if (video_player && video_player->HasVideo()) {
                        auto exr_meta = std::make_unique<EXRMetadata>();

                        // Populate basic info from video player
                        exr_meta->width = video_player->GetVideoWidth();
                        exr_meta->height = video_player->GetVideoHeight();
                        exr_meta->frame_rate = video_player->GetFrameRate();
                        exr_meta->total_frames = video_player->GetTotalFrames();

                        // Get start_frame from MediaItem or VideoPlayer
                        auto media_item = GetMediaItemFromCurrentPath();
                        exr_meta->start_frame = media_item ? media_item->start_frame : video_player->GetEXRSequenceStartFrame();
                        exr_meta->end_frame = exr_meta->start_frame + exr_meta->total_frames - 1;

                        // Extract first file path for metadata extraction
                        std::string url = *current_file_path;
                        if (url.substr(0, 6) == "exr://") {
                            std::string path_part = url.substr(6);
                            size_t layer_pos = path_part.find("?layer=");
                            if (layer_pos != std::string::npos) {
                                exr_meta->file_path = path_part.substr(0, layer_pos);
                                exr_meta->layer_name = path_part.substr(layer_pos + 7);
                            } else {
                                exr_meta->file_path = path_part;
                            }
                        }

                        if (fs::exists(exr_meta->file_path)) {
                            exr_meta->file_name = fs::path(exr_meta->file_path).filename().string();
                            exr_meta->file_size = fs::file_size(exr_meta->file_path);
                        }

                        exr_meta->is_loaded = true;

                        // Cache the metadata for future use
                        {
                            std::lock_guard<std::mutex> lock(queue_mutex);
                            auto& cached = metadata_cache[*current_file_path];
                            cached.exr_meta = std::move(exr_meta);
                        }

                        // Display it
                        const CombinedMetadata* cached_meta_new = GetCachedMetadata(*current_file_path);
                        if (cached_meta_new && cached_meta_new->exr_meta) {
                            DisplayEXRMetadata(cached_meta_new->exr_meta.get());
                        }
                    } else {
                        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "EXR sequence properties will be available when loaded");
                    }
                }
            } else {

                if (video_player && video_player->HasVideo()) {
                    ImGui::Spacing();
                    ImGui::Text("Sequence Properties");
                    ImGui::Separator();

                    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));
                    if (ImGui::BeginTable("ImageSeqProps", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
                        ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                        // Get resolved path (strip mf:// or exr:// prefixes)
                        std::string resolved_path;
                        if (!current_file_path->empty()) {
                            resolved_path = *current_file_path;
                            if (resolved_path.substr(0, 5) == "mf://") {
                                resolved_path = resolved_path.substr(5);
                            } else if (resolved_path.substr(0, 6) == "exr://") {
                                // Format: exr://layer_name@path
                                size_t at_pos = resolved_path.find('@');
                                if (at_pos != std::string::npos) {
                                    resolved_path = resolved_path.substr(at_pos + 1);
                                }
                            }
                        }

                        // Get directory from path
                        std::string seq_directory;
                        size_t last_slash = resolved_path.find_last_of("/\\");
                        if (last_slash != std::string::npos) {
                            seq_directory = resolved_path.substr(0, last_slash);
                        }

                        // Path row
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextDisabled("Path:");
                        ImGui::TableSetColumnIndex(1);
                                ImGui::TextWrapped("%s", seq_directory.c_str());
                                RenderPathButtons(seq_directory, "SeqPath");

                        // Image type
                        std::string image_type = "Unknown";
                        if (!resolved_path.empty()) {
                            size_t dot_pos = resolved_path.find_last_of('.');
                            if (dot_pos != std::string::npos) {
                                std::string ext = resolved_path.substr(dot_pos + 1);
                                std::transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
                                image_type = ext;
                            }
                        }

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextDisabled("Image Type:");
                        ImGui::TableSetColumnIndex(1);
                                ImGui::Text("%s", image_type.c_str());
        
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextDisabled("Resolution:");
                        ImGui::TableSetColumnIndex(1);
                                ImGui::Text("%d x %d", video_player->GetVideoWidth(), video_player->GetVideoHeight());
        
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextDisabled("Frame Rate:");
                        ImGui::TableSetColumnIndex(1);
                        double fps = video_player->GetFrameRate();
                                if (fps > 0) {
                            ImGui::Text("%.3f fps", fps);
                        } else {
                            ImGui::Text("Unknown");
                        }
        
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextDisabled("Frame Range:");
                        ImGui::TableSetColumnIndex(1);
                        int total_frames = video_player->GetTotalFrames();

                        // Get start_frame from MediaItem (authoritative source) rather than VideoPlayer
                        // This ensures we show the actual file frame numbers, not defaults
                        int start_frame = 1; // Fallback default for regular videos
                        auto media_item = GetMediaItemFromCurrentPath();
                        if (media_item) {
                            start_frame = media_item->start_frame;
                        } else {
                            // Fallback to VideoPlayer if MediaItem not found
                            start_frame = video_player->GetImageSequenceStartFrame();
                        }

                                if (total_frames > 0) {
                            int end_frame = start_frame + total_frames - 1;
                            ImGui::Text("%d-%d", start_frame, end_frame);
                        } else {
                            ImGui::Text("Unknown");
                        }
        
                        ImGui::EndTable();
                    }
                    ImGui::PopStyleVar();  // CellPadding
                } else {
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Sequence properties will be available when loaded");
                }
            }
            return;
        }

        // For video/audio files, quietly queue metadata extraction in background
        const CombinedMetadata* cached_meta = GetCachedMetadata(*current_file_path);
        if (!cached_meta) {
            QueueVideoMetadataExtraction(*current_file_path, true);
            // Don't show loading messages - just return and let it load in background
            return;
        }

        // Show available metadata (no loading states or progress bars)
        if (cached_meta->video_meta) {
            DisplayVideoMetadata(cached_meta->video_meta.get());
        }

        // Queue Adobe metadata extraction if not yet done
        if (!cached_meta->adobe_meta) {
            QueueAdobeMetadata(*current_file_path);
        }

        if (cached_meta->adobe_meta) {
            ImGui::Spacing();
            DisplayAdobeMetadata(cached_meta->adobe_meta.get());

            ImGui::Spacing();
            DisplayTimecodeTable(cached_meta->adobe_meta.get());
        }
    }

    // ============================================================================
    // MEDIA ITEM INTERACTION HANDLERS
    // ============================================================================

    void ProjectManager::HandleMediaItemClick(const MediaItem& item) {
        bool ctrl_held = ImGui::GetIO().KeyCtrl;
        bool shift_held = ImGui::GetIO().KeyShift;

        // Single click - handle selection
        SelectMediaItem(item.id, ctrl_held, shift_held);

        // Double-click - load the item
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            if (item.type == MediaType::DUAL_VIEW) {
                // Open dual view in editor mode
                OpenDualViewInEditor(item.timeline_id);
            }
            else if (item.type == MediaType::PLAYLIST) {
                // Open playlist in unified timeline editor (new system)
                OpenPlaylistInTimelineEditor(item.id);
            }
            else {
                // LoadSingleMediaItem handles exit_timeline_mode_callback internally
                LoadSingleMediaItem(item);
            }
        }
    }

    void ProjectManager::HandleMediaItemRightClick(const MediaItem& item) {
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            std::string popup_id = "context_" + item.id;
            ImGui::OpenPopup(popup_id.c_str());

            if (!IsItemSelected(item.id)) {
                ClearSelection();
                SelectMediaItem(item.id, false, false);
            }
        }

        // Context menu popup
        std::string popup_id = "context_" + item.id;
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.065f, 0.065f, 0.065f, 1.0f));
        if (ImGui::BeginPopup(popup_id.c_str())) {
            ShowMediaItemContextMenu(item);
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
    }

    void ProjectManager::HandleMediaItemDragDrop(const MediaItem& item, bool is_selected) {
        if (ImGui::BeginDragDropSource()) {
            if (is_selected && selected_media_items.size() > 1) {
                std::string payload_data;
                for (const auto& selected_id : selected_media_items) {
                    if (!payload_data.empty()) payload_data += ";";
                    payload_data += selected_id;
                }
                ImGui::SetDragDropPayload("MEDIA_ITEMS_MULTI", payload_data.c_str(), payload_data.size() + 1);
                ImGui::Text("Dragging %zu items", selected_media_items.size());
            }
            else {
                ImGui::SetDragDropPayload("MEDIA_ITEM", item.id.c_str(), item.id.size() + 1);
                ImGui::Text("Dragging: %s", item.name.c_str());
            }
            ImGui::EndDragDropSource();
        }
    }

    void ProjectManager::ShowMediaItemContextMenu(const MediaItem& item) {
        int selection_count = static_cast<int>(selected_media_items.size());

        // Only show header for multi-selection
        if (selection_count > 1) {
            ImGui::Text("%d items selected", selection_count);
            ImGui::Separator();
        }

        // Transcode option - available for image sequences and videos
        bool can_transcode = false;
        if (selection_count > 0) {
            // Check if any selected items are transcodable (sequences or videos)
            for (const auto& selected_id : selected_media_items) {
                auto selected_item = GetMediaItem(selected_id);
                if (selected_item && (selected_item->type == MediaType::IMAGE_SEQUENCE ||
                                      selected_item->type == MediaType::EXR_SEQUENCE ||
                                      selected_item->type == MediaType::VIDEO)) {
                    can_transcode = true;
                    break;
                }
            }
        }

        if (ImGui::MenuItem("Add to Transcode Queue", "Ctrl+Shift+Q", false, can_transcode)) {
            AddSelectedItemsToTranscodeQueue();
            ImGui::CloseCurrentPopup();
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Delete", "Del")) {
            DeleteSelectedItems();
            ImGui::CloseCurrentPopup();
        }

        if (selection_count == 1 && (item.type == MediaType::SEQUENCE || item.type == MediaType::DUAL_VIEW || item.type == MediaType::PLAYLIST) && ImGui::MenuItem("Rename", "F2")) {
            StartRenaming(item.id);
            ImGui::CloseCurrentPopup();
        }

        if (selection_count == 1 && item.type != MediaType::SEQUENCE && item.type != MediaType::DUAL_VIEW && item.type != MediaType::PLAYLIST && ImGui::MenuItem("Show in Explorer")) {
            ShowInExplorer(item.path);
            ImGui::CloseCurrentPopup();
        }

        if (selection_count > 1) {
            ImGui::Separator();
            // Check if any selected items are valid for playlists (VIDEO, AUDIO, IMAGE_SEQUENCE, EXR_SEQUENCE)
            bool has_playlist_items = false;
            for (const auto& sel_id : selected_media_items) {
                MediaItem* sel_item = GetMediaItem(sel_id);
                if (sel_item && sel_item->type != MediaType::PLAYLIST && sel_item->type != MediaType::IMAGE &&
                    sel_item->type != MediaType::DUAL_VIEW) {
                    has_playlist_items = true;
                    break;
                }
            }
            if (has_playlist_items && ImGui::MenuItem("Create Playlist from Selection")) {
                std::string playlist_id = CreatePlaylistFromSelection();
                if (!playlist_id.empty()) {
                    OpenPlaylistInTimelineEditor(playlist_id);
                }
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Clear Selection")) {
                ClearSelection();
                ImGui::CloseCurrentPopup();
            }
        }
    }

    // ============================================================================
    // MEDIA MANAGEMENT
    // ============================================================================

    // LoadMediaFiles() removed - use drag & drop instead for adding multiple files

    void ProjectManager::AddMediaFileToProject(const std::string& file_path) {
        // Check if file is already in project (prevents duplicates on pipeline mode changes)
        for (const auto& item : media_pool) {
            if (item.path == file_path) {
                Debug::Log("File already in project, skipping duplicate: " + file_path);
                return;
            }
        }

        MediaItem item;
        item.id = GenerateUniqueID();
        item.name = GetFileName(file_path);
        item.path = file_path;
        item.type = GetMediaType(file_path);

        // EXTRACT AND CACHE FULL METADATA (not just duration)
        if (item.type == MediaType::VIDEO || item.type == MediaType::AUDIO) {
            auto metadata = ump::FFmpegMetadataExtractor::Extract(file_path);

            if (metadata.is_loaded) {
                // Cache metadata immediately
                {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    auto& cached_meta = metadata_cache[file_path];
                    cached_meta.video_meta = std::make_unique<VideoMetadata>(metadata);
                    cached_meta.state = MetadataState::VIDEO_READY;
                }

                // Set duration from container metadata (works for audio-only files too)
                if (metadata.duration > 0) {
                    item.duration = metadata.duration;
                } else if (metadata.total_frames > 0 && metadata.frame_rate > 0) {
                    // Fallback: calculate from video frames if container duration unavailable
                    item.duration = metadata.total_frames / metadata.frame_rate;
                } else {
                    item.duration = (item.type == MediaType::VIDEO) ? 30.0 : 180.0;
                }

                // Store frame rate if available (for video files)
                if (metadata.frame_rate > 0) {
                    item.frame_rate = metadata.frame_rate;
                }

                // Store video dimensions for timeline creation
                if (metadata.width > 0 && metadata.height > 0) {
                    item.timeline_width = metadata.width;
                    item.timeline_height = metadata.height;
                }

                // Store stream start time for H.264/H.265 PTS sync
                item.stream_start_time = metadata.stream_start_time;

                // Check if media has audio track
                item.has_audio = (metadata.audio_channels > 0);

                Debug::Log("AddMediaFileToProject: Cached metadata for: " + file_path);
                Debug::Log("  Resolution: " + std::to_string(metadata.width) + "x" + std::to_string(metadata.height));
                Debug::Log("  Frame Rate: " + std::to_string(metadata.frame_rate) + " fps");
                Debug::Log("  Duration: " + std::to_string(item.duration) + "s");
                Debug::Log("  Codec: " + metadata.video_codec);
                Debug::Log("  Pixel Format: " + metadata.pixel_format);
                Debug::Log("  Has Audio: " + std::to_string(item.has_audio) +
                           " (audio_channels=" + std::to_string(metadata.audio_channels) + ")");
            } else {
                // Fallback: If full extraction fails, try duration probe
                Debug::Log("AddMediaFileToProject: Full extraction failed, falling back to duration probe");
                double probed_duration = ump::FFmpegMetadataExtractor::ProbeDuration(file_path);
                item.duration = (probed_duration > 0) ? probed_duration : ((item.type == MediaType::VIDEO) ? 30.0 : 180.0);
            }
        }
        else {
            item.duration = 1.0;
        }

        media_pool.push_back(item);

        int bin_index = GetBinIndexForMediaType(item.type);
        if (bins.size() > bin_index) {
            bins[bin_index].items.push_back(item);
        }
    }

    void ProjectManager::AddCurrentVideoToProject() {
        if (!current_file_path || current_file_path->empty()) return;

        // Check if already in project
        for (const auto& item : media_pool) {
            if (item.path == *current_file_path) {
                return;
            }
        }

        MediaItem item;
        item.id = GenerateUniqueID();
        item.name = GetFileName(*current_file_path);
        item.path = *current_file_path;
        item.type = GetMediaType(*current_file_path);

        Debug::Log("AddCurrentVideoToProject: Adding media to project");
        Debug::Log("  - ID: " + item.id);
        Debug::Log("  - Name: " + item.name);
        Debug::Log("  - Path: " + item.path);
        Debug::Log("  - Type: " + std::to_string(static_cast<int>(item.type)));

        double detected_duration = 0.0;
        if (video_player && video_player->HasVideo()) {
            detected_duration = video_player->GetDuration();

            // Get video dimensions and frame rate from loaded video
            int vid_width = video_player->GetVideoWidth();
            int vid_height = video_player->GetVideoHeight();
            double vid_fps = video_player->GetFrameRate();

            if (vid_width > 0 && vid_height > 0) {
                item.timeline_width = vid_width;
                item.timeline_height = vid_height;
            }
            if (vid_fps > 0) {
                item.frame_rate = vid_fps;
            }
        }

        if (detected_duration > 0) {
            item.duration = detected_duration;
        }
        else {
            item.duration = GetDefaultDurationForType(item.type);
        }

        media_pool.push_back(item);

        int bin_index = GetBinIndexForMediaType(item.type);
        if (bins.size() > bin_index) {
            bins[bin_index].items.push_back(item);
        }
    }

    void ProjectManager::LoadSingleMediaItem(const MediaItem& item) {
        // Determine loading message based on media type
        std::string loading_msg;
        switch (item.type) {
            case MediaType::IMAGE_SEQUENCE:
            case MediaType::EXR_SEQUENCE:
                loading_msg = "Loading Image Sequence...";
                break;
            case MediaType::AUDIO:
                loading_msg = "Loading Audio...";
                break;
            default:
                loading_msg = "Loading Video...";
                break;
        }

        // Show loading modal and defer the actual loading
        // Skip modal in playlist mode for smoother transitions
        if (loading_modal_callback && !skip_loading_modal_) {
            MediaItem item_copy = item;  // Copy for lambda capture
            loading_modal_callback(loading_msg, [this, item_copy]() {
                LoadSingleMediaItemInternal(item_copy);
            });
        } else {
            // Direct load (no modal) - used for playlist transitions
            skip_loading_modal_ = false;  // Reset flag
            LoadSingleMediaItemInternal(item);
        }
    }

    void ProjectManager::LoadSingleMediaItemInternal(const MediaItem& item) {
        Debug::Log("=== LoadSingleMediaItem CALLED ===");
        Debug::Log("MediaItem details:");
        Debug::Log("  - ID: " + item.id);
        Debug::Log("  - Name: " + item.name);
        Debug::Log("  - Path: " + item.path);
        Debug::Log("  - Type: " + std::to_string(static_cast<int>(item.type)));
        Debug::Log("  - Duration: " + std::to_string(item.duration));
        Debug::Log("  - FFmpeg pattern: " + (item.ffmpeg_pattern.empty() ? "(empty)" : item.ffmpeg_pattern));
        Debug::Log("  - Sequence pattern: " + (item.sequence_pattern.empty() ? "(empty)" : item.sequence_pattern));
        Debug::Log("  - EXR layer: " + (item.exr_layer.empty() ? "(empty)" : item.exr_layer));

        // Exit timeline mode when loading non-timeline media (same as double-click behavior)
        if (exit_timeline_mode_callback) {
            exit_timeline_mode_callback();
        }

        // Clean up EXR/image sequence state if active (video loading will also do this,
        // but explicit cleanup ensures clean transitions even in edge cases)
        // IMPORTANT: Must use ClearEXRCache() not ResetState() to properly shutdown
        // the DirectEXRCache and its background I/O threads
        if (video_player && video_player->IsInEXRMode()) {
            video_player->ClearEXRCache();
            Debug::Log("LoadSingleMediaItem: Cleaned up EXR/image sequence cache");
        }

        // Cache view state of current media BEFORE loading new media
        // This must happen before current_file_path is updated
        if (pre_video_change_callback && current_file_path && !current_file_path->empty()) {
            Debug::Log("LoadSingleMediaItem: Pre-change callback for: " + *current_file_path);
            pre_video_change_callback(*current_file_path);
        }

        ClearSelection();
        // Clear all active states - loading a single file makes nothing else active
        current_timeline_id.clear();

        // Set frame rate for image sequences before loading
        if ((item.type == MediaType::IMAGE_SEQUENCE || item.type == MediaType::EXR_SEQUENCE) && item.frame_rate > 0.0) {
            // Set MPV frame rate for playback
            video_player->SetMFFrameRate(item.frame_rate);
        }

        // === Image Sequence Handling (OTIO Timeline Mode) ===
        if (item.type == MediaType::IMAGE_SEQUENCE) {
            Debug::Log("LoadSingleMediaItem: Loading image sequence via OTIO timeline: " + item.path);

            // Use OTIO timeline callback to load image sequence into timeline view
            if (image_sequence_timeline_callback) {
                // Get mutable pointer to the item in media_pool
                MediaItem* media_item = GetMediaItem(item.id);
                if (media_item) {
                    *current_file_path = item.path;

                    // Notify callbacks
                    if (video_change_callback) {
                        video_change_callback(item.path);
                    }

                    // Select this item
                    SelectMediaItem(item.id, false, false);

                    // Load into OTIO timeline view
                    image_sequence_timeline_callback(media_item);

                    Debug::Log("LoadSingleMediaItem: Image sequence loaded via OTIO timeline - EARLY RETURN");
                    return;  // Early return - handled by timeline view
                } else {
                    Debug::Log("LoadSingleMediaItem: ERROR - Could not find MediaItem in pool");
                }
            } else {
                Debug::Log("LoadSingleMediaItem: ERROR - No image_sequence_timeline_callback set");
            }
        }

        // === EXR Sequence Handling (OTIO Timeline Mode) ===
        if (item.type == MediaType::EXR_SEQUENCE) {
            Debug::Log("LoadSingleMediaItem: Loading EXR sequence via OTIO timeline: " + item.path);

            // Use OTIO timeline callback to load EXR sequence into timeline view
            if (image_sequence_timeline_callback) {
                // Get mutable pointer to the item in media_pool
                MediaItem* media_item = GetMediaItem(item.id);
                if (media_item) {
                    *current_file_path = item.path;

                    // Notify callbacks
                    if (video_change_callback) {
                        video_change_callback(item.path);
                    }

                    // Select this item
                    SelectMediaItem(item.id, false, false);

                    // Load into OTIO timeline view
                    image_sequence_timeline_callback(media_item);

                    Debug::Log("LoadSingleMediaItem: EXR sequence loaded via OTIO timeline - EARLY RETURN");
                    return;  // Early return - handled by timeline view
                } else {
                    Debug::Log("LoadSingleMediaItem: ERROR - Could not find MediaItem in pool");
                }
            } else {
                Debug::Log("LoadSingleMediaItem: ERROR - No image_sequence_timeline_callback set");
            }
        }

        // === Video File Handling (OTIO Timeline Mode) ===
        if (item.type == MediaType::VIDEO) {
            Debug::Log("LoadSingleMediaItem: Loading video file via OTIO timeline: " + item.path);

            // Use OTIO timeline callback to load video file into timeline view
            if (video_file_timeline_callback) {
                // Get mutable pointer to the item in media_pool
                MediaItem* media_item = GetMediaItem(item.id);
                if (media_item) {
                    *current_file_path = item.path;

                    // Notify callbacks
                    if (video_change_callback) {
                        video_change_callback(item.path);
                    }

                    // Select item in bin (visual feedback)
                    SelectMediaItem(item.id, false, false);

                    // Load into OTIO timeline view
                    video_file_timeline_callback(media_item);

                    Debug::Log("LoadSingleMediaItem: Video file loaded via OTIO timeline - EARLY RETURN");
                    return;  // Early return - handled by timeline view
                } else {
                    Debug::Log("LoadSingleMediaItem: ERROR - Could not find MediaItem in pool");
                }
            } else {
                Debug::Log("LoadSingleMediaItem: ERROR - No video_file_timeline_callback set");
            }
        }

        // === Audio File Handling (OTIO Timeline Mode) ===
        if (item.type == MediaType::AUDIO) {
            Debug::Log("LoadSingleMediaItem: Loading audio file via OTIO timeline: " + item.path);

            // Use OTIO timeline callback to load audio file into timeline view
            if (audio_file_timeline_callback) {
                // Get mutable pointer to the item in media_pool
                MediaItem* media_item = GetMediaItem(item.id);
                if (media_item) {
                    *current_file_path = item.path;

                    // Notify callbacks
                    if (video_change_callback) {
                        video_change_callback(item.path);
                    }

                    // Select item in bin (visual feedback)
                    SelectMediaItem(item.id, false, false);

                    // Load into OTIO timeline view (audio-only)
                    audio_file_timeline_callback(media_item);

                    Debug::Log("LoadSingleMediaItem: Audio file loaded via OTIO timeline - EARLY RETURN");
                    return;  // Early return - handled by timeline view
                } else {
                    Debug::Log("LoadSingleMediaItem: ERROR - Could not find MediaItem in pool");
                }
            } else {
                Debug::Log("LoadSingleMediaItem: ERROR - No audio_file_timeline_callback set");
            }
        }

        // === FALLBACK: Legacy loading ===
        // This handles audio files only now (videos are handled by OTIO timeline)
        Debug::Log("LoadSingleMediaItem: Loading as regular file: " + item.path);

        // Check if this is a GIF (disable cache for GIFs - they're fast enough without it)
        std::string extension = std::filesystem::path(item.path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        bool is_gif = (extension == ".gif");

        // Check cache first (metadata should already be cached from AddMediaFileToProject)
        const CombinedMetadata* cached_meta = GetCachedMetadata(item.path);
        bool metadata_cached = (cached_meta && cached_meta->video_meta && cached_meta->video_meta->is_loaded);

        if (!metadata_cached) {
            // Safety fallback: Extract if cache miss (shouldn't happen for normal flow)
            Debug::Log("LoadSingleMediaItem: Cache miss, extracting metadata...");
            auto metadata = ump::FFmpegMetadataExtractor::Extract(item.path);

            if (metadata.is_loaded) {
                // Cache it
                {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    auto& cached = metadata_cache[item.path];
                    cached.video_meta = std::make_unique<VideoMetadata>(metadata);
                    cached.state = MetadataState::VIDEO_READY;
                }

                // Check codec
                std::string codec = metadata.video_codec;
                std::transform(codec.begin(), codec.end(), codec.begin(), ::tolower);
                bool is_h264_h265 = (codec.find("h264") != std::string::npos ||
                                     codec.find("hevc") != std::string::npos ||
                                     codec.find("h265") != std::string::npos ||
                                     codec.find("avc") != std::string::npos);

                if (is_h264_h265 || is_gif) {
                    Debug::Log("LoadSingleMediaItem: H.264/H.265/GIF detected - disabling cache");
                    SetCacheEnabled(false);
                    ClearAllCaches();
                    cache_auto_disabled_for_codec = true;
                    current_video_codec = metadata.video_codec;
                }
            }
        } else {
            Debug::Log("LoadSingleMediaItem: Using cached metadata (no re-extraction)");

            // Check codec from cache
            std::string codec = cached_meta->video_meta->video_codec;
            std::transform(codec.begin(), codec.end(), codec.begin(), ::tolower);
            bool is_h264_h265 = (codec.find("h264") != std::string::npos ||
                                 codec.find("hevc") != std::string::npos ||
                                 codec.find("h265") != std::string::npos ||
                                 codec.find("avc") != std::string::npos);

            if (is_h264_h265 || is_gif) {
                Debug::Log("LoadSingleMediaItem: H.264/H.265/GIF detected - disabling cache");
                SetCacheEnabled(false);
                ClearAllCaches();
                cache_auto_disabled_for_codec = true;
                current_video_codec = cached_meta->video_meta->video_codec;
            }
        }

        // Load into MPV for playback
        video_player->LoadFile(item.path);
        *current_file_path = item.path;

        // Use OnVideoLoaded for proper sequencing (600ms delay before cache starts)
        if (item.type != MediaType::AUDIO) {
            OnVideoLoaded(item.path);
        } else {
            Debug::Log("LoadSingleMediaItem: Skipping OnVideoLoaded for audio file");
            // For audio, just notify main (no cache needed)
            if (video_change_callback) {
                video_change_callback(item.path);
            }
        }

        // Restore view state from MediaItem (for regular videos and audio)
        // Look up fresh from media_pool to get the latest cached values
        if (view_state_callback) {
            MediaItem* fresh_item = GetMediaItem(item.id);
            if (fresh_item) {
                Debug::Log("Restoring view state from fresh MediaItem lookup");
                view_state_callback(fresh_item->view_state.zoom_level,
                                   fresh_item->view_state.scroll_offset,
                                   fresh_item->view_state.playhead_position);
            } else {
                // Fallback to parameter (shouldn't happen)
                view_state_callback(item.view_state.zoom_level,
                                   item.view_state.scroll_offset,
                                   item.view_state.playhead_position);
            }
        }

        // Select this item in the project panel
        SelectMediaItem(item.id, false, false);

        // Metadata already cached - only queue Adobe metadata if needed
        if (video_player && video_player->HasVideo()) {
            QueueAdobeMetadata(item.path);  // Optional: timecode and project links
        }
    }

    MediaItem* ProjectManager::GetMediaItem(const std::string& media_id) {
        auto it = std::find_if(media_pool.begin(), media_pool.end(),
            [&media_id](const MediaItem& item) { return item.id == media_id; });
        return (it != media_pool.end()) ? &(*it) : nullptr;
    }

    MediaItem* ProjectManager::FindMediaItemByPath(const std::string& file_path) {
        if (file_path.empty()) return nullptr;

        // Normalize the search path (strip URL schemes for comparison)
        std::string search_path = file_path;

        // Handle mf:// URLs (image sequences)
        if (search_path.length() > 5 && search_path.substr(0, 5) == "mf://") {
            search_path = search_path.substr(5);
            size_t fps_pos = search_path.find(":fps=");
            if (fps_pos != std::string::npos) {
                search_path = search_path.substr(0, fps_pos);
            }
        }
        // Handle exr:// URLs
        else if (search_path.length() > 6 && search_path.substr(0, 6) == "exr://") {
            search_path = search_path.substr(6);
            size_t query_pos = search_path.find('?');
            if (query_pos != std::string::npos) {
                search_path = search_path.substr(0, query_pos);
            }
        }

        // Search through media pool
        for (auto& item : media_pool) {
            std::string item_path = item.path;

            // Normalize item path the same way
            if (item_path.length() > 5 && item_path.substr(0, 5) == "mf://") {
                item_path = item_path.substr(5);
                size_t fps_pos = item_path.find(":fps=");
                if (fps_pos != std::string::npos) {
                    item_path = item_path.substr(0, fps_pos);
                }
            } else if (item_path.length() > 6 && item_path.substr(0, 6) == "exr://") {
                item_path = item_path.substr(6);
                size_t query_pos = item_path.find('?');
                if (query_pos != std::string::npos) {
                    item_path = item_path.substr(0, query_pos);
                }
            }

            if (item_path == search_path) {
                return &item;
            }
        }

        return nullptr;
    }

    MediaItem* ProjectManager::GetMediaItemFromCurrentPath() {
        if (!current_file_path || current_file_path->empty()) {
            return nullptr;
        }

        std::string path = *current_file_path;

        // For EXR sequences with layer parameter, strip the layer parameter for matching
        // Format: exr://path/to/file.exr?layer=beauty
        if (path.substr(0, 6) == "exr://") {
            // Find matching media item by comparing base path (without layer parameter)
            std::string base_path = path;
            size_t query_pos = path.find("?layer=");
            if (query_pos != std::string::npos) {
                base_path = path.substr(0, query_pos);
            }

            // Search for EXR sequence with matching base path
            for (auto& item : media_pool) {
                if (item.type == MediaType::EXR_SEQUENCE) {
                    std::string item_base_path = item.path;
                    size_t item_query_pos = item.path.find("?layer=");
                    if (item_query_pos != std::string::npos) {
                        item_base_path = item.path.substr(0, item_query_pos);
                    }
                    if (item_base_path == base_path) {
                        return &item;
                    }
                }
            }
        }
        // For regular paths (mf://, videos, etc), do exact match
        else {
            auto it = std::find_if(media_pool.begin(), media_pool.end(),
                [&path](const MediaItem& item) { return item.path == path; });
            if (it != media_pool.end()) {
                return &(*it);
            }
        }

        return nullptr;
    }

    // ============================================================================
    // SELECTION MANAGEMENT
    // ============================================================================

    void ProjectManager::SelectMediaItem(const std::string& item_id, bool ctrl_held, bool shift_held) {
        if (!ctrl_held && !shift_held) {
            selected_media_items.clear();
            selected_media_items.insert(item_id);
            last_selected_item = item_id;
        }
        else if (ctrl_held) {
            if (selected_media_items.count(item_id)) {
                selected_media_items.erase(item_id);
            }
            else {
                selected_media_items.insert(item_id);
                last_selected_item = item_id;
            }
        }
        else if (shift_held && !last_selected_item.empty()) {
            SelectItemRange(last_selected_item, item_id);
        }
    }

    void ProjectManager::SelectItemRange(const std::string& start_id, const std::string& end_id) {
        std::vector<MediaItem*> visible_items;
        for (auto& bin : bins) {
            if (bin.is_open) {
                for (auto& item : bin.items) {
                    visible_items.push_back(&item);
                }
            }
        }

        int start_idx = -1, end_idx = -1;
        for (int i = 0; i < visible_items.size(); i++) {
            if (visible_items[i]->id == start_id) start_idx = i;
            if (visible_items[i]->id == end_id) end_idx = i;
        }

        if (start_idx >= 0 && end_idx >= 0) {
            int min_idx = (std::min)(start_idx, end_idx);
            int max_idx = (std::max)(start_idx, end_idx);
            for (int i = min_idx; i <= max_idx; i++) {
                selected_media_items.insert(visible_items[i]->id);
            }
        }
    }

    void ProjectManager::ClearSelection() {
        selected_media_items.clear();
        last_selected_item.clear();
    }

    bool ProjectManager::IsItemSelected(const std::string& item_id) const {
        return selected_media_items.count(item_id) > 0;
    }

    std::vector<MediaItem> ProjectManager::GetSelectedItems() const {
        std::vector<MediaItem> result;
        for (const auto& id : selected_media_items) {
            auto it = std::find_if(media_pool.begin(), media_pool.end(),
                [&id](const MediaItem& item) { return item.id == id; });
            if (it != media_pool.end()) {
                result.push_back(*it);
            }
        }
        return result;
    }

    // ============================================================================
    // ITEM OPERATIONS (DELETE, RENAME, ETC.)
    // ============================================================================

    void ProjectManager::DeleteSelectedItems() {
        std::vector<std::string> items_to_delete(selected_media_items.begin(), selected_media_items.end());
        std::vector<std::string> sequence_ids_to_delete;
        std::vector<std::string> video_paths_to_uncache;

        // Collect sequence IDs and video file paths before deletion
        for (const std::string& item_id : items_to_delete) {
            auto media_item = std::find_if(media_pool.begin(), media_pool.end(),
                [&item_id](const MediaItem& item) { return item.id == item_id; });
            if (media_item != media_pool.end()) {
                if (media_item->type == MediaType::SEQUENCE) {
                    sequence_ids_to_delete.push_back(media_item->sequence_id);
                } else if (media_item->type == MediaType::VIDEO) {
                    // Collect video file paths to remove from cache
                    video_paths_to_uncache.push_back(media_item->path);
                }
            }
        }

        // Delete items from media_pool and bins
        for (const std::string& item_id : items_to_delete) {
            media_pool.erase(
                std::remove_if(media_pool.begin(), media_pool.end(),
                    [&item_id](const MediaItem& item) { return item.id == item_id; }),
                media_pool.end()
            );

            for (auto& bin : bins) {
                bin.items.erase(
                    std::remove_if(bin.items.begin(), bin.items.end(),
                        [&item_id](const MediaItem& item) { return item.id == item_id; }),
                    bin.items.end()
                );
            }
        }

        // Clean up video caches for deleted video files
        for (const std::string& video_path : video_paths_to_uncache) {
            RemoveVideoFromCache(video_path);
        }

        ClearSelection();
    }

    void ProjectManager::StartRenaming(const std::string& item_id) {
        renaming_item_id = item_id;
        auto item_it = std::find_if(media_pool.begin(), media_pool.end(),
            [&item_id](const MediaItem& item) { return item.id == item_id; });

        if (item_it != media_pool.end()) {
            strncpy_s(rename_buffer, item_it->name.c_str(), sizeof(rename_buffer) - 1);
            show_rename_dialog = true;
        }
    }

    void ProjectManager::ProcessRenameItem() {
        auto item_it = std::find_if(media_pool.begin(), media_pool.end(),
            [this](const MediaItem& item) { return item.id == renaming_item_id; });

        if (item_it != media_pool.end()) {
            item_it->name = rename_buffer;
            // Also update in bins
            for (auto& bin : bins) {
                auto bin_item = std::find_if(bin.items.begin(), bin.items.end(),
                    [this](const MediaItem& item) { return item.id == renaming_item_id; });
                if (bin_item != bin.items.end()) {
                    bin_item->name = rename_buffer;
                }
            }
        }
    }

    void ProjectManager::StartRenamingSelected() {
        if (selected_media_items.size() == 1) {
            StartRenaming(*selected_media_items.begin());
        }
    }

    void ProjectManager::ShowItemProperties(const std::string& item_id) {
        auto item_it = std::find_if(media_pool.begin(), media_pool.end(),
            [&item_id](const MediaItem& item) { return item.id == item_id; });

        if (item_it != media_pool.end()) {
            Debug::Log("Item details:");
            Debug::Log("  Name: " + item_it->name);
            Debug::Log("  Type: " + std::to_string(static_cast<int>(item_it->type)));
            Debug::Log("  Path: " + item_it->path);
            Debug::Log("  Duration: " + std::to_string(item_it->duration));

            // To revisit
        }
    }

    void ProjectManager::ShowInExplorer(const std::string& file_path) {
        if (file_path.empty()) return;

        std::string resolved_path = file_path;

        // Handle mf:// URLs (image sequences)
        if (file_path.substr(0, 5) == "mf://") {
            // Extract directory from mf:// URL
            // Format: mf://directory/basename*extension or mf://path/sequence_%04d.exr:fps=24
            std::string path_part = file_path.substr(5); // Remove "mf://"

            // Remove fps parameter if present
            size_t fps_pos = path_part.find(":fps=");
            if (fps_pos != std::string::npos) {
                path_part = path_part.substr(0, fps_pos);
            }

            // Extract directory (everything up to the last /)
            size_t last_slash = path_part.find_last_of("/\\");
            if (last_slash != std::string::npos) {
                resolved_path = path_part.substr(0, last_slash);
                Debug::Log("ShowInExplorer: Resolved mf:// URL to directory: " + resolved_path);
            } else {
                resolved_path = path_part; // Fallback: use whole path
            }
        }
        // Handle exr:// URLs (EXR sequences with layers)
        else if (file_path.substr(0, 6) == "exr://") {
            // Format: exr://path/to/sequence.exr?layer=beauty
            std::string path_part = file_path.substr(6); // Remove "exr://"

            // Remove layer parameter if present
            size_t query_pos = path_part.find('?');
            if (query_pos != std::string::npos) {
                path_part = path_part.substr(0, query_pos);
            }

            // Extract directory for sequence, or use file directly
            size_t last_slash = path_part.find_last_of("/\\");
            if (last_slash != std::string::npos) {
                resolved_path = path_part.substr(0, last_slash);
                Debug::Log("ShowInExplorer: Resolved exr:// URL to directory: " + resolved_path);
            } else {
                resolved_path = path_part;
            }
        }

#ifdef _WIN32
        // Launch explorer in a separate thread to avoid blocking the UI
        std::thread([resolved_path]() {
            std::string windows_path = resolved_path;
            std::replace(windows_path.begin(), windows_path.end(), '/', '\\');

            // Use /select for files, plain path for directories
            std::filesystem::path fs_path(windows_path);
            std::string command;

            if (std::filesystem::is_directory(fs_path)) {
                command = "explorer \"" + windows_path + "\"";
            } else {
                command = "explorer /select,\"" + windows_path + "\"";
            }

            Debug::Log("ShowInExplorer: Executing command: " + command);
            system(command.c_str());
        }).detach();
#endif
    }

    MediaItem* ProjectManager::GetCurrentPlayingMediaItem() {
        // First check file-based media (videos, image sequences)
        MediaItem* item = GetMediaItemFromCurrentPath();
        if (item) {
            return item;
        }

        // Check for active dual view
        item = GetActiveDualViewItem();
        if (item) {
            return item;
        }

        // Check for active playlist
        for (auto& pool_item : media_pool) {
            if (pool_item.type == MediaType::PLAYLIST && pool_item.is_active) {
                return &pool_item;
            }
        }

        return nullptr;
    }

    void ProjectManager::ReloadWithPipelineMode(PipelineMode mode) {
        // Set the new project-level pipeline mode
        project_pipeline_mode_ = mode;
        Debug::Log("ProjectManager::ReloadWithPipelineMode: Set project pipeline mode to " +
                   std::string(PipelineModeToString(mode)));

        // Get the current playing media item
        MediaItem* current_item = GetCurrentPlayingMediaItem();
        if (!current_item) {
            Debug::Log("ProjectManager::ReloadWithPipelineMode: No current media item to reload");
            return;
        }

        // Reload based on media type
        if (current_item->type == MediaType::VIDEO && video_file_timeline_callback) {
            Debug::Log("ProjectManager::ReloadWithPipelineMode: Reloading video: " + current_item->name);
            video_file_timeline_callback(current_item);
        } else if ((current_item->type == MediaType::IMAGE_SEQUENCE ||
                    current_item->type == MediaType::EXR_SEQUENCE) &&
                   image_sequence_timeline_callback) {
            Debug::Log("ProjectManager::ReloadWithPipelineMode: Reloading image sequence: " + current_item->name);
            image_sequence_timeline_callback(current_item);
        } else {
            Debug::Log("ProjectManager::ReloadWithPipelineMode: Unsupported media type or no callback for: " + current_item->name);
        }
    }

    // ========================================================================
    // DUAL VIEW MANAGEMENT
    // ========================================================================

    std::string ProjectManager::CreateDualView(const std::string& name,
                                               int width, int height,
                                               double fps) {
        // Generate unique name
        std::string dual_view_name = name;
        if (dual_view_name.empty()) {
            int dual_view_count = GetDualViewCount() + 1;
            dual_view_name = "Dual View " + std::to_string(dual_view_count);
        }

        // Create MediaItem for the dual view
        MediaItem item;
        item.id = GenerateUniqueID();
        item.name = dual_view_name;
        item.path = "";  // No source file - this is a scratch dual view
        item.type = MediaType::DUAL_VIEW;
        item.duration = 0.0;  // Duration determined by loaded clips
        item.frame_rate = fps;
        item.timeline_id = item.id;  // Use same ID for dual view
        item.timeline_format = "dual_view";
        item.video_track_count = 2;  // LEFT and RIGHT tracks
        item.audio_track_count = 0;  // No audio tracks initially
        item.timeline_width = width;
        item.timeline_height = height;

        // Add to media pool and bin
        media_pool.push_back(item);
        int bin_index = GetBinIndexForMediaType(item.type);
        if (bins.size() > static_cast<size_t>(bin_index)) {
            bins[bin_index].items.push_back(item);
        }

        Debug::Log("CreateDualView: Created dual view '" + dual_view_name + "' with ID " + item.id);

        return item.id;
    }

    int ProjectManager::GetDualViewCount() const {
        int count = 0;
        for (const auto& item : media_pool) {
            if (item.type == MediaType::DUAL_VIEW) count++;
        }
        return count;
    }

    MediaItem* ProjectManager::GetDualViewItem(const std::string& dual_view_id) {
        for (auto& item : media_pool) {
            if (item.type == MediaType::DUAL_VIEW && item.timeline_id == dual_view_id) {
                return &item;
            }
        }
        return nullptr;
    }

    MediaItem* ProjectManager::GetActiveDualViewItem() {
        // Return the dual view item that has is_active=true
        for (auto& item : media_pool) {
            if (item.type == MediaType::DUAL_VIEW && item.is_active) {
                return &item;
            }
        }
        return nullptr;
    }

    void ProjectManager::OpenDualViewInEditor(const std::string& dual_view_id) {
        // Find the dual view item
        MediaItem* item = GetDualViewItem(dual_view_id);
        if (!item) {
            Debug::Log("OpenDualViewInEditor: Dual view not found: " + dual_view_id);
            return;
        }

        // Set is_active on the dual view, clear on others
        // Must update both media_pool AND bin.items (UI reads from bins)
        for (auto& pool_item : media_pool) {
            pool_item.is_active = (pool_item.type == MediaType::DUAL_VIEW &&
                                   pool_item.timeline_id == dual_view_id);
        }
        for (auto& bin : bins) {
            for (auto& bin_item : bin.items) {
                bin_item.is_active = (bin_item.type == MediaType::DUAL_VIEW &&
                                      bin_item.timeline_id == dual_view_id);
            }
        }
        current_timeline_id.clear();  // Not a regular timeline

        // Call the dual view editor callback
        if (dual_view_editor_callback) {
            dual_view_editor_callback(dual_view_id);
        } else {
            Debug::Log("OpenDualViewInEditor: No dual_view_editor_callback set");
        }
    }

    void ProjectManager::ClearDualViewActiveStates() {
        // Clear is_active flag from all dual views
        // Called when loading regular media (non-dual-view)
        // Must update both media_pool AND bin.items (UI reads from bins)
        for (auto& item : media_pool) {
            if (item.type == MediaType::DUAL_VIEW) {
                item.is_active = false;
            }
        }
        for (auto& bin : bins) {
            for (auto& item : bin.items) {
                if (item.type == MediaType::DUAL_VIEW) {
                    item.is_active = false;
                }
            }
        }
    }

    // ========================================================================
    // PLAYLIST MANAGEMENT
    // ========================================================================

    std::string ProjectManager::CreateNewPlaylist(const std::string& name) {
        // Generate unique name
        std::string playlist_name = name;
        if (playlist_name.empty()) {
            int playlist_count = GetPlaylistCount() + 1;
            playlist_name = "Playlist " + std::to_string(playlist_count);
        }

        // Create MediaItem for the playlist
        MediaItem item;
        item.id = GenerateUniqueID();
        item.name = playlist_name;
        item.path = "";  // No source file - this is a playlist container
        item.type = MediaType::PLAYLIST;
        item.duration = 0.0;  // Calculated from items
        item.playlist_loop = false;
        item.current_playlist_index = 0;

        // Add to media pool and bin
        media_pool.push_back(item);
        int bin_index = GetBinIndexForMediaType(item.type);
        if (bins.size() > static_cast<size_t>(bin_index)) {
            bins[bin_index].items.push_back(item);
        }

        Debug::Log("CreateNewPlaylist: Created playlist '" + playlist_name + "' with ID " + item.id);

        return item.id;
    }

    std::string ProjectManager::CreatePlaylistFromSelection() {
        // Get selected items that are playable (not PLAYLISTs, DUAL_VIEWs, or IMAGEs)
        std::vector<MediaItem> playable_items;
        for (const auto& selected_id : selected_media_items) {
            MediaItem* item = GetMediaItem(selected_id);
            if (item && item->type != MediaType::PLAYLIST && item->type != MediaType::IMAGE &&
                item->type != MediaType::DUAL_VIEW) {
                // Accept VIDEO, AUDIO, IMAGE_SEQUENCE, EXR_SEQUENCE only
                playable_items.push_back(*item);
            }
        }

        if (playable_items.empty()) {
            Debug::Log("CreatePlaylistFromSelection: No playable items selected");
            return "";
        }

        // Create the playlist
        std::string playlist_id = CreateNewPlaylist();
        MediaItem* playlist = GetPlaylistItem(playlist_id);
        if (!playlist) {
            Debug::Log("CreatePlaylistFromSelection: Failed to create playlist");
            return "";
        }

        // Add selected items as playlist entries
        for (const auto& item : playable_items) {
            PlaylistItemEntry entry;
            entry.media_id = item.id;
            entry.in_point = -1.0;  // Use default
            entry.out_point = -1.0;  // Use default
            playlist->playlist_items.push_back(entry);
        }

        // Also update in bins
        int bin_index = GetBinIndexForMediaType(MediaType::PLAYLIST);
        if (bins.size() > static_cast<size_t>(bin_index)) {
            for (auto& bin_item : bins[bin_index].items) {
                if (bin_item.id == playlist_id) {
                    bin_item.playlist_items = playlist->playlist_items;
                    break;
                }
            }
        }

        Debug::Log("CreatePlaylistFromSelection: Created playlist with " + std::to_string(playable_items.size()) + " items");

        return playlist_id;
    }

    MediaItem* ProjectManager::GetPlaylistItem(const std::string& playlist_id) {
        for (auto& item : media_pool) {
            if (item.type == MediaType::PLAYLIST && item.id == playlist_id) {
                return &item;
            }
        }
        return nullptr;
    }

    int ProjectManager::GetPlaylistCount() const {
        int count = 0;
        for (const auto& item : media_pool) {
            if (item.type == MediaType::PLAYLIST) count++;
        }
        return count;
    }

    void ProjectManager::OpenPlaylistInPanel(const std::string& playlist_id) {
        MediaItem* playlist = GetPlaylistItem(playlist_id);
        if (!playlist) {
            Debug::Log("OpenPlaylistInPanel: Playlist not found: " + playlist_id);
            return;
        }

        // Trigger callback to show playlist panel with this playlist
        if (playlist_panel_callback) {
            playlist_panel_callback(playlist_id);
        } else {
            Debug::Log("OpenPlaylistInPanel: No playlist_panel_callback set");
        }
    }

    void ProjectManager::OpenPlaylistInTimelineEditor(const std::string& playlist_id) {
        MediaItem* playlist = GetPlaylistItem(playlist_id);
        if (!playlist) {
            Debug::Log("OpenPlaylistInTimelineEditor: Playlist not found: " + playlist_id);
            return;
        }

        // Clear is_active from all playlists and dual views, then set on target playlist
        // Must update both media_pool AND bin.items (UI reads from bins)
        for (auto& pool_item : media_pool) {
            if (pool_item.type == MediaType::PLAYLIST || pool_item.type == MediaType::DUAL_VIEW) {
                pool_item.is_active = (pool_item.type == MediaType::PLAYLIST &&
                                       pool_item.id == playlist_id);
            }
        }
        for (auto& bin : bins) {
            for (auto& bin_item : bin.items) {
                if (bin_item.type == MediaType::PLAYLIST || bin_item.type == MediaType::DUAL_VIEW) {
                    bin_item.is_active = (bin_item.type == MediaType::PLAYLIST &&
                                          bin_item.id == playlist_id);
                }
            }
        }
        current_timeline_id.clear();  // Not a regular timeline

        // Trigger callback to load playlist into unified timeline editor
        if (playlist_timeline_callback) {
            playlist_timeline_callback(playlist);
        } else {
            // Fall back to legacy panel if no timeline callback set
            Debug::Log("OpenPlaylistInTimelineEditor: No playlist_timeline_callback set, falling back to panel");
            if (playlist_panel_callback) {
                playlist_panel_callback(playlist_id);
            }
        }
    }

    // ========================================================================
    // VIEW STATE MANAGEMENT (for persistent zoom/pan per-media)
    // ========================================================================

    void ProjectManager::CacheCurrentViewState(const std::string& media_path, float zoom, float scroll, double playhead) {
        // Handle URL-style paths (mf://, exr://) by extracting the base path
        std::string search_path = media_path;

        // Strip mf:// prefix and everything after | separator
        if (search_path.rfind("mf://", 0) == 0) {
            search_path = search_path.substr(5);  // Remove "mf://"
            size_t pipe_pos = search_path.find('|');
            if (pipe_pos != std::string::npos) {
                search_path = search_path.substr(0, pipe_pos);
            }
        }
        // Strip exr:// prefix and query params
        else if (search_path.rfind("exr://", 0) == 0) {
            search_path = search_path.substr(6);  // Remove "exr://"
            size_t query_pos = search_path.find('?');
            if (query_pos != std::string::npos) {
                search_path = search_path.substr(0, query_pos);
            }
        }

        for (auto& item : media_pool) {
            // Match by exact path or by base path for sequences
            // For sequences with ffmpeg_pattern, check if the first frame path is in the item's path
            bool exact_match = (item.path == media_path);
            bool stripped_match = (item.path == search_path);
            bool sequence_match = (!item.ffmpeg_pattern.empty() && item.path.find(search_path) != std::string::npos);

            if (exact_match || stripped_match || sequence_match) {
                item.view_state.zoom_level = zoom;
                item.view_state.scroll_offset = scroll;
                item.view_state.playhead_position = playhead;
                Debug::Log("CacheCurrentViewState: Saved zoom=" + std::to_string(zoom) +
                           ", scroll=" + std::to_string(scroll) +
                           ", playhead=" + std::to_string(playhead) + " for: " + item.name);
                return;
            }
        }
        Debug::Log("CacheCurrentViewState: No matching media found for: " + media_path);
        Debug::Log("  search_path: " + search_path);
    }

    void ProjectManager::CacheTimelineViewState(const std::string& timeline_id, float zoom, float scroll,
                                                double playhead, double in_point, double out_point) {
        MediaItem* item = GetTimelineItem(timeline_id);
        if (item) {
            item->view_state.timeline_zoom = zoom;
            item->view_state.timeline_scroll = scroll;
            item->view_state.timeline_playhead = playhead;
            item->view_state.timeline_in_point = in_point;
            item->view_state.timeline_out_point = out_point;
            Debug::Log("CacheTimelineViewState: Saved zoom=" + std::to_string(zoom) +
                       ", scroll=" + std::to_string(scroll) +
                       ", playhead=" + std::to_string(playhead) + " for timeline: " + timeline_id);
        }
    }

    bool ProjectManager::GetCachedViewState(const std::string& media_path, float& zoom, float& scroll, double& playhead) {
        // Handle URL-style paths (mf://, exr://) by extracting the base path
        std::string search_path = media_path;

        // Strip mf:// prefix and everything after | separator
        if (search_path.rfind("mf://", 0) == 0) {
            search_path = search_path.substr(5);  // Remove "mf://"
            size_t pipe_pos = search_path.find('|');
            if (pipe_pos != std::string::npos) {
                search_path = search_path.substr(0, pipe_pos);
            }
        }
        // Strip exr:// prefix and query params
        else if (search_path.rfind("exr://", 0) == 0) {
            search_path = search_path.substr(6);  // Remove "exr://"
            size_t query_pos = search_path.find('?');
            if (query_pos != std::string::npos) {
                search_path = search_path.substr(0, query_pos);
            }
        }

        for (const auto& item : media_pool) {
            // Match by exact path or by base path for sequences
            // For sequences with ffmpeg_pattern, check if the first frame path is in the item's path
            bool exact_match = (item.path == media_path);
            bool stripped_match = (item.path == search_path);
            bool sequence_match = (!item.ffmpeg_pattern.empty() && item.path.find(search_path) != std::string::npos);

            if (exact_match || stripped_match || sequence_match) {
                zoom = item.view_state.zoom_level;
                scroll = item.view_state.scroll_offset;
                playhead = item.view_state.playhead_position;
                Debug::Log("GetCachedViewState: Restored zoom=" + std::to_string(zoom) +
                           ", scroll=" + std::to_string(scroll) +
                           ", playhead=" + std::to_string(playhead) + " for: " + item.name);
                return true;
            }
        }
        Debug::Log("GetCachedViewState: No cached state for: " + media_path);
        return false;
    }

    bool ProjectManager::GetTimelineViewState(const std::string& timeline_id, float& zoom, float& scroll,
                                             double& playhead, double& in_point, double& out_point) {
        MediaItem* item = GetTimelineItem(timeline_id);
        if (item) {
            zoom = item->view_state.timeline_zoom;
            scroll = item->view_state.timeline_scroll;
            playhead = item->view_state.timeline_playhead;
            in_point = item->view_state.timeline_in_point;
            out_point = item->view_state.timeline_out_point;
            Debug::Log("GetTimelineViewState: Restored zoom=" + std::to_string(zoom) +
                       ", scroll=" + std::to_string(scroll) +
                       ", playhead=" + std::to_string(playhead) + " for timeline: " + timeline_id);
            return true;
        }
        return false;
    }

    // ============================================================================
    // DRAG & DROP OPERATIONS
    // ============================================================================

    void ProjectManager::LoadSingleFileFromDrop(const std::string& file_path) {
        Debug::Log("LoadSingleFileFromDrop called with: " + file_path);

        // Exit timeline mode when loading single file
        if (exit_timeline_mode_callback) {
            exit_timeline_mode_callback();
        }

        if (!IsValidMediaFile(file_path)) {
            Debug::Log("File is not valid media: " + file_path);
            return;
        }

        Debug::Log("File is valid media, checking type...");

        // Wait for file to be readable before checking if it's a sequence (cloud sync)
        Debug::Log("LoadSingleFileFromDrop: Checking if file is readable (cloud sync check)...");
        if (!WaitForFileReadable(file_path, 30)) {
            Debug::Log("ERROR: File not readable after waiting - may not be synced from cloud");
            return;
        }
        Debug::Log("LoadSingleFileFromDrop: File is readable, proceeding with type detection");

        // Check if this is an image sequence
        MediaType media_type = GetMediaType(file_path);
        Debug::Log("Media type determined: " + std::to_string(static_cast<int>(media_type)));

        if (media_type == MediaType::IMAGE_SEQUENCE || media_type == MediaType::EXR_SEQUENCE) {
            Debug::Log("Detected as image/EXR sequence, showing frame rate dialog");

            // Show frame rate dialog for image sequences
            ShowFrameRateDialog(file_path);
            return; // ProcessImageSequence will handle the actual loading
        }

        Debug::Log("Processing as regular media file");

        AddMediaFileToProject(file_path);

        // Clear all active states - loading a single file makes nothing else active
        current_timeline_id.clear();

        // Find the MediaItem that was just added
        MediaItem* media_item = nullptr;
        for (auto& item : media_pool) {
            if (item.path == file_path) {
                media_item = &item;
                break;
            }
        }

        if (!media_item) {
            Debug::Log("LoadSingleFileFromDrop: ERROR - Could not find MediaItem after AddMediaFileToProject");
            return;
        }

        // Route VIDEO files through OTIO video timeline path
        if (media_item->type == MediaType::VIDEO) {
            Debug::Log("LoadSingleFileFromDrop: Routing VIDEO through OTIO timeline path");

            if (video_file_timeline_callback) {
                *current_file_path = file_path;

                // Notify video change callback
                if (video_change_callback) {
                    video_change_callback(file_path);
                }

                // Select item in bin (visual feedback)
                SelectMediaItem(media_item->id, false, false);

                // Load into OTIO timeline view
                video_file_timeline_callback(media_item);

                Debug::Log("LoadSingleFileFromDrop: Video loaded via OTIO timeline");
                return;
            } else {
                Debug::Log("LoadSingleFileFromDrop: WARNING - No video_file_timeline_callback, falling back to MPV");
            }
        }

        // Route AUDIO files through OTIO audio timeline path
        if (media_item->type == MediaType::AUDIO) {
            Debug::Log("LoadSingleFileFromDrop: Routing AUDIO through OTIO audio timeline path");

            if (audio_file_timeline_callback) {
                *current_file_path = file_path;

                // Notify video change callback (reuse for consistency)
                if (video_change_callback) {
                    video_change_callback(file_path);
                }

                // Select item in bin (visual feedback)
                SelectMediaItem(media_item->id, false, false);

                // Load into OTIO timeline view (audio-only)
                audio_file_timeline_callback(media_item);

                Debug::Log("LoadSingleFileFromDrop: Audio loaded via OTIO timeline");
                return;
            } else {
                Debug::Log("LoadSingleFileFromDrop: WARNING - No audio_file_timeline_callback, falling back to MPV");
            }
        }

        // FALLBACK: Legacy MPV path (for GIFs or if timeline callback not set)
        Debug::Log("LoadSingleFileFromDrop: Using legacy MPV path");

        // Check if this is a GIF (disable cache for GIFs - they're fast enough without it)
        std::string extension = std::filesystem::path(file_path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        bool is_gif = (extension == ".gif");

        // Use cached metadata (already extracted by AddMediaFileToProject)
        const CombinedMetadata* cached_meta = GetCachedMetadata(file_path);
        if (cached_meta && cached_meta->video_meta && cached_meta->video_meta->is_loaded) {
            Debug::Log("LoadSingleFileFromDrop: Using cached metadata from AddMediaFileToProject");

            // Check for H.264/H.265/GIF and handle cache
            std::string codec = cached_meta->video_meta->video_codec;
            std::transform(codec.begin(), codec.end(), codec.begin(), ::tolower);

            bool is_h264_h265 = (codec.find("h264") != std::string::npos ||
                                 codec.find("hevc") != std::string::npos ||
                                 codec.find("h265") != std::string::npos ||
                                 codec.find("avc") != std::string::npos);

            if (is_h264_h265 || is_gif) {
                Debug::Log("LoadSingleFileFromDrop: H.264/H.265/GIF detected - disabling cache");
                SetCacheEnabled(false);
                ClearAllCaches();
                cache_auto_disabled_for_codec = true;
                current_video_codec = cached_meta->video_meta->video_codec;
            }
        } else {
            Debug::Log("LoadSingleFileFromDrop: WARNING - No cached metadata found (AddMediaFileToProject may have failed)");
        }

        // Load into MPV for playback (metadata already cached)
        video_player->LoadFile(file_path);
        *current_file_path = file_path;

        // Use OnVideoLoaded for proper sequencing (600ms delay before cache starts)
        OnVideoLoaded(file_path);
    }

    void ProjectManager::LoadMultipleFilesFromDrop(const std::vector<std::string>& file_paths) {
        std::set<std::string> processed_sequences; // Track sequences we've already processed

        for (const auto& file_path : file_paths) {
            if (!IsValidMediaFile(file_path)) continue;

            // Wait for file to be readable before checking if it's a sequence (cloud sync)
            Debug::Log("LoadMultipleFilesFromDrop: Checking if file is readable (cloud sync check): " + file_path);
            if (!WaitForFileReadable(file_path, 30)) {
                Debug::Log("ERROR: File not readable after waiting, skipping: " + file_path);
                continue;
            }
            Debug::Log("LoadMultipleFilesFromDrop: File is readable, proceeding with type detection");

            MediaType media_type = GetMediaType(file_path);
            if (media_type == MediaType::IMAGE_SEQUENCE || media_type == MediaType::EXR_SEQUENCE) {
                // For image sequences, we need to group them and process once per sequence
                std::vector<std::string> sequence_files = DetectImageSequence(file_path);
                if (!sequence_files.empty()) {
                    // Use the first file as the sequence identifier
                    std::string sequence_id = sequence_files[0];

                    if (processed_sequences.find(sequence_id) == processed_sequences.end()) {
                        processed_sequences.insert(sequence_id);
                        // Show frame rate dialog for the first file of each sequence
                        ShowFrameRateDialog(file_path);
                    }
                }
            } else {
                // Regular media files
                AddMediaFileToProject(file_path);
            }
        }
    }

    bool ProjectManager::IsValidMediaFile(const std::string& file_path) {
        size_t dot_pos = file_path.find_last_of('.');
        if (dot_pos == std::string::npos) return false;

        std::string extension = file_path.substr(dot_pos + 1);
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

        // Supported formats only: Video, Audio, JPEG, PNG, TIFF, EXR
        // Removed unsupported: BMP, TGA, DPX, JPEG2000 (j2k, jp2)
        std::vector<std::string> supported = {
            // Video formats
            "mp4", "avi", "mkv", "mov", "wmv", "flv", "webm", "m4v", "3gp", "ogv", "ts", "mts", "m2ts", "mxf", "gif",
            // Audio formats
            "wav", "mp3", "aac", "flac", "ogg", "wma", "m4a",
            // Image formats (supported only)
            "jpg", "jpeg", "png", "tiff", "tif", "exr", "hdr"
        };

        return std::find(supported.begin(), supported.end(), extension) != supported.end();
    }

    // ============================================================================
    // METADATA MANAGEMENT
    // ============================================================================

    void ProjectManager::StartAdobeWorkerThread() {
        worker_running = true;
        adobe_worker_thread = std::thread(&ProjectManager::AdobeWorkerLoop, this);
    }

    void ProjectManager::StopAdobeWorkerThread() {
        worker_running = false;
        if (adobe_worker_thread.joinable()) {
            adobe_worker_thread.join();
        }
    }

    void ProjectManager::AdobeWorkerLoop() {
        while (worker_running) {
            std::string file_path;
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (!adobe_metadata_queue.empty()) {
                    file_path = adobe_metadata_queue.front();
                    adobe_metadata_queue.pop();
                }
            }

            if (!file_path.empty()) {
                ProcessAdobeMetadata(file_path);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void ProjectManager::ProcessAdobeMetadata(const std::string& file_path) {
        auto adobe_meta = AdobeMetadataExtractor::ExtractAdobePaths(file_path);
        {
            std::lock_guard<std::mutex> lock(queue_mutex);

            // Remove from queued set (allows re-queueing if needed later)
            adobe_metadata_queued.erase(file_path);

            auto it = metadata_cache.find(file_path);
            if (it != metadata_cache.end()) {
                it->second.adobe_meta = std::move(adobe_meta);
                it->second.state = MetadataState::COMPLETE;

                // Log extraction timing
                auto duration = std::chrono::steady_clock::now() - it->second.start_time;
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
                // Debug removed
            }
        }
    }

    void ProjectManager::QueueAdobeMetadata(const std::string& file_path) {
        // Skip Adobe metadata extraction for image sequences and EXRs
        if (ShouldSkipAdobeMetadataExtraction(file_path)) {
            return;
        }

        std::lock_guard<std::mutex> lock(queue_mutex);

        // Deduplication: don't queue if already queued or in progress
        if (adobe_metadata_queued.find(file_path) != adobe_metadata_queued.end()) {
            return;
        }

        adobe_metadata_queued.insert(file_path);
        adobe_metadata_queue.push(file_path);
    }

    const ProjectManager::CombinedMetadata* ProjectManager::GetCachedMetadata(const std::string& file_path) const {
        auto it = metadata_cache.find(file_path);
        return (it != metadata_cache.end()) ? &it->second : nullptr;
    }

    void ProjectManager::CopyMetadataToEDL(const std::string& original_path, const std::string& edl_path) {
        // Copy metadata from original video file to EDL path
        // This allows EDL (trimmed) videos to inherit all properties from the source
        auto it = metadata_cache.find(original_path);
        if (it != metadata_cache.end()) {
            Debug::Log("CopyMetadataToEDL: Copying metadata from '" + original_path + "' to '" + edl_path + "'");

            // Create new entry for EDL path (deep copy since unique_ptrs don't support copy assignment)
            CombinedMetadata& edl_meta = metadata_cache[edl_path];
            edl_meta.state = it->second.state;
            edl_meta.start_time = it->second.start_time;

            // Deep copy video metadata
            if (it->second.video_meta) {
                edl_meta.video_meta = std::make_unique<VideoMetadata>(*it->second.video_meta);
            }

            // Deep copy adobe metadata
            if (it->second.adobe_meta) {
                edl_meta.adobe_meta = std::make_unique<AdobeMetadata>(*it->second.adobe_meta);
            }

            // Deep copy EXR metadata
            if (it->second.exr_meta) {
                edl_meta.exr_meta = std::make_unique<EXRMetadata>(*it->second.exr_meta);
            }

            Debug::Log("CopyMetadataToEDL: Metadata copied successfully (resolution: " +
                       std::to_string(edl_meta.video_meta ? edl_meta.video_meta->width : 0) + "x" +
                       std::to_string(edl_meta.video_meta ? edl_meta.video_meta->height : 0) + ")");
        } else {
            Debug::Log("CopyMetadataToEDL: WARNING - No cached metadata found for original path: " + original_path);
        }
    }

    void ProjectManager::ExtractMetadataForClip(const std::string& file_path) {
        // Deprecated method - use QueueVideoMetadataExtraction instead
        QueueVideoMetadataExtraction(file_path, true);
    }

    bool ProjectManager::ShouldSkipAdobeMetadataExtraction(const std::string& file_path) {
        if (file_path.empty()) return true;

        // Skip Adobe metadata extraction for MF:// (image sequence) URLs
        if (file_path.substr(0, 5) == "mf://") {
            return true;
        }

        // Skip Adobe metadata extraction for custom EXR:// URLs
        if (file_path.substr(0, 6) == "exr://") {
            return true;
        }

        // Check file extension for image sequences and EXRs
        try {
            std::filesystem::path path(file_path);
            std::string extension = path.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

            // Skip Adobe metadata for image sequence formats and EXRs
            // These files don't contain Adobe project paths or timecode info
            if (extension == ".exr" || extension == ".jpg" || extension == ".jpeg" ||
                extension == ".png" || extension == ".bmp" || extension == ".tiff" ||
                extension == ".tif" || extension == ".tga" || extension == ".hdr" ||
                extension == ".dpx") {

                return true; // Skip Adobe metadata for all image formats
            }
        } catch (...) {
            // If path parsing fails, don't skip (safer to extract if unsure)
            return false;
        }

        return false; // Extract Adobe metadata for video/audio files
    }

    void ProjectManager::QueueVideoMetadataExtraction(const std::string& file_path, bool high_priority) {
        if (file_path.empty()) return;

        // Special handling for image sequences (MF:// URLs)
        if (file_path.substr(0, 5) == "mf://") {
            // For image sequences, extract metadata from the first frame only
            std::string mf_url = file_path;

            // Parse the MF URL to get the first frame path
            // Format: mf://path/to/sequence_%04d.exr:fps=24
            size_t fps_pos = mf_url.find(":fps=");
            if (fps_pos != std::string::npos) {
                mf_url = mf_url.substr(0, fps_pos); // Remove fps parameter
            }

            if (mf_url.substr(0, 5) == "mf://") {
                std::string pattern_path = mf_url.substr(5); // Remove "mf://"

                // Convert printf-style pattern to actual first frame
                // e.g., "path/sequence_%04d.exr" -> find actual first frame
                size_t printf_pos = pattern_path.find('%');
                if (printf_pos != std::string::npos) {
                    // Find the end of the printf pattern
                    size_t d_pos = pattern_path.find('d', printf_pos);
                    if (d_pos != std::string::npos) {
                        std::string directory = pattern_path.substr(0, pattern_path.find_last_of('/'));
                        std::string base_part = pattern_path.substr(0, printf_pos);
                        base_part = base_part.substr(base_part.find_last_of('/') + 1);
                        std::string extension = pattern_path.substr(d_pos + 1);

                        // Parse the base name to understand the pattern
                        std::regex pattern_regex(R"(^(.+?)([_\.\-]?)$)");
                        std::smatch pattern_match;

                        std::string base_name, separator;
                        if (std::regex_match(base_part, pattern_match, pattern_regex)) {
                            base_name = pattern_match[1].str();
                            separator = pattern_match[2].str();
                        } else {
                            base_name = base_part;
                        }

                        // Find the first matching file in the directory
                        try {
                            std::vector<std::string> matching_files;
                            std::regex file_pattern(base_name + separator + R"(\d{1,12})" + extension);

                            for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                                if (entry.is_regular_file()) {
                                    std::string filename = entry.path().filename().string();
                                    if (std::regex_match(filename, file_pattern)) {
                                        matching_files.push_back(entry.path().string());
                                    }
                                }
                            }

                            if (!matching_files.empty()) {
                                // Sort and use the first file
                                std::sort(matching_files.begin(), matching_files.end());
                                QueueVideoMetadataExtraction(matching_files[0], high_priority);
                                return;
                            }
                        } catch (const std::filesystem::filesystem_error&) {
                            // If we can't read the directory, fall back to normal processing
                        }
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex);

            // Skip if metadata already exists or is being processed
            auto it = metadata_cache.find(file_path);
            if (it != metadata_cache.end()) {
                if (it->second.state != MetadataState::NOT_STARTED) {
                    return;  // Already processing or complete
                }
            }

            // Initialize metadata entry with loading state
            metadata_cache[file_path].state = MetadataState::LOADING_VIDEO;
            metadata_cache[file_path].start_time = std::chrono::steady_clock::now();
        }

        {
            std::lock_guard<std::mutex> lock(video_queue_mutex);
            video_metadata_queue.push({file_path, high_priority});
        }

        // Debug removed
    }

    void ProjectManager::StartVideoMetadataWorkerThread() {
        video_worker_running = true;
        video_metadata_worker_thread = std::thread(&ProjectManager::VideoMetadataWorkerLoop, this);
        // Debug removed
    }

    void ProjectManager::StopVideoMetadataWorkerThread() {
        video_worker_running = false;
        if (video_metadata_worker_thread.joinable()) {
            video_metadata_worker_thread.join();
        }
        // Debug removed
    }

    void ProjectManager::VideoMetadataWorkerLoop() {
        while (video_worker_running) {
            std::string file_path;
            bool high_priority = false;

            {
                std::lock_guard<std::mutex> lock(video_queue_mutex);
                if (!video_metadata_queue.empty()) {
                    auto item = video_metadata_queue.front();
                    file_path = item.first;
                    high_priority = item.second;
                    video_metadata_queue.pop();
                }
            }

            if (!file_path.empty()) {
                ProcessVideoMetadata(file_path);
            }
            else {
                // No work to do, sleep briefly
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    void ProjectManager::ProcessVideoMetadata(const std::string& file_path) {
        // === REFACTORED: Video metadata now extracted by FFmpeg BEFORE MPV load ===
        // This method now ONLY handles Adobe metadata (timecode, project links)
        // Video metadata is extracted in LoadSingleMediaItem() using FFmpegMetadataExtractor

        Debug::Log("ProcessVideoMetadata: Checking for Adobe metadata (timecode, project links)...");

        // Verify that FFmpeg metadata already exists
        const CombinedMetadata* cached_meta = GetCachedMetadata(file_path);
        if (!cached_meta || !cached_meta->video_meta || !cached_meta->video_meta->is_loaded) {
            Debug::Log("ProcessVideoMetadata: WARNING - No FFmpeg metadata found for: " + file_path);
            Debug::Log("  This should have been extracted before MPV load!");
            return;
        }

        Debug::Log("ProcessVideoMetadata: FFmpeg metadata confirmed present");
        Debug::Log("  Resolution: " + std::to_string(cached_meta->video_meta->width) + "x" +
                   std::to_string(cached_meta->video_meta->height));
        Debug::Log("  Codec: " + cached_meta->video_meta->video_codec);

        // Auto 1-2-1 detection (non-blocking background processing)
        if (color_preset_callback) {
            if (cached_meta && cached_meta->video_meta) {
                // Trigger lazy NCLC detection if not already done
                if (cached_meta->video_meta->nclc_tag.empty()) {
                    const_cast<VideoMetadata*>(cached_meta->video_meta.get())->DetectAndCacheNCLC();
                }

                // Check if it's a 1-2-1 video and invoke callback
                if (cached_meta->video_meta->nclc_tag == "1-2-1") {
                    Debug::Log("Auto 1-2-1: Detected 1-2-1 NCLC tag, invoking color preset callback");
                    color_preset_callback("1-2-1");
                }
            }
        }

        // Queue Adobe metadata extraction (timecode, project links)
        QueueAdobeMetadata(file_path);

        Debug::Log("ProcessVideoMetadata: Queued Adobe metadata extraction");
    }

    void ProjectManager::DisplayVideoMetadata(const VideoMetadata* video_meta) {
        if (!video_meta || !video_meta->is_loaded) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No metadata available");
            return;
        }

        bool is_audio_only = IsAudioOnlyFile(video_meta);

        // Always show file information
        ImGui::Spacing();
        ImGui::Text("File Information");
        ImGui::Separator();
        DisplayFileInfoTable(video_meta);

        // For audio-only files, prioritize audio properties
        if (is_audio_only) {
            if (HasAudioInfo(video_meta)) {
                ImGui::Spacing();
                ImGui::Text("Audio Properties");
                ImGui::Separator();
                DisplayAudioPropertiesTable(video_meta);
            }
        } else {
            // For video files, show video properties first
            ImGui::Spacing();
            ImGui::Text("Video Properties");
            ImGui::Separator();
            DisplayVideoPropertiesTable(video_meta);

            if (HasColorInfo(video_meta)) {
                ImGui::Spacing();
                ImGui::Text("Color Properties");
                ImGui::Separator();
                DisplayColorPropertiesTable(video_meta);
            }

            if (HasAudioInfo(video_meta)) {
                ImGui::Spacing();
                ImGui::Text("Audio Properties");
                ImGui::Separator();
                DisplayAudioPropertiesTable(video_meta);
            }
        }
    }

    void ProjectManager::DisplayAdobeMetadata(const AdobeMetadata* adobe_meta) {
        if (!adobe_meta || !adobe_meta->is_loaded) {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), "Loading Adobe project info...");
            return;
        }

        if (!adobe_meta->HasAnyAdobeProject()) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No Adobe project links found");
            return;
        }

        ImGui::Spacing();
        ImGui::Text("Adobe Projects");
        ImGui::Separator();
        DisplayAdobeProjectsTable(adobe_meta);
    }

    void ProjectManager::DisplayTimecodeTable(const AdobeMetadata* adobe_meta) {
        if (!adobe_meta) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No timecode metadata available");
            return;
        }

        if (!adobe_meta->HasAnyTimecode()) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No timecode found in metadata");
            return;
        }

        ImGui::Spacing();
        ImGui::Text("Timecode");
        ImGui::Separator();

        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));
        if (ImGui::BeginTable("TimecodeTable", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Timecode", ImGuiTableColumnFlags_WidthStretch);

            // QuickTime Start Timecode
            if (!adobe_meta->qt_start_timecode.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("QT Start:");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s", adobe_meta->qt_start_timecode.c_str());
                // Invert button background based on row alternation
                int row_idx = ImGui::TableGetRowIndex();
                bool is_alt_row = (row_idx % 2) == 1;
                ImGui::PushStyleColor(ImGuiCol_Button, is_alt_row ? ImVec4(0.141f, 0.141f, 0.141f, 1.0f) : ImVec4(0.192f, 0.192f, 0.192f, 1.0f));
                if (ImGui::SmallButton("Copy##QTStart")) {
                    CopyToClipboard(adobe_meta->qt_start_timecode);
                }
                ImGui::PopStyleColor();
            }

            // QuickTime General Timecode
            if (!adobe_meta->qt_timecode.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("QT TimeCode:");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s", adobe_meta->qt_timecode.c_str());
                // Invert button background based on row alternation
                int row_idx2 = ImGui::TableGetRowIndex();
                bool is_alt_row2 = (row_idx2 % 2) == 1;
                ImGui::PushStyleColor(ImGuiCol_Button, is_alt_row2 ? ImVec4(0.141f, 0.141f, 0.141f, 1.0f) : ImVec4(0.192f, 0.192f, 0.192f, 1.0f));
                if (ImGui::SmallButton("Copy##QTCode")) {
                    CopyToClipboard(adobe_meta->qt_timecode);
                }
                ImGui::PopStyleColor();
            }

            // Creation dates for reference
            if (!adobe_meta->qt_creation_date.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Created:");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", adobe_meta->qt_creation_date.c_str());
            }

            if (!adobe_meta->qt_media_create_date.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Media Created:");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", adobe_meta->qt_media_create_date.c_str());
            }

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();  // CellPadding
    }

    // ============================================================================
    // EXR SEQUENCE METADATA DISPLAY
    // ============================================================================

    void ProjectManager::DisplayEXRMetadata(const EXRMetadata* exr_meta) {
        if (!exr_meta || !exr_meta->is_loaded) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No metadata available");
            return;
        }

        // Show file information
        ImGui::Spacing();
        ImGui::Text("File Information");
        ImGui::Separator();
        DisplayEXRFileInfoTable(exr_meta);

        // Show image properties
        ImGui::Spacing();
        ImGui::Text("Image Properties");
        ImGui::Separator();
        DisplayEXRImagePropertiesTable(exr_meta);

        // Show layer information (lazy loaded)
        if (exr_meta->extended_properties_detected && exr_meta->total_layers > 0) {
            ImGui::Spacing();
            ImGui::Text("EXR Layers");
            ImGui::Separator();
            DisplayEXRChannelsTable(exr_meta);
        }
    }

    void ProjectManager::DisplayEXRFileInfoTable(const EXRMetadata* exr_meta) {
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));
        if (ImGui::BeginTable("EXRFileInfoTable", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 120.0f);

            // File Name
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("Name:");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", exr_meta->file_name.c_str());

            // Path (directory)
            std::string exr_directory;
            if (!exr_meta->file_path.empty()) {
                size_t last_slash = exr_meta->file_path.find_last_of("/\\");
                if (last_slash != std::string::npos) {
                    exr_directory = exr_meta->file_path.substr(0, last_slash);
                }
            }
            if (!exr_directory.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Path:");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextWrapped("%s", exr_directory.c_str());
                RenderPathButtons(exr_directory, "EXRMetaPath");
            }

            // Sequence Pattern
            if (!exr_meta->sequence_pattern.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Sequence:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", exr_meta->sequence_pattern.c_str());
            }

            // Frame Range
            if (exr_meta->total_frames > 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Frame Range:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d-%d (%d frames)", exr_meta->start_frame, exr_meta->end_frame, exr_meta->total_frames);
            }

            // Frame Rate
            if (exr_meta->frame_rate > 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Frame Rate:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f fps", exr_meta->frame_rate);
            }

            // Duration
            if (exr_meta->total_frames > 0 && exr_meta->frame_rate > 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Duration:");
                ImGui::TableSetColumnIndex(1);
                double duration = exr_meta->total_frames / exr_meta->frame_rate;
                ImGui::Text("%.2f seconds", duration);
            }

            // File Size (first frame)
            if (exr_meta->file_size > 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("File Size:");
                ImGui::TableSetColumnIndex(1);
                double size_mb = exr_meta->file_size / (1024.0 * 1024.0);
                ImGui::Text("%.2f MB (per frame)", size_mb);
            }

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();  // CellPadding
    }

    void ProjectManager::DisplayEXRImagePropertiesTable(const EXRMetadata* exr_meta) {
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));
        if (ImGui::BeginTable("EXRImagePropsTable", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            // Resolution
            if (exr_meta->width > 0 && exr_meta->height > 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Resolution:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d x %d", exr_meta->width, exr_meta->height);
            }

            // Trigger lazy metadata extraction when properties are displayed
            if (!exr_meta->extended_properties_detected) {
                const_cast<EXRMetadata*>(exr_meta)->DetectAndCacheExtendedProperties();
            }

            // Display Window (if different from resolution)
            if (exr_meta->extended_properties_detected &&
                (exr_meta->display_width != exr_meta->width || exr_meta->display_height != exr_meta->height)) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Display Window:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d x %d", exr_meta->display_width, exr_meta->display_height);
            }

            // Data Window
            if (exr_meta->extended_properties_detected && exr_meta->data_width > 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Data Window:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d x %d", exr_meta->data_width, exr_meta->data_height);
            }

            // Pixel Format
            if (exr_meta->extended_properties_detected && !exr_meta->pixel_format.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Pixel Format:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", exr_meta->pixel_format.c_str());
            }

            // Bit Depth
            if (exr_meta->extended_properties_detected && exr_meta->bit_depth > 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Bit Depth:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d-bit", exr_meta->bit_depth);
            }

            // Compression
            if (exr_meta->extended_properties_detected && !exr_meta->compression.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Compression:");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s", exr_meta->compression.c_str());
            }

            // Tiled format
            if (exr_meta->extended_properties_detected && exr_meta->is_tiled) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Format:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("Tiled");
            }

            // Multi-part
            if (exr_meta->extended_properties_detected && exr_meta->is_multi_part) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Multi-Part:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("Yes (%d parts)", exr_meta->part_count);
            }

            // Selected Layer - Interactive dropdown for EXR sequences
            if (!exr_meta->layer_name.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Layer:");
                ImGui::TableSetColumnIndex(1);

                // Try to get the media item for this EXR sequence
                MediaItem* current_item = GetMediaItemFromCurrentPath();

                // Only show dropdown if we have a media item (sequence is in media pool)
                if (current_item && current_item->type == MediaType::EXR_SEQUENCE) {
                    // Detect available layers from the first frame
                    static std::vector<std::string> available_layer_names;
                    static std::vector<std::string> available_layer_display_names;
                    static std::string last_exr_path;

                    // Get the sequence path from the media item
                    std::string sequence_path = current_item->path;
                    if (sequence_path.substr(0, 6) == "exr://") {
                        size_t query_pos = sequence_path.find("?layer=");
                        if (query_pos != std::string::npos) {
                            sequence_path = sequence_path.substr(6, query_pos - 6);
                        }
                    }

                    // Re-detect layers if this is a different file
                    if (sequence_path != last_exr_path) {
                        available_layer_names.clear();
                        available_layer_display_names.clear();
                        last_exr_path = sequence_path;

                        // Detect layers using EXRLayerDetector
                        EXRLayerDetector detector;
                        std::vector<EXRLayer> layers;
                        int crypto_count = 0;
                        if (detector.DetectLayers(sequence_path, layers, crypto_count)) {
                            for (const EXRLayer& layer : layers) {
                                available_layer_names.push_back(layer.name);
                                available_layer_display_names.push_back(layer.display_name);
                            }
                        }

                        // Fallback: If no layers detected, add current layer
                        if (available_layer_names.empty() && !current_item->exr_layer.empty()) {
                            available_layer_names.push_back(current_item->exr_layer);
                            available_layer_display_names.push_back(current_item->exr_layer_display);
                        }
                    }

                    // Find current layer index
                    int current_layer_index = 0;
                    for (int i = 0; i < available_layer_names.size(); i++) {
                        if (available_layer_names[i] == current_item->exr_layer) {
                            current_layer_index = i;
                            break;
                        }
                    }

                    // Show dropdown combo box
                    if (!available_layer_display_names.empty()) {
                        if (font_regular) ImGui::PushFont(font_regular);
                        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.07f, 0.07f, 1.00f));
                        if (ImGui::BeginCombo("##layer_selector", available_layer_display_names[current_layer_index].c_str())) {
                            for (int i = 0; i < available_layer_display_names.size(); i++) {
                                bool is_selected = (current_layer_index == i);
                                if (ImGui::Selectable(available_layer_display_names[i].c_str(), is_selected)) {
                                    // Layer changed - update media item and reload
                                    if (i != current_layer_index) {
                                        std::string item_id = current_item->id;

                                        Debug::Log("EXR Layer Changed:");
                                        Debug::Log("  Old layer: " + current_item->exr_layer_display);
                                        Debug::Log("  New layer: " + available_layer_display_names[i]);
                                        Debug::Log("  Item ID: " + item_id);

                                        current_item->exr_layer = available_layer_names[i];
                                        current_item->exr_layer_display = available_layer_display_names[i];

                                        // Update path with new layer
                                        std::string base_path = current_item->path;
                                        size_t query_pos = base_path.find("?layer=");
                                        if (query_pos != std::string::npos) {
                                            base_path = base_path.substr(0, query_pos);
                                        }
                                        current_item->path = base_path + "?layer=" + current_item->exr_layer;

                                        // Update item name to reflect new layer
                                        // Format: "basename [N frames X-Y] - LayerName"
                                        // Extract base name (everything before " [")
                                        std::string old_name = current_item->name;
                                        size_t bracket_pos = old_name.find(" [");
                                        if (bracket_pos != std::string::npos) {
                                            std::string base_name = old_name.substr(0, bracket_pos);
                                            current_item->name = base_name + " [" +
                                                std::to_string(current_item->frame_count) + " frames " +
                                                std::to_string(current_item->start_frame) + "-" +
                                                std::to_string(current_item->end_frame) + "] - " +
                                                current_item->exr_layer_display;
                                            Debug::Log("  Updated item name: " + current_item->name);
                                        } else {
                                            Debug::Log("  WARNING: Could not parse item name format");
                                        }

                                        Debug::Log("  Updated media_pool item - new path: " + current_item->path);

                                        // Also update the bin copy (bins store copies of MediaItems, not references)
                                        // Find the item in its bin and update it
                                        bool bin_updated = false;
                                        for (auto& bin : bins) {
                                            for (auto& bin_item : bin.items) {
                                                if (bin_item.id == item_id) {
                                                    bin_item.exr_layer = available_layer_names[i];
                                                    bin_item.exr_layer_display = available_layer_display_names[i];
                                                    bin_item.path = current_item->path;
                                                    bin_item.name = current_item->name;  // ← Update name too!
                                                    bin_updated = true;
                                                    Debug::Log("  Updated bin item in bin: " + bin.name);
                                                    break;  // Break inner loop, continue checking other bins
                                                }
                                            }
                                            if (bin_updated) break;  // Item found and updated, exit outer loop
                                        }

                                        if (bin_updated) {
                                            Debug::Log("  Successfully updated bin copy");
                                        } else {
                                            Debug::Log("  WARNING: Item not found in any bin!");
                                        }

                                        // Reload the sequence with new layer
                                        Debug::Log("  Reloading sequence with new layer...");
                                        LoadSingleMediaItem(*current_item);
                                        Debug::Log("  Layer change complete");
                                    }
                                }
                                if (is_selected) {
                                    ImGui::SetItemDefaultFocus();
                                }
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::PopStyleColor();
                        if (font_regular) ImGui::PopFont();
                    } else {
                        // Fallback: Show as text if no layers available
                        if (font_regular) ImGui::PushFont(font_regular);
                        ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s", exr_meta->layer_name.c_str());
                        if (font_regular) ImGui::PopFont();
                    }
                } else {
                    // Not a media pool item - show as read-only text
                    if (font_regular) ImGui::PushFont(font_regular);
                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s", exr_meta->layer_name.c_str());
                    if (font_regular) ImGui::PopFont();
                }
            }

            // Color space (if present)
            if (exr_meta->extended_properties_detected && !exr_meta->colorspace.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Colorspace:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", exr_meta->colorspace.c_str());
            }

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();  // CellPadding
    }

    void ProjectManager::DisplayEXRChannelsTable(const EXRMetadata* exr_meta) {
        // Show layer summary above the table
        if (!exr_meta->layer_summary.empty()) {
            ImGui::TextDisabled("%s", exr_meta->layer_summary.c_str());
            ImGui::Spacing();
        }

        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));
        if (ImGui::BeginTable("EXRLayersTable", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("Channels", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableHeadersRow();

            // Show individual layers (all of them, typically not more than 10-15)
            for (size_t i = 0; i < exr_meta->layers.size(); i++) {
                const EXRLayerInfo& layer = exr_meta->layers[i];

                ImGui::TableNextRow();

                // Layer name/display name
                ImGui::TableSetColumnIndex(0);
                if (layer.is_main_layer) {
                    // Highlight main layer
                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s", layer.display_name.c_str());
                } else {
                    ImGui::Text("%s", layer.display_name.c_str());
                }

                // Channel types (RGB, RGBA, etc.)
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", layer.channel_types.c_str());

                // Pixel type
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", layer.pixel_type.c_str());

                // Channel count
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%d", layer.channel_count);
            }

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();  // CellPadding
    }

    void ProjectManager::DisplayEXRPropertiesForItem(MediaItem* item, TimelineView* timeline_view) {
        if (!item) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No EXR sequence selected");
            return;
        }

        // Try to get cached EXR metadata
        const CombinedMetadata* cached_meta = GetCachedMetadata(item->path);
        if (cached_meta && cached_meta->exr_meta) {
            // Use the existing detailed EXR metadata display
            DisplayEXRMetadata(cached_meta->exr_meta.get());
        } else {
            // No cached metadata - create display from MediaItem, TimelineView, and EXR file analysis

            // Get source directory
            std::string source_dir;
            if (timeline_view) {
                source_dir = timeline_view->GetSourceDirectory();
            }
            // Fallback: extract directory from item path (exr://path?layer=...)
            if (source_dir.empty() && item->path.size() > 6 && item->path.substr(0, 6) == "exr://") {
                std::string path_part = item->path.substr(6);
                size_t query_pos = path_part.find('?');
                if (query_pos != std::string::npos) {
                    path_part = path_part.substr(0, query_pos);
                }
                size_t last_slash = path_part.find_last_of("/\\");
                if (last_slash != std::string::npos) {
                    source_dir = path_part.substr(0, last_slash);
                }
            }

            // Find first EXR file for metadata extraction
            std::string first_exr;
            try {
                namespace fs = std::filesystem;
                for (const auto& entry : fs::directory_iterator(source_dir)) {
                    if (entry.is_regular_file()) {
                        std::string ext = entry.path().extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if (ext == ".exr") {
                            first_exr = entry.path().string();
                            break;
                        }
                    }
                }
            } catch (...) {}

            // Get sequence pattern from item
            std::string seq_pattern = !item->image_seq.pattern.empty() ? item->image_seq.pattern :
                                      (!item->sequence_pattern.empty() ? item->sequence_pattern : "");

            // === FILE INFORMATION SECTION ===
            ImGui::Spacing();
            ImGui::Text("File Information");
            ImGui::Separator();

            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));
            if (ImGui::BeginTable("EXRFileInfo", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
                ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                // File Name (from first EXR or pattern)
                std::string file_name = !first_exr.empty() ?
                    std::filesystem::path(first_exr).filename().string() :
                    (!seq_pattern.empty() ? seq_pattern : item->name);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Name:");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextWrapped("%s", file_name.c_str());

                // Path
                if (!source_dir.empty()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("Path:");
                    ImGui::TableSetColumnIndex(1);
                        ImGui::TextWrapped("%s", source_dir.c_str());
                        RenderPathButtons(source_dir, "EXRPath");
                }

                // Sequence Pattern
                if (!seq_pattern.empty()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("Sequence:");
                    ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%s", seq_pattern.c_str());
                    }

                // Frame Range
                int start_frame = item->start_frame > 0 ? item->start_frame : item->image_seq.start_frame;
                int end_frame = item->end_frame > 0 ? item->end_frame : item->image_seq.end_frame;
                int frame_count = item->frame_count > 0 ? item->frame_count : item->image_seq.frame_count;
                if (frame_count > 0) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("Frame Range:");
                    ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d - %d (%d frames)", start_frame, end_frame, frame_count);
                    }

                ImGui::EndTable();
            }
            ImGui::PopStyleVar();

            // === IMAGE PROPERTIES SECTION ===
            ImGui::Spacing();
            ImGui::Text("Image Properties");
            ImGui::Separator();

            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));
            if (ImGui::BeginTable("EXRImageProps", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
                ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                // Resolution
                int width = item->image_seq.width > 0 ? item->image_seq.width :
                            (item->sequence_width > 0 ? item->sequence_width :
                            (timeline_view ? timeline_view->GetCanvasWidth() : 0));
                int height = item->image_seq.height > 0 ? item->image_seq.height :
                             (item->sequence_height > 0 ? item->sequence_height :
                             (timeline_view ? timeline_view->GetCanvasHeight() : 0));
                if (width > 0 && height > 0) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("Resolution:");
                    ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d x %d", width, height);
                    }

                // Pixel Format (detect from first channel type)
                std::string pixel_format = "16-bit half float";  // EXR default

                // Frame Rate
                double fps = item->frame_rate > 0 ? item->frame_rate :
                            (item->image_seq.frame_rate > 0 ? item->image_seq.frame_rate :
                            (timeline_view ? timeline_view->GetFrameRate() : 24.0));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Frame Rate:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f fps", fps);

                // Duration
                int frame_count_for_dur = item->frame_count > 0 ? item->frame_count : item->image_seq.frame_count;
                double duration = timeline_view ? timeline_view->GetDuration() :
                                  (frame_count_for_dur > 0 && fps > 0 ? frame_count_for_dur / fps : 0);
                if (duration > 0) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("Duration:");
                    ImGui::TableSetColumnIndex(1);
                        int total_secs = (int)duration;
                    int mins = total_secs / 60;
                    int secs = total_secs % 60;
                    int frames = (int)((duration - total_secs) * fps);
                    ImGui::Text("%02d:%02d:%02d (%.2fs)", mins, secs, frames, duration);
                    }

                ImGui::EndTable();
            }
            ImGui::PopStyleVar();

            // === EXR LAYER PICKER ===
            // Show layer selection combo box if we have detected layers
            if (!first_exr.empty() && item->type == MediaType::EXR_SEQUENCE) {
                // Cache detected layers (static to avoid re-detecting every frame)
                static std::vector<std::string> available_layer_names;
                static std::vector<std::string> available_layer_display_names;
                static std::string last_exr_path;

                // Re-detect layers if this is a different file
                if (first_exr != last_exr_path) {
                    available_layer_names.clear();
                    available_layer_display_names.clear();
                    last_exr_path = first_exr;

                    EXRLayerDetector detector;
                    std::vector<EXRLayer> layers;
                    if (detector.DetectLayers(first_exr, layers)) {
                        for (const EXRLayer& layer : layers) {
                            available_layer_names.push_back(layer.name);
                            available_layer_display_names.push_back(layer.display_name);
                        }
                    }

                    // Fallback: If no layers detected, add current layer
                    if (available_layer_names.empty() && !item->exr_layer.empty()) {
                        available_layer_names.push_back(item->exr_layer);
                        available_layer_display_names.push_back(
                            !item->exr_layer_display.empty() ? item->exr_layer_display : item->exr_layer);
                    }
                }

                // Find current layer index
                int current_layer_index = 0;
                for (int i = 0; i < (int)available_layer_names.size(); i++) {
                    if (available_layer_names[i] == item->exr_layer ||
                        available_layer_names[i] == item->image_seq.layer) {
                        current_layer_index = i;
                        break;
                    }
                }

                // Show dropdown if we have layers
                if (!available_layer_display_names.empty()) {
                    ImGui::Spacing();
                    ImGui::Text("Layer Selection");
                    ImGui::Separator();

                    ImGui::SetNextItemWidth(250.0f);
                    if (font_regular) ImGui::PushFont(font_regular);
                    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.07f, 0.07f, 1.00f));
                    if (ImGui::BeginCombo("##layer_selector", available_layer_display_names[current_layer_index].c_str())) {
                        for (int i = 0; i < (int)available_layer_display_names.size(); i++) {
                            bool is_selected = (current_layer_index == i);
                            if (ImGui::Selectable(available_layer_display_names[i].c_str(), is_selected)) {
                                // Layer changed - update media item and reload
                                if (i != current_layer_index) {
                                    std::string item_id = item->id;

                                    Debug::Log("EXR Layer Changed:");
                                    Debug::Log("  Old layer: " + item->exr_layer_display);
                                    Debug::Log("  New layer: " + available_layer_display_names[i]);

                                    item->exr_layer = available_layer_names[i];
                                    item->exr_layer_display = available_layer_display_names[i];
                                    item->image_seq.layer = available_layer_names[i];
                                    item->image_seq.layer_display = available_layer_display_names[i];

                                    // Update path with new layer
                                    std::string base_path = item->path;
                                    size_t query_pos = base_path.find("?layer=");
                                    if (query_pos != std::string::npos) {
                                        base_path = base_path.substr(0, query_pos);
                                    }
                                    item->path = base_path + "?layer=" + item->exr_layer;

                                    // Update item name to reflect new layer
                                    std::string old_name = item->name;
                                    size_t bracket_pos = old_name.find(" [");
                                    if (bracket_pos != std::string::npos) {
                                        std::string base_name = old_name.substr(0, bracket_pos);
                                        item->name = base_name + " [" +
                                            std::to_string(item->frame_count) + " frames " +
                                            std::to_string(item->start_frame) + "-" +
                                            std::to_string(item->end_frame) + "] - " +
                                            item->exr_layer_display;
                                    }

                                    // Update the bin copy
                                    for (auto& bin : bins) {
                                        for (auto& bin_item : bin.items) {
                                            if (bin_item.id == item_id) {
                                                bin_item.exr_layer = available_layer_names[i];
                                                bin_item.exr_layer_display = available_layer_display_names[i];
                                                bin_item.image_seq.layer = available_layer_names[i];
                                                bin_item.image_seq.layer_display = available_layer_display_names[i];
                                                bin_item.path = item->path;
                                                bin_item.name = item->name;
                                                break;
                                            }
                                        }
                                    }

                                    // Reload the sequence with new layer
                                    LoadSingleMediaItem(*item);
                                }
                            }
                            if (is_selected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::PopStyleColor();
                    if (font_regular) ImGui::PopFont();
                }
            }

            // === EXR LAYERS SECTION ===
            if (!first_exr.empty()) {
                EXRLayerDetector detector;
                std::vector<EXRLayer> layers;
                if (detector.DetectLayers(first_exr, layers) && !layers.empty()) {
                    ImGui::Spacing();
                    ImGui::Text("EXR Layers");
                    ImGui::Separator();

                    // Layer summary
                        ImGui::TextDisabled("%d layer%s detected", (int)layers.size(), layers.size() > 1 ? "s" : "");
                        ImGui::Spacing();

                    // Get selected layer for highlighting
                    std::string selected_layer = !item->image_seq.layer.empty() ? item->image_seq.layer :
                                                 (!item->exr_layer.empty() ? item->exr_layer : "");

                    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));
                    if (ImGui::BeginTable("EXRLayersTable", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
                        ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthFixed, 200.0f);
                        ImGui::TableSetupColumn("Channels", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                        ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                        ImGui::TableHeadersRow();

                        for (const auto& layer : layers) {
                            ImGui::TableNextRow();

                            // Layer name
                            ImGui::TableSetColumnIndex(0);
                            bool is_selected = (layer.name == selected_layer) ||
                                               (layer.is_default && selected_layer.empty());
                                        if (is_selected) {
                                ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s", layer.display_name.c_str());
                            } else {
                                ImGui::Text("%s", layer.display_name.c_str());
                            }
            
                            // Channel types (RGB, RGBA, etc.)
                            ImGui::TableSetColumnIndex(1);
                                        std::string channel_types;
                            if (layer.has_rgba) {
                                channel_types = layer.has_alpha ? "RGBA" : "RGB";
                            } else {
                                // Build from channel names
                                for (const auto& ch : layer.channels) {
                                    std::string ch_name = ch.name;
                                    size_t dot_pos = ch_name.rfind('.');
                                    if (dot_pos != std::string::npos) {
                                        ch_name = ch_name.substr(dot_pos + 1);
                                    }
                                    channel_types += ch_name;
                                }
                            }
                            ImGui::Text("%s", channel_types.c_str());
            
                            // Pixel type (half/float)
                            ImGui::TableSetColumnIndex(2);
                                        std::string pixel_type = "half";
                            if (!layer.channels.empty()) {
                                pixel_type = layer.channels[0].pixel_type;
                            }
                            ImGui::Text("%s", pixel_type.c_str());
            
                            // Channel count
                            ImGui::TableSetColumnIndex(3);
                                        ImGui::Text("%d", (int)layer.channels.size());
                                    }

                        ImGui::EndTable();
                    }
                    ImGui::PopStyleVar();
                }
            }
        }
    }

    // ============================================================================
    // METADATA DISPLAY HELPERS
    // ============================================================================

    void ProjectManager::DisplayFileInfoTable(const VideoMetadata* video_meta) {
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));
        if (ImGui::BeginTable("FileInfoTable", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            // File Name
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("Name:");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", video_meta->file_name.c_str());

            // File Path
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("Path:");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", video_meta->file_path.c_str());
            RenderPathButtons(video_meta->file_path, "FilePath");

            // File Size
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("Size:");
            ImGui::TableSetColumnIndex(1);
            if (video_meta->file_size > 0) {
                double size_mb = video_meta->file_size / (1024.0 * 1024.0);
                if (size_mb >= 1024.0) {
                    ImGui::Text("%.2f GB", size_mb / 1024.0);
                }
                else {
                    ImGui::Text("%.2f MB", size_mb);
                }
            }
            else {
                ImGui::Text("Unknown");
            }

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();  // CellPadding
    }

    void ProjectManager::DisplayVideoPropertiesTable(const VideoMetadata* video_meta) {
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));
        if (ImGui::BeginTable("VideoPropsTable", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            if (video_meta->width > 0 && video_meta->height > 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Resolution:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%dx%d", video_meta->width, video_meta->height);
            }

            if (video_meta->frame_rate > 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Frame Rate:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f fps", video_meta->frame_rate);
            }

            if (video_meta->total_frames > 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Total Frames:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", video_meta->total_frames);
            }

            // NEW: Display total duration in multiple formats
            if (video_meta->total_frames > 0 && video_meta->frame_rate > 0) {
                double duration_seconds = video_meta->total_frames / video_meta->frame_rate;
                int64_t duration_ms = static_cast<int64_t>(duration_seconds * 1000.0);

                // Calculate timecode (HH:MM:SS:FF) - show last frame's timecode (0-indexed)
                int total_frames_tc = video_meta->total_frames - 1;
                int fps_rounded = static_cast<int>(video_meta->frame_rate + 0.5);
                int frames = total_frames_tc % fps_rounded;
                int total_seconds = total_frames_tc / fps_rounded;
                int seconds = total_seconds % 60;
                int minutes = (total_seconds / 60) % 60;
                int hours = total_seconds / 3600;

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Duration:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3fs | %lldms | %02d:%02d:%02d:%02d",
                    duration_seconds, duration_ms, hours, minutes, seconds, frames);
            }

            if (!video_meta->video_codec.empty() && video_meta->video_codec != "Unknown") {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Video Codec:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", video_meta->video_codec.c_str());
            }

            if (!video_meta->pixel_format.empty() && video_meta->pixel_format != "Unknown") {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Pixel Format:");
                ImGui::TableSetColumnIndex(1);

                // Display pixel format with chroma subsampling indicator
                if (video_meta->is_411_format) {
                    ImGui::Text("%s ", video_meta->pixel_format.c_str());
                    ImGui::SameLine();
                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "(4:1:1)");
                } else if (video_meta->is_421_format) {
                    ImGui::Text("%s ", video_meta->pixel_format.c_str());
                    ImGui::SameLine();
                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "(4:2:1)");
                } else {
                    ImGui::Text("%s", video_meta->pixel_format.c_str());
                }

            }

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();  // CellPadding
    }

    void ProjectManager::DisplayColorPropertiesTable(const VideoMetadata* video_meta) {
        // Lazy NCLC detection - only compute when color properties are being displayed
        if (video_meta->nclc_tag.empty()) {
            const_cast<VideoMetadata*>(video_meta)->DetectAndCacheNCLC();
        }

        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));
        if (ImGui::BeginTable("ColorPropsTable", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            if (!video_meta->colorspace.empty() && video_meta->colorspace != "Unknown") {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Colorspace:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", video_meta->colorspace.c_str());
            }

            if (!video_meta->color_primaries.empty() && video_meta->color_primaries != "Unknown") {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Primaries:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", video_meta->color_primaries.c_str());
            }

            if (!video_meta->color_transfer.empty() && video_meta->color_transfer != "Unknown") {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Transfer:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", video_meta->color_transfer.c_str());
            }

            // NEW: Display color range
            if (!video_meta->range_type.empty() && video_meta->range_type != "unknown") {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Range:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", video_meta->range_type.c_str());
            }

            // NEW: Display NCLC tag (ColorPrimaries-TransferCharacteristics-MatrixCoefficients)
            if (!video_meta->nclc_tag.empty() && video_meta->nclc_tag != "Unknown") {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("NCLC Tag:");
                ImGui::TableSetColumnIndex(1);

                // Highlight common NCLC tags
                if (video_meta->nclc_tag == "1-1-1") {
                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s (BT.709)", video_meta->nclc_tag.c_str());
                } else if (video_meta->nclc_tag == "1-2-1") {
                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s (BT.709 Unspec)", video_meta->nclc_tag.c_str());
                } else if (video_meta->nclc_tag == "9-16-9") {
                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s (BT.2020 PQ)", video_meta->nclc_tag.c_str());
                } else if (video_meta->nclc_tag == "9-18-9") {
                    ImGui::TextColored(Bright(GetWindowsAccentColor()), "%s (BT.2020 HLG)", video_meta->nclc_tag.c_str());
                } else {
                    ImGui::Text("%s", video_meta->nclc_tag.c_str());
                }

            }

            // NEW: Display color matrix status for 4444 formats
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("Processing:");
            ImGui::TableSetColumnIndex(1);

            // Debug: Log when this is called
            bool is_4444 = video_meta->Is4444Format();
           /* Debug::Log("Inspector: Is4444Format() returned " + std::string(is_4444 ? "true" : "false") +
                      " for pixel_format: '" + video_meta->pixel_format + "'");*/
            if (is_4444) {
                ImGui::TextColored(Bright(GetWindowsAccentColor()), "4444 Color Matrix Applied");
            } else {
                ImGui::TextDisabled("Standard Processing");
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();  // CellPadding
    }

    void ProjectManager::DisplayAudioPropertiesTable(const VideoMetadata* video_meta) {
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));
        if (ImGui::BeginTable("AudioPropsTable", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            if (!video_meta->audio_codec.empty() && video_meta->audio_codec != "Unknown") {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Audio Codec:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", video_meta->audio_codec.c_str());
            }

            if (video_meta->audio_sample_rate > 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Sample Rate:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d Hz", video_meta->audio_sample_rate);
            }

            if (video_meta->audio_channels > 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("Channels:");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", video_meta->audio_channels);
            }

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();  // CellPadding
    }

    void ProjectManager::DisplayAdobeProjectsTable(const AdobeMetadata* adobe_meta) {
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));
        if (ImGui::BeginTable("AdobeProjectsTable", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("Application", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Project File", ImGuiTableColumnFlags_WidthStretch);

            if (!adobe_meta->ae_project_path.empty()) {
                DisplayAdobeProjectRow("After Effects:", adobe_meta->ae_project_path, "AE");
            }

            if (!adobe_meta->premiere_win_path.empty()) {
                DisplayAdobeProjectRow("Premiere (Win):", adobe_meta->premiere_win_path, "PR");
            }

            if (!adobe_meta->premiere_mac_path.empty()) {
                DisplayAdobeProjectRow("Premiere (Mac):", adobe_meta->premiere_mac_path, "PRM");
            }

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();  // CellPadding
    }

    void ProjectManager::DisplayAdobeProjectRow(const std::string& app_name, const std::string& project_path, const std::string& button_suffix) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("%s", app_name.c_str());

        ImGui::TableSetColumnIndex(1);
        std::string filename = std::filesystem::path(project_path).filename().string();
        ImVec4 color = Bright(GetWindowsAccentColor());
        ImGui::TextColored(color, "%s", filename.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", project_path.c_str());
        }
        // Buttons below the filename in the same row
        // Invert button background based on row alternation
        int row_idx = ImGui::TableGetRowIndex();
        bool is_alt_row = (row_idx % 2) == 1;
        ImGui::PushStyleColor(ImGuiCol_Button, is_alt_row ? ImVec4(0.141f, 0.141f, 0.141f, 1.0f) : ImVec4(0.192f, 0.192f, 0.192f, 1.0f));
        if (button_suffix != "PRM") { // Mac paths might not work with Windows Explorer
            std::string open_button = "Open##" + button_suffix;
            if (ImGui::SmallButton(open_button.c_str())) {
                OpenFileInExplorer(project_path);
            }
            ImGui::SameLine();
        }
        std::string copy_button = "Copy##" + button_suffix;
        if (ImGui::SmallButton(copy_button.c_str())) {
            CopyToClipboard(project_path);
        }
        ImGui::PopStyleColor();
    }

    // ============================================================================
    // UTILITY FUNCTIONS
    // ============================================================================

    double ProjectManager::GetTimelineDuration() const {
        return video_player ? video_player->GetDuration() : 0.0;
    }

    double ProjectManager::GetTimelinePosition() const {
        return video_player ? video_player->GetPosition() : 0.0;
    }

    void ProjectManager::CreateNewBin(const std::string& name) {
        ProjectBin new_bin;
        new_bin.name = name.empty() ? ("Bin " + std::to_string(bins.size() + 1)) : name;
        new_bin.is_open = true;
        bins.push_back(new_bin);
    }

    std::string ProjectManager::GenerateUniqueID() {
        // Use counter + random suffix to guarantee uniqueness even across sessions
        // This eliminates the fragile N+1 logic that required UpdateIDCounter()
        static int counter = 0;
        static const char alphanum[] = "0123456789abcdefghijklmnopqrstuvwxyz";
        static std::random_device rd;
        static std::mt19937 gen(rd());
        static std::uniform_int_distribution<> dis(0, sizeof(alphanum) - 2);

        std::string suffix;
        for (int i = 0; i < 6; ++i) {
            suffix += alphanum[dis(gen)];
        }
        return "item_" + std::to_string(++counter) + "_" + suffix;
    }

    void ProjectManager::UpdateIDCounter() {
        // No longer needed - IDs now include random suffix for guaranteed uniqueness
        // Kept as no-op for API compatibility
        Debug::Log("UpdateIDCounter: No-op (IDs now use random suffix for uniqueness)");
    }

    std::string ProjectManager::GetProjectName(const std::string& path) {
        std::filesystem::path file_path(path);
        return file_path.stem().string();
    }

    std::string ProjectManager::GetFileName(const std::string& path) {
        std::filesystem::path file_path(path);
        return file_path.filename().string();
    }

    bool ProjectManager::WaitForFileReadable(const std::string& file_path, int timeout_seconds) {
        Debug::Log("WaitForFileReadable: Waiting for file to become readable: " + file_path);

        const auto start_time = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::seconds(timeout_seconds);
        const int check_interval_ms = 500;  // Check every 500ms
        int attempt = 0;

        while (std::chrono::steady_clock::now() - start_time < timeout) {
            attempt++;

            // Try to open file for reading
            std::ifstream file(file_path, std::ios::binary);
            if (file.is_open()) {
                // Successfully opened - check if we can read at least 1 byte
                char byte;
                if (file.read(&byte, 1)) {
                    file.close();
                    if (attempt > 1) {
                        Debug::Log("WaitForFileReadable: File became readable after " + std::to_string(attempt) + " attempts");
                    }
                    return true;
                }
                file.close();
            }

            if (attempt == 1) {
                Debug::Log("WaitForFileReadable: File not yet readable (may be syncing from cloud)");
            }

            // Wait before next attempt
            std::this_thread::sleep_for(std::chrono::milliseconds(check_interval_ms));
        }

        Debug::Log("WaitForFileReadable: File did not become readable after " + std::to_string(timeout_seconds) + " seconds");
        return false;
    }

    MediaType ProjectManager::GetMediaType(const std::string& path) const {
        std::filesystem::path file_path(path);
        std::string ext = file_path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        // Video formats
        if (ext == ".mp4" || ext == ".mov" || ext == ".avi" || ext == ".mxf" ||
            ext == ".mkv" || ext == ".wmv" || ext == ".flv" || ext == ".webm" ||
            ext == ".m4v" || ext == ".3gp" || ext == ".ogv" || ext == ".ts" ||
            ext == ".mts" || ext == ".m2ts" || ext == ".gif") {
            return MediaType::VIDEO;
        }

        // Audio formats
        if (ext == ".wav" || ext == ".mp3" || ext == ".aac" || ext == ".flac" ||
            ext == ".ogg" || ext == ".wma" || ext == ".m4a" || ext == ".aiff" || ext == ".aif") {
            return MediaType::AUDIO;
        }

        // Image formats - check if it's part of a sequence
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp" ||
            ext == ".tiff" || ext == ".tif" || ext == ".tga" || ext == ".hdr" || ext == ".dpx") {

            // Check if this looks like part of an image sequence
            if (IsPartOfImageSequence(path)) {
                return MediaType::IMAGE_SEQUENCE;
            }
            return MediaType::IMAGE;
        }

        // EXR files - special handling for multi-layer support
        if (ext == ".exr") {
            // Check if this looks like part of an image sequence
            if (IsPartOfImageSequence(path)) {
                return MediaType::EXR_SEQUENCE;
            }
            return MediaType::IMAGE;
        }

        return MediaType::VIDEO; // Default fallback
    }

    int ProjectManager::GetBinIndexForMediaType(MediaType type) {
        switch (type) {
        case MediaType::VIDEO: return 0;
        case MediaType::AUDIO: return 1;
        case MediaType::IMAGE: return 2;
        case MediaType::IMAGE_SEQUENCE: return 2; // Put image sequences in Images bin as originally intended
        case MediaType::EXR_SEQUENCE: return 2; // Put EXR sequences in Images bin as originally intended
        case MediaType::DUAL_VIEW: return DUAL_VIEWS_BIN_INDEX;
        case MediaType::PLAYLIST: return PLAYLISTS_BIN_INDEX;
        default: return 0;
        }
    }

    double ProjectManager::GetDefaultDurationForType(MediaType type) {
        switch (type) {
        case MediaType::VIDEO: return 30.0;
        case MediaType::AUDIO: return 180.0;
        case MediaType::IMAGE: return 1.0;
        case MediaType::IMAGE_SEQUENCE: return 10.0; // Default 10 seconds, will be calculated based on frame count and rate
        case MediaType::EXR_SEQUENCE: return 10.0; // Default 10 seconds, will be calculated based on frame count and rate
        default: return 30.0;
        }
    }

    std::vector<std::string> ProjectManager::ParsePayloadString(const std::string& payload_string) {
        std::vector<std::string> media_ids;
        size_t pos = 0;
        std::string payload_copy = payload_string;

        while (pos < payload_copy.length()) {
            size_t next_pos = payload_copy.find(';', pos);
            if (next_pos == std::string::npos) {
                std::string media_id = payload_copy.substr(pos);
                if (!media_id.empty()) {
                    media_ids.push_back(media_id);
                }
                break;
            }
            else {
                std::string media_id = payload_copy.substr(pos, next_pos - pos);
                if (!media_id.empty()) {
                    media_ids.push_back(media_id);
                }
                pos = next_pos + 1;
            }
        }
        return media_ids;
    }

    bool ProjectManager::HasColorInfo(const VideoMetadata* video_meta) {
        return (!video_meta->colorspace.empty() && video_meta->colorspace != "Unknown") ||
            (!video_meta->color_primaries.empty() && video_meta->color_primaries != "Unknown") ||
            (!video_meta->color_transfer.empty() && video_meta->color_transfer != "Unknown");
    }

    bool ProjectManager::HasAudioInfo(const VideoMetadata* video_meta) {
        return (!video_meta->audio_codec.empty() && video_meta->audio_codec != "Unknown") ||
            video_meta->audio_sample_rate > 0 || video_meta->audio_channels > 0;
    }

    bool ProjectManager::IsAudioOnlyFile(const VideoMetadata* video_meta) {
        // Audio-only file: has audio info but no video dimensions
        return HasAudioInfo(video_meta) && (video_meta->width == 0 || video_meta->height == 0);
    }

    void ProjectManager::OpenFileInExplorer(const std::string& file_path) {
        // Delegate to the main ShowInExplorer implementation (non-blocking + handles special URLs)
        ShowInExplorer(file_path);
    }

    void ProjectManager::CopyToClipboard(const std::string& text) {
        ImGui::SetClipboardText(text.c_str());
    }

    void ProjectManager::RenderPathButtons(const std::string& path, const char* id_suffix) {
        if (path.empty()) return;

        // Ensure unique IDs for each button set
        std::string open_id = std::string("Open##") + id_suffix;
        std::string copy_id = std::string("Copy##") + id_suffix;

        // Respect alt row button color logic
        int row_idx = ImGui::TableGetRowIndex();
        bool is_alt_row = (row_idx % 2) == 1;
        ImGui::PushStyleColor(ImGuiCol_Button, is_alt_row ? ImVec4(0.141f, 0.141f, 0.141f, 1.0f) : ImVec4(0.192f, 0.192f, 0.192f, 1.0f));

        if (ImGui::SmallButton(open_id.c_str())) {
            ShowInExplorer(path);
        }
        ImGui::SameLine();

        if (ImGui::SmallButton(copy_id.c_str())) {
            CopyToClipboard(path);
        }

        ImGui::PopStyleColor();
    }

    // ============================================================================
    // VIDEO CACHE MANAGEMENT (ProjectManager interface)
    // ============================================================================

    FrameCache* ProjectManager::GetCurrentVideoCache() const {
        if (!video_cache_manager || !current_file_path || current_file_path->empty()) {
            return nullptr;
        }

        // Skip cache for audio files (no video frames to cache)
        MediaType media_type = GetMediaType(*current_file_path);
        if (media_type == MediaType::AUDIO) {
            return nullptr;
        }

        return video_cache_manager->GetCacheForVideo(*current_file_path);
    }

    bool ProjectManager::GetCachedFrame(double timestamp, GLuint& texture_id, int& width, int& height) {
        if (!cache_enabled || !video_cache_manager || !current_file_path || current_file_path->empty()) {
            return false;
        }
        return video_cache_manager->GetCachedFrame(*current_file_path, timestamp, texture_id, width, height);
    }

    void ProjectManager::NotifyVideoChanged(const std::string& video_path) {
        // Skip FFMPEG-based FrameCache for image sequences - they use DirectEXRCache only
        // NOTE: Do NOT clear caches here! The DirectEXRCache was just created by the load function.
        // Cache clearing happens at the START of load functions, not in this callback after loading.
        if (video_path.substr(0, 6) == "exr://") {
            Debug::Log("ProjectManager: Skipping FFMPEG cache for EXR sequence (uses DirectEXRCache)");
            return;
        }

        // Skip FFMPEG-based FrameCache for TIFF/PNG/JPEG image sequences - they use DirectEXRCache
        // NOTE: Do NOT clear caches here! The DirectEXRCache was just created by the load function.
        // Cache clearing happens at the START of load functions, not in this callback after loading.
        if (video_path.substr(0, 5) == "mf://") {
            Debug::Log("ProjectManager: Skipping FFMPEG cache for TIFF/PNG/JPEG image sequence (uses DirectEXRCache with universal loaders)");
            return;
        }

        // Skip frame cache for audio-only files (no video frames to cache)
        MediaType media_type = GetMediaType(video_path);
        if (media_type == MediaType::AUDIO) {
            Debug::Log("ProjectManager: Skipping FrameCache for audio file (no video frames)");
            return;
        }

        // Re-enable cache if it was auto-disabled for previous video's codec
        // BUT first check if we already have metadata for the new video to avoid re-enabling for H.264/H.265
        if (cache_auto_disabled_for_codec) {
            Debug::Log("NotifyVideoChanged: Cache was previously auto-disabled, checking new video: " + video_path);
            const CombinedMetadata* cached_meta = GetCachedMetadata(video_path);

            if (cached_meta && cached_meta->video_meta && cached_meta->video_meta->is_loaded) {
                // We have metadata - check codec before re-enabling
                std::string codec = cached_meta->video_meta->video_codec;
                Debug::Log("NotifyVideoChanged: Found cached metadata, codec = '" + codec + "'");
                std::transform(codec.begin(), codec.end(), codec.begin(), ::tolower);

                // Skip H.264 check for image sequences - they always use DirectEXRCache
                bool is_image_sequence = (video_path.substr(0, 5) == "mf://") ||
                                         (video_path.substr(0, 6) == "exr://");

                bool is_h264_h265 = !is_image_sequence && (codec.find("h264") != std::string::npos ||
                                     codec.find("hevc") != std::string::npos ||
                                     codec.find("h265") != std::string::npos ||
                                     codec.find("avc") != std::string::npos);

                if (is_h264_h265) {
                    Debug::Log("=== CACHED H.264/H.265 DETECTED ===");
                    Debug::Log("Codec: " + codec);
                    Debug::Log("Cache remains disabled (not re-enabling for H.264/H.265)");
                    current_video_codec = cached_meta->video_meta->video_codec;
                    // Keep cache_auto_disabled_for_codec = true
                } else {
                    Debug::Log("=== NEW MEDIA LOADED ===");
                    Debug::Log("Previous video codec: " + current_video_codec);
                    Debug::Log("New codec: " + codec);
                    Debug::Log("Restoring cache to user preference (" + std::string(user_cache_preference ? "enabled" : "disabled") + ") for non-H.264/H.265 media");
                    SetCacheEnabled(user_cache_preference);
                    cache_auto_disabled_for_codec = false;
                    current_video_codec = "";
                }
            } else {
                // No cached metadata - restore user preference and check codec when metadata arrives
                Debug::Log("=== NEW MEDIA LOADED ===");
                Debug::Log("Previous video codec: " + current_video_codec);
                Debug::Log("Restoring cache to user preference (" + std::string(user_cache_preference ? "enabled" : "disabled") + ") for new media (will check codec when metadata arrives)");
                SetCacheEnabled(user_cache_preference);
                cache_auto_disabled_for_codec = false;
                current_video_codec = "";
            }
        }

        // Skip seek cache (FrameCache) entirely for image sequences - they use DirectEXRCache
        bool is_image_sequence = (video_path.substr(0, 5) == "mf://") ||
                                 (video_path.substr(0, 6) == "exr://");

        if (is_image_sequence) {
            Debug::Log("ProjectManager: Skipping FFMPEG cache for image sequence (uses DirectEXRCache)");
        } else if (cache_enabled && video_cache_manager) {
            // NEW FLOW: Get metadata BEFORE initializing cache (metadata extracted by FFmpeg before MPV load)
            const CombinedMetadata* cached_meta = GetCachedMetadata(video_path);

            if (cached_meta && cached_meta->video_meta && cached_meta->video_meta->is_loaded) {
                Debug::Log("ProjectManager: Found cached metadata for " + video_path + ", checking codec");

                // Check if this is H.264/H.265 and disable cache if needed
                std::string codec = cached_meta->video_meta->video_codec;
                std::string codec_lower = codec;
                std::transform(codec_lower.begin(), codec_lower.end(), codec_lower.begin(), ::tolower);
                current_video_codec = codec;

                if (codec_lower.find("h264") != std::string::npos ||
                    codec_lower.find("hevc") != std::string::npos ||
                    codec_lower.find("h265") != std::string::npos ||
                    codec_lower.find("avc") != std::string::npos) {

                    Debug::Log("=== H.264/H.265 DETECTED (from cached metadata) ===");
                    Debug::Log("Codec: " + current_video_codec);
                    Debug::Log("Disabling cache for B-frame safety");

                    SetCacheEnabled(false);
                    ClearAllCaches();  // Clear ALL caches, not just current
                    cache_auto_disabled_for_codec = true;

                    Debug::Log("Cache disabled and ALL caches cleared for: " + video_path);
                } else {
                    // Safe codec - initialize cache with metadata IMMEDIATELY
                    Debug::Log("ProjectManager: Safe codec, initializing cache with metadata");
                    video_cache_manager->NotifyVideoChanged(video_path, video_player);

                    // Apply metadata IMMEDIATELY after cache init (not conditionally later)
                    video_cache_manager->UpdateVideoMetadata(video_path, *cached_meta->video_meta);
                    Debug::Log("ProjectManager: Metadata applied to cache for: " + video_path);
                }
            } else {
                // No cached metadata yet - initialize cache without metadata
                // (metadata will be applied when ProcessVideoMetadata is called)
                Debug::Log("ProjectManager: No cached metadata, initializing cache without metadata");
                video_cache_manager->NotifyVideoChanged(video_path, video_player);
            }
        }
    }

    void ProjectManager::SetCacheConfig(const FrameCache::CacheConfig& config) {
        if (video_cache_manager) {
            video_cache_manager->SetCacheConfig(config);
        }
    }


    void ProjectManager::RemoveVideoFromCache(const std::string& video_path) {
        if (video_cache_manager) {
            video_cache_manager->RemoveCacheForVideo(video_path);
        }
    }

    void ProjectManager::ClearAllCaches() {
        if (video_cache_manager) {
            // Get all cached video paths and remove each one
            std::vector<std::string> cached_paths = video_cache_manager->GetAllCachedVideoPaths();

            // Remove all caches
            for (const auto& path : cached_paths) {
                video_cache_manager->RemoveCacheForVideo(path);
            }
        }

        // Clear EXR cache if active (BUT NOT for image sequences!)
        // Image sequences use DirectEXRCache as their PRIMARY frame source
        // Clearing it would break viewport rendering completely
        if (video_player && current_file_path) {
            bool is_image_sequence = (current_file_path->substr(0, 5) == "mf://") ||
                                     (current_file_path->substr(0, 6) == "exr://");

            if (!is_image_sequence) {
                video_player->ClearEXRCache();
                Debug::Log("ProjectManager: Cleared EXR cache");
            } else {
                Debug::Log("ProjectManager: Skipping EXR cache clear (image sequence uses it for frames)");
            }
        }
    }

    // ClearCurrentVideoCache() removed - use ClearAllCaches() instead
    // State management only allows one video cached, so "current" vs "all" is redundant

    void ProjectManager::RestartCache() {
        if (!video_cache_manager || !current_file_path || current_file_path->empty()) {
            Debug::Log("ProjectManager: Cannot restart cache - no video loaded or cache manager unavailable");
            return;
        }

        Debug::Log("ProjectManager: Performing cache restart for: " + *current_file_path);

        // Enable caching
        cache_enabled = true;
        video_cache_manager->SetCachingEnabled(true);

        // Check if cache exists AND is initialized
        // GetCacheForVideo() creates empty cache if it doesn't exist, so we need to check initialization
        FrameCache* cache = video_cache_manager->GetCacheForVideo(*current_file_path);
        bool cache_needs_init = (!cache || !cache->IsInitialized());

        if (cache_needs_init) {
            Debug::Log("ProjectManager: Cache not initialized (was destroyed by Clear Cache or new), initializing...");

            // Initialize cache by calling NotifyVideoChanged (same as initial load)
            // This will also set current_video_path and call SetVideoFile()
            video_cache_manager->NotifyVideoChanged(*current_file_path, video_player);

            // Apply metadata if we have it cached
            const CombinedMetadata* cached_meta = GetCachedMetadata(*current_file_path);
            if (cached_meta && cached_meta->video_meta && cached_meta->video_meta->is_loaded) {
                Debug::Log("ProjectManager: Applying cached metadata to initialized cache");
                video_cache_manager->UpdateVideoMetadata(*current_file_path, *cached_meta->video_meta);
            }
        } else {
            // Cache exists and is initialized - just clear and restart
            Debug::Log("ProjectManager: Cache initialized, clearing and restarting extraction");
            video_cache_manager->SetCurrentVideo(*current_file_path);
            video_cache_manager->RestartCurrentVideoCache();
        }

        // Restart EXR cache if active
        if (video_player && video_player->HasEXRCache()) {
            Debug::Log("ProjectManager: Restarting EXR cache");
            const auto& exr_files = video_player->GetEXRSequenceFiles();
            std::string exr_layer = video_player->GetEXRLayerName();
            double exr_fps = video_player->GetEXRFrameRate();

            if (!exr_files.empty()) {
                video_player->ClearEXRCache();
                video_player->InitializeEXRCache(exr_files, exr_layer, exr_fps);
                Debug::Log("ProjectManager: EXR cache restarted with " + std::to_string(exr_files.size()) + " frames");
            }
        }

        Debug::Log("ProjectManager: Cache restart completed");
    }

    void ProjectManager::SetCacheEnabled(bool enabled) {
        bool was_enabled = cache_enabled;
        cache_enabled = enabled;

        // EXR PATTERN: Just set the flag, threads keep running
        if (video_cache_manager) {
            video_cache_manager->SetCachingEnabled(enabled);
        }

        // EXR cache is now controlled by VideoDisplayComponent/TimelinePlaybackController
        // The timeline automatically manages cache based on current media

        // === AUTO-CLEAR WHEN DISABLING ===
        // User expects cache to be cleared when they disable it
        if (!enabled && was_enabled) {
            ClearAllCaches();
            Debug::Log("ProjectManager: Cache disabled and cleared");
        }

        // === AUTO-RELOAD WHEN ENABLING ===
        // Explicitly reload current video to properly initialize cache
        // NotifyVideoChanged alone doesn't work - need full reload
        if (enabled && !was_enabled && current_file_path && !current_file_path->empty()) {
            MediaItem* item = GetMediaItemFromCurrentPath();
            if (item) {
                Debug::Log("ProjectManager: Cache enabled, reloading video: " + item->name);
                LoadSingleMediaItem(*item);
            } else {
                // Fallback: just reinitialize cache without reload
                Debug::Log("ProjectManager: Cache enabled, no MediaItem found, using NotifyVideoChanged");
                NotifyVideoChanged(*current_file_path);
            }
        }
    }

    void ProjectManager::SetUserCachePreference(bool enabled) {
        user_cache_preference = enabled;
        Debug::Log("ProjectManager: User cache preference updated to " + std::string(enabled ? "enabled" : "disabled"));

        // Apply immediately if not auto-disabled for codec
        if (!cache_auto_disabled_for_codec) {
            SetCacheEnabled(enabled);
        }
    }

    bool ProjectManager::IsCacheEnabled() const {
        return cache_enabled;
    }

    FrameCache::CacheStats ProjectManager::GetCacheStats() const {
        if (video_cache_manager) {
            return video_cache_manager->GetTotalStats();
        }
        return FrameCache::CacheStats{};
    }

    std::vector<FrameCache::CacheSegment> ProjectManager::GetCacheSegments() const {
        if (!current_file_path || current_file_path->empty()) {
            return std::vector<FrameCache::CacheSegment>();
        }

        // Check if current video player is in EXR mode
        if (video_player && video_player->IsInEXRMode()) {
            // Use EXR cache segments instead of regular video cache (convert types)
            auto exr_segments = video_player->GetEXRCacheSegments();
            std::vector<FrameCache::CacheSegment> result;
            result.reserve(exr_segments.size());
            for (const auto& seg : exr_segments) {
                FrameCache::CacheSegment fc_seg;
                fc_seg.start_time = seg.start_time;
                fc_seg.end_time = seg.end_time;
                fc_seg.density = seg.density;
                fc_seg.type = FrameCache::CacheSegment::SCRUB_CACHE;
                result.push_back(fc_seg);
            }
            return result;
        }

        // Original regular video cache logic
        if (video_cache_manager) {
            return video_cache_manager->GetCacheSegments(*current_file_path);
        }

        return std::vector<FrameCache::CacheSegment>();
    }

    void ProjectManager::NotifyPlaybackState(bool is_playing) {
        if (video_cache_manager) {
            video_cache_manager->NotifyPlaybackState(is_playing);
        }
    }

    // Note: TryOpportunisticCaching() removed - using window-based extraction only


    // ============================================================================
    // VIDEO CACHE IMPLEMENTATION
    // ============================================================================

    VideoCache::VideoCache() {
        // Initialize default config for seconds-based cache management
        default_config.max_cache_seconds = 20; // 20 second default cache window
        default_config.use_centered_caching = true; // Center around seekbar by default
        default_config.cache_width = 1920;
        default_config.cache_height = -1; // Auto-calculate aspect ratio
    }

    VideoCache::~VideoCache() {
        std::lock_guard<std::mutex> lock(cache_mutex);
        video_caches.clear(); // This will destroy all FrameCache instances
    }

    FrameCache* VideoCache::GetCacheForVideo(const std::string& video_path) {
        if (video_path.empty()) return nullptr;

        // Skip FFMPEG-based FrameCache for EXR sequences - they use DirectEXRCache only
        if (video_path.substr(0, 6) == "exr://") {
            return nullptr;
        }

        // Skip cache for audio files (no video frames to cache)
        size_t dot_pos = video_path.find_last_of('.');
        if (dot_pos != std::string::npos) {
            std::string ext = video_path.substr(dot_pos);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".wav" || ext == ".mp3" || ext == ".aac" ||
                ext == ".flac" || ext == ".ogg" || ext == ".wma" || ext == ".m4a" ||
                ext == ".aiff" || ext == ".aif") {
                return nullptr;
            }
        }

        {
            std::lock_guard<std::mutex> lock(cache_mutex);

            auto it = video_caches.find(video_path);
            if (it != video_caches.end()) {
                UpdateLRUOrder(video_path);
                it->second->last_accessed = std::chrono::steady_clock::now();
                return it->second->cache.get();
            }
        }

        // Create new cache for this video OUTSIDE the lock to avoid deadlocks
        auto new_cache = std::make_unique<FrameCache>(default_config);

        // Cache eviction now handled by SetCurrentVideo() when switching videos
        FrameCache* cache_ptr = new_cache.get();

        {
            std::lock_guard<std::mutex> lock(cache_mutex);

            // Double-check that another thread didn't create the cache while we were unlocked
            auto it = video_caches.find(video_path);
            if (it != video_caches.end()) {
                // Another thread created it, return that one and discard ours
                return it->second->cache.get();
            }

            // Store our newly created cache
            auto cache_entry = std::make_unique<VideoCacheEntry>(std::move(new_cache), video_path);
            cache_ptr = cache_entry->cache.get(); // Update pointer after move

            video_caches[video_path] = std::move(cache_entry);
            lru_order.push_front(video_path);

            // Cache eviction now handled by SetCurrentVideo() when switching videos
        }

        return cache_ptr;
    }

    void VideoCache::SetCurrentVideo(const std::string& video_path) {
        std::lock_guard<std::mutex> lock(cache_mutex);

        // Clear cache of previous video to free RAM when switching
        if (!current_video_path.empty() && current_video_path != video_path) {
            Debug::Log("VideoCache: Clearing cache for previous video: " + current_video_path);
            auto it = video_caches.find(current_video_path);
            if (it != video_caches.end()) {
                // Remove from LRU order
                lru_order.remove(current_video_path);
                // Remove from cache map (this will destroy the FrameCache automatically)
                video_caches.erase(it);
            }
        }

        current_video_path = video_path;

        // Update access time for current video
        if (!video_path.empty()) {
            UpdateLRUOrder(video_path);
        }
    }

    bool VideoCache::GetCachedFrame(const std::string& video_path, double timestamp, GLuint& texture_id, int& width, int& height) {
        FrameCache* cache = GetCacheForVideo(video_path);
        if (!cache) return false;
        
        return cache->GetCachedFrame(timestamp, texture_id, width, height);
    }

    void VideoCache::NotifyVideoChanged(const std::string& video_path, VideoPlayer* video_player) {
        SetCurrentVideo(video_path);

        FrameCache* cache = GetCacheForVideo(video_path);
        if (cache && caching_enabled) {
            // Initialize cache with video file - metadata will be passed later via UpdateVideoMetadata
            cache->SetVideoFile(video_path, nullptr);

            // Background caching will now be started by the FrameCache after metadata is available
            // This ensures proper sequencing and eliminates race conditions

            if (video_player) {
                // CRITICAL: Immediately notify cache of current playback state to avoid conflicts
                bool is_playing = video_player->IsPlaying();
                cache->NotifyPlaybackState(is_playing);

                cache->UpdateScrubPosition(video_player->GetPosition(), video_player);
            }

            // Previous video cache is now cleared automatically in SetCurrentVideo()
            // Individual video caches use seconds-based eviction around playhead
        }
    }

    // Note: TryOpportunisticCaching() removed - using window-based extraction only

    void VideoCache::NotifyPlaybackState(bool is_playing) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        
        // Notify all caches, but focus on current video
        for (auto& pair : video_caches) {
            pair.second->cache->NotifyPlaybackState(is_playing);
        }
    }


    void VideoCache::SetCacheConfig(const FrameCache::CacheConfig& config) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        default_config = config;

        // Apply to all existing caches
        for (auto& pair : video_caches) {
            pair.second->cache->SetCacheConfig(config);
        }
    }

    PipelineMode VideoCache::GetPipelineMode() const {
        std::lock_guard<std::mutex> lock(cache_mutex);

        // Get pipeline mode from current video cache
        if (!current_video_path.empty()) {
            auto it = video_caches.find(current_video_path);
            if (it != video_caches.end() && it->second->cache) {
                return it->second->cache->GetPipelineMode();
            }
        }

        // Fallback to default config
        return default_config.pipeline_mode;
    }

    FrameCache::CacheStats VideoCache::GetTotalStats() const {
        std::lock_guard<std::mutex> lock(cache_mutex);
        FrameCache::CacheStats total_stats;
        
        for (const auto& pair : video_caches) {
            auto stats = pair.second->cache->GetStats();
            total_stats.total_frames_cached += stats.total_frames_cached;
            // Removed: Memory usage aggregation (memory-based eviction removed)
            total_stats.cache_hits += stats.cache_hits;
            total_stats.cache_misses += stats.cache_misses;
        }
        
        if (total_stats.cache_hits + total_stats.cache_misses > 0) {
            total_stats.hit_ratio = static_cast<float>(total_stats.cache_hits) / 
                                   (total_stats.cache_hits + total_stats.cache_misses);
        }
        
        return total_stats;
    }

    FrameCache::CacheStats VideoCache::GetStatsForVideo(const std::string& video_path) const {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = video_caches.find(video_path);
        if (it != video_caches.end()) {
            return it->second->cache->GetStats();
        }
        return FrameCache::CacheStats{};
    }

    std::vector<FrameCache::CacheSegment> VideoCache::GetCacheSegments(const std::string& video_path) const {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = video_caches.find(video_path);
        if (it != video_caches.end()) {
            return it->second->cache->GetCacheSegments();
        }
        return std::vector<FrameCache::CacheSegment>();
    }


    void VideoCache::RemoveCacheForVideo(const std::string& video_path) {
        if (video_path.empty()) return;
        
        std::lock_guard<std::mutex> lock(cache_mutex);
        
        auto it = video_caches.find(video_path);
        if (it != video_caches.end()) {
            // Remove from LRU order
            lru_order.remove(video_path);
            
            // Remove from cache map (this will destroy the FrameCache automatically)
            video_caches.erase(it);
            
            // Clear current video if it was the removed one
            if (current_video_path == video_path) {
                current_video_path.clear();
            }
        }
    }


    std::vector<std::string> VideoCache::GetAllCachedVideoPaths() const {
        std::lock_guard<std::mutex> lock(cache_mutex);
        std::vector<std::string> paths;
        for (const auto& entry : video_caches) {
            paths.push_back(entry.first);
        }
        return paths;
    }


    size_t VideoCache::GetCacheCount() const {
        std::lock_guard<std::mutex> lock(cache_mutex);
        return video_caches.size();
    }

    // Private methods
    void VideoCache::UpdateLRUOrder(const std::string& video_path) {
        // Remove from current position and add to front
        lru_order.remove(video_path);
        lru_order.push_front(video_path);
    }

    void VideoCache::EvictOldestCache() {
        if (lru_order.empty()) return;
        
        // Don't evict the current video
        std::string oldest_path;
        for (auto it = lru_order.rbegin(); it != lru_order.rend(); ++it) {
            if (*it != current_video_path) {
                oldest_path = *it;
                break;
            }
        }
        
        if (!oldest_path.empty()) {
            video_caches.erase(oldest_path);
            lru_order.remove(oldest_path);
        }
    }


    void VideoCache::SetCachingEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        caching_enabled = enabled;

        // Enable/disable caching for all video caches
        for (auto& pair : video_caches) {
            pair.second->cache->SetCachingEnabled(enabled);
        }
    }

    // ClearCurrentVideoCache() removed - use ClearAllCaches() instead

    void VideoCache::RestartCurrentVideoCache() {
        std::lock_guard<std::mutex> lock(cache_mutex);

        if (current_video_path.empty()) {
            Debug::Log("VideoCache: RestartCurrentVideoCache - current_video_path is EMPTY, cannot restart");
            return;
        }

        Debug::Log("VideoCache: Restarting cache for: " + current_video_path);

        auto it = video_caches.find(current_video_path);
        if (it != video_caches.end()) {
            // EXR PATTERN: Just clear frames, thread keeps running
            Debug::Log("VideoCache: Found cache, calling ClearCachedFrames()...");
            it->second->cache->ClearCachedFrames();
            Debug::Log("VideoCache: ClearCachedFrames() returned");
        } else {
            Debug::Log("VideoCache: Cache NOT found in video_caches map for: " + current_video_path);
        }
    }

    bool VideoCache::IsCachingEnabled() const {
        std::lock_guard<std::mutex> lock(cache_mutex);
        return caching_enabled;
    }

    void VideoCache::UpdateVideoMetadata(const std::string& video_path, const VideoMetadata& metadata) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = video_caches.find(video_path);
        if (it != video_caches.end() && it->second->cache) {
            it->second->cache->UpdateVideoMetadata(video_path, metadata);
            // Background extraction will be started by FrameCache::UpdateVideoMetadata() after metadata is applied
        }
    }


    // ============================================================================
    // IMAGE SEQUENCE DETECTION
    // ============================================================================

    bool ProjectManager::IsPartOfImageSequence(const std::string& file_path) const {
        try {
            std::filesystem::path path(file_path);
            std::string filename = path.stem().string(); // filename without extension
            std::string directory = path.parent_path().string();
            std::string extension = path.extension().string();

            // Parse from the end backward - look for separator + digits at the end
            // Patterns: file.000012, file_0014, file-0000000000014
            std::regex pattern(R"(^(.+)([_\.\-])(\d+)$)");
            std::smatch match;

            if (!std::regex_match(filename, match, pattern)) {
                // Try pattern without separator (rare case: file000012)
                std::regex no_sep_pattern(R"(^(.+?)(\d{3,})$)"); // Require 3+ digits to avoid false positives
                if (!std::regex_match(filename, match, no_sep_pattern)) {
                    // No numbered pattern - treat as single image (allow loading through sequence dialog)
                    return true;
                }
            }

            std::string base_name = match[1].str();
            std::string separator = (match.size() > 3) ? match[2].str() : ""; // Separator if exists
            std::string number_str = (match.size() > 3) ? match[3].str() : match[2].str();

            // Create pattern to match similar files with same separator and digit length
            std::string search_pattern;
            if (!separator.empty()) {
                // With separator: base + separator + digits
                search_pattern = base_name + R"([_\.\-]\d{)" + std::to_string(number_str.length()) + R"(})";
            } else {
                // Without separator: base + digits (be more specific to avoid false matches)
                search_pattern = base_name + R"(\d{)" + std::to_string(number_str.length()) + R"(})";
            }
            std::regex sequence_pattern(search_pattern);

            int count = 0;
            for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                if (!entry.is_regular_file()) continue;

                std::filesystem::path other_path = entry.path();
                if (other_path.extension() != extension) continue;

                std::string other_filename = other_path.stem().string();
                if (std::regex_match(other_filename, sequence_pattern)) {
                    count++;
                    if (count >= 2) { // Found at least 2 files with the pattern
                        return true;
                    }
                }
            }

            // Allow single images (including those with numbered pattern but only 1 file)
            return true;
        } catch (...) {
            return false; // Any error means not a sequence
        }
    }

    std::vector<std::string> ProjectManager::DetectImageSequence(const std::string& file_path) {
        try {
            std::filesystem::path path(file_path);
            std::string filename = path.stem().string();
            std::string directory = path.parent_path().string();
            std::string extension = path.extension().string();

            // Use same improved pattern as IsPartOfImageSequence
            std::regex pattern(R"(^(.+)([_\.\-])(\d+)$)");
            std::smatch match;

            if (!std::regex_match(filename, match, pattern)) {
                // Try pattern without separator (rare case: file000012)
                std::regex no_sep_pattern(R"(^(.+?)(\d{3,})$)"); // Require 3+ digits to avoid false positives
                if (!std::regex_match(filename, match, no_sep_pattern)) {
                    // No numbered pattern - treat as single image file
                    Debug::Log("DetectImageSequence: Single image detected (no pattern): " + path.string());
                    return {path.string()};
                }
            }

            std::string base_name = match[1].str();
            std::string separator = (match.size() > 3) ? match[2].str() : ""; // Separator if exists
            std::string number_str = (match.size() > 3) ? match[3].str() : match[2].str();

            // Create pattern to match similar files with same separator and digit length
            std::string search_pattern;
            if (!separator.empty()) {
                // With separator: base + separator + digits
                search_pattern = base_name + R"([_\.\-]\d{)" + std::to_string(number_str.length()) + R"(})";
            } else {
                // Without separator: base + digits (be more specific to avoid false matches)
                search_pattern = base_name + R"(\d{)" + std::to_string(number_str.length()) + R"(})";
            }
            std::regex sequence_pattern(search_pattern);

            // Collect all matching files
            std::vector<std::string> sequence_files;

            // Use error_code to avoid exceptions from directory_iterator (cloud placeholders may cause issues)
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
                if (ec) {
                    // Error during iteration - may be cloud sync issue
                    Debug::Log("DetectImageSequence: Directory iteration error - " + ec.message());
                    continue;  // Try to continue with what we have
                }

                // Check if entry exists and is accessible (cloud placeholders may fail this)
                std::error_code entry_ec;
                if (!entry.exists(entry_ec) || entry_ec) {
                    continue;  // Skip files that don't exist or can't be accessed
                }

                if (!entry.is_regular_file(entry_ec) || entry_ec) {
                    continue;  // Skip non-regular files or inaccessible files
                }

                std::filesystem::path other_path = entry.path();
                if (other_path.extension() != extension) continue;

                std::string other_filename = other_path.stem().string();
                if (std::regex_match(other_filename, sequence_pattern)) {
                    sequence_files.push_back(other_path.string());
                }
            }

            // CRITICAL: Sort files alphabetically so frame indices match file order
            // directory_iterator does NOT guarantee order!
            std::sort(sequence_files.begin(), sequence_files.end());

            // Handle single image with numbered pattern (only 1 matching file found)
            if (sequence_files.empty()) {
                Debug::Log("DetectImageSequence: Single image detected (numbered pattern, no other files): " + path.string());
                return {path.string()};
            }

            return sequence_files;
        } catch (const std::exception& e) {
            Debug::Log("DetectImageSequence: Exception - " + std::string(e.what()));
            return {}; // Any error returns empty vector
        } catch (...) {
            Debug::Log("DetectImageSequence: Unknown exception");
            return {}; // Any error returns empty vector
        }
    }

    void ProjectManager::ShowFrameRateDialog(const std::string& sequence_path) {
        Debug::Log("ShowFrameRateDialog called with: " + sequence_path);
        pending_sequence_path = sequence_path;

        // Check if this is an EXR sequence and detect layers
        is_exr_sequence = false;
        is_tiff_png_sequence = false;
        is_single_image = false;
        {
            std::lock_guard<std::mutex> lock(exr_layers_mutex);
            exr_layer_names.clear();
            exr_layer_display_names.clear();
            exr_layer_part_indices.clear();
            selected_exr_layer_index = 0;
        }
        hidden_cryptomatte_count = 0;

        // Detect if this is a single image or a sequence
        std::vector<std::string> sequence_files = DetectImageSequence(sequence_path);
        if (sequence_files.size() == 1) {
            Debug::Log("Single image detected: " + sequence_path);
            is_single_image = true;
        }

        std::filesystem::path path(sequence_path);
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

        if (extension == ".exr") {
            Debug::Log("EXR sequence detected, analyzing layers asynchronously...");
            is_exr_sequence = true;

            // Show dialog immediately with "Detecting layers..." placeholder
            {
                std::lock_guard<std::mutex> lock(exr_layers_mutex);
                exr_layer_names.clear();
                exr_layer_display_names.clear();
                exr_layer_part_indices.clear();
                exr_layer_names.push_back("Detecting layers...");
                exr_layer_display_names.push_back("Detecting layers...");
                exr_layer_part_indices.push_back(0);
                selected_exr_layer_index = 0;
            }

            // Launch async layer detection
            std::string path_copy = sequence_path;
            std::thread([this, path_copy]() {
                EXRLayerDetector detector;
                std::vector<EXRLayer> layers;
                int crypto_count = 0;

                // Get first file from sequence
                std::vector<std::string> exr_files = DetectImageSequence(path_copy);
                if (exr_files.empty()) {
                    std::lock_guard<std::mutex> lock(exr_layers_mutex);
                    exr_layer_names.clear();
                    exr_layer_display_names.clear();
                    exr_layer_part_indices.clear();
                    exr_layer_names.push_back("RGBA");
                    exr_layer_display_names.push_back("RGBA (default)");
                    exr_layer_part_indices.push_back(0);
                    selected_exr_layer_index = 0;
                    Debug::Log("EXR Layer Detection: No sequence files found");
                    return;
                }

                if (detector.DetectLayers(exr_files[0], layers, crypto_count)) {
                    // Update UI data with mutex protection
                    std::lock_guard<std::mutex> lock(exr_layers_mutex);
                    exr_layer_names.clear();
                    exr_layer_display_names.clear();
                    exr_layer_part_indices.clear();
                    hidden_cryptomatte_count = crypto_count;

                    if (!layers.empty()) {
                        for (const EXRLayer& layer : layers) {
                            exr_layer_names.push_back(layer.name);
                            exr_layer_display_names.push_back(layer.name);
                            exr_layer_part_indices.push_back(layer.part_index);

                            if (layer.is_default) {
                                selected_exr_layer_index = exr_layer_names.size() - 1;
                            }
                        }
                        Debug::Log("Found " + std::to_string(layers.size()) + " EXR layers");
                    } else {
                        exr_layer_names.push_back("RGBA");
                        exr_layer_display_names.push_back("RGBA");
                        exr_layer_part_indices.push_back(0);
                        selected_exr_layer_index = 0;
                    }
                } else {
                    // Fallback to default
                    std::lock_guard<std::mutex> lock(exr_layers_mutex);
                    exr_layer_names.clear();
                    exr_layer_display_names.clear();
                    exr_layer_part_indices.clear();
                    exr_layer_names.push_back("RGBA");
                    exr_layer_display_names.push_back("RGBA (default)");
                    exr_layer_part_indices.push_back(0);
                    selected_exr_layer_index = 0;
                    Debug::Log("EXR Layer Detection: Failed, using default RGBA");
                }
            }).detach();
        } else if (extension == ".tif" || extension == ".tiff" || extension == ".png") {
            // TIFF/PNG sequence detected
            Debug::Log("TIFF/PNG sequence detected, checking for transcode eligibility...");
            is_tiff_png_sequence = true;

            // Detect if this is a 16-bit or large resolution sequence that would benefit from transcode
            // For now, we'll show the option for all TIFF/PNG sequences
            // The user can choose whether to transcode based on their needs
        }

        show_frame_rate_dialog = true;
        frame_rate_dialog_opened = false; // Reset flag so popup can open
        Debug::Log("Frame rate dialog state set successfully");
    }

    void ProjectManager::ProcessImageSequence(const std::string& sequence_path, double frame_rate, const std::string& exr_layer) {
        // Exit timeline mode when loading image sequence
        if (exit_timeline_mode_callback) {
            exit_timeline_mode_callback();
        }

        Debug::Log("ProcessImageSequence: Step 1 - Detecting sequence files");

        // Get the full sequence file list
        std::vector<std::string> sequence_files = DetectImageSequence(sequence_path);
        if (sequence_files.empty()) {
            Debug::Log("ProcessImageSequence: No sequence files found");
            return;
        }

        Debug::Log("ProcessImageSequence: Step 2 - Found " + std::to_string(sequence_files.size()) + " files");

        // Extract sequence information from first and last files
        std::filesystem::path first_file(sequence_files[0]);
        std::filesystem::path last_file(sequence_files.back());

        Debug::Log("ProcessImageSequence: Step 3 - Extracted file paths");

        // Parse the first file to understand the naming pattern
        std::string first_filename = first_file.stem().string();
        Debug::Log("ProcessImageSequence: Step 4 - Parsing filename: " + first_filename);

        // Use same improved pattern as IsPartOfImageSequence and DetectImageSequence
        std::regex pattern(R"(^(.+)([_\.\-])(\d+)$)");
        std::smatch match;

        if (!std::regex_match(first_filename, match, pattern)) {
            // Try pattern without separator (rare case: file000012)
            std::regex no_sep_pattern(R"(^(.+?)(\d{3,})$)"); // Require 3+ digits to avoid false positives
            if (!std::regex_match(first_filename, match, no_sep_pattern)) {
                Debug::Log("ProcessImageSequence: Failed to match pattern");
                return;
            }
        }

        std::string base_name = match[1].str();
        std::string separator = (match.size() > 3) ? match[2].str() : ""; // Separator if exists
        std::string first_number = (match.size() > 3) ? match[3].str() : match[2].str();
        Debug::Log("ProcessImageSequence: Step 5 - Parsed base_name=" + base_name + " separator='" + separator + "' number=" + first_number);

        // Parse last file to get the end frame using same logic
        std::string last_filename = last_file.stem().string();
        std::smatch last_match;
        std::string last_number = first_number; // Default to first if parsing fails

        if (std::regex_match(last_filename, last_match, pattern)) {
            last_number = (last_match.size() > 3) ? last_match[3].str() : last_match[2].str();
        } else {
            std::regex no_sep_pattern(R"(^(.+?)(\d{3,})$)");
            if (std::regex_match(last_filename, last_match, no_sep_pattern)) {
                last_number = last_match[2].str();
            }
        }

        int start_frame = std::stoi(first_number);
        int end_frame = std::stoi(last_number);
        Debug::Log("ProcessImageSequence: Step 6 - Frame range: " + std::to_string(start_frame) + " to " + std::to_string(end_frame));

        // Create a more specific MPV pattern
        // MPV supports patterns like: mf://path/sequence_%04d.exr:fps=24
        std::string directory = first_file.parent_path().string();
        std::string extension = first_file.extension().string();
        Debug::Log("ProcessImageSequence: Step 7 - Directory: " + directory);

        // Determine the padding for the sequence
        int padding = static_cast<int>(first_number.length());

        // Create pattern: base_name + separator + %0Xd + extension (where X is padding)
        std::string mf_pattern = base_name + separator + "%0" + std::to_string(padding) + "d" + extension;
        Debug::Log("ProcessImageSequence: Step 8 - MF pattern: " + mf_pattern);

        // Replace backslashes with forward slashes for MPV (cross-platform compatibility)
        std::replace(directory.begin(), directory.end(), '\\', '/');

        // Create MPV MF:// URL using the working C# pattern
        // For complex names like "20250920_1809--ACES2065-1_TIFF", we need to remove the separator + digits
        std::string file_basename = base_name;
        if (!separator.empty()) {
            // Remove the separator from the end if it exists
            if (!file_basename.empty() && file_basename.back() == separator[0]) {
                file_basename.pop_back();
            }
        }

        // C# working format: mf://directory/basename*extension (no fps in URL)
        std::string mf_url = "mf://" + directory + "/" + file_basename + "*" + extension;
        Debug::Log("ProcessImageSequence: Using C# working pattern: " + mf_url);
        Debug::Log("ProcessImageSequence: File basename after regex: '" + file_basename + "'");

        // Alternative: Try first file path to test basic loading
        std::string first_file_path = sequence_files[0];
        Debug::Log("ProcessImageSequence: Step 9b - First file for testing: " + first_file_path);

        // For non-standard start frames, we can add start parameter if MPV supports it
        // For now, we'll let MPV handle the sequence as-is

        // Determine if this is an EXR sequence
        std::filesystem::path seq_path(sequence_path);
        std::string ext = seq_path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        bool is_exr = (ext == ".exr");
        bool is_transcoded_exr = is_exr && exr_layer.empty();  // Transcoded single-layer EXRs have empty layer

        // NEW: Extract dimensions from first frame for EXR sequences (for instant loading later)
        int exr_width = 0, exr_height = 0;
        if (is_exr && !sequence_files.empty()) {
            if (ump::DirectEXRCache::GetFrameDimensions(sequence_files[0], exr_width, exr_height)) {
                Debug::Log("ProcessImageSequence: Cached EXR sequence dimensions: " +
                          std::to_string(exr_width) + "x" + std::to_string(exr_height));
            } else {
                Debug::Log("ProcessImageSequence: WARNING - Could not extract EXR dimensions from first file");
            }
        }

        // Create MediaItem for the sequence
        MediaItem item;
        item.id = GenerateUniqueID();

        if (is_exr && !exr_layer.empty()) {
            // EXR sequence with layer selection (original multi-layer)
            item.name = base_name + " [" + std::to_string(sequence_files.size()) + " frames " +
                       std::to_string(start_frame) + "-" + std::to_string(end_frame) + "] - " + exr_layer;
            item.type = MediaType::EXR_SEQUENCE;
            item.exr_layer = exr_layer;

            // Find display name for layer
            {
                std::lock_guard<std::mutex> lock(exr_layers_mutex);
                for (int i = 0; i < exr_layer_names.size(); i++) {
                    if (exr_layer_names[i] == exr_layer) {
                        item.exr_layer_display = exr_layer_display_names[i];
                        break;
                    }
                }
            }

            // For EXR sequences, we'll use the same path format as current_file_path for consistency
            item.path = "exr://" + sequence_path + "?layer=" + exr_layer;
        } else if (is_transcoded_exr) {
            // Transcoded single-layer EXR (treat as EXR, not regular image sequence)
            item.name = base_name + " [" + std::to_string(sequence_files.size()) + " frames " +
                       std::to_string(start_frame) + "-" + std::to_string(end_frame) + "] - Transcoded";
            item.type = MediaType::EXR_SEQUENCE;
            item.exr_layer = "RGBA";  // Transcoded files are always single-layer RGBA
            item.exr_layer_display = "RGBA (Transcoded)";
            item.path = "exr://" + sequence_path + "?layer=RGBA";
        } else {
            // Regular image sequence (TIFF, PNG, JPEG, etc.)
            item.name = base_name + " [" + std::to_string(sequence_files.size()) + " frames " +
                       std::to_string(start_frame) + "-" + std::to_string(end_frame) + "]";
            item.path = mf_url;
            item.type = MediaType::IMAGE_SEQUENCE;
        }

        // === POPULATE ImageSequenceData (primary source of truth) ===
        item.image_seq.frame_count = static_cast<int>(sequence_files.size());
        item.image_seq.start_frame = start_frame;
        item.image_seq.end_frame = end_frame;
        item.image_seq.frame_rate = frame_rate;
        item.image_seq.duration = static_cast<double>(sequence_files.size()) / frame_rate;
        item.image_seq.pattern = mf_pattern;
        item.image_seq.directory = directory;

        // Set format based on extension
        std::string format_ext = extension;
        std::transform(format_ext.begin(), format_ext.end(), format_ext.begin(), ::toupper);
        if (format_ext.length() > 0 && format_ext[0] == '.') {
            format_ext = format_ext.substr(1);  // Remove leading dot
        }
        item.image_seq.format = format_ext;

        // EXR layer info
        if (is_exr) {
            item.image_seq.layer = item.exr_layer;
            item.image_seq.layer_display = item.exr_layer_display;
        }

        // Assign cached EXR dimensions (if extracted above)
        if (item.type == MediaType::EXR_SEQUENCE && exr_width > 0 && exr_height > 0) {
            item.image_seq.width = exr_width;
            item.image_seq.height = exr_height;
        }

        // LEGACY FIELDS (keep in sync for backward compatibility)
        item.frame_count = item.image_seq.frame_count;
        item.start_frame = item.image_seq.start_frame;
        item.end_frame = item.image_seq.end_frame;
        item.frame_rate = item.image_seq.frame_rate;
        item.duration = item.image_seq.duration;
        item.sequence_pattern = item.image_seq.pattern;
        item.sequence_width = item.image_seq.width;
        item.sequence_height = item.image_seq.height;

        // Auto-detect pipeline mode from first image file (for all image sequences)
        PipelineMode pipeline_mode = PipelineMode::NORMAL;  // Default

        // Auto-detect pipeline mode from first file (for all sequence types)
        if (!sequence_files.empty()) {
            ump::ImageInfo img_info;
            if (ump::GetImageInfo(sequence_files[0], img_info)) {
                pipeline_mode = img_info.recommended_pipeline;

                // Cache dimensions from first frame (for instant loading later)
                item.image_seq.width = img_info.width;
                item.image_seq.height = img_info.height;
                item.sequence_width = img_info.width;  // Legacy
                item.sequence_height = img_info.height;  // Legacy

                Debug::Log("ProcessImageSequence: Auto-detected pipeline mode: " +
                          std::string(PipelineModeToString(pipeline_mode)) +
                          " (" + std::to_string(img_info.bit_depth) + "-bit, " +
                          (img_info.is_float ? "float" : "int") + ")");
                Debug::Log("ProcessImageSequence: Cached sequence dimensions: " +
                          std::to_string(img_info.width) + "x" + std::to_string(img_info.height));
            } else {
                // Fallback to safe default (8-bit Normal mode)
                pipeline_mode = PipelineMode::NORMAL;
                Debug::Log("ProcessImageSequence: Using safe fallback mode: NORMAL (8-bit)");
            }
        }

        // Store pipeline mode in both places
        item.image_seq.pipeline_mode = pipeline_mode;
        item.pipeline_mode = pipeline_mode;  // Legacy

        // Parse sequence for FFMPEG pattern (for both IMAGE_SEQUENCE and EXR_SEQUENCE)
        // This is needed for transcode functionality
        ump::ImageSequenceConfig ffmpeg_config =
            ump::ImageSequencePatternConverter::ParseSequence(sequence_files, frame_rate, pipeline_mode);

        if (ffmpeg_config.is_valid) {
            Debug::Log("ProcessImageSequence: FFMPEG pattern: " + ffmpeg_config.ffmpeg_pattern);
            Debug::Log("ProcessImageSequence: Pipeline mode: " + std::string(PipelineModeToString(pipeline_mode)));

            // Store the full FFmpeg pattern in MediaItem for transcode support
            item.image_seq.ffmpeg_pattern = ffmpeg_config.ffmpeg_pattern;
            item.ffmpeg_pattern = ffmpeg_config.ffmpeg_pattern;  // Legacy
        } else {
            Debug::Log("ProcessImageSequence: Warning - FFMPEG pattern parsing failed");
        }

        // Log the complete ImageSequenceData for debugging
        Debug::Log("ProcessImageSequence: ImageSequenceData populated:");
        Debug::Log("  - frame_count: " + std::to_string(item.image_seq.frame_count));
        Debug::Log("  - frame_rate: " + std::to_string(item.image_seq.frame_rate));
        Debug::Log("  - duration: " + std::to_string(item.image_seq.duration));
        Debug::Log("  - dimensions: " + std::to_string(item.image_seq.width) + "x" + std::to_string(item.image_seq.height));
        Debug::Log("  - format: " + item.image_seq.format);
        if (!item.image_seq.layer.empty()) {
            Debug::Log("  - layer: " + item.image_seq.layer);
        }

        // === BRANCH A: FFMPEG CACHE PATH ===
        // For regular image sequences (not EXR), prepare FFMPEG cache configuration
        if (item.type == MediaType::IMAGE_SEQUENCE) {
            Debug::Log("ProcessImageSequence: Preparing FFMPEG cache configuration for IMAGE_SEQUENCE");
        }

        // Add to project
        media_pool.push_back(item);
        int bin_index = GetBinIndexForMediaType(item.type);
        if (bins.size() > bin_index) {
            bins[bin_index].items.push_back(item);
        }

        // Clear previous selection and select this new item
        ClearSelection();
        SelectMediaItem(item.id, false, false);

        // === OTIO TIMELINE PATH ===
        // Route through timeline callback for unified playback (same as LoadSingleMediaItem)
        Debug::Log("ProcessImageSequence: Step 10 - Loading via OTIO timeline");

        if (image_sequence_timeline_callback) {
            // Find the item we just added to media_pool
            MediaItem* media_item = GetMediaItem(item.id);
            if (media_item) {
                *current_file_path = item.path;

                // Notify callbacks
                if (video_change_callback) {
                    video_change_callback(item.path);
                }

                // Load into OTIO timeline view
                image_sequence_timeline_callback(media_item);

                Debug::Log("ProcessImageSequence: Loaded via OTIO timeline successfully");
            } else {
                Debug::Log("ProcessImageSequence: ERROR - Could not find MediaItem in pool after adding");
            }
        } else {
            Debug::Log("ProcessImageSequence: ERROR - No image_sequence_timeline_callback set");
        }

        Debug::Log("Processed image sequence: " + item.name + " (" + std::to_string(sequence_files.size()) +
                   " frames at " + std::to_string(frame_rate) + " fps)");
        Debug::Log("MPV URL: " + mf_url);
    }

    void ProjectManager::ProcessImageSequenceWithTranscode(const std::string& sequence_path, double frame_rate,
                                                           const std::string& exr_layer, int part_index, int max_width, int compression) {
        Debug::Log("ProcessImageSequenceWithTranscode: Starting transcode workflow (part_index=" + std::to_string(part_index) + ")");

        // Get the full sequence file list
        std::vector<std::string> sequence_files = DetectImageSequence(sequence_path);
        if (sequence_files.empty()) {
            Debug::Log("ProcessImageSequenceWithTranscode: No sequence files found");
            return;
        }

        Debug::Log("ProcessImageSequenceWithTranscode: Found " + std::to_string(sequence_files.size()) + " source files");

        // Use shared transcoder (ensures consistent cache path and cancellation support)
        ump::EXRTranscoder& transcoder = GetSharedTranscoder();

        // Build transcode config
        ump::EXRTranscodeConfig config;
        config.max_width = max_width;
        config.compression = static_cast<Imf::Compression>(compression);
        config.threadCount = static_cast<size_t>(g_exr_transcode_threads);

        // Check if transcode already exists
        if (transcoder.HasTranscodedSequence(sequence_files, exr_layer, part_index, max_width, config.compression)) {
            Debug::Log("ProcessImageSequenceWithTranscode: Transcode already exists, loading directly");

            // Get transcoded files
            std::vector<std::string> transcoded_files = transcoder.GetTranscodedFiles(
                sequence_files, exr_layer, part_index, max_width, config.compression);

            if (!transcoded_files.empty()) {
                // Load transcoded sequence as single-layer EXR
                ProcessImageSequence(transcoded_files[0], frame_rate, "");  // Empty layer = single-layer
                return;
            }
        }

        // Start async transcode
        Debug::Log("ProcessImageSequenceWithTranscode: Starting async transcode...");

        // Show progress dialog
        extern bool show_transcode_progress;
        extern std::atomic<int> transcode_current_frame;
        extern std::atomic<int> transcode_total_frames;
        extern std::string transcode_status_message;
        extern std::mutex transcode_status_mutex;

        show_transcode_progress = true;
        transcode_current_frame = 0;
        transcode_total_frames = static_cast<int>(sequence_files.size());

        transcoder.TranscodeSequenceAsync(
            sequence_files,
            exr_layer,
            part_index,
            config,
            // Progress callback
            [](int current, int total, const std::string& message) {
                Debug::Log("Transcode progress: " + std::to_string(current) + "/" + std::to_string(total) + " - " + message);

                // Update UI progress (thread-safe)
                extern std::atomic<int> transcode_current_frame;
                extern std::atomic<int> transcode_total_frames;
                extern std::string transcode_status_message;
                extern std::mutex transcode_status_mutex;

                transcode_current_frame = current;
                transcode_total_frames = total;

                {
                    std::lock_guard<std::mutex> lock(transcode_status_mutex);
                    transcode_status_message = message;
                }
            },
            // Completion callback
            [this, sequence_files, exr_layer, part_index, max_width, compression, frame_rate](bool success, const std::string& error_message) {
                // Hide progress dialog
                extern bool show_transcode_progress;
                show_transcode_progress = false;

                if (success) {
                    Debug::Log("ProcessImageSequenceWithTranscode: Transcode complete!");

                    // Get transcoded files using shared transcoder
                    ump::EXRTranscoder& transcoder = GetSharedTranscoder();
                    Imf::Compression comp = static_cast<Imf::Compression>(compression);
                    std::vector<std::string> transcoded_files = transcoder.GetTranscodedFiles(
                        sequence_files, exr_layer, part_index, max_width, comp);

                    if (!transcoded_files.empty()) {
                        Debug::Log("ProcessImageSequenceWithTranscode: Queuing transcoded sequence for load (" +
                                  std::to_string(transcoded_files.size()) + " frames)");

                        // Queue the load for main thread (can't call ProcessImageSequence from worker thread)
                        {
                            std::lock_guard<std::mutex> lock(pending_transcode_mutex);
                            pending_transcode_first_file = transcoded_files[0];
                            pending_transcode_frame_rate = frame_rate;
                        }
                        pending_transcode_load.store(true);
                    } else {
                        Debug::Log("ProcessImageSequenceWithTranscode: ERROR - No transcoded files found after completion");
                        // TODO: Show error dialog
                    }
                } else {
                    Debug::Log("ProcessImageSequenceWithTranscode: ERROR - Transcode failed: " + error_message);
                    // TODO: Show error dialog with message: error_message
                }
            }
        );

        Debug::Log("ProcessImageSequenceWithTranscode: Async transcode initiated");
    }

    void ProjectManager::CancelTranscode() {
        // Use shared transcoder for cancellation
        ump::EXRTranscoder& transcoder = GetSharedTranscoder();
        transcoder.CancelTranscode();
        Debug::Log("ProjectManager: Transcode cancellation requested");

        // Hide progress dialog
        extern bool show_transcode_progress;
        show_transcode_progress = false;
    }

    bool ProjectManager::IsInImageSequenceMode() const {
        return (current_file_path && current_file_path->find("mf://") == 0);
    }

    bool ProjectManager::HasLoadedEXRSequences() const {
        // Check if currently playing video is an EXR sequence
        bool in_exr_mode = video_player && video_player->IsInEXRMode();
        Debug::Log("HasLoadedEXRSequences check: video_player IsInEXRMode = " + std::string(in_exr_mode ? "true" : "false"));

        if (in_exr_mode) {
            Debug::Log("HasLoadedEXRSequences: Returning true (video player is in EXR mode)");
            return true;
        }

        // Check if any media items in project are EXR sequences
        int exr_count = 0;
        for (const auto& item : media_pool) {
            if (item.type == MediaType::EXR_SEQUENCE) {
                exr_count++;
            }
        }
        Debug::Log("HasLoadedEXRSequences: Found " + std::to_string(exr_count) + " EXR sequences in media_pool");

        if (exr_count > 0) {
            Debug::Log("HasLoadedEXRSequences: Returning true (found EXR in media pool)");
            return true;
        }

        Debug::Log("HasLoadedEXRSequences: Returning false (no EXR sequences found)");
        return false;
    }

    PipelineMode ProjectManager::GetImageSequencePipelineMode() const {
        // Get pipeline mode from video cache (auto-detected from first frame)
        if (video_cache_manager) {
            return video_cache_manager->GetPipelineMode();
        }

        // Default fallback
        return PipelineMode::NORMAL;
    }

    std::string ProjectManager::GetAnnotationPathForMedia(const std::string& media_path) const {
        // For regular files (videos), return as-is
        if (media_path.find("mf://") != 0 && media_path.find("exr://") != 0) {
            return media_path;
        }

        // For mf:// URLs (image sequences), extract directory from stored MediaItem
        if (media_path.find("mf://") == 0) {
            for (const auto& item : media_pool) {
                if (item.path == media_path && !item.ffmpeg_pattern.empty()) {
                    // ffmpeg_pattern is like "/path/to/sequence_%04d.png"
                    // Extract directory from it
                    std::filesystem::path pattern_path(item.ffmpeg_pattern);
                    return pattern_path.parent_path().string();
                }
            }
        }

        // For exr:// URLs, extract path before the ? (layer parameter)
        if (media_path.find("exr://") == 0) {
            std::string path_part = media_path.substr(6); // Remove "exr://"
            size_t query_pos = path_part.find('?');
            if (query_pos != std::string::npos) {
                path_part = path_part.substr(0, query_pos);
            }
            return path_part;
        }

        // Fallback: return as-is
        return media_path;
    }

    // ============================================================================
    // IN/OUT POINT MANAGEMENT (Per-video)
    // ============================================================================

    void ProjectManager::SetInPoint(double timestamp) {
        if (!current_file_path || current_file_path->empty()) return;

        // Find MediaItem for current file
        MediaItem* item = GetMediaItemFromCurrentPath();
        if (item) {
            item->in_point = timestamp;
            Debug::Log("Set In point for \"" + item->name + "\": " + std::to_string(timestamp) + "s");

            // Also update bin copy if it exists
            for (auto& bin : bins) {
                for (auto& bin_item : bin.items) {
                    if (bin_item.id == item->id) {
                        bin_item.in_point = timestamp;
                        break;
                    }
                }
            }
        }
    }

    void ProjectManager::SetOutPoint(double timestamp) {
        if (!current_file_path || current_file_path->empty()) return;

        // Find MediaItem for current file
        MediaItem* item = GetMediaItemFromCurrentPath();
        if (item) {
            item->out_point = timestamp;
            Debug::Log("Set Out point for \"" + item->name + "\": " + std::to_string(timestamp) + "s");

            // Also update bin copy if it exists
            for (auto& bin : bins) {
                for (auto& bin_item : bin.items) {
                    if (bin_item.id == item->id) {
                        bin_item.out_point = timestamp;
                        break;
                    }
                }
            }
        }
    }

    double ProjectManager::GetInPoint() const {
        if (!current_file_path || current_file_path->empty()) return -1.0;

        // Find MediaItem for current file (const version)
        const MediaItem* item = const_cast<ProjectManager*>(this)->GetMediaItemFromCurrentPath();
        return item ? item->in_point : -1.0;
    }

    double ProjectManager::GetOutPoint() const {
        if (!current_file_path || current_file_path->empty()) return -1.0;

        // Find MediaItem for current file (const version)
        const MediaItem* item = const_cast<ProjectManager*>(this)->GetMediaItemFromCurrentPath();
        return item ? item->out_point : -1.0;
    }

    bool ProjectManager::HasInPoint() const {
        return GetInPoint() >= 0.0;
    }

    bool ProjectManager::HasOutPoint() const {
        return GetOutPoint() >= 0.0;
    }

    bool ProjectManager::HasBothInOutPoints() const {
        return HasInPoint() && HasOutPoint();
    }

    void ProjectManager::ClearInOutPoints() {
        if (!current_file_path || current_file_path->empty()) return;

        MediaItem* item = GetMediaItemFromCurrentPath();
        if (item) {
            item->in_point = -1.0;
            item->out_point = -1.0;
            Debug::Log("Cleared In/Out points for \"" + item->name + "\"");

            // Also update bin copy
            for (auto& bin : bins) {
                for (auto& bin_item : bin.items) {
                    if (bin_item.id == item->id) {
                        bin_item.in_point = -1.0;
                        bin_item.out_point = -1.0;
                        break;
                    }
                }
            }
        }
    }

}