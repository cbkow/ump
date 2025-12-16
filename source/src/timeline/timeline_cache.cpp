#include "timeline_cache.h"
#include "timeline_view.h"
#include "../player/image_loaders.h"
#include "../utils/debug_utils.h"

#include <algorithm>
#include <filesystem>
#include <set>

// FFmpeg for video probing
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace fs = std::filesystem;

namespace ump {

//=============================================================================
// Video File Probing
//=============================================================================

struct VideoProbeResult {
    bool valid = false;
    double fps = 0.0;
    double duration = 0.0;
    int width = 0;
    int height = 0;
};

static VideoProbeResult ProbeVideoFile(const std::string& path) {
    VideoProbeResult result;

    AVFormatContext* format_ctx = nullptr;
    if (avformat_open_input(&format_ctx, path.c_str(), nullptr, nullptr) < 0) {
        Debug::Log("VideoProbe: Failed to open " + path);
        return result;
    }

    if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
        Debug::Log("VideoProbe: Failed to find stream info for " + path);
        avformat_close_input(&format_ctx);
        return result;
    }

    // Find video stream
    int video_stream = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream < 0) {
        Debug::Log("VideoProbe: No video stream in " + path);
        avformat_close_input(&format_ctx);
        return result;
    }

    AVStream* stream = format_ctx->streams[video_stream];

    // Get dimensions
    result.width = stream->codecpar->width;
    result.height = stream->codecpar->height;

    // Get FPS from avg_frame_rate or r_frame_rate
    if (stream->avg_frame_rate.den > 0 && stream->avg_frame_rate.num > 0) {
        result.fps = av_q2d(stream->avg_frame_rate);
    } else if (stream->r_frame_rate.den > 0 && stream->r_frame_rate.num > 0) {
        result.fps = av_q2d(stream->r_frame_rate);
    } else {
        result.fps = 24.0;  // Fallback
    }

    // Get duration
    if (format_ctx->duration > 0) {
        result.duration = static_cast<double>(format_ctx->duration) / AV_TIME_BASE;
    } else if (stream->duration > 0) {
        result.duration = static_cast<double>(stream->duration) * av_q2d(stream->time_base);
    } else {
        result.duration = 0.0;
    }

    result.valid = true;

    Debug::Log("VideoProbe: " + path + " -> " +
               std::to_string(result.width) + "x" + std::to_string(result.height) +
               " @ " + std::to_string(result.fps) + " fps, " +
               std::to_string(result.duration) + "s");

    avformat_close_input(&format_ctx);
    return result;
}

//=============================================================================
// Media Type Detection
//=============================================================================

ClipMediaType DetectMediaType(const std::string& path) {
    if (path.empty()) return ClipMediaType::UNKNOWN;

    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Video formats
    if (ext == ".mov" || ext == ".mp4" || ext == ".mxf" ||
        ext == ".avi" || ext == ".mkv" || ext == ".m4v" ||
        ext == ".webm" || ext == ".wmv") {
        return ClipMediaType::VIDEO;
    }

    // EXR sequences
    if (ext == ".exr") {
        return ClipMediaType::EXR_SEQUENCE;
    }

    // Image sequences
    if (ext == ".tiff" || ext == ".tif" || ext == ".png" ||
        ext == ".jpg" || ext == ".jpeg" || ext == ".dpx") {
        return ClipMediaType::IMAGE_SEQUENCE;
    }

    return ClipMediaType::UNKNOWN;
}

//=============================================================================
// TimelineCache Implementation
//=============================================================================

TimelineCache::TimelineCache() {
    // EXR-style defaults
    config_.readAheadFrames = 72;       // 3 seconds @ 24fps
    config_.readBehindSeconds = 0.5;    // 0.5s behind for backward scrub
    config_.io_threads = 8;
    config_.fps = 24.0;
    config_.use_shared_pool = true;
    config_.cacheGB = 8.0;
}

TimelineCache::~TimelineCache() {
    Shutdown();
}

void TimelineCache::Initialize(const std::vector<OTIOTrack>& tracks,
                                TimelineFlattener* flattener,
                                double fps) {
    if (initialized_) {
        Shutdown();
    }

    flattener_ = flattener;
    config_.fps = fps;

    // Calculate timeline duration from tracks
    timeline_duration_ = 0.0;
    for (const auto& track : tracks) {
        for (const auto& clip : track.clips) {
            double clip_end = clip.start_time + clip.duration;
            if (clip_end > timeline_duration_) {
                timeline_duration_ = clip_end;
            }
        }
    }

    total_timeline_frames_ = static_cast<int>(timeline_duration_ * fps);

    Debug::Log("TimelineCache: Initializing for " + std::to_string(timeline_duration_) +
               "s timeline (" + std::to_string(total_timeline_frames_) + " frames) at " +
               std::to_string(fps) + " fps");

    // Reset state for EXR-style caching
    // Initialize to frame 0 so CacheManagementThread starts pre-warming immediately
    lastCacheUpdateFrame_ = 0;
    previousFrame_ = 0;
    lastSeekFrame_ = 0;
    current_frame_ = 0;
    needsFillReset_ = false;
    cacheIterationCount_ = 0;
    hasActualFrameSize_ = false;
    actualFrameSize_ = 0;

    // Reset scrub state
    scrub_state_ = ScrubState::IDLE;
    pending_refine_frame_ = -1;
    active_refine_frame_ = -1;
    scrub_stuck_counter_ = 0;
    last_successful_frame_time_ = std::chrono::steady_clock::now();

    // Start I/O worker threads
    io_running_ = true;
    for (int i = 0; i < config_.io_threads; ++i) {
        io_threads_.emplace_back(&TimelineCache::IOWorkerThread, this);
    }

    // Start cache management thread (EXR-style - runs every 10ms)
    cache_running_ = true;
    cache_thread_ = std::thread(&TimelineCache::CacheManagementThread, this);

    initialized_ = true;
    Debug::Log("TimelineCache: Initialized with " + std::to_string(config_.io_threads) +
               " I/O threads, readAhead=" + std::to_string(config_.readAheadFrames) +
               " frames, readBehind=" + std::to_string(config_.readBehindSeconds) + "s");
}

void TimelineCache::Shutdown() {
    if (!initialized_) return;

    Debug::Log("TimelineCache: Shutting down...");

    // Stop threads
    io_running_ = false;
    cache_running_ = false;
    request_cv_.notify_all();

    // Join I/O threads
    for (auto& t : io_threads_) {
        if (t.joinable()) t.join();
    }
    io_threads_.clear();

    // Join cache management thread
    if (cache_thread_.joinable()) {
        cache_thread_.join();
    }

    // Clear caches
    ClearCache();

    // Delete gap texture
    DeleteGapTexture();

    // Cleanup letterbox compositing resources
    CleanupLetterboxResources();

    // Clear loaders
    {
        std::lock_guard<std::mutex> lock(loaders_mutex_);
        loaders_.clear();
    }

    flattener_ = nullptr;
    initialized_ = false;

    Debug::Log("TimelineCache: Shutdown complete");
}

//=============================================================================
// Frame Access
//=============================================================================

