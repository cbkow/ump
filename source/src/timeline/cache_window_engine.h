#pragma once

#include <vector>
#include <set>
#include <atomic>
#include <mutex>

namespace ump {

//=============================================================================
// CacheWindowEngine - Central engine for demand-driven circular caching
//
// This class owns ALL circular math in one place. Instead of making components
// circular-aware (which leads to bugs when circular logic desyncs), this engine:
// 1. Provides prioritized frame lists to consumers
// 2. Handles all boundary wrapping
// 3. Calculates circular distances
//
// Components (decoder, eviction, cache fill) stay simple/linear - they just
// follow orders from this engine.
//
// Key principle: Always circular. No "loop mode" flag. Boundaries are either
// [0, total_frames-1] or [in_point, out_point] when set.
//=============================================================================

class CacheWindowEngine {
public:
    CacheWindowEngine();
    ~CacheWindowEngine() = default;

    // Prevent copying
    CacheWindowEngine(const CacheWindowEngine&) = delete;
    CacheWindowEngine& operator=(const CacheWindowEngine&) = delete;

    //=========================================================================
    // Configuration
    //=========================================================================

    // Set total frame count (must be called before using the engine)
    void SetTotalFrames(int total_frames);

    // Set custom loop boundaries (in/out points)
    // When set, cache window wraps within [start, end] instead of [0, total-1]
    void SetBoundaries(int start, int end);

    // Clear custom boundaries (use [0, total-1])
    void ClearBoundaries();

    // Set cache window size (frames behind and ahead of playhead)
    void SetWindow(int behind, int ahead);

    // Set linear mode (no wrap-around at boundaries)
    // When true: frames outside boundaries are clamped/skipped instead of wrapped
    // Use for MULTI_TRACK/DUAL_VIEW where clips have defined positions
    void SetLinearMode(bool linear) { linear_mode_ = linear; }
    bool IsLinearMode() const { return linear_mode_; }

    //=========================================================================
    // Playhead Management
    //=========================================================================

    // Update playhead position
    void UpdatePlayhead(int frame);

    // Get current playhead position
    int GetPlayhead() const { return playhead_.load(); }

    //=========================================================================
    // Frame Window Queries - THE CORE API
    //=========================================================================

    // Get all frames in the cache window, sorted by priority
    // Priority order:
    //   1. Current frame (playhead) - highest priority
    //   2. Frames ahead sorted by distance (1, 2, 3, ... ahead)
    //   3. Frames behind sorted by distance (1, 2, 3, ... behind)
    //
    // Handles wrap-around at boundaries automatically.
    // Example: playhead=145, boundary=[0,146], ahead=10, behind=5
    //   Returns: [145, 146, 0, 1, 2, 3, 4, 5, 6, 7, 144, 143, 142, 141, 140]
    std::vector<int> GetFrameWindow() const;

    // Get frames in the window as a set (for O(1) membership check)
    std::set<int> GetFrameWindowSet() const;

    // Check if a frame is within the current cache window
    bool IsInWindow(int frame) const;

    //=========================================================================
    // Circular Math Utilities
    //=========================================================================

    // Wrap a frame number into the valid boundary range
    // Always wraps (no clamping) - maintains circular behavior
    int WrapFrame(int frame) const;

    // Calculate circular distance between two frames
    // Returns the shortest distance considering wrap-around
    // Positive = target is ahead, Negative = target is behind
    int CircularDistance(int from, int to) const;

    // Calculate absolute circular distance (always positive)
    int AbsCircularDistance(int from, int to) const;

    //=========================================================================
    // Boundary Queries
    //=========================================================================

    // Get effective boundary start (custom or 0)
    int GetBoundaryStart() const;

    // Get effective boundary end (custom or total_frames-1)
    int GetBoundaryEnd() const;

    // Get boundary length (end - start + 1)
    int GetBoundaryLength() const;

    // Check if custom boundaries are set
    bool HasCustomBoundaries() const;

    //=========================================================================
    // Window Size Queries
    //=========================================================================

    int GetReadBehind() const { return read_behind_; }
    int GetReadAhead() const { return read_ahead_; }
    int GetWindowSize() const { return read_behind_ + read_ahead_ + 1; }

private:
    // Configuration
    int total_frames_ = 0;
    int read_behind_ = 12;   // ~0.5s @ 24fps (default)
    int read_ahead_ = 108;   // ~4.5s @ 24fps (default)

    // Custom boundaries (-1 = use default)
    std::atomic<int> boundary_start_{-1};
    std::atomic<int> boundary_end_{-1};

    // Linear mode - when true, don't wrap at boundaries (for multitrack/dual view)
    bool linear_mode_ = false;

    // Current playhead
    std::atomic<int> playhead_{0};

    // Cached boundary length (updated when boundaries change)
    mutable int cached_boundary_length_ = 0;
    mutable bool boundary_length_dirty_ = true;

    // Thread safety
    mutable std::mutex config_mutex_;

    // Helper to update cached boundary length
    void UpdateBoundaryLength() const;
};

} // namespace ump
