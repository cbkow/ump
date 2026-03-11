// ============================================================================
// PLATFORM AND SYSTEM INCLUDES
// ============================================================================
#ifdef _WIN32
// NOMINMAX defined globally in CMakeLists.txt for OCIO 2.5 compatibility
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// ============================================================================
// GPU DRIVER HINTS
// These exports tell NVIDIA/AMD drivers to use high-performance GPU and
// help override application profile settings that may interfere with D3D11
// ============================================================================
extern "C" {
    // NVIDIA Optimus: Request high-performance NVIDIA GPU
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;

    // AMD PowerXpress: Request high-performance AMD GPU
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#include <shellapi.h>
#include <dwmapi.h>
#include <VersionHelpers.h>
#include <shobjidl.h>
#include <shlobj.h>  // For SHGetKnownFolderPath
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#endif

// ============================================================================
// GRAPHICS AND UI INCLUDES
// ============================================================================
#include <glad/gl.h>
#include <GLFW/glfw3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include <imgui.h>
#include <imgui_impl_glfw.h>
#ifdef QCVIEW_USE_VULKAN
#include <imgui_impl_vulkan.h>
#include "gpu/vulkan_device.h"
#else
#include <imgui_impl_opengl3.h>
#endif
#include <imgui_internal.h>
#include <implot.h>
#include <nfd.h>
#define STB_IMAGE_IMPLEMENTATION
#include "../external/glfw/deps/stb_image.h"
#include "../external/glfw/deps/stb_image_write.h"

// ============================================================================
// STANDARD LIBRARY INCLUDES
// ============================================================================
#include <iostream>
#include <memory>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <future>
#include <chrono>
#include <thread>
#include <atomic>
#include <sstream>
#include <iomanip>
#include <map>
#include <filesystem>
#include <nlohmann/json.hpp>

// ============================================================================
// FFMPEG INCLUDES (for base64 encoding)
// ============================================================================
extern "C" {
#include <libavutil/base64.h>
}

// ============================================================================
// PROJECT INCLUDES
// ============================================================================
#include "player/video_player.h"
#include "player/thumbnail_cache.h"
#include "utils/exiftool_helper.h"
#include "utils/debug_utils.h"
#include "utils/frame_indexing.h"
#include "utils/system_pressure_monitor.h"
#include "utils/asset_path.h"
#ifdef _WIN32
#include "hdr/hdr_output_manager.h"
#include "gpu/d3d11_video_interop.h"
#endif
#if defined(__linux__) && defined(QCVIEW_HAS_SDBUS)
#include "utils/single_instance.h"
#endif
#ifdef QCVIEW_USE_VULKAN
#include "gpu/vulkan_device.h"
#endif
#include "app/app_ui_macros.h"
#include "project/project_manager.h"
#include "project/media_item.h"
#include "imnodes/imnodes.h"
#include "color/ocio_config_manager.h"
#include "ui/node_editor_theme.h"
#include "nodes/node_manager.h"
#include "overlay/safety_overlay_system.h"
#include "nodes/node_base.h"
#include "color/ocio_pipeline.h"
#include "ui/timeline_manager.h"
#include "annotations/annotation_manager.h"
#include "ui/annotation_panel.h"
#include "annotations/viewport_annotator.h"
#include "annotations/annotation_toolbar.h"
#include "annotations/annotation_renderer.h"
#include "annotations/stroke_smoother.h"
#include "annotations/annotation_serializer.h"
#include "annotations/annotation_exporter.h"
#include "annotations/nanovg_context.h"
#include "integrations/frameio_url_parser.h"
#include "integrations/frameio_client.h"
#include "integrations/frameio_converter.h"
#include "timeline/timeline_view.h"
#include "timeline/timeline_playback_controller.h"
#include "timeline/timeline_cache.h"
#include "timeline/timeline_thumbnail_cache.h"
#include "timeline/timeline_commands.h"
#include "timeline/timeline_selection.h"
#include "player/shared_memory_pool.h"
#include "player/hw_context_manager.h"
#include "player/video_decoder_factory.h"
#include "timeline/media_linker.h"
#include "audio/audio_player.h"
#include "audio/audio_mixer.h"
#include "audio/audio_decoder.h"

// ============================================================================
// COLOR INCLUDES
// ============================================================================
#include "transcode/ocio_lut_baker.h"
std::unique_ptr<OCIOConfigManager> ocio_manager;

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
ImFont* font_regular = nullptr;
ImFont* font_bold = nullptr;
ImFont* font_italic = nullptr;
ImFont* font_mono = nullptr;
ImFont* font_icons = nullptr;

// Windows accent color toggle state
bool use_windows_accent_color = false;
int custom_accent_color_index = -1;  // -1 = use default/system, -2 = custom picker color, 0+ = index into accent_color_palette
ImVec4 custom_picker_color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);  // User-selected color from color picker