GLuint TimelineCache::GetFrame(int timeline_frame, int& width, int& height, bool* got_exact_frame) {
    if (!initialized_) {
        if (got_exact_frame) *got_exact_frame = false;
        return 0;
    }

    // Helper to set output dimensions - uses canvas dimensions if set for consistency
    // This prevents flickering when clips have different resolutions
    auto setOutputDimensions = [this, &width, &height](int actual_w, int actual_h) {
        if (canvas_width_ > 0 && canvas_height_ > 0) {
            width = canvas_width_;
            height = canvas_height_;
        } else {
            width = actual_w;
            height = actual_h;
        }
    };

    // Helper to composite frame to canvas with letterbox/pillarbox if dimensions differ
    // Returns the composited texture (or original if no compositing needed)
    auto maybeComposite = [this](GLuint texture, int src_w, int src_h) -> GLuint {
        if (texture == 0) return 0;
        // Only composite if canvas is set and dimensions differ
        if (canvas_width_ > 0 && canvas_height_ > 0 &&
            (src_w != canvas_width_ || src_h != canvas_height_)) {
            return CompositeFrameToCanvas(texture, src_w, src_h);
        }
        return texture;
    };

    // Track whether we return the exact requested frame
    bool is_exact = false;

    SourceCoords coords = TimelineToSource(timeline_frame);
    if (!coords.valid) {
        // Gap or unlinked clip - return gap texture at consistent dimensions
        // This prevents OpenGL corruption from constant FBO resize on clip/gap transitions
        if (gap_texture_ != 0) {
            setOutputDimensions(gap_texture_width_, gap_texture_height_);
            return gap_texture_;
        }
        return 0;
    }

    TimelineCacheKey key{coords.source_path, coords.source_frame};

    // DEBUG: Periodic playback frame mapping log (every ~1 second during playback)
    static int playback_log_counter = 0;
    static int last_logged_frame = -1000;
    int frame_delta = std::abs(timeline_frame - last_logged_frame);
    if (frame_delta >= static_cast<int>(config_.fps)) {  // Log every ~1 second of playback
        playback_log_counter++;
        // Calculate expected source frames from duration to check for mismatch
        int expected_source_frames = coords.source_duration > 0 && coords.source_fps > 0 ?
            static_cast<int>(coords.source_duration * coords.source_fps) : -1;
        Debug::Log("TimelineCache PLAYBACK [" + std::to_string(playback_log_counter) + "]: " +
                   "timeline=" + std::to_string(timeline_frame) +
                   " -> source=" + std::to_string(coords.source_frame) +
                   " (timeline_fps=" + std::to_string(config_.fps) +
                   ", source_fps=" + std::to_string(coords.source_fps) +
                   ", source_dur=" + std::to_string(coords.source_duration) +
                   ", expected_frames=" + std::to_string(expected_source_frames) + ")");
        last_logged_frame = timeline_frame;
    }

    // DEBUG: Log the main display request unconditionally for first calls after edit
    static int display_req_count = 0;
    static bool saw_post_edit = false;
    bool is_post_edit = post_edit_pending_.load();

    if (is_post_edit) {
        saw_post_edit = true;
        display_req_count = 0;  // Reset counter on new edit
    }

    if (saw_post_edit && ++display_req_count <= 20) {
        Debug::Log("TimelineCache::GetFrame [DISPLAY REQ #" + std::to_string(display_req_count) +
                   "]: timeline=" + std::to_string(timeline_frame) +
                   " -> source=" + std::to_string(coords.source_frame) +
                   " (clip_id=" + coords.clip_id +
                   ", source_in=" + std::to_string(coords.source_in) +
                   ", clip_start=" + std::to_string(coords.clip_start_time) + ")");
    }

    // During scrubbing, always try decoder buffer first (freshest frame)
    // This ensures we show the most recently decoded frame immediately
    bool is_scrubbing = (scrub_state_.load() != ScrubState::IDLE);

    // CRITICAL: Invalidate last_good_texture_ if user has moved too far from it
    // This prevents showing a frame from position 100 when user is at position 500
    // The threshold should be large enough to tolerate normal decode latency but
    // small enough to prevent showing wildly wrong frames after big scrubs
    constexpr int kLastGoodMaxDistance = 48;  // ~2 seconds @ 24fps
    if (last_good_frame_ >= 0 && std::abs(timeline_frame - last_good_frame_) > kLastGoodMaxDistance) {
        // last_good_texture_ is too stale - invalidate it
        // Don't delete the texture if it's in the cache (would cause double-free)
        bool in_cache = false;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            for (const auto& [k, cf] : frame_cache_) {
                if (cf.texture_id == last_good_texture_) {
                    in_cache = true;
                    break;
                }
            }
        }
        if (!in_cache && last_good_texture_ != 0) {
            glDeleteTextures(1, &last_good_texture_);
        }
        last_good_texture_ = 0;
        last_good_width_ = 0;
        last_good_height_ = 0;
        last_good_frame_ = -1;
    }

    // Post-edit state tracking (for logging only now)
    // The decoder seek in NotifyTracksEdited() clears stale frames, so we can allow
    // GetClosestFrame fallback - the frames in the buffer are from the new position
    // Note: is_post_edit was already declared above for DISPLAY REQ logging
    if (is_post_edit) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - post_edit_time_).count();

        // DEBUG: Log post-edit GetFrame attempts (rate-limited)
        static int post_edit_log_count = 0;
        if (++post_edit_log_count <= 10 || post_edit_log_count % 30 == 0) {
            Debug::Log("TimelineCache::GetFrame [POST-EDIT " + std::to_string(elapsed) + "ms]: " +
                       "timeline=" + std::to_string(timeline_frame) +
                       " -> source=" + std::to_string(coords.source_frame) +
                       " (clip=" + coords.clip_name + ")");
        }

        constexpr int kPostEditGracePeriodMs = 500;
        if (elapsed >= kPostEditGracePeriodMs) {
            post_edit_pending_ = false;  // Grace period expired
            post_edit_log_count = 0;  // Reset for next edit
            Debug::Log("TimelineCache::GetFrame: Post-edit grace period expired");
        }
    }

    // Helper lambda to mark success and reset stuck counter
    auto markSuccess = [this]() {
        scrub_stuck_counter_ = 0;
        last_successful_frame_time_ = std::chrono::steady_clock::now();
        // NOTE: Don't clear post_edit_pending_ here - let the grace period expire naturally
        // or let the explicit timeout in the post-edit block handle it.
        // Clearing it here was causing the cache fill to start requesting read-behind frames
        // too early, before the buffer had a chance to fill forward.
    };

    // ALWAYS check cache first - even during scrubbing
    // This prevents creating duplicate textures for frames we already have
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = frame_cache_.find(key);
        if (it != frame_cache_.end()) {
            setOutputDimensions(it->second.width, it->second.height);
            cache_hits_++;
            markSuccess();

            // Track as last good frame for fallback (store actual dimensions for internal use)
            last_good_texture_ = it->second.texture_id;
            last_good_width_ = it->second.width;
            last_good_height_ = it->second.height;
            last_good_frame_ = timeline_frame;  // Track which frame this is
            if (got_exact_frame) *got_exact_frame = true;  // Cache hit = exact frame
            return maybeComposite(it->second.texture_id, it->second.width, it->second.height);
        }
    }

    if (is_scrubbing) {
        // Scrubbing path: try to get exact frame, fall back to closest
        std::shared_ptr<ClipLoaderInfo> loader_info;
        {
            std::lock_guard<std::mutex> lock(loaders_mutex_);
            auto loader_it = loaders_.find(key.source_path);
            if (loader_it != loaders_.end()) {
                loader_info = loader_it->second;
            }
        }
        if (loader_info && loader_info->video_decoder) {
            // Try exact frame first
            auto pixels = loader_info->video_decoder->GetFrame(key.source_frame);
            bool is_exact_frame = (pixels != nullptr);

            // If exact not available, get closest
            if (!pixels) {
                pixels = loader_info->video_decoder->GetClosestFrame(key.source_frame);
            }

            if (pixels) {
                // Check cache first for exact frames
                if (is_exact_frame) {
                    std::lock_guard<std::mutex> cache_lock(cache_mutex_);
                    auto it = frame_cache_.find(key);
                    if (it != frame_cache_.end()) {
                        setOutputDimensions(it->second.width, it->second.height);
                        cache_hits_++;
                        markSuccess();
                        last_good_texture_ = it->second.texture_id;
                        last_good_width_ = it->second.width;
                        last_good_height_ = it->second.height;
                        last_good_frame_ = timeline_frame;
                        if (got_exact_frame) *got_exact_frame = true;
                        return maybeComposite(it->second.texture_id, it->second.width, it->second.height);
                    }
                }

                // Cache and return exact frames
                if (is_exact_frame) {
                    GLuint texture = CreateGLTexture(pixels);
                    if (texture != 0) {
                        std::lock_guard<std::mutex> cache_lock(cache_mutex_);
                        auto existing = frame_cache_.find(key);
                        if (existing != frame_cache_.end()) {
                            glDeleteTextures(1, &texture);
                            setOutputDimensions(existing->second.width, existing->second.height);
                            cache_hits_++;
                            markSuccess();
                            last_good_texture_ = existing->second.texture_id;
                            last_good_width_ = existing->second.width;
                            last_good_height_ = existing->second.height;
                            last_good_frame_ = timeline_frame;
                            if (got_exact_frame) *got_exact_frame = true;
                            return maybeComposite(existing->second.texture_id, existing->second.width, existing->second.height);
                        }
                        CachedFrame cf;
                        cf.texture_id = texture;
                        cf.width = pixels->width;
                        cf.height = pixels->height;
                        cf.byte_size = pixels->pixels.size();
                        frame_cache_[key] = cf;

                        setOutputDimensions(pixels->width, pixels->height);
                        cache_hits_++;
                        markSuccess();

                        last_good_texture_ = texture;
                        last_good_width_ = pixels->width;
                        last_good_height_ = pixels->height;
                        last_good_frame_ = timeline_frame;
                        if (got_exact_frame) *got_exact_frame = true;
                        return maybeComposite(texture, pixels->width, pixels->height);
                    }
                }
                // For non-exact frames, fall through to use last_good_texture
            }
        }
    }

    // Playback path: Use same logic as scrubbing - just show whatever we have
    // This matches dual view behavior where MPV shows whatever it can decode
    // The timer is the source of truth for timecode, video can lag slightly
    if (!is_scrubbing) {
        std::shared_ptr<ClipLoaderInfo> loader_info;
        {
            std::lock_guard<std::mutex> lock(loaders_mutex_);
            auto loader_it = loaders_.find(key.source_path);
            if (loader_it != loaders_.end()) {
                loader_info = loader_it->second;
            }
        }
        if (loader_info && loader_info->video_decoder) {
            // Try exact frame first
            auto pixels = loader_info->video_decoder->GetFrame(key.source_frame);
            bool is_exact_frame = (pixels != nullptr);

            // If not available, get closest frame (like scrubbing does)
            if (!pixels) {
                pixels = loader_info->video_decoder->GetClosestFrame(key.source_frame);
            }

            if (pixels) {
                if (is_exact_frame) {
                    // Exact frame - cache it
                    std::lock_guard<std::mutex> cache_lock(cache_mutex_);
                    auto existing = frame_cache_.find(key);
                    if (existing != frame_cache_.end()) {
                        setOutputDimensions(existing->second.width, existing->second.height);
                        cache_hits_++;
                        markSuccess();
                        last_good_texture_ = existing->second.texture_id;
                        last_good_width_ = existing->second.width;
                        last_good_height_ = existing->second.height;
                        last_good_frame_ = timeline_frame;
                        if (got_exact_frame) *got_exact_frame = true;
                        return maybeComposite(existing->second.texture_id, existing->second.width, existing->second.height);
                    }

                    GLuint texture = CreateGLTexture(pixels);
                    if (texture != 0) {
                        CachedFrame cf;
                        cf.texture_id = texture;
                        cf.width = pixels->width;
                        cf.height = pixels->height;
                        cf.byte_size = pixels->pixels.size();
                        frame_cache_[key] = cf;

                        setOutputDimensions(pixels->width, pixels->height);
                        cache_hits_++;
                        markSuccess();
                        last_good_texture_ = texture;
                        last_good_width_ = pixels->width;
                        last_good_height_ = pixels->height;
                        last_good_frame_ = timeline_frame;
                        if (got_exact_frame) *got_exact_frame = true;
                        return maybeComposite(texture, pixels->width, pixels->height);
                    }
                } else {
                    // Closest frame (not exact) - don't create texture, just use last_good
                    // Creating textures for every closest frame causes oscillation
                    if (last_good_texture_ != 0) {
                        setOutputDimensions(last_good_width_, last_good_height_);
                        if (got_exact_frame) *got_exact_frame = false;
                        return maybeComposite(last_good_texture_, last_good_width_, last_good_height_);
                    }
                }
            }
        }
    }

    // Track stuck state during scrubbing
    if (is_scrubbing) {
        scrub_stuck_counter_++;
    }

    // FALLBACK: Return last good frame instead of black
    // This ensures visual continuity when cache/decoder is momentarily behind
    if (last_good_texture_ != 0) {
        setOutputDimensions(last_good_width_, last_good_height_);
        // Don't increment cache_misses_ - this is a soft miss (showing previous frame)
        if (got_exact_frame) *got_exact_frame = false;  // Fallback, not exact

        // DEBUG: Log when using fallback during post-edit
        if (is_post_edit) {
            static int fallback_count = 0;
            if (++fallback_count <= 5) {
                Debug::Log("TimelineCache::GetFrame [POST-EDIT]: Using last_good_texture fallback");
            }
        }
        return maybeComposite(last_good_texture_, last_good_width_, last_good_height_);
    }

    // DEBUG: Log hard miss during post-edit (no frame to show)
    if (is_post_edit) {
        static int hard_miss_count = 0;
        if (++hard_miss_count <= 5) {
            Debug::Log("TimelineCache::GetFrame [POST-EDIT]: HARD MISS - no frame available!");
        }
    }

    if (got_exact_frame) *got_exact_frame = false;  // No frame at all
    cache_misses_++;
    return 0;
}

//=============================================================================
// EXR-style Playhead Update (like DirectEXRCache::UpdateCurrentPosition)
//=============================================================================

void TimelineCache::UpdatePlayhead(int timeline_frame, bool is_playing) {
    auto now = std::chrono::steady_clock::now();

    // Detect seeks and cancel in-flight requests (like EXR cache)
    bool isSeek = false;
    {
        std::lock_guard<std::mutex> lock(request_mutex_);

        // Detect seek: jump > 20 frames
        if (previousFrame_ >= 0 && std::abs(timeline_frame - previousFrame_) > 20) {
            isSeek = true;
            needsFillReset_ = true;  // Tell cache thread to reset fill counters
        }

        // Detect scrub start: not playing + frame jump > 1
        // This detects user dragging the playhead
        if (!is_playing && previousFrame_ >= 0 && std::abs(timeline_frame - previousFrame_) > 1) {
            scrub_state_ = ScrubState::SCRUBBING;
            last_scrub_time_ = now;
            pending_refine_frame_ = timeline_frame;
        } else if (!is_playing && scrub_state_.load() == ScrubState::SCRUBBING) {
            // Already scrubbing - keep updating the target frame and time
            // This ensures we refine to the FINAL position, not an intermediate one
            last_scrub_time_ = now;
            pending_refine_frame_ = timeline_frame;
        } else if (is_playing) {
            // Playing - exit scrub mode
            ScrubState prev_state = scrub_state_.exchange(ScrubState::IDLE);

            // CRITICAL: If we were scrubbing and now starting playback, force a seek
            // to ensure the decoder is at the exact frame. Without this, the decoder
            // might be at a keyframe from the last scrub position, causing desync.
            if (prev_state == ScrubState::SCRUBBING || prev_state == ScrubState::REFINING) {
                SourceCoords coords = TimelineToSource(timeline_frame);
                if (coords.valid) {
                    std::shared_ptr<ClipLoaderInfo> loader_info;
                    {
                        std::lock_guard<std::mutex> lock(loaders_mutex_);
                        auto it = loaders_.find(coords.source_path);
                        if (it != loaders_.end() && it->second) {
                            loader_info = it->second;
                        }
                    }
                    if (loader_info && loader_info->video_decoder) {
                        // Force seek to exact frame before playback starts
                        loader_info->video_decoder->UpdatePlayhead(coords.source_frame, SeekQuality::NORMAL, true);
                        Debug::Log("TimelineCache: Play-from-scrub force seek to source frame " +
                                   std::to_string(coords.source_frame));
                    }
                }
            }
        }

        previousFrame_ = timeline_frame;
        lastCacheUpdateFrame_ = timeline_frame;
    }

    // Update atomic state
    current_frame_ = timeline_frame;
    is_playing_ = is_playing;

    // Wake up cache thread immediately (don't wait for next tick)
    // This ensures instant response on seeks and position updates
    request_cv_.notify_one();
}

void TimelineCache::RequestFrame(int timeline_frame) {
    // Request a specific timeline frame to be loaded
    // Thread-safe: can be called from any thread
    if (!initialized_) return;
    if (timeline_frame < 0 || timeline_frame >= total_timeline_frames_) return;

    SourceCoords coords = TimelineToSource(timeline_frame);
    if (!coords.valid) return;

    TimelineCacheKey key{coords.source_path, coords.source_frame};

    // Check if already cached
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (frame_cache_.find(key) != frame_cache_.end()) {
            return;  // Already cached
        }
    }

    // Check if already in progress or pending
    {
        std::lock_guard<std::mutex> lock(request_mutex_);

        if (requests_in_progress_.count(timeline_frame) > 0) return;

        for (int pending : video_requests_) {
            if (pending == timeline_frame) return;  // Already pending
        }

        // Add to request queue (at front for priority)
        video_requests_.push_front(timeline_frame);
    }

    // Wake up I/O workers
    request_cv_.notify_one();
}

// Static counters for texture leak detection
static std::atomic<int> s_textures_created{0};
static std::atomic<int> s_textures_deleted{0};

void TimelineCache::ProcessPendingUploads() {
    if (!initialized_) return;

    // Delete textures marked for deletion
    {
        std::lock_guard<std::mutex> lock(delete_mutex_);
        if (!textures_to_delete_.empty()) {
            int delete_count = static_cast<int>(textures_to_delete_.size());
            /*Debug::Log("TimelineCache: [GL DELETE] Deleting " + std::to_string(delete_count) +
                       " textures (IDs: " + std::to_string(textures_to_delete_[0]) +
                       (delete_count > 1 ? "..." : "") + ")");*/
            glDeleteTextures(static_cast<GLsizei>(delete_count),
                             textures_to_delete_.data());
            s_textures_deleted += delete_count;
            textures_to_delete_.clear();
        }
    }

    // Process pending uploads (limit per frame to avoid stalls)
    // Increase limit during playback to keep up with frame rate
    const int MAX_UPLOADS_PER_FRAME = is_playing_.load() ? 8 : 4;
    int uploads_done = 0;

    // Window-based eviction in CacheManagementThread is the primary limiter.
    // max_textures is a safety cap - should be >= window size (readAhead + readBehind).
    const size_t MAX_TEXTURES = static_cast<size_t>(config_.max_textures);

    while (uploads_done < MAX_UPLOADS_PER_FRAME) {
        PendingUpload upload;
        {
            std::lock_guard<std::mutex> lock(upload_mutex_);
            if (pending_uploads_.empty()) break;
            upload = std::move(pending_uploads_.front());
            pending_uploads_.pop_front();
            pending_uploads_set_.erase(upload.key);  // Keep set in sync
        }

        // Safety eviction: only if WAY over limit (window eviction handles normal case)
        // This prevents runaway memory if window eviction fails for some reason
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            while (frame_cache_.size() >= MAX_TEXTURES) {
                auto oldest = frame_cache_.begin();
                if (oldest != frame_cache_.end()) {
                    // Remove from pool
                    RemoveFromPool(oldest->first);

                    // Delete texture IMMEDIATELY (not queued) - we're on GL thread
                    if (oldest->second.texture_id != 0) {
                        glDeleteTextures(1, &oldest->second.texture_id);
                        s_textures_deleted++;
                    }

                    frame_cache_.erase(oldest);
                    segments_dirty_ = true;
                }
            }
        }

        // Now create GL texture
        GLuint texture = CreateGLTexture(upload.pixels);
        if (texture == 0) continue;

        // Add to cache
        CachedFrame frame;
        frame.texture_id = texture;
        frame.width = upload.pixels->width;
        frame.height = upload.pixels->height;
        frame.byte_size = upload.pixels->ByteSize();

        {
            std::lock_guard<std::mutex> lock(cache_mutex_);

            // FIX: Check if key already exists - delete old texture to prevent leak!
            // This can happen when duplicate requests slip through (race condition)
            auto existing = frame_cache_.find(upload.key);
            if (existing != frame_cache_.end()) {
                if (existing->second.texture_id != 0) {
                    // Delete old texture immediately (we're on GL thread)
                    glDeleteTextures(1, &existing->second.texture_id);
                    s_textures_deleted++;
                   /* Debug::Log("TimelineCache: [DUPLICATE] Deleted old texture " +
                               std::to_string(existing->second.texture_id) +
                               " for frame " + std::to_string(upload.key.source_frame));*/
                }
            }

            frame_cache_[upload.key] = frame;
        }

        // Log upload (periodically)
        static int total_uploads = 0;
        total_uploads++;
        if (total_uploads <= 5 || total_uploads % 50 == 0) {
            /*Debug::Log("TimelineCache: GPU upload #" + std::to_string(total_uploads) +
                       " - frame " + std::to_string(upload.key.source_frame) +
                       " (" + std::to_string(frame.width) + "x" + std::to_string(frame.height) + ")");*/
        }

        // Mark segments dirty for cache visualization
        segments_dirty_ = true;

        // Register with shared pool
        if (config_.use_shared_pool) {
            RegisterWithPool(upload.key, frame.byte_size);
        }

        uploads_done++;
    }
}

