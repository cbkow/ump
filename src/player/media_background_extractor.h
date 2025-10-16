#pragma once

#include <glad/gl.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>
#include <queue>
#include <set>
#include <thread>
#include <condition_variable>

// Forward declarations for FFmpeg types
struct AVFormatContext;
struct AVCodecContext;
struct AVBufferRef;
struct SwsContext;
struct AVFrame;

// Forward declarations
struct VideoMetadata;
class FrameCache;
#include "video_player.h"

// Color matrix processing modes for different pixel formats
enum class ColorMatrixMode {
    NONE,           // No color matrix processing (fallback/unknown formats)
    RANGE_ONLY,     // Apply range conversion only (422/420 formats with known range issues)
    FULL_MATRIX     // Apply full colorspace + range conversion (4444 formats)
};

// Smart conversion strategy for metadata-driven cache extraction with format-specific processing
struct ConversionStrategy {
    int source_colorspace = 1;          // FFmpeg colorspace constant (1=BT.709, 5=BT.601, 9=BT.2020)
    int source_range = 0;               // 0=limited range (16-235), 1=full range (0-255)
    int source_bit_depth = 8;           // Source bit depth for quality decisions
    int sws_algorithm = 0;              // SWS_FAST_BILINEAR or SWS_LANCZOS
    bool needs_tone_mapping = false;    // For future HDR support
    std::string debug_info;             // Human readable conversion info
    ColorMatrixMode matrix_mode = ColorMatrixMode::NONE;  // NEW: Format-specific processing mode

    // Create strategy from metadata with format-specific mode selection
    static ConversionStrategy FromMetadata(const VideoMetadata& metadata);

    // NEW: Check if any color matrix processing should be applied
    bool ShouldApplyColorMatrix() const;

    // NEW: Check if full colorspace matrix should be applied (4444 formats)
    bool ShouldApplyFullMatrix() const;

    // NEW: Check if only range conversion should be applied (422/420 formats)
    bool ShouldApplyRangeOnly() const;

    // Get debug description
    std::string GetDescription() const;
};

enum class HardwareDecodeMode {
    SOFTWARE_ONLY,
    D3D11VA,        // Default - broad Windows GPU support
    NVDEC,          // Optional - maximum NVIDIA performance
    AUTO            // Try NVDEC → D3D11VA → Software
};

enum class ExtractorState {
    STOPPED,           // Not running
    EXTRACTING,        // Active extraction
    PAUSED_PLAYBACK,   // Paused due to playback
    PAUSED_REPOSITION, // Paused due to timeline seek
    PAUSED_MANUAL      // Manually paused
};

struct HardwareDecodeConfig {
    HardwareDecodeMode mode = HardwareDecodeMode::D3D11VA;
    bool enable_nvidia_optimizations = false;
    int preferred_gpu_index = 0;  // For multi-GPU systems
    bool allow_software_fallback = true;
};

struct FrameExtractionRequest {
    int frame_number;
    double timestamp;
    int priority;  // Higher = more urgent
    std::chrono::steady_clock::time_point requested_at;

    bool operator<(const FrameExtractionRequest& other) const {
        return priority < other.priority;  // Higher priority first
    }
};

struct ExtractionBatch {
    std::vector<FrameExtractionRequest> frames;
    bool is_sequential = true;  // Optimization hint
    double start_timestamp = 0.0;
    double end_timestamp = 0.0;

    ExtractionBatch() = default;
    ExtractionBatch(std::vector<FrameExtractionRequest> f, bool seq = true)
        : frames(std::move(f)), is_sequential(seq) {
        if (!frames.empty()) {
            start_timestamp = frames.front().timestamp;
            end_timestamp = frames.back().timestamp;
        }
    }
};

