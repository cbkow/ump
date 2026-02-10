#include "playlist_single_decoder.h"

#ifdef _WIN32

#include "../player/d3d11_video_decoder.h"
#include "../player/direct_exr_cache.h"
#include "../player/image_loaders.h"
#include "../utils/debug_utils.h"
#include <filesystem>
#include <algorithm>
#include <chrono>

namespace fs = std::filesystem;

// Global settings from main.cpp (global namespace)
extern int g_exr_read_ahead_frames;
extern int g_read_behind_frames;
extern int g_exr_thread_count;

namespace ump {

PlaylistSingleDecoder::PlaylistSingleDecoder() = default;

PlaylistSingleDecoder::~PlaylistSingleDecoder() {
    Shutdown();
}

//=============================================================================
// Lifecycle
//=============================================================================

bool PlaylistSingleDecoder::Initialize(ID3D11Device* device) {
    if (initialized_) {
        Debug::Log("[PlaylistSingleDecoder] Already initialized");
        return true;
    }

    if (!device) {
        Debug::Log("[PlaylistSingleDecoder] Error: device is null");
        return false;
    }

    device_ = device;
    initialized_ = true;

    Debug::Log("[PlaylistSingleDecoder] Initialized (no decoder loaded yet)");
    return true;
}

void PlaylistSingleDecoder::Shutdown() {
    if (!initialized_) {
        return;
    }

    Debug::Log("[PlaylistSingleDecoder] Shutting down");

    // Lock and shutdown decoder
    {
        std::lock_guard<std::mutex> lock(switch_mutex_);

        if (decoder_) {
            decoder_->Shutdown();
            decoder_.reset();
        }

        if (image_cache_) {
            image_cache_->Shutdown();
            image_cache_.reset();
        }

        current_source_.clear();
        is_image_sequence_ = false;
    }

    device_ = nullptr;
    initialized_ = false;
}

//=============================================================================
// Source Management
//=============================================================================

bool PlaylistSingleDecoder::SwitchSource(const std::string& source_path) {
    if (!initialized_) {
        Debug::Log("[PlaylistSingleDecoder] Cannot switch source - not initialized");
        return false;
    }

    if (source_path.empty()) {
        Debug::Log("[PlaylistSingleDecoder] Cannot switch to empty source path");
        return false;
    }

    // Check if this is an image sequence
    bool new_is_sequence = IsSequencePath(source_path);

    // Fast path: already on this source with valid decoder/cache
    {
        std::lock_guard<std::mutex> lock(switch_mutex_);

        if (source_path == current_source_) {
            if (is_image_sequence_ && image_cache_) {
                return true;  // Already on this sequence
            }
            if (!is_image_sequence_ && decoder_ && decoder_->IsInitialized()) {
                return true;  // Already on this video
            }
        }
    }

    // Mark as switching
    switching_.store(true);

    Debug::Log("[PlaylistSingleDecoder] Switching source to: " + source_path +
               (new_is_sequence ? " (image sequence)" : " (video)"));

    std::lock_guard<std::mutex> lock(switch_mutex_);

    // Shutdown existing decoder/cache cleanly
    if (decoder_) {
        Debug::Log("[PlaylistSingleDecoder] Shutting down previous video decoder for: " + current_source_);
        decoder_->Shutdown();
        decoder_.reset();
    }
    if (image_cache_) {
        Debug::Log("[PlaylistSingleDecoder] Shutting down previous image cache for: " + current_source_);
        image_cache_->Shutdown();
        image_cache_.reset();
        source_frame_offset_ = 0;  // Reset frame offset
    }

    is_image_sequence_ = new_is_sequence;

    if (new_is_sequence) {
        // Initialize image sequence cache
        if (!InitializeImageSequence(source_path)) {
            Debug::Log("[PlaylistSingleDecoder] Failed to initialize image sequence for: " + source_path);
            switching_.store(false);
            return false;
        }
    } else {
        // Create fresh video decoder for new source
        decoder_ = std::make_unique<D3D11VideoDecoder>();
        decoder_->SetVideoPath(source_path);

        // Configure for single-decoder mode (smaller buffer, no readahead)
        StreamingDecoderConfig config;
        config.readAheadFrames = 16;   // Minimal readahead - we're not pre-warming
        config.readBehindFrames = 4;   // Minimal back buffer
        decoder_->SetConfig(config);

        if (!decoder_->Initialize()) {
            Debug::Log("[PlaylistSingleDecoder] Failed to initialize decoder for: " + source_path);
            decoder_.reset();
            switching_.store(false);
            return false;
        }

        Debug::Log("[PlaylistSingleDecoder] Successfully switched to video: " + source_path +
                   " (" + std::to_string(decoder_->GetWidth()) + "x" + std::to_string(decoder_->GetHeight()) +
                   " @ " + std::to_string(decoder_->GetFPS()) + "fps, " +
                   std::to_string(decoder_->GetFrameCount()) + " frames)");
    }

    current_source_ = source_path;
    last_source_switch_time_ = std::chrono::steady_clock::now();  // Record switch time for warmup grace
    switching_.store(false);

    return true;
}

//=============================================================================
// Frame Access
//=============================================================================

void PlaylistSingleDecoder::UpdatePlayhead(int source_frame) {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (is_image_sequence_) {
        if (image_cache_) {
            // Convert absolute source frame to relative (0-indexed) frame
            int relative_frame = source_frame - source_frame_offset_;
            // Convert frame to timestamp for DirectEXRCache
            double timestamp = static_cast<double>(relative_frame) / sequence_fps_;
            image_cache_->UpdateCurrentPosition(timestamp);
        }
    } else {
        if (decoder_ && decoder_->IsInitialized()) {
            decoder_->UpdatePlayhead(source_frame);
        }
    }
}

bool PlaylistSingleDecoder::HasFrame(int source_frame) const {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (is_image_sequence_) {
        if (image_cache_) {
            // Convert absolute source frame to relative (0-indexed) frame
            int relative_frame = source_frame - source_frame_offset_;
            return image_cache_->IsFrameCached(relative_frame);
        }
        return false;
    } else {
        if (decoder_ && decoder_->IsInitialized()) {
            return decoder_->HasFrame(source_frame);
        }
        return false;
    }
}

GLuint PlaylistSingleDecoder::GetFrameAsGLTexture(int source_frame) {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (is_image_sequence_) {
        if (image_cache_) {
            // Convert absolute source frame to relative (0-indexed) frame
            int relative_frame = source_frame - source_frame_offset_;
            GLuint texture = 0;
            int width = 0, height = 0;
            if (image_cache_->GetFrameOrLoad(relative_frame, texture, width, height)) {
                return texture;
            }
            // Return fallback texture if available
            return image_cache_->GetTexture(relative_frame, width, height);
        }
        return 0;
    } else {
        if (decoder_ && decoder_->IsInitialized()) {
            return decoder_->GetFrameAsGLTexture(source_frame);
        }
        return 0;
    }
}

ID3D11ShaderResourceView* PlaylistSingleDecoder::GetFrameAsD3D11SRV(int source_frame) {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    // Image sequences don't have D3D11 SRV - use GL texture path instead
    if (is_image_sequence_) {
        return nullptr;
    }

    if (decoder_ && decoder_->IsInitialized()) {
        return decoder_->GetFrameAsD3D11SRV(source_frame);
    }

    return nullptr;
}

//=============================================================================
// Metadata
//=============================================================================

int PlaylistSingleDecoder::GetWidth() const {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (is_image_sequence_) {
        if (image_cache_) {
            int w, h;
            image_cache_->GetFrameDimensions(w, h);
            return w;
        }
        return 0;
    }

    if (decoder_ && decoder_->IsInitialized()) {
        return decoder_->GetWidth();
    }
    return 0;
}

int PlaylistSingleDecoder::GetHeight() const {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (is_image_sequence_) {
        if (image_cache_) {
            int w, h;
            image_cache_->GetFrameDimensions(w, h);
            return h;
        }
        return 0;
    }

    if (decoder_ && decoder_->IsInitialized()) {
        return decoder_->GetHeight();
    }
    return 0;
}

double PlaylistSingleDecoder::GetFPS() const {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (is_image_sequence_) {
        return sequence_fps_;
    }

    if (decoder_ && decoder_->IsInitialized()) {
        return decoder_->GetFPS();
    }
    return 0.0;
}

int PlaylistSingleDecoder::GetFrameCount() const {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (is_image_sequence_) {
        if (image_cache_) {
            return image_cache_->GetStats().totalFrames;
        }
        return 0;
    }

    if (decoder_ && decoder_->IsInitialized()) {
        return decoder_->GetFrameCount();
    }
    return 0;
}

bool PlaylistSingleDecoder::IsHDR() const {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    // Image sequences (EXR) are typically HDR
    if (is_image_sequence_) {
        return true;
    }

    if (decoder_ && decoder_->IsInitialized()) {
        return decoder_->IsHDRContent();
    }
    return false;
}

//=============================================================================
// Image Sequence Support
//=============================================================================

void PlaylistSingleDecoder::SetSequenceMetadata(const std::string& directory, const std::string& pattern,
                                                 int start_frame, int end_frame, double fps,
                                                 const std::string& exr_layer,
                                                 double timeline_offset_seconds) {
    std::lock_guard<std::mutex> lock(switch_mutex_);
    pending_seq_directory_ = directory;
    pending_seq_pattern_ = pattern;
    pending_seq_start_frame_ = start_frame;
    pending_seq_end_frame_ = end_frame;
    sequence_fps_ = fps > 0 ? fps : 24.0;
    pending_seq_exr_layer_ = exr_layer;
    timeline_offset_seconds_ = timeline_offset_seconds;
}

bool PlaylistSingleDecoder::IsSequencePath(const std::string& path) const {
    // Check if path contains printf-style format specifier
    if (path.find('%') != std::string::npos) {
        return true;
    }

    // Check file extension for image types
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    return (ext == ".exr" || ext == ".tiff" || ext == ".tif" ||
            ext == ".png" || ext == ".jpg" || ext == ".jpeg");
}

bool PlaylistSingleDecoder::InitializeImageSequence(const std::string& source_path) {
    // Build file list from metadata or parse from path
    std::vector<std::string> sequence_files;

    if (!pending_seq_directory_.empty() && !pending_seq_pattern_.empty()) {
        // Use provided metadata
        fs::path dir(pending_seq_directory_);
        for (int frame = pending_seq_start_frame_; frame <= pending_seq_end_frame_; ++frame) {
            char frame_name[1024];
            snprintf(frame_name, sizeof(frame_name), pending_seq_pattern_.c_str(), frame);
            sequence_files.push_back((dir / frame_name).string());
        }
        Debug::Log("[PlaylistSingleDecoder] Using metadata: " + std::to_string(sequence_files.size()) +
                   " files from " + pending_seq_directory_);
    } else {
        // Try to parse from source_path (FFmpeg pattern format)
        // e.g., "D:/path/seq_%04d.exr" with start/end frames from ffprobe
        Debug::Log("[PlaylistSingleDecoder] No sequence metadata provided, cannot initialize: " + source_path);
        return false;
    }

    if (sequence_files.empty()) {
        Debug::Log("[PlaylistSingleDecoder] No sequence files found");
        return false;
    }

    // Detect format from first file extension
    std::string ext = fs::path(sequence_files[0]).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    std::unique_ptr<IImageLoader> loader;
    PipelineMode pipeline_mode = PipelineMode::HDR_RES;
    std::string format_name;

    if (ext == ".exr") {
        auto exr_loader = std::make_unique<EXRImageLoader>();
        if (!pending_seq_exr_layer_.empty()) {
            exr_loader->SetLayer(pending_seq_exr_layer_);
        }
        loader = std::move(exr_loader);
        format_name = "EXR";
    } else if (ext == ".tiff" || ext == ".tif") {
        loader = std::make_unique<TIFFImageLoader>();
        format_name = "TIFF";
        pipeline_mode = PipelineMode::NORMAL;
    } else if (ext == ".png") {
        loader = std::make_unique<PNGImageLoader>();
        format_name = "PNG";
        pipeline_mode = PipelineMode::NORMAL;
    } else if (ext == ".jpg" || ext == ".jpeg") {
        loader = std::make_unique<JPEGImageLoader>();
        format_name = "JPEG";
        pipeline_mode = PipelineMode::NORMAL;
    } else {
        Debug::Log("[PlaylistSingleDecoder] Unsupported image format: " + ext);
        return false;
    }

    // Create and initialize DirectEXRCache
    image_cache_ = std::make_unique<DirectEXRCache>();

    EXRCacheConfig cache_config;
    cache_config.readAheadFrames = g_exr_read_ahead_frames;
    cache_config.readBehindFrames = g_read_behind_frames;
    cache_config.threadCount = static_cast<size_t>(g_exr_thread_count);
    image_cache_->SetConfig(cache_config);

    // Store the source frame offset for converting between absolute and relative frames
    source_frame_offset_ = pending_seq_start_frame_;

    image_cache_->Initialize(
        std::move(loader),
        sequence_files,
        pending_seq_exr_layer_,
        sequence_fps_,
        pipeline_mode,
        pending_seq_start_frame_,
        0.0  // initial position
    );

    Debug::Log("[PlaylistSingleDecoder] Successfully initialized " + format_name + " sequence: " +
               std::to_string(sequence_files.size()) + " frames @ " + std::to_string(sequence_fps_) +
               "fps, source offset: " + std::to_string(source_frame_offset_));

    // Clear pending metadata
    pending_seq_directory_.clear();
    pending_seq_pattern_.clear();

    return true;
}

//=============================================================================
// Playback Control
//=============================================================================

bool PlaylistSingleDecoder::IsInWarmupPeriod() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_source_switch_time_).count();
    return elapsed < kWarmupGraceMs;
}