//=============================================================================
// Cache Management
//=============================================================================

void TimelineCache::ClearCache() {
    // Mark textures for deletion
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        std::lock_guard<std::mutex> delete_lock(delete_mutex_);

        for (const auto& [key, frame] : frame_cache_) {
            if (frame.texture_id != 0) {
                textures_to_delete_.push_back(frame.texture_id);
            }

            // Remove from shared pool
            if (config_.use_shared_pool) {
                auto pool_key = MakeTimelineKey(key.source_path, key.source_frame);
                SharedMemoryPool::Instance().RemoveEntry(pool_key);
            }
        }

        frame_cache_.clear();
    }

    // Mark segments dirty for cache visualization
    segments_dirty_ = true;

    // Clear pending uploads
    {
        std::lock_guard<std::mutex> lock(upload_mutex_);
        pending_uploads_.clear();
        pending_uploads_set_.clear();  // Keep set in sync
    }

    cache_hits_ = 0;
    cache_misses_ = 0;

    Debug::Log("TimelineCache: Cache cleared");
}

void TimelineCache::ClearRequests() {
    std::lock_guard<std::mutex> lock(request_mutex_);
    video_requests_.clear();
    video_requests_set_.clear();  // Keep set in sync
    requests_in_progress_.clear();
    Debug::Log("TimelineCache: Requests cleared (" +
               std::to_string(video_requests_.size()) + " pending, " +
               std::to_string(requests_in_progress_.size()) + " in progress)");
}

void TimelineCache::NotifyTracksEdited() {
    // Called when timeline tracks have been edited (move, trim, cut, delete)
    // The flattener has already been updated with new track data

    int current = current_frame_.load();
    Debug::Log("TimelineCache::NotifyTracksEdited: START at timeline frame " + std::to_string(current));

    // Set post-edit flag to prevent showing stale "closest frames" from decoder buffer
    post_edit_pending_ = true;
    post_edit_time_ = std::chrono::steady_clock::now();

    // Clear the mapping log so we get fresh debug output after the edit
    ClearMappingLog();

    // Clear all pending requests - they reference old timeline positions
    ClearRequests();

    // Mark cache segments as dirty for UI refresh
    segments_dirty_ = true;

    // Clear last_good_texture_ to prevent showing stale frames
    last_good_texture_ = 0;
    last_good_width_ = 0;
    last_good_height_ = 0;
    last_good_frame_ = -1;

    // Clear pending uploads - frames decoded before edit may be stale
    {
        std::lock_guard<std::mutex> lock(upload_mutex_);
        int uploads_cleared = static_cast<int>(pending_uploads_.size());
        pending_uploads_.clear();
        pending_uploads_set_.clear();  // Keep set in sync
        if (uploads_cleared > 0) {
            Debug::Log("TimelineCache: Cleared " + std::to_string(uploads_cleared) + " pending uploads on edit");
        }
    }

    // Clear the entire frame cache
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        int frames_cleared = static_cast<int>(frame_cache_.size());
        {
            std::lock_guard<std::mutex> delete_lock(delete_mutex_);
            for (const auto& [key, frame] : frame_cache_) {
                if (frame.texture_id != 0) {
                    textures_to_delete_.push_back(frame.texture_id);
                }
            }
        }
        frame_cache_.clear();
        Debug::Log("TimelineCache: Cleared " + std::to_string(frames_cleared) + " cached frames on edit");
    }

    // Get current frame and new source coordinates AFTER clearing cache
    // The flattener has been updated, so this should return the NEW mapping
    SourceCoords coords = TimelineToSource(current);
    Debug::Log("TimelineCache::NotifyTracksEdited: Timeline frame " + std::to_string(current) +
               " -> source " + (coords.valid ? coords.source_path + " frame " + std::to_string(coords.source_frame) : "(invalid)"));

    // Reset cache fill state
    {
        std::lock_guard<std::mutex> lock(request_mutex_);
        needsFillReset_ = true;
        lastCacheUpdateFrame_ = current;
    }

    // CRITICAL: Flush ALL decoder buffers, not just the current clip's decoder
    // After an edit, stale frames in any decoder's ring buffer are invalid because
    // timeline-to-source mappings have changed. The decoder might have frames that
    // would be returned by GetClosestFrame() but are no longer valid for this timeline position.
    {
        std::vector<std::shared_ptr<ClipLoaderInfo>> loaders_to_flush;
        {
            std::lock_guard<std::mutex> lock(loaders_mutex_);
            for (auto& [path, loader_info] : loaders_) {
                if (loader_info && loader_info->video_decoder) {
                    loaders_to_flush.push_back(loader_info);
                }
            }
        }

        int flushed_count = 0;
        for (auto& loader_info : loaders_to_flush) {
            // Force a hard reset on each decoder to completely clear its buffer
            // Use frame 0 as a placeholder - it will be re-seeked when needed
            loader_info->video_decoder->HardReset(0);
            flushed_count++;
        }
        if (flushed_count > 0) {
            Debug::Log("TimelineCache: Hard reset " + std::to_string(flushed_count) + " decoder(s) on edit");
        }
    }

    // Now seek the decoder for the CURRENT clip to the new position
    if (coords.valid) {
        std::shared_ptr<ClipLoaderInfo> loader_info;
        {
            std::lock_guard<std::mutex> lock(loaders_mutex_);
            auto it = loaders_.find(coords.source_path);
            if (it != loaders_.end()) {
                loader_info = it->second;
            }
        }
        if (loader_info && loader_info->video_decoder) {
            loader_info->video_decoder->UpdatePlayhead(coords.source_frame, SeekQuality::NORMAL, true);
            Debug::Log("TimelineCache: Forced decoder seek to " + coords.source_path +
                       " frame " + std::to_string(coords.source_frame) + " after edit");
        }

        // KICKSTART: Immediately queue the current frame as highest priority
        // This ensures the I/O workers start loading it right away
        {
            std::lock_guard<std::mutex> lock(request_mutex_);
            video_requests_.push_front(current);  // Current frame at front = highest priority
            Debug::Log("TimelineCache: Queued frame " + std::to_string(current) + " for immediate load");
        }
    }

    // Wake up I/O workers and cache management thread to start filling immediately
    request_cv_.notify_all();

    Debug::Log("TimelineCache::NotifyTracksEdited: END");
}

void TimelineCache::UpdateDuration(double new_duration) {
    double old_duration = timeline_duration_;
    int old_frames = total_timeline_frames_;

    timeline_duration_ = new_duration;
    total_timeline_frames_ = static_cast<int>(new_duration * config_.fps);

    if (old_frames != total_timeline_frames_) {
        // Mark segments dirty so cache bar recalculates with new duration
        segments_dirty_ = true;

        Debug::Log("TimelineCache::UpdateDuration: " + std::to_string(old_duration) + "s -> " +
                   std::to_string(new_duration) + "s (" + std::to_string(old_frames) + " -> " +
                   std::to_string(total_timeline_frames_) + " frames)");
    }
}

void TimelineCache::SetConfig(const TimelineCacheConfig& config) {
    bool need_restart = config.io_threads != config_.io_threads;
    config_ = config;

    if (need_restart && initialized_) {
        // Restart with new thread count
        std::vector<OTIOTrack> empty;  // Will use existing flattener
        double fps = config_.fps;
        Shutdown();
        // Note: Re-initialization should be done by caller with proper tracks
        Debug::Log("TimelineCache: Config changed, restart required");
    }
}

void TimelineCache::SetLooping(bool enabled) {
    config_.enable_looping = enabled;
    Debug::Log("TimelineCache: Looping " + std::string(enabled ? "enabled" : "disabled"));
}

// Helper: Wrap or clamp frame index based on looping mode
int TimelineCache::WrapFrame(int frame) const {
    if (total_timeline_frames_ <= 0) return 0;

    if (config_.enable_looping) {
        // Wrap around: frame -1 becomes last frame, frame N becomes 0
        return ((frame % total_timeline_frames_) + total_timeline_frames_) % total_timeline_frames_;
    } else {
        // Clamp to valid range
        return std::max(0, std::min(frame, total_timeline_frames_ - 1));
    }
}

// Helper: Calculate distance between frames considering wrap-around
int TimelineCache::FrameDistance(int from, int to) const {
    if (!config_.enable_looping || total_timeline_frames_ <= 0) {
        return std::abs(to - from);
    }

    // Calculate shortest distance considering wrap-around
    int forward = ((to - from) % total_timeline_frames_ + total_timeline_frames_) % total_timeline_frames_;
    int backward = ((from - to) % total_timeline_frames_ + total_timeline_frames_) % total_timeline_frames_;
    return std::min(forward, backward);
}

TimelineCacheStats TimelineCache::GetStats() const {
    TimelineCacheStats stats;

    // Timeline duration and total frames (for progress display)
    stats.timeline_duration = timeline_duration_;
    stats.total_timeline_frames = total_timeline_frames_;

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        stats.cached_frames = static_cast<int>(frame_cache_.size());

        for (const auto& [key, frame] : frame_cache_) {
            stats.cache_bytes += frame.byte_size;
        }
    }

    {
        std::lock_guard<std::mutex> lock(request_mutex_);
        stats.pending_requests = static_cast<int>(video_requests_.size() + requests_in_progress_.size());
    }

    {
        std::lock_guard<std::mutex> lock(upload_mutex_);
        stats.pending_uploads = static_cast<int>(pending_uploads_.size());
    }

    {
        std::lock_guard<std::mutex> lock(loaders_mutex_);
        stats.total_clips = static_cast<int>(loaders_.size());
        stats.linked_clips = stats.total_clips;  // All loaders are for linked clips
        stats.active_loaders = static_cast<int>(loaders_.size());
    }

    stats.cache_hits = cache_hits_.load();
    stats.cache_misses = cache_misses_.load();

    // Texture leak tracking
    stats.textures_created = s_textures_created.load();
    stats.textures_deleted = s_textures_deleted.load();
    stats.texture_balance = stats.textures_created - stats.textures_deleted;

    if (config_.use_shared_pool) {
        auto pool_stats = SharedMemoryPool::Instance().GetStats();
        stats.max_bytes = pool_stats.budget_bytes;
    }

    return stats;
}

std::vector<TimelineCacheSegment> TimelineCache::GetCacheSegments() const {
    // Return cached segments if not dirty
    if (!segments_dirty_.load()) {
        std::lock_guard<std::mutex> lock(segments_mutex_);
        return cached_segments_;
    }

    std::vector<TimelineCacheSegment> segments;

    if (!initialized_ || timeline_duration_ <= 0 || config_.fps <= 0) {
        return segments;
    }

    // Get total frames
    int total_frames = static_cast<int>(timeline_duration_ * config_.fps);
    if (total_frames <= 0) return segments;

    // Take snapshot of cache keys for iteration
    std::set<TimelineCacheKey> cached_keys;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        for (const auto& [key, frame] : frame_cache_) {
            if (frame.texture_id != 0) {
                cached_keys.insert(key);
            }
        }
    }

    if (cached_keys.empty()) {
        segments_dirty_ = false;
        std::lock_guard<std::mutex> lock(segments_mutex_);
        cached_segments_ = segments;
        return segments;
    }

    // Scan timeline frames and check if each is cached
    // Group consecutive cached frames into segments
    int segment_start = -1;
    int segment_end = -1;

    for (int frame = 0; frame <= total_frames; ++frame) {
        bool is_cached = false;

        // Check if this timeline frame maps to a cached source
        SourceCoords coords = TimelineToSource(frame);
        if (coords.valid) {
            TimelineCacheKey key{coords.source_path, coords.source_frame};
            is_cached = (cached_keys.find(key) != cached_keys.end());
        }

        if (is_cached) {
            if (segment_start < 0) {
                segment_start = frame;
            }
            segment_end = frame;
        } else {
            // End of cached segment
            if (segment_start >= 0) {
                TimelineCacheSegment seg;
                seg.type = TimelineCacheSegment::TIMELINE_CACHE;
                seg.start_time = segment_start / config_.fps;
                seg.end_time = (segment_end + 1) / config_.fps;
                seg.density = 1.0f;
                segments.push_back(seg);
                segment_start = -1;
            }
        }
    }

    // Don't forget the last segment
    if (segment_start >= 0) {
        TimelineCacheSegment seg;
        seg.type = TimelineCacheSegment::TIMELINE_CACHE;
        seg.start_time = segment_start / config_.fps;
        seg.end_time = (segment_end + 1) / config_.fps;
        seg.density = 1.0f;
        segments.push_back(seg);
    }

    // Cache the result
    segments_dirty_ = false;
    {
        std::lock_guard<std::mutex> lock(segments_mutex_);
        cached_segments_ = segments;
    }

    return segments;
}

