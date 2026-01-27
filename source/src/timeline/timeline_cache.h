#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <unordered_set>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <functional>
#include <chrono>
#include <cmath>

#include <glad/gl.h>

#ifdef _WIN32
#include <d3d11_1.h>
#include <wrl/client.h>
#endif

#include "../player/image_loader_interface.h"
#include "../player/pipeline_mode.h"
#include "../player/shared_memory_pool.h"
#include "../player/video_decoder_interface.h"  // For IVideoDecoder, SeekQuality enum
#include "../player/managed_video_decoder.h"    // For ManagedVideoDecoder (MULTI_TRACK mode)
#include "../player/image_sequence_decoder.h"   // For ImageSequenceDecoder
#include "../player/scrub_decoder.h"            // For ScrubDecoderManager (scrub-only decoders)
#include "cache_window_engine.h"                // Central circular cache engine
#include "timeline_types.h"                     // For TimelineSourceMode

namespace ump {

// Forward declarations
struct OTIOClip;
struct OTIOTrack;
class TimelineFlattener;
class IImageLoader;

//=============================================================================
// Timeline Cache Key - Identifies a cached frame by timeline position
// Simple approach: each timeline frame position has its own cache entry
//=============================================================================

struct TimelineCacheKey {
    int timeline_frame;             // Frame number on the timeline (unique per position)

    bool operator<(const TimelineCacheKey& other) const {
        return timeline_frame < other.timeline_frame;
    }

    bool operator==(const TimelineCacheKey& other) const {
        return timeline_frame == other.timeline_frame;
    }
};

// Hash function for TimelineCacheKey (for unordered containers)
struct TimelineCacheKeyHash {
    std::size_t operator()(const TimelineCacheKey& key) const {
        return std::hash<int>{}(key.timeline_frame);
    }
};

//=============================================================================
// Source Coordinates - Result of timeline-to-source mapping
//=============================================================================

struct SourceCoords {
    std::string source_path;
    int source_frame = -1;
    bool valid = false;

    // Clip info for context
    std::string clip_id;
    std::string clip_name;
    double clip_start_time = 0.0;
    double clip_duration = 0.0;
    double source_in = 0.0;  // For debugging source frame calculation
    double source_fps = 0.0; // For debugging FPS mismatch
    double source_duration = 0.0;  // For debugging frame count
};

//=============================================================================
// Clip Media Type - Auto-detected from file extension
//=============================================================================

enum class ClipMediaType {
    UNKNOWN,
    VIDEO,              // .mov, .mp4, .mxf, .avi, .mkv
    IMAGE_SEQUENCE,     // .tiff, .tif, .png, .jpg, .jpeg
    EXR_SEQUENCE        // .exr
};

// Detect media type from file path
ClipMediaType DetectMediaType(const std::string& path);

//=============================================================================
// Scrub State - For adaptive scrubbing optimization
//=============================================================================

enum class ScrubState {
    IDLE,       // Normal playback - use full quality
    SCRUBBING,  // Active scrub - use preview quality (fast keyframes)
    REFINING    // Post-scrub - decode to exact frame at full quality
};

//=============================================================================
// Aggressive Scrub Mode - For MULTI_TRACK/DUAL_VIEW responsive scrubbing
//=============================================================================

enum class AggressiveScrubMode {
    INACTIVE,           // Normal operation
    ACTIVE_SCRUBBING,   // User actively dragging - show best-guess immediately
    SETTLING            // User stopped - spawning fresh decoders, transitioning to normal
};

//=============================================================================
// Cached Frame - GPU texture + metadata
//=============================================================================

struct CachedFrame {
    // OpenGL texture (used on all platforms, or when D3D11 mode is disabled)
    GLuint texture_id = 0;

#ifdef _WIN32
    // D3D11 texture (used when full D3D11 rendering is enabled on Windows)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_d3d;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_d3d;
#endif

    int width = 0;
    int height = 0;
    size_t byte_size = 0;
};

//=============================================================================
// Per-Clip Loader Info - Cached loader and metadata per source file
//=============================================================================

struct ClipLoaderInfo {
    // For VIDEO clips in VIDEO_FILE mode: use IVideoDecoder (GStreamer)
    std::shared_ptr<IVideoDecoder> video_decoder;

    // For VIDEO clips in MULTI_TRACK/DUAL_VIEW mode: use ManagedVideoDecoder (FFmpeg spawn-and-abandon)
    std::unique_ptr<ManagedVideoDecoder> managed_decoder;

