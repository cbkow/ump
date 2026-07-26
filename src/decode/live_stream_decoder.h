// LiveStreamDecoder — v2.2.3, Blender-bridge stage 2.
//
// Contained "live mode" decode path for srt:// (and other FFmpeg
// network-protocol) URLs. Deliberately bypasses the file-oriented
// architecture: no FrameIndex, no ScrubDecoder/GOP cache, no seeking,
// no duration, no pacing — frames arrive network-paced from the
// sender and are published immediately, latest-wins.
//
// It does NOT render into its own slot: decoded frames are pushed into
// the main VideoDecoder's publish slot via publishExternalFrame() (the
// same sanctioned inlet ScrubDecoder uses), so the renderer's video
// path — Metal and D3D11, single and A/B — needs no changes and no new
// binding. The sink VideoDecoder stays closed (Idle) the whole time;
// only its latest-wins publish slot is borrowed. WindowManager
// guarantees ordering: stopLiveStream() joins this worker before the
// sink's publish slot is cleared by VideoDecoder::close().
//
// Connection lifecycle (stage-0 lessons, see the qcbridge repo's
// streaming-pipeline.md): an SRT listener accepts ONE connection and
// stream-end orphans the peer, so reconnect is entirely the receiver's
// job. The worker loops connect → read/decode/publish → on error or
// EOF back to connect, with capped exponential backoff, until close().
// All blocking FFmpeg calls are bounded by an AVIO interrupt callback
// so close() joins promptly.
//
// Startup honesty: decode starts at the first keyframe (packets before
// it are dropped), which avoids the classic ~1 GOP of benign "missing
// references" decode-error spam a mid-GOP join produces. Sender GOP is
// 1 s, so the gate costs at most that.

#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVRational;
struct SwsContext;

namespace qcv {

class VideoDecoder;

class LiveStreamDecoder : public QObject
{
    Q_OBJECT
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString url READ url CONSTANT)
    Q_PROPERTY(int width READ width NOTIFY metadataChanged)
    Q_PROPERTY(int height READ height NOTIFY metadataChanged)
    Q_PROPERTY(QString codecName READ codecName NOTIFY metadataChanged)
    Q_PROPERTY(QString pixelFormatName READ pixelFormatName NOTIFY metadataChanged)
    Q_PROPERTY(bool hasAudio READ hasAudio NOTIFY metadataChanged)
    Q_PROPERTY(int reconnectCount READ reconnectCount NOTIFY statusChanged)

public:
    enum Status : int {
        Idle         = 0,   // constructed / closed
        Connecting   = 1,   // first connection attempt in progress
        Live         = 2,   // frames flowing
        Reconnecting = 3,   // stream dropped / sender gone; retrying
    };
    Q_ENUM(Status)

    explicit LiveStreamDecoder(QObject *parent = nullptr);
    ~LiveStreamDecoder() override;

    // Must be set before open(). The sink must outlive this object's
    // close() — WindowManager owns both and tears down in that order.
    void setSink(VideoDecoder *sink) { m_sink = sink; }

    // Invoked (from the worker thread) after every published frame.
    // WindowManager installs the renderer's requestUpdate here so the
    // D3D11 render-on-demand loop wakes per frame (Metal free-runs and
    // doesn't need it). Same cross-thread contract as the dual
    // sources' setFrameAvailableCallback.
    void setFrameCallback(std::function<void()> cb);

    bool open(const QString &url);
    void close();

    Status  status() const {
        return static_cast<Status>(m_status.load(std::memory_order_acquire));
    }
    QString url() const { return m_url; }
    int     width() const { return m_width.load(std::memory_order_acquire); }
    int     height() const { return m_height.load(std::memory_order_acquire); }
    bool    hasAudio() const { return m_hasAudio.load(std::memory_order_acquire); }
    int     reconnectCount() const {
        return m_reconnects.load(std::memory_order_acquire);
    }
    qint64  framesReceived() const {
        return m_framesReceived.load(std::memory_order_acquire);
    }
    QString codecName() const;
    QString pixelFormatName() const;

    // Polled by the live strip's 1 s QML Timer (deltas → fps/Mbps).
    // Deliberately Q_INVOKABLE not Q_PROPERTY: per-packet NOTIFY
    // signals would be pure churn.
    Q_INVOKABLE double statFramesReceived() const {
        return double(m_framesReceived.load(std::memory_order_acquire));
    }
    Q_INVOKABLE double statBytesReceived() const {
        return double(m_bytesReceived.load(std::memory_order_acquire));
    }
    // Seconds since this session went Live; 0 when not live.
    Q_INVOKABLE int statLiveSeconds() const;

signals:
    void statusChanged();
    void metadataChanged();

private:
    // Worker internals — all run on m_thread.
    void workerLoop();
    bool connectOnce();          // one connect+read/decode session
    void teardownSession(AVFormatContext **fmt, AVCodecContext **cctx,
                         SwsContext **sws);
    void publishFrame(AVFrame *frame, AVCodecContext *cctx,
                      SwsContext **sws, const AVRational &tb,
                      const char *srcLabel = "sw→RGBA");
    void setStatus(Status s);
    void setSessionMetadata(int w, int h, const QString &codec,
                            const QString &pixFmt, bool hasAudio);
    bool interruptibleSleep(int ms);   // false when close() interrupted it

    static int interruptCb(void *opaque);

    VideoDecoder         *m_sink = nullptr;
    QString               m_url;

    std::thread           m_thread;
    std::atomic<bool>     m_stopRequested{false};
    std::mutex            m_sleepMutex;
    std::condition_variable m_sleepCv;

    std::atomic<int>      m_status{Idle};
    std::atomic<int>      m_width{0};
    std::atomic<int>      m_height{0};
    std::atomic<bool>     m_hasAudio{false};
    std::atomic<int>      m_reconnects{0};
    std::atomic<qint64>   m_framesReceived{0};
    std::atomic<qint64>   m_bytesReceived{0};
    std::atomic<qint64>   m_liveSinceMs{0};   // steady_clock ms; 0 = not live

    mutable std::mutex    m_metaMutex;   // guards the two strings below
    QString               m_codecName;
    QString               m_pixelFormatName;

    std::mutex            m_frameCbMutex;
    std::function<void()> m_frameCb;
};

} // namespace qcv