SourceCoords TimelineCache::GetSourceCoords(int timeline_frame) const {
    return TimelineToSource(timeline_frame);
}

bool TimelineCache::HasFrameReady(int timeline_frame) const {
    if (!initialized_) return false;

    // Map timeline frame to source coordinates
    SourceCoords coords = TimelineToSource(timeline_frame);
    if (!coords.valid) {
        // Gap or unlinked - gaps are always "ready" (we show black)
        return true;
    }

    TimelineCacheKey key{coords.source_path, coords.source_frame};

    // Check 1: Is it in the GPU cache? (instant)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (frame_cache_.find(key) != frame_cache_.end()) {
            return true;
        }
    }

    // Check 2: Is it in the decoder buffer? (fast - no decode, just buffer check)
    {
        std::lock_guard<std::mutex> lock(loaders_mutex_);
        auto it = loaders_.find(key.source_path);
        if (it != loaders_.end() && it->second && it->second->video_decoder) {
            // Check if the exact frame is in the decoder's frame buffer
            // This does NOT trigger decoding - it just checks the ring buffer
            if (it->second->video_decoder->HasFrame(key.source_frame)) {
                return true;
            }
        }
    }

    return false;
}

GLuint TimelineCache::GetSourceFrame(const std::string& source_path, int source_frame,
                                      int& width, int& height) {
    if (source_path.empty()) return 0;

    TimelineCacheKey key{source_path, source_frame};

    // Check cache
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = frame_cache_.find(key);
        if (it != frame_cache_.end()) {
            width = it->second.width;
            height = it->second.height;
            return it->second.texture_id;
        }
    }

    // Not cached - queue async load
    RequestSourceFrame(source_path, source_frame);
    return 0;
}

void TimelineCache::RequestSourceFrame(const std::string& source_path, int source_frame) {
    if (source_path.empty()) return;

    TimelineCacheKey key{source_path, source_frame};

    // Check if already cached
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (frame_cache_.find(key) != frame_cache_.end()) {
            return;  // Already cached
        }
    }

    // Check if already requested
    std::string request_key = source_path + ":" + std::to_string(source_frame);
    {
        std::lock_guard<std::mutex> lock(request_mutex_);
        if (direct_requests_in_progress_.count(request_key) > 0) {
            return;  // Already in progress
        }
        direct_requests_in_progress_.insert(request_key);

        // Add to direct request queue (high priority - at front)
        DirectSourceRequest req;
        req.source_path = source_path;
        req.source_frame = source_frame;
        direct_source_requests_.push_front(req);
    }

    request_cv_.notify_one();
}

//=============================================================================
// Timeline-to-Source Mapping
//=============================================================================

// Static set for logging - cleared on edit to re-log mappings
static std::set<std::string> s_logged_clips;

void TimelineCache::ClearMappingLog() {
    s_logged_clips.clear();
    Debug::Log("TimelineCache: Cleared mapping log for fresh logging after edit");
}

SourceCoords TimelineCache::TimelineToSource(int timeline_frame) const {
    SourceCoords coords;
    coords.valid = false;

    if (!flattener_) return coords;

    double timestamp = static_cast<double>(timeline_frame) / config_.fps;

    // Get visible clip at this timestamp
    const OTIOClip* clip = flattener_->GetVisibleClipAtTime(timestamp);
    if (!clip) {
        // Debug: Log when no clip found (only every 24 frames to reduce spam)
        /*if (timeline_frame % 24 == 0) {
            Debug::Log("TimelineToSource: No clip at frame " + std::to_string(timeline_frame) +
                       " (t=" + std::to_string(timestamp) + "s)");
        }*/
        return coords;
    }

    // Skip gaps
    if (clip->is_gap) return coords;

    // Skip unlinked clips
    if (!clip->is_linked || clip->linked_path.empty()) {
       /* if (timeline_frame % 24 == 0) {
            Debug::Log("TimelineToSource: Unlinked clip '" + clip->name + "' at frame " +
                       std::to_string(timeline_frame));
        }*/
        return coords;
    }

    // Calculate source frame using source media's fps (not timeline fps)
    double clip_offset = timestamp - clip->start_time;

    // Use source fps if available, otherwise fall back to timeline fps
    double fps_for_frame_calc = (clip->source_fps > 0) ? clip->source_fps : config_.fps;

    // Handle Avid MXF subclips: EDL source_in contains absolute tape timecode,
    // but the MXF file itself starts at frame 0. Detect this by comparing
    // source_in to the actual probed source_duration. If source_in exceeds
    // the file's duration, it's a tape timecode and should be ignored.
    double source_time;
    bool source_in_is_tape_timecode = false;
    if (clip->source_duration > 0) {
        // We have probed duration - use it to detect tape timecodes
        source_in_is_tape_timecode = (clip->source_in > clip->source_duration);
    } else {
        // Fallback: no probed duration, use heuristic (source_in > 1 hour)
        // This preserves backward compatibility for unprobed clips
        source_in_is_tape_timecode = (clip->source_in > 3600.0);
    }

    if (source_in_is_tape_timecode) {
        // Tape timecode - MXF subclip starts at frame 0
        source_time = clip_offset;
    } else {
        // Valid source_in - use it
        source_time = clip->source_in + clip_offset;
    }

    // Use rounding (+ 0.5) to match decoder's PTS-based frame numbering
    int source_frame = static_cast<int>(source_time * fps_for_frame_calc + 0.5);

    // Clamp source_frame to valid range for the source media
    // This handles single-frame holds and prevents seeking beyond end of clip
    if (source_frame < 0) source_frame = 0;

    // CRITICAL: Also clamp to max source frame to handle frame holds
    // When a short clip (e.g., still image) is stretched longer than its source,
    // source_frame would otherwise increment beyond available frames, causing
    // endless cache misses and memory leaks as the decoder tries to find non-existent frames
    int original_source_frame = source_frame;
    if (clip->source_duration > 0 && clip->source_fps > 0) {
        int max_source_frame = static_cast<int>(clip->source_duration * clip->source_fps) - 1;
        // For single-frame sources, max_source_frame might be -1 due to truncation
        // In that case, treat it as frame 0 (there's at least one frame if file exists)
        if (max_source_frame < 0) max_source_frame = 0;
        if (source_frame > max_source_frame) {
            source_frame = max_source_frame;  // Hold on last frame
        }
    } else if (clip->source_duration > 0) {
        // Fallback: use timeline fps if source_fps not available
        int max_source_frame = static_cast<int>(clip->source_duration * config_.fps) - 1;
        // For single-frame sources, ensure at least frame 0
        if (max_source_frame < 0) max_source_frame = 0;
        if (source_frame > max_source_frame) {
            source_frame = max_source_frame;
        }
    } else if (clip->source_duration == 0 && clip->source_fps == 0) {
        // No source metadata available (e.g., MXF-wrapped stills from Avid)
        // Treat as single-frame still image - always use frame 0
        // The decoder will cache this single frame for the clip's duration
        source_frame = 0;
    }

    // Debug: Log mapping (only first time per clip ID, reset after edit)
    // Use clip->id instead of clip->name so we see mappings for ALL clips after a cut
    if (s_logged_clips.find(clip->id) == s_logged_clips.end()) {
        Debug::Log("TimelineToSource: id='" + clip->id + "' name='" + clip->name +
                   "' frame " + std::to_string(timeline_frame) +
                   " -> source frame " + std::to_string(source_frame) +
                   " (clip_start=" + std::to_string(clip->start_time) +
                   ", source_in=" + std::to_string(clip->source_in) +
                   ", source_fps=" + std::to_string(clip->source_fps) +
                   ", source_duration=" + std::to_string(clip->source_duration) + ")");
        if (original_source_frame != source_frame) {
            Debug::Log("TimelineToSource: CLAMPED from " + std::to_string(original_source_frame) +
                       " to " + std::to_string(source_frame) + " (max=" +
                       std::to_string(static_cast<int>(clip->source_duration * clip->source_fps) - 1) + ")");
        }
        s_logged_clips.insert(clip->id);
    }

    coords.source_path = clip->linked_path;
    coords.source_frame = source_frame;
    coords.valid = true;
    coords.clip_id = clip->id;
    coords.clip_name = clip->name;
    coords.clip_start_time = clip->start_time;
    coords.clip_duration = clip->duration;
    coords.source_in = clip->source_in;
    coords.source_fps = clip->source_fps;
    coords.source_duration = clip->source_duration;

    return coords;
}

std::shared_ptr<ClipLoaderInfo> TimelineCache::GetOrCreateLoader(const std::string& source_path) {
    // FAST PATH: Check if loader already exists (brief lock)
    {
        std::lock_guard<std::mutex> lock(loaders_mutex_);
        auto it = loaders_.find(source_path);
        if (it != loaders_.end()) {
            // Update last used time to prevent premature cleanup
            it->second->last_used_time = std::chrono::steady_clock::now();
            return it->second;  // Return shared_ptr copy
        }
    }
    // Lock released - allows other threads to proceed during slow initialization

    // SLOW PATH: Create new loader WITHOUT holding lock
    // This is critical for avoiding contention during FFmpeg decoder initialization (50-500ms)
    auto info = std::make_shared<ClipLoaderInfo>();
    info->media_type = DetectMediaType(source_path);
    info->last_used_time = std::chrono::steady_clock::now();

    switch (info->media_type) {
        case ClipMediaType::VIDEO: {
            // Create StreamingVideoDecoder for continuous decode + buffering
            // This is the slow operation (FFmpeg init) - lock NOT held!
            auto decoder = std::make_unique<StreamingVideoDecoder>(source_path);

            if (!decoder->Initialize()) {
                Debug::Log("TimelineCache: Failed to initialize StreamingVideoDecoder for " + source_path);
                return nullptr;
            }

            info->video_decoder = std::move(decoder);
            info->pipeline_mode = PipelineMode::NORMAL;
            info->width = info->video_decoder->GetWidth();
            info->height = info->video_decoder->GetHeight();
            info->fps = info->video_decoder->GetFPS();
            info->frame_count = info->video_decoder->GetFrameCount();
            break;
        }

        case ClipMediaType::EXR_SEQUENCE: {
            auto exr_loader = std::make_unique<EXRImageLoader>();
            info->image_loader = std::move(exr_loader);
            info->pipeline_mode = PipelineMode::ULTRA_HIGH_RES;  // Half-float for EXR
            info->image_loader->GetDimensions(source_path, info->width, info->height);
            break;
        }

        case ClipMediaType::IMAGE_SEQUENCE: {
            // Detect specific format
            std::string ext = fs::path(source_path).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            if (ext == ".tiff" || ext == ".tif") {
                info->image_loader = std::make_unique<TIFFImageLoader>();
                info->pipeline_mode = PipelineMode::HIGH_RES;
            } else if (ext == ".png") {
                info->image_loader = std::make_unique<PNGImageLoader>();
                info->pipeline_mode = PipelineMode::HIGH_RES;
            } else if (ext == ".jpg" || ext == ".jpeg") {
                info->image_loader = std::make_unique<JPEGImageLoader>();
                info->pipeline_mode = PipelineMode::NORMAL;
            } else {
                info->image_loader = std::make_unique<PNGImageLoader>();  // Default
                info->pipeline_mode = PipelineMode::NORMAL;
            }

            if (info->image_loader) {
                info->image_loader->GetDimensions(source_path, info->width, info->height);
            }
            break;
        }

        default:
            Debug::Log("TimelineCache: Unknown media type for " + source_path);
            return nullptr;
    }

    // RE-ACQUIRE LOCK to insert (with double-check for race condition)
    {
        std::lock_guard<std::mutex> lock(loaders_mutex_);

        // Double-check: another thread may have created it while we were initializing
        auto it = loaders_.find(source_path);
        if (it != loaders_.end()) {
            // Another thread won the race - discard our decoder and use theirs
            Debug::Log("TimelineCache: Discarding duplicate loader for " + source_path +
                       " (another thread won the race)");
            return it->second;
        }

        // We won the race - insert our loader
        loaders_[source_path] = info;

        // Log creation (moved here to only log the winner)
        if (info->media_type == ClipMediaType::VIDEO && info->video_decoder) {
            Debug::Log("TimelineCache: StreamingVideoDecoder created for " + source_path +
                       " (" + std::to_string(info->width) + "x" + std::to_string(info->height) +
                       " @ " + std::to_string(info->fps) + " fps, " +
                       std::to_string(info->frame_count) + " frames)");
        } else if (info->image_loader) {
            Debug::Log("TimelineCache: " + info->image_loader->GetLoaderName() +
                       " loader created for " + source_path +
                       " (" + std::to_string(info->width) + "x" + std::to_string(info->height) + ")");
        }
    }

    return info;
}