    // For IMAGE_SEQUENCE/EXR clips: use sequence decoder (ring buffer like video)
    std::unique_ptr<ImageSequenceDecoder> sequence_decoder;

    // Legacy: For image sequences without full metadata (falls back to per-file loading)
    std::unique_ptr<IImageLoader> image_loader;

    ClipMediaType media_type = ClipMediaType::UNKNOWN;
    PipelineMode pipeline_mode = PipelineMode::NORMAL;
    int width = 0;
    int height = 0;
    double fps = 24.0;
    int frame_count = 0;

    // Grace period tracking - prevent aggressive cleanup/recreate cycles
    std::chrono::steady_clock::time_point last_used_time;

    // VIDEO ONLY: Reusable GPU texture - no caching, just re-upload each frame
    // GStreamer's ring buffer is the cache, we just display frames directly
    GLuint video_texture = 0;
    int video_texture_width = 0;
    int video_texture_height = 0;
    GLenum video_texture_format = 0;  // Track internal format for HDR/High-Res switching

    //=========================================================================
    // Helper methods for unified decoder access
    //=========================================================================

    // Get frame from appropriate decoder
    std::shared_ptr<PixelData> GetFrame(int frame) {
        if (managed_decoder) return managed_decoder->GetFrame(frame);
        if (video_decoder) return video_decoder->GetFrame(frame);
        if (sequence_decoder) return sequence_decoder->GetFrame(frame);
        return nullptr;
    }

    // Get closest frame (for scrubbing fallback)
    std::shared_ptr<PixelData> GetClosestFrame(int frame, int* actual = nullptr) {
        if (managed_decoder) return managed_decoder->GetClosestFrame(frame, actual);
        if (video_decoder) return video_decoder->GetClosestFrame(frame, actual);
        if (sequence_decoder) return sequence_decoder->GetClosestFrame(frame, actual);
        return nullptr;
    }

    // Update playhead position
    void UpdatePlayhead(int frame, SeekQuality quality = SeekQuality::NORMAL, bool force = false, bool is_prefetch = false) {
        if (managed_decoder) managed_decoder->UpdatePlayhead(frame, quality, force, is_prefetch);
        if (video_decoder) video_decoder->UpdatePlayhead(frame, quality, force);
        if (sequence_decoder) sequence_decoder->UpdatePlayhead(frame, quality, force);
    }

    // Set playback mode
    void SetPlaybackMode(bool playing) {
        if (managed_decoder) managed_decoder->SetPlaybackMode(playing);
        // GStreamer handles playback/scrub mode internally
        // ImageSequenceDecoder doesn't need this - it handles frames independently
    }

    // Check if frame is buffered
    bool HasFrame(int frame) const {
        if (managed_decoder) return managed_decoder->HasFrame(frame);
        if (video_decoder) return video_decoder->HasFrame(frame);
        if (sequence_decoder) return sequence_decoder->HasFrame(frame);
        return false;
    }

    // Get buffered range
    void GetBufferedRange(int& start, int& end) const {
        if (managed_decoder) { managed_decoder->GetBufferedRange(start, end); return; }
        if (video_decoder) { video_decoder->GetBufferedRange(start, end); return; }
        if (sequence_decoder) { sequence_decoder->GetBufferedRange(start, end); return; }
        start = end = -1;
    }

    // Get buffer size
    int GetBufferSize() const {
        if (managed_decoder) return managed_decoder->GetBufferSize();
        if (video_decoder) return video_decoder->GetBufferSize();
        if (sequence_decoder) return sequence_decoder->GetBufferSize();
        return 0;
    }

    // Clear buffer
    void ClearBuffer() {
        if (managed_decoder) managed_decoder->ClearBuffer();
        if (video_decoder) video_decoder->ClearBuffer();
        if (sequence_decoder) sequence_decoder->ClearBuffer();
    }

    // Hard reset
    void HardReset(int frame) {
        if (managed_decoder) managed_decoder->HardReset(frame);
        if (video_decoder) video_decoder->HardReset(frame);
        if (sequence_decoder) sequence_decoder->HardReset(frame);
    }

    // Check if this is a buffered decoder (vs legacy per-file loader)
    bool HasBufferedDecoder() const {
        return managed_decoder != nullptr || video_decoder != nullptr || sequence_decoder != nullptr;
    }

