#include "timeline_cache.h"
#include "timeline_view.h"
#include "../player/image_loaders.h"
#include "../utils/debug_utils.h"

#ifdef QCVIEW_USE_VULKAN
#include "../gpu/vulkan_texture_pool.h"
#endif

#ifdef _WIN32
#include "../gpu/d3d11_device_manager.h"
#include "../gpu/d3d11_video_interop.h"
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

// Image sequence cache settings (from Settings > Image Sequences panel)
extern int g_exr_read_ahead_frames;   // Frames to cache ahead
extern int g_read_behind_frames;      // Frames to keep behind playhead
extern int g_exr_thread_count;        // DirectEXRCache parallel I/O threads

namespace fs = std::filesystem;

namespace qcview {

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
    // Default configuration
    config_.readAheadFrames = 108;      // ~4.5 seconds @ 24fps
    config_.readBehindFrames = 12;      // ~0.5s behind for backward scrub @ 24fps
    config_.io_threads = 1;             // 1 thread for VIDEO_FILE mode (D3D11VideoDecoder handles decoding)
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
    // - VIDEO_FILE: D3D11VideoDecoder handles decoding internally - 1 thread sufficient
    // - DUAL_VIEW/PLAYLIST: May need multiple threads for parallel decode
    if (source_mode == TimelineSourceMode::VIDEO_FILE) {
        config_.io_threads = 1;
    } else {
        // Use user setting (default 8) for DUAL_VIEW/PLAYLIST modes
        config_.io_threads = g_timeline_io_threads;
    }

    Debug::Log("TimelineCache: Source mode = " +
               std::string(source_mode == TimelineSourceMode::VIDEO_FILE ? "VIDEO_FILE (D3D11)" :
                          source_mode == TimelineSourceMode::DUAL_VIEW ? "DUAL_VIEW" :
                          source_mode == TimelineSourceMode::PLAYLIST ? "PLAYLIST" : "OTHER"));

    // Enable single-decoder mode for PLAYLIST (EDL-style) when configured
    // This eliminates D3D11 contention crashes during rapid seeking
#ifdef _WIN32
    if (source_mode == TimelineSourceMode::PLAYLIST && config_.use_single_decoder) {
        use_single_decoder_mode_ = true;

        single_decoder_ = std::make_unique<PlaylistSingleDecoder>();

        auto& device_mgr = D3D11DeviceManager::Instance();
        if (!device_mgr.IsInitialized()) {
            Debug::Log("TimelineCache: ERROR: D3D11DeviceManager not initialized - cannot use single decoder");
            single_decoder_.reset();
            use_single_decoder_mode_ = false;
        } else if (!single_decoder_->Initialize(device_mgr.GetDevice())) {
            Debug::Log("TimelineCache: ERROR: Failed to initialize single decoder");
            single_decoder_.reset();
            use_single_decoder_mode_ = false;
        } else {
            Debug::Log("TimelineCache: *** SINGLE-DECODER MODE ENABLED ***");
            // Reduce I/O threads in single-decoder mode - one decoder handles all
            config_.io_threads = 1;
        }
    }
#endif

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
    // Use the maximum of timeline and image sequence settings to support both content types
    // Image sequences need larger windows; video decoders have their own internal limits
    int effective_behind = std::max(config_.readBehindFrames, g_read_behind_frames);
    int effective_ahead = std::max(config_.readAheadFrames, g_exr_read_ahead_frames);
    cache_engine_.SetTotalFrames(total_timeline_frames_);
    cache_engine_.SetWindow(effective_behind, effective_ahead);

    // Use linear mode for DUAL_VIEW - don't wrap cache window at boundaries
    // These modes have clips at defined positions; wrapping would pre-cache clips we've passed
    bool use_linear = (source_mode == TimelineSourceMode::DUAL_VIEW);
    cache_engine_.SetLinearMode(use_linear);

    Debug::Log("TimelineCache: Window set - behind=" + std::to_string(effective_behind) +
               " ahead=" + std::to_string(effective_ahead) +
               " total=" + std::to_string(effective_behind + effective_ahead) +
               " linear=" + (use_linear ? "yes" : "no") +
               " (timeline: " + std::to_string(config_.readBehindFrames) + "/" + std::to_string(config_.readAheadFrames) +
               ", image: " + std::to_string(g_read_behind_frames) + "/" + std::to_string(g_exr_read_ahead_frames) + ")");

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

    //=========================================================================
    // IMAGE_SEQUENCE mode: Use DirectEXRCache instead of I/O workers
    // DirectEXRCache has proven adaptive speed control that actually works
    //=========================================================================
    if (source_mode == TimelineSourceMode::IMAGE_SEQUENCE) {
        // Find the primary sequence clip (first video track, first clip)
        const OTIOClip* seq_clip = nullptr;
        for (const auto& track : tracks) {
            if (track.is_video && !track.clips.empty()) {
                for (const auto& clip : track.clips) {
                    if (clip.is_sequence && !clip.is_gap) {
                        seq_clip = &clip;
                        break;
                    }
                }
                if (seq_clip) break;
            }
        }

        if (seq_clip && seq_clip->sequence_end_frame >= seq_clip->sequence_start_frame) {
            use_direct_exr_cache_ = true;
            direct_exr_cache_source_path_ = seq_clip->linked_path;  // Track source for solo mode
            fps_ = fps;  // Store fps for position conversion

            // Build file list from sequence metadata
            std::vector<std::string> sequence_files;
            fs::path dir(seq_clip->sequence_directory);
            for (int frame = seq_clip->sequence_start_frame; frame <= seq_clip->sequence_end_frame; ++frame) {
                char frame_name[1024];
                snprintf(frame_name, sizeof(frame_name), seq_clip->sequence_pattern.c_str(), frame);
                sequence_files.push_back((dir / frame_name).string());
            }

            // CRITICAL: Sync total_timeline_frames_ with actual sequence file count
            // The duration-based calculation (timeline_duration_ * fps) can be off by one
            // due to floating-point precision. For IMAGE_SEQUENCE, use exact file count.
            int actual_frame_count = static_cast<int>(sequence_files.size());
            if (total_timeline_frames_ != actual_frame_count) {
                Debug::Log("TimelineCache: Adjusting total_timeline_frames_ from " +
                           std::to_string(total_timeline_frames_) + " to " +
                           std::to_string(actual_frame_count) + " (sequence file count)");
                total_timeline_frames_ = actual_frame_count;
                // Also update cache engine
                cache_engine_.SetTotalFrames(total_timeline_frames_);
            }

            // Create and initialize DirectEXRCache
            direct_exr_cache_ = std::make_unique<DirectEXRCache>();

            EXRCacheConfig exr_config;
            exr_config.readAheadFrames = g_exr_read_ahead_frames;
            exr_config.readBehindFrames = g_read_behind_frames;
            exr_config.threadCount = static_cast<size_t>(g_exr_thread_count);
            direct_exr_cache_->SetConfig(exr_config);

            // Detect format from first file extension and create appropriate loader
            std::string ext = fs::path(sequence_files[0]).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            std::unique_ptr<IImageLoader> loader;
            PipelineMode pipeline_mode = PipelineMode::HDR_RES;
            std::string format_name = "EXR";

            if (ext == ".exr") {
                auto exr_loader = std::make_unique<EXRImageLoader>();
                exr_loader->SetLayer(seq_clip->sequence_exr_layer);
                loader = std::move(exr_loader);
                pipeline_mode = PipelineMode::HDR_RES;
                format_name = "EXR";
            } else if (ext == ".tiff" || ext == ".tif") {
                loader = std::make_unique<TIFFImageLoader>();
                pipeline_mode = PipelineMode::HIGH_RES;
                format_name = "TIFF";
            } else if (ext == ".png") {
                loader = std::make_unique<PNGImageLoader>();
                pipeline_mode = PipelineMode::HIGH_RES;
                format_name = "PNG";
            } else if (ext == ".jpg" || ext == ".jpeg") {
                loader = std::make_unique<JPEGImageLoader>();
                pipeline_mode = PipelineMode::NORMAL;
                format_name = "JPEG";
            } else {
                // Default to PNG loader for unknown formats
                loader = std::make_unique<PNGImageLoader>();
                pipeline_mode = PipelineMode::NORMAL;
                format_name = "Unknown (using PNG)";
            }

            direct_exr_cache_->Initialize(
                std::move(loader),
                sequence_files,
                seq_clip->sequence_exr_layer,
                fps,
                pipeline_mode,
                seq_clip->sequence_start_frame,
                0.0  // initial_position
            );

            Debug::Log("TimelineCache: IMAGE_SEQUENCE mode (" + format_name + ") - using DirectEXRCache with " +
                       std::to_string(sequence_files.size()) + " frames, " +
                       std::to_string(exr_config.threadCount) + " threads, " +
                       "readAhead=" + std::to_string(exr_config.readAheadFrames) +
                       " readBehind=" + std::to_string(exr_config.readBehindFrames));

            // DirectEXRCache handles its own threading - no legacy threads needed

            initialized_ = true;
            return;
        }
    }

    initialized_ = true;
    Debug::Log("TimelineCache: Initialized");
}

