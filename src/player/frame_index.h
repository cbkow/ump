#pragma once

#include <cstdint>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <string>

// Forward declarations (avoid pulling in FFmpeg headers)
struct AVFormatContext;

namespace qcview {

//=============================================================================
// FrameIndex
//
// Exact frame↔PTS mapping for video streams. Replaces the fragile
// round(pts * fps) arithmetic with a proper lookup table built from
// container metadata (AVStream::index_entries) or packet scanning.
//
// Three-tier construction:
//   Tier 1: Project cache (instant, keyframes only)
//   Tier 2: AVStream index_entries (microseconds, full table)
//   Tier 3: Async packet scan fallback (for containers without index)
//
// Thread safety: all public methods are thread-safe. The decode thread
// calls RegisterDecodedFrame() while the main thread queries.
//=============================================================================

struct FrameEntry {
    int64_t pts;        // In stream timebase units
    int64_t dts;        // In stream timebase units (AV_NOPTS_VALUE if unknown)
    bool is_keyframe;
};

class FrameIndex {
public:
    FrameIndex();
    ~FrameIndex();

    // Non-copyable, non-movable (owns a thread)
    FrameIndex(const FrameIndex&) = delete;
    FrameIndex& operator=(const FrameIndex&) = delete;

    //=========================================================================
    // Construction — call ONE tier, then optionally upgrade with a higher tier.
    // Tier 2 always supersedes Tier 1 when it succeeds.
    //=========================================================================

    // Tier 1: Load keyframe-only index from project cache.
    // Provides NearestKeyframeBefore() and TotalFrames() instantly.
    // complete_ remains false — per-frame queries fall back to arithmetic.
    void LoadFromCache(const std::vector<int64_t>& keyframe_pts,
                       int total_frames,
                       int timebase_num, int timebase_den,
                       double fps, bool is_all_intra);

    // Tier 2: Build full index from AVStream::index_entries (in-memory, no I/O).
    // Returns true if index_entries had data, false if fallback needed.
    bool BuildFromStreamIndex(AVFormatContext* fmt_ctx, int stream_index,
                              double fps);

    // Tier 3: Async packet scan for containers without index_entries.
    // Opens a separate AVFormatContext (decoder's is in use).
    // on_complete fires on the scan thread when done.
    void BuildFromPacketScanAsync(const std::string& file_path,
                                  int stream_index, double fps,
                                  std::function<void()> on_complete = nullptr);

    //=========================================================================
    // Runtime registration — for incomplete index path.
    // Called from decode thread when a frame is decoded.
    //=========================================================================

    void RegisterDecodedFrame(int64_t pts, int64_t dts, bool is_keyframe);

    //=========================================================================
    // Queries — all thread-safe
    //=========================================================================

    // Exact frame number for a given PTS (binary search on sorted entries).
    // Returns -1 if PTS not found and index is incomplete.
    int FrameForPTS(int64_t pts) const;

    // Exact PTS for a given frame number.
    // Falls back to arithmetic estimate if index is incomplete or frame out of range.
    int64_t PTSForFrame(int frame) const;

    // Frame number of nearest keyframe at or before target.
    // Returns target itself for all-intra codecs.
    int NearestKeyframeBefore(int frame) const;

    // PTS of nearest keyframe at or before target frame.
    // Used as the timestamp argument to av_seek_frame.
    int64_t KeyframePTSBefore(int frame) const;

    // Number of frames between keyframe_frame and target_frame (inclusive).
    // Used to know how many frames to decode/skip after seeking to a keyframe.
    int FramesBetween(int keyframe_frame, int target_frame) const;

    int TotalFrames() const;
    bool IsComplete() const { return complete_.load(std::memory_order_acquire); }
    bool IsAllIntra() const { return is_all_intra_; }

    //=========================================================================
    // Serialization — for project cache persistence
    //=========================================================================

    // Returns sorted keyframe PTS values (stream timebase units).
    std::vector<int64_t> GetKeyframePTSList() const;

    // Returns keyframe frame numbers (for backward compat with existing API).
    std::vector<int> GetKeyframePositions() const;

    // Stream timebase accessors
    int TimebaseNum() const { return timebase_num_; }
    int TimebaseDen() const { return timebase_den_; }

    //=========================================================================
    // Lifecycle
    //=========================================================================

    // Cancel async packet scan (call before destroying decoder).
    void CancelAsyncScan();

private:
    // Arithmetic fallback: estimate PTS from frame number (or vice versa)
    int64_t EstimatePTSForFrame(int frame) const;
    int EstimateFrameForPTS(int64_t pts) const;

    // Rebuild keyframe_frames_ from entries_. Caller must hold mutex_.
    void RebuildKeyframeList();

    mutable std::mutex mutex_;

    // Core data: sorted by PTS (presentation order). Vector index = frame number.
    std::vector<FrameEntry> entries_;

    // Keyframe subset: sorted by frame number for binary search
    std::vector<int> keyframe_frames_;

    // Metadata
    int total_frames_ = 0;
    int timebase_num_ = 0;
    int timebase_den_ = 1;
    double fps_ = 0.0;
    bool is_all_intra_ = false;
    std::atomic<bool> complete_{false};

    // Async packet scan
    std::thread scan_thread_;
    std::atomic<bool> cancel_scan_{false};
};

} // namespace qcview
