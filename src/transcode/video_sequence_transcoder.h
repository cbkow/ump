#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <memory>
#include <filesystem>

// Forward declarations for FFmpeg types
struct AVFormatContext;
struct AVCodecContext;
struct AVBufferRef;
struct SwsContext;
struct AVFrame;

// Include full definitions (needed for default parameters and member variables)
#include "../metadata/video_metadata.h"
#include "../player/media_background_extractor.h"  // For HardwareDecodeMode and ConversionStrategy

namespace fs = std::filesystem;

namespace qcview {

/**
 * VideoSequenceTranscoder
 *
 * Transcodes video files to frame-indexed PNG sequences on disk for frame-accurate
 * dual video comparison. Uses the same color awareness patterns as MediaBackgroundExtractor
 * to ensure correct colorspace handling for 4444/422/420 formats.
 *
 * Cache Directory Pattern:
 *   %LOCALAPPDATA%\qcview\VideoFrameCache\{hash}_1920_bt709_limited\
 *     ├── frame_00000.png
 *     ├── frame_00001.png
 *     └── ...
 */
class VideoSequenceTranscoder {
public:
    struct TranscodeConfig {
        int max_width = 1920;                           // Target resolution (width)
        HardwareDecodeMode hw_decode = HardwareDecodeMode::D3D11VA;
        bool force_retranscode = false;                 // Re-transcode even if exists
        std::string custom_cache_path;                  // User-specified cache directory
        int png_compression_level = 3;                  // PNG compression (0-9, 3 is balanced)
    };

    VideoSequenceTranscoder();
    ~VideoSequenceTranscoder();

    // Lifecycle
    bool Initialize(const std::string& video_path, const VideoMetadata& metadata, const TranscodeConfig& config = TranscodeConfig{});
    bool StartTranscode();                              // Start async transcode to disk
    void CancelTranscode();                             // Cancel in-progress transcode
    void Cleanup();                                     // Stop transcode and cleanup resources

    // Status
    bool IsInitialized() const { return initialized_; }
    bool IsTranscoding() const { return transcoding_; }
    bool IsTranscodeComplete() const { return transcode_complete_; }
    float GetTranscodeProgress() const { return transcode_progress_.load(); }

    // Cache management
    std::string GetCacheDirectory() const;              // e.g., %LOCALAPPDATA%\qcview\VideoFrameCache\{hash}/
    bool IsCached() const;                              // Check if transcode already exists on disk
    void ClearCache();                                  // Delete cached sequence

    // Frame access
    std::string GetFramePath(int frame_number) const;   // frame_00000.png
    int GetTotalFrames() const { return total_frames_; }
    int GetWidth() const { return output_width_; }
    int GetHeight() const { return output_height_; }
    double GetFrameRate() const { return frame_rate_; }

private:
    // FFmpeg context per worker (thread-safe pattern from MediaBackgroundExtractor)
    struct WorkerContext {
        AVFormatContext* format_ctx = nullptr;
        AVCodecContext* codec_ctx = nullptr;
        AVBufferRef* hw_device_ctx = nullptr;
        SwsContext* sws_ctx = nullptr;
        int video_stream_index = -1;
        bool initialized = false;

        ~WorkerContext();
        void Cleanup();
        bool Initialize(const std::string& video_path, HardwareDecodeMode hw_mode);
    };

    // Cache directory setup (mimic EXRTranscoder pattern)
    void SetupCacheDirectory();
    std::string GenerateCacheKey() const;               // Hash from path + resolution + color settings

    // Transcode worker (background thread)
    void TranscodeWorker();
    bool ExtractFrameToPNG(int frame_number, double timestamp, WorkerContext& ctx);
    bool DecodeFrameAtTimestamp(double timestamp, AVFrame* output_frame, WorkerContext& ctx);
    bool SaveFrameAsPNG(AVFrame* frame, const std::string& output_path, int frame_number);

    // Hardware decode setup
    bool SetupHardwareDecode(WorkerContext& ctx);
    bool InitializeD3D11VA(WorkerContext& ctx);
    bool InitializeNVDEC(WorkerContext& ctx);

    // Configuration
    TranscodeConfig config_;
    std::string video_path_;
    VideoMetadata metadata_;
    std::unique_ptr<ConversionStrategy> conversion_strategy_;

    // Video properties
    int total_frames_ = 0;
    int output_width_ = 0;
    int output_height_ = 0;
    double frame_rate_ = 30.0;
    double duration_ = 0.0;

    // Cache directory
    fs::path cache_dir_;
    std::string cache_key_;

    // State
    std::atomic<bool> initialized_{false};
    std::atomic<bool> transcoding_{false};
    std::atomic<bool> transcode_complete_{false};
    std::atomic<bool> cancel_requested_{false};
    std::atomic<float> transcode_progress_{0.0f};

    // Threading
    std::thread transcode_thread_;
    mutable std::mutex state_mutex_;
};

} // namespace qcview
