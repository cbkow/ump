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

#include <glad/gl.h>

#include "../player/image_loader_interface.h"
#include "../player/pipeline_mode.h"
#include "../player/shared_memory_pool.h"
#include "../player/streaming_video_decoder.h"  // For SeekQuality enum

namespace ump {

// Forward declarations
struct OTIOClip;
struct OTIOTrack;
class TimelineFlattener;
class IImageLoader;

//=============================================================================
// Timeline Cache Key - Identifies a frame from a source clip
//=============================================================================

struct TimelineCacheKey {
    std::string source_path;    // Linked media file path
    int source_frame;           // Frame number within the source

    bool operator<(const TimelineCacheKey& other) const {
        if (source_path != other.source_path) return source_path < other.source_path;
        return source_frame < other.source_frame;
    }

    bool operator==(const TimelineCacheKey& other) const {
        return source_path == other.source_path && source_frame == other.source_frame;
    }
};

// Hash function for TimelineCacheKey (for unordered containers)
struct TimelineCacheKeyHash {
    std::size_t operator()(const TimelineCacheKey& key) const {
        // Combine path hash and frame number
        std::size_t h1 = std::hash<std::string>{}(key.source_path);
        std::size_t h2 = std::hash<int>{}(key.source_frame);
        return h1 ^ (h2 << 1);  // Simple hash combination
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
// Cached Frame - GPU texture + metadata
//=============================================================================

struct CachedFrame {
    GLuint texture_id = 0;
    int width = 0;
    int height = 0;
    size_t byte_size = 0;
};

//=============================================================================
// Per-Clip Loader Info - Cached loader and metadata per source file
//=============================================================================

struct ClipLoaderInfo {
    // For VIDEO clips: use streaming decoder (continuous decode + buffer)
    std::unique_ptr<StreamingVideoDecoder> video_decoder;

    // For IMAGE_SEQUENCE/EXR clips: use image loader (random access)
    std::unique_ptr<IImageLoader> image_loader;

    ClipMediaType media_type = ClipMediaType::UNKNOWN;
    PipelineMode pipeline_mode = PipelineMode::NORMAL;
    int width = 0;
    int height = 0;
    double fps = 24.0;
    int frame_count = 0;

    // Grace period tracking - prevent aggressive cleanup/recreate cycles
    std::chrono::steady_clock::time_point last_used_time;
};

//=============================================================================
// Timeline Cache Configuration (matches EXR cache pattern)
//=============================================================================

struct TimelineCacheConfig {
    // Read-ahead for smooth playback
    int readAheadFrames = 72;       // ~3 seconds ahead @ 24fps (was prefetch_ahead)

    // Read-behind for instant backward scrubbing
    double readBehindSeconds = 0.5; // Keep 0.5s behind playhead 

    int io_threads = 8;             // Number of background I/O threads
    double fps = 24.0;              // Timeline frame rate
    bool use_shared_pool = true;    // Use SharedMemoryPool for eviction

    // Cache size (used if not using shared pool)
    double cacheGB = 8.0;           // Local cache size in GB

    // Loop/wrap-around support (like EXR cache)
    bool enable_looping = true;     // Wrap cache around at timeline ends (default ON for timeline)

    // Max GPU textures to keep (safety cap - window eviction is primary limiter)
    int max_textures = 120;         // Should be > readAheadFrames + readBehindFrames

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
        TIMELINE_CACHE      // Green - cached frames
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
    void Initialize(const std::vector<OTIOTrack>& tracks,
                    TimelineFlattener* flattener,
                    double fps);

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

    // Statistics
    TimelineCacheStats GetStats() const;

    // Cache visualization (for progress bar)
    std::vector<TimelineCacheSegment> GetCacheSegments() const;

    // Check if initialized
    bool IsInitialized() const { return initialized_; }

    // Get total timeline duration (in seconds)
    double GetDuration() const { return timeline_duration_; }

    // Get source coordinates for a timeline frame (for debugging/UI)
    SourceCoords GetSourceCoords(int timeline_frame) const;

    // Check if a specific frame is ready for display (in cache or decoder buffer)
    // This is a lightweight check - doesn't trigger loading or modify state
    // Used by playback controller to decide whether to advance the timer
    bool HasFrameReady(int timeline_frame) const;

    // Get frame directly by source path and frame number (for slip/trim preview)
    // This bypasses the flattener and looks up cached source frames directly
    // Returns 0 if not cached, queues async load if not available
    GLuint GetSourceFrame(const std::string& source_path, int source_frame,
                          int& width, int& height);

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

private:
    //=========================================================================
    // Loop-around Helpers
    //=========================================================================

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

    //=========================================================================
    // SharedMemoryPool Integration
    //=========================================================================

    void RegisterWithPool(const TimelineCacheKey& key, size_t bytes);
    void TouchInPool(const TimelineCacheKey& key);
    void RemoveFromPool(const TimelineCacheKey& key);
    void OnEvicted(const TimelineCacheKey& key);

    //=========================================================================
    // State
    //=========================================================================

    bool initialized_ = false;
    TimelineCacheConfig config_;
    TimelineFlattener* flattener_ = nullptr;
    double timeline_duration_ = 0.0;
    int total_timeline_frames_ = 0;

    // Source clip loaders (one per unique source path)
    // Using shared_ptr so I/O workers can hold a reference that keeps loader alive
    std::map<std::string, std::shared_ptr<ClipLoaderInfo>> loaders_;
    mutable std::mutex loaders_mutex_;

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

    // Stuck detection - reset decoders if scrubbing seems stuck
    std::atomic<int> scrub_stuck_counter_{0};  // Incremented when no frame shown during scrub
    static constexpr int kScrubStuckThreshold = 50;   // ~500ms - soft reset (NORMAL quality seek)
    static constexpr int kScrubHardResetThreshold = 150;  // ~1.5s - hard reset (reopen video)
    std::chrono::steady_clock::time_point last_successful_frame_time_;
    std::atomic<int> soft_reset_count_{0};  // Track how many soft resets we've done

    // Actual frame size (calculated from first loaded frame)
    size_t actualFrameSize_ = 0;
    bool hasActualFrameSize_ = false;

    // Last good frame fallback (for visual continuity during frame misses)
    GLuint last_good_texture_ = 0;
    int last_good_width_ = 0;
    int last_good_height_ = 0;
    int last_good_frame_ = -1;  // Timeline frame number of last_good_texture_

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
};

} // namespace ump
