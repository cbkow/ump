#pragma once

// STUB FILE: StreamingVideoDecoder (FFmpeg backend) has been removed.
// This file provides a forward declaration to allow dependent code to compile.
// The actual implementation has been removed - all video decoding now uses D3D11VideoDecoder.

#include "video_decoder_interface.h"

namespace ump {

// Forward declaration only - no implementation
// Code using StreamingVideoDecoder should be updated to use D3D11VideoDecoder instead
class StreamingVideoDecoder : public IVideoDecoder {
public:
    StreamingVideoDecoder(const std::string& /*path*/) {}
    ~StreamingVideoDecoder() override = default;

    // IVideoDecoder interface - stub implementations that return failure
    bool Initialize() override { return false; }
    void Shutdown() override {}
    void HardReset(int) override {}
    bool IsInitialized() const override { return false; }

    std::shared_ptr<PixelData> GetFrame(int) override { return nullptr; }
    std::shared_ptr<PixelData> GetClosestFrame(int, int* = nullptr) override { return nullptr; }
    bool HasFrame(int) const override { return false; }

    std::shared_ptr<PixelData> GetKeyframe(int, int* = nullptr) override { return nullptr; }
    int GetNearestKeyframePosition(int target) const override { return target; }
    bool IsIntraFrameCodec() const override { return true; }
    bool HasKeyframeIndex() const override { return false; }
    const std::vector<int>& GetKeyframePositions() const override { static std::vector<int> empty; return empty; }

    void UpdatePlayhead(int, SeekQuality = SeekQuality::NORMAL, bool = false) override {}
    void SetNeededFrames(const std::vector<int>&) override {}
    DecodeStatus GetDecodeStatus() const override { return {}; }
    void EvictOutsideWindow(const std::set<int>&) override {}
    std::set<int> GetBufferedFramesSet() const override { return {}; }

    int GetBufferedAhead() const override { return 0; }
    int GetBufferedBehind() const override { return 0; }
    int GetBufferSize() const override { return 0; }
    void GetBufferedRange(int& start, int& end) const override { start = end = 0; }
    void ClearBuffer() override {}
    bool IsSeekPending() const override { return false; }
    int GetLastSeekTarget() const override { return 0; }

    int GetWidth() const override { return 0; }
    int GetHeight() const override { return 0; }
    double GetFPS() const override { return 0; }
    double GetDuration() const override { return 0; }
    int GetFrameCount() const override { return 0; }
    const std::string& GetPath() const override { static std::string empty; return empty; }

    HWAccelType GetHWAccelType() const override { return HWAccelType::NONE; }
    bool IsHardwareAccelerated() const override { return false; }

    void SetConfig(const StreamingDecoderConfig&) override {}
    const StreamingDecoderConfig& GetConfig() const override { static StreamingDecoderConfig cfg; return cfg; }

    void SetPipelineMode(PipelineMode) override {}
    PipelineMode GetPipelineMode() const override { return PipelineMode::NORMAL; }

    void SetShuttleMode(bool) override {}
    bool IsShuttleMode() const override { return false; }

    void SetLoopPoints(int, int) override {}
    void ClearLoopPoints() override {}

    const char* GetBackendName() const override { return "Stub (FFmpeg removed)"; }
    VideoDecoderBackend GetBackendType() const override { return VideoDecoderBackend::FFMPEG; }

    // Additional stub methods for HDRDirectVideoDecoder compatibility
    void SetRawHWFrameMode(bool) {}
    void* GetRawHWFrame(int) { return nullptr; }
    bool IsHWFrame() const { return false; }
};

} // namespace ump
