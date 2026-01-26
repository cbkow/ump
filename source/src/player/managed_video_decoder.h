#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <deque>
#include <chrono>
#include <set>

#include "video_decoder_interface.h"

namespace ump {

//=============================================================================
// Shuttle State - For FF/RW mode where UI moves faster than decode
//=============================================================================

struct ShuttleState {
    bool active = false;
    int direction = 0;              // -1 = RW, +1 = FF
    int playhead = 0;               // Current UI position (moves freely)
    int last_spawn_position = -1;   // Where we last spawned a decoder

    // The shuttle buffer - holds decoded frames for shuttle display
    struct ShuttleFrame {
        int frame_number = 0;
        std::shared_ptr<PixelData> pixels;
    };
    std::deque<ShuttleFrame> frame_queue;
    mutable std::mutex queue_mutex;

    // Display state
    int last_displayed_frame = -1;
    std::shared_ptr<PixelData> held_frame;  // Hold until better arrives
};

//=============================================================================
// Managed Video Decoder
//
// Wraps StreamingVideoDecoder with spawn-and-abandon lifecycle management.
// When seeking outside the current buffer, instead of waiting for FFmpeg to
// handle the seek (which can take 100ms+ for large frames), we:
//   1. Spawn a fresh decoder at the new position
//   2. Abandon the old decoder (queued for background cleanup)
//   3. Never wait for the old decoder
//
// This provides instant seek responsiveness regardless of frame size.
// The interface matches StreamingVideoDecoder for drop-in replacement.
//
// Usage:
//   auto decoder = std::make_shared<ManagedVideoDecoder>(path);
//   decoder->Initialize();
//   decoder->UpdatePlayhead(frame);  // May spawn new decoder if needed
//   auto pixels = decoder->GetFrame(frame);
//=============================================================================

class ManagedVideoDecoder {
public:
    explicit ManagedVideoDecoder(const std::string& video_path);
    ~ManagedVideoDecoder();

    // Prevent copying
    ManagedVideoDecoder(const ManagedVideoDecoder&) = delete;
    ManagedVideoDecoder& operator=(const ManagedVideoDecoder&) = delete;

    //=========================================================================
    // Lifecycle
    //=========================================================================

    // Initialize decoder and start decode thread
    bool Initialize();

    // Stop decoder and release resources
    void Shutdown();

    // Hard reset: spawn fresh decoder at target frame
    void HardReset(int target_frame);

    // Check if decoder is ready
    bool IsInitialized() const { return initialized_.load(); }

    //=========================================================================
    // Frame Access (delegates to active decoder)
    //=========================================================================

    // Get frame from buffer (instant if buffered)
    std::shared_ptr<PixelData> GetFrame(int frame_number);

    // Get closest frame when exact frame not available
    std::shared_ptr<PixelData> GetClosestFrame(int frame_number, int* actual_frame = nullptr);

    // Check if frame is available in buffer
    bool HasFrame(int frame_number) const;

    //=========================================================================
    // Keyframe Access (delegates to active decoder)
    //=========================================================================

    // Get nearest keyframe at or before target
    std::shared_ptr<PixelData> GetKeyframe(int target_frame, int* actual_keyframe = nullptr);

    // Get frame number of nearest keyframe
    int GetNearestKeyframePosition(int target_frame) const;

    // Check if video is intra-frame codec (every frame is keyframe)
    bool IsIntraFrameCodec() const;

    // Check if keyframe index has been built
    bool HasKeyframeIndex() const;

    //=========================================================================
    // Playhead Management - THE KEY DIFFERENCE
    //=========================================================================

    // Update playhead position
    // May spawn a fresh decoder if target is far from current buffer
    // quality: PREVIEW for fast scrubbing, NORMAL for full quality
    // force_seek: always spawn fresh decoder
    // is_prefetch: when true, treat as prefetch for upcoming clip - don't respawn aggressively
    //              (allows buffer to fill even when not playing, for cross-edit caching)
    void UpdatePlayhead(int frame_number, SeekQuality quality = SeekQuality::NORMAL,
                        bool force_seek = false, bool is_prefetch = false);

    //=========================================================================
    // Demand-Driven Decode API (delegates to active decoder)
    //=========================================================================

    // Set which frames are needed, in priority order
    void SetNeededFrames(const std::vector<int>& frames_by_priority);

    // Get current decode status
    DecodeStatus GetDecodeStatus() const;

    // Evict frames outside the given keep set
    void EvictOutsideWindow(const std::set<int>& keep_frames);

    // Get frames currently in buffer as a set
    std::set<int> GetBufferedFramesSet() const;

    //=========================================================================
    // Buffer Status (delegates to active decoder)
    //=========================================================================

    int GetBufferedAhead() const;
    int GetBufferedBehind() const;
    int GetBufferSize() const;
    void GetBufferedRange(int& start_frame, int& end_frame) const;
    void ClearBuffer();
    bool IsSeekPending() const;
    int GetLastSeekTarget() const;

    //=========================================================================
    // Metadata (delegates to active decoder)
    //=========================================================================

