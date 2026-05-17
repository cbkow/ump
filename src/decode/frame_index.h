// FrameIndex — Phase 1.8.3 (formula) + 1.8.7 (async packet scan).
//
// First-class PTS ↔ frame-number mapping for a single video stream.
// Owned by VideoDecoder; queryable from any thread.
//
// Two-tier construction:
//
//   Tier 1 — formula:        Constructed at open() from time_base +
//                            frame_rate. Exact for constant-rate
//                            intra codecs; approximate for inter or
//                            variable-rate footage. Always available.
//
//   Tier 2 — async scan:     Spawned via startScan() after open()
//                            succeeds. Walks every video packet on a
//                            background thread (own AVFormatContext)
//                            and builds an exact (pts, isKeyframe)
//                            table sorted by display order. While
//                            building, lookups still use the formula.
//                            Once Ready, lookups consult the table.
//
// The "tier 3" terminology in old QCView referred to a third level —
// a packet scan as a fallback when neither cache nor stream
// index_entries cover the file. We collapsed that to a single async
// scan because modern containers reliably surface index entries via
// FFmpeg's stream metadata, making a separate stream-entries-only
// tier redundant.
//
// PIMPL because the scan owns a thread + table; FrameIndex is move-
// only. VideoDecoder still uses value semantics (move-assigns a
// fresh FrameIndex on close), which works because std::unique_ptr's
// move-assign destroys the previous Impl, which joins its thread.

#pragma once

#include <QString>
#include <cstdint>
#include <memory>

namespace qcv {

class FrameIndex
{
public:
    FrameIndex();
    FrameIndex(int timeBaseNum, int timeBaseDen,
               int frameRateNum, int frameRateDen,
               int totalFrames, bool isIntraOnly);
    ~FrameIndex();

    FrameIndex(const FrameIndex &) = delete;
    FrameIndex &operator=(const FrameIndex &) = delete;

    FrameIndex(FrameIndex &&other) noexcept;
    FrameIndex &operator=(FrameIndex &&other) noexcept;

    bool isValid() const;

    // Lookup methods. When scanState() == Ready, the exact table is
    // used; otherwise the formula path. Callers don't need to branch.
    int64_t ptsForFrame(int frameNo) const;
    int     frameForPts(int64_t pts) const;
    int     frameForTime(double seconds) const;

    int     totalFrames() const;
    bool    isIntraOnly() const;
    int     timeBaseNum() const;
    int     timeBaseDen() const;
    int     frameRateNum() const;
    int     frameRateDen() const;
    double  fps() const;

    // Tier 2 — async packet scan.
    enum class ScanState : int {
        NotStarted = 0,
        Building   = 1,
        Ready      = 2,
        Failed     = 3,
    };

    // Begin scanning the given file. Spawns a worker thread that
    // opens its own AVFormatContext and walks every video packet to
    // build an exact (pts, isKeyframe) table. Returns immediately.
    // Idempotent: calling again cancels the previous scan first.
    void startScan(const QString &path, int videoStreamIdx);

    // Stops the scan (if running). Joins the worker thread. Idempotent.
    void cancelScan();

    ScanState scanState() const;
    int       scanFramesBuilt() const;
    bool      hasExactMapping() const;   // true if scanState() == Ready

    // Returns the PTS of the keyframe at-or-before the given frame.
    // Useful for direct keyframe seek (skips av_seek_frame's BACKWARD
    // search). Returns -1 if no scan complete or no keyframe is at-
    // or-before targetFrame in the table.
    int64_t keyframePtsBefore(int targetFrame) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace qcv
