#pragma once

#ifdef _WIN32

#include <string>
#include <memory>
#include <mutex>
#include <vector>
#include <d3d11_1.h>
#include <glad/gl.h>

namespace ump {

// Forward declarations
class D3D11VideoDecoder;
class DirectEXRCache;
class IImageLoader;
struct CacheSegment;

//=============================================================================
// PlaylistSingleDecoder
//
// EDL-style single decoder for playlist playback.
//
// Key design principles:
// - **Single decoder instance** for the entire playlist
// - **No prefetch/pre-warm** of upcoming segments
// - **Clean flush + seek** when switching clips
// - **Brief pause at transitions** (acceptable tradeoff for stability)
//
// This approach eliminates D3D11 contention issues that cause crashes
// when multiple decoders are initialized/used simultaneously.
//
// Flow on clip transition:
//   1. Pause playback
//   2. decoder_.Shutdown()
//   3. decoder_.SetVideoPath(new_clip)
//   4. decoder_.Initialize()
//   5. decoder_.Seek(clip_start_frame)
//   6. Resume playback
//=============================================================================

class PlaylistSingleDecoder {
public:
    PlaylistSingleDecoder();
    ~PlaylistSingleDecoder();

    // Non-copyable
    PlaylistSingleDecoder(const PlaylistSingleDecoder&) = delete;
    PlaylistSingleDecoder& operator=(const PlaylistSingleDecoder&) = delete;

    //=========================================================================
    // Lifecycle
    //=========================================================================

    // Initialize with D3D11 device
    // Must be called before any other methods
    bool Initialize(ID3D11Device* device);

    // Shutdown and release all resources
    void Shutdown();

    // Check if initialized
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Source Management
    //=========================================================================

    // Switch to a different source file (flush + reinit)
    // Returns false if switch failed (caller should show last good frame or black)
    // If source_path matches current source and decoder is valid, returns immediately
    bool SwitchSource(const std::string& source_path);

    // Get current source path
    const std::string& GetCurrentSource() const { return current_source_; }

    // Check if a source switch is currently in progress
    bool IsSwitching() const { return switching_.load(); }

    //=========================================================================
    // Frame Access - Delegates to internal decoder
    //=========================================================================

    // Update playhead position (triggers decode)
    void UpdatePlayhead(int source_frame);

    // Check if frame is available
    bool HasFrame(int source_frame) const;

    // Get frame as OpenGL texture
    // Returns 0 if frame not available
    GLuint GetFrameAsGLTexture(int source_frame);

    // Get frame as D3D11 SRV
    // Returns nullptr if frame not available
    ID3D11ShaderResourceView* GetFrameAsD3D11SRV(int source_frame);

    //=========================================================================
    // Metadata - Delegates to internal decoder
    //=========================================================================

    int GetWidth() const;
    int GetHeight() const;
    double GetFPS() const;
    int GetFrameCount() const;
    bool IsHDR() const;

    //=========================================================================
    // Internal Decoder Access (for advanced use)
    //=========================================================================

    D3D11VideoDecoder* GetDecoder() const { return decoder_.get(); }

    //=========================================================================
    // Image Sequence Support
    //=========================================================================

    // Check if current source is an image sequence
    bool IsImageSequence() const { return is_image_sequence_; }

    // Pass playback stride to internal DirectEXRCache
    void SetPlaybackStride(int stride);

    // Set sequence metadata for proper initialization
    // Must be called before SwitchSource for image sequences
    // timeline_offset_seconds: where this clip starts in the timeline (for cache bar positioning)
    void SetSequenceMetadata(const std::string& directory, const std::string& pattern,
                             int start_frame, int end_frame, double fps,
                             const std::string& exr_layer = "",
                             double timeline_offset_seconds = 0.0);

    //=========================================================================
    // Playback Control (for adaptive speed and buffer management)
    //=========================================================================


    // Check if we're in warmup period after source switch
    bool IsInWarmupPeriod() const;

    // Get cache progress (0.0 to 1.0) for progress bar display
    double GetCacheProgress() const;

    // Get number of frames currently cached
    int GetCachedFrameCount() const;

    // Get total frames in current source
    int GetTotalFrameCount() const;

    // Get cache segments for progress bar visualization
    std::vector<CacheSegment> GetCacheSegments() const;

private:
    //=========================================================================
    // State
    //=========================================================================

    bool initialized_ = false;
    std::string current_source_;
    std::unique_ptr<D3D11VideoDecoder> decoder_;

    // Image sequence support
    bool is_image_sequence_ = false;
    std::unique_ptr<DirectEXRCache> image_cache_;
    double sequence_fps_ = 24.0;
    double timeline_offset_seconds_ = 0.0;  // Where this clip starts in the timeline
    int source_frame_offset_ = 0;  // First source frame number (for converting to 0-indexed)

    // Sequence metadata (set via SetSequenceMetadata before SwitchSource)
    std::string pending_seq_directory_;
    std::string pending_seq_pattern_;
    int pending_seq_start_frame_ = 0;
    int pending_seq_end_frame_ = 0;
    std::string pending_seq_exr_layer_;

    // D3D11 device reference (not owned)
    ID3D11Device* device_ = nullptr;

    // Thread safety for source switching
    mutable std::mutex switch_mutex_;
    std::atomic<bool> switching_{false};

    // Source switch timing - for warmup grace period
    std::chrono::steady_clock::time_point last_source_switch_time_;
    static constexpr int kWarmupGraceMs = 500;  // 500ms grace period after source switch

    // Helper to detect if a path is an image sequence
    bool IsSequencePath(const std::string& path) const;

    // Helper to initialize image sequence cache
    bool InitializeImageSequence(const std::string& source_path);
};

} // namespace ump

#endif // _WIN32
