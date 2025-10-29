#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <filesystem>

// Forward declarations for FFmpeg types
struct AVFormatContext;
struct AVCodecContext;
struct AVBufferRef;
struct SwsContext;
struct AVFrame;

// Include full definitions (needed for member variables and default parameters)
#include "../metadata/video_metadata.h"
#include "../player/media_background_extractor.h"  // For HardwareDecodeMode

namespace fs = std::filesystem;

namespace ump {

/**
 * DifferenceSequenceTranscoder
 *
 * Direct 2-producer, 1-consumer architecture for difference transcoding.
 * Decodes two videos in parallel, composites differences on CPU, and writes
 * ONLY the combined difference sequence to disk.
 *
 * Flow:
 *   [Primary Decoder] ──┐
 *                       ├──> [CPU Compositor] ──> [PNG Writer]
 *   [Comparison Decoder]┘
 *
 * Benefits:
 *   - 3x faster (no intermediate PNGs)
 *   - 2/3 less disk space (only combined sequence)
 *   - Simpler architecture
 */
class DifferenceSequenceTranscoder {
public:
    struct TranscodeConfig {
        int max_width = 1920;                           // Target resolution
        HardwareDecodeMode hw_decode = HardwareDecodeMode::D3D11VA;
        float amplification = 5.0f;                     // Difference amplification
        bool force_retranscode = false;
        std::string custom_cache_path;
        int png_compression_level = 3;
    };

    DifferenceSequenceTranscoder();
    ~DifferenceSequenceTranscoder();

    // Initialize with both video paths
    bool Initialize(
        const std::string& primary_video_path,
        const VideoMetadata& primary_metadata,
        const std::string& comparison_video_path,
        const VideoMetadata& comparison_metadata,
        const TranscodeConfig& config = TranscodeConfig{}
    );

    // Start async transcoding
    bool StartTranscode();
    void CancelTranscode();
    void Cleanup();

    // Status
    bool IsInitialized() const { return initialized_; }
    bool IsTranscoding() const { return transcoding_; }
    bool IsComplete() const { return complete_; }
    float GetProgress() const { return progress_.load(); }

    // Cache info
    std::string GetCombinedCacheDirectory() const { return combined_cache_dir_.string(); }
    bool IsCached() const;
    int GetTotalFrames() const { return total_frames_; }

    // Callbacks
    void SetProgressCallback(std::function<void(float)> callback) { progress_callback_ = callback; }
    void SetCompletionCallback(std::function<void()> callback) { completion_callback_ = callback; }

private:
    // Dual FFmpeg decoder contexts
    struct DecoderContext {
        AVFormatContext* format_ctx = nullptr;
        AVCodecContext* codec_ctx = nullptr;
        AVBufferRef* hw_device_ctx = nullptr;
        SwsContext* sws_ctx = nullptr;
        int video_stream_index = -1;
        bool initialized = false;

        ~DecoderContext();
        void Cleanup();
        bool Initialize(const std::string& video_path, HardwareDecodeMode hw_mode);
    };

    // Transcoding workers (multi-threaded)
    void TranscodeWorker();
    void ParallelTranscodeWorker(int worker_id);
    bool TranscodeFrame(int frame_number, DecoderContext& primary_ctx, DecoderContext& comparison_ctx);

    // Decode frame from specific video
    bool DecodeFrameAtTimestamp(double timestamp, AVFrame* output_frame, DecoderContext& ctx);

    // CPU-based difference compositing
    bool CompositeAndSaveDifference(AVFrame* primary_frame, AVFrame* comparison_frame, int frame_number);

    // PNG I/O
    bool SaveFrameAsPNG(unsigned char* rgb_data, int width, int height, const std::string& output_path);

    // Hardware decode setup
    bool SetupHardwareDecode(DecoderContext& ctx);

    // Cache management
    void SetupCacheDirectory();
    std::string GenerateCacheKey() const;

    // Configuration
    TranscodeConfig config_;
    std::string primary_video_path_;
    std::string comparison_video_path_;
    VideoMetadata primary_metadata_;
    VideoMetadata comparison_metadata_;

    // Decoder contexts (single-threaded fallback)
    DecoderContext primary_decoder_;
    DecoderContext comparison_decoder_;

    // Multi-threaded decoder contexts (one pair per worker)
    std::vector<DecoderContext> primary_decoders_;
    std::vector<DecoderContext> comparison_decoders_;
    int num_threads_ = 8;  // Configurable worker count

    // Video properties
    int total_frames_ = 0;
    int output_width_ = 0;
    int output_height_ = 0;
    double frame_rate_ = 30.0;
    double duration_ = 0.0;

    // Cache directory
    fs::path combined_cache_dir_;
    std::string cache_key_;

    // State
    std::atomic<bool> initialized_{false};
    std::atomic<bool> transcoding_{false};
    std::atomic<bool> complete_{false};
    std::atomic<bool> cancel_requested_{false};
    std::atomic<float> progress_{0.0f};
    std::atomic<int> frames_completed_{0};

    // Threading
    std::thread transcode_thread_;
    std::vector<std::thread> worker_threads_;
    std::atomic<int> next_frame_{0};
    mutable std::mutex state_mutex_;
    std::mutex progress_mutex_;

    // Callbacks
    std::function<void(float)> progress_callback_;
    std::function<void()> completion_callback_;
};

} // namespace ump
