#include "video_decoder_factory.h"

#include "streaming_video_decoder.h"  // FFmpeg-based decoder

#ifdef _WIN32
#include "d3d11_video_decoder.h"      // D3D11 GPU-native decoder
#endif

#include <iostream>

namespace ump {

// Global video range override - set from main.cpp, read by factory when creating decoders
VideoRangeMode g_video_range_override = VideoRangeMode::AUTO;

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

    // Handle AUTO mode: prefer D3D11 on Windows for GPU-native decoding
    // D3D11VideoDecoder returns GL textures directly via GetFrameAsGLTexture()
    // TimelineCache's D3D11 direct path handles this (no PixelData needed)
    // Falls back to FFmpeg only if D3D11 init fails
    if (backend == VideoDecoderBackend::AUTO) {
#ifdef _WIN32
        backend = VideoDecoderBackend::D3D11;
#else
        backend = VideoDecoderBackend::FFMPEG;
#endif
    }


#ifdef _WIN32
    //=========================================================================
    // D3D11 Backend (GPU-native, Windows only)
    //=========================================================================
    if (backend == VideoDecoderBackend::D3D11) {
        try {
            std::cout << "[VideoDecoderFactory] Creating D3D11 decoder for: " << video_path << std::endl;
            auto decoder = std::make_unique<D3D11VideoDecoder>();
            decoder->SetVideoPath(video_path);
            // Apply global video range override
            decoder->SetVideoRangeOverride(g_video_range_override);
            if (decoder->Initialize()) {
                decoders_created_++;
                return decoder;
            } else {
                std::cout << "[VideoDecoderFactory] D3D11 decoder init failed, falling back to FFmpeg" << std::endl;
                // Fall through to FFmpeg
            }
        } catch (const std::exception& e) {
            std::cout << "[VideoDecoderFactory] D3D11 decoder exception: " << e.what()
                      << ", falling back to FFmpeg" << std::endl;
            // Fall through to FFmpeg
        }
    }
#endif

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

bool VideoDecoderFactory::IsFFmpegAvailable() const {
    return true;  // FFmpeg backend available (StreamingVideoDecoder)
}

bool VideoDecoderFactory::IsD3D11Available() const {
#ifdef _WIN32
    return true;  // D3D11 backend available on Windows
#else
    return false;
#endif
}

bool VideoDecoderFactory::IsBackendAvailable(VideoDecoderBackend backend) const {
    switch (backend) {
        case VideoDecoderBackend::FFMPEG:
            return IsFFmpegAvailable();
        case VideoDecoderBackend::D3D11:
            return IsD3D11Available();
        case VideoDecoderBackend::AUTO:
            return IsFFmpegAvailable() || IsD3D11Available();
        default:
            return false;
    }
}

std::vector<VideoDecoderBackend> VideoDecoderFactory::GetAvailableBackends() const {
    std::vector<VideoDecoderBackend> backends;
#ifdef _WIN32
    backends.push_back(VideoDecoderBackend::D3D11);
#endif
    if (IsFFmpegAvailable()) {
        backends.push_back(VideoDecoderBackend::FFMPEG);
    }
    return backends;
}

//=============================================================================
// Configuration
//=============================================================================

void VideoDecoderFactory::SetPreferredBackend(VideoDecoderBackend backend) {
#ifndef _WIN32
    if (backend == VideoDecoderBackend::D3D11) {
        std::cerr << "[VideoDecoderFactory] D3D11 backend only available on Windows" << std::endl;
        return;
    }
#endif

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