    //=========================================================================
    // Demand-Driven Decode API (for CacheWindowEngine integration)
    //=========================================================================

    // Set which source frames are needed, in priority order
    void SetNeededFrames(const std::vector<int>& frames_by_priority) {
        if (managed_decoder) managed_decoder->SetNeededFrames(frames_by_priority);
        if (video_decoder) video_decoder->SetNeededFrames(frames_by_priority);
        // ImageSequenceDecoder doesn't need this - it decodes on demand
    }

    // Evict frames outside the given keep set
    void EvictOutsideWindow(const std::set<int>& keep_frames) {
        if (managed_decoder) managed_decoder->EvictOutsideWindow(keep_frames);
        if (video_decoder) video_decoder->EvictOutsideWindow(keep_frames);
        // ImageSequenceDecoder doesn't need this - it manages its own buffer
    }

    // Get frames currently in buffer as a set
    std::set<int> GetBufferedFramesSet() const {
        if (managed_decoder) return managed_decoder->GetBufferedFramesSet();
        if (video_decoder) return video_decoder->GetBufferedFramesSet();
        return std::set<int>{};
    }

    //=========================================================================
    // Shuttle Mode (FF/RW)
    //=========================================================================

    void SetShuttleMode(bool enabled, int direction) {
        if (managed_decoder) managed_decoder->SetShuttleMode(enabled, direction);
        if (video_decoder) video_decoder->SetShuttleMode(enabled);
        // ImageSequenceDecoder doesn't need shuttle mode - it's fast enough
    }

    bool IsShuttleMode() const {
        if (managed_decoder) return managed_decoder->IsShuttleMode();
        if (video_decoder) return video_decoder->IsShuttleMode();
        return false;
    }

    // For shuttle, get best available frame
    std::shared_ptr<PixelData> UpdateShuttle(int frame) {
        if (managed_decoder) return managed_decoder->UpdateShuttle(frame);
        if (video_decoder) return video_decoder->GetClosestFrame(frame, nullptr);
        return nullptr;
    }

    // Exit shuttle - return frame to snap to
    int ExitShuttle() {
        if (managed_decoder) return managed_decoder->ExitShuttle();
        // GStreamer - disable shuttle mode and return current position
        if (video_decoder) {
            video_decoder->SetShuttleMode(false);
            // Clear buffer of scrubbed frames so normal buffering can restart fresh
            video_decoder->ClearBuffer();
        }
        return 0;
    }

    //=========================================================================
    // Loop Boundaries - Forward to decoder
    //=========================================================================

    void SetLoopBoundaries(int loop_start, int loop_end) {
        if (managed_decoder) {
            managed_decoder->SetLoopBoundaries(loop_start, loop_end);
        }
        if (video_decoder) {
            video_decoder->SetLoopPoints(loop_start, loop_end);
        }
    }

    void ClearLoopBoundaries() {
        if (managed_decoder) {
            managed_decoder->ClearLoopBoundaries();
        }
        if (video_decoder) {
            video_decoder->ClearLoopPoints();
        }
    }
};

//=============================================================================
// Sequence Metadata - Stored per source path for creating ImageSequenceDecoder
//=============================================================================

struct SequenceMetadata {
    std::string directory;
    std::string pattern;
    int start_frame = 1;
    int end_frame = 1;
    std::string exr_layer;
    PipelineMode pipeline_mode = PipelineMode::NORMAL;
    bool valid = false;
};

//=============================================================================
// Timeline Cache Configuration (matches EXR cache pattern)
//=============================================================================

struct TimelineCacheConfig {
    // Read-ahead for smooth playback
    int readAheadFrames = 72;       // ~3 seconds ahead @ 24fps (was prefetch_ahead)

    // Read-behind for instant backward scrubbing
    double readBehindSeconds = 0.5; // Keep 0.5s behind playhead

    int io_threads = 1;             // GStreamer does decoding internally
    double fps = 24.0;              // Timeline frame rate
    bool use_shared_pool = true;    // Use SharedMemoryPool for eviction

    // Cache size (used if not using shared pool)
    double cacheGB = 8.0;           // Local cache size in GB

    // Loop/wrap-around support (like EXR cache)
    bool enable_looping = true;     // Wrap cache around at timeline ends (default ON for timeline)

    // Max GPU textures to keep (safety cap - window eviction is primary limiter)
    int max_textures = 120;         // Should be > readAheadFrames + readBehindFrames