struct ExtractionResult {
    bool success = false;
    int frame_number = -1;
    double timestamp = 0.0;
    GLuint texture_id = 0;
    int width = 0;
    int height = 0;
    size_t memory_bytes = 0;
    std::chrono::steady_clock::time_point completed_at;
    std::string error_message;
    std::vector<uint8_t> pixel_data;  // Raw pixel data for texture creation on main thread (format depends on pipeline mode)
    bool from_native_image = false;  // True if extracted from native TIFF/PNG/JPEG loader (not FFmpeg)
};

class MediaBackgroundExtractor {
public:
    struct ExtractorConfig {
        HardwareDecodeConfig hw_config;
        int max_batch_size = 8;                     // Frames per batch
        int max_concurrent_batches = 4;             // Parallel batch processing
        bool pause_during_playback = true;          // Don't steal resources during playback
        int max_queue_size = 200;                  // Safety limit for request queue
        int texture_pool_size = 50;                // Pre-allocated textures
        PipelineMode pipeline_mode = PipelineMode::NORMAL;
    };

    explicit MediaBackgroundExtractor(FrameCache* parent_cache, const ExtractorConfig& config = ExtractorConfig{});
    ~MediaBackgroundExtractor();

    // Lifecycle
    bool Initialize(const std::string& video_path, const VideoMetadata* metadata = nullptr);
    void Shutdown();
    bool IsInitialized() const { return initialized.load(); }

    // Control
    void StartBackgroundExtraction();
    void StopBackgroundExtraction();
    void PauseExtraction();
    void ResumeExtraction();

    // Main thread processing (call from main render loop)
    void ProcessCompletedFrames();

    // Timeline interaction
    void NotifyPlaybackState(bool is_playing);           // Pause/resume based on playback
    void SetPlayheadPosition(double timestamp);          // Update priority calculations

    // Request management
    void RequestFrame(int frame_number, double timestamp, int priority = 0);
    void RequestFrameRange(int start_frame, int end_frame, int base_priority = 0);
    void ClearPendingRequests();  // Cancel all queued work
    void ForceWindowRefresh();     // Force refresh window around current playhead

    // Video properties
    double GetDuration() const { return duration; }
    int GetVideoWidth() const { return video_width; }
    int GetVideoHeight() const { return video_height; }
    double GetFrameRate() const { return frame_rate; }
    int64_t GetStartTime() const { return start_time; }

    // Configuration
    void UpdateHardwareConfig(const HardwareDecodeConfig& config);
    void SetBatchSize(int size) { config.max_batch_size = std::max(1, size); }

    // Metadata-driven conversion (NEW: Conditional 4444 color matrix support)
    void SetConversionStrategy(const ConversionStrategy& strategy);
    bool HasConversionStrategy() const { return has_conversion_strategy; }
    void ClearConversionStrategy() { conversion_strategy.reset(); has_conversion_strategy = false; }
    void UpdateVideoMetadata(const VideoMetadata& metadata);  // NEW: Update metadata after initialization

    // Statistics
    struct ExtractorStats {
        size_t total_frames_extracted = 0;
        size_t total_batches_processed = 0;
        size_t failed_extractions = 0;
        double average_extraction_time_ms = 0.0;
        double frames_per_second = 0.0;
        size_t pending_requests = 0;
        std::string current_hardware_decoder;
        bool is_hardware_accelerated = false;
    };
    ExtractorStats GetStats() const;

    // Cache visualization for timeline
    struct CacheSegment {
        double start_time;
        double end_time;
        enum Type { BACKGROUND_CACHED } type;
    };
    std::vector<CacheSegment> GetCacheSegments() const;

    // Cache management for evictions
    void RemoveFrameFromTracking(int frame_number);

    // Hardware enumeration (for settings UI)
    struct GPUInfo {
        int index;
        std::string name;
        bool supports_d3d11va;
        bool supports_nvdec;
        bool is_nvidia;
    };
    static std::vector<GPUInfo> EnumerateGPUs();

private:
    ExtractorConfig config;
    FrameCache* parent_cache;

    // FFmpeg context
    AVFormatContext* format_context = nullptr;
    AVCodecContext* codec_context = nullptr;
    AVBufferRef* hw_device_ctx = nullptr;
    SwsContext* sws_context = nullptr;
    int video_stream_index = -1;

