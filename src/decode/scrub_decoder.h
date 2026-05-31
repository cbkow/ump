// ScrubDecoder — Phase 1.8.5.
//
// A separate decoder dedicated to fast, synchronous, single-frame
// fetches during user scrub gestures. Owns its own FFmpeg context +
// hwaccel device so it can seek freely without disturbing the
// streaming VideoDecoder's read position or codec state. Shares the
// FrameIndex on the streaming decoder (PTS↔frame mapping is identical
// for the same file) and pushes decoded frames into the streaming
// decoder's publish slot via VideoDecoder::publishExternalFrame, so
// the renderer doesn't need a second fetch path.
//
// Semantics (per Guide 13 §I.7):
//   - "latest request wins": each requestFrame(N) overwrites the
//     pending target; if a newer one arrives mid-decode, the worker
//     bails and seeks again.
//   - last-frame cache: repeat queries to the same frame return
//     immediately without re-decoding.
//   - lazy lifetime: open() lazily when VideoDecoder opens; close()
//     when it closes.
//
// Phase 1.8.5 carries the same code-duplication trade-off as Guide 13
// §I.13 flags (~30% overlap with VideoDecoder's FFmpeg setup). Phase
// 1.8.6+ extracts a common FFmpeg-session helper.

#pragma once

#include "frame_handle.h"
#include "scrub_frame_cache.h"
#include "simple_lru.h"

#include <QObject>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

struct AVBufferRef;
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace qcv {

class VideoDecoder;

class ScrubDecoder : public QObject
{
    Q_OBJECT
public:
    explicit ScrubDecoder(VideoDecoder *streaming, QObject *parent = nullptr);
    ~ScrubDecoder() override;

    // Open / close are tied to VideoDecoder's lifecycle by WindowManager.
    bool open(const QString &path);
    void close();

    // QML-callable. Latest target wins; older requests are abandoned
    // mid-decode. Cheap to call at slider-drag rate (60+ Hz).
    Q_INVOKABLE void requestFrame(int frameNo);

    // Phase 3.G parity — per-clip YUV range override, mirroring
    // VideoDecoder::setRangeOverride so scrubbed frames match playback
    // levels (0 = Auto / use stream color_range, 1 = Full, 2 = Limited).
    // Folded into the sws srcFullRange in initSwsContext; setting it
    // marks the cached sws colorspace dirty so the next decode re-applies
    // even when dimensions/format are unchanged. WindowManager pushes the
    // active item's override here alongside the streaming decoder.
    void setRangeOverride(int range);
    int  rangeOverride() const {
        return m_rangeOverride.load(std::memory_order_acquire);
    }

private:
    void workerLoop();
    bool initFFmpeg(const QString &path);
    void teardownFFmpeg();
    bool initSwsContext(AVFrame *frame);
    bool decodeAndPublish(int target, AVPacket *pkt,
                          AVFrame *frame, AVFrame *swFrame);
    // Decode forward from the decoder's current position until the frame
    // at/after `targetPts` is produced, caching EVERY frame en route
    // (forward fill + the QuickTime/MPV "keep the GOP" trick). Publishes
    // the target frame. Assumes the decoder is already positioned (caller
    // seeks + flushes for the cold/backward case).
    bool decodeForwardCaching(int target, int64_t targetPts, AVPacket *pkt,
                              AVFrame *frame, AVFrame *swFrame);
    // Turn one decoded AVFrame into a cacheable entry: zero-copy Metal
    // retain where available, else a cheap planar-YUV ref (cloned, NOT
    // converted — the convert is deferred to publishEntry so the cold GOP
    // fill doesn't pay a swscale + RGBA malloc per prefix frame).
    std::shared_ptr<ScrubCacheEntry> makeCacheEntry(AVFrame *frame,
                                                    AVFrame *swFrame,
                                                    int64_t framePts);
    // Publish a cached entry to the renderer: re-retain for Metal, or
    // swscale the planar YUV frame to RGBA on demand for Yuv entries.
    void publishEntry(const std::shared_ptr<ScrubCacheEntry> &entry);

    VideoDecoder    *m_streaming = nullptr;     // not owned

    AVFormatContext *m_fmt = nullptr;
    AVCodecContext  *m_cctx = nullptr;
    AVBufferRef     *m_hwDeviceCtx = nullptr;
    int              m_videoStreamIdx = -1;

    SwsContext      *m_sws = nullptr;
    int              m_swsSrcWidth = 0;
    int              m_swsSrcHeight = 0;
    int              m_swsSrcFormat = -1;

    // Per-clip YUV range override (qcv::VideoRange int). m_swsColorspaceDirty
    // forces initSwsContext to re-apply sws_setColorspaceDetails when the
    // override changes without a dims/format change.
    std::atomic<int>  m_rangeOverride{0};
    std::atomic<bool> m_swsColorspaceDirty{false};

    std::atomic<int> m_pendingTarget{-1};

    // Decoded-GOP cache (the QuickTime/MPV "decode the GOP once, keep the
    // frames" strategy). Keyed by display-frame-number, byte-bounded. Only
    // touched by the worker thread; SimpleLRU's locking is incidental.
    SimpleLRU<int, std::shared_ptr<ScrubCacheEntry>> m_gopCache;

    // Frame currently published to the renderer (exact-repeat skip).
    int  m_lastShown = -1;
    // Display-frame-number of the last frame the decoder produced, and
    // whether the decoder is positioned to continue forward from it
    // without a re-seek/flush. Drives the forward-no-reseek fast path.
    int  m_decoderPos = -1;
    bool m_decoderPositioned = false;

    // Set by setRangeOverride (UI thread); the worker clears the cache
    // before its next decode because CPU entries bake the range in and
    // GPU entries carry a now-stale range snapshot.
    std::atomic<bool> m_cacheInvalidate{false};

    // performance/hwScrubInterCodecs snapshot, read once at open(). Lifts
    // the intra-only hwaccel gate for inter (b-frame) codecs during scrub.
    bool m_hwInterScrub = false;

    std::thread             m_thread;
    std::atomic<bool>       m_stopRequested{false};
    std::mutex              m_condMutex;
    std::condition_variable m_cond;
};

} // namespace qcv