    // Video pipeline mode (8-bit NORMAL or 16-bit HIGH_RES)
    PipelineMode pipeline_mode = PipelineMode::NORMAL;

    // Computed helpers
    int GetReadBehindFrames() const {
        return static_cast<int>(readBehindSeconds * fps);
    }

    int GetWindowSize() const {
        return readAheadFrames + GetReadBehindFrames();
    }
};

//=============================================================================
// Timeline Cache Statistics
//=============================================================================

struct TimelineCacheStats {
    int cached_frames = 0;
    int total_timeline_frames = 0;  // Total frames in timeline (for progress display)
    int pending_requests = 0;
    int pending_uploads = 0;    // Pixel data waiting for GPU upload
    int total_clips = 0;
    int linked_clips = 0;
    size_t cache_bytes = 0;
    size_t max_bytes = 0;
    int cache_hits = 0;
    int cache_misses = 0;
    int active_loaders = 0;  // Number of source file loaders (FFmpeg contexts)
    double timeline_duration = 0.0;  // Timeline duration in seconds

    // Texture leak detection
    int textures_created = 0;
    int textures_deleted = 0;
    int texture_balance = 0;   // created - deleted (should match cached_frames roughly)

    double GetHitRatio() const {
        int total = cache_hits + cache_misses;
        return total > 0 ? static_cast<double>(cache_hits) / total : 0.0;
    }
};

//=============================================================================
// Cache Segment - For progress bar visualization
//=============================================================================

struct TimelineCacheSegment {
    double start_time = 0.0;    // Start time in seconds
    double end_time = 0.0;      // End time in seconds
    float density = 1.0f;       // 0.0 = sparse, 1.0 = full coverage

    enum Type {
        TIMELINE_CACHE,     // Filled - actually cached frames (image sequences)
        VIDEO_BUFFER,       // Filled - frames in video decoder's ring buffer
        TARGET_WINDOW,      // Outline - frames the cache is trying to fill
        BOUNDARY_REGION     // Background - the loop boundary region (In/Out range)
    } type = TIMELINE_CACHE;
};

//=============================================================================
// Timeline Cache - Multi-source frame cache for timeline playback
//=============================================================================

class TimelineCache {
public:
    TimelineCache();
    ~TimelineCache();

    // Initialize with timeline data
    // tracks: OTIO tracks from TimelineView
    // flattener: TimelineFlattener for visibility queries
    // fps: Timeline frame rate
    // source_mode: Determines decoder backend selection
    //              VIDEO_FILE → GStreamer (single video, HW accel)
    //              MULTI_TRACK → FFmpeg (multiple simultaneous decoders)
    void Initialize(const std::vector<OTIOTrack>& tracks,
                    TimelineFlattener* flattener,
                    double fps,
                    TimelineSourceMode source_mode = TimelineSourceMode::MULTI_TRACK);

    // Shutdown and clean up
    void Shutdown();

    // Get frame for display (returns 0 if not ready)
    // Must be called from GL thread
    // If got_exact_frame is provided, it will be set to true only if the exact requested frame was returned
    // (false means a fallback/held frame was returned)
    GLuint GetFrame(int timeline_frame, int& width, int& height, bool* got_exact_frame = nullptr);

    // Request frame to be loaded (async)
    // Can be called from any thread
    void RequestFrame(int timeline_frame);

    // Update playhead position (triggers prefetching)
    // Call this when playhead moves
    void UpdatePlayhead(int timeline_frame, bool is_playing);

    // Process pending GPU uploads (must call from GL thread each frame)
    void ProcessPendingUploads();

    // Clear all cached frames
    void ClearCache();

    // Clear requests (call on seek to stop current prefetch)
    void ClearRequests();

    // Notify cache that timeline tracks have been edited
    // This clears stale requests and marks cache segments as dirty
    // The flattener should already be updated before calling this
    void NotifyTracksEdited();

    // Update timeline duration (call after edits that change duration)
    void UpdateDuration(double new_duration);

    // Clear mapping log (for debug logging after edit)
    void ClearMappingLog();

    // Configuration
    void SetConfig(const TimelineCacheConfig& config);
    TimelineCacheConfig GetConfig() const { return config_; }

    // Loop control (like EXR cache)
    void SetLooping(bool enabled);
    bool IsLooping() const { return config_.enable_looping; }

    // Loop boundaries - defines the circular cache window region
    // When set: cache wraps within [start, end]
    // When not set: cache wraps within [0, total_frames-1]
    void SetLoopBoundaries(int start_frame, int end_frame);
    void ClearLoopBoundaries();

