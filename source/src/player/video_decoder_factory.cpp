#include "video_decoder_factory.h"

#include "streaming_video_decoder.h"  // FFmpeg-based decoder

#include <iostream>

namespace ump {

//=============================================================================
// Singleton Implementation
//=============================================================================

VideoDecoderFactory& VideoDecoderFactory::Instance() {
    static VideoDecoderFactory instance;
    return instance;
}

VideoDecoderFactory::VideoDecoderFactory() {
    // Default to FFmpeg backend (LibMPV removed - now handled directly in VideoDisplayComponent)
    preferred_backend_ = VideoDecoderBackend::FFMPEG;
}

VideoDecoderFactory::~VideoDecoderFactory() = default;

//=============================================================================
// Decoder Creation (FFmpeg only - LibMPV removed)
//
// NOTE: LibMPV-based decoding for VIDEO_FILE mode is now handled directly
// in VideoDisplayComponent using direct GPU rendering (no CPU roundtrip).
// This factory now only supports FFmpeg for MULTI_TRACK/DUAL_VIEW modes.
//=============================================================================

std::unique_ptr<IVideoDecoder> VideoDecoderFactory::CreateDecoder(const std::string& video_path) {
    return CreateDecoder(video_path, preferred_backend_, false);
}

std::unique_ptr<IVideoDecoder> VideoDecoderFactory::CreateDecoder(
    const std::string& video_path,
    VideoDecoderBackend backend,
    bool force)
{
    last_error_.clear();

    // Handle AUTO mode: use FFmpeg (only backend available in factory now)
    if (backend == VideoDecoderBackend::AUTO) {
        backend = VideoDecoderBackend::FFMPEG;
    }

    // GSTREAMER enum no longer supported through factory
    // (VIDEO_FILE mode uses direct MPV in VideoDisplayComponent)
    if (backend == VideoDecoderBackend::GSTREAMER) {
        std::cout << "[VideoDecoderFactory] GSTREAMER backend deprecated - VIDEO_FILE mode "
                  << "now uses direct MPV rendering. Falling back to FFmpeg." << std::endl;
        backend = VideoDecoderBackend::FFMPEG;
    }

    //=========================================================================
    // FFmpeg Backend (for MULTI_TRACK/DUAL_VIEW modes)
    //=========================================================================
    try {
        std::cout << "[VideoDecoderFactory] Creating FFmpeg decoder for: " << video_path << std::endl;
        auto decoder = std::make_unique<StreamingVideoDecoder>(video_path);
        decoders_created_++;
        return decoder;
    } catch (const std::exception& e) {
        last_error_ = std::string("FFmpeg decoder creation failed: ") + e.what();
        std::cerr << "[VideoDecoderFactory] " << last_error_ << std::endl;
        return nullptr;
    }
}

//=============================================================================
// Backend Availability
//=============================================================================

bool VideoDecoderFactory::IsGStreamerAvailable() const {
    // GSTREAMER/LibMPV no longer available through factory
    // VIDEO_FILE mode uses direct MPV in VideoDisplayComponent
    return false;
}

bool VideoDecoderFactory::IsFFmpegAvailable() const {
    return true;  // FFmpeg backend available (StreamingVideoDecoder)
}

bool VideoDecoderFactory::IsBackendAvailable(VideoDecoderBackend backend) const {
    switch (backend) {
        case VideoDecoderBackend::GSTREAMER:
            return IsGStreamerAvailable();
        case VideoDecoderBackend::FFMPEG:
            return IsFFmpegAvailable();
        case VideoDecoderBackend::AUTO:
            return IsFFmpegAvailable();
        default:
            return false;
    }
}

std::vector<VideoDecoderBackend> VideoDecoderFactory::GetAvailableBackends() const {
    std::vector<VideoDecoderBackend> backends;
    if (IsFFmpegAvailable()) {
        backends.push_back(VideoDecoderBackend::FFMPEG);
    }
    // GSTREAMER no longer available through factory
    return backends;
}

//=============================================================================
// Configuration
//=============================================================================

void VideoDecoderFactory::SetPreferredBackend(VideoDecoderBackend backend) {
    if (backend == VideoDecoderBackend::GSTREAMER) {
        std::cerr << "[VideoDecoderFactory] GSTREAMER backend deprecated - "
                  << "VIDEO_FILE mode uses direct MPV in VideoDisplayComponent" << std::endl;
        return;
    }

    if (IsBackendAvailable(backend)) {
        preferred_backend_ = backend;
        std::cout << "[VideoDecoderFactory] Preferred backend set to: "
                  << VideoDecoderBackendToString(backend) << std::endl;
    } else {
        std::cerr << "[VideoDecoderFactory] Backend not available: "
                  << VideoDecoderBackendToString(backend) << std::endl;
    }
}

} // namespace ump