// Accent color palette (hex colors converted to ImVec4)
// Note: extern const to give external linkage for other translation units
extern const ImVec4 accent_color_palette[] = {
    ImVec4(0xdb/255.0f, 0x85/255.0f, 0x32/255.0f, 1.0f),  // 0: db8532 - Default amber
    ImVec4(0x69/255.0f, 0x79/255.0f, 0x7e/255.0f, 1.0f),  // 1: 69797e - Gray-blue (old default)
    ImVec4(0x65/255.0f, 0x55/255.0f, 0x15/255.0f, 1.0f),  // 2: 655515 - Old yellow
    ImVec4(0xda/255.0f, 0x3b/255.0f, 0x01/255.0f, 1.0f),  // 3: da3b01 - Orange
    ImVec4(0xef/255.0f, 0x69/255.0f, 0x50/255.0f, 1.0f),  // 4: ef6950 - Coral
    ImVec4(0xd1/255.0f, 0x34/255.0f, 0x38/255.0f, 1.0f),  // 5: d13438 - Red
    ImVec4(0xff/255.0f, 0x43/255.0f, 0x43/255.0f, 1.0f),  // 6: ff4343 - Bright red
    ImVec4(0xc2/255.0f, 0x39/255.0f, 0xb3/255.0f, 1.0f),  // 7: c239b3 - Magenta
    ImVec4(0x00/255.0f, 0x78/255.0f, 0xd4/255.0f, 1.0f),  // 8: 0078d4 - Blue
    ImVec4(0x8e/255.0f, 0x8c/255.0f, 0xd8/255.0f, 1.0f),  // 9: 8e8cd8 - Light purple
    ImVec4(0x87/255.0f, 0x64/255.0f, 0xb8/255.0f, 1.0f),  // 10: 8764b8 - Purple
    ImVec4(0xb1/255.0f, 0x46/255.0f, 0xc2/255.0f, 1.0f),  // 11: b146c2 - Violet
    ImVec4(0x2d/255.0f, 0x7d/255.0f, 0x9a/255.0f, 1.0f),  // 12: 2d7d9a - Teal
    ImVec4(0x7a/255.0f, 0x75/255.0f, 0x74/255.0f, 1.0f),  // 13: 7a7574 - Warm gray
    ImVec4(0x68/255.0f, 0x76/255.0f, 0x8a/255.0f, 1.0f),  // 14: 68768a - Cool gray
    ImVec4(0x56/255.0f, 0x7c/255.0f, 0x73/255.0f, 1.0f),  // 15: 567c73 - Sage
    ImVec4(0x64/255.0f, 0x7c/255.0f, 0x64/255.0f, 1.0f),  // 16: 647c64 - Green
    ImVec4(0x84/255.0f, 0x75/255.0f, 0x45/255.0f, 1.0f),  // 17: 847545 - Olive
};
extern const int accent_color_palette_count = sizeof(accent_color_palette) / sizeof(accent_color_palette[0]);

// ============================================================================
// TIMECODE MODE VARIABLES
// ============================================================================
#include "app/timecode.h"  // TimecodeState enum + timecode globals

// ============================================================================
// CACHE CONTROL VARIABLES
// ============================================================================
bool cache_enabled = false;  // Off by default - user can enable in settings
bool show_cache_settings = false;

// Pipeline-aware cache settings
#include "app/app_config.h"  // CacheSettings struct + extern cache_settings
CacheSettings cache_settings;

// ============================================================================
// TRANSCODE PERFORMANCE SETTINGS
// ============================================================================
#include "transcode/transcode_settings.h"

// Global transcode settings instance (definition - declared extern in header)
TranscodeSettings transcode_settings;

// ============================================================================
// AUTO-PLAY ON LOAD STATE
// ============================================================================
// Auto-play is now buffer-aware: waits for 90% cache fill before starting playback
// This ensures smooth playback from the start, even for heavy sequences
bool auto_play_buffering = false;  // True when waiting for buffer to fill before auto-play
std::chrono::steady_clock::time_point auto_play_buffer_start;  // When buffering started (for timeout)

