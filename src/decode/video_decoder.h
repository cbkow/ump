// VideoDecoder — Phase 1.8.1 skeleton.
//
// Background thread reads packets, decodes frames with FFmpeg's
// software path, converts to RGBA8 via swscale, and publishes the
// most-recent frame as a QImage protected by a mutex. The render
// thread (PlayerRhiRenderer::synchronize) calls fetchLatest() to
// pick up new frames for upload.
//
// Phase 1.8.1 deliberately does NOT include:
//   - hardware acceleration (Phase 1.8.2)
//   - FrameIndex / accurate frame numbering (Phase 1.8.3)
//   - seeking / scrubbing (Phase 1.8.3 / 1.8.4)
//   - playback clock / pacing (later phase)
//
// At Phase 1.8.1 the decoder runs as fast as possible from the
// beginning of the file; the player displays frames as they arrive
// (so playback speed = decode rate, not real-time fps).

#pragma once

#include "frame_handle.h"
#include "frame_index.h"
#include "timecode_formatter.h"

#include <QImage>
#include <QObject>
#include <QString>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

struct AVBufferRef;
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace qcv {

class VideoDecoder : public QObject
{
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY sourcePathChanged)
    Q_PROPERTY(int width READ width NOTIFY metadataChanged)
    Q_PROPERTY(int height READ height NOTIFY metadataChanged)
    Q_PROPERTY(int frameCount READ frameCount NOTIFY metadataChanged)
    Q_PROPERTY(QString codecName READ codecName NOTIFY metadataChanged)
    Q_PROPERTY(QString pixelFormat READ pixelFormat NOTIFY metadataChanged)
    Q_PROPERTY(QString hwAccel READ hwAccel NOTIFY metadataChanged)
    Q_PROPERTY(int currentFrame READ currentFrame NOTIFY currentFrameChanged)
    Q_PROPERTY(double fps READ fps NOTIFY metadataChanged)
    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY isPlayingChanged)
    Q_PROPERTY(int decodeErrorCount READ decodeErrorCount NOTIFY decodeHealthChanged)
    Q_PROPERTY(QString lastDecodeError READ lastDecodeError NOTIFY decodeHealthChanged)
    Q_PROPERTY(QString sourceTimecode READ sourceTimecode NOTIFY metadataChanged)
    Q_PROPERTY(bool isDropFrame READ isDropFrame NOTIFY metadataChanged)
    Q_PROPERTY(int sourceTimecodeStartFrame READ sourceTimecodeStartFrame NOTIFY metadataChanged)
    Q_PROPERTY(QString startTimecode READ startTimecode NOTIFY startTimecodeChanged)