    // Statistics
    TimelineCacheStats GetStats() const;

    // Cache visualization (for progress bar)
    // Returns segments of actually cached frames (within boundary range)
    std::vector<TimelineCacheSegment> GetCacheSegments() const;

    // Check if all content is video-only (skip cache bar for GStreamer video)
    // Returns true if all loaded clips use GStreamer video decoder
    bool IsVideoOnly() const;

    // Get video buffer segments for cache bar visualization
    // Returns segments representing what's currently buffered in video decoders
    // Each segment has start_time/end_time in seconds for timeline display
    std::vector<TimelineCacheSegment> GetVideoBufferSegments() const;

    // Returns the target cache window segments (what the cache is trying to fill)
    // Handles wrap-around: may return 2 segments if window crosses boundary edge
    std::vector<TimelineCacheSegment> GetTargetWindowSegments(int current_frame) const;

    // Returns the boundary region segment (In/Out loop range)
    // Returns empty if no custom boundaries set
    std::vector<TimelineCacheSegment> GetBoundarySegments() const;

    // Check if initialized
    bool IsInitialized() const { return initialized_; }

    // Get total timeline duration (in seconds)
    double GetDuration() const { return timeline_duration_; }

    // Get source coordinates for a timeline frame (for debugging/UI)
    SourceCoords GetSourceCoords(int timeline_frame) const;

    // Get pipeline mode for the clip at given timeline frame
    // Returns PipelineMode::NORMAL if no clip or clip not loaded
    PipelineMode GetClipPipelineMode(int timeline_frame) const;

    // Get resolution of the clip at given timeline frame
    // Returns 0 if no clip or clip not loaded
    int GetClipWidth(int timeline_frame) const;
    int GetClipHeight(int timeline_frame) const;

    // Check if a specific frame is ready for display (in cache or decoder buffer)
    // This is a lightweight check - doesn't trigger loading or modify state
    // Used by playback controller to decide whether to advance the timer
    bool HasFrameReady(int timeline_frame) const;

    // Get frame directly by source path and frame number (for slip/trim preview)
    // This bypasses the flattener and looks up cached source frames directly
    // Returns 0 if not cached, queues async load if not available
    GLuint GetSourceFrame(const std::string& source_path, int source_frame,
                          int& width, int& height);

    // Register sequence metadata for a source path
    // Call this when linking a clip to an image sequence
    // This enables ImageSequenceDecoder creation with proper buffering
    void RegisterSequenceMetadata(const std::string& source_path,
                                   const SequenceMetadata& metadata);

    // Request a specific source frame to be loaded (for slip/trim preview)
    void RequestSourceFrame(const std::string& source_path, int source_frame);

    // Set gap texture dimensions (creates black texture at this size)
    // Must be called from GL thread after Initialize()
    // This prevents OpenGL corruption when transitioning between clips and gaps
    void SetGapTextureDimensions(int width, int height);

    // Set canvas dimensions for consistent output sizing
    // All GetFrame() calls will report these dimensions regardless of actual frame size
    // This prevents flickering when clips have different resolutions
    void SetCanvasDimensions(int width, int height);

    // Get canvas dimensions
    int GetCanvasWidth() const { return canvas_width_; }
    int GetCanvasHeight() const { return canvas_height_; }

    //=========================================================================
    // Shuttle Mode (FF/RW) - For responsive UI during fast seek
    //=========================================================================

    // Enable/disable shuttle mode on all decoders
    // direction: -1 = rewind, +1 = fast forward
    void SetShuttleMode(bool enabled, int direction);
    bool IsShuttleMode() const { return shuttle_active_; }

    // Aggressive scrub mode (MULTI_TRACK/DUAL_VIEW only)
    // Fires best-guess frames immediately during scrub, settles on stop
    void SetAggressiveScrubMode(bool enabled);
    bool IsAggressiveScrubMode() const;

    // Get frame during shuttle mode - returns best available frame
    // Uses shuttle queue logic instead of normal cache lookup
    GLuint GetShuttleFrame(int timeline_frame, int& width, int& height);

    // Exit shuttle mode - returns snap frame, triggers exact decode
    int ExitShuttleMode();

    // Get effective boundary start (custom or 0)
    int GetBoundaryStart() const;

    // Get effective boundary end (custom or total_frames-1)
    int GetBoundaryEnd() const;

