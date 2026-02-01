#pragma once

#include <memory>
#include <string>
#include <vector>

#include "video_decoder_interface.h"
#include "pipeline_mode.h"  // For VideoRangeMode

namespace ump {

// Global video range override - set from main.cpp, read by factory when creating decoders
// NOTE: VideoRangeMode is defined in pipeline_mode.h (global namespace)
extern VideoRangeMode g_video_range_override;

//=============================================================================
// Video Decoder Factory
//
// Creates video decoders using GStreamer backend.
//=============================================================================

class VideoDecoderFactory {
public:
    // Singleton access
    static VideoDecoderFactory& Instance();

    // Delete copy/move
    VideoDecoderFactory(const VideoDecoderFactory&) = delete;
    VideoDecoderFactory& operator=(const VideoDecoderFactory&) = delete;

    //=========================================================================
    // Decoder Creation
    //=========================================================================

    // Create decoder for video path
    // Returns nullptr on failure (check GetLastError() for details)
    std::unique_ptr<IVideoDecoder> CreateDecoder(const std::string& video_path);

    // Create decoder with explicit backend selection (for API compatibility)
    std::unique_ptr<IVideoDecoder> CreateDecoder(
        const std::string& video_path,
        VideoDecoderBackend backend,
        bool force = false
    );

    //=========================================================================
    // Backend Configuration
    //=========================================================================

    void SetPreferredBackend(VideoDecoderBackend backend);
    VideoDecoderBackend GetPreferredBackend() const { return preferred_backend_; }

    // Check backend availability
    bool IsGStreamerAvailable() const;
    bool IsFFmpegAvailable() const;
    bool IsD3D11Available() const;
    bool IsBackendAvailable(VideoDecoderBackend backend) const;

    // Get list of available backends
    std::vector<VideoDecoderBackend> GetAvailableBackends() const;

    //=========================================================================
    // Diagnostics
    //=========================================================================

    const std::string& GetLastError() const { return last_error_; }
    int GetDecodersCreated() const { return decoders_created_; }

private:
    VideoDecoderFactory();
    ~VideoDecoderFactory();

    // State
    VideoDecoderBackend preferred_backend_ = VideoDecoderBackend::GSTREAMER;
    mutable std::string last_error_;
    int decoders_created_ = 0;
};

} // namespace ump