void TimelineCache::Shutdown() {
    if (!initialized_) return;

    Debug::Log("TimelineCache: Shutting down...");

    // Clear caches (marks textures for deletion)
    ClearCache();

    // CRITICAL: Actually delete textures now!
    // ClearCache() only adds them to textures_to_delete_, but ProcessPendingUploads()
    // which normally handles deletion won't be called after shutdown.
    {
        std::lock_guard<std::mutex> lock(delete_mutex_);
        if (!textures_to_delete_.empty()) {
            int delete_count = static_cast<int>(textures_to_delete_.size());
            Debug::Log("TimelineCache: [SHUTDOWN] Deleting " + std::to_string(delete_count) + " textures");
#ifdef QCVIEW_USE_VULKAN
            for (int i = 0; i < delete_count; i++) {
                qcview::VulkanTexturePool::Instance().QueueDelete(
                    static_cast<uint64_t>(textures_to_delete_[i]));
            }
            qcview::VulkanTexturePool::Instance().ProcessPendingDeletions();
#else
            glDeleteTextures(static_cast<GLsizei>(delete_count), textures_to_delete_.data());
#endif
            textures_to_delete_.clear();
        }
    }

    // Delete gap texture
    DeleteGapTexture();

    // Cleanup letterbox compositing resources
    CleanupLetterboxResources();

    // Cleanup aggressive scrub held texture
    if (aggressive_held_texture_ != 0) {
#ifdef QCVIEW_USE_VULKAN
        qcview::VulkanTexturePool::Instance().QueueDelete(static_cast<uint64_t>(aggressive_held_texture_));
#else
        glDeleteTextures(1, &aggressive_held_texture_);
#endif
        aggressive_held_texture_ = 0;
        aggressive_held_width_ = 0;
        aggressive_held_height_ = 0;
        aggressive_held_frame_ = -1;
        aggressive_held_source_.clear();
    }
    aggressive_scrub_mode_ = AggressiveScrubMode::INACTIVE;

    // Cleanup shuttle mode textures
    if (shuttle_last_texture_ != 0) {
#ifdef QCVIEW_USE_VULKAN
        qcview::VulkanTexturePool::Instance().QueueDelete(static_cast<uint64_t>(shuttle_last_texture_));
#else
        glDeleteTextures(1, &shuttle_last_texture_);
#endif
        shuttle_last_texture_ = 0;
    }
    shuttle_composited_texture_ = 0;  // Just clear reference, may be letterbox texture
    shuttle_last_width_ = 0;
    shuttle_last_height_ = 0;

    // Cleanup last_good_texture_ if it's not already in frame_cache_
    // This can happen when D3D11 decoder's direct GL texture was used
    // NOTE: Don't delete if it might be a D3D11 decoder texture (owned by decoder)
    // Just clear the reference to prevent stale usage
    last_good_texture_ = 0;
    last_good_width_ = 0;
    last_good_height_ = 0;
    last_good_frame_ = -1;
    last_good_source_path_.clear();

    // Clear scrub decoders
    scrub_decoders_.ClearAll();

#ifdef _WIN32
    // Shutdown single decoder (PLAYLIST mode)
    if (single_decoder_) {
        Debug::Log("TimelineCache: Shutting down single decoder");
        single_decoder_->Shutdown();
        single_decoder_.reset();
    }
    use_single_decoder_mode_ = false;
#endif

    // Shutdown DirectEXRCache (IMAGE_SEQUENCE mode)
    if (direct_exr_cache_) {
        Debug::Log("TimelineCache: Shutting down DirectEXRCache");
        direct_exr_cache_->Shutdown();
        direct_exr_cache_.reset();
    }
    use_direct_exr_cache_ = false;
    direct_exr_cache_source_path_.clear();

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

    // Poll aggressive scrub settling (moved from CacheManagementThread)
    // Called every frame render (~60Hz) which is sufficient for settling detection
    HandleAggressiveScrubSettling();

    //=========================================================================
    // IMAGE_SEQUENCE / DUAL_VIEW with DirectEXRCache: Delegate to DirectEXRCache
    //=========================================================================
    if (use_direct_exr_cache_ && direct_exr_cache_) {
        // For DUAL_VIEW, we need to map timeline_frame to source_frame
        // For IMAGE_SEQUENCE, timeline_frame == source_frame (1:1 mapping)
        int source_frame = timeline_frame;
        if (source_mode_ == TimelineSourceMode::DUAL_VIEW) {
            SourceCoords coords = TimelineToSource(timeline_frame);
            if (coords.valid) {
                source_frame = coords.source_frame;
            }
            // If coords invalid (frame beyond clip), source_frame stays as timeline_frame
            // DirectEXRCache will clamp to valid range
        }

        // Use GetFrameOrLoad which returns true only if the exact frame is cached
        // GetTexture alone can return fallback texture on cache miss, masking the miss
        GLuint texture = 0;
        bool loaded = direct_exr_cache_->GetFrameOrLoad(source_frame, texture, width, height);

        if (loaded && texture != 0) {
            // Exact frame was in cache
            if (got_exact_frame) *got_exact_frame = true;
            last_good_texture_ = texture;
            last_good_width_ = width;
            last_good_height_ = height;
            cache_hits_++;

            // Composite to canvas if needed
            if (canvas_width_ > 0 && canvas_height_ > 0 &&
                (width != canvas_width_ || height != canvas_height_)) {
                texture = CompositeFrameToCanvas(texture, width, height);
                width = canvas_width_;
                height = canvas_height_;
            }
            return texture;
        }

        // Cache miss - frame was not in cache, texture may be a fallback
        if (got_exact_frame) *got_exact_frame = false;
        cache_misses_++;

        // Return fallback texture if available (for display continuity)
        if (texture != 0) {
            // Apply canvas dimensions if set
            if (canvas_width_ > 0 && canvas_height_ > 0) {
                width = canvas_width_;
                height = canvas_height_;
            }
            return texture;
        }

        // No fallback available
        if (last_good_texture_ != 0) {
            width = last_good_width_;
            height = last_good_height_;
            if (canvas_width_ > 0 && canvas_height_ > 0) {
                width = canvas_width_;
                height = canvas_height_;
            }
            return last_good_texture_;
        }
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

#ifdef _WIN32
    //=========================================================================
    // SINGLE-DECODER PATH - For PLAYLIST mode with use_single_decoder enabled
    // Uses one D3D11VideoDecoder instance to eliminate D3D11 contention
    // Accepts ~100-200ms pause at clip boundaries for stability
    //=========================================================================
    if (use_single_decoder_mode_ && single_decoder_) {
        // Provide sequence metadata before switching to image sequence sources
        {
            std::lock_guard<std::mutex> lock(sequence_metadata_mutex_);
            auto it = sequence_metadata_.find(coords.source_path);
            if (it != sequence_metadata_.end() && it->second.valid) {
                const auto& meta = it->second;
                single_decoder_->SetSequenceMetadata(
                    meta.directory, meta.pattern,
                    meta.start_frame, meta.end_frame,
                    config_.fps, meta.exr_layer,
                    coords.clip_start_time  // Timeline offset for cache bar positioning
                );
            }
        }

        // Switch source if needed (handles flush + reinit)
        if (!single_decoder_->SwitchSource(coords.source_path)) {
            // Transition in progress - show last good frame
            if (last_good_texture_ != 0) {
                setOutputDimensions(last_good_width_, last_good_height_);
                if (got_exact_frame) *got_exact_frame = false;
                return maybeComposite(last_good_texture_, last_good_width_, last_good_height_);
            }
            // No last good frame - return gap texture
            if (gap_texture_ != 0) {
                setOutputDimensions(gap_texture_width_, gap_texture_height_);
                if (got_exact_frame) *got_exact_frame = false;
                return gap_texture_;
            }
            return 0;
        }

        // Pass playback stride to single decoder's image cache
        if (single_decoder_->IsImageSequence()) {
            single_decoder_->SetPlaybackStride(playback_stride_);
        }

        // Update playhead in single decoder
        single_decoder_->UpdatePlayhead(coords.source_frame);

        // Get frame from single decoder as GL texture
        GLuint texture = single_decoder_->GetFrameAsGLTexture(coords.source_frame);

        if (texture != 0) {
            int w = single_decoder_->GetWidth();
            int h = single_decoder_->GetHeight();

            // Update last good frame
            last_good_texture_ = texture;
            last_good_width_ = w;
            last_good_height_ = h;
            last_good_frame_ = timeline_frame;
            last_good_source_path_ = coords.source_path;

            setOutputDimensions(w, h);
            if (got_exact_frame) *got_exact_frame = single_decoder_->HasFrame(coords.source_frame);
            cache_hits_++;
            return maybeComposite(texture, w, h);
        }

        // Frame not ready yet - return last good
        if (last_good_texture_ != 0) {
            setOutputDimensions(last_good_width_, last_good_height_);
            if (got_exact_frame) *got_exact_frame = false;
            cache_misses_++;
            return maybeComposite(last_good_texture_, last_good_width_, last_good_height_);
        }

        // No frame available
        if (gap_texture_ != 0) {
            setOutputDimensions(gap_texture_width_, gap_texture_height_);
        }
        if (got_exact_frame) *got_exact_frame = false;
        cache_misses_++;
        return gap_texture_;
    }
#endif

    //=========================================================================
    // AGGRESSIVE SCRUB PATH - For DUAL_VIEW and PLAYLIST responsive scrubbing
    // Bypass CacheWindowEngine entirely - go direct to decoder buffer
    // This avoids SetNeededFrames() calls which would trigger full-window pre-buffering
    //=========================================================================
    if ((source_mode_ == TimelineSourceMode::DUAL_VIEW ||
         source_mode_ == TimelineSourceMode::PLAYLIST) &&
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

        // SCRUB PATH: Use ScrubDecoder with small buffers and keyframe-only seeks for fast response
        {
            // ScrubDecoder path (non-D3D11 clips)
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
#ifdef QCVIEW_USE_VULKAN
                        qcview::VulkanTexturePool::Instance().UpdateTexture(
                            static_cast<uint64_t>(aggressive_held_texture_),
                            pixels->pixels.data(), pixels->pixels.size());
#else
                        glBindTexture(GL_TEXTURE_2D, aggressive_held_texture_);
                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                                        pixels->width, pixels->height,
                                        pixels->gl_format, pixels->gl_type,
                                        pixels->pixels.data());
                        glBindTexture(GL_TEXTURE_2D, 0);
#endif
                    } else {
                        // Need new texture
                        if (aggressive_held_texture_ != 0) {
#ifdef QCVIEW_USE_VULKAN
                            qcview::VulkanTexturePool::Instance().QueueDelete(static_cast<uint64_t>(aggressive_held_texture_));
#else
                            glDeleteTextures(1, &aggressive_held_texture_);
#endif
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
    // VIDEO FAST PATH - For VIDEO_FILE mode and DUAL_VIEW with D3D11 decoder
    // Decoder handles buffering internally, so we bypass cache and pull directly
    // For DUAL_VIEW mode, VIDEO clips use the cache path below
    //=========================================================================
    ClipMediaType media_type = DetectMediaType(coords.source_path);

#ifdef _WIN32
    // DUAL_VIEW with D3D11VideoDecoder - use decoder's direct GL texture path
    if (media_type == ClipMediaType::VIDEO && source_mode_ == TimelineSourceMode::DUAL_VIEW) {
        std::shared_ptr<ClipLoaderInfo> loader_info;
        {
            std::lock_guard<std::mutex> lock(loaders_mutex_);
            auto it = loaders_.find(coords.source_path);
            if (it != loaders_.end()) {
                loader_info = it->second;
            }
        }

        // Create decoder on demand if not found (replaces removed CacheManagementThread)
        if (!loader_info) {
            loader_info = GetOrCreateLoader(coords.source_path);
        }

        if (loader_info && loader_info->d3d11_decoder) {
            // Update playhead to trigger decode-ahead
            loader_info->d3d11_decoder->UpdatePlayhead(coords.source_frame);

            // Get GL texture directly from D3D11 decoder (zero-copy interop)
            GLuint d3d11_texture = loader_info->d3d11_decoder->GetFrameAsGLTexture(coords.source_frame);
            if (d3d11_texture != 0) {
                setOutputDimensions(loader_info->width, loader_info->height);
                if (got_exact_frame) *got_exact_frame = loader_info->d3d11_decoder->HasFrame(coords.source_frame);
                cache_hits_++;
                return maybeComposite(d3d11_texture, loader_info->width, loader_info->height);
            }

            // Frame not ready yet
            cache_misses_++;
            return 0;
        }
        // Fall through to FFmpeg decoder path if no D3D11 decoder
    }

    // PLAYLIST VIDEO: Direct D3D11 decoder path (same as DUAL_VIEW)
    // Uses direct decoder access for reliable seek/scrub - returns 0 if frame not ready
    // rather than using cache path fallbacks which can return stale/wrong frames
    if (media_type == ClipMediaType::VIDEO && source_mode_ == TimelineSourceMode::PLAYLIST) {
        std::shared_ptr<ClipLoaderInfo> loader_info;
        {
            std::lock_guard<std::mutex> lock(loaders_mutex_);
            auto it = loaders_.find(coords.source_path);
            if (it != loaders_.end()) {
                loader_info = it->second;
            }
        }

        // Create decoder on demand if not found (replaces removed CacheManagementThread)
        if (!loader_info) {
            loader_info = GetOrCreateLoader(coords.source_path);
        }

        if (loader_info && loader_info->d3d11_decoder) {
            // Update playhead to trigger decode-ahead
            loader_info->d3d11_decoder->UpdatePlayhead(coords.source_frame);

            // Get GL texture directly from D3D11 decoder (zero-copy interop)
            GLuint d3d11_texture = loader_info->d3d11_decoder->GetFrameAsGLTexture(coords.source_frame);
            if (d3d11_texture != 0) {
                setOutputDimensions(loader_info->width, loader_info->height);
                if (got_exact_frame) *got_exact_frame = loader_info->d3d11_decoder->HasFrame(coords.source_frame);
                cache_hits_++;
                return maybeComposite(d3d11_texture, loader_info->width, loader_info->height);
            }

            // Frame not ready - return 0 (no fallback to avoid showing wrong frame)
            cache_misses_++;
            return 0;
        }
        // Fall through to cache path for non-D3D11 video or non-video clips
    }
#endif

#ifdef QCVIEW_USE_VULKAN
    // Vulkan video path — all VIDEO modes use VulkanVideoDecoder
    if (media_type == ClipMediaType::VIDEO) {
        std::shared_ptr<ClipLoaderInfo> loader_info;
        {
            std::lock_guard<std::mutex> lock(loaders_mutex_);
            auto it = loaders_.find(coords.source_path);
            if (it != loaders_.end()) {
                loader_info = it->second;
            }
        }

        if (!loader_info) {
            loader_info = GetOrCreateLoader(coords.source_path);
        }

        if (loader_info && loader_info->vulkan_decoder) {
            loader_info->vulkan_decoder->UpdatePlayhead(coords.source_frame);

            uint64_t pool_id = loader_info->vulkan_decoder->GetFrameAsPoolTexture(coords.source_frame);
            if (pool_id != 0) {
                setOutputDimensions(loader_info->width, loader_info->height);
                if (got_exact_frame) *got_exact_frame = loader_info->vulkan_decoder->HasFrame(coords.source_frame);
                cache_hits_++;
                return maybeComposite(static_cast<GLuint>(pool_id), loader_info->width, loader_info->height);
            }

            cache_misses_++;
            return 0;
        }
    }
#endif

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

#ifdef _WIN32
            // D3D11 direct GL texture path (zero-copy) - scrubbing mode
            GLuint d3d11_texture = loader_info->GetGLTexture(coords.source_frame, true);  // is_scrubbing=true
            if (d3d11_texture != 0) {
                setOutputDimensions(loader_info->width, loader_info->height);
                if (got_exact_frame) *got_exact_frame = true;
                cache_hits_++;
                return maybeComposite(d3d11_texture, loader_info->width, loader_info->height);
            }
#endif

            // Standard PixelData path (for non-D3D11 decoders)
            auto pixels = loader_info->video_decoder->GetFrame(coords.source_frame);
            bool is_exact = (pixels != nullptr);

            // Only fall back to closest frame during active shuttle/scrub mode
            // This prevents flashing wrong frames on click-seeks while keeping smooth scrubbing
            if (!pixels && shuttle_active_) {
                pixels = loader_info->video_decoder->GetClosestFrame(coords.source_frame, nullptr);
            }

            if (pixels) {
                // Upload to reusable texture (create if needed, resize if needed)
#ifdef QCVIEW_USE_VULKAN
                VkFormat vk_format = VK_FORMAT_R8G8B8A8_UNORM;
                switch (pixels->pixel_format) {
                    case PixelFormat::RGBA8:  vk_format = VK_FORMAT_R8G8B8A8_UNORM; break;
                    case PixelFormat::RGBA16: vk_format = VK_FORMAT_R16G16B16A16_UNORM; break;
                    case PixelFormat::RGBA16F: vk_format = VK_FORMAT_R16G16B16A16_SFLOAT; break;
                }
                auto& pool = qcview::VulkanTexturePool::Instance();
                // Always recreate for simplicity (pool handles cleanup)
                if (loader_info->video_texture != 0) {
                    pool.QueueDelete(static_cast<uint64_t>(loader_info->video_texture));
                }
                uint64_t pool_id = pool.CreateTextureFromPixels(
                    pixels->width, pixels->height, vk_format,
                    pixels->pixels.data(), pixels->pixels.size());
                loader_info->video_texture = static_cast<GLuint>(pool_id);
                loader_info->video_texture_width = pixels->width;
                loader_info->video_texture_height = pixels->height;
#else
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
#endif

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

    // - DUAL_VIEW/PLAYLIST scrubbing: AggressiveScrub path (line ~703)
    //
    // If we reach here, no frame was available from any decoder
    if (got_exact_frame) *got_exact_frame = false;
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

    //=========================================================================
    // IMAGE_SEQUENCE / DUAL_VIEW with DirectEXRCache: Delegate to DirectEXRCache
    //=========================================================================
    if (use_direct_exr_cache_ && direct_exr_cache_) {
        // For DUAL_VIEW, map timeline_frame to source_frame
        int source_frame = timeline_frame;
        if (source_mode_ == TimelineSourceMode::DUAL_VIEW) {
            SourceCoords coords = TimelineToSource(timeline_frame);
            if (coords.valid) {
                source_frame = coords.source_frame;
            }
        }

        double timestamp = static_cast<double>(source_frame) / fps_;
        direct_exr_cache_->UpdateCurrentPosition(timestamp);
        direct_exr_cache_->UpdatePlaybackState(is_playing);
        previousFrame_ = timeline_frame;
        current_frame_ = timeline_frame;
        is_playing_ = is_playing;
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
            // NOTE: Skip for D3D11 buffers - they handle seeking via GetDecodedFrame()
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
#ifdef _WIN32
        // For D3D11 modes, frame_cache_ is empty - D3D11 decoders handle frames directly
        if (source_mode_ == TimelineSourceMode::DUAL_VIEW) {
            size_t decoder_count = 0;
            {
                std::lock_guard<std::mutex> lock(loaders_mutex_);
                decoder_count = loaders_.size();
            }
            Debug::Log("TimelineCache: [PLAY STATE CHANGE] is_playing=" + std::to_string(is_playing) +
                       " frame=" + std::to_string(timeline_frame) +
                       " [D3D11 direct - " + std::to_string(decoder_count) + " decoder(s)]");
        } else
#endif
        Debug::Log("TimelineCache: [PLAY STATE CHANGE] is_playing=" + std::to_string(is_playing) +
                   " frame=" + std::to_string(timeline_frame));
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
}

void TimelineCache::RequestFrame(int timeline_frame) {
    // No-op: frame loading is handled by specialized decoders (D3D11VideoDecoder, DirectEXRCache)
    (void)timeline_frame;
}

// Static counters for texture leak detection
static std::atomic<int> s_textures_created{0};
static std::atomic<int> s_textures_deleted{0};

void TimelineCache::ProcessPendingUploads() {
    if (!initialized_) return;

    //=========================================================================
    // IMAGE_SEQUENCE mode: Use DirectEXRCache's texture processing
    //=========================================================================
    if (use_direct_exr_cache_ && direct_exr_cache_) {
        direct_exr_cache_->ProcessReadyTextures();
        return;
    }

#ifdef _WIN32
    // Process D3D11 interop GL texture deletions queued from background threads
    // This MUST be called from the main/GL thread
    D3D11VideoInterop::ProcessPendingGLDeletions();
#endif

    // Delete textures marked for deletion (always do this, even during shuttle)
    {
        std::lock_guard<std::mutex> lock(delete_mutex_);
        if (!textures_to_delete_.empty()) {
            int total_queued = static_cast<int>(textures_to_delete_.size());

            // In linear mode (DUAL_VIEW), throttle deletions to avoid
            // GPU stalls when many textures are evicted at once (e.g., entire clip)
            bool is_linear = cache_engine_.IsLinearMode();
            int delete_count = total_queued;
            if (is_linear && total_queued > 8) {
                delete_count = 8;  // Limit per frame in linear mode
            }

            /*Debug::Log("TimelineCache: [GL DELETE] Deleting " + std::to_string(delete_count) +
                       " of " + std::to_string(total_queued) + " queued textures");*/

#ifdef QCVIEW_USE_VULKAN
            // Vulkan path: Queue deletions via VulkanTexturePool
            for (int i = 0; i < delete_count; i++) {
                qcview::VulkanTexturePool::Instance().QueueDelete(
                    static_cast<uint64_t>(textures_to_delete_[i]));
            }
            qcview::VulkanTexturePool::Instance().ProcessPendingDeletions();
#else
            glDeleteTextures(static_cast<GLsizei>(delete_count),
                             textures_to_delete_.data());
#endif
            s_textures_deleted += delete_count;

            // Remove deleted entries
            if (delete_count == total_queued) {
                textures_to_delete_.clear();
            } else {
                textures_to_delete_.erase(textures_to_delete_.begin(),
                                          textures_to_delete_.begin() + delete_count);
            }
        }
    }

}

//=============================================================================
// Cache Management
//=============================================================================

void TimelineCache::ClearCache() {
    // Reset last_good_texture_ to prevent dangling reference
    last_good_texture_ = 0;
    last_good_width_ = 0;
    last_good_height_ = 0;
    last_good_frame_ = -1;
    last_good_source_path_.clear();

    // Mark segments dirty for cache visualization
    segments_dirty_ = true;

    cache_hits_ = 0;
    cache_misses_ = 0;

    Debug::Log("TimelineCache: Cache cleared");
}

void TimelineCache::ClearRequests() {
    Debug::Log("TimelineCache: Requests cleared");
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

    // CRITICAL: Remove loaders for sources that are no longer in the timeline
    // This prevents stale decoders from being accessed and ensures cleanup happens
    // on the main thread (not the background CacheManagementThread which would crash
    // when trying to delete GL resources without a GL context)
    {
        // Build set of all active source paths from the flattener
        std::set<std::string> active_sources;
        for (int frame = 0; frame < total_timeline_frames_; frame += 24) {  // Sample every second
            SourceCoords sample = TimelineToSource(frame);
            if (sample.valid && !sample.source_path.empty()) {
                active_sources.insert(sample.source_path);
            }
        }
        // Also check current frame explicitly
        if (coords.valid && !coords.source_path.empty()) {
            active_sources.insert(coords.source_path);
        }

        // Remove loaders not in active sources
        std::vector<std::string> loaders_to_remove;
        {
            std::lock_guard<std::mutex> lock(loaders_mutex_);
            for (const auto& [path, loader] : loaders_) {
                if (active_sources.find(path) == active_sources.end()) {
                    loaders_to_remove.push_back(path);
                }
            }
            for (const auto& path : loaders_to_remove) {
                loaders_.erase(path);
            }
        }

        if (!loaders_to_remove.empty()) {
            Debug::Log("TimelineCache: Removed " + std::to_string(loaders_to_remove.size()) +
                       " stale loader(s) on edit (sources no longer in timeline)");
        }
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

    }

    // For DUAL_VIEW/PLAYLIST: Check if content type changed and manage DirectEXRCache accordingly
    if (source_mode_ == TimelineSourceMode::DUAL_VIEW ||
        source_mode_ == TimelineSourceMode::PLAYLIST) {
        // Check if we need to DISABLE or RECREATE DirectEXRCache
        if (use_direct_exr_cache_) {
            SourceCoords coords_check = TimelineToSource(0);
            bool still_sequence = false;
            bool source_changed = false;
            if (coords_check.valid) {
                std::lock_guard<std::mutex> lock(sequence_metadata_mutex_);
                still_sequence = (sequence_metadata_.find(coords_check.source_path) != sequence_metadata_.end());
                // Check if the source path changed (different sequence loaded)
                if (still_sequence && coords_check.source_path != direct_exr_cache_source_path_) {
                    source_changed = true;
                }
            }
            if (!still_sequence) {
                // Content changed from image sequence to video - shutdown DirectEXRCache
                Debug::Log("TimelineCache: Content changed to video, disabling DirectEXRCache");
                if (direct_exr_cache_) {
                    direct_exr_cache_->Shutdown();
                    direct_exr_cache_.reset();
                }
                use_direct_exr_cache_ = false;
                direct_exr_cache_source_path_.clear();
            } else if (source_changed) {
                // Content changed from one sequence to another - recreate DirectEXRCache
                Debug::Log("TimelineCache: Sequence changed from " + direct_exr_cache_source_path_ +
                           " to " + coords_check.source_path + ", recreating DirectEXRCache");
                if (direct_exr_cache_) {
                    direct_exr_cache_->Shutdown();
                    direct_exr_cache_.reset();
                }
                use_direct_exr_cache_ = false;
                direct_exr_cache_source_path_.clear();
            }
        }

        // Then try to ENABLE DirectEXRCache if we have image sequence content
        EnableDirectEXRCacheIfSequence();
    }

    Debug::Log("TimelineCache::NotifyTracksEdited: END");
}

void TimelineCache::EnableDirectEXRCacheIfSequence() {
    // Only for DUAL_VIEW/PLAYLIST mode, and only if not already using DirectEXRCache
    if (source_mode_ != TimelineSourceMode::DUAL_VIEW &&
        source_mode_ != TimelineSourceMode::PLAYLIST) return;
    if (use_direct_exr_cache_) return;

    // First, check what content THIS cache is responsible for via TimelineToSource
    // This uses our flattener which only sees our track (left or right)
    SourceCoords coords = TimelineToSource(0);
    if (!coords.valid) {
        Debug::Log("TimelineCache: TimelineToSource(0) invalid, not enabling DirectEXRCache");
        return;
    }

    Debug::Log("TimelineCache: TimelineToSource(0) =" + coords.source_path);

    // Now find sequence metadata that matches OUR content
    // Both caches register ALL metadata, but we only want the one for OUR track
    std::string source_path;
    SequenceMetadata seq_meta;
    {
        std::lock_guard<std::mutex> lock(sequence_metadata_mutex_);
        if (sequence_metadata_.empty()) {
            Debug::Log("TimelineCache: sequence_metadata_ is empty, not enabling DirectEXRCache");
            return;
        }

        Debug::Log("TimelineCache: sequence_metadata_ has" +
                   std::to_string(sequence_metadata_.size()) + " entries");

        // Look for metadata matching our track's content
        auto it = sequence_metadata_.find(coords.source_path);
        if (it == sequence_metadata_.end()) {
            // Our content is not an image sequence (might be video)
            Debug::Log("TimelineCache: Content" + coords.source_path +
                       " is not a registered sequence, not enabling DirectEXRCache");
            // List registered sequences for debugging
            for (const auto& [path, meta] : sequence_metadata_) {
                Debug::Log("  Registered: " + path);
            }
            return;
        }
        source_path = it->first;
        seq_meta = it->second;
    }

    Debug::Log("TimelineCache: Enabling DirectEXRCache (sequence:" + source_path + ")");

    // Build file list from sequence metadata
    std::vector<std::string> sequence_files;
    fs::path dir(seq_meta.directory);
    for (int frame = seq_meta.start_frame; frame <= seq_meta.end_frame; ++frame) {
        char frame_name[1024];
        snprintf(frame_name, sizeof(frame_name), seq_meta.pattern.c_str(), frame);
        sequence_files.push_back((dir / frame_name).string());
    }

    if (sequence_files.empty()) {
        Debug::Log("TimelineCache: No sequence files found, not enabling DirectEXRCache");
        return;
    }

    // Store FPS for position conversion
    fps_ = config_.fps;

    // Create and initialize DirectEXRCache
    direct_exr_cache_ = std::make_unique<DirectEXRCache>();

    EXRCacheConfig exr_config;
    exr_config.readAheadFrames = g_exr_read_ahead_frames;
    exr_config.readBehindFrames = g_read_behind_frames;
    exr_config.threadCount = static_cast<size_t>(g_exr_thread_count);
    direct_exr_cache_->SetConfig(exr_config);

    // Detect format from first file extension and create appropriate loader
    std::string ext = fs::path(sequence_files[0]).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    std::unique_ptr<IImageLoader> loader;
    PipelineMode pipeline_mode = seq_meta.pipeline_mode;
    std::string format_name = "Unknown";

    if (ext == ".exr") {
        auto exr_loader = std::make_unique<EXRImageLoader>();
        exr_loader->SetLayer(seq_meta.exr_layer);
        loader = std::move(exr_loader);
        format_name = "EXR";
    } else if (ext == ".tiff" || ext == ".tif") {
        loader = std::make_unique<TIFFImageLoader>();
        format_name = "TIFF";
    } else if (ext == ".png") {
        loader = std::make_unique<PNGImageLoader>();
        format_name = "PNG";
    } else if (ext == ".jpg" || ext == ".jpeg") {
        loader = std::make_unique<JPEGImageLoader>();
        format_name = "JPEG";
    } else {
        loader = std::make_unique<PNGImageLoader>();
        format_name = "Unknown (using PNG)";
    }

    direct_exr_cache_->Initialize(
        std::move(loader),
        sequence_files,
        seq_meta.exr_layer,
        config_.fps,
        pipeline_mode,
        seq_meta.start_frame,
        0.0  // initial position
    );

    use_direct_exr_cache_ = true;
    direct_exr_cache_source_path_ = source_path;  // Track which source this cache is for

    Debug::Log("TimelineCache: DirectEXRCache enabled -" + format_name +
               " sequence with " + std::to_string(sequence_files.size()) + " frames" +
               " (source: " + source_path + ")");
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

    // Use the maximum of timeline and image sequence settings to support both content types
    int effective_behind = std::max(config_.readBehindFrames, g_read_behind_frames);
    int effective_ahead = std::max(config_.readAheadFrames, g_exr_read_ahead_frames);
    Debug::Log("TimelineCache::SetConfig: pipeline_mode=" +
               std::string(PipelineModeToString(video_pipeline_mode_)) +
               " window: behind=" + std::to_string(effective_behind) +
               " ahead=" + std::to_string(effective_ahead) +
               " (timeline: " + std::to_string(config_.readBehindFrames) + "/" + std::to_string(config_.readAheadFrames) +
               ", image: " + std::to_string(g_read_behind_frames) + "/" + std::to_string(g_exr_read_ahead_frames) + ")");

    // Update the cache window engine with new window size
    cache_engine_.SetWindow(effective_behind, effective_ahead);

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
    // Use the maximum of timeline and image sequence settings to support both content types
    int effective_behind = std::max(config_.readBehindFrames, g_read_behind_frames);
    int effective_ahead = std::max(config_.readAheadFrames, g_exr_read_ahead_frames);
    cache_engine_.SetTotalFrames(total_timeline_frames_);
    cache_engine_.SetWindow(effective_behind, effective_ahead);

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
            if (!loader) continue;

            // Update standalone video decoder
            if (loader->video_decoder) {
                loader->video_decoder->SetPipelineMode(mode);
            }

#ifdef _WIN32
            // Update direct D3D11 decoder
            if (loader->d3d11_decoder) {
                loader->d3d11_decoder->SetPipelineMode(mode);
            }
#endif
#ifdef QCVIEW_USE_VULKAN
            if (loader->vulkan_decoder) {
                loader->vulkan_decoder->SetPipelineMode(mode);
            }
#endif
            // Note: sequence decoders don't have SetPipelineMode - their bit depth
            // is determined by the image format (EXR=float, TIFF/PNG=8/16-bit)
        }
    }

    // Clear cache since bit depth changed
    ClearCache();

    Debug::Log("TimelineCache: Cleared cache after pipeline mode change");
}

void TimelineCache::SetPlaybackStride(int stride) {
    playback_stride_ = stride;
    if (use_direct_exr_cache_ && direct_exr_cache_) {
        direct_exr_cache_->SetPlaybackStride(stride);
    }
}

int TimelineCache::GetPlaybackStride() const {
    return playback_stride_;
}

void TimelineCache::ResetPlaybackSpeed() {
    // IMAGE_SEQUENCE mode: reset DirectEXRCache overrun state
    if (use_direct_exr_cache_ && direct_exr_cache_) {
        direct_exr_cache_->ResetPlaybackSpeed();
    }
}

void TimelineCache::UpdateDirectEXRCacheConfig(int read_ahead_frames, int read_behind_frames, int thread_count) {
    // Only applies to IMAGE_SEQUENCE mode
    if (!use_direct_exr_cache_ || !direct_exr_cache_) {
        return;
    }

    EXRCacheConfig config = direct_exr_cache_->GetConfig();  // Preserve existing fields (playbackStride, etc.)
    config.readAheadFrames = read_ahead_frames;
    config.readBehindFrames = read_behind_frames;
    config.threadCount = static_cast<size_t>(thread_count);

    direct_exr_cache_->SetConfig(config);

    Debug::Log("TimelineCache: Updated DirectEXRCache config - readAhead=" +
               std::to_string(read_ahead_frames) + " readBehind=" +
               std::to_string(read_behind_frames) + " threads=" +
               std::to_string(thread_count));
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
    //
    // SKIP in single-decoder mode: single_decoder_ manages its own source
    // switching on demand. Pre-spawning decoders would defeat the purpose.
    //=========================================================================

    // Safety check: ensure cache is initialized before accessing flattener
    if (!initialized_ || !flattener_) {
        return;
    }

    // Forward loop range to DirectEXRCache if enabled
    if (use_direct_exr_cache_ && direct_exr_cache_) {
        // Convert timeline frames to source frames for DUAL_VIEW
        int src_start = start_frame;
        int src_end = end_frame;
        if (source_mode_ == TimelineSourceMode::DUAL_VIEW) {
            SourceCoords start_coords = TimelineToSource(start_frame);
            SourceCoords end_coords = TimelineToSource(end_frame);
            if (start_coords.valid) src_start = start_coords.source_frame;
            if (end_coords.valid) src_end = end_coords.source_frame;
        }
        direct_exr_cache_->SetLoopRange(src_start, src_end);
        Debug::Log("DirectEXRCache: Loop range set to [" + std::to_string(src_start) + ", " + std::to_string(src_end) + "]");
        return;  // DirectEXRCache handles its own loop prefetching
    }

    // Skip loop prefetching in single-decoder mode
    if (use_single_decoder_mode_) {
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

    // Clear loop range on DirectEXRCache if enabled
    if (use_direct_exr_cache_ && direct_exr_cache_) {
        direct_exr_cache_->ClearLoopRange();
    }

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
    //=========================================================================
    // IMAGE_SEQUENCE mode: Generate priority window from DirectEXRCache config
    //=========================================================================
    if (use_direct_exr_cache_ && direct_exr_cache_) {
        std::vector<int> window;
        auto config = direct_exr_cache_->GetConfig();
        int current = previousFrame_;  // Last known playhead position
        int total = total_timeline_frames_;

        // Priority order: current frame first, then read-ahead, then read-behind
        // This matches how buffer-wait checks for sequential fill
        if (current >= 0 && current < total) {
            window.push_back(current);
        }

        // Add read-ahead frames
        for (int i = 1; i <= config.readAheadFrames && (int)window.size() < config.readAheadFrames + config.readBehindFrames + 1; ++i) {
            int frame = current + i;
            if (frame < total) {
                window.push_back(frame);
            }
        }

        // Add read-behind frames
        for (int i = 1; i <= config.readBehindFrames && (int)window.size() < config.readAheadFrames + config.readBehindFrames + 1; ++i) {
            int frame = current - i;
            if (frame >= 0) {
                window.push_back(frame);
            }
        }

        return window;
    }

    return cache_engine_.GetFrameWindow();
}

TimelineCacheStats TimelineCache::GetStats() const {
    TimelineCacheStats stats;

#ifdef _WIN32
    //=========================================================================
    // PLAYLIST mode with single decoder: Get stats from PlaylistSingleDecoder
    //=========================================================================
    if (use_single_decoder_mode_ && single_decoder_ && single_decoder_->IsImageSequence()) {
        stats.timeline_duration = timeline_duration_;
        stats.total_timeline_frames = total_timeline_frames_;
        stats.cached_frames = single_decoder_->GetCachedFrameCount();
        stats.pending_requests = 0;  // PlaylistSingleDecoder doesn't expose pending requests
        stats.cache_bytes = 0;  // Not tracked
        stats.total_clips = 1;
        stats.linked_clips = 1;
        stats.active_loaders = 1;
        stats.cache_hits = cache_hits_.load();
        stats.cache_misses = cache_misses_.load();
        return stats;
    }
#endif

    //=========================================================================
    // IMAGE_SEQUENCE mode: Get stats from DirectEXRCache
    //=========================================================================
    if (use_direct_exr_cache_ && direct_exr_cache_) {
        auto exr_stats = direct_exr_cache_->GetStats();
        stats.timeline_duration = timeline_duration_;
        stats.total_timeline_frames = total_timeline_frames_;
        stats.cached_frames = exr_stats.cachedFrames;
        stats.pending_requests = exr_stats.pendingRequests;
        stats.cache_bytes = exr_stats.cacheBytes;
        stats.total_clips = 1;  // Single sequence
        stats.linked_clips = 1;
        stats.active_loaders = 1;
        stats.cache_hits = exr_stats.cache_hits;
        stats.cache_misses = exr_stats.cache_misses;
        return stats;
    }

    // Timeline duration and total frames (for progress display)
    stats.timeline_duration = timeline_duration_;
    stats.total_timeline_frames = total_timeline_frames_;

    stats.cached_frames = 0;
    stats.cache_bytes = 0;
    stats.pending_requests = 0;
    stats.pending_uploads = 0;

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
    // For DUAL_VIEW mode, show the cache progress bar
    // Only skip cache bar for VIDEO_FILE mode where D3D11VideoDecoder handles buffering internally
    if (source_mode_ == TimelineSourceMode::DUAL_VIEW) {
        return false;  // Show cache bar
    }

    // For VIDEO_FILE mode, check if all clips are video (D3D11VideoDecoder handles buffering)
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
    return true;  // All clips are video in VIDEO_FILE mode - D3D11VideoDecoder handles caching
}

bool TimelineCache::HasImageContent() const {
    // Buffer wait and cache bar only apply to image/EXR content which uses the ring buffer
    // Returns true if source mode is IMAGE_SEQUENCE, or if any loaded clip is IMAGE_SEQUENCE/EXR_SEQUENCE

    // Check source mode first - IMAGE_SEQUENCE mode always has image content
    if (source_mode_ == TimelineSourceMode::IMAGE_SEQUENCE) {
        return true;
    }

    // DUAL_VIEW with DirectEXRCache enabled means this cache has image content
    if (use_direct_exr_cache_ && direct_exr_cache_) {
        return true;
    }

#ifdef _WIN32
    // PLAYLIST mode with single decoder on image sequence
    if (use_single_decoder_mode_ && single_decoder_ && single_decoder_->IsImageSequence()) {
        return true;
    }
#endif

    // Check loaders for mixed content (e.g., playlist with images)
    std::lock_guard<std::mutex> lock(loaders_mutex_);
    for (const auto& [path, info] : loaders_) {
        if (!info) continue;
        if (info->media_type == ClipMediaType::IMAGE_SEQUENCE ||
            info->media_type == ClipMediaType::EXR_SEQUENCE) {
            return true;  // Has image content
        }
    }
    return false;  // All video or empty - no image content
}

bool TimelineCache::IsFrameImageContent(int timeline_frame) const {
    // Check if the specific frame maps to an image/EXR clip
    // Used for clip-aware buffer wait (only wait within image clips, not video)

    if (!initialized_) return false;
    if (timeline_frame < 0 || timeline_frame >= total_timeline_frames_) return false;

    // In IMAGE_SEQUENCE mode, all content is image
    if (source_mode_ == TimelineSourceMode::IMAGE_SEQUENCE) {
        return true;
    }

    // Map timeline frame to source coordinates
    SourceCoords coords = TimelineToSource(timeline_frame);
    if (!coords.valid) {
        return false;  // Gap or invalid - no buffer wait needed
    }

    // Check the media type of the source clip
    ClipMediaType media_type = DetectMediaType(coords.source_path);
    return (media_type == ClipMediaType::IMAGE_SEQUENCE ||
            media_type == ClipMediaType::EXR_SEQUENCE);
}

std::vector<TimelineCacheSegment> TimelineCache::GetCacheSegments() const {
#ifdef _WIN32
    //=========================================================================
    // PLAYLIST mode with single decoder: Get segments from PlaylistSingleDecoder
    //=========================================================================
    if (use_single_decoder_mode_ && single_decoder_ && single_decoder_->IsImageSequence()) {
        auto exr_segments = single_decoder_->GetCacheSegments();
        std::vector<TimelineCacheSegment> segments;
        segments.reserve(exr_segments.size());
        for (const auto& seg : exr_segments) {
            TimelineCacheSegment ts;
            ts.start_time = seg.start_time;
            ts.end_time = seg.end_time;
            ts.density = static_cast<float>(seg.density);
            ts.type = TimelineCacheSegment::TIMELINE_CACHE;
            segments.push_back(ts);
        }
        return segments;
    }
#endif

    //=========================================================================
    // IMAGE_SEQUENCE mode: Get segments from DirectEXRCache
    //=========================================================================
    if (use_direct_exr_cache_ && direct_exr_cache_) {
        auto exr_segments = direct_exr_cache_->GetCacheSegments();
        std::vector<TimelineCacheSegment> segments;
        segments.reserve(exr_segments.size());
        for (const auto& seg : exr_segments) {
            TimelineCacheSegment ts;
            ts.start_time = seg.start_time;
            ts.end_time = seg.end_time;
            ts.density = static_cast<float>(seg.density);
            ts.type = TimelineCacheSegment::TIMELINE_CACHE;
            segments.push_back(ts);
        }
        return segments;
    }

    // Return empty for unsupported modes
    std::vector<TimelineCacheSegment> segments;
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
                    // Check video_decoder buffer
                    if (it->second->video_decoder) {
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

    // Skip target window for video clips - only show for image/EXR content
    // Video clips use D3D11 decoders which handle their own buffering
    if (source_mode_ == TimelineSourceMode::PLAYLIST ||
        source_mode_ == TimelineSourceMode::DUAL_VIEW) {
        // Check if current frame is a video clip
        SourceCoords coords = TimelineToSource(current_frame);
        if (coords.valid) {
            ClipMediaType media_type = DetectMediaType(coords.source_path);
            if (media_type == ClipMediaType::VIDEO) {
                return segments;  // No target window for video clips
            }
        }
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

    // First check if loader exists and has pipeline mode set
    {
        std::lock_guard<std::mutex> lock(loaders_mutex_);
        auto it = loaders_.find(coords.source_path);
        if (it != loaders_.end() && it->second) {
            return it->second->pipeline_mode;
        }
    }

    // Fallback: check sequence metadata (registered before loader is created)
    {
        std::lock_guard<std::mutex> lock(sequence_metadata_mutex_);
        auto it = sequence_metadata_.find(coords.source_path);
        if (it != sequence_metadata_.end() && it->second.valid) {
            return it->second.pipeline_mode;
        }
    }

    return PipelineMode::NORMAL;
}

bool TimelineCache::HasFrameReady(int timeline_frame) const {
    if (!initialized_) return false;

    //=========================================================================
    // IMAGE_SEQUENCE mode: Delegate to DirectEXRCache
    //=========================================================================
    if (use_direct_exr_cache_ && direct_exr_cache_) {
        return direct_exr_cache_->IsFrameCached(timeline_frame);
    }

    // Map timeline frame to source coordinates
    SourceCoords coords = TimelineToSource(timeline_frame);
    if (!coords.valid) {
        // Gap or unlinked - gaps are always "ready" (we show black)
        return true;
    }

#ifdef _WIN32
    //=========================================================================
    // PLAYLIST mode with single decoder: Check PlaylistSingleDecoder
    //=========================================================================
    if (use_single_decoder_mode_ && single_decoder_) {
        // During warmup grace period, return true to avoid triggering buffer pause
        // This gives the cache time to start loading before throttle kicks in
        if (single_decoder_->IsImageSequence() && single_decoder_->IsInWarmupPeriod()) {
            return true;
        }
        // For image sequences, delegate to single_decoder which handles frame offset
        if (single_decoder_->IsImageSequence()) {
            return single_decoder_->HasFrame(coords.source_frame);
        }
        // For video, check the decoder's buffer
        if (single_decoder_->GetDecoder()) {
            return single_decoder_->HasFrame(coords.source_frame);
        }
    }
#endif

    // Check if frame is in the decoder buffer (fast - no decode, just buffer check)
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
        // NOTE: Don't add +0.5 here - timestamp already has +0.5 for center-of-frame
        // Adding another +0.5 would cause off-by-one (frame 0 → source 1)
        source_frame = static_cast<int>(clip_offset * fps_for_frame_calc);
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

void TimelineCache::ClearSequenceMetadata() {
    std::lock_guard<std::mutex> lock(sequence_metadata_mutex_);
    if (!sequence_metadata_.empty()) {
        Debug::Log("TimelineCache: Cleared " + std::to_string(sequence_metadata_.size()) + " sequence metadata entries");
        sequence_metadata_.clear();
    }
}

std::shared_ptr<ClipLoaderInfo> TimelineCache::GetOrCreateLoader(const std::string& source_path) {
    // FAST PATH: Check if loader already exists or being created
    {
        std::unique_lock<std::mutex> lock(loaders_mutex_);

        // Wait if another thread is creating this loader
        while (loaders_creating_.count(source_path) > 0) {
            // Another thread is creating this loader - wait for it
            loaders_cv_.wait_for(lock, std::chrono::milliseconds(100));

            // Check if loader is now available (creation finished)
            auto it = loaders_.find(source_path);
            if (it != loaders_.end()) {
                it->second->last_used_time = std::chrono::steady_clock::now();
                return it->second;
            }
            // If still creating, loop and wait again
        }

        // Check if loader exists
        auto it = loaders_.find(source_path);
        if (it != loaders_.end()) {
            it->second->last_used_time = std::chrono::steady_clock::now();
            return it->second;
        }

        // Mark that we're creating this loader (prevents other threads from duplicating work)
        loaders_creating_.insert(source_path);
    }
    // Lock released - allows other threads to proceed during slow initialization

    // RAII guard to ensure we remove from loaders_creating_ on all exit paths
    struct CreationGuard {
        TimelineCache* cache;
        const std::string& path;
        ~CreationGuard() {
            std::lock_guard<std::mutex> lock(cache->loaders_mutex_);
            cache->loaders_creating_.erase(path);
            cache->loaders_cv_.notify_all();
        }
    } guard{this, source_path};

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
            // VIDEO clips use D3D11VideoDecoder for hardware-accelerated decoding
            Debug::Log("TimelineCache::GetOrCreateLoader VIDEO clip, source_mode=" +
                       std::to_string(static_cast<int>(source_mode_)) +
                       " (0=IMAGE_SEQ, 1=VIDEO_FILE, 2=AUDIO_FILE, 3=DUAL_VIEW, 4=PLAYLIST) path=" + source_path);

#ifdef _WIN32
            // Use D3D11VideoDecoder for hardware-accelerated decoding
            Debug::Log("TimelineCache: Attempting D3D11VideoDecoder for " + source_path);

            auto& device_manager = D3D11DeviceManager::Instance();
            bool is_init = device_manager.IsInitialized();
            ID3D11Device* device = device_manager.GetDevice();

            Debug::Log("TimelineCache: D3D11DeviceManager - initialized=" + std::string(is_init ? "yes" : "no") +
                       " device=" + (device ? "valid" : "nullptr"));

            if (device) {
                auto decoder = std::make_unique<D3D11VideoDecoder>();
                decoder->SetVideoPath(source_path);
                decoder->SetPipelineMode(video_pipeline_mode_);

                // Configure buffer sizes from cache config
                StreamingDecoderConfig dec_config;
                dec_config.readAheadFrames = config_.readAheadFrames;
                dec_config.readBehindFrames = config_.readBehindFrames;
                decoder->SetConfig(dec_config);

                Debug::Log("TimelineCache: Calling D3D11VideoDecoder::Initialize()...");
                if (decoder->Initialize()) {
                    info->d3d11_decoder = std::move(decoder);
                    info->pipeline_mode = video_pipeline_mode_;
                    info->width = info->d3d11_decoder->GetWidth();
                    info->height = info->d3d11_decoder->GetHeight();
                    info->fps = info->d3d11_decoder->GetFPS();
                    info->frame_count = info->d3d11_decoder->GetFrameCount();

                    Debug::Log("TimelineCache: D3D11VideoDecoder created for " + source_path +
                               " (" + std::to_string(info->width) + "x" + std::to_string(info->height) +
                               " @ " + std::to_string(info->fps) + " fps)");

                    // Store detected FPS for pending update check
                    if (detected_media_fps_ <= 0 && source_mode_ == TimelineSourceMode::VIDEO_FILE) {
                        detected_media_fps_ = info->fps;
                        if (std::abs(info->fps - config_.fps) > 0.01) {
                            Debug::Log("TimelineCache: FPS mismatch - media=" + std::to_string(info->fps) +
                                       " timeline=" + std::to_string(config_.fps) + " (pending update)");
                        }
                    }
                    break;
                } else {
                    Debug::Log("TimelineCache: D3D11VideoDecoder init failed for " + source_path);
                }
            } else {
                Debug::Log("TimelineCache: D3D11 device not available for " + source_path);
            }

            // D3D11 is the only video path on Windows - fail if not available
            Debug::Log("TimelineCache: Failed to create video decoder for " + source_path +
                       " (D3D11 hardware acceleration required)");
            return nullptr;
#elif defined(QCVIEW_USE_VULKAN)
            // Linux/Vulkan: Use VulkanVideoDecoder
            Debug::Log("TimelineCache: Attempting VulkanVideoDecoder for " + source_path);
            {
                auto decoder = std::make_unique<VulkanVideoDecoder>();
                decoder->SetVideoPath(source_path);
                decoder->SetPipelineMode(video_pipeline_mode_);
                decoder->SetForceSoftwareDecode(config_.force_software_decode);

                StreamingDecoderConfig dec_config;
                dec_config.readAheadFrames = config_.readAheadFrames;
                dec_config.readBehindFrames = config_.readBehindFrames;
                decoder->SetConfig(dec_config);

                if (decoder->Initialize()) {
                    info->vulkan_decoder = std::move(decoder);
                    info->pipeline_mode = video_pipeline_mode_;
                    info->width = info->vulkan_decoder->GetWidth();
                    info->height = info->vulkan_decoder->GetHeight();
                    info->fps = info->vulkan_decoder->GetFPS();
                    info->frame_count = info->vulkan_decoder->GetFrameCount();

                    Debug::Log("TimelineCache: VulkanVideoDecoder created for " + source_path +
                               " (" + std::to_string(info->width) + "x" + std::to_string(info->height) +
                               " @ " + std::to_string(info->fps) + " fps)");

                    if (detected_media_fps_ <= 0 && source_mode_ == TimelineSourceMode::VIDEO_FILE) {
                        detected_media_fps_ = info->fps;
                        if (std::abs(info->fps - config_.fps) > 0.01) {
                            Debug::Log("TimelineCache: FPS mismatch - media=" + std::to_string(info->fps) +
                                       " timeline=" + std::to_string(config_.fps) + " (pending update)");
                        }
                    }
                    break;
                } else {
                    Debug::Log("TimelineCache: VulkanVideoDecoder init failed for " + source_path);
                }
            }
            Debug::Log("TimelineCache: Failed to create video decoder for " + source_path);
            return nullptr;
#else
            // No video decoder available on this platform
            Debug::Log("TimelineCache: Video decoding not supported on this platform");
            return nullptr;
#endif
        }

        case ClipMediaType::EXR_SEQUENCE: {
            // In DUAL_VIEW mode, skip ImageSequenceDecoder - DirectEXRCache handles sequences
            if (source_mode_ == TimelineSourceMode::DUAL_VIEW) {
                return nullptr;  // DirectEXRCache handles this
            }

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
                    // Configure decoder to use Image Sequence settings (not timeline cache settings)
                    StreamingDecoderConfig dec_config;
                    dec_config.readAheadFrames = g_exr_read_ahead_frames;
                    dec_config.readBehindFrames = g_read_behind_frames;
                    decoder->SetConfig(dec_config);

                    info->sequence_decoder = std::move(decoder);
                    info->pipeline_mode = seq_meta.pipeline_mode;
                    info->width = info->sequence_decoder->GetWidth();
                    info->height = info->sequence_decoder->GetHeight();
                    info->fps = info->sequence_decoder->GetFPS();
                    info->frame_count = info->sequence_decoder->GetFrameCount();
                    Debug::Log("TimelineCache: ImageSequenceDecoder created for EXR sequence " + source_path +
                               " (g_exr_read_ahead_frames=" + std::to_string(g_exr_read_ahead_frames) +
                               ", dec_config.readAhead=" + std::to_string(dec_config.readAheadFrames) + ")");
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
            // In DUAL_VIEW mode, skip ImageSequenceDecoder - DirectEXRCache handles sequences
            if (source_mode_ == TimelineSourceMode::DUAL_VIEW) {
                return nullptr;  // DirectEXRCache handles this
            }

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
                    // Configure decoder to use Image Sequence settings (not timeline cache settings)
                    StreamingDecoderConfig dec_config;
                    dec_config.readAheadFrames = g_exr_read_ahead_frames;
                    dec_config.readBehindFrames = g_read_behind_frames;
                    decoder->SetConfig(dec_config);

                    info->sequence_decoder = std::move(decoder);
                    info->pipeline_mode = seq_meta.pipeline_mode;
                    info->width = info->sequence_decoder->GetWidth();
                    info->height = info->sequence_decoder->GetHeight();
                    info->fps = info->sequence_decoder->GetFPS();
                    info->frame_count = info->sequence_decoder->GetFrameCount();
                    Debug::Log("TimelineCache: ImageSequenceDecoder created for image sequence " + source_path +
                               " (g_exr_read_ahead_frames=" + std::to_string(g_exr_read_ahead_frames) +
                               ", dec_config.readAhead=" + std::to_string(dec_config.readAheadFrames) + ")");
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

    // RE-ACQUIRE LOCK to insert
    // Note: With loaders_creating_ tracking, duplicates should not occur,
    // but we keep the safety check just in case.
    {
        std::lock_guard<std::mutex> lock(loaders_mutex_);

        // Safety check: should not happen with loaders_creating_ tracking
        auto it = loaders_.find(source_path);
        if (it != loaders_.end()) {
            // Unexpected: another thread created it despite loaders_creating_ tracking
            return it->second;
        }

        // Insert our loader
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
// GPU Upload
//=============================================================================

GLuint TimelineCache::CreateGLTexture(const std::shared_ptr<PixelData>& pixels) {
    if (!pixels || pixels->pixels.empty()) return 0;

#ifdef QCVIEW_USE_VULKAN
    VkFormat vk_format = VK_FORMAT_R8G8B8A8_UNORM;
    switch (pixels->pixel_format) {
        case PixelFormat::RGBA8:  vk_format = VK_FORMAT_R8G8B8A8_UNORM; break;
        case PixelFormat::RGBA16: vk_format = VK_FORMAT_R16G16B16A16_UNORM; break;
        case PixelFormat::RGBA16F: vk_format = VK_FORMAT_R16G16B16A16_SFLOAT; break;
    }
    uint64_t pool_id = qcview::VulkanTexturePool::Instance().CreateTextureFromPixels(
        pixels->width, pixels->height, vk_format,
        pixels->pixels.data(), pixels->pixels.size());
    s_textures_created++;
    return static_cast<GLuint>(pool_id);
#else
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
#endif
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

    //=========================================================================
    // SINGLE-DECODER PATH - For PLAYLIST mode with use_single_decoder enabled
    //=========================================================================
    if (use_single_decoder_mode_ && single_decoder_) {
        // Provide sequence metadata before switching to image sequence sources
        {
            std::lock_guard<std::mutex> lock(sequence_metadata_mutex_);
            auto it = sequence_metadata_.find(coords.source_path);
            if (it != sequence_metadata_.end() && it->second.valid) {
                const auto& meta = it->second;
                single_decoder_->SetSequenceMetadata(
                    meta.directory, meta.pattern,
                    meta.start_frame, meta.end_frame,
                    config_.fps, meta.exr_layer,
                    coords.clip_start_time  // Timeline offset for cache bar positioning
                );
            }
        }

        // Switch source if needed (handles flush + reinit)
        if (!single_decoder_->SwitchSource(coords.source_path)) {
            // Transition in progress - return nullptr and let caller use fallback
            width = canvas_width_;
            height = canvas_height_;
            return nullptr;
        }

        // Update playhead in single decoder
        single_decoder_->UpdatePlayhead(coords.source_frame);

        // Get frame from single decoder as D3D11 SRV (video only - image sequences use GL texture path)
        ID3D11ShaderResourceView* srv = single_decoder_->GetFrameAsD3D11SRV(coords.source_frame);

        if (srv != nullptr) {
            width = single_decoder_->GetWidth();
            height = single_decoder_->GetHeight();
            if (got_exact_frame) *got_exact_frame = single_decoder_->HasFrame(coords.source_frame);
            return srv;
        }

        // Frame not ready
        width = canvas_width_;
        height = canvas_height_;
        return nullptr;
    }

    // No decoder available for this source
    cache_misses_++;
    return nullptr;
}

D3D11VideoDecoder* TimelineCache::GetD3D11Decoder() {
    // First, determine what source path the flattener expects for the current frame
    // This prevents returning a stale decoder after media swap
    std::string expected_source;
    {
        // Check what source the flattener maps to at current position
        int current = current_frame_.load();
        SourceCoords coords = TimelineToSource(current);
        if (coords.valid) {
            expected_source = coords.source_path;
        }
    }

    std::lock_guard<std::mutex> lock(loaders_mutex_);

    // For VIDEO_FILE mode, find a loader with D3D11 decoder matching expected source
    // If expected_source is empty (no clip at current frame), fall back to first decoder
    for (auto& pair : loaders_) {
        if (pair.second && pair.second->d3d11_decoder) {
            // If we know the expected source, only return decoder for that source
            if (!expected_source.empty()) {
                if (pair.first == expected_source) {
                    return pair.second->d3d11_decoder.get();
                }
            } else {
                // No expected source (maybe gap or empty track) - return first found
                return pair.second->d3d11_decoder.get();
            }
        }
    }

    return nullptr;
}

bool TimelineCache::HasD3D11Decoder() const {
    // Check what source the flattener expects
    std::string expected_source;
    {
        int current = current_frame_.load();
        // Use const_cast for the non-const call in this const method
        SourceCoords coords = const_cast<TimelineCache*>(this)->TimelineToSource(current);
        if (coords.valid) {
            expected_source = coords.source_path;
        }
    }

    std::lock_guard<std::mutex> lock(loaders_mutex_);

    for (const auto& pair : loaders_) {
        if (pair.second && pair.second->d3d11_decoder) {
            // If we know expected source, only count decoder for that source
            if (!expected_source.empty()) {
                if (pair.first == expected_source) {
                    return true;
                }
            } else {
                return true;
            }
        }
    }

    return false;
}
#endif

bool TimelineCache::HasImageSequenceContent() const {
    std::lock_guard<std::mutex> lock(loaders_mutex_);

    for (const auto& pair : loaders_) {
        if (pair.second && pair.second->sequence_decoder) {
            return true;
        }
    }

    // Also check registered sequence metadata (sequences that haven't loaded yet)
    std::lock_guard<std::mutex> seq_lock(sequence_metadata_mutex_);
    if (!sequence_metadata_.empty()) {
        return true;
    }

    return false;
}

//=============================================================================
// SharedMemoryPool Integration
//=============================================================================

void TimelineCache::RegisterWithPool(const TimelineCacheKey& key, size_t bytes) {
    // Key already contains source info - use directly
    auto pool_key = MakeTimelineKey(key.source_path, key.source_frame);

    SharedMemoryPool::Instance().RegisterEntry(
        pool_key,
        bytes,
        [this, key]() { OnEvicted(key); }
    );
}

void TimelineCache::TouchInPool(const TimelineCacheKey& key) {
    if (!config_.use_shared_pool) return;

    // Key already contains source info - use directly
    auto pool_key = MakeTimelineKey(key.source_path, key.source_frame);
    SharedMemoryPool::Instance().TouchEntry(pool_key);
}

void TimelineCache::RemoveFromPool(const TimelineCacheKey& key) {
    if (!config_.use_shared_pool) return;

    // Key already contains source info - use directly
    auto pool_key = MakeTimelineKey(key.source_path, key.source_frame);
    SharedMemoryPool::Instance().RemoveEntry(pool_key);  // Does NOT trigger callback
}

void TimelineCache::OnEvicted(const TimelineCacheKey& key) {
    (void)key;
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

#ifdef QCVIEW_USE_VULKAN
    // Vulkan path: Create gap texture via VulkanTexturePool
    uint64_t pool_id = qcview::VulkanTexturePool::Instance().CreateTextureFromPixels(
        width, height, VK_FORMAT_R8G8B8A8_UNORM,
        black_pixels.data(), black_pixels.size());
    gap_texture_ = static_cast<GLuint>(pool_id);
#else
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
#endif

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
#ifdef QCVIEW_USE_VULKAN
        qcview::VulkanTexturePool::Instance().QueueDelete(static_cast<uint64_t>(gap_texture_));
        qcview::VulkanTexturePool::Instance().ProcessPendingDeletions();
#else
        glDeleteTextures(1, &gap_texture_);
#endif
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

#ifdef QCVIEW_USE_VULKAN
    // Letterbox compositing not needed on Vulkan - frames are passed through directly
    // (compositing will be done via Vulkan compute/graphics pipeline in Phase 2+)
    Debug::Log("TimelineCache::InitializeLetterboxShader: Skipped on Vulkan");
    return;
#else
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
#endif
}

void TimelineCache::CleanupLetterboxResources() {
#ifdef QCVIEW_USE_VULKAN
    // On Vulkan, letterbox resources (shader/FBO/VAO/VBO) are never created
    // Only the output texture needs cleanup if it exists
    if (letterbox_output_texture_ != 0) {
        qcview::VulkanTexturePool::Instance().QueueDelete(static_cast<uint64_t>(letterbox_output_texture_));
        letterbox_output_texture_ = 0;
    }
#else
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
#endif
    letterbox_output_width_ = 0;
    letterbox_output_height_ = 0;
}

GLuint TimelineCache::CompositeFrameToCanvas(GLuint source_texture, int src_w, int src_h) {
    if (source_texture == 0 || canvas_width_ <= 0 || canvas_height_ <= 0) {
        return source_texture;  // Can't composite, return original
    }

#ifdef QCVIEW_USE_VULKAN
    // Vulkan path: Skip GL compositing for Phase 1, return source directly
    (void)src_w; (void)src_h;
    return source_texture;
#endif

    // Initialize shader on first use
    if (letterbox_shader_ == 0) {
        InitializeLetterboxShader();
        if (letterbox_shader_ == 0) {
            Debug::Log("TimelineCache::CompositeFrameToCanvas: Shader init failed, returning original");
            return source_texture;
        }
    }

    // Determine internal format based on pipeline mode
    // NOTE: HDR_RES (float) not available for video - FFmpeg/D3D11 only supports integer formats
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
// Aggressive Scrub Mode Implementation (DUAL_VIEW and PLAYLIST)
//=============================================================================

void TimelineCache::SetAggressiveScrubMode(bool enabled) {
    // For DUAL_VIEW and PLAYLIST - VIDEO_FILE/IMAGE_SEQUENCE use normal shuttle
    if (source_mode_ != TimelineSourceMode::DUAL_VIEW &&
        source_mode_ != TimelineSourceMode::PLAYLIST) {
        return;
    }

    if (enabled) {
        AggressiveScrubMode expected = AggressiveScrubMode::INACTIVE;
        if (aggressive_scrub_mode_.compare_exchange_strong(expected, AggressiveScrubMode::ACTIVE_SCRUBBING)) {
            Debug::Log("TimelineCache: Aggressive scrub mode STARTED");

            // Put all decoders into shuttle mode (unthrottled decode)
            std::lock_guard<std::mutex> lock(loaders_mutex_);
            for (auto& [path, loader] : loaders_) {
                if (loader && loader->video_decoder) {
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

    // Exit shuttle mode on all video decoders
    int current_frame = current_frame_.load();
    SourceCoords coords = TimelineToSource(current_frame);

    {
        std::lock_guard<std::mutex> lock(loaders_mutex_);
        for (auto& [path, loader] : loaders_) {
            if (loader && loader->video_decoder && loader->IsShuttleMode()) {
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
#ifdef QCVIEW_USE_VULKAN
            qcview::VulkanTexturePool::Instance().UpdateTexture(
                static_cast<uint64_t>(shuttle_last_texture_),
                pixels->pixels.data(), pixels->pixels.size());
#else
            glBindTexture(GL_TEXTURE_2D, shuttle_last_texture_);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                            pixels->width, pixels->height,
                            GL_RGBA, GL_UNSIGNED_BYTE,
                            pixels->pixels.data());
            glBindTexture(GL_TEXTURE_2D, 0);
#endif
        } else {
            // Need new texture
            if (shuttle_last_texture_ != 0) {
#ifdef QCVIEW_USE_VULKAN
                qcview::VulkanTexturePool::Instance().QueueDelete(static_cast<uint64_t>(shuttle_last_texture_));
#else
                glDeleteTextures(1, &shuttle_last_texture_);
#endif
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
#ifdef QCVIEW_USE_VULKAN
        qcview::VulkanTexturePool::Instance().QueueDelete(static_cast<uint64_t>(shuttle_last_texture_));
#else
        glDeleteTextures(1, &shuttle_last_texture_);
#endif
        shuttle_last_texture_ = 0;
        shuttle_last_width_ = 0;
        shuttle_last_height_ = 0;
    }

    return snap_frame;
}

} // namespace qcview