    // Get frames ahead of current position for throttle calculation
    // Returns frames in playback order, respecting boundary wrapping
    std::vector<int> GetAheadFrames(int current_frame, int count) const;

    // Get priority-sorted frame window from engine (for buffer-wait checks)
    // Returns [current, ahead1, ahead2, ..., behind1, behind2, ...]
    std::vector<int> GetPriorityFrameWindow() const;

    // Get number of read-ahead frames configured
    int GetReadAheadFrames() const { return config_.readAheadFrames; }

    void SetPipelineMode(PipelineMode mode);
    PipelineMode GetPipelineMode() const { return video_pipeline_mode_; }

    // FPS synchronization - propagate detected media FPS back to timeline
    void SetFPS(double fps);
    double GetDetectedFPS() const { return detected_media_fps_; }
    bool HasPendingFPSUpdate() const { return detected_media_fps_ > 0 &&
        std::abs(detected_media_fps_ - config_.fps) > 0.01; }

#ifdef _WIN32
    //=========================================================================
    // D3D11 Rendering Mode (Windows)
    //=========================================================================

    // Enable/disable full D3D11 rendering mode
    // When enabled, creates D3D11 textures instead of OpenGL textures
    void SetD3D11RenderingMode(bool enabled);
    bool IsD3D11RenderingMode() const { return use_d3d11_rendering_; }

    // Get frame for display with D3D11 SRV
    // Returns nullptr if not ready, uses D3D11 textures when in D3D11 mode
    ID3D11ShaderResourceView* GetFrameD3D11(int timeline_frame, int& width, int& height,
                                             bool* got_exact_frame = nullptr);
#endif

private:
    //=========================================================================
    // Loop-around Helpers
    //=========================================================================

    // Get the circular cache window - single source of truth for what frames to cache
    // Returns set of frame numbers that should be in cache for given playhead position
    std::set<int> GetCacheWindow(int current_frame) const;

    // Wrap or clamp frame index based on looping mode
    int WrapFrame(int frame) const;

    // Calculate distance between frames considering wrap-around
    int FrameDistance(int from, int to) const;

    //=========================================================================
    // Timeline-to-Source Mapping
    //=========================================================================

    // Convert timeline frame to source coordinates
    SourceCoords TimelineToSource(int timeline_frame) const;

    // Get or create loader for a source path (returns shared_ptr for thread safety)
    std::shared_ptr<ClipLoaderInfo> GetOrCreateLoader(const std::string& source_path);

    //=========================================================================
    // Background I/O Management (EXR-style)
    //=========================================================================

    void IOWorkerThread();          // Processes video_requests_ queue
    void CacheManagementThread();   // Runs every 10ms, fills bi-directionally

    // Process a single load request
    std::shared_ptr<PixelData> LoadPixels(const TimelineCacheKey& key);

    //=========================================================================
    // GPU Upload Management
    //=========================================================================

    struct PendingUpload {
        TimelineCacheKey key;
        std::shared_ptr<PixelData> pixels;
    };

    // Create GL texture from pixels (must be called from GL thread)
    GLuint CreateGLTexture(const std::shared_ptr<PixelData>& pixels);

#ifdef _WIN32
    // Create D3D11 texture from pixels (must be called from main thread)
    bool CreateD3D11Texture(const std::shared_ptr<PixelData>& pixels,
                            Microsoft::WRL::ComPtr<ID3D11Texture2D>& out_texture,
                            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& out_srv);
#endif

    //=========================================================================
    // SharedMemoryPool Integration
    //=========================================================================

    void RegisterWithPool(const TimelineCacheKey& key, size_t bytes);
    void TouchInPool(const TimelineCacheKey& key);
    void RemoveFromPool(const TimelineCacheKey& key);
    void OnEvicted(const TimelineCacheKey& key);

    //=========================================================================
    // Aggressive Scrub Mode Helpers (MULTI_TRACK/DUAL_VIEW only)
    // NOTE: GetDirectDecoderFrame and GetAggressiveScrubFrame have been removed.
    // Scrub handling is now integrated directly into GetFrame() for better performance.
    //=========================================================================

    // Handle settling phase - called from CacheManagementThread
    void HandleAggressiveScrubSettling();

    //=========================================================================
    // State
    //=========================================================================

    bool initialized_ = false;
    TimelineCacheConfig config_;
    TimelineFlattener* flattener_ = nullptr;
    double timeline_duration_ = 0.0;
    int total_timeline_frames_ = 0;

