#pragma once

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <unordered_set>
#include <future>

#include "image_loader_interface.h"
#include "pipeline_mode.h"
#include "streaming_video_decoder.h"  // For StreamingDecoderConfig and SeekQuality

namespace ump {

//=============================================================================
// Image Sequence Decoder
//
// Provides real-time image sequence frame access through I/O with buffering.
// Mirrors StreamingVideoDecoder's ring buffer architecture for consistent
// behavior when mixing video and image sequence clips in timelines.
//
// Key differences from StreamingVideoDecoder:
// - No FFmpeg/hardware decode - uses IImageLoader for file I/O
// - No keyframe handling needed (every frame is independent)
// - Seeking is instant (just repositions I/O thread)
// - Supports EXR layers and pipeline modes
//=============================================================================

class ImageSequenceDecoder {
public:
    // Constructor takes directory and pattern (e.g., "C:/shots", "shot_%04d.exr")
    ImageSequenceDecoder(const std::string& directory, const std::string& pattern);
    ~ImageSequenceDecoder();

    // Prevent copying
    ImageSequenceDecoder(const ImageSequenceDecoder&) = delete;
    ImageSequenceDecoder& operator=(const ImageSequenceDecoder&) = delete;

    //=========================================================================
    // Lifecycle
    //=========================================================================

    // Initialize decoder and start I/O thread
    // start_frame/end_frame are the file frame numbers (e.g., 1001-1100)
    // fps is used for timing calculations
    // Returns true on success
    bool Initialize(int start_frame, int end_frame, double fps,
                    PipelineMode mode = PipelineMode::NORMAL,
                    const std::string& exr_layer = "");

    // Stop I/O thread and release resources
    void Shutdown();

    // Hard reset: clear buffer and restart I/O from target frame
    // Thread-safe: can be called from any thread
    void HardReset(int target_frame);

    // Check if decoder is ready
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Frame Access (same interface as StreamingVideoDecoder)
    //=========================================================================

    // Get frame from buffer (instant if buffered)
    // Returns nullptr if frame not yet loaded
    // Thread-safe
    std::shared_ptr<PixelData> GetFrame(int frame_number);

    // Get closest frame from buffer when exact frame not available
    // Useful during scrubbing to show something rather than nothing
    // Returns nullptr if buffer is empty
    // Thread-safe
    std::shared_ptr<PixelData> GetClosestFrame(int frame_number, int* actual_frame = nullptr);

    // Check if frame is available in buffer
    // Thread-safe
    bool HasFrame(int frame_number) const;

    //=========================================================================
    // Keyframe Access (for API compatibility with StreamingVideoDecoder)
    // For image sequences, every frame is a "keyframe" (independent)
    //=========================================================================

    // Get frame (same as GetFrame for sequences - all frames are independent)
    std::shared_ptr<PixelData> GetKeyframe(int target_frame, int* actual_keyframe = nullptr);

    // Get the frame number (returns target_frame - all frames are keyframes)
    int GetNearestKeyframePosition(int target_frame) const { return target_frame; }

    // Always true for image sequences (every frame is independent)
    bool IsIntraFrameCodec() const { return true; }

    // No keyframe index needed
    bool HasKeyframeIndex() const { return true; }

    //=========================================================================
    // Playhead Management
    //=========================================================================

    // Update playhead position
    // Triggers buffer repositioning if frame is outside buffer range
    // quality: PREVIEW for fast scrubbing, NORMAL for full quality (same for sequences)
    // force_seek: bypass tolerance checks (for refinement after scrub)
    // Thread-safe
    void UpdatePlayhead(int frame_number, SeekQuality quality = SeekQuality::NORMAL,
                        bool force_seek = false);

    //=========================================================================
    // Buffer Status (for UI/debugging)
    //=========================================================================

    // Get number of frames buffered ahead of playhead
    int GetBufferedAhead() const;

    // Get number of frames buffered behind playhead
    int GetBufferedBehind() const;

    // Get total buffer size
    int GetBufferSize() const;

    // Get range of buffered frames [start, end]
    void GetBufferedRange(int& start_frame, int& end_frame) const;

    // Clear the frame buffer (free memory for inactive decoders)
    // Thread-safe
    void ClearBuffer();