//=============================================================================
// Background I/O
//=============================================================================

//=============================================================================
// I/O Worker Thread (like EXR cache IOWorkerThread)
// Processes timeline frame requests from video_requests_ queue
//=============================================================================

void TimelineCache::IOWorkerThread() {
    Debug::Log("TimelineCache: I/O worker thread started");

    while (io_running_) {
        int timeline_frame = -1;

        // Get next frame to load
        {
            std::unique_lock<std::mutex> lock(request_mutex_);

            // Wait for work
            request_cv_.wait(lock, [this] {
                return !io_running_ || !video_requests_.empty() || !direct_source_requests_.empty();
            });

            if (!io_running_) break;

            // Priority: direct source requests first (for interactive slip/trim preview)
            if (!direct_source_requests_.empty()) {
                DirectSourceRequest direct_req = direct_source_requests_.front();
                direct_source_requests_.pop_front();
                lock.unlock();

                // Process direct source request
                TimelineCacheKey key{direct_req.source_path, direct_req.source_frame};
                std::string request_key = direct_req.source_path + ":" + std::to_string(direct_req.source_frame);

                // Check if already cached
                bool already_cached = false;
                {
                    std::lock_guard<std::mutex> cache_lock(cache_mutex_);
                    already_cached = (frame_cache_.find(key) != frame_cache_.end());
                }

                if (!already_cached) {
                    // For direct source requests (trim/slip preview), we need TRUE random access
                    // to any frame in the source. The StreamingVideoDecoder is optimized for
                    // playback and conflicts with cache management. Use VideoImageLoader instead
                    // for video files - it provides proper random access frame extraction.

                    ClipMediaType media_type = DetectMediaType(key.source_path);
                    std::shared_ptr<PixelData> pixels;

                    if (media_type == ClipMediaType::VIDEO) {
                        // For VIDEO: Use VideoImageLoader for random access (not StreamingVideoDecoder)
                        // This avoids conflicts with the main playback cache's decoder management
                        auto video_loader = std::make_unique<VideoImageLoader>(key.source_path, 24.0, 0.0);

                        if (video_loader->IsInitialized()) {
                            // LoadFrame expects frame number as string
                            std::string frame_str = std::to_string(key.source_frame);
                            pixels = video_loader->LoadFrame(frame_str, "", PipelineMode::NORMAL);

                            if (!pixels) {
                                Debug::Log("TimelineCache: Direct request failed for video frame " +
                                           std::to_string(key.source_frame) + " from " + key.source_path);
                            }
                        } else {
                            Debug::Log("TimelineCache: Failed to init VideoImageLoader for direct request: " +
                                       key.source_path);
                        }
                    } else {
                        // For IMAGE_SEQUENCE/EXR: normal random access via LoadPixels
                        pixels = LoadPixels(key);
                    }

                    if (pixels) {
                        // Queue for GPU upload (O(1) duplicate check + size limit)
                        std::lock_guard<std::mutex> upload_lock(upload_mutex_);

                        // CRITICAL: Limit pending uploads to prevent memory explosion
                        static constexpr size_t MAX_PENDING_UPLOADS = 16;
                        while (pending_uploads_.size() >= MAX_PENDING_UPLOADS) {
                            auto& oldest = pending_uploads_.front();
                            pending_uploads_set_.erase(oldest.key);
                            pending_uploads_.pop_front();
                        }

                        if (pending_uploads_set_.count(key) == 0) {
                            pending_uploads_.push_back({key, pixels});
                            pending_uploads_set_.insert(key);
                        }
                    }
                }

                // Remove from in-progress
                {
                    std::lock_guard<std::mutex> req_lock(request_mutex_);
                    direct_requests_in_progress_.erase(request_key);
                }

                segments_dirty_ = true;
                continue;
            }

            if (video_requests_.empty()) continue;

            // Get first request (FIFO - CacheThread adds in priority order)
            timeline_frame = video_requests_.front();
            video_requests_.pop_front();
            video_requests_set_.erase(timeline_frame);  // Keep set in sync

            // Mark as in progress
            requests_in_progress_.insert(timeline_frame);
        }

        if (timeline_frame < 0) continue;

        // Convert timeline frame to source coordinates
        SourceCoords coords = TimelineToSource(timeline_frame);
        if (!coords.valid) {
            std::lock_guard<std::mutex> lock(request_mutex_);
            requests_in_progress_.erase(timeline_frame);
            continue;
        }

        TimelineCacheKey key{coords.source_path, coords.source_frame};

        // Check if already cached (another thread might have loaded it)
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (frame_cache_.find(key) != frame_cache_.end()) {
                std::lock_guard<std::mutex> req_lock(request_mutex_);
                requests_in_progress_.erase(timeline_frame);
                continue;  // Already loaded
            }
        }

        // Check if already pending upload (O(1) lookup)
        {
            std::lock_guard<std::mutex> lock(upload_mutex_);
            if (pending_uploads_set_.count(key) > 0) {
                std::lock_guard<std::mutex> req_lock(request_mutex_);
                requests_in_progress_.erase(timeline_frame);
                continue;  // Already being uploaded
            }
        }

        //=====================================================================
        // CRITICAL: During post-edit, validate that the source frame is reachable
        // from the decoder's current position. After an edit, the timeline-to-source
        // mapping may have changed, and the read-ahead window may include source
        // frames that are BEFORE the decoder's buffer start (unreachable).
        //=====================================================================
        bool skip_unreachable = false;
        if (post_edit_pending_.load()) {
            // Get loader to check decoder buffer range
            auto loader_info = GetOrCreateLoader(key.source_path);
            if (loader_info && loader_info->video_decoder) {
                int buffer_start = -1, buffer_end = -1;
                loader_info->video_decoder->GetBufferedRange(buffer_start, buffer_end);

                // If buffer has frames and requested frame is before buffer start,
                // this request is unreachable - skip it
                if (buffer_start >= 0 && key.source_frame < buffer_start) {
                    skip_unreachable = true;
                    // Log once per unique unreachable frame (rate-limited)
                    static std::set<int> logged_unreachable;
                    if (logged_unreachable.find(key.source_frame) == logged_unreachable.end()) {
                        Debug::Log("TimelineCache: [POST-EDIT] Skip unreachable source frame " +
                                   std::to_string(key.source_frame) + " (buffer starts at " +
                                   std::to_string(buffer_start) + ")");
                        logged_unreachable.insert(key.source_frame);
                        // Clear log set occasionally to avoid unbounded growth
                        if (logged_unreachable.size() > 100) {
                            logged_unreachable.clear();
                        }
                    }
                }
            }
        }

        if (skip_unreachable) {
            std::lock_guard<std::mutex> lock(request_mutex_);
            requests_in_progress_.erase(timeline_frame);
            continue;  // Skip unreachable frame
        }

        // CRITICAL: Check if already cached BEFORE loading pixels
        // This prevents redundant decoding when frame was cached by another path
        {
            std::lock_guard<std::mutex> cache_lock(cache_mutex_);
            if (frame_cache_.find(key) != frame_cache_.end()) {
                // Already cached - skip loading
                std::lock_guard<std::mutex> lock(request_mutex_);
                requests_in_progress_.erase(timeline_frame);
                continue;
            }
        }

        // Load pixels
        auto pixels = LoadPixels(key);

        // Remove from in-progress
        {
            std::lock_guard<std::mutex> lock(request_mutex_);
            requests_in_progress_.erase(timeline_frame);
        }

        if (!pixels) {
            // During post-edit, if frame not ready, re-queue it at the FRONT
            // BUT only if the frame is still reachable from decoder's current position
            if (post_edit_pending_.load()) {
                // Re-check if frame is reachable before re-queuing
                bool should_requeue = true;
                auto loader_info = GetOrCreateLoader(key.source_path);
                if (loader_info && loader_info->video_decoder) {
                    int buffer_start = -1, buffer_end = -1;
                    loader_info->video_decoder->GetBufferedRange(buffer_start, buffer_end);
                    if (buffer_start >= 0 && key.source_frame < buffer_start) {
                        // Frame is before buffer start - unreachable, don't re-queue
                        should_requeue = false;
                    }
                }

                if (should_requeue) {
                    std::lock_guard<std::mutex> lock(request_mutex_);
                    // Check it's not already in queue
                    bool already_queued = false;
                    for (int f : video_requests_) {
                        if (f == timeline_frame) {
                            already_queued = true;
                            break;
                        }
                    }
                    if (!already_queued) {
                        video_requests_.push_front(timeline_frame);  // Priority re-queue
                    }
                    // Small delay to let decoder catch up
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
            continue;
        }

        // Track actual frame size (like EXR cache)
        if (!hasActualFrameSize_ && pixels) {
            actualFrameSize_ = pixels->ByteSize();
            hasActualFrameSize_ = true;
            Debug::Log("TimelineCache: Detected frame size: " +
                       std::to_string(actualFrameSize_ / (1024*1024)) + " MB");
        }

        // Queue for GPU upload (with duplicate check and size limit to prevent memory explosion)
        {
            std::lock_guard<std::mutex> lock(upload_mutex_);

            // CRITICAL: Limit pending uploads queue to prevent memory explosion during seek
            // 4K frames are ~33MB each. 16 pending uploads = ~500MB worst case.
            // During rapid seeking, I/O workers can queue faster than ProcessPendingUploads consumes.
            static constexpr size_t MAX_PENDING_UPLOADS = 16;

            // If queue is full, drop oldest entry to make room (FIFO eviction)
            while (pending_uploads_.size() >= MAX_PENDING_UPLOADS) {
                auto& oldest = pending_uploads_.front();
                pending_uploads_set_.erase(oldest.key);
                pending_uploads_.pop_front();
                // PixelData shared_ptr is released here, freeing memory
            }

            // Check if this key is already pending upload (O(1) lookup)
            if (pending_uploads_set_.count(key) == 0) {
                pending_uploads_.push_back({key, pixels});
                pending_uploads_set_.insert(key);
            }
        }

        // Mark segments dirty for visualization update
        segments_dirty_ = true;
    }

    Debug::Log("TimelineCache: I/O worker thread stopped");
}

//=============================================================================
// EXR-style Cache Management Thread (like DirectEXRCache::CacheThread)
// Runs every 10ms, handles bi-directional filling and eviction
//=============================================================================

void TimelineCache::CacheManagementThread() {
    Debug::Log("TimelineCache: Cache management thread started (EXR-style, 10ms interval)");

    const std::chrono::milliseconds interval(10);  // 100 ticks/second like EXR
    int iteration = 0;

    while (cache_running_) {
        // Wait with timeout (interruptible via request_cv_.notify_one())
        {
            std::unique_lock<std::mutex> lock(request_mutex_);
            request_cv_.wait_for(lock, interval);
        }

        if (!cache_running_) break;
        if (!initialized_ || total_timeline_frames_ <= 0) continue;

        iteration++;

        // Get current playback position
        int current_frame = -1;
        bool needsReset = false;
        {
            std::lock_guard<std::mutex> lock(request_mutex_);
            current_frame = lastCacheUpdateFrame_;
            needsReset = needsFillReset_;

            if (needsFillReset_) {
                needsFillReset_ = false;
                // Reset iteration to trigger post-edit boost (like seek)
                iteration = 1;
            }
        }

        if (current_frame < 0) continue;

        // Detect seeks and reset iteration counter for post-seek boost
        bool isSeek = needsReset;  // Track edit also triggers "seek-like" behavior
        if (lastSeekFrame_ >= 0 && std::abs(current_frame - lastSeekFrame_) > 20) {
            isSeek = true;
            iteration = 1;  // Reset for post-seek boost
        }
        lastSeekFrame_ = current_frame;
        cacheIterationCount_ = iteration;

        //=====================================================================
        // Step 0.5: Adaptive scrubbing - check for refinement opportunity
        //=====================================================================
        auto now = std::chrono::steady_clock::now();
        ScrubState current_scrub_state = scrub_state_.load();

        //=====================================================================
        // Stuck detection: If scrubbing but no frames showing, force reset
        // Two-tier: soft reset (NORMAL seek) first, then hard reset (reopen video)
        //=====================================================================
        if (current_scrub_state != ScrubState::IDLE) {
            int stuck_count = scrub_stuck_counter_.load();

            if (stuck_count > kScrubHardResetThreshold) {
                // HARD RESET: Reopen video file to completely reset FFmpeg state
                Debug::Log("TimelineCache: Scrub severely stuck (" + std::to_string(stuck_count) +
                           " misses), performing HARD RESET (reopen video)");

                // Force reset to IDLE
                scrub_state_ = ScrubState::IDLE;
                current_scrub_state = ScrubState::IDLE;
                scrub_stuck_counter_ = 0;
                soft_reset_count_ = 0;

                // Hard reset the decoder for the current position
                // Minimize lock hold time - collect shared_ptr, then call HardReset outside lock
                SourceCoords coords = TimelineToSource(current_frame);
                if (coords.valid) {
                    std::shared_ptr<ClipLoaderInfo> loader_to_reset;
                    {
                        std::lock_guard<std::mutex> lock(loaders_mutex_);
                        auto it = loaders_.find(coords.source_path);
                        if (it != loaders_.end() && it->second) {
                            loader_to_reset = it->second;
                        }
                    }
                    if (loader_to_reset && loader_to_reset->video_decoder) {
                        loader_to_reset->video_decoder->HardReset(coords.source_frame);
                    }
                }
            }
            else if (stuck_count > kScrubStuckThreshold) {
                // SOFT RESET: Just force a NORMAL quality seek
                soft_reset_count_++;
                Debug::Log("TimelineCache: Scrub appears stuck (" + std::to_string(stuck_count) +
                           " misses), soft reset #" + std::to_string(soft_reset_count_.load()) +
                           " with NORMAL quality seek");

                // Force reset to IDLE
                scrub_state_ = ScrubState::IDLE;
                current_scrub_state = ScrubState::IDLE;
                // Don't reset stuck_counter here - let it continue counting for hard reset

                // Force NORMAL quality seek on all active decoders to reset them
                // Minimize lock hold time - collect shared_ptrs, then update outside lock
                SourceCoords coords = TimelineToSource(current_frame);
                std::shared_ptr<ClipLoaderInfo> loader_to_update;
                if (coords.valid) {
                    std::lock_guard<std::mutex> lock(loaders_mutex_);
                    auto it = loaders_.find(coords.source_path);
                    if (it != loaders_.end() && it->second) {
                        loader_to_update = it->second;
                    }
                }
                if (loader_to_update && loader_to_update->video_decoder) {
                    loader_to_update->video_decoder->UpdatePlayhead(coords.source_frame, SeekQuality::NORMAL);
                }
            }
        } else {
            // Reset counters when not scrubbing
            if (scrub_stuck_counter_.load() > 0) {
                scrub_stuck_counter_ = 0;
            }
            soft_reset_count_ = 0;
        }

        if (current_scrub_state == ScrubState::SCRUBBING) {
            // Check if scrubbing has stopped (100ms of no movement)
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_scrub_time_).count();

            if (elapsed > kScrubRefineDelayMs) {
                // Scrubbing stopped - transition to REFINING state
                // IMPORTANT: Lock in the frame NOW to prevent race with UpdatePlayhead
                active_refine_frame_ = pending_refine_frame_;
                refine_start_time_ = now;
                scrub_state_ = ScrubState::REFINING;
                current_scrub_state = ScrubState::REFINING;

                // CRITICAL: Force a NORMAL quality seek to the exact frame!
                // During PREVIEW scrubbing, seeks are rate-limited and may have landed on
                // a keyframe far from the target. We must explicitly seek to get the exact frame.
                // Use force_seek=true to bypass ALL tolerance checks in UpdatePlayhead.
                SourceCoords refine_coords = TimelineToSource(active_refine_frame_);
                if (refine_coords.valid) {
                    std::shared_ptr<ClipLoaderInfo> loader_to_refine;
                    {
                        std::lock_guard<std::mutex> lock(loaders_mutex_);
                        auto it = loaders_.find(refine_coords.source_path);
                        if (it != loaders_.end() && it->second) {
                            loader_to_refine = it->second;
                        }
                    }
                    if (loader_to_refine && loader_to_refine->video_decoder) {
                        // force_seek=true bypasses all tolerance checks (buffer proximity, 24-frame threshold)
                        loader_to_refine->video_decoder->UpdatePlayhead(refine_coords.source_frame, SeekQuality::NORMAL, true);
                    }
                }

                // Log only occasionally to avoid spam (every ~1 second of scrub sessions)
                static int refine_count = 0;
                if (++refine_count % 10 == 1) {
                    Debug::Log("TimelineCache: Scrub refinement triggered (frame " +
                               std::to_string(active_refine_frame_) + ")");
                }
            }
        } else if (current_scrub_state == ScrubState::REFINING) {
            // Stay in REFINING until the frame is actually available
            // This keeps the fast path active in GetFrame()
            // Use active_refine_frame_ (locked in when entering REFINING) to avoid race condition
            int refine_frame = active_refine_frame_;
            int current = current_frame_.load();
            SourceCoords coords = TimelineToSource(refine_frame);
            bool frame_ready = false;

            // Timeout check: don't stay in REFINING forever (max 2 seconds from start)
            auto refine_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - refine_start_time_).count();
            constexpr int kMaxRefineTimeMs = 2000;

            // Obsolescence check: if user has moved significantly, this refinement is no longer relevant
            // (they'll re-enter SCRUBBING -> REFINING cycle for the new position)
            constexpr int kRefineObsoleteThreshold = 10;  // frames
            if (std::abs(refine_frame - current) > kRefineObsoleteThreshold) {
                Debug::Log("TimelineCache: Refinement obsolete - user moved from " +
                           std::to_string(refine_frame) + " to " + std::to_string(current));
                ScrubState expected = ScrubState::REFINING;
                scrub_state_.compare_exchange_strong(expected, ScrubState::IDLE);
                // Reset stuck counter so new position gets a fresh start
                scrub_stuck_counter_ = 0;
                // Don't continue with refinement check
            }
            else if (coords.valid && refine_elapsed < kMaxRefineTimeMs) {
                TimelineCacheKey key{coords.source_path, coords.source_frame};

                // Check if frame is in cache
                {
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    if (frame_cache_.find(key) != frame_cache_.end()) {
                        frame_ready = true;
                    }
                }

                // Check if frame is in decoder buffer (without uploading)
                if (!frame_ready) {
                    std::lock_guard<std::mutex> lock(loaders_mutex_);
                    auto loader_it = loaders_.find(key.source_path);
                    if (loader_it != loaders_.end() && loader_it->second &&
                        loader_it->second->video_decoder) {
                        frame_ready = loader_it->second->video_decoder->HasFrame(key.source_frame);
                    }
                }
            }

            if (frame_ready || refine_elapsed >= kMaxRefineTimeMs) {
                // Frame is available OR timeout reached - transition to IDLE
                // BUT only if user hasn't started scrubbing again (which sets state to SCRUBBING)
                ScrubState expected = ScrubState::REFINING;
                if (scrub_state_.compare_exchange_strong(expected, ScrubState::IDLE)) {
                    // Successfully transitioned from REFINING to IDLE
                    scrub_stuck_counter_ = 0;  // Reset stuck counter on successful refinement
                    if (frame_ready) {
                        Debug::Log("TimelineCache: Refinement complete, frame " +
                                   std::to_string(refine_frame) + " ready (user at " +
                                   std::to_string(current) + ")");
                    } else {
                        Debug::Log("TimelineCache: Refinement timeout, giving up on frame " +
                                   std::to_string(refine_frame));
                    }
                }
                // If compare_exchange failed, user started scrubbing again - let SCRUBBING handle it
            } else {
                // Frame not ready yet - keep in REFINING and ensure it's queued
                std::lock_guard<std::mutex> lock(request_mutex_);
                // Remove from in_progress if it was there (allow re-request)
                requests_in_progress_.erase(refine_frame);
                // Check if already in queue
                bool already_queued = false;
                for (int f : video_requests_) {
                    if (f == refine_frame) {
                        already_queued = true;
                        break;
                    }
                }
                if (!already_queued) {
                    video_requests_.push_front(refine_frame);
                }
            }
        }

        // Determine seek quality based on scrub state
        SeekQuality seek_quality = (current_scrub_state == ScrubState::SCRUBBING)
            ? SeekQuality::PREVIEW
            : SeekQuality::NORMAL;

        //=====================================================================
        // Step 1: Window-based eviction (like EXR cache)
        // Build set of keys that SHOULD stay, evict anything else
        //=====================================================================
        int readBehindFrames = config_.GetReadBehindFrames();
        int readAheadFrames = config_.readAheadFrames;

        {
            // Build set of keys that should stay in cache
            // When looping, the window wraps around timeline boundaries
            std::set<TimelineCacheKey> keys_to_keep;

            if (config_.enable_looping) {
                // Wrap-around window: go through each frame position with wrapping
                for (int i = -readBehindFrames; i <= readAheadFrames; i++) {
                    int frame = WrapFrame(current_frame + i);
                    SourceCoords coords = TimelineToSource(frame);
                    if (coords.valid) {
                        keys_to_keep.insert({coords.source_path, coords.source_frame});
                    }
                }
            } else {
                // Standard clamped window
                int window_start = std::max(0, current_frame - readBehindFrames);
                int window_end = std::min(total_timeline_frames_ - 1, current_frame + readAheadFrames);

                for (int frame = window_start; frame <= window_end; frame++) {
                    SourceCoords coords = TimelineToSource(frame);
                    if (coords.valid) {
                        keys_to_keep.insert({coords.source_path, coords.source_frame});
                    }
                }
            }

            // Find frames to evict (outside window)
            std::vector<TimelineCacheKey> keys_to_evict;
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                for (const auto& [key, frame] : frame_cache_) {
                    if (keys_to_keep.find(key) == keys_to_keep.end()) {
                        keys_to_evict.push_back(key);
                    }
                }
            }

            // Evict stale frames from cache
            int cache_evicted = 0;
            int textures_queued = 0;
            if (!keys_to_evict.empty()) {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                for (const auto& key : keys_to_evict) {
                    auto it = frame_cache_.find(key);
                    if (it != frame_cache_.end()) {
                        // Remove from pool first (clean removal, no callback)
                        RemoveFromPool(key);

                        // Queue texture for deletion
                        if (it->second.texture_id != 0) {
                            std::lock_guard<std::mutex> delete_lock(delete_mutex_);
                            textures_to_delete_.push_back(it->second.texture_id);
                            textures_queued++;
                        }

                        frame_cache_.erase(it);
                        cache_evicted++;
                    }
                }
                segments_dirty_ = true;

                //if (textures_queued > 0) {
                //    Debug::Log("TimelineCache: [EVICT] Queued " + std::to_string(textures_queued) +
                //               " textures for deletion, pending_delete=" +
                //               std::to_string(textures_to_delete_.size()));
                //}
            }

            // Also filter pending uploads - drop pixel data for frames outside window
            int uploads_dropped = 0;
            {
                std::lock_guard<std::mutex> lock(upload_mutex_);
                std::deque<PendingUpload> filtered;
                std::unordered_set<TimelineCacheKey, TimelineCacheKeyHash> filtered_set;
                for (auto& upload : pending_uploads_) {
                    if (keys_to_keep.find(upload.key) != keys_to_keep.end()) {
                        filtered.push_back(std::move(upload));
                        filtered_set.insert(upload.key);
                    } else {
                        uploads_dropped++;
                        // PixelData will be freed when upload goes out of scope
                    }
                }
                pending_uploads_ = std::move(filtered);
                pending_uploads_set_ = std::move(filtered_set);  // FIX: Keep set in sync
            }

            /*if ((cache_evicted > 0 || uploads_dropped > 0) && (iteration <= 5 || iteration % 100 == 0)) {
                Debug::Log("TimelineCache: [EVICT] cache=" + std::to_string(cache_evicted) +
                           " uploads=" + std::to_string(uploads_dropped) +
                           " window=[" + std::to_string(window_start) +
                           ", " + std::to_string(window_end) + "]");
            }*/
        }

        //=====================================================================
        // Step 1b: Cleanup unused loaders (free FFmpeg contexts)
        // A loader is unused if no frames in cache, pending_uploads_, or
        // request queues reference its source path
        //=====================================================================
        {
            // Collect active source paths (avoid holding multiple locks simultaneously)
            std::set<std::string> active_sources;

            // Check cached frames
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                for (const auto& [key, frame] : frame_cache_) {
                    active_sources.insert(key.source_path);
                }
            }

            // Check pending uploads
            {
                std::lock_guard<std::mutex> lock(upload_mutex_);
                for (const auto& upload : pending_uploads_) {
                    active_sources.insert(upload.key.source_path);
                }
            }

            // Check request queues (frames being loaded or queued for loading)
            {
                std::lock_guard<std::mutex> lock(request_mutex_);
                for (int tf : requests_in_progress_) {
                    SourceCoords coords = TimelineToSource(tf);
                    if (coords.valid) {
                        active_sources.insert(coords.source_path);
                    }
                }
                for (int tf : video_requests_) {
                    SourceCoords coords = TimelineToSource(tf);
                    if (coords.valid) {
                        active_sources.insert(coords.source_path);
                    }
                }
            }

            // CRITICAL: Also keep sources needed for the current playhead window
            // This prevents destroying StreamingVideoDecoders before they can produce frames
            {
                if (config_.enable_looping) {
                    // Wrap-around window
                    for (int i = -readBehindFrames; i <= readAheadFrames; i++) {
                        int frame = WrapFrame(current_frame + i);
                        SourceCoords coords = TimelineToSource(frame);
                        if (coords.valid) {
                            active_sources.insert(coords.source_path);
                        }
                    }
                } else {
                    // Standard clamped window
                    int window_start = std::max(0, current_frame - readBehindFrames);
                    int window_end = std::min(total_timeline_frames_ - 1, current_frame + readAheadFrames);
                    for (int frame = window_start; frame <= window_end; frame++) {
                        SourceCoords coords = TimelineToSource(frame);
                        if (coords.valid) {
                            active_sources.insert(coords.source_path);
                        }
                    }
                }
            }

            // Remove loaders not in active sources (immediate cleanup)
            // But only if they haven't been used recently (grace period)
            // Note: With shared_ptr, I/O workers hold a reference that keeps loader alive
            // even after removal from map, preventing use-after-free
            int loaders_cleaned = 0;
            {
                std::lock_guard<std::mutex> lock(loaders_mutex_);
                std::vector<std::string> to_remove;

                // Use much longer grace period during/after scrubbing to avoid decoder churn
                // Keeping decoders alive is cheap; recreating them is expensive
                int grace_period_ms = (current_scrub_state != ScrubState::IDLE) ? 30000 : 10000;

                for (const auto& [path, info] : loaders_) {
                    if (active_sources.find(path) == active_sources.end()) {
                        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - info->last_used_time).count();
                        if (age > grace_period_ms) {
                            to_remove.push_back(path);
                        }
                    }
                }

                for (const auto& path : to_remove) {
                    loaders_.erase(path);
                    loaders_cleaned++;
                }
            }

            /*if (loaders_cleaned > 0) {
                Debug::Log("TimelineCache: [CLEANUP] Freed " + std::to_string(loaders_cleaned) +
                           " unused loaders");
            }*/

            // CRITICAL: Clear buffers of decoders not in active_sources
            // This prevents memory accumulation from obscured clips (e.g., V1 under V2)
            // The decoder stays alive (grace period) but its buffer is cleared to free RAM
            {
                std::lock_guard<std::mutex> lock(loaders_mutex_);
                for (const auto& [path, info] : loaders_) {
                    if (active_sources.find(path) == active_sources.end()) {
                        // This loader is NOT needed for visible clips - clear its buffer
                        if (info->video_decoder) {
                            int buffer_size = info->video_decoder->GetBufferSize();
                            if (buffer_size > 0) {
                                info->video_decoder->ClearBuffer();
                                Debug::Log("TimelineCache: Cleared buffer (" + std::to_string(buffer_size) +
                                           " frames) for inactive source: " + path);
                            }
                        }
                    }
                }
            }

            // Periodic status log (every ~5 seconds)
            if (iteration % 500 == 0) {
                size_t loader_count = 0;
                {
                    std::lock_guard<std::mutex> lock2(loaders_mutex_);
                    loader_count = loaders_.size();
                }
                size_t cache_size = 0;
                {
                    std::lock_guard<std::mutex> lock2(cache_mutex_);
                    cache_size = frame_cache_.size();
                }
                size_t upload_size = 0;
                {
                    std::lock_guard<std::mutex> lock2(upload_mutex_);
                    upload_size = pending_uploads_.size();
                }
                int tex_created = s_textures_created.load();
                int tex_deleted = s_textures_deleted.load();
                int tex_balance = tex_created - tex_deleted;
               /* Debug::Log("TimelineCache: [STATUS] loaders=" + std::to_string(loader_count) +
                           " cached=" + std::to_string(cache_size) +
                           " uploads=" + std::to_string(upload_size) +
                           " tex_balance=" + std::to_string(tex_balance) +
                           " (created=" + std::to_string(tex_created) +
                           " deleted=" + std::to_string(tex_deleted) + ")");*/
            }
        }

        //=====================================================================
        // Step 1.5: Pre-warm video decoders and update playheads
        // This ensures decoders exist BEFORE we reach clips (smooth gap→clip transitions)
        // Only the CacheManagementThread updates playheads, not I/O workers
        //=====================================================================
        {
            // Find which video sources are in the upcoming window
            // and what frame each decoder should target
            std::map<std::string, int> decoder_targets;  // source_path -> target frame

            // CRITICAL: First, get the source frame for the CURRENT timeline position
            // This takes priority over readahead frames when multiple clips share a source file
            SourceCoords current_coords = TimelineToSource(current_frame);
            if (current_coords.valid) {
                decoder_targets[current_coords.source_path] = current_coords.source_frame;
            }

            // Use wrap-around aware window iteration for readahead
            auto processFrame = [&](int frame) {
                SourceCoords coords = TimelineToSource(frame);
                if (!coords.valid) return;

                // Track the earliest (closest to current) frame for each source
                // BUT: if we already have a target from the current frame, don't override it
                // (current frame takes priority for decoder positioning)
                auto it = decoder_targets.find(coords.source_path);
                if (it == decoder_targets.end()) {
                    decoder_targets[coords.source_path] = coords.source_frame;
                } else if (frame != current_frame) {
                    // Only consider overriding if this isn't the current frame
                    // Prefer frame closest to current playhead position
                    int distance_existing = FrameDistance(current_frame, it->second);
                    int distance_new = FrameDistance(current_frame, coords.source_frame);
                    if (distance_new < distance_existing) {
                        // Don't override if the existing target came from the current frame
                        // (we set that first above, and it has absolute priority)
                        if (current_coords.valid && coords.source_path == current_coords.source_path) {
                            // Skip - current frame's source takes priority
                        } else {
                            it->second = coords.source_frame;
                        }
                    }
                }
            };

            // During post-edit, skip read-behind window - decoder can only fill forward from seek position
            bool skip_read_behind_window = post_edit_pending_.load();
            int effective_read_behind = skip_read_behind_window ? 0 : readBehindFrames;

            if (config_.enable_looping) {
                // Wrap-around window
                for (int i = -effective_read_behind; i <= readAheadFrames; i++) {
                    processFrame(WrapFrame(current_frame + i));
                }
            } else {
                // Standard clamped window
                int window_start = std::max(0, current_frame - effective_read_behind);
                int window_end = std::min(total_timeline_frames_ - 1, current_frame + readAheadFrames);
                for (int frame = window_start; frame <= window_end; frame++) {
                    processFrame(frame);
                }
            }

            // Pre-create decoders for upcoming clips (before we reach them)
            // This ensures smooth gap→clip transitions
            for (const auto& [source_path, target_frame] : decoder_targets) {
                // GetOrCreateLoader will create the decoder if it doesn't exist
                // This happens asynchronously, so by the time we reach the clip,
                // the decoder should have started buffering
                GetOrCreateLoader(source_path);
            }

            // Update each decoder's playhead with appropriate quality
            // PREVIEW during scrubbing = fast keyframe decode
            // NORMAL during playback/refining = full quality
            //
            // IMPORTANT: Minimize lock hold time to reduce contention with GetFrame/I/O threads
            // Step 1: Collect shared_ptrs while holding lock (fast - just map lookups)
            // Step 2: Release lock, then update decoders (slow but safe - shared_ptr keeps alive)
            std::vector<std::pair<std::shared_ptr<ClipLoaderInfo>, int>> decoder_updates;
            {
                std::lock_guard<std::mutex> lock(loaders_mutex_);
                for (const auto& [source_path, target_frame] : decoder_targets) {
                    auto it = loaders_.find(source_path);
                    if (it != loaders_.end() && it->second) {
                        decoder_updates.emplace_back(it->second, target_frame);
                    }
                }
            }
            // Now update playheads WITHOUT holding lock
            // IMPORTANT: Skip playhead updates during post-edit grace period!
            // The decoder was force-seeked to the correct position in NotifyTracksEdited().
            // If we update playhead here with the read-behind window target, we'll set it
            // to a frame BEFORE the buffer start, causing endless cache misses.
            bool skip_playhead_update = post_edit_pending_.load();
            if (skip_playhead_update) {
                static int skip_count = 0;
                if (++skip_count <= 3) {
                    Debug::Log("TimelineCache: Skipping playhead update during post-edit grace period");
                }
            }

            for (auto& [loader_info, target_frame] : decoder_updates) {
                if (loader_info->video_decoder && !skip_playhead_update) {
                    // DEBUG: Log what target we're setting (rate-limited)
                    static int update_log_count = 0;
             /*       if (++update_log_count <= 5 || update_log_count % 100 == 0) {
                        Debug::Log("TimelineCache: CacheThread UpdatePlayhead target=" +
                                   std::to_string(target_frame) + " for decoder");
                    }*/
                    loader_info->video_decoder->UpdatePlayhead(target_frame, seek_quality);
                }
            }
        }

        //=====================================================================
        // Step 2: Fill cache bi-directionally (like EXR cache)
        //=====================================================================
        {
            std::lock_guard<std::mutex> lock(request_mutex_);

            // Limit concurrent requests
            const size_t MAX_CONCURRENT_REQUESTS = 32;
            size_t total_pending = video_requests_.size() + requests_in_progress_.size();

            if (total_pending >= MAX_CONCURRENT_REQUESTS) {
                continue;  // Too many requests pending
            }

            // Calculate batch size (larger on first iteration for post-seek boost)
            // IMPORTANT: During post-edit grace period, limit requests to just 1-2 frames
            // to ensure the current frame is prioritized and decoder can catch up
            bool is_post_edit = post_edit_pending_.load();
            int batch_limit;
            if (is_post_edit) {
                batch_limit = 2;  // Only request current frame + 1 during post-edit
            } else if (iteration == 1) {
                batch_limit = config_.io_threads * 4;  // Deep initial saturation
            } else {
                batch_limit = config_.readAheadFrames;  // Normal fill
            }

            int max_to_request = std::min(batch_limit,
                                          static_cast<int>(MAX_CONCURRENT_REQUESTS - total_pending));

            int requested_count = 0;

            // Fill read-ahead frames (priority for forward playback)
            for (int i = 0; i <= max_to_request && requested_count < max_to_request; i++) {
                int raw_frame = current_frame + i;

                // Wrap or clamp based on looping mode
                int frame;
                if (config_.enable_looping) {
                    frame = WrapFrame(raw_frame);
                } else {
                    // Skip frames outside bounds when not looping
                    if (raw_frame < 0 || raw_frame >= total_timeline_frames_) continue;
                    frame = raw_frame;
                }

                // Check if already cached
                SourceCoords coords = TimelineToSource(frame);
                if (!coords.valid) continue;

                TimelineCacheKey key{coords.source_path, coords.source_frame};
                {
                    std::lock_guard<std::mutex> cache_lock(cache_mutex_);
                    if (frame_cache_.find(key) != frame_cache_.end()) continue;
                }

                // Check if already pending/in-progress (O(1) lookups)
                if (requests_in_progress_.count(frame) > 0) continue;
                if (video_requests_set_.count(frame) > 0) continue;  // O(1) instead of O(N)

                // Add to request queue (keep deque and set in sync)
                video_requests_.push_back(frame);
                video_requests_set_.insert(frame);
                requested_count++;
            }

            // Fill read-behind frames (for backward scrubbing)
            // IMPORTANT: Skip read-behind during post-edit grace period!
            // After an edit, the decoder seeks to the current position and can only fill FORWARD.
            // Read-behind frames are before the decoder's buffer start and will never be available.
            // This prevents endless cache miss spam for unreachable frames.
            bool skip_read_behind = post_edit_pending_.load();
            for (int i = 1; i <= readBehindFrames && requested_count < max_to_request && !skip_read_behind; i++) {
                int raw_frame = current_frame - i;

                // Wrap or clamp based on looping mode
                int frame;
                if (config_.enable_looping) {
                    frame = WrapFrame(raw_frame);
                } else {
                    // Skip frames outside bounds when not looping
                    if (raw_frame < 0 || raw_frame >= total_timeline_frames_) continue;
                    frame = raw_frame;
                }

                // Check if already cached
                SourceCoords coords = TimelineToSource(frame);
                if (!coords.valid) continue;

                TimelineCacheKey key{coords.source_path, coords.source_frame};
                {
                    std::lock_guard<std::mutex> cache_lock(cache_mutex_);
                    if (frame_cache_.find(key) != frame_cache_.end()) continue;
                }

                // Check if already pending/in-progress (O(1) lookups)
                if (requests_in_progress_.count(frame) > 0) continue;
                if (video_requests_set_.count(frame) > 0) continue;  // O(1) instead of O(N)

                // Add to request queue (keep deque and set in sync)
                video_requests_.push_back(frame);
                video_requests_set_.insert(frame);
                requested_count++;
            }

            // Wake I/O threads if we added requests
            if (requested_count > 0) {
                request_cv_.notify_all();

                // Log periodically
                /*if (iteration <= 5 || iteration % 100 == 0) {
                    Debug::Log("TimelineCache: [FILL] iter=" + std::to_string(iteration) +
                               " frame=" + std::to_string(current_frame) +
                               " requested=" + std::to_string(requested_count) +
                               " pending=" + std::to_string(video_requests_.size()));
                }*/
            }
        }

        //=====================================================================
        // Step 3: Priority touching (like EXR cache)
        // Touch cached frames in REVERSE order so frames closest to playhead
        // stay in cache longest (LRU keeps most recently touched)
        //=====================================================================
        if (config_.use_shared_pool) {
            std::vector<TimelineCacheKey> keys_to_touch;

            // Build list of cached keys for frames near current position
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                for (int dist = readAheadFrames; dist >= 0; dist--) {
                    // Check ahead
                    int frame_ahead = current_frame + dist;
                    if (frame_ahead >= 0 && frame_ahead < total_timeline_frames_) {
                        SourceCoords coords = TimelineToSource(frame_ahead);
                        if (coords.valid) {
                            TimelineCacheKey key{coords.source_path, coords.source_frame};
                            if (frame_cache_.find(key) != frame_cache_.end()) {
                                keys_to_touch.push_back(key);
                            }
                        }
                    }

                    // Check behind
                    if (dist > 0) {
                        int frame_behind = current_frame - dist;
                        if (frame_behind >= 0 && frame_behind < total_timeline_frames_) {
                            SourceCoords coords = TimelineToSource(frame_behind);
                            if (coords.valid) {
                                TimelineCacheKey key{coords.source_path, coords.source_frame};
                                if (frame_cache_.find(key) != frame_cache_.end()) {
                                    keys_to_touch.push_back(key);
                                }
                            }
                        }
                    }
                }
            }

            // Touch in the order we built (furthest first, closest last)
            for (const auto& key : keys_to_touch) {
                TouchInPool(key);
            }
        }
    }

    Debug::Log("TimelineCache: Cache management thread stopped");
}