    // Source mode - determines decoder backend selection
    TimelineSourceMode source_mode_ = TimelineSourceMode::MULTI_TRACK;

    // Pipeline mode for video (bit depth / HDR)
    PipelineMode video_pipeline_mode_ = PipelineMode::NORMAL;

    // FPS detected from GStreamer - for pending update check (VIDEO_FILE mode)
    double detected_media_fps_ = 0.0;

#ifdef _WIN32
    // D3D11 rendering mode - when enabled, creates D3D11 textures instead of GL
    bool use_d3d11_rendering_ = false;
#endif

    // Central cache window engine - owns all circular math
    CacheWindowEngine cache_engine_;

    // Loop boundaries - defines the circular cache window region
    // -1 means "use default" (0 for start, total_frames-1 for end)
    std::atomic<int> loop_start_frame_{-1};
    std::atomic<int> loop_end_frame_{-1};

    // Source clip loaders (one per unique source path)
    // Using shared_ptr so I/O workers can hold a reference that keeps loader alive
    std::map<std::string, std::shared_ptr<ClipLoaderInfo>> loaders_;
    mutable std::mutex loaders_mutex_;

    // Sequence metadata (for creating ImageSequenceDecoder with proper buffering)
    // Registered via RegisterSequenceMetadata() when linking clips to sequences
    std::map<std::string, SequenceMetadata> sequence_metadata_;
    mutable std::mutex sequence_metadata_mutex_;

    // Frame cache (key -> texture)
    std::map<TimelineCacheKey, CachedFrame> frame_cache_;
    mutable std::mutex cache_mutex_;

    // Pending GPU uploads (filled by I/O threads, consumed by GL thread)
    std::deque<PendingUpload> pending_uploads_;
    std::unordered_set<TimelineCacheKey, TimelineCacheKeyHash> pending_uploads_set_;  // O(1) duplicate check
    mutable std::mutex upload_mutex_;

    // Request queue (frames to load) - managed by CacheThread like EXR
    std::deque<int> video_requests_;              // Timeline frames to load (FIFO order)
    std::unordered_set<int> video_requests_set_;  // O(1) duplicate check for video_requests_
    std::set<int> requests_in_progress_;          // Currently loading
    mutable std::mutex request_mutex_;
    std::condition_variable request_cv_;

    // Direct source frame requests (for slip/trim preview, bypasses flattener)
    struct DirectSourceRequest {
        std::string source_path;
        int source_frame;
    };
    std::deque<DirectSourceRequest> direct_source_requests_;
    std::set<std::string> direct_requests_in_progress_;  // "path:frame" strings

    //=========================================================================
    // Playhead Tracking (EXR cache pattern)
    //=========================================================================

    // Updated by UpdatePlayhead(), read by CacheThread
    std::atomic<int> current_frame_{0};
    std::atomic<bool> is_playing_{false};

    // Cache thread coordination (like EXR cache)
    int lastCacheUpdateFrame_ = -1;         // Last frame seen by cache thread
    int previousFrame_ = -1;                // For seek detection
    bool needsFillReset_ = false;           // Reset fill counters on seek

    // Post-seek boost (like EXR cache)
    std::atomic<int> cacheIterationCount_{0};
    int lastSeekFrame_ = -1;

    //=========================================================================
    // Adaptive Scrubbing State
    //=========================================================================

    std::atomic<ScrubState> scrub_state_{ScrubState::IDLE};
    std::chrono::steady_clock::time_point last_scrub_time_;
    int pending_refine_frame_ = -1;        // Updated during scrubbing (latest position)
    int active_refine_frame_ = -1;         // Locked in when entering REFINING (prevents race)
    std::chrono::steady_clock::time_point refine_start_time_;  // When REFINING started
    static constexpr int kScrubRefineDelayMs = 100;  // Wait 100ms after scrub stops before refining

    // Timing for frame tracking
    std::chrono::steady_clock::time_point last_successful_frame_time_;

    // Actual frame size (calculated from first loaded frame)
    size_t actualFrameSize_ = 0;
    bool hasActualFrameSize_ = false;

    // Last good frame fallback (for visual continuity during frame misses)
    GLuint last_good_texture_ = 0;
    int last_good_width_ = 0;
    int last_good_height_ = 0;
    int last_good_frame_ = -1;  // Timeline frame number of last_good_texture_
    std::string last_good_source_path_;  // Source path - invalidate on clip boundary cross

