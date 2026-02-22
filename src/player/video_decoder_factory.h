#pragma once

// STUB FILE: VideoDecoderFactory (FFmpeg backend factory) has been removed.
// This file provides minimal declarations needed by other code.
// All video decoding now uses D3D11VideoDecoder directly.

#include <memory>
#include <string>
#include "video_decoder_interface.h"
#include "pipeline_mode.h"  // For VideoRangeMode

namespace qcview {

// Global video range override (used by HDR pipeline)
inline VideoRangeMode g_video_range_override = VideoRangeMode::AUTO;

//=============================================================================
// Video Decoder Factory - STUB
//
// The factory has been removed. Use D3D11VideoDecoder directly instead.
//=============================================================================

class VideoDecoderFactory {
public:
    static VideoDecoderFactory& Instance() {
        static VideoDecoderFactory instance;
        return instance;
    }

    // CreateDecoder is no longer supported - use D3D11VideoDecoder directly
    std::unique_ptr<IVideoDecoder> CreateDecoder(const std::string& /*path*/,
                                                  VideoDecoderBackend /*backend*/ = VideoDecoderBackend::AUTO) {
        return nullptr;  // Factory no longer creates decoders
    }

    const std::string& GetLastError() const {
        static std::string error = "VideoDecoderFactory has been removed. Use D3D11VideoDecoder directly.";
        return error;
    }

private:
    VideoDecoderFactory() = default;
};

} // namespace qcview
