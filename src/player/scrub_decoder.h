#pragma once

#include <memory>
#include <string>
#include <map>
#include <mutex>
#include <cstdint>
#include "image_loader_interface.h"
#include "frame_index.h"

#ifdef QCVIEW_USE_METAL
namespace qcview {
class MetalHWFrameExtractor;
class MetalYUVRenderer;
}
#endif

// Forward declarations for FFmpeg
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;
struct SwsContext;

namespace qcview {

//=============================================================================
// ScrubDecoder - Single-frame on-demand decoder for responsive scrubbing
//
// Key design principles:
// - NO background thread (synchronous decode)
// - NO buffer/queue (single frame at a time)
// - Latest request wins (no backlog)
// - Keyframe seeking for speed
// - Returns closest keyframe if exact frame not available
//=============================================================================

class ScrubDecoder {
public:
    explicit ScrubDecoder(const std::string& video_path);
    ~ScrubDecoder();

    // Initialize FFmpeg context
    bool Initialize();
    void Shutdown();

    // Primary scrub access - seeks and decodes single frame
    // Returns the decoded frame (may be keyframe if exact frame too expensive)
    std::shared_ptr<PixelData> GetClosestFrame(int frame_number, int* actual_frame = nullptr);

    // Alias for GetClosestFrame (same behavior for scrubbing)
    std::shared_ptr<PixelData> GetFrame(int frame_number);

    // Metal zero-copy path: returns MetalTexturePool ID directly.
    // CVPixelBuffer → HWFrameExtractor → YUVRenderer → pool texture.
    // Returns 0 if not available (non-Metal or HW accel inactive).
    uint64_t GetFrameAsPoolTexture(int frame_number, int* actual_frame = nullptr);

    // Keyframe-only scrub: seek to nearest keyframe and decode just that frame.
    // Much faster than decoding to target for inter-frame codecs (H.264/HEVC).
    // Returns the keyframe's pool texture ID (Metal) or PixelData (CPU).
    uint64_t GetKeyframeAsPoolTexture(int frame_number, int* actual_frame = nullptr);
    std::shared_ptr<PixelData> GetKeyframe(int frame_number, int* actual_frame = nullptr);

    // Metadata
    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }
    double GetFPS() const { return fps_; }
    int GetFrameCount() const { return frame_count_; }
    bool IsInitialized() const { return initialized_; }
    const std::string& GetPath() const { return video_path_; }

private:
    // FFmpeg decode - seeks to keyframe and decodes to target (or just keyframe)
    std::shared_ptr<PixelData> DecodeFrame(int target_frame, int* actual_frame);

    // Convert AVFrame to PixelData
    std::shared_ptr<PixelData> ConvertFrame(AVFrame* frame);

    // Seek to nearest keyframe at or before target
    bool SeekToKeyframe(int target_frame);

    std::string video_path_;
    bool initialized_ = false;

    // FFmpeg contexts
    AVFormatContext* format_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* rgb_frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;

    AVBufferRef* hw_device_ctx_ = nullptr;
    bool hw_accel_active_ = false;

#ifdef QCVIEW_USE_METAL
    // Metal zero-copy GPU pipeline (reuses same shaders as MetalVideoDecoder)
    std::unique_ptr<MetalHWFrameExtractor> hw_extractor_;
    std::unique_ptr<MetalYUVRenderer> yuv_renderer_;
    bool gpu_pipeline_ready_ = false;
    bool is_full_range_ = false;
    bool is_bt2020_ = false;
    bool is_hdr_ = false;
    uint64_t last_pool_texture_id_ = 0;  // Track for cleanup on next scrub

    // Decode to pool texture (Metal zero-copy path)
    uint64_t DecodeFrameToPoolTexture(int target_frame, int* actual_frame);
#endif

    int video_stream_idx_ = -1;
    int width_ = 0;
    int height_ = 0;
    double fps_ = 24.0;
    int frame_count_ = 0;
    int64_t start_time_ = 0;

    // Frame index for precise seeking (built from AVStream index_entries)
    std::shared_ptr<qcview::FrameIndex> frame_index_;

    // Cache last decoded frame to avoid redundant decodes
    int last_decoded_frame_ = -1;
    std::shared_ptr<PixelData> last_decoded_pixels_;

    mutable std::mutex decode_mutex_;
};

//=============================================================================
// ScrubDecoderManager - Manages scrub decoders per source file
//=============================================================================

class ScrubDecoderManager {
public:
    ScrubDecoderManager() = default;
    ~ScrubDecoderManager();

    // Get or create scrub decoder for a source path
    ScrubDecoder* GetDecoder(const std::string& source_path);

    // Clear all decoders (call when scrub mode ends)
    void ClearAll();

    // Clear decoder for a specific source
    void Clear(const std::string& source_path);

    // Check if any decoders are active
    bool HasDecoders() const;

private:
    std::map<std::string, std::unique_ptr<ScrubDecoder>> decoders_;
    mutable std::mutex mutex_;
};

} // namespace qcview