    // Post-edit state: After an edit, disable "closest frame" fallback briefly
    // This prevents showing stale frames from the decoder's pre-edit buffer
    std::atomic<bool> post_edit_pending_{false};
    std::chrono::steady_clock::time_point post_edit_time_;

    //=========================================================================
    // Threading
    //=========================================================================

    // I/O worker threads
    std::vector<std::thread> io_threads_;
    std::atomic<bool> io_running_{false};

    // Cache management thread (like EXR CacheThread - runs every 10ms)
    std::thread cache_thread_;
    std::atomic<bool> cache_running_{false};

    // Textures marked for deletion (delete on GL thread)
    std::vector<GLuint> textures_to_delete_;
    std::mutex delete_mutex_;

    //=========================================================================
    // Canvas Dimensions - Consistent output size for all frames
    // Prevents flickering when clips have different resolutions
    //=========================================================================

    int canvas_width_ = 0;
    int canvas_height_ = 0;

    //=========================================================================
    // Shuttle Mode State
    //=========================================================================

    bool shuttle_active_ = false;
    int shuttle_direction_ = 0;  // -1 = RW, +1 = FF
    GLuint shuttle_last_texture_ = 0;       // Raw frame from decoder
    int shuttle_last_width_ = 0;
    int shuttle_last_height_ = 0;
    GLuint shuttle_composited_texture_ = 0; // Final composited result (aspect ratio corrected)
    std::chrono::steady_clock::time_point shuttle_last_texture_time_;  // Rate limit texture updates

    //=========================================================================
    // Aggressive Scrub Mode State (MULTI_TRACK/DUAL_VIEW only)
    // Fires best-guess frames immediately during scrub, settles on stop
    //=========================================================================

    std::atomic<AggressiveScrubMode> aggressive_scrub_mode_{AggressiveScrubMode::INACTIVE};
    std::chrono::steady_clock::time_point aggressive_scrub_last_move_;
    static constexpr int kAggressiveScrubSettleDelayMs = 150;

    // Held frame for immediate display during aggressive scrub
    GLuint aggressive_held_texture_ = 0;
    int aggressive_held_width_ = 0;
    int aggressive_held_height_ = 0;
    int aggressive_held_frame_ = -1;
    std::string aggressive_held_source_;  // Invalidate on clip boundary

    // Dedicated scrub decoders - lightweight, detached from CacheWindowEngine
    // These are used exclusively during ACTIVE_SCRUBBING to prevent full window decodes
    ScrubDecoderManager scrub_decoders_;

    //=========================================================================
    // Letterbox Compositing - GPU-side aspect ratio preservation
    // Composites source frames onto canvas with letterbox/pillarbox
    //=========================================================================

    GLuint letterbox_shader_ = 0;
    GLuint letterbox_quad_vao_ = 0;
    GLuint letterbox_quad_vbo_ = 0;
    GLuint letterbox_fbo_ = 0;
    GLuint letterbox_output_texture_ = 0;
    int letterbox_output_width_ = 0;
    int letterbox_output_height_ = 0;
    GLenum letterbox_output_format_ = 0;  // Track format for HDR/High-Res switching

    // Initialize letterbox shader and resources (called on first use)
    void InitializeLetterboxShader();
    void CleanupLetterboxResources();

    // Composite source texture onto canvas with letterbox/pillarbox
    // Returns texture at canvas dimensions with aspect ratio preserved
    GLuint CompositeFrameToCanvas(GLuint source_texture, int src_w, int src_h);

    //=========================================================================
    // Gap Texture - Persistent black texture for timeline gaps
    //=========================================================================

    GLuint gap_texture_ = 0;
    int gap_texture_width_ = 0;
    int gap_texture_height_ = 0;

    // Create/delete gap texture (must be called from GL thread)
    void CreateGapTexture(int width, int height);
    void DeleteGapTexture();

    //=========================================================================
    // Statistics
    //=========================================================================

    std::atomic<int> cache_hits_{0};
    std::atomic<int> cache_misses_{0};

    // Cache visualization
    mutable std::atomic<bool> segments_dirty_{true};
    mutable std::vector<TimelineCacheSegment> cached_segments_;
    mutable std::mutex segments_mutex_;

    // Tape timecode handling: minimum source_in per source file
    // Used to calculate relative offsets when source_in contains tape timecode
    std::map<std::string, double> min_source_in_per_file_;
};

} // namespace ump