// ============================================================================
// SEEK CACHE DELAYED START STATE
// ============================================================================
bool pending_seek_cache_start = false;
std::chrono::steady_clock::time_point seek_cache_start_timer;

// ============================================================================
// MOUSE ACTIVITY TRACKING (for cache/auto-play pause during interaction)
// ============================================================================
ImVec2 last_mouse_pos = ImVec2(0, 0);
std::chrono::steady_clock::time_point last_mouse_move_time = std::chrono::steady_clock::now();
bool mouse_is_idle = false;

// ============================================================================
// EXR TRANSCODE PROGRESS STATE
// ============================================================================
bool show_transcode_progress = false;
std::atomic<int> transcode_current_frame{0};
std::atomic<int> transcode_total_frames{0};
std::string transcode_status_message = "";
std::mutex transcode_status_mutex;

// ============================================================================
// CACHE CLEAR DIALOG STATE
// ============================================================================
bool show_cannot_clear_exr_cache_error = false;
bool show_exr_cache_cleared_success = false;
size_t exr_cache_bytes_cleared = 0;

// ============================================================================
// FRAME RATE LIMITER
// ============================================================================
constexpr bool g_limit_imgui_fps = true;           // Enable UI limit to reduce GPU load
constexpr double g_target_frame_time = 1.0 / 60.00; // Locked at 60
std::chrono::steady_clock::time_point g_last_frame_time;

// ============================================================================
// OTIO TIMELINE MODE STATE
// ============================================================================
bool otio_timeline_mode = true;                     // OTIO timeline view is now the default
bool otio_dual_view_mode = false;                   // Dual view mode (LEFT/RIGHT tracks, split viewport)
bool otio_dual_view_split_mode = false;             // Split mode (vertical wipe) vs Dual mode (side-by-side)
bool g_skip_viewport_render_frame = false;          // Skip viewport rendering this frame (after ForceReloadCurrentMedia)
bool g_force_reload_pending = false;                // Deferred reload - process at start of next frame
float otio_dual_view_split_pos = 0.5f;              // Split position (0.0-1.0, default center)
bool otio_dual_view_left_audio = true;              // LEFT track audio enabled (default ON)
bool otio_dual_view_right_audio = false;            // RIGHT track audio enabled (default OFF)
std::unique_ptr<qcview::TimelineView> timeline_view;   // OTIO timeline viewer instance
std::unique_ptr<qcview::MediaLinker> media_linker;     // Media linking system for timeline clips
std::unique_ptr<qcview::TimelinePlaybackController> scratch_timeline_controller;  // For new/scratch timelines without EDL/OTIO file
qcview::TimelinePlaybackController::DualViewTextures cached_dual_view_textures;  // Pre-rendered dual view textures (updated before ImGui)
std::vector<GLuint> g_pending_texture_deletions;  // Textures to delete BEFORE ImGui::NewFrame() (avoid GL ops during render)

// Pre-rendered color-corrected textures (updated BEFORE ImGui::NewFrame to avoid GL ops during render)
// Uses double-buffering with fence sync to avoid rendering with incomplete textures
struct ColorCorrectedTextureCache {
    // Current frame textures (being uploaded this frame)
    GLuint left_texture = 0;
    GLuint right_texture = 0;
    int left_width = 0;
    int left_height = 0;
    int right_width = 0;
    int right_height = 0;
    GLuint cached_frame_texture = 0;
    int cached_frame_width = 0;
    int cached_frame_height = 0;
    GLuint cached_frame_source_id = 0;

    // Unified composite texture (when dual view uses single interop)
    GLuint composite_texture = 0;
    int composite_width = 0;
    int composite_height = 0;

    // Previous frame textures (fallback if current not ready)
    GLuint prev_left_texture = 0;
    GLuint prev_right_texture = 0;
    GLuint prev_cached_frame_texture = 0;
    GLuint prev_composite_texture = 0;

    // GPU sync state
    GLsync upload_fence = nullptr;
    bool current_ready = true;  // Start true (no textures = ready)

