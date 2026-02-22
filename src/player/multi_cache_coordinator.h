#pragma once

#include "frame_source_interface.h"
#include "direct_exr_cache_source.h"
#include <map>
#include <memory>
#include <mutex>
#include <atomic>
#include <string>
#include <vector>

namespace qcview {

//=============================================================================
// MultiCacheCoordinator
// Manages multiple autonomous frame sources with unified playback speed
//
// Design principle: "Autonomous Caches + Thin Coordinator"
// - Each source (DirectEXRCache) manages its own threads, timing, adaptive speed
// - Coordinator provides:
//   - Source registration/lookup
//   - Unified position updates
//   - Unified playback speed (slowest source wins)
//   - Frame collection for dual-view/playlist modes
//=============================================================================

class MultiCacheCoordinator {
public:
    MultiCacheCoordinator();
    ~MultiCacheCoordinator();

    // Non-copyable
    MultiCacheCoordinator(const MultiCacheCoordinator&) = delete;
    MultiCacheCoordinator& operator=(const MultiCacheCoordinator&) = delete;

    //=========================================================================
    // Source Management
    //=========================================================================

    // Add an image sequence source
    // source_id: Unique identifier (typically the source path)
    // Returns true on success
    bool AddImageSequenceSource(
        const std::string& source_id,
        const std::vector<std::string>& files,
        const std::string& layer,
        double fps,
        PipelineMode pipeline_mode,
        int read_ahead_frames = 72,
        int read_behind_frames = 12,
        size_t thread_count = 16,
        int start_frame = 0
    );

    // Remove a source (shuts down its cache)
    void RemoveSource(const std::string& source_id);

    // Remove all sources
    void ClearAllSources();

    // Check if source exists
    bool HasSource(const std::string& source_id) const;

    // Get source (returns nullptr if not found)
    IFrameSource* GetSource(const std::string& source_id);
    const IFrameSource* GetSource(const std::string& source_id) const;

    // Get all source IDs
    std::vector<std::string> GetSourceIds() const;

    // Get number of sources
    size_t GetSourceCount() const;

    //=========================================================================
    // Position & Playback (forwarded to all sources)
    //=========================================================================

    // Update position on all sources (timestamp in seconds)
    void UpdatePosition(double timestamp);

    // Set playback state on all sources
    void SetPlaying(bool playing);

    // Set loop range on all sources
    void SetLoopRange(int in_frame, int out_frame);
    void ClearLoopRange();
    void SetLooping(bool enabled);

    // Reset playback state on all sources (call on seek, pause)
    void ResetPlaybackState();

    //=========================================================================
    // Frame Access
    //=========================================================================

    // Get frame from specific source
    // Returns GL texture ID (0 if not available)
    GLuint GetFrame(const std::string& source_id, int frame,
                    int& width, int& height, bool& exact);

    // Check if frame is cached in source
    bool IsFrameCached(const std::string& source_id, int frame) const;

    // Process pending texture operations on all sources
    // Must call from main thread with GL context
    void ProcessPendingOperations();

    //=========================================================================
    // Buffer Health (for statistics)
    //=========================================================================

    // Get combined buffer health (aggregated across all sources)
    BufferHealth GetCombinedBufferHealth() const;

    //=========================================================================
    // Playlist Support (Simple Mode)
    //=========================================================================

    // Set active source for playlist mode
    // Only the active source contributes to playback speed
    // Inactive sources can be shut down to save resources
    void SetActiveSource(const std::string& source_id);
    std::string GetActiveSourceId() const;

    // Swap to new source at clip boundary
    // 1. Creates new source for incoming clip
    // 2. Shuts down old source
    // Returns true on success
    bool SwapActiveSource(
        const std::string& new_source_id,
        const std::vector<std::string>& files,
        const std::string& layer,
        double fps,
        PipelineMode pipeline_mode
    );

    //=========================================================================
    // Dual-View Support
    //=========================================================================

    // Common identifiers for dual-view sources
    static constexpr const char* LEFT_SOURCE_ID = "dual_view_left";
    static constexpr const char* RIGHT_SOURCE_ID = "dual_view_right";

    // Convenience methods for dual-view
    bool AddLeftSource(const std::vector<std::string>& files,
                       const std::string& layer, double fps,
                       PipelineMode pipeline_mode);

    bool AddRightSource(const std::vector<std::string>& files,
                        const std::string& layer, double fps,
                        PipelineMode pipeline_mode);

    GLuint GetLeftFrame(int frame, int& width, int& height, bool& exact);
    GLuint GetRightFrame(int frame, int& width, int& height, bool& exact);

    bool HasLeftSource() const { return HasSource(LEFT_SOURCE_ID); }
    bool HasRightSource() const { return HasSource(RIGHT_SOURCE_ID); }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<IFrameSource>> sources_;

    // Playlist mode: which source is active
    std::string active_source_id_;

    // Cached unified speed (for performance - avoid recalculating every frame)
};

} // namespace qcview
