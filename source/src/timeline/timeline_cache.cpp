#include "timeline_cache.h"
#include "timeline_view.h"
#include "../player/image_loaders.h"
#include "../player/video_decoder_factory.h"
#include "../player/managed_video_decoder.h"
#include "../utils/debug_utils.h"

#ifdef _WIN32
#include "../gpu/d3d11_device_manager.h"
#endif

#include <algorithm>
#include <filesystem>
#include <set>

// FFmpeg for video probing
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

// User setting for I/O threads (default 8, configurable in Settings > Timeline)
extern int g_timeline_io_threads;

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

    // Handle URL schemes (exr://, mf://)
    std::string clean_path = path;

    // Check for exr:// URL scheme - indicates EXR sequence
    if (path.substr(0, 6) == "exr://") {
        return ClipMediaType::EXR_SEQUENCE;
    }

    // Check for mf:// URL scheme - indicates image sequence
    if (path.substr(0, 5) == "mf://") {
        // Extract extension from the path after mf://
        clean_path = path.substr(5);
        // Remove any query parameters (e.g., ?fps=24)
        size_t query_pos = clean_path.find('?');
        if (query_pos != std::string::npos) {
            clean_path = clean_path.substr(0, query_pos);
        }
    }

    std::string ext = fs::path(clean_path).extension().string();
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
    // GStreamer-optimized defaults
    // GStreamer handles internal threading/buffering, I/O worker just transfers between buffers
    config_.readAheadFrames = 72;       // 3 seconds @ 24fps
    config_.readBehindSeconds = 0.5;    // 0.5s behind for backward scrub
    config_.io_threads = 1;             // 1 thread sufficient - GStreamer does the actual decoding
    config_.fps = 24.0;
    config_.use_shared_pool = true;
    config_.cacheGB = 8.0;
}

TimelineCache::~TimelineCache() {
    Shutdown();
}