    // Get the texture to use (current if ready, prev if not)
    GLuint GetLeftTexture() const {
        return current_ready ? left_texture : prev_left_texture;
    }
    GLuint GetRightTexture() const {
        return current_ready ? right_texture : prev_right_texture;
    }
    GLuint GetCachedFrameTexture() const {
        return current_ready ? cached_frame_texture : prev_cached_frame_texture;
    }
    GLuint GetCompositeTexture() const {
        return current_ready ? composite_texture : prev_composite_texture;
    }
};
ColorCorrectedTextureCache g_color_corrected_cache;

// Timeline editing system
std::unique_ptr<qcview::TimelineCommandManager> timeline_command_manager;  // Undo/redo command manager
std::unique_ptr<qcview::TimelineThumbnailCache> timeline_thumbnail_cache;  // Separate LRU cache for trim/slip preview

// Helper to ensure command manager exists and has the timeline view set for thread safety
inline qcview::TimelineCommandManager* GetCommandManager(qcview::TimelineView* view) {
    if (!timeline_command_manager) {
        timeline_command_manager = std::make_unique<qcview::TimelineCommandManager>();
    }
    // Always update the view in case it changed (e.g., new timeline loaded)
    timeline_command_manager->SetTimelineView(view);
    return timeline_command_manager.get();
}
bool timeline_thumbnail_cache_clear_deferred = false;  // Deferred clear to avoid deleting textures mid-frame
qcview::TimelineClipDragState timeline_clip_drag;      // State for clip dragging
qcview::TimelineTrimState timeline_trim_state;         // State for clip edge trimming
qcview::TimelineSlipState timeline_slip_state;         // State for slip editing (PLAYLIST mode)
qcview::TimelineReorderState timeline_reorder_state;   // State for reorder/move (PLAYLIST mode)
qcview::TimelineMediaDropState timeline_media_drop;    // State for media drop from project panel

// Pending drag state for PLAYLIST mode - requires drag threshold before starting edit
bool playlist_pending_drag_active = false;
std::string playlist_pending_drag_clip_id;
int playlist_pending_drag_clip_idx = -1;
float playlist_pending_drag_start_x = 0.0f;
bool playlist_pending_drag_shift = false;  // true = slip, false = reorder
double playlist_pending_drag_source_in = 0.0;
double playlist_pending_drag_source_out = 0.0;
double playlist_pending_drag_source_duration = 0.0;
double playlist_pending_drag_clip_duration = 0.0;
constexpr float kPlaylistDragThreshold = 15.0f;  // Pixels of movement before edit starts

// Viewport drop state for dual view LEFT/RIGHT selection popup
struct ViewportDualViewDropState {
    bool popup_open = false;
    std::string pending_media_id;           // Media ID to add
    std::vector<std::string> pending_media_ids;  // For multi-select drops
};
ViewportDualViewDropState g_viewport_dv_drop;

// Screenshot button flash state (visual feedback on success)
struct ScreenshotFlashState {
    std::chrono::steady_clock::time_point clipboard_flash_start;
    std::chrono::steady_clock::time_point desktop_flash_start;
    bool clipboard_flashing = false;
    bool desktop_flashing = false;
    static constexpr float FLASH_DURATION_MS = 400.0f;  // Flash duration in milliseconds
};
ScreenshotFlashState g_screenshot_flash;

// In-memory cache for media links (persists across timeline reloads until app quit or project save)
struct CachedClipLink {
    std::string clip_id;
    std::string linked_path;
    double source_fps = 0.0;
    int source_width = 0;
    int source_height = 0;
    double source_duration = 0.0;
    bool has_audio = false;
    bool audio_muted = false;  // Per-clip audio mute state
};
std::map<std::string, std::vector<CachedClipLink>> timeline_link_cache;  // timeline_path -> clip links
std::string current_timeline_path;  // Track which timeline is currently loaded (file path for EDL/OTIO)
std::string current_timeline_id;    // Track current timeline by ID (for scratch timelines and lookup)

// Static pointer to project manager for link caching (set during initialization)
qcview::ProjectManager* g_project_manager = nullptr;

// Helper: Save current timeline's links to cache (both session cache and project)
void SaveLinksToCache(const std::string& timeline_id = "") {
    if (current_timeline_path.empty() || !timeline_view) return;

    std::vector<CachedClipLink> links;
    for (const auto& track : timeline_view->GetTracks()) {
        for (const auto& clip : track.clips) {
            if (clip.is_linked && !clip.linked_path.empty()) {
                CachedClipLink cached;
                cached.clip_id = clip.id;
                cached.linked_path = clip.linked_path;
                cached.source_fps = clip.source_fps;
                cached.source_width = clip.source_width;
                cached.source_height = clip.source_height;
                cached.source_duration = clip.source_duration;
                cached.has_audio = clip.has_audio;
                cached.audio_muted = clip.audio_muted;
                links.push_back(cached);
            }
        }
    }

    if (!links.empty()) {
        // Save to session cache
        timeline_link_cache[current_timeline_path] = links;
        Debug::Log("Cached " + std::to_string(links.size()) +
                   " media links for " + current_timeline_path);
    }
}