    // Check if a seek/reposition is in progress
    bool IsSeekPending() const { return seek_requested_.load(); }

    // Get the last seek target frame
    int GetLastSeekTarget() const { return seek_target_frame_; }

    //=========================================================================
    // Metadata
    //=========================================================================

    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }
    double GetFPS() const { return fps_; }
    double GetDuration() const { return duration_; }
    int GetFrameCount() const { return frame_count_; }
    const std::string& GetDirectory() const { return directory_; }
    const std::string& GetPattern() const { return pattern_; }
    int GetStartFrame() const { return start_frame_; }
    int GetEndFrame() const { return end_frame_; }

    //=========================================================================
    // Configuration
    //=========================================================================

    void SetConfig(const StreamingDecoderConfig& config);
    const StreamingDecoderConfig& GetConfig() const { return config_; }

private:
    //=========================================================================
    // I/O Thread
    //=========================================================================

    // Main I/O loop (runs in io_thread_)
    void IOThread();

    // Load a single frame from disk
    std::shared_ptr<PixelData> LoadFrame(int frame_number);

    // Get the file path for a frame number
    std::string GetFramePath(int frame_number) const;

    //=========================================================================
    // Ring Buffer Management (mirrors StreamingVideoDecoder)
    //=========================================================================

    struct BufferedFrame {
        int frame_number = -1;
        std::shared_ptr<PixelData> pixels;
    };

    // Add frame to buffer
    void AddToBuffer(int frame_number, std::shared_ptr<PixelData> pixels);

    // Find frame in buffer
    std::shared_ptr<PixelData> FindInBuffer(int frame_number) const;

    // Check if buffer needs more frames
    bool NeedsMoreFrames() const;

    // Get next frame to load (prioritizes ahead of playhead)
    int GetNextFrameToLoad();

    // Evict frames outside the current window
    void EvictOutsideWindow();

    //=========================================================================
    // State
    //=========================================================================

    bool initialized_ = false;
    StreamingDecoderConfig config_;

    // Sequence metadata
    std::string directory_;
    std::string pattern_;
    int start_frame_ = 1;      // First file frame number (e.g., 1001)
    int end_frame_ = 1;        // Last file frame number (e.g., 1100)
    int frame_count_ = 0;      // Total frames in sequence
    double fps_ = 24.0;
    double duration_ = 0.0;
    int width_ = 0;
    int height_ = 0;

    // Loader
    std::unique_ptr<IImageLoader> loader_;
    PipelineMode pipeline_mode_ = PipelineMode::NORMAL;
    std::string exr_layer_;

    //=========================================================================
    // Ring Buffer (identical structure to StreamingVideoDecoder)
    //=========================================================================

    std::deque<BufferedFrame> ring_buffer_;
    std::unordered_set<int> buffer_frame_set_;  // O(1) lookup
    mutable std::mutex buffer_mutex_;

    // Fast O(1) buffer tracking
    std::atomic<int> buffer_ahead_count_{0};  // Frames ahead of playhead
    std::atomic<int> buffer_size_{0};         // Total frames in buffer

    // Current playhead position (set by UpdatePlayhead)
    std::atomic<int> playhead_frame_{0};

    // Current I/O position (where we're loading next)
    int io_frame_ = 0;

    //=========================================================================
    // Threading (with parallel async loading)
    //=========================================================================

    std::thread io_thread_;
    std::atomic<bool> running_{false};

    // Seek/reposition coordination
    std::mutex io_mutex_;
    std::condition_variable io_cv_;
    std::atomic<bool> seek_requested_{false};
    int seek_target_frame_ = 0;

    // Track which frames are currently being loaded (to avoid duplicates)
    std::unordered_set<int> frames_in_flight_;
    std::mutex in_flight_mutex_;

    // Parallel async loading (each task creates its own loader for thread safety)
    struct AsyncLoadRequest {
        int frame;
        std::future<std::shared_ptr<PixelData>> future;
    };
    std::vector<AsyncLoadRequest> async_requests_;
    static constexpr size_t MAX_CONCURRENT_LOADS = 8;

    // Helper to create loader and load a single frame (thread-safe, no shared state)
    std::shared_ptr<PixelData> LoadFrameWithFreshLoader(int frame_number);
};

} // namespace ump