void TimelineCache::Initialize(const std::vector<OTIOTrack>& tracks,
                                TimelineFlattener* flattener,
                                double fps,
                                TimelineSourceMode source_mode) {
    if (initialized_) {
        Shutdown();
    }

    flattener_ = flattener;
    config_.fps = fps;
    source_mode_ = source_mode;

    // Adjust I/O threads based on source mode:
    // - VIDEO_FILE: GStreamer handles decoding internally - 1 thread sufficient
    // - MULTI_TRACK/DUAL_VIEW: FFmpeg needs multiple threads for parallel decode (user configurable)
    if (source_mode == TimelineSourceMode::VIDEO_FILE) {
        config_.io_threads = 1;
    } else {
        // Use user setting (default 8) for MULTI_TRACK/DUAL_VIEW modes
        config_.io_threads = g_timeline_io_threads;
    }

    Debug::Log("TimelineCache: Source mode = " +
               std::string(source_mode == TimelineSourceMode::VIDEO_FILE ? "VIDEO_FILE (GStreamer)" :
                          source_mode == TimelineSourceMode::MULTI_TRACK ? "MULTI_TRACK (FFmpeg)" :
                          source_mode == TimelineSourceMode::DUAL_VIEW ? "DUAL_VIEW (FFmpeg)" : "OTHER"));

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

    // Initialize cache window engine with timeline parameters
    cache_engine_.SetTotalFrames(total_timeline_frames_);
    cache_engine_.SetWindow(config_.GetReadBehindFrames(), config_.readAheadFrames);

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

    // Clear caches (marks textures for deletion)
    ClearCache();

    // CRITICAL: Actually delete the GL textures now!
    // ClearCache() only adds them to textures_to_delete_, but ProcessPendingUploads()
    // which normally handles deletion won't be called after shutdown.
    // We must delete textures here while we still have a valid GL context.
    {
        std::lock_guard<std::mutex> lock(delete_mutex_);
        if (!textures_to_delete_.empty()) {
            int delete_count = static_cast<int>(textures_to_delete_.size());
            Debug::Log("TimelineCache: [SHUTDOWN] Deleting " + std::to_string(delete_count) + " textures");
            glDeleteTextures(static_cast<GLsizei>(delete_count), textures_to_delete_.data());
            textures_to_delete_.clear();
        }
    }

    // Delete gap texture
    DeleteGapTexture();

    // Cleanup letterbox compositing resources
    CleanupLetterboxResources();

    // Cleanup aggressive scrub held texture
    if (aggressive_held_texture_ != 0) {
        glDeleteTextures(1, &aggressive_held_texture_);
        aggressive_held_texture_ = 0;
        aggressive_held_width_ = 0;
        aggressive_held_height_ = 0;
        aggressive_held_frame_ = -1;
        aggressive_held_source_.clear();
    }
    aggressive_scrub_mode_ = AggressiveScrubMode::INACTIVE;

    // Clear scrub decoders
    scrub_decoders_.ClearAll();

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

    // Clamp timeline_frame to valid range [0, total_frames-1]
    // This prevents black frames when scrubbing/seeking to the exact end of timeline
    if (total_timeline_frames_ > 0) {
        if (timeline_frame >= total_timeline_frames_) {
            timeline_frame = total_timeline_frames_ - 1;
        }
        if (timeline_frame < 0) {
            timeline_frame = 0;
        }
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

    //=========================================================================
    // AGGRESSIVE SCRUB PATH - For MULTI_TRACK/DUAL_VIEW responsive scrubbing
    // Bypass CacheWindowEngine entirely - go direct to decoder buffer
    // This avoids SetNeededFrames() calls which would trigger full-window pre-buffering
    //=========================================================================
    if ((source_mode_ == TimelineSourceMode::MULTI_TRACK ||
         source_mode_ == TimelineSourceMode::DUAL_VIEW) &&
        aggressive_scrub_mode_.load() == AggressiveScrubMode::ACTIVE_SCRUBBING) {

        // Update timestamp on any scrub movement
        aggressive_scrub_last_move_ = std::chrono::steady_clock::now();

        // Check clip boundary crossing - invalidate held frame if source changed
        if (!aggressive_held_source_.empty() && coords.source_path != aggressive_held_source_) {
            aggressive_held_frame_ = -1;
            aggressive_held_source_.clear();
        }

        // Rate limit texture updates to ~30fps to prevent GPU starvation on 4K+ content
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - shuttle_last_texture_time_).count();

        const int kMinTextureUpdateMs = 33;  // ~30fps max texture update rate
        if (elapsed_ms < kMinTextureUpdateMs && aggressive_held_texture_ != 0) {
            // Rate limited - return held texture
            setOutputDimensions(aggressive_held_width_, aggressive_held_height_);
            if (got_exact_frame) *got_exact_frame = false;
            return maybeComposite(aggressive_held_texture_, aggressive_held_width_, aggressive_held_height_);
        }

        // SCRUB PATH: Use dedicated ScrubDecoder (completely detached from CacheWindowEngine)
        // This prevents full window decode floods - ScrubDecoder has small buffer (15 frames)
        // and uses keyframe-only seeks for fast response
        ScrubDecoder* scrub_decoder = scrub_decoders_.GetDecoder(coords.source_path);

        if (scrub_decoder) {
            // Try exact frame first (might be buffered from recent decode)
            auto pixels = scrub_decoder->GetFrame(coords.source_frame);
            bool is_exact_frame = (pixels != nullptr);

            if (!pixels) {
                // Fall back to closest frame in buffer (fast keyframe access)
                int actual_frame = -1;
                pixels = scrub_decoder->GetClosestFrame(coords.source_frame, &actual_frame);
            }

            if (pixels) {
                shuttle_last_texture_time_ = now;

                // Reuse texture if dimensions match, otherwise recreate
                if (aggressive_held_texture_ != 0 &&
                    aggressive_held_width_ == pixels->width &&
                    aggressive_held_height_ == pixels->height) {
                    // Reuse existing texture, just update content
                    glBindTexture(GL_TEXTURE_2D, aggressive_held_texture_);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                                    pixels->width, pixels->height,
                                    pixels->gl_format, pixels->gl_type,
                                    pixels->pixels.data());
                    glBindTexture(GL_TEXTURE_2D, 0);
                } else {
                    // Need new texture
                    if (aggressive_held_texture_ != 0) {
                        glDeleteTextures(1, &aggressive_held_texture_);
                    }
                    aggressive_held_texture_ = CreateGLTexture(pixels);
                    aggressive_held_width_ = pixels->width;
                    aggressive_held_height_ = pixels->height;
                }

                aggressive_held_frame_ = timeline_frame;
                aggressive_held_source_ = coords.source_path;

                // Also update last_good for fallback chain
                last_good_texture_ = aggressive_held_texture_;
                last_good_width_ = aggressive_held_width_;
                last_good_height_ = aggressive_held_height_;
                last_good_frame_ = timeline_frame;
                last_good_source_path_ = coords.source_path;

                setOutputDimensions(pixels->width, pixels->height);
                if (got_exact_frame) *got_exact_frame = is_exact_frame;
                return maybeComposite(aggressive_held_texture_, aggressive_held_width_, aggressive_held_height_);
            }
        }

        // No new frame available - return held texture for visual continuity
        if (aggressive_held_texture_ != 0) {
            setOutputDimensions(aggressive_held_width_, aggressive_held_height_);
            if (got_exact_frame) *got_exact_frame = false;
            return maybeComposite(aggressive_held_texture_, aggressive_held_width_, aggressive_held_height_);
        }

        // Ultimate fallback: return last_good_texture
        if (last_good_texture_ != 0) {
            setOutputDimensions(last_good_width_, last_good_height_);
            if (got_exact_frame) *got_exact_frame = false;
            return maybeComposite(last_good_texture_, last_good_width_, last_good_height_);
        }

        // No held frame - return gap texture
        if (gap_texture_ != 0) {
            setOutputDimensions(gap_texture_width_, gap_texture_height_);
            if (got_exact_frame) *got_exact_frame = false;
            return gap_texture_;
        }

        return 0;
    }

    //=========================================================================
    // VIDEO FAST PATH - Only for VIDEO_FILE mode (GStreamer single video)
    // GStreamer handles buffering internally, so we bypass cache and pull directly
    // For MULTI_TRACK mode (FFmpeg), VIDEO clips use the cache path below
    //=========================================================================
    ClipMediaType media_type = DetectMediaType(coords.source_path);
    if (media_type == ClipMediaType::VIDEO && source_mode_ == TimelineSourceMode::VIDEO_FILE) {
        std::shared_ptr<ClipLoaderInfo> loader_info;
        {
            std::lock_guard<std::mutex> lock(loaders_mutex_);
            auto it = loaders_.find(coords.source_path);
            if (it != loaders_.end()) {
                loader_info = it->second;
            }
        }

        if (loader_info && loader_info->video_decoder) {
            // Update playhead to trigger decode-ahead
            loader_info->video_decoder->UpdatePlayhead(coords.source_frame, SeekQuality::NORMAL, false);

            // Try to get exact frame
            auto pixels = loader_info->video_decoder->GetFrame(coords.source_frame);
            bool is_exact = (pixels != nullptr);

            // Only fall back to closest frame during active shuttle/scrub mode
            // This prevents flashing wrong frames on click-seeks while keeping smooth scrubbing
            if (!pixels && shuttle_active_) {
                pixels = loader_info->video_decoder->GetClosestFrame(coords.source_frame, nullptr);
            }

            if (pixels) {
                // Upload to reusable texture (create if needed, resize if needed)
                // Select internal format based on pixel data type (must match for HDR/High-Res)
                GLenum internal_format = GL_RGBA8;
                if (pixels->gl_type == GL_HALF_FLOAT) {
                    internal_format = GL_RGBA16F;
                } else if (pixels->gl_type == GL_UNSIGNED_SHORT) {
                    internal_format = GL_RGBA16;
                }

                if (loader_info->video_texture == 0 ||
                    loader_info->video_texture_width != pixels->width ||
                    loader_info->video_texture_height != pixels->height ||
                    loader_info->video_texture_format != internal_format) {
                    // Delete old texture if exists
                    if (loader_info->video_texture != 0) {
                        glDeleteTextures(1, &loader_info->video_texture);
                    }
                    // Create new texture with correct internal format
                    glGenTextures(1, &loader_info->video_texture);
                    glBindTexture(GL_TEXTURE_2D, loader_info->video_texture);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, pixels->width, pixels->height,
                                 0, pixels->gl_format, pixels->gl_type, pixels->pixels.data());
                    loader_info->video_texture_width = pixels->width;
                    loader_info->video_texture_height = pixels->height;
                    loader_info->video_texture_format = internal_format;
                } else {
                    // Reuse existing texture, just update data
                    glBindTexture(GL_TEXTURE_2D, loader_info->video_texture);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pixels->width, pixels->height,
                                    pixels->gl_format, pixels->gl_type, pixels->pixels.data());
                }
                glBindTexture(GL_TEXTURE_2D, 0);

                setOutputDimensions(pixels->width, pixels->height);
                if (got_exact_frame) *got_exact_frame = is_exact;
                cache_hits_++;  // Count as hit for stats
                return maybeComposite(loader_info->video_texture, pixels->width, pixels->height);
            }
        }

        // No frame available - return gap or 0
        if (got_exact_frame) *got_exact_frame = false;
        if (gap_texture_ != 0) {
            setOutputDimensions(gap_texture_width_, gap_texture_height_);
            return gap_texture_;
        }
        return 0;
    }

    //=========================================================================
    // CACHE PATH - For IMAGE_SEQUENCE and MULTI_TRACK VIDEO (FFmpeg)
    // Uses timeline_frame as cache key (simple, matches working v069 backup)
    //=========================================================================
    TimelineCacheKey key{timeline_frame};

    // DEBUG: Periodic playback frame mapping log (every ~1 second during playback)
    static int playback_log_counter = 0;
    static int last_logged_frame = -1000;
    int frame_delta = std::abs(timeline_frame - last_logged_frame);
    // Reduced logging - only log on large jumps (>5 seconds)
    if (frame_delta >= static_cast<int>(config_.fps * 5)) {
        playback_log_counter++;
        last_logged_frame = timeline_frame;
    }

    bool is_post_edit = post_edit_pending_.load();

    // During scrubbing, always try decoder buffer first (freshest frame)
    // This ensures we show the most recently decoded frame immediately
    bool is_scrubbing = (scrub_state_.load() != ScrubState::IDLE);

    // CRITICAL: Invalidate last_good_texture_ if:
    // 1. User has moved too far from it (timeline distance > 48 frames)
    // 2. Source path has changed (crossed clip boundary into different source file)
    // This prevents showing a frame from position 100 when user is at position 500,
    // AND prevents showing source A's frame when we've moved to source B
    constexpr int kLastGoodMaxDistance = 48;  // ~2 seconds @ 24fps
    bool too_far = last_good_frame_ >= 0 && std::abs(timeline_frame - last_good_frame_) > kLastGoodMaxDistance;
    bool source_changed = !last_good_source_path_.empty() && coords.source_path != last_good_source_path_;

    if (too_far || source_changed) {
        // last_good_texture_ is stale - invalidate it
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
            // Queue for deletion in ProcessPendingUploads() - avoid GL ops during render
            std::lock_guard<std::mutex> lock(delete_mutex_);
            textures_to_delete_.push_back(last_good_texture_);
        }
        last_good_texture_ = 0;
        last_good_width_ = 0;
        last_good_height_ = 0;
        last_good_frame_ = -1;
        last_good_source_path_.clear();
    }

    // Post-edit state tracking (for logging only now)
    // The decoder seek in NotifyTracksEdited() clears stale frames, so we can allow
    // GetClosestFrame fallback - the frames in the buffer are from the new position
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

    // Helper lambda to mark success
    auto markSuccess = [this]() {
        last_successful_frame_time_ = std::chrono::steady_clock::now();
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
            last_good_source_path_ = coords.source_path;  // Track source for clip boundary detection
            if (got_exact_frame) *got_exact_frame = true;  // Cache hit = exact frame
            return maybeComposite(it->second.texture_id, it->second.width, it->second.height);
        }
    }

    if (is_scrubbing) {
        // Scrubbing path: try to get exact frame, fall back to closest
        std::shared_ptr<ClipLoaderInfo> loader_info;
        {
            std::lock_guard<std::mutex> lock(loaders_mutex_);
            auto loader_it = loaders_.find(coords.source_path);
            if (loader_it != loaders_.end()) {
                loader_info = loader_it->second;
            }
        }
        if (loader_info && loader_info->HasBufferedDecoder()) {
            // Try exact frame first
            auto pixels = loader_info->GetFrame(coords.source_frame);
            bool is_exact_frame = (pixels != nullptr);

            // If exact not available, get closest
            if (!pixels) {
                pixels = loader_info->GetClosestFrame(coords.source_frame);
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
                        last_good_source_path_ = coords.source_path;
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
                            // Queue for deletion - avoid GL ops during render
                            {
                                std::lock_guard<std::mutex> del_lock(delete_mutex_);
                                textures_to_delete_.push_back(texture);
                            }
                            setOutputDimensions(existing->second.width, existing->second.height);
                            cache_hits_++;
                            markSuccess();
                            last_good_texture_ = existing->second.texture_id;
                            last_good_width_ = existing->second.width;
                            last_good_height_ = existing->second.height;
                            last_good_frame_ = timeline_frame;
                            last_good_source_path_ = coords.source_path;
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
                        last_good_source_path_ = coords.source_path;
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
            auto loader_it = loaders_.find(coords.source_path);
            if (loader_it != loaders_.end()) {
                loader_info = loader_it->second;
            }
        }
        if (loader_info && loader_info->HasBufferedDecoder()) {
            // Try exact frame first
            auto pixels = loader_info->GetFrame(coords.source_frame);
            bool is_exact_frame = (pixels != nullptr);

            // If not available, get closest frame (like scrubbing does)
            if (!pixels) {
                pixels = loader_info->GetClosestFrame(coords.source_frame);
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
                        last_good_source_path_ = coords.source_path;
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
                        last_good_source_path_ = coords.source_path;
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
    // CRITICAL: Skip all expensive work during shuttle mode
    // Shuttle has its own frame management system; playhead just needs to update atomically
    if (shuttle_active_) {
        current_frame_ = timeline_frame;
        return;
    }

    auto now = std::chrono::steady_clock::now();

    // Detect seeks and cancel in-flight requests (like EXR cache)
    bool isSeek = false;
    {
        std::lock_guard<std::mutex> lock(request_mutex_);

        // Detect seek: jump > 20 frames (circular distance accounts for loop boundaries)
        int circular_dist = cache_engine_.AbsCircularDistance(previousFrame_, timeline_frame);
        if (previousFrame_ >= 0 && circular_dist > 20) {
            isSeek = true;
            needsFillReset_ = true;  // Tell cache thread to reset fill counters

            // CRITICAL: Clear post-edit grace period on explicit seek
            // The grace period prevents thrashing during rapid EDITS, but a seek
            // indicates user intent to view a specific position. If we don't clear it,
            // decoder playhead updates are skipped and cache can't generate.
            if (post_edit_pending_.load()) {
                post_edit_pending_ = false;
                Debug::Log("TimelineCache: Cleared post-edit grace period on seek");
            }
        }

        // Detect scrub start: not playing + frame jump > 1
        // This detects user dragging the playhead
        if (!is_playing && previousFrame_ >= 0 && std::abs(timeline_frame - previousFrame_) > 1) {
            scrub_state_ = ScrubState::SCRUBBING;
            last_scrub_time_ = now;
            pending_refine_frame_ = timeline_frame;

            // Clear post-edit grace period on scrub too (any frame change while not playing)
            // This handles seeks < 20 frames that wouldn't trigger the seek detection above
            if (post_edit_pending_.load()) {
                post_edit_pending_ = false;
                Debug::Log("TimelineCache: Cleared post-edit grace period on scrub");
            }
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
                    if (loader_info && loader_info->HasBufferedDecoder()) {
                        // Force seek to exact frame before playback starts
                        loader_info->UpdatePlayhead(coords.source_frame, SeekQuality::NORMAL, true);
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
    bool was_playing = is_playing_.exchange(is_playing);

    // Update cache window engine's playhead
    // CRITICAL: Skip during ACTIVE_SCRUBBING to prevent priming engine for full window fill
    // When scrub mode ends, the next iteration would otherwise trigger full window decode
    if (aggressive_scrub_mode_.load() != AggressiveScrubMode::ACTIVE_SCRUBBING) {
        cache_engine_.UpdatePlayhead(timeline_frame);
    }

    // DEBUG: Log when play state changes
    if (is_playing != was_playing) {
        size_t cache_size = 0;
        int min_frame = INT_MAX, max_frame = INT_MIN;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cache_size = frame_cache_.size();
            for (const auto& [key, frame] : frame_cache_) {
                if (key.timeline_frame < min_frame) min_frame = key.timeline_frame;
                if (key.timeline_frame > max_frame) max_frame = key.timeline_frame;
            }
        }
        Debug::Log("TimelineCache: [PLAY STATE CHANGE] is_playing=" + std::to_string(is_playing) +
                   " frame=" + std::to_string(timeline_frame) +
                   " cached=" + std::to_string(cache_size) +
                   " range=[" + std::to_string(min_frame) + "-" + std::to_string(max_frame) + "]");
    }

    // Propagate playback mode changes to all decoders
    if (is_playing != was_playing) {
        std::lock_guard<std::mutex> lock(loaders_mutex_);
        for (auto& [path, loader] : loaders_) {
            if (loader) {
                loader->SetPlaybackMode(is_playing);
            }
        }
    }

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

    // SKIP VIDEO CLIPS in VIDEO_FILE mode - GStreamer handles decoding internally
    // For MULTI_TRACK mode, VIDEO clips use the cache path
    ClipMediaType media_type = DetectMediaType(coords.source_path);
    if (media_type == ClipMediaType::VIDEO && source_mode_ == TimelineSourceMode::VIDEO_FILE) {
        return;  // GStreamer decoder handles this
    }

    TimelineCacheKey key{timeline_frame};

    // Check if already cached
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (frame_cache_.find(key) != frame_cache_.end()) {
            return;  // Already cached
        }
    }

    // Check if already in progress or pending (IMAGE_SEQUENCE only at this point)
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

    // Delete textures marked for deletion (always do this, even during shuttle)
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

    // Skip expensive texture uploads during shuttle mode
    // Shuttle has its own texture handling - don't compete with it
    if (shuttle_active_) {
        return;
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

        // Create texture (D3D11 or GL depending on mode)
        CachedFrame frame;
        frame.width = upload.pixels->width;
        frame.height = upload.pixels->height;
        frame.byte_size = upload.pixels->ByteSize();

#ifdef _WIN32
        if (use_d3d11_rendering_) {
            // Create D3D11 texture
            if (!CreateD3D11Texture(upload.pixels, frame.texture_d3d, frame.srv_d3d)) {
                continue;
            }
        } else
#endif
        {
            // Create GL texture
            GLuint texture = CreateGLTexture(upload.pixels);
            if (texture == 0) continue;
            frame.texture_id = texture;
        }

        {
            std::lock_guard<std::mutex> lock(cache_mutex_);

            // FIX: Check if key already exists - delete old texture to prevent leak!
            // This can happen when duplicate requests slip through (race condition)
            auto existing = frame_cache_.find(upload.key);
            if (existing != frame_cache_.end()) {
#ifdef _WIN32
                // D3D11 textures are released automatically via ComPtr
                // Just need to handle GL textures
#endif
                if (existing->second.texture_id != 0) {
                    // Delete old texture immediately (we're on GL thread)
                    glDeleteTextures(1, &existing->second.texture_id);
                    s_textures_deleted++;
                   /* Debug::Log("TimelineCache: [DUPLICATE] Deleted old texture " +
                               std::to_string(existing->second.texture_id) +
                               " for frame " + std::to_string(upload.key.timeline_frame));*/
                }
            }

            frame_cache_[upload.key] = frame;
        }

        // Log upload (periodically)
        static int total_uploads = 0;
        total_uploads++;
        if (total_uploads <= 5 || total_uploads % 50 == 0) {
            /*Debug::Log("TimelineCache: GPU upload #" + std::to_string(total_uploads) +
                       " - tl_frame " + std::to_string(upload.key.timeline_frame) +
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
                SourceCoords coords = TimelineToSource(key.timeline_frame);
                if (coords.valid) {
                    auto pool_key = MakeTimelineKey(coords.source_path, coords.source_frame);
                    SharedMemoryPool::Instance().RemoveEntry(pool_key);
                }
            }
        }

        frame_cache_.clear();
    }

    // Reset last_good_texture_ to prevent dangling reference
    // (the texture it pointed to was just marked for deletion)
    last_good_texture_ = 0;
    last_good_width_ = 0;
    last_good_height_ = 0;
    last_good_frame_ = -1;
    last_good_source_path_.clear();

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
    last_good_source_path_.clear();

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
                if (loader_info && loader_info->HasBufferedDecoder()) {
                    loaders_to_flush.push_back(loader_info);
                }
            }
        }

        int flushed_count = 0;
        for (auto& loader_info : loaders_to_flush) {
            // Force a hard reset on each decoder to completely clear its buffer
            // Use frame 0 as a placeholder - it will be re-seeked when needed
            loader_info->HardReset(0);
            flushed_count++;
        }
        if (flushed_count > 0) {
            Debug::Log("TimelineCache: Hard reset " + std::to_string(flushed_count) + " decoder(s) on edit");
        }
    }

    // Now seek decoders for CURRENT clip AND UPCOMING clips in the readahead window
    // This is critical: we just hard-reset all decoders to frame 0, so upcoming clips
    // need to be seeked to their correct positions too, not just the current clip.
    {
        // Build decoder targets for the readahead window (like CacheManagementThread does)
        std::map<std::string, int> decoder_targets;  // source_path -> target frame

        // Current clip first (highest priority)
        if (coords.valid) {
            decoder_targets[coords.source_path] = coords.source_frame;
        }

        // Scan readahead window for upcoming clips
        int readAheadFrames = config_.readAheadFrames;
        for (int i = 1; i <= readAheadFrames && (current + i) < total_timeline_frames_; i++) {
            SourceCoords upcoming = TimelineToSource(current + i);
            if (!upcoming.valid) continue;

            // Only add if we haven't seen this source yet
            if (decoder_targets.find(upcoming.source_path) == decoder_targets.end()) {
                decoder_targets[upcoming.source_path] = upcoming.source_frame;
            }
        }

        // Now seek all decoders to their target positions
        for (const auto& [source_path, target_frame] : decoder_targets) {
            std::shared_ptr<ClipLoaderInfo> loader_info;
            {
                std::lock_guard<std::mutex> lock(loaders_mutex_);
                auto it = loaders_.find(source_path);
                if (it != loaders_.end()) {
                    loader_info = it->second;
                }
            }
            if (loader_info && loader_info->HasBufferedDecoder()) {
                bool is_current = coords.valid && source_path == coords.source_path;
                // Use is_prefetch=true for upcoming clips to avoid aggressive respawning
                loader_info->UpdatePlayhead(target_frame, SeekQuality::NORMAL, true, !is_current);

                std::string filename = source_path;
                size_t pos = filename.find_last_of("/\\");
                if (pos != std::string::npos) filename = filename.substr(pos + 1);
                Debug::Log("TimelineCache: Post-edit seek " + filename +
                           " to frame " + std::to_string(target_frame) +
                           (is_current ? " [current]" : " [upcoming]"));
            }
        }

        // KICKSTART: Immediately queue the current frame as highest priority
        if (coords.valid) {
            std::lock_guard<std::mutex> lock(request_mutex_);
            video_requests_.push_front(current);  // Current frame at front = highest priority
            video_requests_set_.insert(current);
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

    // Update the cache window engine with new total frames
    cache_engine_.SetTotalFrames(total_timeline_frames_);

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

    // Apply pipeline mode from config (must be set before Initialize creates decoders)
    video_pipeline_mode_ = config_.pipeline_mode;
    Debug::Log("TimelineCache::SetConfig: pipeline_mode set to " +
               std::string(PipelineModeToString(video_pipeline_mode_)));

    // Update the cache window engine with new window size
    cache_engine_.SetWindow(config_.GetReadBehindFrames(), config_.readAheadFrames);

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

void TimelineCache::SetFPS(double fps) {
    if (std::abs(config_.fps - fps) < 0.01) return;  // No significant change

    Debug::Log("TimelineCache: FPS updated from " + std::to_string(config_.fps) +
               " to " + std::to_string(fps));

    config_.fps = fps;

    // Recalculate frame counts
    total_timeline_frames_ = static_cast<int>(timeline_duration_ * fps);

    // Update cache window engine
    cache_engine_.SetTotalFrames(total_timeline_frames_);
    cache_engine_.SetWindow(config_.GetReadBehindFrames(), config_.readAheadFrames);

    // Clear cache - old frames have stale frame numbers
    ClearCache();

    // Reset detected FPS since we've now applied it
    detected_media_fps_ = 0.0;
}

void TimelineCache::SetPipelineMode(PipelineMode mode) {
    if (video_pipeline_mode_ == mode) {
        return;  // No change
    }

    video_pipeline_mode_ = mode;
    Debug::Log("TimelineCache: Pipeline mode set to " +
               std::string(PipelineModeToString(mode)));

    // Propagate to all video decoders (image sequences handle their own bit depth)
    {
        std::lock_guard<std::mutex> lock(loaders_mutex_);
        for (auto& [path, loader] : loaders_) {
            if (loader->video_decoder) {
                loader->video_decoder->SetPipelineMode(mode);
            }
            // Note: sequence decoders don't have SetPipelineMode - their bit depth
            // is determined by the image format (EXR=float, TIFF/PNG=8/16-bit)
        }
    }

    // Clear cache since bit depth changed
    ClearCache();

    Debug::Log("TimelineCache: Cleared cache after pipeline mode change");
}

void TimelineCache::SetLoopBoundaries(int start_frame, int end_frame) {
    // Validate and swap if needed
    if (start_frame > end_frame) std::swap(start_frame, end_frame);

    // Only log if values actually changed
    int old_start = loop_start_frame_.load();
    int old_end = loop_end_frame_.load();
    if (start_frame == old_start && end_frame == old_end) {
        return;  // No change
    }

    loop_start_frame_.store(start_frame);
    loop_end_frame_.store(end_frame);

    // Update the cache window engine with new boundaries
    cache_engine_.SetBoundaries(start_frame, end_frame);

    Debug::Log("TimelineCache: Loop boundaries set [" + std::to_string(start_frame) +
               " - " + std::to_string(end_frame) + "]");

    //=========================================================================
    // LOOP PREFETCHING: Forward source frame boundaries to the decoder
    //
    // When playback loops, we need to pre-spawn a decoder at the loop start
    // before the loop actually happens. This eliminates the ~200ms gap that
    // occurs when spawning at the moment of loop.
    //
    // Translate timeline frames to source frames and tell the decoder.
    //=========================================================================

    // Safety check: ensure cache is initialized before accessing flattener
    if (!initialized_ || !flattener_) {
        return;
    }

    SourceCoords start_coords = TimelineToSource(start_frame);
    SourceCoords end_coords = TimelineToSource(end_frame);

    // Case 1: Same source file - use decoder-level loop prefetching (proactive dual)
    if (start_coords.valid && end_coords.valid &&
        !start_coords.source_path.empty() && !end_coords.source_path.empty() &&
        start_coords.source_path == end_coords.source_path) {

        std::shared_ptr<ClipLoaderInfo> loader_info;
        {
            std::lock_guard<std::mutex> lock(loaders_mutex_);
            auto it = loaders_.find(start_coords.source_path);
            if (it != loaders_.end() && it->second) {
                loader_info = it->second;
            }
        }

        if (loader_info && loader_info->HasBufferedDecoder()) {
            loader_info->SetLoopBoundaries(start_coords.source_frame, end_coords.source_frame);

            // Safe filename extraction
            std::string filename = start_coords.source_path;
            size_t pos = filename.find_last_of("/\\");
            if (pos != std::string::npos && pos + 1 < filename.size()) {
                filename = filename.substr(pos + 1);
            }
            Debug::Log("TimelineCache: Forwarded loop boundaries to decoder [" +
                       std::to_string(start_coords.source_frame) + ", " +
                       std::to_string(end_coords.source_frame) + "] for " + filename);
        }
    }
    // Case 2: CROSS-CLIP LOOP - different source files at start vs end
    // Just ensure the target decoder exists and is targeting the loop start position.
    // Don't use proactive dual for cross-clip - it adds complexity that can interfere
    // with multi-clip OTIO flow. CacheManagementThread's wrapped_frames handling
    // will keep the target decoder warmed.
    else if (start_coords.valid && !start_coords.source_path.empty()) {
        // Get or create the decoder for the loop start clip
        std::shared_ptr<ClipLoaderInfo> start_loader = GetOrCreateLoader(start_coords.source_path);

        if (start_loader && start_loader->HasBufferedDecoder()) {
            // Target the loop start frame - use is_prefetch=true to avoid aggressive respawning
            start_loader->UpdatePlayhead(start_coords.source_frame, SeekQuality::NORMAL, false, true);

            // Safe filename extraction
            std::string filename = start_coords.source_path;
            size_t pos = filename.find_last_of("/\\");
            if (pos != std::string::npos && pos + 1 < filename.size()) {
                filename = filename.substr(pos + 1);
            }
            Debug::Log("TimelineCache: CROSS-CLIP LOOP - targeting decoder at frame " +
                       std::to_string(start_coords.source_frame) + " for " + filename);
        }
    }
}

void TimelineCache::ClearLoopBoundaries() {
    // Only log if we actually had boundaries before
    bool had_boundaries = (loop_start_frame_.load() >= 0 && loop_end_frame_.load() > 0);
    loop_start_frame_.store(-1);
    loop_end_frame_.store(-1);

    // Clear boundaries in the cache window engine
    cache_engine_.ClearBoundaries();

    // Clear loop boundaries on all decoders
    {
        std::lock_guard<std::mutex> lock(loaders_mutex_);
        for (auto& [path, loader] : loaders_) {
            if (loader && loader->HasBufferedDecoder()) {
                loader->ClearLoopBoundaries();
            }
        }
    }

    if (had_boundaries) {
        Debug::Log("TimelineCache: Loop boundaries cleared");
    }
}

int TimelineCache::GetBoundaryStart() const {
    // Delegate to the CacheWindowEngine for boundary info
    return cache_engine_.GetBoundaryStart();
}

int TimelineCache::GetBoundaryEnd() const {
    // Delegate to the CacheWindowEngine for boundary info
    return cache_engine_.GetBoundaryEnd();
}

std::vector<int> TimelineCache::GetAheadFrames(int current_frame, int count) const {
    // Use the engine to wrap frames correctly
    int start = cache_engine_.GetBoundaryStart();
    int end = cache_engine_.GetBoundaryEnd();
    int len = cache_engine_.GetBoundaryLength();

    if (len <= 0) return {};

    std::vector<int> frames;
    frames.reserve(count);

    for (int i = 1; i <= count && static_cast<int>(frames.size()) < count; i++) {
        int f = current_frame + i;
        // Wrap into boundary region using engine
        f = cache_engine_.WrapFrame(f);
        frames.push_back(f);
    }

    return frames;
}

std::set<int> TimelineCache::GetCacheWindow(int current_frame) const {
    // Use the CacheWindowEngine as the single source of truth for circular math
    // The engine's UpdatePlayhead should already be called from UpdatePlayhead()
    // but we also update it here for safety in case of direct calls

    // Create a non-const copy of the engine to update playhead
    // (GetCacheWindow is const for backward compatibility, but engine update is needed)
    const_cast<CacheWindowEngine&>(cache_engine_).UpdatePlayhead(current_frame);

    // Event-based logging: log once when boundaries change
    static int last_logged_start = -999, last_logged_end = -999;
    int start = cache_engine_.GetBoundaryStart();
    int end = cache_engine_.GetBoundaryEnd();
    int len = cache_engine_.GetBoundaryLength();
    bool has_custom = cache_engine_.HasCustomBoundaries();
    if (has_custom && (start != last_logged_start || end != last_logged_end)) {
        Debug::Log("TimelineCache: GetCacheWindow using boundaries [" +
                   std::to_string(start) + "-" + std::to_string(end) +
                   "] (len=" + std::to_string(len) + ")");
        last_logged_start = start;
        last_logged_end = end;
    }

    // Get the frame window as a set from the engine
    return cache_engine_.GetFrameWindowSet();
}

// Helper: Wrap or clamp frame index within loop boundaries
int TimelineCache::WrapFrame(int frame) const {
    // Delegate to the CacheWindowEngine for circular math
    return cache_engine_.WrapFrame(frame);
}

// Helper: Calculate distance between frames considering wrap-around within boundaries
int TimelineCache::FrameDistance(int from, int to) const {
    // Delegate to the CacheWindowEngine for circular math
    return cache_engine_.AbsCircularDistance(from, to);
}

// Get priority-sorted frame window from engine (for buffer-wait checks)
std::vector<int> TimelineCache::GetPriorityFrameWindow() const {
    return cache_engine_.GetFrameWindow();
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

bool TimelineCache::IsVideoOnly() const {
    // Check if all loaded sources are video clips (GStreamer handles buffering)
    // If so, we skip the cache progress bar since caching is internal to GStreamer
    std::lock_guard<std::mutex> lock(loaders_mutex_);
    if (loaders_.empty()) {
        return false;  // No content yet
    }
    for (const auto& [path, info] : loaders_) {
        if (!info) continue;
        // If any clip is NOT video, return false (cache bar is useful)
        if (info->media_type != ClipMediaType::VIDEO) {
            return false;
        }
    }
    return true;  // All clips are video - GStreamer handles caching
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

    // Get boundary range - only scan within boundaries
    int boundary_start = GetBoundaryStart();
    int boundary_end = GetBoundaryEnd();

    if (boundary_end < boundary_start) {
        return segments;
    }

    // Take snapshot of cache keys for iteration
    // Include both frames with GPU textures AND frames pending upload (decoded but not yet uploaded)
    std::set<TimelineCacheKey> cached_keys;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        for (const auto& [key, frame] : frame_cache_) {
            if (frame.texture_id != 0) {
                cached_keys.insert(key);
            }
        }
    }

    // Also include frames in pending uploads - these are decoded and ready, just awaiting GPU upload
    {
        std::lock_guard<std::mutex> lock(upload_mutex_);
        for (const auto& key : pending_uploads_set_) {
            cached_keys.insert(key);
        }
    }

    if (cached_keys.empty()) {
        segments_dirty_ = false;
        std::lock_guard<std::mutex> lock(segments_mutex_);
        cached_segments_ = segments;
        return segments;
    }

    // Scan ONLY within boundary range - this respects the boundary-based cache system
    // Group consecutive cached frames into segments
    int segment_start = -1;
    int segment_end = -1;

    for (int frame = boundary_start; frame <= boundary_end; ++frame) {
        bool is_cached = false;

        // Check if this timeline frame is cached
        TimelineCacheKey key{frame};
        is_cached = (cached_keys.find(key) != cached_keys.end());

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

std::vector<TimelineCacheSegment> TimelineCache::GetVideoBufferSegments() const {
    std::vector<TimelineCacheSegment> segments;

    if (!initialized_ || timeline_duration_ <= 0 || config_.fps <= 0) {
        return segments;
    }

    // Get boundary range - only scan within boundaries
    int boundary_start = GetBoundaryStart();
    int boundary_end = GetBoundaryEnd();

    if (boundary_end < boundary_start) {
        return segments;
    }

    // Scan timeline frames and check if each maps to a buffered source frame in video decoder
    // Group consecutive buffered frames into segments
    int segment_start = -1;
    int segment_end = -1;

    for (int frame = boundary_start; frame <= boundary_end; ++frame) {
        bool is_buffered = false;

        // Map timeline frame to source coordinates
        SourceCoords coords = TimelineToSource(frame);
        if (coords.valid) {
            ClipMediaType media_type = DetectMediaType(coords.source_path);
            if (media_type == ClipMediaType::VIDEO) {
                // Check if this source frame is in the video decoder's buffer
                std::lock_guard<std::mutex> lock(loaders_mutex_);
                auto it = loaders_.find(coords.source_path);
                if (it != loaders_.end() && it->second) {
                    // Check managed_decoder (MULTI_TRACK mode) or video_decoder (VIDEO_FILE mode)
                    if (it->second->managed_decoder) {
                        is_buffered = it->second->managed_decoder->HasFrame(coords.source_frame);
                    } else if (it->second->video_decoder) {
                        is_buffered = it->second->video_decoder->HasFrame(coords.source_frame);
                    }
                }
            }
        }

        if (is_buffered) {
            if (segment_start < 0) {
                segment_start = frame;
            }
            segment_end = frame;
        } else {
            // End of buffered segment
            if (segment_start >= 0) {
                TimelineCacheSegment seg;
                seg.type = TimelineCacheSegment::VIDEO_BUFFER;
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
        seg.type = TimelineCacheSegment::VIDEO_BUFFER;
        seg.start_time = segment_start / config_.fps;
        seg.end_time = (segment_end + 1) / config_.fps;
        seg.density = 1.0f;
        segments.push_back(seg);
    }

    return segments;
}

std::vector<TimelineCacheSegment> TimelineCache::GetTargetWindowSegments(int current_frame) const {
    std::vector<TimelineCacheSegment> segments;

    if (!initialized_ || timeline_duration_ <= 0 || config_.fps <= 0) {
        return segments;
    }

    // Use the engine for boundary info
    int boundary_start = cache_engine_.GetBoundaryStart();
    int boundary_end = cache_engine_.GetBoundaryEnd();
    int boundary_len = cache_engine_.GetBoundaryLength();

    if (boundary_len <= 0) {
        return segments;
    }

    // Wrap current frame into boundaries using engine
    current_frame = cache_engine_.WrapFrame(current_frame);

    int behind = cache_engine_.GetReadBehind();
    int ahead = cache_engine_.GetReadAhead();

    // Calculate raw window start/end (may extend outside boundaries)
    int window_start = current_frame - behind;
    int window_end = current_frame + ahead;

    // Check if we need wrap-around (window extends outside boundary)
    bool wraps_at_start = (window_start < boundary_start);
    bool wraps_at_end = (window_end > boundary_end);

    if (!wraps_at_start && !wraps_at_end) {
        // Simple case: window fits entirely within boundaries
        TimelineCacheSegment seg;
        seg.type = TimelineCacheSegment::TARGET_WINDOW;
        seg.start_time = window_start / config_.fps;
        seg.end_time = (window_end + 1) / config_.fps;
        seg.density = 1.0f;
        segments.push_back(seg);
    } else {
        // Wrap-around case: split into two segments

        if (wraps_at_end) {
            // Segment 1: From current-behind to boundary_end
            int seg1_start = std::max(window_start, boundary_start);
            int seg1_end = boundary_end;

            if (seg1_end >= seg1_start) {
                TimelineCacheSegment seg;
                seg.type = TimelineCacheSegment::TARGET_WINDOW;
                seg.start_time = seg1_start / config_.fps;
                seg.end_time = (seg1_end + 1) / config_.fps;
                seg.density = 1.0f;
                segments.push_back(seg);
            }

            // Segment 2: From boundary_start to wrapped portion
            int overflow = window_end - boundary_end;
            int seg2_end = boundary_start + overflow - 1;
            seg2_end = std::min(seg2_end, boundary_end); // Safety clamp

            if (seg2_end >= boundary_start) {
                TimelineCacheSegment seg;
                seg.type = TimelineCacheSegment::TARGET_WINDOW;
                seg.start_time = boundary_start / config_.fps;
                seg.end_time = (seg2_end + 1) / config_.fps;
                seg.density = 1.0f;
                segments.push_back(seg);
            }
        }

        if (wraps_at_start && !wraps_at_end) {
            // Segment 1: From boundary_start to current+ahead
            int seg1_start = boundary_start;
            int seg1_end = std::min(window_end, boundary_end);

            if (seg1_end >= seg1_start) {
                TimelineCacheSegment seg;
                seg.type = TimelineCacheSegment::TARGET_WINDOW;
                seg.start_time = seg1_start / config_.fps;
                seg.end_time = (seg1_end + 1) / config_.fps;
                seg.density = 1.0f;
                segments.push_back(seg);
            }

            // Segment 2: From wrapped portion to boundary_end
            int underflow = boundary_start - window_start;
            int seg2_start = boundary_end - underflow + 1;
            seg2_start = std::max(seg2_start, boundary_start); // Safety clamp

            if (boundary_end >= seg2_start) {
                TimelineCacheSegment seg;
                seg.type = TimelineCacheSegment::TARGET_WINDOW;
                seg.start_time = seg2_start / config_.fps;
                seg.end_time = (boundary_end + 1) / config_.fps;
                seg.density = 1.0f;
                segments.push_back(seg);
            }
        }
    }

    return segments;
}

std::vector<TimelineCacheSegment> TimelineCache::GetBoundarySegments() const {
    std::vector<TimelineCacheSegment> segments;

    if (!initialized_ || timeline_duration_ <= 0 || config_.fps <= 0) {
        return segments;
    }

    // Only return boundary segment if custom boundaries are set
    if (!cache_engine_.HasCustomBoundaries()) {
        return segments;
    }

    int boundary_start = cache_engine_.GetBoundaryStart();
    int boundary_end = cache_engine_.GetBoundaryEnd();

    if (boundary_end < boundary_start) {
        return segments;
    }

    TimelineCacheSegment seg;
    seg.type = TimelineCacheSegment::BOUNDARY_REGION;
    seg.start_time = boundary_start / config_.fps;
    seg.end_time = (boundary_end + 1) / config_.fps;
    seg.density = 1.0f;
    segments.push_back(seg);

    return segments;
}

SourceCoords TimelineCache::GetSourceCoords(int timeline_frame) const {
    return TimelineToSource(timeline_frame);
}

PipelineMode TimelineCache::GetClipPipelineMode(int timeline_frame) const {
    SourceCoords coords = GetSourceCoords(timeline_frame);
    if (!coords.valid) return PipelineMode::NORMAL;

    std::lock_guard<std::mutex> lock(loaders_mutex_);
    auto it = loaders_.find(coords.source_path);
    if (it != loaders_.end() && it->second) {
        return it->second->pipeline_mode;
    }
    return PipelineMode::NORMAL;
}

bool TimelineCache::HasFrameReady(int timeline_frame) const {
    if (!initialized_) return false;

    // Map timeline frame to source coordinates
    SourceCoords coords = TimelineToSource(timeline_frame);
    if (!coords.valid) {
        // Gap or unlinked - gaps are always "ready" (we show black)
        return true;
    }

    TimelineCacheKey key{timeline_frame};

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
        auto it = loaders_.find(coords.source_path);
        if (it != loaders_.end() && it->second && it->second->HasBufferedDecoder()) {
            // Check if the exact frame is in the decoder's frame buffer
            // This does NOT trigger decoding - it just checks the ring buffer
            if (it->second->HasFrame(coords.source_frame)) {
                return true;
            }
        }
    }

    return false;
}

GLuint TimelineCache::GetSourceFrame(const std::string& source_path, int source_frame,
                                      int& width, int& height) {
    // NOTE: Direct source frame access not supported with timeline_frame-based caching
    // This function was for slip/trim preview but is not currently used
    (void)source_path;
    (void)source_frame;
    width = 0;
    height = 0;
    return 0;
}

void TimelineCache::RequestSourceFrame(const std::string& source_path, int source_frame) {
    // NOTE: Direct source frame requests not supported with timeline_frame-based caching
    // This function was for slip/trim preview but is not currently used
    (void)source_path;
    (void)source_frame;
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

    // Use center of frame's display period to avoid boundary ambiguity
    double timestamp = (static_cast<double>(timeline_frame) + 0.5) / config_.fps;

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

    // Calculate source frame
    double clip_offset = timestamp - clip->start_time;

    // Use source fps if available, otherwise fall back to timeline fps
    double fps_for_frame_calc = (clip->source_fps > 0) ? clip->source_fps : config_.fps;

    int source_frame = 0;
    int original_source_frame = 0;

    //=========================================================================
    // IMAGE SEQUENCE: Special handling - must be checked FIRST
    // ImageSequenceDecoder expects 0-based frame index (it adds start_frame internally)
    //=========================================================================
    if (clip->is_sequence && clip->sequence_end_frame >= clip->sequence_start_frame) {
        int sequence_frame_count = clip->sequence_end_frame - clip->sequence_start_frame + 1;

        // Calculate 0-based frame index within the sequence
        // clip_offset is time into the clip, convert to frame index
        // DON'T use source_in here - sequences use 0-based indexing
        source_frame = static_cast<int>(clip_offset * fps_for_frame_calc + 0.5);
        original_source_frame = source_frame;

        // Clamp to valid sequence range (0 to frame_count-1)
        if (source_frame < 0) source_frame = 0;
        if (source_frame >= sequence_frame_count) source_frame = sequence_frame_count - 1;
    }
    //=========================================================================
    // VIDEO FILES: Standard calculation with source_in and duration clamping
    //=========================================================================
    else {
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

        // Use floor (no +0.5) since we already added 0.5 to timestamp for center-of-frame
        // Adding +0.5 here would double-count and shift all frames by 1
        source_frame = static_cast<int>(source_time * fps_for_frame_calc);
        original_source_frame = source_frame;

        // Clamp source_frame to valid range for the source media
        // This handles single-frame holds and prevents seeking beyond end of clip
        if (source_frame < 0) source_frame = 0;

        // CRITICAL: Also clamp to max source frame to handle frame holds
        // When a short clip (e.g., still image) is stretched longer than its source,
        // source_frame would otherwise increment beyond available frames
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
    }

    // Debug: Log mapping (only first time per clip ID, reset after edit)
    // Use clip->id instead of clip->name so we see mappings for ALL clips after a cut
    if (s_logged_clips.find(clip->id) == s_logged_clips.end()) {
        std::string seq_info = clip->is_sequence ?
            ", is_sequence=true, seq_frames=" + std::to_string(clip->sequence_start_frame) +
            "-" + std::to_string(clip->sequence_end_frame) : "";
        Debug::Log("TimelineToSource: id='" + clip->id + "' name='" + clip->name +
                   "' frame " + std::to_string(timeline_frame) +
                   " -> source frame " + std::to_string(source_frame) +
                   " (clip_start=" + std::to_string(clip->start_time) +
                   ", source_in=" + std::to_string(clip->source_in) +
                   ", source_fps=" + std::to_string(clip->source_fps) +
                   ", source_duration=" + std::to_string(clip->source_duration) +
                   seq_info + ")");
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

//=============================================================================
// Sequence Metadata Registration
//=============================================================================

void TimelineCache::RegisterSequenceMetadata(const std::string& source_path,
                                              const SequenceMetadata& metadata) {
    std::lock_guard<std::mutex> lock(sequence_metadata_mutex_);
    sequence_metadata_[source_path] = metadata;
    Debug::Log("TimelineCache: Registered sequence metadata for " + source_path +
               " (pattern: " + metadata.pattern + ", frames: " +
               std::to_string(metadata.start_frame) + "-" + std::to_string(metadata.end_frame) + ")");
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

    // NOTE: Aggressive scrub gate removed - GetFrame scrub path now accesses
    // loaders directly, so decoder creation here is safe. This allows
    // CacheManagementThread to prewarm decoders for upcoming clips even during scrub.

    // Check for registered sequence metadata
    SequenceMetadata seq_meta;
    {
        std::lock_guard<std::mutex> lock(sequence_metadata_mutex_);
        auto it = sequence_metadata_.find(source_path);
        if (it != sequence_metadata_.end()) {
            seq_meta = it->second;
        }
    }

    // SLOW PATH: Create new loader WITHOUT holding lock
    // This is critical for avoiding contention during FFmpeg decoder initialization (50-500ms)
    auto info = std::make_shared<ClipLoaderInfo>();
    info->media_type = DetectMediaType(source_path);
    info->last_used_time = std::chrono::steady_clock::now();

    switch (info->media_type) {
        case ClipMediaType::VIDEO: {
            // Select decoder based on source mode:
            // - VIDEO_FILE: Use GStreamer via factory (single video, HW accel, A/V sync)
            // - MULTI_TRACK/DUAL_VIEW: Use ManagedVideoDecoder (FFmpeg with spawn-and-abandon)
            Debug::Log("TimelineCache::GetOrCreateLoader VIDEO clip, source_mode=" +
                       std::to_string(static_cast<int>(source_mode_)) +
                       " (0=MULTI_TRACK, 1=IMAGE_SEQ, 2=VIDEO_FILE, 4=DUAL_VIEW) path=" + source_path);
            if (source_mode_ == TimelineSourceMode::VIDEO_FILE) {
                // GStreamer path for single video playback
                auto decoder = VideoDecoderFactory::Instance().CreateDecoder(
                    source_path, VideoDecoderBackend::GSTREAMER);

                if (!decoder) {
                    Debug::Log("TimelineCache: Failed to create GStreamer decoder for " + source_path +
                               " - " + VideoDecoderFactory::Instance().GetLastError());
                    return nullptr;
                }

                // Set pipeline mode BEFORE Initialize() to avoid double pipeline build
                decoder->SetPipelineMode(video_pipeline_mode_);

                if (!decoder->Initialize()) {
                    Debug::Log("TimelineCache: Failed to initialize GStreamer decoder for " + source_path);
                    return nullptr;
                }

                info->video_decoder = std::move(decoder);
                info->pipeline_mode = video_pipeline_mode_;
                info->width = info->video_decoder->GetWidth();
                info->height = info->video_decoder->GetHeight();
                info->fps = info->video_decoder->GetFPS();
                info->frame_count = info->video_decoder->GetFrameCount();

                // Store detected FPS for pending update check (only for VIDEO_FILE mode)
                if (detected_media_fps_ <= 0) {
                    detected_media_fps_ = info->fps;
                    if (std::abs(info->fps - config_.fps) > 0.01) {
                        Debug::Log("TimelineCache: FPS mismatch - media=" + std::to_string(info->fps) +
                                   " timeline=" + std::to_string(config_.fps) + " (pending update)");
                    }
                }
            } else {
                // ManagedVideoDecoder for MULTI_TRACK/DUAL_VIEW (FFmpeg with spawn-and-abandon)
                // This provides responsive seeking for large 4K+ frames
                auto decoder = std::make_unique<ManagedVideoDecoder>(source_path);

                // Set pipeline mode BEFORE Initialize()
                decoder->SetPipelineMode(video_pipeline_mode_);

                if (!decoder->Initialize()) {
                    Debug::Log("TimelineCache: Failed to initialize ManagedVideoDecoder for " + source_path);
                    return nullptr;
                }

                info->managed_decoder = std::move(decoder);
                info->pipeline_mode = video_pipeline_mode_;
                info->width = info->managed_decoder->GetWidth();
                info->height = info->managed_decoder->GetHeight();
                info->fps = info->managed_decoder->GetFPS();
                info->frame_count = info->managed_decoder->GetFrameCount();

                Debug::Log("TimelineCache: ManagedVideoDecoder created for " + source_path +
                           " (" + std::to_string(info->width) + "x" + std::to_string(info->height) +
                           " @ " + std::to_string(info->fps) + " fps)");
            }
            break;
        }

        case ClipMediaType::EXR_SEQUENCE: {
            // Check if we have sequence metadata for buffered playback
            if (seq_meta.valid) {
                auto decoder = std::make_unique<ImageSequenceDecoder>(
                    seq_meta.directory, seq_meta.pattern);

                if (decoder->Initialize(
                    seq_meta.start_frame, seq_meta.end_frame,
                    config_.fps,  // Use timeline FPS
                    seq_meta.pipeline_mode,
                    seq_meta.exr_layer
                )) {
                    // Configure decoder to match timeline cache settings
                    StreamingDecoderConfig dec_config;
                    dec_config.readAheadFrames = config_.readAheadFrames;
                    dec_config.readBehindFrames = static_cast<int>(config_.readBehindSeconds * config_.fps);
                    decoder->SetConfig(dec_config);

                    info->sequence_decoder = std::move(decoder);
                    info->pipeline_mode = seq_meta.pipeline_mode;
                    info->width = info->sequence_decoder->GetWidth();
                    info->height = info->sequence_decoder->GetHeight();
                    info->fps = info->sequence_decoder->GetFPS();
                    info->frame_count = info->sequence_decoder->GetFrameCount();
                    Debug::Log("TimelineCache: ImageSequenceDecoder created for EXR sequence " + source_path +
                               " (readAhead=" + std::to_string(dec_config.readAheadFrames) + ")");
                    break;
                }
                Debug::Log("TimelineCache: ImageSequenceDecoder init failed, falling back to per-file loader");
            }

            // Fallback: per-file loader (legacy behavior)
            auto exr_loader = std::make_unique<EXRImageLoader>();
            info->image_loader = std::move(exr_loader);
            info->pipeline_mode = PipelineMode::ULTRA_HIGH_RES;  // Half-float for EXR
            info->image_loader->GetDimensions(source_path, info->width, info->height);
            break;
        }

        case ClipMediaType::IMAGE_SEQUENCE: {
            // Check if we have sequence metadata for buffered playback
            if (seq_meta.valid) {
                auto decoder = std::make_unique<ImageSequenceDecoder>(
                    seq_meta.directory, seq_meta.pattern);

                if (decoder->Initialize(
                    seq_meta.start_frame, seq_meta.end_frame,
                    config_.fps,  // Use timeline FPS
                    seq_meta.pipeline_mode,
                    ""  // No EXR layer for non-EXR sequences
                )) {
                    // Configure decoder to match timeline cache settings
                    StreamingDecoderConfig dec_config;
                    dec_config.readAheadFrames = config_.readAheadFrames;
                    dec_config.readBehindFrames = static_cast<int>(config_.readBehindSeconds * config_.fps);
                    decoder->SetConfig(dec_config);

                    info->sequence_decoder = std::move(decoder);
                    info->pipeline_mode = seq_meta.pipeline_mode;
                    info->width = info->sequence_decoder->GetWidth();
                    info->height = info->sequence_decoder->GetHeight();
                    info->fps = info->sequence_decoder->GetFPS();
                    info->frame_count = info->sequence_decoder->GetFrameCount();
                    Debug::Log("TimelineCache: ImageSequenceDecoder created for image sequence " + source_path +
                               " (readAhead=" + std::to_string(dec_config.readAheadFrames) + ")");
                    break;
                }
                Debug::Log("TimelineCache: ImageSequenceDecoder init failed, falling back to per-file loader");
            }

            // Fallback: per-file loader (legacy behavior)
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

        // CRITICAL: Propagate current playback state to new loader
        // Without this, loaders created during playback have is_playing_=false
        // and may spawn decoders aggressively (causing traffic jams in loops)
        if (is_playing_.load()) {
            info->SetPlaybackMode(true);
        }

        // Log creation (moved here to only log the winner)
        if (info->media_type == ClipMediaType::VIDEO && info->video_decoder) {
            Debug::Log("TimelineCache: " + std::string(info->video_decoder->GetBackendName()) +
                       " decoder created for " + source_path +
                       " (" + std::to_string(info->width) + "x" + std::to_string(info->height) +
                       " @ " + std::to_string(info->fps) + " fps, " +
                       std::to_string(info->frame_count) + " frames)");
        } else if (info->sequence_decoder) {
            Debug::Log("TimelineCache: ImageSequenceDecoder ready for " + source_path +
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

        try {
            // Get next frame to load
            {
                std::unique_lock<std::mutex> lock(request_mutex_);

                // Wait for work
                request_cv_.wait(lock, [this] {
                    return !io_running_ || !video_requests_.empty() || !direct_source_requests_.empty();
                });

                if (!io_running_) break;

                // NOTE: Direct source requests (slip/trim preview) disabled with timeline_frame caching
                // direct_source_requests_ queue is not used

                if (video_requests_.empty()) continue;

                // Get first request (FIFO - CacheThread adds in priority order)
                timeline_frame = video_requests_.front();
                video_requests_.pop_front();
                video_requests_set_.erase(timeline_frame);  // Keep set in sync

                // Mark as in progress
                requests_in_progress_.insert(timeline_frame);
            }

            if (timeline_frame < 0) continue;

            // Skip IO work during aggressive scrub - GetFrame uses ScrubDecoders exclusively
            // This prevents ManagedVideoDecoder creation which would flood memory
            if (aggressive_scrub_mode_.load() == AggressiveScrubMode::ACTIVE_SCRUBBING) {
                std::lock_guard<std::mutex> lock(request_mutex_);
                requests_in_progress_.erase(timeline_frame);
                continue;  // Frame will be re-requested when scrub ends
            }

            // Convert timeline frame to source coordinates
            SourceCoords coords = TimelineToSource(timeline_frame);
            if (!coords.valid) {
                std::lock_guard<std::mutex> lock(request_mutex_);
                requests_in_progress_.erase(timeline_frame);
                continue;
            }

            TimelineCacheKey key{timeline_frame};

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

            // GStreamer can seek to any frame - no unreachable frame handling needed

            // Check if already cached BEFORE loading pixels
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
                // Frame not ready yet - re-queue for retry
                // FFmpeg StreamingVideoDecoder uses a ring buffer that may not have the frame
                // immediately after a seek. Re-queue to back of queue so other frames can proceed.
                static int requeue_log_count = 0;
                if (requeue_log_count++ < 10) {
                    Debug::Log("TimelineCache: IOWorker re-queue tl_frame=" + std::to_string(timeline_frame) +
                               " src_frame=" + std::to_string(coords.source_frame) +
                               " (pixels not ready)");
                }
                {
                    std::lock_guard<std::mutex> lock(request_mutex_);
                    // Only re-queue if not already queued (avoid duplicates)
                    if (video_requests_set_.count(timeline_frame) == 0) {
                        video_requests_.push_back(timeline_frame);
                        video_requests_set_.insert(timeline_frame);
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

            // DEBUG: Log successful pixel loads (first 5)
            static int pixel_load_log = 0;
            if (pixel_load_log++ < 5) {
                Debug::Log("TimelineCache: IOWorker loaded tl_frame=" + std::to_string(timeline_frame) +
                           " -> src_frame=" + std::to_string(coords.source_frame) +
                           " path_end=" + coords.source_path.substr(coords.source_path.length() > 20 ? coords.source_path.length() - 20 : 0));
            }

            // Queue for GPU upload (with duplicate check and size limit to prevent memory explosion)
            {
                std::lock_guard<std::mutex> lock(upload_mutex_);

                // CRITICAL: Limit pending uploads queue to prevent memory explosion during seek
                // 4K frames are ~33MB each. 16 pending uploads = ~500MB worst case.
                // During rapid seeking, I/O workers can queue faster than ProcessPendingUploads consumes.
                static constexpr size_t MAX_PENDING_UPLOADS = 16;

                // Simple FIFO eviction - drop oldest when full
                while (pending_uploads_.size() >= MAX_PENDING_UPLOADS) {
                    auto& oldest = pending_uploads_.front();
                    pending_uploads_set_.erase(oldest.key);
                    pending_uploads_.pop_front();
                }

                // Check if this key is already pending upload (O(1) lookup)
                if (pending_uploads_set_.count(key) == 0) {
                    pending_uploads_.push_back({key, pixels});
                    pending_uploads_set_.insert(key);
                }
            }

            // Mark segments dirty for visualization update
            segments_dirty_ = true;

        } catch (const std::exception& e) {
            // Log exception but keep thread alive
            Debug::Log("TimelineCache: IOWorker exception: " + std::string(e.what()) +
                       " (frame=" + std::to_string(timeline_frame) + ")");

            // Clean up in-progress state
            if (timeline_frame >= 0) {
                std::lock_guard<std::mutex> lock(request_mutex_);
                requests_in_progress_.erase(timeline_frame);
            }

            // Brief sleep to prevent tight exception loop
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        } catch (...) {
            // Catch-all for COM exceptions, etc.
            Debug::Log("TimelineCache: IOWorker unknown exception (frame=" +
                       std::to_string(timeline_frame) + ") - thread continuing");

            // Clean up in-progress state
            if (timeline_frame >= 0) {
                std::lock_guard<std::mutex> lock(request_mutex_);
                requests_in_progress_.erase(timeline_frame);
            }

            // Brief sleep to prevent tight exception loop
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
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
        try {
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

        // GStreamer handles seeking reliably - no stuck detection needed

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
                    if (loader_to_refine && loader_to_refine->HasBufferedDecoder()) {
                        // force_seek=true bypasses all tolerance checks (buffer proximity, 24-frame threshold)
                        loader_to_refine->UpdatePlayhead(refine_coords.source_frame, SeekQuality::NORMAL, true);
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
                // Don't continue with refinement check
            }
            else if (coords.valid && refine_elapsed < kMaxRefineTimeMs) {
                TimelineCacheKey key{refine_frame};

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
                    auto loader_it = loaders_.find(coords.source_path);
                    if (loader_it != loaders_.end() && loader_it->second &&
                        loader_it->second->HasBufferedDecoder()) {
                        frame_ready = loader_it->second->HasFrame(coords.source_frame);
                    }
                }
            }

            if (frame_ready || refine_elapsed >= kMaxRefineTimeMs) {
                // Frame is available OR timeout reached - transition to IDLE
                // BUT only if user hasn't started scrubbing again (which sets state to SCRUBBING)
                ScrubState expected = ScrubState::REFINING;
                if (scrub_state_.compare_exchange_strong(expected, ScrubState::IDLE)) {
                    // Successfully transitioned from REFINING to IDLE
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
        // Step 0.6: Handle aggressive scrub settling (MULTI_TRACK/DUAL_VIEW only)
        // Called every iteration to check if settle delay has elapsed
        //=====================================================================
        HandleAggressiveScrubSettling();

        //=====================================================================
        // Step 1: Window-based eviction (like EXR cache)
        // Build set of keys that SHOULD stay, evict anything else
        //=====================================================================
        int readBehindFrames = config_.GetReadBehindFrames();
        int readAheadFrames = config_.readAheadFrames;

        {
            // Get the circular cache window - single source of truth (matching backup)
            std::set<int> cache_window = GetCacheWindow(current_frame);

            // DEBUG: Log boundary info on first few iterations
            if (iteration <= 3) {
                Debug::Log("TimelineCache: [WINDOW DEBUG] iter=" + std::to_string(iteration) +
                           " current_frame=" + std::to_string(current_frame) +
                           " readAhead=" + std::to_string(readAheadFrames) +
                           " readBehind=" + std::to_string(readBehindFrames) +
                           " window_size=" + std::to_string(cache_window.size()));
            }

            // Convert to keys, filtering out gaps (invalid coords) - matching backup
            std::set<TimelineCacheKey> keys_to_keep;
            for (int frame : cache_window) {
                SourceCoords coords = TimelineToSource(frame);
                if (coords.valid) {
                    keys_to_keep.insert({frame});
                }
            }

            // Find frames to evict (outside window)
            std::vector<TimelineCacheKey> keys_to_evict;
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);

                // DEBUG: Log cache contents on first few iterations
                if (iteration <= 3 && !frame_cache_.empty()) {
                    Debug::Log("TimelineCache: [CACHE DEBUG] frame_cache_ size=" +
                               std::to_string(frame_cache_.size()));
                    // Log first few cached keys
                    int count = 0;
                    for (const auto& [key, cached_frame] : frame_cache_) {
                        if (count++ < 5) {
                            Debug::Log("  cached key: tl_frame=" + std::to_string(key.timeline_frame));
                        }
                    }
                }

                for (const auto& [key, cached_frame] : frame_cache_) {
                    if (keys_to_keep.find(key) == keys_to_keep.end()) {
                        keys_to_evict.push_back(key);
                    }
                }
            }

            // Evict stale frames from cache
            int cache_evicted = 0;
            int textures_queued = 0;
            if (!keys_to_evict.empty()) {
                // DEBUG: Log eviction details on ANY eviction
                bool is_playing = is_playing_.load();
                bool should_log = true;  // Always log evictions for debugging
                if (should_log) {
                    // Get window range for debugging
                    int window_min = INT_MAX, window_max = INT_MIN;
                    for (const auto& key : keys_to_keep) {
                        if (key.timeline_frame < window_min) window_min = key.timeline_frame;
                        if (key.timeline_frame > window_max) window_max = key.timeline_frame;
                    }
                    // Get evicted range
                    int evict_min = INT_MAX, evict_max = INT_MIN;
                    for (const auto& key : keys_to_evict) {
                        if (key.timeline_frame < evict_min) evict_min = key.timeline_frame;
                        if (key.timeline_frame > evict_max) evict_max = key.timeline_frame;
                    }
                    Debug::Log("TimelineCache: [EVICT] playhead=" + std::to_string(current_frame) +
                               " window=[" + std::to_string(window_min) + "-" + std::to_string(window_max) + "]" +
                               " evict=[" + std::to_string(evict_min) + "-" + std::to_string(evict_max) + "]" +
                               " (#" + std::to_string(keys_to_evict.size()) + ")" +
                               " cached=" + std::to_string(frame_cache_.size()) +
                               " playing=" + std::to_string(is_playing));
                }

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

            // Check cached frames - convert timeline_frame to source path
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                for (const auto& [key, frame] : frame_cache_) {
                    SourceCoords coords = TimelineToSource(key.timeline_frame);
                    if (coords.valid) {
                        active_sources.insert(coords.source_path);
                    }
                }
            }

            // Check pending uploads - convert timeline_frame to source path
            {
                std::lock_guard<std::mutex> lock(upload_mutex_);
                for (const auto& upload : pending_uploads_) {
                    SourceCoords coords = TimelineToSource(upload.key.timeline_frame);
                    if (coords.valid) {
                        active_sources.insert(coords.source_path);
                    }
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
            // Use GetCacheWindow for circular wrapping within loop boundaries
            {
                std::set<int> cache_window = GetCacheWindow(current_frame);
                for (int frame : cache_window) {
                    SourceCoords coords = TimelineToSource(frame);
                    if (coords.valid) {
                        active_sources.insert(coords.source_path);
                    }
                }
            }

            // Remove loaders not in active sources (immediate cleanup)
            // But only if they haven't been used recently (grace period)
            // Note: With shared_ptr, I/O workers hold a reference that keeps loader alive
            // even after removal from map, preventing use-after-free
            //
            // SKIP entirely during aggressive scrub: Removing loaders during rapid scrubbing
            // on multi-layer nested timelines can cause resource exhaustion and crashes.
            AggressiveScrubMode scrub_mode_for_loader_cleanup = aggressive_scrub_mode_.load();
            int loaders_cleaned = 0;
            if (scrub_mode_for_loader_cleanup == AggressiveScrubMode::INACTIVE)
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
            AggressiveScrubMode scrub_mode_for_cleanup = aggressive_scrub_mode_.load();
            if (scrub_mode_for_cleanup == AggressiveScrubMode::INACTIVE)
            {
                // Normal playback: Clear buffers for sources not in active window
                std::lock_guard<std::mutex> lock(loaders_mutex_);
                for (const auto& [path, info] : loaders_) {
                    if (active_sources.find(path) == active_sources.end()) {
                        // This loader is NOT needed for visible clips - clear its buffer
                        if (info->HasBufferedDecoder()) {
                            int buffer_size = info->GetBufferSize();
                            if (buffer_size > 0) {
                                info->ClearBuffer();
                                Debug::Log("TimelineCache: Cleared buffer (" + std::to_string(buffer_size) +
                                           " frames) for inactive source: " + path);
                            }
                        }
                    }
                }
            }
            else if (scrub_mode_for_cleanup == AggressiveScrubMode::ACTIVE_SCRUBBING)
            {
                // Scrub mode: Only keep buffer for CURRENT source, clear others after grace period
                // This prevents RAM explosion while allowing brief back-and-forth scrubbing
                SourceCoords current_coords = TimelineToSource(current_frame);
                const int kScrubBufferGracePeriodMs = 500;  // Keep buffers for 500ms after last use

                std::lock_guard<std::mutex> lock(loaders_mutex_);
                for (const auto& [path, info] : loaders_) {
                    // Never clear the current source's buffer
                    if (current_coords.valid && path == current_coords.source_path) {
                        continue;
                    }

                    // Clear buffers for sources not used recently
                    if (info->HasBufferedDecoder()) {
                        auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - info->last_used_time).count();
                        if (age_ms > kScrubBufferGracePeriodMs) {
                            int buffer_size = info->GetBufferSize();
                            if (buffer_size > 0) {
                                info->ClearBuffer();
                                Debug::Log("TimelineCache: [SCRUB] Cleared stale buffer (" +
                                           std::to_string(buffer_size) + " frames, " +
                                           std::to_string(age_ms) + "ms old): " + path);
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
        AggressiveScrubMode scrub_mode_step1_5 = aggressive_scrub_mode_.load();
        {
            // Find which video sources are in the upcoming window
            // and what frame each decoder should target
            std::map<std::string, int> decoder_targets;  // source_path -> target frame

            // Get boundary info for wrap detection
            int boundary_start = GetBoundaryStart();
            int boundary_end = GetBoundaryEnd();
            int boundary_len = boundary_end - boundary_start + 1;

            // Separate frames into contiguous and wrapped (same logic as fill)
            std::set<int> cache_window = GetCacheWindow(current_frame);
            std::vector<int> contiguous_frames;
            std::vector<int> wrapped_frames;

            for (int frame : cache_window) {
                bool is_wrapped = false;
                int dist_to_end = boundary_end - current_frame;
                int dist_from_start = current_frame - boundary_start;

                if (dist_to_end < config_.readAheadFrames &&
                    (frame - boundary_start) < config_.readAheadFrames) {
                    is_wrapped = true;
                } else if (dist_from_start < config_.GetReadBehindFrames() &&
                           (boundary_end - frame) < config_.GetReadBehindFrames()) {
                    is_wrapped = true;
                }

                if (is_wrapped) {
                    wrapped_frames.push_back(frame);
                } else {
                    contiguous_frames.push_back(frame);
                }
            }

            // Check if contiguous region is fully cached
            bool contiguous_complete = true;
            {
                std::lock_guard<std::mutex> cache_lock(cache_mutex_);
                for (int frame : contiguous_frames) {
                    TimelineCacheKey key{frame};
                    SourceCoords coords = TimelineToSource(frame);
                    if (coords.valid && frame_cache_.find(key) == frame_cache_.end()) {
                        contiguous_complete = false;
                        break;
                    }
                }
            }

            // CRITICAL: First, get the source frame for the CURRENT timeline position
            // This takes priority over readahead frames when multiple clips share a source file
            SourceCoords current_coords = TimelineToSource(current_frame);
            if (current_coords.valid) {
                decoder_targets[current_coords.source_path] = current_coords.source_frame;
            }

            // AGGRESSIVE SCRUB: Skip ALL decoder creation and cache management
            // GetFrame() now uses dedicated ScrubDecoders during scrub, which are lightweight
            // and don't need ManagedVideoDecoder's heavy buffering. Creating ManagedVideoDecoders
            // here would cause memory floods as they each buffer 120+ frames.
            if (scrub_mode_step1_5 == AggressiveScrubMode::ACTIVE_SCRUBBING) {
                // Skip window expansion and Step 2 cache fill entirely
                // ScrubDecoders handle frame access in GetFrame()
                goto skip_step1_5;
            }

            // If contiguous is complete, target the wrapped region instead
            // This allows decoders to seek to the loop start and fill those frames
            //
            // CROSS-CLIP LOOP PREFETCHING:
            // For multi-clip timelines, the wrapped region may contain frames from
            // DIFFERENT source files than the current clip. We must pre-warm ALL
            // source decoders in the wrapped region, not just the current source.
            // Example: Loop from clip C (source X) back to clip A (source Y)
            // - decoder_targets["X"] already set to current frame (keep playing)
            // - decoder_targets["Y"] should be set to first uncached frame at loop start
            if (contiguous_complete && !wrapped_frames.empty()) {
                std::lock_guard<std::mutex> cache_lock(cache_mutex_);
                for (int frame : wrapped_frames) {
                    SourceCoords coords = TimelineToSource(frame);
                    if (!coords.valid) continue;
                    TimelineCacheKey key{frame};
                    if (frame_cache_.find(key) == frame_cache_.end()) {
                        // This wrapped frame needs caching
                        // Add its source if not already targeted (preserves current clip's target)
                        if (decoder_targets.find(coords.source_path) == decoder_targets.end()) {
                            decoder_targets[coords.source_path] = coords.source_frame;
                        }
                        // Continue loop to find ALL unique sources in wrapped region
                    }
                }
            }

            // During post-edit, skip read-behind frames - decoder can only fill forward
            bool skip_read_behind_window = post_edit_pending_.load();

            // Add other sources from the window (for multi-clip timelines)
            for (int frame : contiguous_frames) {
                if (skip_read_behind_window && frame < current_frame) continue;

                SourceCoords coords = TimelineToSource(frame);
                if (!coords.valid) continue;

                auto it = decoder_targets.find(coords.source_path);
                if (it == decoder_targets.end()) {
                    decoder_targets[coords.source_path] = coords.source_frame;
                }
            }

            // Pre-create decoders for upcoming clips (before we reach them)
            // This ensures smooth gap→clip transitions
        create_decoders:
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
            //
            // NEW: Track which decoder is for the current clip vs upcoming clips (prefetch)
            // Upcoming clips use is_prefetch=true to avoid aggressive respawning during seeks
            struct DecoderUpdate {
                std::shared_ptr<ClipLoaderInfo> loader;
                int target_frame;
                bool is_prefetch;  // true for upcoming clips, false for current clip
            };
            std::vector<DecoderUpdate> decoder_updates;
            {
                std::lock_guard<std::mutex> lock(loaders_mutex_);
                for (const auto& [source_path, target_frame] : decoder_targets) {
                    auto it = loaders_.find(source_path);
                    if (it != loaders_.end() && it->second) {
                        // Determine if this is the current clip or an upcoming clip (prefetch)
                        // Current clip = the clip at the playhead position
                        bool is_current_clip = current_coords.valid &&
                                               source_path == current_coords.source_path;
                        decoder_updates.push_back({it->second, target_frame, !is_current_clip});
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

            // DEBUG: Log decoder targets with source paths
            static int targets_log_count = 0;
            if (++targets_log_count <= 10 || targets_log_count % 500 == 0) {
               /* Debug::Log("TimelineCache: decoder_targets at frame " + std::to_string(current_frame) +
                           " (current_coords: " + (current_coords.valid ? current_coords.source_path : "INVALID") + ")");*/
                for (const auto& [source_path, target_frame] : decoder_targets) {
                    bool is_current = current_coords.valid && source_path == current_coords.source_path;
                    // Extract just filename for readability
                    std::string filename = source_path;
                    size_t pos = filename.find_last_of("/\\");
                    if (pos != std::string::npos) filename = filename.substr(pos + 1);
                    /*Debug::Log("  -> " + filename + " target=" + std::to_string(target_frame) +
                               (is_current ? " [current]" : " [prefetch]"));*/
                }
            }

            // SKIP playhead updates during aggressive scrub - UpdatePlayhead triggers decoder
            // shuttle mode which buffers 120 frames. During scrub, GetFrame accesses decoder
            // buffers directly for immediate frames without triggering full buffering.
            bool skip_during_scrub = (scrub_mode_step1_5 == AggressiveScrubMode::ACTIVE_SCRUBBING);

            for (auto& update : decoder_updates) {
                if (update.loader->HasBufferedDecoder() && !skip_playhead_update && !skip_during_scrub) {
                    // Pass is_prefetch so upcoming clips don't get their decoders respawned aggressively
                    update.loader->UpdatePlayhead(update.target_frame, seek_quality, false, update.is_prefetch);
                }
            }

            // NOTE: Decoder eviction removed - it's redundant.
            // Step 1 of CacheManagementThread handles circular eviction at the
            // frame_cache_ (GPU texture) level using GetCacheWindow() -> engine.
            // The decoder just buffers frames with a safety size limit; it doesn't
            // need to be circular-aware.
        }

        skip_step1_5:
        //=====================================================================
        // Step 2: Fill cache using CacheWindowEngine (single source of truth)
        // Engine returns frames sorted by priority: current, ahead, behind
        //
        // SKIP during aggressive scrub: Cache filling is wasteful during scrub.
        // GetFrame gets frames directly from decoder buffers.
        //=====================================================================
        if (scrub_mode_step1_5 != AggressiveScrubMode::ACTIVE_SCRUBBING)
        {
            static int step2_run_count = 0;
            if (++step2_run_count <= 5 || step2_run_count % 100 == 0) {
                Debug::Log("TimelineCache: [STEP2] Running cache fill, scrub_mode=" +
                           std::to_string(static_cast<int>(scrub_mode_step1_5)));
            }
            std::lock_guard<std::mutex> lock(request_mutex_);

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

            // Memory safety limit only - not for flow control
            // Flow is naturally controlled by batch_limit and I/O completion rate
            const size_t MAX_PENDING_SAFETY = 256;
            size_t total_pending = video_requests_.size() + requests_in_progress_.size();
            int max_to_request = batch_limit;
            if (total_pending > MAX_PENDING_SAFETY) {
                max_to_request = 1;  // Safety valve - something is backed up
            }

            // Get priority-sorted frame window from CacheWindowEngine
            // The engine returns frames in optimal order: current, ahead (by distance), behind (by distance)
            // All circular math (wrap-around at boundaries) is handled by the engine
            std::vector<int> frames_ordered = cache_engine_.GetFrameWindow();

            int requested_count = 0;
            int skip_behind = 0, skip_cached = 0, skip_in_progress = 0, skip_queued = 0, skip_uploading = 0, skip_invalid = 0;

            // Skip read-behind during post-edit grace period
            // After an edit, the decoder seeks to current position and can only fill FORWARD
            bool skip_read_behind = post_edit_pending_.load();

            for (int frame : frames_ordered) {
                if (requested_count >= max_to_request) break;

                // Skip read-behind frames during post-edit
                // CircularDistance returns negative if frame is behind current_frame
                if (skip_read_behind && cache_engine_.CircularDistance(current_frame, frame) < 0) {
                    skip_behind++;
                    continue;
                }

                // Check if already cached
                SourceCoords coords = TimelineToSource(frame);
                if (!coords.valid) { skip_invalid++; continue; }

                // SKIP VIDEO CLIPS in VIDEO_FILE mode - GStreamer handles decoding internally
                // For MULTI_TRACK mode, VIDEO clips use the cache path (FFmpeg decoders)
                ClipMediaType media_type = DetectMediaType(coords.source_path);
                if (media_type == ClipMediaType::VIDEO && source_mode_ == TimelineSourceMode::VIDEO_FILE) {
                    continue;  // GStreamer decoder handles this - no need to queue
                }

                TimelineCacheKey key{frame};
                {
                    std::lock_guard<std::mutex> cache_lock(cache_mutex_);
                    if (frame_cache_.find(key) != frame_cache_.end()) { skip_cached++; continue; }
                }

                // Check if already pending/in-progress/awaiting upload (O(1) lookups)
                if (requests_in_progress_.count(frame) > 0) { skip_in_progress++; continue; }
                if (video_requests_set_.count(frame) > 0) { skip_queued++; continue; }

                // CRITICAL: Also check pending uploads - frames decoded but not yet uploaded
                // Without this, dropped frames from upload queue get re-requested immediately,
                // creating decode thrashing that starves the upload queue
                {
                    std::lock_guard<std::mutex> upload_lock(upload_mutex_);
                    if (pending_uploads_set_.count(key) > 0) { skip_uploading++; continue; }
                }

                // Add to request queue (keep deque and set in sync) - IMAGE_SEQUENCE only
                video_requests_.push_back(frame);
                video_requests_set_.insert(frame);
                requested_count++;
            }

            // DIAGNOSTIC: Log fill state (reduced frequency - every 500 iterations)
            static int diag_count = 0;
            if (++diag_count % 500 == 0) {
                Debug::Log("FILL: playhead=" + std::to_string(current_frame) +
                          " window=" + std::to_string(frames_ordered.size()) +
                          " req=" + std::to_string(requested_count) +
                          " [cached=" + std::to_string(skip_cached) +
                          " inprog=" + std::to_string(skip_in_progress) +
                          " queued=" + std::to_string(skip_queued) +
                          " upload=" + std::to_string(skip_uploading) +
                          "] pending=" + std::to_string(video_requests_.size()) +
                          "+" + std::to_string(requests_in_progress_.size()));
            }

            // Wake I/O threads if we added requests
            if (requested_count > 0) {
                request_cv_.notify_all();
            }
        }
        else
        {
            static int step2_skip_count = 0;
            if (++step2_skip_count <= 5 || step2_skip_count % 100 == 0) {
                Debug::Log("TimelineCache: [STEP2] SKIPPED - aggressive scrub active");
            }
        }

        //=====================================================================
        // Step 3: Priority touching (like EXR cache)
        // Touch cached frames in REVERSE distance order so frames closest to
        // playhead stay in cache longest (LRU keeps most recently touched)
        //=====================================================================
        if (config_.use_shared_pool) {
            // Get the cache window (already respects boundaries via GetCacheWindow)
            std::set<int> cache_window = GetCacheWindow(current_frame);

            // Build list sorted by distance from playhead (furthest first)
            std::vector<std::pair<int, int>> frames_with_dist;  // (distance, frame)
            for (int frame : cache_window) {
                int dist = FrameDistance(current_frame, frame);
                frames_with_dist.push_back({dist, frame});
            }
            // Sort by distance descending (furthest first, so closest touched last)
            std::sort(frames_with_dist.begin(), frames_with_dist.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });

            // Touch cached frames in order
            std::lock_guard<std::mutex> lock(cache_mutex_);
            for (const auto& [dist, frame] : frames_with_dist) {
                TimelineCacheKey key{frame};
                if (frame_cache_.find(key) != frame_cache_.end()) {
                    TouchInPool(key);
                }
            }
        }

        } catch (const std::exception& e) {
            // Log exception but keep thread alive
            Debug::Log("TimelineCache: CacheManagementThread exception: " + std::string(e.what()));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } catch (...) {
            // Catch-all for COM exceptions, etc.
            Debug::Log("TimelineCache: CacheManagementThread unknown exception - continuing");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    Debug::Log("TimelineCache: Cache management thread stopped");
}

std::shared_ptr<PixelData> TimelineCache::LoadPixels(const TimelineCacheKey& key) {
    // Convert timeline frame to source coordinates
    SourceCoords coords = TimelineToSource(key.timeline_frame);
    if (!coords.valid) {
        return nullptr;  // Gap or unlinked clip
    }

    // Get shared_ptr to loader - this keeps it alive even if removed from map by cleanup
    auto loader_info = GetOrCreateLoader(coords.source_path);
    if (!loader_info) {
        //Debug::Log("TimelineCache::LoadPixels: No loader for " + coords.source_path);
        return nullptr;
    }

    std::shared_ptr<PixelData> result;

    // Check for buffered decoders first (video or sequence)
    if (loader_info->HasBufferedDecoder()) {
        // For VIDEO and SEQUENCE clips: use buffered decoder
        // NOTE: Don't call UpdatePlayhead here - that's done by CacheManagementThread
        // to avoid seek thrashing from multiple I/O threads.
        // Just get the frame from buffer (instant if buffered).
        result = loader_info->GetFrame(coords.source_frame);

        // Frame not yet buffered - will be retried. This is normal during buffer fill.
    } else if (loader_info->image_loader) {
        // Fallback: For IMAGE_SEQUENCE/EXR clips without sequence metadata
        // Use legacy per-file image loader
        result = loader_info->image_loader->LoadFrame(
            coords.source_path,
            "",  // layer (used by EXR)
            loader_info->pipeline_mode
        );
    }

    return result;
}

//=============================================================================
// GPU Upload
//=============================================================================

GLuint TimelineCache::CreateGLTexture(const std::shared_ptr<PixelData>& pixels) {
    if (!pixels || pixels->pixels.empty()) return 0;

    // Save current GL state to avoid corrupting ImGui during render
    GLint previous_texture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);

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

    // Restore previous texture binding (critical for ImGui compatibility)
    glBindTexture(GL_TEXTURE_2D, previous_texture);

    s_textures_created++;
    return texture;
}

#ifdef _WIN32
//=============================================================================
// D3D11 Rendering Mode
//=============================================================================

void TimelineCache::SetD3D11RenderingMode(bool enabled) {
    if (use_d3d11_rendering_ == enabled) return;

    Debug::Log("TimelineCache: D3D11 rendering mode " + std::string(enabled ? "enabled" : "disabled"));
    use_d3d11_rendering_ = enabled;

    // When switching modes, clear the cache since texture types change
    if (initialized_) {
        ClearCache();
    }
}

bool TimelineCache::CreateD3D11Texture(const std::shared_ptr<PixelData>& pixels,
                                        Microsoft::WRL::ComPtr<ID3D11Texture2D>& out_texture,
                                        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& out_srv) {
    if (!pixels || pixels->pixels.empty()) return false;

    auto& device_mgr = D3D11DeviceManager::Instance();
    if (!device_mgr.IsInitialized()) {
        Debug::Log("TimelineCache: D3D11DeviceManager not initialized");
        return false;
    }

    // Determine DXGI format based on pixel data format
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT bytes_per_pixel = 4;

    if (pixels->gl_type == GL_HALF_FLOAT) {
        format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        bytes_per_pixel = 8;
    } else if (pixels->gl_type == GL_UNSIGNED_SHORT) {
        format = DXGI_FORMAT_R16G16B16A16_UNORM;
        bytes_per_pixel = 8;
    }

    // Create the texture
    out_texture = device_mgr.CreateTexture2D(
        static_cast<UINT>(pixels->width),
        static_cast<UINT>(pixels->height),
        format,
        D3D11_USAGE_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE);

    if (!out_texture) {
        Debug::Log("TimelineCache: Failed to create D3D11 texture");
        return false;
    }

    // Upload pixel data
    UINT row_pitch = static_cast<UINT>(pixels->width) * bytes_per_pixel;
    if (!device_mgr.UploadTextureData(out_texture.Get(),
                                       pixels->pixels.data(),
                                       static_cast<UINT>(pixels->width),
                                       static_cast<UINT>(pixels->height),
                                       row_pitch)) {
        Debug::Log("TimelineCache: Failed to upload texture data");
        out_texture.Reset();
        return false;
    }

    // Create shader resource view
    out_srv = device_mgr.CreateSRV(out_texture.Get(), format);
    if (!out_srv) {
        Debug::Log("TimelineCache: Failed to create SRV");
        out_texture.Reset();
        return false;
    }

    return true;
}

ID3D11ShaderResourceView* TimelineCache::GetFrameD3D11(int timeline_frame, int& width, int& height,
                                                        bool* got_exact_frame) {
    if (!initialized_ || !use_d3d11_rendering_) return nullptr;

    if (got_exact_frame) *got_exact_frame = false;

    // Map timeline frame to source coordinates
    SourceCoords coords = TimelineToSource(timeline_frame);
    if (!coords.valid) {
        // Gap or invalid position
        width = canvas_width_;
        height = canvas_height_;
        return nullptr;
    }

    TimelineCacheKey key{timeline_frame};

    // Check cache
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = frame_cache_.find(key);
        if (it != frame_cache_.end() && it->second.srv_d3d) {
            width = it->second.width;
            height = it->second.height;
            if (got_exact_frame) *got_exact_frame = true;

            // Touch in pool
            if (config_.use_shared_pool) {
                TouchInPool(key);
            }

            return it->second.srv_d3d.Get();
        }
    }

    // During aggressive scrub, don't create new decoders - return null and let caller use fallback
    if (aggressive_scrub_mode_.load() == AggressiveScrubMode::ACTIVE_SCRUBBING) {
        return nullptr;
    }

    // Try to get from decoder ring buffer for immediate display
    auto loader_info = GetOrCreateLoader(coords.source_path);
    if (loader_info && loader_info->HasBufferedDecoder()) {
        auto pixels = loader_info->GetFrame(coords.source_frame);
        if (pixels) {
            // Frame is in decoder buffer, create D3D11 texture immediately
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;

            if (CreateD3D11Texture(pixels, texture, srv)) {
                // Store in cache
                CachedFrame frame;
                frame.texture_d3d = texture;
                frame.srv_d3d = srv;
                frame.width = pixels->width;
                frame.height = pixels->height;
                frame.byte_size = pixels->ByteSize();

                {
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    frame_cache_[key] = frame;
                }

                width = pixels->width;
                height = pixels->height;
                if (got_exact_frame) *got_exact_frame = true;

                segments_dirty_ = true;

                if (config_.use_shared_pool) {
                    RegisterWithPool(key, frame.byte_size);
                }

                return srv.Get();
            }
        }
    }

    // Frame not ready - request it
    cache_misses_++;
    RequestFrame(timeline_frame);

    return nullptr;
}
#endif

//=============================================================================
// SharedMemoryPool Integration
//=============================================================================

void TimelineCache::RegisterWithPool(const TimelineCacheKey& key, size_t bytes) {
    // Convert timeline_frame to source for pool key (pool uses source-based keys)
    SourceCoords coords = TimelineToSource(key.timeline_frame);
    if (!coords.valid) return;

    auto pool_key = MakeTimelineKey(coords.source_path, coords.source_frame);

    SharedMemoryPool::Instance().RegisterEntry(
        pool_key,
        bytes,
        [this, key]() { OnEvicted(key); }
    );
}

void TimelineCache::TouchInPool(const TimelineCacheKey& key) {
    if (!config_.use_shared_pool) return;

    // Convert timeline_frame to source for pool key
    SourceCoords coords = TimelineToSource(key.timeline_frame);
    if (!coords.valid) return;

    auto pool_key = MakeTimelineKey(coords.source_path, coords.source_frame);
    SharedMemoryPool::Instance().TouchEntry(pool_key);
}

void TimelineCache::RemoveFromPool(const TimelineCacheKey& key) {
    if (!config_.use_shared_pool) return;

    // Convert timeline_frame to source for pool key
    SourceCoords coords = TimelineToSource(key.timeline_frame);
    if (!coords.valid) return;

    auto pool_key = MakeTimelineKey(coords.source_path, coords.source_frame);
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

    // Determine internal format based on pipeline mode
    // NOTE: HDR_RES (float) not available for video - GStreamer only supports integer formats
    GLenum target_format = GL_RGBA8;
    if (video_pipeline_mode_ == PipelineMode::HIGH_RES) {
        target_format = GL_RGBA16;  // 16-bit integer - best available for video
    }

    // Create/resize output texture if needed (also recreate if format changed)
    if (letterbox_output_width_ != canvas_width_ ||
        letterbox_output_height_ != canvas_height_ ||
        letterbox_output_format_ != target_format) {
        // Delete old resources
        if (letterbox_output_texture_ != 0) {
            glDeleteTextures(1, &letterbox_output_texture_);
        }
        if (letterbox_fbo_ != 0) {
            glDeleteFramebuffers(1, &letterbox_fbo_);
        }

        // Create output texture with appropriate format for HDR/High-Res
        glGenTextures(1, &letterbox_output_texture_);
        glBindTexture(GL_TEXTURE_2D, letterbox_output_texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, target_format, canvas_width_, canvas_height_, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        letterbox_output_format_ = target_format;

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

//=============================================================================
// Shuttle Mode - FF/RW support
//=============================================================================

//=============================================================================
// Aggressive Scrub Mode Implementation (MULTI_TRACK/DUAL_VIEW only)
//=============================================================================

void TimelineCache::SetAggressiveScrubMode(bool enabled) {
    // Only for MULTI_TRACK/DUAL_VIEW - VIDEO_FILE/IMAGE_SEQUENCE use normal shuttle
    if (source_mode_ != TimelineSourceMode::MULTI_TRACK &&
        source_mode_ != TimelineSourceMode::DUAL_VIEW) {
        return;
    }

    if (enabled) {
        AggressiveScrubMode expected = AggressiveScrubMode::INACTIVE;
        if (aggressive_scrub_mode_.compare_exchange_strong(expected, AggressiveScrubMode::ACTIVE_SCRUBBING)) {
            Debug::Log("TimelineCache: Aggressive scrub mode STARTED");

            // Put all managed_decoders into shuttle mode (unthrottled decode)
            std::lock_guard<std::mutex> lock(loaders_mutex_);
            for (auto& [path, loader] : loaders_) {
                if (loader && loader->managed_decoder) {
                    loader->SetShuttleMode(true, 0);  // 0 = no direction preference
                }
            }
        }
        // Always update the timestamp when we get movement
        aggressive_scrub_last_move_ = std::chrono::steady_clock::now();
    } else {
        // Transition to SETTLING (HandleAggressiveScrubSettling will complete)
        AggressiveScrubMode expected = AggressiveScrubMode::ACTIVE_SCRUBBING;
        if (aggressive_scrub_mode_.compare_exchange_strong(expected, AggressiveScrubMode::SETTLING)) {
            aggressive_scrub_last_move_ = std::chrono::steady_clock::now();
            Debug::Log("TimelineCache: Aggressive scrub mode -> SETTLING");

            // Clear scrub decoders to free memory - normal decoders will take over
            // This prevents having both scrub decoders and managed decoders active
            scrub_decoders_.ClearAll();
        }
    }
}

bool TimelineCache::IsAggressiveScrubMode() const {
    return aggressive_scrub_mode_.load() != AggressiveScrubMode::INACTIVE;
}

// NOTE: GetDirectDecoderFrame and GetAggressiveScrubFrame have been removed.
// Scrub handling is now integrated directly into GetFrame() for better performance.
// The inline implementation accesses decoder buffers directly without calling
// SetNeededFrames() or GetOrCreateLoader(), avoiding the pre-buffering that
// caused scrubbing to be slow.

void TimelineCache::HandleAggressiveScrubSettling() {
    // Only process if in SETTLING state
    if (aggressive_scrub_mode_.load() != AggressiveScrubMode::SETTLING) {
        return;
    }

    // Check if settle delay has elapsed
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - aggressive_scrub_last_move_).count();

    if (elapsed_ms < kAggressiveScrubSettleDelayMs) {
        return;  // Still waiting for settle
    }

    Debug::Log("TimelineCache: Aggressive scrub settling complete, transitioning to INACTIVE");

    // Exit shuttle mode on all managed decoders
    int current_frame = current_frame_.load();
    SourceCoords coords = TimelineToSource(current_frame);

    {
        std::lock_guard<std::mutex> lock(loaders_mutex_);
        for (auto& [path, loader] : loaders_) {
            if (loader && loader->managed_decoder && loader->IsShuttleMode()) {
                loader->ExitShuttle();

                // HardReset the decoder for the current clip to ensure exact frame
                if (coords.valid && coords.source_path == path) {
                    loader->HardReset(coords.source_frame);
                    Debug::Log("TimelineCache: HardReset decoder for " + path +
                               " at frame " + std::to_string(coords.source_frame));
                }
            }
        }
    }

    // Clean up held texture
    if (aggressive_held_texture_ != 0) {
        std::lock_guard<std::mutex> lock(delete_mutex_);
        textures_to_delete_.push_back(aggressive_held_texture_);
        aggressive_held_texture_ = 0;
        aggressive_held_width_ = 0;
        aggressive_held_height_ = 0;
        aggressive_held_frame_ = -1;
        aggressive_held_source_.clear();
    }

    // Transition to INACTIVE
    aggressive_scrub_mode_ = AggressiveScrubMode::INACTIVE;
}

void TimelineCache::SetShuttleMode(bool enabled, int direction) {
    if (enabled && !shuttle_active_) {
        // Starting shuttle mode
        shuttle_active_ = true;
        shuttle_direction_ = direction;
        shuttle_last_texture_ = 0;
        shuttle_last_width_ = 0;
        shuttle_last_height_ = 0;

        Debug::Log("TimelineCache: Shuttle mode STARTED, direction=" + std::to_string(direction));

        // Propagate to all active loaders
        std::lock_guard<std::mutex> lock(loaders_mutex_);
        for (auto& [path, loader] : loaders_) {
            if (loader && loader->HasBufferedDecoder()) {
                loader->SetShuttleMode(true, direction);
            }
        }
    }
    else if (!enabled && shuttle_active_) {
        // Exiting shuttle mode
        ExitShuttleMode();
    }
}

GLuint TimelineCache::GetShuttleFrame(int timeline_frame, int& width, int& height) {
    if (!shuttle_active_) {
        return GetFrame(timeline_frame, width, height, nullptr);
    }

    // Map timeline frame to source
    SourceCoords coords = TimelineToSource(timeline_frame);
    if (!coords.valid) {
        // Gap - return gap texture
        if (gap_texture_ != 0) {
            width = canvas_width_ > 0 ? canvas_width_ : gap_texture_width_;
            height = canvas_height_ > 0 ? canvas_height_ : gap_texture_height_;
            return gap_texture_;
        }
        return 0;
    }

    // Get loader for this source
    std::shared_ptr<ClipLoaderInfo> loader_info;
    {
        std::lock_guard<std::mutex> lock(loaders_mutex_);
        auto it = loaders_.find(coords.source_path);
        if (it != loaders_.end()) {
            loader_info = it->second;
        }
    }

    if (!loader_info || !loader_info->HasBufferedDecoder()) {
        // No decoder - fall back to normal GetFrame
        return GetFrame(timeline_frame, width, height, nullptr);
    }

    // ALWAYS call UpdateShuttle to harvest frames from decoder (even when rate-limited)
    // This ensures we continuously pull frames from the decode buffer into the shuttle queue
    auto pixels = loader_info->UpdateShuttle(coords.source_frame);

    // RATE LIMIT: Only update texture at ~30fps max to prevent UI stutter
    // The playhead moves smoothly via time-based calculation, but texture
    // uploads for large frames (4K+) can block the render thread.
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - shuttle_last_texture_time_).count();

    const int kMinTextureUpdateMs = 33;  // ~30fps max texture update rate
    if (elapsed_ms < kMinTextureUpdateMs && shuttle_composited_texture_ != 0) {
        // Return cached texture but we already harvested above
        width = canvas_width_ > 0 ? canvas_width_ : shuttle_last_width_;
        height = canvas_height_ > 0 ? canvas_height_ : shuttle_last_height_;
        return shuttle_composited_texture_;
    }

    // pixels already obtained from UpdateShuttle above (harvesting happens regardless of rate limit)
    if (pixels) {
        // Create/update texture from pixels
        // For efficiency, we reuse shuttle_last_texture_ if dimensions match
        if (shuttle_last_texture_ != 0 &&
            shuttle_last_width_ == pixels->width &&
            shuttle_last_height_ == pixels->height) {
            // Reuse existing texture, just update content
            glBindTexture(GL_TEXTURE_2D, shuttle_last_texture_);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                            pixels->width, pixels->height,
                            GL_RGBA, GL_UNSIGNED_BYTE,
                            pixels->pixels.data());
            glBindTexture(GL_TEXTURE_2D, 0);
        } else {
            // Need new texture
            if (shuttle_last_texture_ != 0) {
                glDeleteTextures(1, &shuttle_last_texture_);
            }
            shuttle_last_texture_ = CreateGLTexture(pixels);
            shuttle_last_width_ = pixels->width;
            shuttle_last_height_ = pixels->height;
        }

        // Update timestamp for rate limiting
        shuttle_last_texture_time_ = now;

        // Determine final output texture (with aspect ratio compositing if needed)
        GLuint result_texture = shuttle_last_texture_;
        if (canvas_width_ > 0 && canvas_height_ > 0) {
            width = canvas_width_;
            height = canvas_height_;
            // Composite to canvas if dimensions differ
            if (pixels->width != canvas_width_ || pixels->height != canvas_height_) {
                result_texture = CompositeFrameToCanvas(shuttle_last_texture_, pixels->width, pixels->height);
            }
        } else {
            width = pixels->width;
            height = pixels->height;
        }

        // Cache the composited result for fast return during rate limiting
        shuttle_composited_texture_ = result_texture;
        return result_texture;
    }

    // No shuttle frame available - return last composited texture if we have one
    if (shuttle_composited_texture_ != 0) {
        width = canvas_width_ > 0 ? canvas_width_ : shuttle_last_width_;
        height = canvas_height_ > 0 ? canvas_height_ : shuttle_last_height_;
        return shuttle_composited_texture_;
    }

    // Fall back to last_good_texture_
    if (last_good_texture_ != 0) {
        width = canvas_width_ > 0 ? canvas_width_ : last_good_width_;
        height = canvas_height_ > 0 ? canvas_height_ : last_good_height_;
        return last_good_texture_;
    }

    return 0;
}

int TimelineCache::ExitShuttleMode() {
    if (!shuttle_active_) {
        return current_frame_.load();
    }

    shuttle_active_ = false;
    int snap_frame = current_frame_.load();

    Debug::Log("TimelineCache: Shuttle mode EXITED");

    // Exit shuttle on all loaders and force immediate frame for current position
    std::lock_guard<std::mutex> lock(loaders_mutex_);
    SourceCoords coords = TimelineToSource(snap_frame);

    for (auto& [path, loader] : loaders_) {
        if (loader && loader->HasBufferedDecoder() && loader->IsShuttleMode()) {
            loader->ExitShuttle();

            // Force immediate frame decode for the current clip
            if (coords.valid && coords.source_path == path) {
                // Trigger synchronous seek to ensure frame is ready immediately
                loader->UpdatePlayhead(coords.source_frame, SeekQuality::NORMAL, true);
            }
        }
    }

    // Clean up shuttle textures
    // Note: shuttle_composited_texture_ may point to letterbox_output_texture_
    // which is shared/reused, so don't delete it - just reset the pointer
    shuttle_composited_texture_ = 0;

    if (shuttle_last_texture_ != 0) {
        glDeleteTextures(1, &shuttle_last_texture_);
        shuttle_last_texture_ = 0;
        shuttle_last_width_ = 0;
        shuttle_last_height_ = 0;
    }

    return snap_frame;
}

} // namespace ump