// Helper: Restore links from cache when loading a timeline
// Returns number of links restored (0 if none)
// First checks session cache, then ProjectManager (from project file)
int RestoreLinksFromCache(const std::string& timeline_id = "") {
    if (current_timeline_path.empty() || !timeline_view) return 0;

    // First check session cache
    auto it = timeline_link_cache.find(current_timeline_path);
    if (it != timeline_link_cache.end()) {
        const auto& cached_links = it->second;
        int restored_count = 0;

        for (auto& track : timeline_view->GetTracks()) {
            for (auto& clip : track.clips) {
                for (const auto& cached : cached_links) {
                    if (clip.id == cached.clip_id) {
                        clip.linked_path = cached.linked_path;
                        clip.is_linked = true;
                        clip.source_fps = cached.source_fps;
                        clip.source_width = cached.source_width;
                        clip.source_height = cached.source_height;
                        clip.source_duration = cached.source_duration;
                        clip.has_audio = cached.has_audio;
                        clip.audio_muted = cached.audio_muted;
                        restored_count++;
                        break;
                    }
                }
            }
        }

        if (restored_count > 0) {
            Debug::Log("Restored " + std::to_string(restored_count) + " cached media links from session cache");
            timeline_view->GetFlattener().SetTracks(timeline_view->GetTracks());
            return restored_count;
        }
    }

    return 0;
}

// Link Media popup state
bool show_link_media_popup = false;
std::string link_media_search_path;
qcview::LinkSummary last_link_summary;
bool show_link_results_popup = false;

// Auto-relink state (for automatic media linking on timeline load)
bool auto_relink_in_progress = false;
bool show_auto_relink_progress = false;
bool show_auto_relink_results = false;
int auto_relink_current = 0;
int auto_relink_total = 0;
std::string auto_relink_status;
std::string auto_relink_timeline_id;  // Timeline ID for saving links to project

// ============================================================================
// SYSTEM PRESSURE MONITOR STATE
// ============================================================================
std::unique_ptr<qcview::SystemPressureMonitor> pressure_monitor;
bool show_pressure_critical_dialog = false;
bool in_emergency_mode = false;  // Track if we're in critical emergency state
qcview::SystemPressureStatus last_pressure_status;

// Menu bar notification system (for RAM warnings)
std::string stats_bar_notification_message;
std::chrono::steady_clock::time_point notification_start_time;
float notification_timeout_seconds = 8.0f;  // Auto-dismiss after 8 seconds (for recovery message)
bool show_notification_permanent = false;   // true = permanent (critical), false = timed (recovery)

// Track if EXR cache was shut down during emergency (to restore on recovery)
bool exr_cache_was_active = false;
std::string exr_video_path_before_shutdown;

// ============================================================================
// FONT SETTINGS
// ============================================================================
float g_font_scale = 1.0f;  // Global font scale (0.5x - 2.0x)
bool show_font_settings_window = false;
float font_settings_temp_scale = 1.0f;  // Temporary scale while editing

// ============================================================================
// KEYBOARD SHORTCUTS POPUP
// ============================================================================
bool show_shortcuts_popup = false;

// ============================================================================
// DELETE PREFERENCES CONFIRMATION
// ============================================================================
bool show_delete_prefs_confirm = false;

// ============================================================================
// LUT EXPORT POPUP
// ============================================================================
bool show_lut_export_popup = false;
std::string lut_export_path;
std::atomic<float> lut_export_progress{0.0f};
std::atomic<bool> lut_export_cancel{false};
std::atomic<bool> lut_export_complete{false};
std::atomic<bool> lut_export_error{false};
std::string lut_export_error_message;

// Font scale presets
static constexpr float FONT_SCALE_SMALL = 0.75f;
static constexpr float FONT_SCALE_MEDIUM = 1.0f;
static constexpr float FONT_SCALE_LARGE = 1.25f;
static constexpr float FONT_SCALE_XLARGE = 1.5f;

