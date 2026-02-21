#include "direct_exr_cache_source.h"
#include "../utils/debug_utils.h"
#include "image_loaders.h"

#include <algorithm>
#include <filesystem>

namespace ump {

//=============================================================================
// Construction / Destruction
//=============================================================================

DirectEXRCacheSource::DirectEXRCacheSource()
    : cache_(std::make_unique<DirectEXRCache>())
{
}

DirectEXRCacheSource::~DirectEXRCacheSource() {
    Shutdown();
}

//=============================================================================
// Lifecycle
//=============================================================================

bool DirectEXRCacheSource::Initialize(const FrameSourceConfig& config) {
    if (config.files.empty()) {
        Debug::Log("DirectEXRCacheSource: No files provided");
        return false;
    }

    config_ = config;
    fps_ = config.fps;

    // Detect format from first file extension
    std::string ext;
    if (!config.files.empty()) {
        ext = std::filesystem::path(config.files[0]).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    // Create appropriate loader based on format
    std::unique_ptr<IImageLoader> loader;
    if (ext == ".exr") {
        loader = std::make_unique<EXRImageLoader>();
        source_type_ = "EXR";
    } else if (ext == ".tiff" || ext == ".tif") {
        loader = std::make_unique<TIFFImageLoader>();
        source_type_ = "TIFF";
    } else if (ext == ".png") {
        loader = std::make_unique<PNGImageLoader>();
        source_type_ = "PNG";
    } else if (ext == ".jpg" || ext == ".jpeg") {
        loader = std::make_unique<JPEGImageLoader>();
        source_type_ = "JPEG";
    } else {
        Debug::Log("DirectEXRCacheSource: Unsupported format: " + ext);
        return false;
    }

    // Configure the cache
    EXRCacheConfig cache_config;
    cache_config.threadCount = config.thread_count;
    cache_config.readAheadFrames = config.read_ahead_frames;
    cache_config.readBehindFrames = config.read_behind_frames;
    cache_config.use_shared_pool = config.use_shared_pool;
    cache_->SetConfig(cache_config);

    // Initialize the cache with the loader
    bool success = cache_->Initialize(
        std::move(loader),
        config.files,
        config.layer,
        config.fps,
        config.pipeline_mode,
        config.start_frame,
        0.0  // initial_position
    );

    if (success) {
        Debug::Log("DirectEXRCacheSource: Initialized " + source_type_ +
                   " sequence with " + std::to_string(config.files.size()) + " frames");
    }

    return success;
}

void DirectEXRCacheSource::Shutdown() {
    if (cache_) {
        cache_->Shutdown();
    }
}

bool DirectEXRCacheSource::IsInitialized() const {
    return cache_ && cache_->IsInitialized();
}

//=============================================================================
// Position & Playback State
//=============================================================================

void DirectEXRCacheSource::UpdatePosition(double timestamp) {
    if (cache_) {
        cache_->UpdateCurrentPosition(timestamp);
    }
}

void DirectEXRCacheSource::SetPlaying(bool playing) {
    if (cache_) {
        cache_->UpdatePlaybackState(playing);
    }
}

void DirectEXRCacheSource::SetLoopRange(int in_frame, int out_frame) {
    if (cache_) {
        cache_->SetLoopRange(in_frame, out_frame);
    }
}

void DirectEXRCacheSource::ClearLoopRange() {
    if (cache_) {
        cache_->ClearLoopRange();
    }
}

void DirectEXRCacheSource::SetLooping(bool enabled) {
    if (cache_) {
        cache_->SetLooping(enabled);
    }
}

//=============================================================================
// Frame Access
//=============================================================================

GLuint DirectEXRCacheSource::GetFrame(int frame, int& width, int& height, bool& exact) {
    if (!cache_) {
        exact = false;
        return 0;
    }

    GLuint texture = 0;
    exact = cache_->GetFrameOrLoad(frame, texture, width, height);
    return texture;
}

bool DirectEXRCacheSource::IsFrameCached(int frame) const {
    if (!cache_) return false;
    return cache_->IsFrameCached(frame);
}

void DirectEXRCacheSource::ProcessPendingOperations() {
    if (cache_) {
        cache_->ProcessReadyTextures();
    }
}

//=============================================================================
// Buffer Health
//=============================================================================

BufferHealth DirectEXRCacheSource::GetBufferHealth() const {
    BufferHealth health;

    if (!cache_) return health;

    auto stats = cache_->GetStats();
    health.total_cached = stats.cachedFrames;
    health.total_frames = stats.totalFrames;
    health.speed_factor = 1.0;

    return health;
}

void DirectEXRCacheSource::ResetPlaybackState() {
    if (cache_) {
        cache_->ResetPlaybackSpeed();
        cache_->ResetOverrunMode();
    }
}

//=============================================================================
// Source Metadata
//=============================================================================

int DirectEXRCacheSource::GetTotalFrames() const {
    if (!cache_) return 0;
    return cache_->GetStats().totalFrames;
}

double DirectEXRCacheSource::GetFPS() const {
    return fps_;
}

bool DirectEXRCacheSource::GetDimensions(int& width, int& height) const {
    if (!cache_) return false;
    return cache_->GetFrameDimensions(width, height);
}

std::string DirectEXRCacheSource::GetSourceType() const {
    return source_type_;
}

//=============================================================================
// Statistics
//=============================================================================

DirectEXRCache::Stats DirectEXRCacheSource::GetStats() const {
    if (!cache_) return DirectEXRCache::Stats();
    return cache_->GetStats();
}

std::vector<CacheSegment> DirectEXRCacheSource::GetCacheSegments() const {
    if (!cache_) return {};
    return cache_->GetCacheSegments();
}

} // namespace ump