    // Video properties
    std::string video_path;
    std::atomic<double> duration{0.0};
    std::atomic<int> video_width{0};
    std::atomic<int> video_height{0};
    std::atomic<double> frame_rate{30.0};
    std::atomic<int64_t> start_time{0};
    std::atomic<bool> initialized{false};

    // Metadata-driven conversion
    std::unique_ptr<ConversionStrategy> conversion_strategy;
    bool has_conversion_strategy = false;

    // Threading
    std::vector<std::thread> worker_threads;
    std::atomic<bool> shutdown_requested{false};
    std::atomic<ExtractorState> current_state{ExtractorState::STOPPED};

    // Request queue
    std::priority_queue<FrameExtractionRequest> request_queue;
    std::set<int> requested_frames;             // Simple duplicate prevention
    std::set<int> extracted_frames;             // Successfully extracted frames for timeline visualization
    mutable std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::condition_variable worker_cv;

    // Results queue (for main thread processing)
    std::queue<ExtractionResult> completed_results;
    mutable std::mutex results_mutex;

    // Timeline state
    std::atomic<double> current_playhead_position{0.0};

    // Texture pool for efficient batching
    std::vector<GLuint> texture_pool;
    std::queue<GLuint> available_textures;
    std::mutex texture_pool_mutex;

    // Statistics
    mutable std::mutex stats_mutex;
    ExtractorStats stats;
    std::chrono::steady_clock::time_point extraction_start_time;

    // Hardware decode state
    HardwareDecodeMode current_hw_mode = HardwareDecodeMode::SOFTWARE_ONLY;
    std::string current_hw_decoder_name;

    // Thread-safe FFmpeg context per worker
    struct WorkerContext {
        AVFormatContext* format_context = nullptr;
        AVCodecContext* codec_context = nullptr;
        AVBufferRef* hw_device_ctx = nullptr;
        SwsContext* sws_context = nullptr;
        int video_stream_index = -1;
        bool initialized = false;

        ~WorkerContext() { Cleanup(); }
        void Cleanup();
        bool Initialize(const std::string& video_path, const HardwareDecodeConfig& hw_config);
    };

    // Internal methods
    void WorkerThread();
    ExtractionBatch BuildNextBatch();
    std::vector<ExtractionResult> ProcessBatch(const ExtractionBatch& batch, WorkerContext& worker_ctx);

    // State management
    void SetState(ExtractorState new_state);
    bool ShouldExtract() const;

    // Hardware decode setup
    bool SetupHardwareDecode();
    bool InitializeD3D11VA();
    bool InitializeNVDEC();
    void CleanupHardwareContext();

    // Frame extraction
    ExtractionResult ExtractSingleFrame(const FrameExtractionRequest& request, AVFrame* frame, WorkerContext& worker_ctx);
    bool DecodeFrameAtTimestamp(double timestamp, AVFrame* output_frame, WorkerContext& worker_ctx);
    bool ConvertFrameToPixelBuffer(AVFrame* frame, std::vector<uint8_t>& pixel_data, int& width, int& height);
    GLuint CreateTextureFromPixels(const std::vector<uint8_t>& pixel_data, int width, int height);

    // Texture management
    void InitializeTexturePool();
    void DestroyTexturePool();
    GLuint AcquireTexture();
    void ReleaseTexture(GLuint texture_id);

    // Priority calculation
    int CalculateFramePriority(int frame_number, double timestamp) const;

    // Utility
    void UpdateStats(const ExtractionResult& result);
    void LogHardwareInfo() const;
    bool IsFrameAlreadyCached(int frame_number) const;
    bool CanRequestMoreFrames() const;  // Check global RAM limit

    // Note: Sequential caching removed - using window-around-playhead only

    // Smart windowing around playhead
    void RequestWindowAroundPlayhead(double center_timestamp);
    double CalculateWindowSize() const;  // Adaptive based on available memory
};