// Auto-configure EXR threading settings based on CPU capabilities
void AutoConfigureEXRThreading(decltype(cache_settings)& settings) {
    int hardware_threads = static_cast<int>(std::thread::hardware_concurrency());

    if (hardware_threads <= 0) {
        printf("EXR Auto-Config: Unable to detect CPU cores, using defaults\n");
        return;
    }

    printf("EXR Auto-Config: Detected %d hardware threads\n", hardware_threads);

    // Configure EXR I/O worker threads - AGGRESSIVE settings for max throughput
    // With generation tagging + PER_WORKER mode (1:1 mapping), we can use ALL cores
    if (hardware_threads <= 4) {
        settings.exr_oiio_threads = std::max(1, hardware_threads - 1);  // 4 cores ? 3 workers
    } else if (hardware_threads <= 8) {
        settings.exr_oiio_threads = hardware_threads;  // 8 cores ? 8 workers
    } else if (hardware_threads <= 16) {
        settings.exr_oiio_threads = hardware_threads - 2;  // 16 cores ? 14 workers (leave room for OS)
    } else {
        settings.exr_oiio_threads = 16;  // 32+ cores ? cap at 16 workers (reasonable limit)
    }

    // Ensure bounds
    settings.exr_oiio_threads = std::clamp(settings.exr_oiio_threads, 1, 16);

    printf("EXR Auto-Config: %d OIIO worker threads (PER_WORKER 1:1 mode + generation tagging)\n",
           settings.exr_oiio_threads);
}

// Create DirectEXRCacheConfig from current cache_settings
// Global EXR cache settings (visible to project_manager)
int g_exr_read_ahead_frames = 72;     // Frames to cache ahead (~3s @ 24fps)
int g_read_behind_frames = 12;        // Frames to keep behind playhead (~0.5s @ 24fps)
int g_exr_thread_count = 16;          // DirectEXRCache parallel I/O threads
int g_exr_transcode_threads = 8;      // EXRTranscoder parallel transcode threads

// Global timeline cache settings (EDL/OTIO playback)
int g_timeline_read_ahead_frames = 16;      // Frames to cache ahead
int g_timeline_read_behind_frames = 4;      // Frames to keep behind playhead
int g_timeline_max_textures = 120;          // Max GPU textures (safety cap)
int g_timeline_io_threads = 8;              // Background I/O threads
int g_video_decode_threads = 4;             // FFmpeg decode threads per video (4 default, 8+ for high-core systems)

// Global disk cache settings
std::string g_custom_cache_path = "";  // Empty = use default %LOCALAPPDATA%
int g_cache_retention_days = 7;  // Auto-cleanup files older than N days
int g_transcode_cache_max_gb = 10;  // EXR transcode cache size limit
bool g_clear_cache_on_exit = false;  // Clear all cache on app exit

// Global executable directory - used for resolving asset paths
// We store this separately because working directory is set to a writable location
std::filesystem::path g_exe_dir;

// Resolve asset paths relative to executable directory
// On Linux, also check the system install location (/usr/share/qcview/)
std::string GetAssetPath(const std::string& relative_path) {
    // First check relative to executable (works for dev builds and portable installs)
    auto local_path = g_exe_dir / relative_path;
    if (std::filesystem::exists(local_path))
        return local_path.string();

#ifdef __linux__
    // Check system install location (e.g. /usr/share/qcview/assets/...)
    auto system_path = std::filesystem::path("/usr/share/qcview") / relative_path;
    if (std::filesystem::exists(system_path))
        return system_path.string();

    // Check /usr/local/share as well
    auto local_system_path = std::filesystem::path("/usr/local/share/qcview") / relative_path;
    if (std::filesystem::exists(local_system_path))
        return local_system_path.string();
#endif

    // Fallback to original path (may not exist, but caller can handle)
    return local_path.string();
}

qcview::DirectEXRCacheConfig GetCurrentEXRCacheConfig() {
    qcview::DirectEXRCacheConfig config;

    // Use UI-configured values
    config.readAheadFrames = g_exr_read_ahead_frames;
    config.readBehindFrames = g_read_behind_frames;
    config.threadCount = static_cast<size_t>(g_exr_thread_count);

    return config;
}