std::shared_ptr<PixelData> TimelineCache::LoadPixels(const TimelineCacheKey& key) {
    // Get shared_ptr to loader - this keeps it alive even if removed from map by cleanup
    auto loader_info = GetOrCreateLoader(key.source_path);
    if (!loader_info) {
        //Debug::Log("TimelineCache::LoadPixels: No loader for " + key.source_path);
        return nullptr;
    }

    std::shared_ptr<PixelData> result;

    if (loader_info->media_type == ClipMediaType::VIDEO) {
        // For VIDEO clips: use streaming decoder
        if (!loader_info->video_decoder) {
            return nullptr;
        }

        // NOTE: Don't call UpdatePlayhead here - that's done by CacheManagementThread
        // to avoid seek thrashing from multiple I/O threads.
        // Just get the frame from buffer (instant if buffered).
        result = loader_info->video_decoder->GetFrame(key.source_frame);

        // Frame not yet buffered - will be retried. This is normal during buffer fill.
    } else {
        // For IMAGE_SEQUENCE/EXR clips: use image loader
        if (!loader_info->image_loader) {
            //Debug::Log("TimelineCache::LoadPixels: No image loader for " + key.source_path);
            return nullptr;
        }

        result = loader_info->image_loader->LoadFrame(
            key.source_path,
            "",  // layer (used by EXR)
            loader_info->pipeline_mode
        );
    }

    //if (result) {
    //    // Log success (once per unique source path to reduce spam)
    //    static std::set<std::string> logged_success;
    //    if (logged_success.find(key.source_path) == logged_success.end()) {
    //        Debug::Log("TimelineCache: Loaded frame " + std::to_string(key.source_frame) +
    //                   " from " + key.source_path + " (" +
    //                   std::to_string(result->width) + "x" + std::to_string(result->height) + ")");
    //        logged_success.insert(key.source_path);
    //    }
    //}

    return result;
}