public:
    enum State : int {
        Idle        = 0,   // no file open
        Opening     = 1,   // opening file / creating context
        Decoding    = 2,   // background thread producing frames
        EndOfStream = 3,   // decoded all frames; loops not yet supported
        Errored     = 4,   // open or decode failed; lastError() has details
    };
    Q_ENUM(State)

    explicit VideoDecoder(QObject *parent = nullptr);
    ~VideoDecoder() override;

    // QML-callable. Opens the file, kicks off decode in the background.
    Q_INVOKABLE bool open(const QString &path);
    Q_INVOKABLE void close();

    // Clear a sticky Errored state back to Idle. close() deliberately
    // preserves Errored (so genuine decode failures stay visible in the
    // status strip); callers that handle the failure gracefully — e.g.
    // the ARRIRAW viewport notice — use this so the toolbar doesn't keep
    // reading "ERROR" for an intentionally-unsupported file. No-op when
    // not in Errored.
    void clearErrorState();

    // QML-callable. Requests a seek to the given frame number. The
    // request is delivered to the decode thread via an atomic slot;
    // calls during an in-flight seek win and the older one is
    // abandoned. The first frame at or after the target PTS is
    // published; on inter-frame codecs the keyframe at-or-before
    // is sought first and intermediate frames are decoded silently.
    Q_INVOKABLE void seekToFrame(int frameNo);

    // Play/pause. Decoder default is "playing" once a file is open;
    // pause() blocks the decode thread on the same condvar that
    // handles seek/EOF. play()/pause()/togglePlayback() are safe to
    // call from any thread.
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlayback();

    State state() const { return m_state.load(std::memory_order_acquire); }
    QString sourcePath() const { return m_sourcePath; }
    QString lastError() const { return m_lastError; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    int frameCount() const { return m_frameCount; }
    QString codecName() const { return m_codecName; }
    QString pixelFormat() const { return m_pixelFormat; }
    QString hwAccel() const { return m_hwAccelType; }
    int     currentFrame() const { return m_currentFrame.load(std::memory_order_acquire); }
    double  fps() const { return m_frameIndex.isValid() ? m_frameIndex.fps() : 0.0; }
    bool    isPlaying() const { return m_isPlaying.load(std::memory_order_acquire); }
    int     decodeErrorCount() const { return m_decodeErrorCount.load(std::memory_order_acquire); }
    QString lastDecodeError() const;

    // Container-supplied source timecode (e.g., "01:00:00;00" for a
    // ProRes master that starts at one hour, drop-frame). Empty when
    // the source has no embedded timecode track. The `;` separator
    // before the frames field is the standard SMPTE drop-frame
    // signaler — we detect DF by string format, matching how FFmpeg
    // emits MOV `tmcd` tracks.
    QString sourceTimecode() const { return m_sourceTimecode; }
    bool    isDropFrame() const { return m_isDropFrame; }
    // Frame offset of the source's start timecode relative to a
    // hypothetical "00:00:00:00" zero. For "01:00:00:00" at 24 fps
    // this is 86400. -1 if no embedded timecode. DF-aware.
    int     sourceTimecodeStartFrame() const { return m_sourceStartFrame; }

    // Format an absolute frame number (0-based, internal counter)
    // as a SMPTE timecode using the source's drop-frame flag and
    // start offset. Returns an empty string if no source is open.
    Q_INVOKABLE QString formatTimecode(int frameNo) const;

    // Phase 3.F — playhead-origin timecode picker. The Inspector's
    // Timecodes section lists multiple TC strings (FFmpeg embedded,
    // QT Start, QT TimeCode, XMP Alt, …). Calling this with one of
    // those strings re-parses it and uses the result as the offset
    // formatTimecode() adds to the internal frame number. Empty
    // string ⇒ no offset (origin = 00:00:00:00); the user picks
    // "From start" to disable timecode mode.
    Q_INVOKABLE void setStartTimecode(const QString &tc);
    QString startTimecode() const { return m_startTimecodeString; }

    // Phase 3.H.2 — true once any frame has been published since the
    // last open(). Used by the playlist orchestrator to know when a
    // pre-warmed decoder is ready to swap (vs. still spinning up).
    // Atomic — readable from the GUI thread without locks.
    bool isReady() const {
        return m_publishedSeq.load(std::memory_order_acquire) > 0;
    }

    // Phase 3.G — per-clip YUV range override.
    //   0 = Auto (use the stream's color_range)
    //   1 = Full (force AVCOL_RANGE_JPEG / 0..255)
    //   2 = Limited (force AVCOL_RANGE_MPEG / 16..235)
    // CPU path: forces an `m_sws` rebuild on the next frame so
    // sws_setColorspaceDetails sees the new srcFullRange. HW path:
    // the renderer reads `rangeOverride()` from the decoder and
    // stomps `planes.fullRange` before the YUV→RGB kernel call.
    // Both paths trigger a re-decode of the current frame so the
    // user sees the change immediately, even when paused.
    Q_INVOKABLE void setRangeOverride(int range);
    int rangeOverride() const {
        return m_rangeOverride.load(std::memory_order_acquire);
    }

    const FrameIndex &frameIndex() const { return m_frameIndex; }

    // Render-thread-safe. If a frame newer than the last one consumed
    // is available, MOVES it into *out and returns true. The handle
    // owns its underlying resource (CPU image or platform GPU buffer).
    // Returns false if no new frame is available.
    bool fetchLatest(FrameHandle *out);

    // External publish entry — used by ScrubDecoder to inject a frame
    // into this decoder's slot without the streaming context having
    // produced it. Bypasses FPS pacing (scrub frames must appear
    // immediately). Updates currentFrame and emits frameAvailable.
    void publishExternalFrame(FrameHandle handle, int64_t pts);

    // Soft decode error — increments the count, sets lastDecodeError,
    // emits decodeHealthChanged. Public so ScrubDecoder can register
    // errors against the same health surface (users see one unified
    // "decoder family" health state, not two).
    void recordDecodeError(const QString &msg);

signals:
    void stateChanged();
    void sourcePathChanged();
    void metadataChanged();
    void currentFrameChanged();
    void isPlayingChanged();
    void decodeHealthChanged();
    void startTimecodeChanged();
    void errorOccurred(const QString &message);
    // Emitted from the decode thread each time a new frame is
    // published. Delivered to UI-thread receivers via a queued
    // connection. Carries no payload — receivers call fetchLatest().
    void frameAvailable();

private:
    void setState(State s);
    void setError(const QString &msg);

    void decodeLoop();
    bool initFFmpeg(const QString &path);
    void teardownFFmpeg();
    bool initSwsContext(AVFrame *firstFrame);
    void publishCpuFrame(AVFrame *frame);
    void publishMetalFrame(AVFrame *frame);
    // Phase F.2.4.2 — Vulkan-decoded frame on our shared VkDevice.
    // Clones the AVFrame (keeps the AVVkFrame ref alive), wraps in a
    // FrameHandle::Vulkan, publishes. The renderer-side bridge
    // (F.2.4.3) imports the VkImage into D3D11 via NT shared handle.
    void publishVulkanFrame(AVFrame *frame);
    void publishHandle(FrameHandle handle, int64_t pts, bool pace);
    // Performs a synchronous seek + decode-forward to the target
    // frame number on the decode thread. Caller owns pkt/frame/swFrame.
    void performSeek(int targetFrame, AVPacket *pkt, AVFrame *frame,
                     AVFrame *swFrame);

    // Drop the cached Vulkan hwdevice immediately + synchronously.
    // Used by the device-lost detection path (Phase I.B) so a
    // poisoned device doesn't get reattached on the next reopen.
    // Also called from close() on a true (non-reopening) close so
    // the cleanup-queue post is the canonical free path. On non-
    // Windows / non-Vulkan this is a no-op; m_hwDeviceCtx is null
    // and there's nothing to release.
    void releaseCachedHwDevice();

    // Properties surfaced to QML / cached at open()
    QString m_sourcePath;
    QString m_lastError;
    // True for the brief window between open()'s internal close()
    // call and the new path being committed. Used to suppress the
    // close()'s sourcePathChanged("") emit so downstream subsystems
    // (AudioPlayer, ScrubDecoder) only see ONE close/open cycle per
    // playlist boundary cross instead of two. The true-close (called
    // directly by the owner, no follow-up open) still emits — the
    // path-clearing handlers in WindowManager need that signal.
    bool    m_reopening = false;
    int     m_width = 0;
    int     m_height = 0;
    int     m_frameCount = 0;
    QString m_codecName;
    QString m_pixelFormat;
    QString m_hwAccelType;     // "" if software, "videotoolbox" / etc. if HW
    QString m_sourceTimecode;  // e.g., "01:00:00;00" or "" if absent
    bool    m_isDropFrame = false;
    int     m_sourceStartFrame = -1;   // -1 if no embedded timecode
    TimecodeFormatter m_tcFormatter;   // built once at open()
    // Phase 3.F — user-selected playhead-origin timecode. Empty when
    // user has picked "From start" (no offset). Defaults to the
    // FFmpeg-detected source TC at open(); Inspector toggle replaces
    // it with QT/XMP/MXF variants on click. Atomic frame so the
    // formatter (called from QML thread) doesn't race with the setter.
    QString m_startTimecodeString;
    std::atomic<int> m_currentStartFrame{-1};

    // Phase 3.G — per-clip YUV range override (qcv::VideoRange).
    // 0 = Auto (default); 1 = Full; 2 = Limited. Atomic so the
    // render thread (HW path read) and the QML thread (setter) can
    // both touch it without locking.
    std::atomic<int> m_rangeOverride{0};

    // One-shot diagnostic flags — fire once per opened source so we
    // can see which decode path we're on (zero-copy Metal, VT-then-
    // CPU fallback, or pure software). Cleared in open().
    bool    m_loggedMetalFormat     = false;
    bool    m_loggedCpuFormat       = false;
    bool    m_loggedHwToCpuFallback = false;
    bool    m_loggedVulkanFormat    = false;

    // Convert a stream-time-base PTS to microseconds. Used so
    // FrameHandles from different sources can be compared on a
    // common time axis (atomic-pair gate, Guide 01 §5).
    int64_t ptsToMicroseconds(int64_t streamTickPts) const;

    // FFmpeg state — touched only by the decode thread.
    AVFormatContext *m_fmt = nullptr;
    AVCodecContext  *m_cctx = nullptr;
    SwsContext      *m_sws = nullptr;
    AVBufferRef     *m_hwDeviceCtx = nullptr;   // VideoToolbox / VAAPI / etc.
    int              m_videoStreamIdx = -1;
    int              m_swsSrcWidth = 0;
    int              m_swsSrcHeight = 0;
    int              m_swsSrcFormat = -1;

    // Latest-wins publish slot. Decoder writes a FrameHandle (either
    // a CPU QImage from the software path or a retained CVPixelBuffer
    // from the zero-copy hwaccel path); render thread moves it out
    // via fetchLatest. At most 1 handle in slot + 1 with the consumer.
    mutable std::mutex     m_publishMutex;
    FrameHandle            m_publishedFrame;
    std::atomic<uint64_t>  m_publishedSeq{0};
    uint64_t               m_lastFetchedSeq = 0;

    // FrameIndex — built at open() from stream metadata. Drives
    // PTS↔frame conversion for both the published-frame number
    // (currentFrame) and seekToFrame's target PTS computation.
    FrameIndex             m_frameIndex;
    std::atomic<int>       m_currentFrame{-1};

    // (Phase 7.4.b.4 removed m_externalFps + the openExternal/
    // closeExternal/publishExternalFrameByIndex façade — image
    // sequences flow through ImageSequenceCache and the renderer's
    // pull path now, no longer through VideoDecoder.)

    // Seek request channel — UI thread writes target frame, decode
    // thread polls + clears. -1 = no pending seek. Cond var wakes the
    // decode thread when it's blocked at EOF or paused.
    std::atomic<int>       m_pendingSeekTarget{-1};
    std::mutex             m_seekCondMutex;
    std::condition_variable m_seekCond;

    // Play/pause + frame-pacing state. Used only on the decode thread.
    std::atomic<bool>      m_isPlaying{false};
    bool                   m_paceBaselineSet = false;
    std::chrono::steady_clock::time_point m_paceBaselineWall;
    int64_t                m_paceBaselinePts = 0;

    // Decode-health surface (Guide 13 §I.10). decodeErrorCount is
    // atomic so any thread can bump it; lastDecodeError is mutex-
    // guarded since QString isn't atomic-safe.
    std::atomic<int>       m_decodeErrorCount{0};
    mutable std::mutex     m_lastErrorMutex;
    QString                m_lastDecodeError;

    // Threading
    std::thread       m_thread;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<State> m_state{Idle};
};

} // namespace qcv