double PlaylistSingleDecoder::GetPlaybackSpeedFactor() const {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (is_image_sequence_ && image_cache_) {
        // During warmup grace period, don't throttle
        if (IsInWarmupPeriod()) {
            return 1.0;
        }
        return image_cache_->GetPlaybackSpeedFactor();
    }

    // Video decoder doesn't have adaptive speed - return full speed
    return 1.0;
}

bool PlaylistSingleDecoder::NeedsBufferWait() const {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (is_image_sequence_ && image_cache_) {
        // During warmup grace period after source switch, don't trigger buffer wait
        // This gives the cache time to start loading before we pause
        if (IsInWarmupPeriod()) {
            return false;
        }
        return image_cache_->NeedsBufferWait();
    }

    // Video decoder handles its own buffering
    return false;
}

double PlaylistSingleDecoder::GetCacheProgress() const {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (is_image_sequence_ && image_cache_) {
        auto stats = image_cache_->GetStats();
        if (stats.totalFrames > 0) {
            return static_cast<double>(stats.cachedFrames) / stats.totalFrames;
        }
    }

    // For video, we don't track cache progress the same way
    return 1.0;
}

int PlaylistSingleDecoder::GetCachedFrameCount() const {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (is_image_sequence_ && image_cache_) {
        return image_cache_->GetStats().cachedFrames;
    }

    return 0;
}

int PlaylistSingleDecoder::GetTotalFrameCount() const {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (is_image_sequence_ && image_cache_) {
        return image_cache_->GetStats().totalFrames;
    }

    if (decoder_ && decoder_->IsInitialized()) {
        return decoder_->GetFrameCount();
    }

    return 0;
}

std::vector<CacheSegment> PlaylistSingleDecoder::GetCacheSegments() const {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (is_image_sequence_ && image_cache_) {
        auto segments = image_cache_->GetCacheSegments();
        // Offset segments by timeline position so they appear at correct location in playlist
        for (auto& seg : segments) {
            seg.start_time += timeline_offset_seconds_;
            seg.end_time += timeline_offset_seconds_;
        }
        return segments;
    }

    // Video decoder doesn't have cache segments in the same way
    return {};
}

} // namespace ump

#endif // _WIN32