//=============================================================================
// GPU Upload
//=============================================================================

GLuint TimelineCache::CreateGLTexture(const std::shared_ptr<PixelData>& pixels) {
    if (!pixels || pixels->pixels.empty()) return 0;

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    // Set texture parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Upload pixel data
    GLenum internal_format = GL_RGBA8;
    if (pixels->gl_type == GL_HALF_FLOAT) {
        internal_format = GL_RGBA16F;
    } else if (pixels->gl_type == GL_UNSIGNED_SHORT) {
        internal_format = GL_RGBA16;
    }

    glTexImage2D(GL_TEXTURE_2D, 0, internal_format,
                 pixels->width, pixels->height, 0,
                 pixels->gl_format, pixels->gl_type,
                 pixels->pixels.data());

    glBindTexture(GL_TEXTURE_2D, 0);

    s_textures_created++;
    return texture;
}

//=============================================================================
// SharedMemoryPool Integration
//=============================================================================

void TimelineCache::RegisterWithPool(const TimelineCacheKey& key, size_t bytes) {
    auto pool_key = MakeTimelineKey(key.source_path, key.source_frame);

    SharedMemoryPool::Instance().RegisterEntry(
        pool_key,
        bytes,
        [this, key]() { OnEvicted(key); }
    );
}

void TimelineCache::TouchInPool(const TimelineCacheKey& key) {
    if (!config_.use_shared_pool) return;

    auto pool_key = MakeTimelineKey(key.source_path, key.source_frame);
    SharedMemoryPool::Instance().TouchEntry(pool_key);
}

