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

#include <QObject>
#include <QString>
#include <atomic>
#include <condition_variable>
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

private:
    void workerLoop();
    bool initFFmpeg(const QString &path);
    void teardownFFmpeg();
    bool initSwsContext(AVFrame *frame);
    bool decodeAndPublish(int target, AVPacket *pkt,
                          AVFrame *frame, AVFrame *swFrame);

    VideoDecoder    *m_streaming = nullptr;     // not owned

    AVFormatContext *m_fmt = nullptr;
    AVCodecContext  *m_cctx = nullptr;
    AVBufferRef     *m_hwDeviceCtx = nullptr;
    int              m_videoStreamIdx = -1;

    SwsContext      *m_sws = nullptr;
    int              m_swsSrcWidth = 0;
    int              m_swsSrcHeight = 0;
    int              m_swsSrcFormat = -1;

    std::atomic<int> m_pendingTarget{-1};
    int              m_lastDecodedFrame = -1;

    std::thread             m_thread;
    std::atomic<bool>       m_stopRequested{false};
    std::mutex              m_condMutex;
    std::condition_variable m_cond;
};

} // namespace qcv