qcview::TimelineCacheConfig GetCurrentTimelineCacheConfig() {
    qcview::TimelineCacheConfig config;

    // Use UI-configured values
    config.readAheadFrames = g_timeline_read_ahead_frames;
    config.readBehindFrames = g_timeline_read_behind_frames;
    config.max_textures = g_timeline_max_textures;
    config.io_threads = g_timeline_io_threads;
    config.decodeThreads = g_video_decode_threads;

    return config;
}

qcview::ThumbnailConfig GetCurrentThumbnailConfig() {
    qcview::ThumbnailConfig config;

    // Use cache_settings values
    config.width = cache_settings.thumbnail_width;
    config.height = cache_settings.thumbnail_height;
    config.cache_size = cache_settings.thumbnail_cache_size;
    config.enabled = cache_settings.enable_thumbnails;

    return config;
}

#include "app/app_icons.h"
#include "app/application.h"
#include "app/drawing_utils.h"
#ifdef QCVIEW_USE_VULKAN
#include "annotations/vulkan_annotation_renderer.h"
#endif


// ============================================================================
// GLOBAL WRAPPER FUNCTIONS (for external access to Application methods)
// ============================================================================

// Global SaveSettings wrapper - allows other translation units to save settings
void SaveSettings() {
    if (Application::app_instance) {
        Application::app_instance->SaveSettings();
    }
}

// Global function to schedule an import with overlay
// Called from ProjectManager for AAF/XML imports
void ScheduleImport(const std::string& path, const std::string& message) {
    if (Application::app_instance) {
        Application::app_instance->ScheduleImport(path, message);
    }
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

#ifdef _WIN32
// Helper function to find the existing QCView instance window
// Uses window properties instead of class names for reliable identification
static HWND FindUmpWindow() {
    HWND found_hwnd = NULL;

    // Enumerate all top-level windows to find ours
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        // Check if this window has our unique property
        HANDLE prop = GetPropW(hwnd, L"qcview_SingleInstanceWindow");
        if (prop == (HANDLE)0x514356) {  // "QCV" in hex
            HWND* result = (HWND*)lParam;
            *result = hwnd;
            return FALSE;  // Stop enumeration
        }
        return TRUE;  // Continue enumeration
    }, (LPARAM)&found_hwnd);

    return found_hwnd;
}
#endif // _WIN32

// ============================================================================
// ENTRY POINTS
// ============================================================================
#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
#ifdef _DEBUG
    if (AllocConsole()) {
        FILE* pCout;
        freopen_s(&pCout, "CONOUT$", "w", stdout);
        freopen_s(&pCout, "CONOUT$", "w", stderr);
    }
#endif

    // Set working directory to a writable location BEFORE any D3D11/ffmpeg initialization
    // This fixes issues when running from Program Files (read-only location)
    // D3D11 drivers and ffmpeg may try to write to CWD during initialization
    {
        PWSTR localappdata_path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localappdata_path))) {
            std::filesystem::path writable_dir = std::filesystem::path(localappdata_path) / "qcview" / "data";
            CoTaskMemFree(localappdata_path);

            try {
                std::filesystem::create_directories(writable_dir);
                std::filesystem::current_path(writable_dir);
            } catch (...) {
                // Silently continue - will use whatever CWD exists
            }
        }
    }

    // Store executable directory for asset path resolution
    {
        wchar_t exe_path_buf[MAX_PATH];
        GetModuleFileNameW(nullptr, exe_path_buf, MAX_PATH);
        g_exe_dir = std::filesystem::path(exe_path_buf).parent_path();
    }

    Application app;

    if (!app.Initialize()) {
        return -1;
    }

    app.Run();
    app.Cleanup();

    return 0;
}
#endif