void TimelineCache::RemoveFromPool(const TimelineCacheKey& key) {
    if (!config_.use_shared_pool) return;

    auto pool_key = MakeTimelineKey(key.source_path, key.source_frame);
    SharedMemoryPool::Instance().RemoveEntry(pool_key);  // Does NOT trigger callback
}

void TimelineCache::OnEvicted(const TimelineCacheKey& key) {
    // Called from SharedMemoryPool when this entry is evicted
    // Mark texture for deletion (can't delete GL texture from arbitrary thread)

    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = frame_cache_.find(key);
    if (it != frame_cache_.end()) {
        if (it->second.texture_id != 0) {
            std::lock_guard<std::mutex> delete_lock(delete_mutex_);
            textures_to_delete_.push_back(it->second.texture_id);
        }
        frame_cache_.erase(it);

        // Mark segments dirty for cache visualization
        segments_dirty_ = true;
    }
}

//=============================================================================
// Canvas Dimensions - Consistent output size for all frames
//=============================================================================

void TimelineCache::SetCanvasDimensions(int width, int height) {
    if (width > 0 && height > 0) {
        canvas_width_ = width;
        canvas_height_ = height;
        Debug::Log("TimelineCache::SetCanvasDimensions: " +
                   std::to_string(width) + "x" + std::to_string(height));
    }
}

//=============================================================================
// Gap Texture - Persistent black texture for timeline gaps
//=============================================================================

void TimelineCache::SetGapTextureDimensions(int width, int height) {
    // Public method to set/update gap texture dimensions
    // Delegates to CreateGapTexture which handles existing texture
    CreateGapTexture(width, height);
}

void TimelineCache::CreateGapTexture(int width, int height) {
    // Delete existing gap texture if different size
    if (gap_texture_ != 0) {
        if (gap_texture_width_ == width && gap_texture_height_ == height) {
            Debug::Log("TimelineCache::CreateGapTexture: Already have gap texture at " +
                       std::to_string(width) + "x" + std::to_string(height));
            return;  // Already have correct size
        }
        DeleteGapTexture();
    }

    if (width <= 0 || height <= 0) {
        Debug::Log("TimelineCache::CreateGapTexture: Invalid dimensions " +
                   std::to_string(width) + "x" + std::to_string(height));
        return;
    }

    // Create black pixel data (RGBA8)
    std::vector<unsigned char> black_pixels(width * height * 4, 0);
    // Set alpha to 255 for each pixel
    for (size_t i = 3; i < black_pixels.size(); i += 4) {
        black_pixels[i] = 255;
    }

    // Create OpenGL texture
    glGenTextures(1, &gap_texture_);
    glBindTexture(GL_TEXTURE_2D, gap_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, black_pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    gap_texture_width_ = width;
    gap_texture_height_ = height;

    Debug::Log("TimelineCache::CreateGapTexture: Created gap texture " +
               std::to_string(gap_texture_) + " at " +
               std::to_string(width) + "x" + std::to_string(height));
}

void TimelineCache::DeleteGapTexture() {
    if (gap_texture_ != 0) {
        Debug::Log("TimelineCache::DeleteGapTexture: Deleting gap texture " +
                   std::to_string(gap_texture_));
        glDeleteTextures(1, &gap_texture_);
        gap_texture_ = 0;
        gap_texture_width_ = 0;
        gap_texture_height_ = 0;
    }
}

//=============================================================================
// Letterbox Compositing - GPU-side aspect ratio preservation
//=============================================================================

void TimelineCache::InitializeLetterboxShader() {
    if (letterbox_shader_ != 0) return;  // Already initialized

    Debug::Log("TimelineCache::InitializeLetterboxShader: Compiling letterbox shader");

    // Vertex shader - fullscreen quad
    const char* vertex_shader_src = R"(
        #version 330 core
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aTexCoord;
        out vec2 TexCoord;
        void main() {
            gl_Position = vec4(aPos, 0.0, 1.0);
            TexCoord = aTexCoord;
        }
    )";

    // Fragment shader - letterbox compositing
    const char* fragment_shader_src = R"(
        #version 330 core
        in vec2 TexCoord;
        out vec4 FragColor;
        uniform sampler2D sourceTexture;
        uniform vec4 letterboxRect;  // x, y, width, height (normalized 0-1)

        void main() {
            float lx = letterboxRect.x;
            float ly = letterboxRect.y;
            float lw = letterboxRect.z;
            float lh = letterboxRect.w;

            // Check if outside letterbox area
            if (TexCoord.x < lx || TexCoord.x > (lx + lw) ||
                TexCoord.y < ly || TexCoord.y > (ly + lh)) {
                FragColor = vec4(0.0, 0.0, 0.0, 1.0);  // Black bars
                return;
            }

            // Map to source texture coordinates
            vec2 srcUV = (TexCoord - vec2(lx, ly)) / vec2(lw, lh);
            FragColor = texture(sourceTexture, srcUV);
        }
    )";

    // Compile vertex shader
    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &vertex_shader_src, nullptr);
    glCompileShader(vertex_shader);

    GLint success;
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetShaderInfoLog(vertex_shader, 512, nullptr, info_log);
        Debug::Log("TimelineCache: Vertex shader compilation failed: " + std::string(info_log));
        glDeleteShader(vertex_shader);
        return;
    }

    // Compile fragment shader
    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &fragment_shader_src, nullptr);
    glCompileShader(fragment_shader);

    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetShaderInfoLog(fragment_shader, 512, nullptr, info_log);
        Debug::Log("TimelineCache: Fragment shader compilation failed: " + std::string(info_log));
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        return;
    }

    // Link program
    letterbox_shader_ = glCreateProgram();
    glAttachShader(letterbox_shader_, vertex_shader);
    glAttachShader(letterbox_shader_, fragment_shader);
    glLinkProgram(letterbox_shader_);

    glGetProgramiv(letterbox_shader_, GL_LINK_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetProgramInfoLog(letterbox_shader_, 512, nullptr, info_log);
        Debug::Log("TimelineCache: Shader program linking failed: " + std::string(info_log));
        glDeleteProgram(letterbox_shader_);
        letterbox_shader_ = 0;
    }

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    // Create fullscreen quad VAO/VBO
    float quad_vertices[] = {
        // pos        // texcoord
        -1.0f,  1.0f,  0.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
        -1.0f,  1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f
    };

    glGenVertexArrays(1, &letterbox_quad_vao_);
    glGenBuffers(1, &letterbox_quad_vbo_);

    glBindVertexArray(letterbox_quad_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, letterbox_quad_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    Debug::Log("TimelineCache::InitializeLetterboxShader: Shader initialized successfully");
}

void TimelineCache::CleanupLetterboxResources() {
    if (letterbox_shader_ != 0) {
        glDeleteProgram(letterbox_shader_);
        letterbox_shader_ = 0;
    }
    if (letterbox_quad_vao_ != 0) {
        glDeleteVertexArrays(1, &letterbox_quad_vao_);
        letterbox_quad_vao_ = 0;
    }
    if (letterbox_quad_vbo_ != 0) {
        glDeleteBuffers(1, &letterbox_quad_vbo_);
        letterbox_quad_vbo_ = 0;
    }
    if (letterbox_fbo_ != 0) {
        glDeleteFramebuffers(1, &letterbox_fbo_);
        letterbox_fbo_ = 0;
    }
    if (letterbox_output_texture_ != 0) {
        glDeleteTextures(1, &letterbox_output_texture_);
        letterbox_output_texture_ = 0;
    }
    letterbox_output_width_ = 0;
    letterbox_output_height_ = 0;
}

GLuint TimelineCache::CompositeFrameToCanvas(GLuint source_texture, int src_w, int src_h) {
    if (source_texture == 0 || canvas_width_ <= 0 || canvas_height_ <= 0) {
        return source_texture;  // Can't composite, return original
    }

    // Initialize shader on first use
    if (letterbox_shader_ == 0) {
        InitializeLetterboxShader();
        if (letterbox_shader_ == 0) {
            Debug::Log("TimelineCache::CompositeFrameToCanvas: Shader init failed, returning original");
            return source_texture;
        }
    }

    // Create/resize output texture if needed
    if (letterbox_output_width_ != canvas_width_ || letterbox_output_height_ != canvas_height_) {
        // Delete old resources
        if (letterbox_output_texture_ != 0) {
            glDeleteTextures(1, &letterbox_output_texture_);
        }
        if (letterbox_fbo_ != 0) {
            glDeleteFramebuffers(1, &letterbox_fbo_);
        }

        // Create output texture
        glGenTextures(1, &letterbox_output_texture_);
        glBindTexture(GL_TEXTURE_2D, letterbox_output_texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, canvas_width_, canvas_height_, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);

        // Create FBO
        glGenFramebuffers(1, &letterbox_fbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, letterbox_fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               letterbox_output_texture_, 0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            Debug::Log("TimelineCache::CompositeFrameToCanvas: FBO incomplete!");
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            CleanupLetterboxResources();
            return source_texture;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        letterbox_output_width_ = canvas_width_;
        letterbox_output_height_ = canvas_height_;

        Debug::Log("TimelineCache::CompositeFrameToCanvas: Created output texture " +
                   std::to_string(canvas_width_) + "x" + std::to_string(canvas_height_));
    }

    // Save GL state
    GLint prev_fbo, prev_program, prev_viewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);

    // Calculate letterbox parameters
    float src_aspect = (float)src_w / (float)src_h;
    float canvas_aspect = (float)canvas_width_ / (float)canvas_height_;
    float rect_x, rect_y, rect_w, rect_h;

    if (src_aspect > canvas_aspect) {
        // Source is wider - letterbox (black bars top/bottom)
        rect_w = 1.0f;
        rect_h = canvas_aspect / src_aspect;
        rect_x = 0.0f;
        rect_y = (1.0f - rect_h) / 2.0f;
    } else {
        // Source is taller - pillarbox (black bars left/right)
        rect_h = 1.0f;
        rect_w = src_aspect / canvas_aspect;
        rect_x = (1.0f - rect_w) / 2.0f;
        rect_y = 0.0f;
    }

    // Bind FBO and set viewport
    glBindFramebuffer(GL_FRAMEBUFFER, letterbox_fbo_);
    glViewport(0, 0, canvas_width_, canvas_height_);

    // Clear to black
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Use letterbox shader
    glUseProgram(letterbox_shader_);

    // Set uniforms
    GLint rect_loc = glGetUniformLocation(letterbox_shader_, "letterboxRect");
    glUniform4f(rect_loc, rect_x, rect_y, rect_w, rect_h);

    GLint tex_loc = glGetUniformLocation(letterbox_shader_, "sourceTexture");
    glUniform1i(tex_loc, 0);

    // Bind source texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source_texture);

    // Render quad
    glBindVertexArray(letterbox_quad_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Restore GL state
    glUseProgram(prev_program);
    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);

    return letterbox_output_texture_;
}

} // namespace ump
