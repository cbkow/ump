#include "video_decoder_factory.h"

#ifdef WITH_GSTREAMER
#include "gstreamer_video_decoder.h"
#include "gstreamer_manager.h"
#endif

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
    preferred_backend_ = VideoDecoderBackend::GSTREAMER;
}

VideoDecoderFactory::~VideoDecoderFactory() = default;

//=============================================================================
// Decoder Creation (GStreamer or FFmpeg)
//=============================================================================

std::unique_ptr<IVideoDecoder> VideoDecoderFactory::CreateDecoder(const std::string& video_path) {
    // Default: use preferred backend (GStreamer for single file playback)
    return CreateDecoder(video_path, preferred_backend_, false);
}

std::unique_ptr<IVideoDecoder> VideoDecoderFactory::CreateDecoder(
    const std::string& video_path,
    VideoDecoderBackend backend,
    bool force)
{
    last_error_.clear();

    // Handle AUTO mode: use preferred backend
    if (backend == VideoDecoderBackend::AUTO) {
        backend = preferred_backend_;
    }

    //=========================================================================
    // FFmpeg Backend (for timeline/multi-track mode)
    //=========================================================================
    if (backend == VideoDecoderBackend::FFMPEG || force) {
        try {
            std::cout << "[VideoDecoderFactory] Creating FFmpeg decoder for: " << video_path << std::endl;
            auto decoder = std::make_unique<StreamingVideoDecoder>(video_path);
            decoders_created_++;
            return decoder;
        } catch (const std::exception& e) {
            last_error_ = std::string("FFmpeg decoder creation failed: ") + e.what();
            std::cerr << "[VideoDecoderFactory] " << last_error_ << std::endl;
            // If forced, don't fall back
            if (force) return nullptr;
            // Otherwise fall through to try GStreamer
        }
    }

    //=========================================================================
    // GStreamer Backend (for single file playback with HW accel + A/V sync)
    //=========================================================================
#ifdef WITH_GSTREAMER
    if (backend == VideoDecoderBackend::GSTREAMER || backend == VideoDecoderBackend::AUTO) {
        // Initialize GStreamer if not already done
        if (!GStreamerManager::Instance().IsInitialized()) {
            std::cout << "[VideoDecoderFactory] GStreamer not initialized, initializing now..." << std::endl;
            if (!GStreamerManager::Instance().Initialize()) {
                last_error_ = "Failed to initialize GStreamer: " + GStreamerManager::Instance().GetInitError();
                std::cerr << "[VideoDecoderFactory] " << last_error_ << std::endl;
                return nullptr;
            }
            std::cout << "[VideoDecoderFactory] GStreamer initialized: "
                      << GStreamerManager::Instance().GetVersionString() << std::endl;
        }

        // Create GStreamer decoder
        try {
            std::cout << "[VideoDecoderFactory] Creating GStreamer decoder for: " << video_path << std::endl;
            auto decoder = std::make_unique<GStreamerVideoDecoder>(video_path);
            decoders_created_++;
            return decoder;
        } catch (const std::exception& e) {
            last_error_ = std::string("GStreamer decoder creation failed: ") + e.what();
            std::cerr << "[VideoDecoderFactory] " << last_error_ << std::endl;
            return nullptr;
        }
    }
#endif

    last_error_ = "No video decoder backend available";
    std::cerr << "[VideoDecoderFactory] " << last_error_ << std::endl;
    return nullptr;
}

//=============================================================================
// Backend Availability
//=============================================================================

bool VideoDecoderFactory::IsGStreamerAvailable() const {
#ifdef WITH_GSTREAMER
    return GStreamerManager::Instance().IsInitialized();
#else
    return false;
#endif
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
            return IsGStreamerAvailable() || IsFFmpegAvailable();
        default:
            return false;
    }
}

std::vector<VideoDecoderBackend> VideoDecoderFactory::GetAvailableBackends() const {
    std::vector<VideoDecoderBackend> backends;
    if (IsFFmpegAvailable()) {
        backends.push_back(VideoDecoderBackend::FFMPEG);
    }
    if (IsGStreamerAvailable()) {
        backends.push_back(VideoDecoderBackend::GSTREAMER);
    }
    return backends;
}

//=============================================================================
// Configuration
//=============================================================================

void VideoDecoderFactory::SetPreferredBackend(VideoDecoderBackend backend) {
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