int main(int argc, char* argv[]) {
    // Set AppUserModelID for Windows 11 taskbar/start menu integration
    #ifdef _WIN32
    SetCurrentProcessExplicitAppUserModelID(L"cbkow.qcview");
    #endif

    // Store executable directory for asset path resolution
    #ifdef _WIN32
    {
        wchar_t exe_path_buf[MAX_PATH];
        GetModuleFileNameW(nullptr, exe_path_buf, MAX_PATH);
        g_exe_dir = std::filesystem::path(exe_path_buf).parent_path();
        Debug::Log("Executable directory: " + g_exe_dir.string());
    }

    // Set working directory to a writable location (%LOCALAPPDATA%\qcview\data)
    {
        PWSTR localappdata_path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localappdata_path))) {
            std::filesystem::path writable_dir = std::filesystem::path(localappdata_path) / "qcview" / "data";
            CoTaskMemFree(localappdata_path);

            try {
                std::filesystem::create_directories(writable_dir);
                std::filesystem::current_path(writable_dir);
                Debug::Log("Set working directory to: " + writable_dir.string());
            } catch (const std::exception& e) {
                Debug::Log("Warning: Failed to set writable working directory: " + std::string(e.what()));
                std::filesystem::current_path(g_exe_dir);
            }
        } else {
            std::filesystem::current_path(g_exe_dir);
            Debug::Log("Warning: Could not get LOCALAPPDATA, using exe directory as working directory");
        }
    }
    #else
    // Linux: resolve exe directory and set writable working directory
    try {
        std::filesystem::path exe_path = std::filesystem::canonical("/proc/self/exe");
        g_exe_dir = exe_path.parent_path();
        Debug::Log("Executable directory: " + g_exe_dir.string());
    } catch (const std::exception& e) {
        // Fallback to argv[0]
        try {
            std::filesystem::path exe_path = std::filesystem::absolute(argv[0]);
            g_exe_dir = exe_path.parent_path();
        } catch (...) {}
        std::cerr << "Warning: Failed to determine executable directory: " << e.what() << std::endl;
    }

    // Set writable working directory on Linux (~/.local/share/qcview/data)
    {
        const char* xdg_data = std::getenv("XDG_DATA_HOME");
        std::filesystem::path data_dir;
        if (xdg_data && xdg_data[0]) {
            data_dir = std::filesystem::path(xdg_data) / "qcview" / "data";
        } else {
            const char* home = std::getenv("HOME");
            if (home) {
                data_dir = std::filesystem::path(home) / ".local" / "share" / "qcview" / "data";
            }
        }
        if (!data_dir.empty()) {
            try {
                std::filesystem::create_directories(data_dir);
                std::filesystem::current_path(data_dir);
                Debug::Log("Set working directory to: " + data_dir.string());
            } catch (const std::exception& e) {
                Debug::Log("Warning: Failed to set writable working directory: " + std::string(e.what()));
            }
        }
    }
    #endif

    // Single instance enforcement (Windows only)
    #ifdef _WIN32
    static HANDLE single_instance_mutex = CreateMutexW(NULL, TRUE, L"Local\\qcview_SingleInstanceMutex");

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Debug::Log("Another instance is already running - attempting to pass command to it");

        HWND existing_window = FindUmpWindow();

        if (existing_window) {
            Debug::Log("Found existing instance window - sending command");
            std::string args_to_send;
            for (int i = 1; i < argc; i++) {
                if (!args_to_send.empty()) args_to_send += "|";
                args_to_send += argv[i];
            }

            if (!args_to_send.empty()) {
                Debug::Log("Sending command to existing instance: " + args_to_send);

                COPYDATASTRUCT cds;
                cds.dwData = 1;
                cds.cbData = static_cast<DWORD>(args_to_send.length() + 1);
                cds.lpData = (PVOID)args_to_send.c_str();

                SendMessageW(existing_window, WM_COPYDATA, 0, (LPARAM)&cds);

                SetForegroundWindow(existing_window);
                ShowWindow(existing_window, SW_RESTORE);

                Debug::Log("Command sent successfully - exiting");
            } else {
                SetForegroundWindow(existing_window);
                ShowWindow(existing_window, SW_RESTORE);
                Debug::Log("No files to send - just bringing window to front");
            }
        } else {
            Debug::Log("Another instance starting - exiting silently");
        }

        if (single_instance_mutex) {
            CloseHandle(single_instance_mutex);
        }
        return 0;
    }
    #endif // _WIN32

    // Collect file paths from command-line arguments
    std::vector<std::string> initial_files;
    for (int i = 1; i < argc; i++) {  // Skip argv[0] (executable path)
        initial_files.push_back(argv[i]);
    }

    // Single instance enforcement (Linux via D-Bus)
    #if defined(__linux__) && defined(QCVIEW_HAS_SDBUS)
    qcview::SingleInstance single_instance;
    if (!single_instance.TryAcquire(initial_files)) {
        Debug::Log("Files sent to existing instance — exiting");
        return 0;
    }
    #endif

    Application app;

    #if defined(__linux__) && defined(QCVIEW_HAS_SDBUS)
    app.SetSingleInstance(&single_instance);
    #endif

    if (!app.Initialize(initial_files)) {
        return -1;
    }

    app.Run();
    app.Cleanup();

    return 0;
}