    int GetWidth() const;
    int GetHeight() const;
    double GetFPS() const;
    double GetDuration() const;
    int GetFrameCount() const;
    const std::string& GetPath() const { return video_path_; }

    HWAccelType GetHWAccelType() const;
    bool IsHardwareAccelerated() const;

    //=========================================================================
    // Configuration
    //=========================================================================

    void SetConfig(const StreamingDecoderConfig& config);
    const StreamingDecoderConfig& GetConfig() const { return config_; }

    // Pipeline mode (for bit depth / HDR support)
    void SetPipelineMode(PipelineMode mode);
    PipelineMode GetPipelineMode() const { return pipeline_mode_; }

    // Playback mode - affects spawn behavior
    // When playing: spawn once, let decoder catch up naturally
    // When not playing (scrubbing): spawn for new positions
    void SetPlaybackMode(bool playing);
    bool IsPlaying() const { return is_playing_.load(); }

    //=========================================================================
    // Shuttle Mode - For FF/RW where UI moves faster than decode
    //=========================================================================

    // Enable/disable shuttle mode
    // direction: -1 = rewind, +1 = fast forward
    void SetShuttleMode(bool enabled, int direction);
    bool IsShuttleMode() const { return shuttle_.active; }
    int GetShuttleDirection() const { return shuttle_.direction; }

    // Called every frame during shuttle - updates playhead, returns best frame
    std::shared_ptr<PixelData> UpdateShuttle(int playhead_frame);

    // Exit shuttle - returns frame to snap to, triggers exact frame decode
    int ExitShuttle();

    //=========================================================================
    // Loop Boundaries (stub interface for timeline cache compatibility)
    // The watchdog handles recovery, so these are no-ops
    //=========================================================================

    void SetLoopBoundaries(int /*loop_start*/, int /*loop_end*/) {}
    void ClearLoopBoundaries() {}

    //=========================================================================
    // Stats (for monitoring spawn-and-abandon behavior)
    //=========================================================================

    int GetSpawnCount() const { return spawn_count_.load(); }
    int GetAbandonCount() const { return abandon_count_.load(); }

private:
    //=========================================================================
    // Spawn-and-Abandon Logic
    //=========================================================================

    // Decide whether to spawn a fresh decoder or use existing one
    // is_prefetch: when true, treat as prefetch/playback (less aggressive spawning)
    bool ShouldSpawnNewDecoder(int target_frame, bool is_prefetch = false) const;

    // Spawn a fresh decoder at target frame, abandon the current one
    void SpawnFreshDecoder(int target_frame);

    //=========================================================================
    // Shuttle Mode Helpers
    //=========================================================================

    // Harvest frames from decoder into shuttle queue
    void HarvestDecoderFrames();

    // Remove frames too far from playhead in wrong direction
    void PurgeStaleFrames();

    // Find best frame to display during shuttle
    std::shared_ptr<PixelData> GetBestShuttleFrame();

    // Check if frame is already in shuttle queue
    bool IsInShuttleQueue(int frame_number) const;

    // Sort shuttle queue by frame number
    void SortShuttleQueue();

    // Spawn decoder for next chunk (rewind pre-decode)
    void MaybeSpawnNextChunk();

    // Get chunk size for shuttle spawning (~2 seconds)
    int GetChunkSize() const;

    //=========================================================================
    // State
    //=========================================================================

    std::string video_path_;
    StreamingDecoderConfig config_;
    std::atomic<bool> initialized_{false};
    PipelineMode pipeline_mode_ = PipelineMode::NORMAL;

    // The active decoder - all reads go here
    // Uses IVideoDecoder interface to support FFmpeg and GStreamer backends
    std::shared_ptr<IVideoDecoder> active_;
    mutable std::mutex active_mutex_;

    // Cached metadata (from first successful init)
    int width_ = 0;
    int height_ = 0;
    double fps_ = 0.0;
    double duration_ = 0.0;
    int frame_count_ = 0;
    bool metadata_cached_ = false;

    // Stats
    std::atomic<int> spawn_count_{0};
    std::atomic<int> abandon_count_{0};

    // Last known playhead for spawn decision
    std::atomic<int> last_playhead_{0};

    // Track what frame we last spawned for - don't re-spawn for same target
    std::atomic<int> last_spawn_target_{-1};

    // Playback mode - when true, don't respawn on empty buffer (let decoder catch up)
    std::atomic<bool> is_playing_{false};

    // Shuttle mode state
    ShuttleState shuttle_;

    //=========================================================================
    // Watchdog Recovery
    //
    // Detects when the decoder is "stuck" (playing but not producing frames)
    // and forces a hard reset to recover. This is a safety net for cases
    // where the decoder jams (e.g., after loop wrap on some codecs).
    //=========================================================================

    // Last time we successfully got a frame (for stuck detection)
    mutable std::chrono::steady_clock::time_point last_frame_time_{std::chrono::steady_clock::now()};

    // How long without frames before we consider decoder stuck (ms)
    static constexpr int WATCHDOG_THRESHOLD_MS = 500;

    // Check if decoder appears stuck, trigger recovery if needed
    // Returns true if recovery was triggered
    bool CheckWatchdogAndRecover();
};

} // namespace ump
