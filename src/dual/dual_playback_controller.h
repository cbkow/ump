// DualPlaybackController — Phase 7.7 Stage 3.
//
// Lifecycle coordinator for the dual playback island. Owns the
// master clock + both sources + (eventually) compositor + audio
// mixer. Single entry/exit point used by WindowManager when the
// compositor mode flips between Single and Dual.
//
// Stage 3 scope:
//   - Construct: factory two IDualSources from path inputs (auto-
//     detect video vs image-sequence by file extension / probe).
//   - Drive both sources' decode_target each timer tick on a 60 Hz
//     clock-pump thread.
//   - Expose getter for current frame numbers + frame handles for
//     the renderer's pull.
//
// Future stages add: audio mixer ownership, safety overlay coupling,
// MediaItem-aware swap (swapA/swapB), spinner state surface.

#pragma once

#include "i_dual_source.h"
#include "dual_audio_mixer.h"

#include <QObject>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace qcv { class TimelineController; }

namespace qcv::dual {

class DualPlaybackTimer;

// Source kind hint for the factory. AutoDetect inspects the path's
// extension; existing video extensions go through DualVideoDecoder,
// everything else through DualImageSeqSource.
enum class DualSourceKind {
    AutoDetect,
    Video,
    ImageSequence,
};

class DualPlaybackController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int currentFrame READ currentFrame NOTIFY currentFrameChanged)
    Q_PROPERTY(int frameCount READ frameCount NOTIFY frameCountChanged)
    Q_PROPERTY(double fps READ fps NOTIFY fpsChanged)
    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY isPlayingChanged)
    // Phase 7.7 Stage 6 — expose the mixer so QML can bind per-side
    // mute toggles (mutedA / mutedB) to the source-bar buttons.
    Q_PROPERTY(qcv::dual::DualAudioMixer *audio READ audio CONSTANT)
    // Per-side hwaccel label for the status strip ("A: vulkan | B:
    // software" etc.). hwAccelChanged fires when sources are (re)opened.
    Q_PROPERTY(QString hwAccel READ hwAccel NOTIFY hwAccelChanged)

public:
    explicit DualPlaybackController(QObject *parent = nullptr);
    ~DualPlaybackController() override;

    DualPlaybackController(const DualPlaybackController &)            = delete;
    DualPlaybackController &operator=(const DualPlaybackController &) = delete;

    // ---- Lifecycle ----
    // Open both sides. Pass an empty path / empty kind to leave a
    // side empty (rendered transparent). Both must be non-null on
    // first call; later swaps happen via swapA / swapB.
    //
    // exrLayerA / exrLayerB are optional EXR layer / part names —
    // applied via DualImageSeqSource::setLayer() BEFORE the source
    // opens, so the first probed frame routes through the right
    // channel-prefix. Ignored for non-EXR sources.
    bool open(const QString &pathA, DualSourceKind kindA,
              const QString &pathB, DualSourceKind kindB,
              const QString &exrLayerA = QString(),
              const QString &exrLayerB = QString());
    void close();
    bool isOpen() const { return m_open.load(std::memory_order_acquire); }

    // Hot-swap B's source while staying in dual mode. Closes the
    // current B source, factories a new one from `path`, preserves
    // the master clock + A's source. Returns true on success;
    // returns false (and leaves B closed) if the new source open
    // fails. Empty path clears B (transparent in dual render).
    bool swapB(const QString &path,
               DualSourceKind kind = DualSourceKind::AutoDetect);

    // Phase 7.8 — wire the timeline so the controller can translate
    // master frame → per-side source frame using each track's clip
    // layout (startTime + sourceIn). Pass nullptr to reset to
    // identity-mode (no translation; master frame == source frame).
    // Caller (WindowManager) sets this AFTER it has rebuilt tracks
    // A and B in the timeline; pump thread reads via raw pointer
    // (no mutex for v1 — edit ops will introduce serialization).
    void setTimeline(TimelineController *t);

    // Adaptive thread halving for image-seq sources. Called by open()
    // automatically — exposed for tests.
    void applyAdaptiveThreading();

    // ---- Transport ----
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void seekToFrame(int frameNumber);
    Q_INVOKABLE void togglePlayback();

    // ---- Q_PROPERTY accessors ----
    int    currentFrame() const;
    int    frameCount() const;
    double fps() const;
    bool   isPlaying() const;
    QString hwAccel() const;

    // ---- Source access (render thread pulls from these) ----
    IDualSource *sourceA() { return m_sourceA.get(); }
    IDualSource *sourceB() { return m_sourceB.get(); }
    DualPlaybackTimer *timer() { return m_timer.get(); }
    DualAudioMixer    *audio() { return m_audio.get(); }

    // Convenience pull-by-master-frame. Returns nullptr if a side
    // isn't open or the frame isn't yet buffered. Pass the master
    // frame number explicitly so A and B sample the SAME frame —
    // calling them without an arg uses m_timer->currentFrame() but
    // can race the pump thread across integer boundaries.
    //
    // Phase 7.8 — these now apply per-side master→source frame
    // translation when a TimelineController is wired. With no
    // edits applied (one clip at startTime=0, sourceIn=0) the
    // translation is identity and behavior matches Phase 7.7. With
    // a trimmed/slid clip the source frame is offset accordingly.
    std::shared_ptr<DualFrame> pullFrameA(int masterFrame) const;
    std::shared_ptr<DualFrame> pullFrameB(int masterFrame) const;
    std::shared_ptr<DualFrame> pullFrameA() const;
    std::shared_ptr<DualFrame> pullFrameB() const;

    // Translate a master-clock frame to a per-side source frame.
    // Returns -1 when no clip on that track covers the master frame
    // (gap → caller renders that side transparent). When no
    // timeline is wired, returns the input unchanged (identity).
    int translateMasterToSourceFrame(int masterFrame, char trackSide) const;

    // Format a master frame number as SMPTE timecode "HH:MM:SS:FF"
    // at the master fps. Drop-frame is not applied — dual sessions
    // don't carry per-source DF metadata; we always emit non-DF.
    // Returns an empty string if fps isn't established yet.
    Q_INVOKABLE QString formatTimecode(int masterFrame) const;

    // Phase 7.8 — pre-warm helper. When the master is in a gap on
    // a side, return the source frame the decoder should buffer
    // around so that when master enters the next clip's range,
    // the first frame is already decoded. Looks forward through
    // the track's clips for the next non-gap clip whose end is
    // beyond `masterFrame`. Returns -1 if no upcoming clip.
    int nextClipSourceFrame(int masterFrame, char trackSide) const;

    // Phase 7.8 — generation counter that bumps on every timeline
    // mutation (edit commit, save-load, etc.). The compositor
    // reads it and drops its cached MTLTextures when the value
    // changes — prevents stale content from displaying while the
    // decoder catches up after an edit. Bump applies to both
    // sides since any clip rearrangement affects the master→
    // source mapping for both decoders.
    int timelineGeneration() const {
        return m_timelineGeneration.load(std::memory_order_acquire);
    }

    // Force a generation bump. The compositor uses the gen counter
    // to detect "anything changed; drop per-side cache" — bumping
    // here forces the next render to re-upload from the source's
    // (now-flushed) decode buffer. Called after a layer change in
    // a side's image-seq source, where the source's content
    // changes mid-session but the timeline hasn't.
    void bumpTimelineGeneration() {
        m_timelineGeneration.fetch_add(1, std::memory_order_acq_rel);
    }

    // Past-end-of-source predicates. The renderer uses these to
    // decide when to render that side as transparent (per Phase 7.7
    // design — no sticky-last-frame).
    bool aPastEnd(int masterFrame) const;
    bool bPastEnd(int masterFrame) const;

    // Per-side videoRangeOverride mirrored from each side's source
    // MediaItem (0 = Auto, 1 = Full / PC-range, 2 = Limited / TV-
    // range). WindowManager pushes these on dual entry from the A /
    // B MediaItems and on every Inspector pill change via the
    // ProjectManager::videoRangeOverrideChanged signal.
    //
    // Two consumers:
    //   - Windows: `WindowManagerDualSourceAdapter` reads per pull and
    //     stuffs into `DualFramePayload.rangeOverride` so the D3D11
    //     YUV→RGB compute honors the user's override per side.
    //   - macOS: setters also forward to the per-side IDualSource so
    //     DualVideoDecoder applies it in its CPU sws_setColorspaceDetails
    //     path AND tags Metal-kind DualFrames with the override for the
    //     Metal compositor's YUV→RGB pass. Image-seq sources no-op.
    void setRangeOverrideA(int v) {
        m_rangeOverrideA.store(v, std::memory_order_release);
        if (m_sourceA) m_sourceA->setRangeOverride(v);
    }
    void setRangeOverrideB(int v) {
        m_rangeOverrideB.store(v, std::memory_order_release);
        if (m_sourceB) m_sourceB->setRangeOverride(v);
    }
    int rangeOverrideA() const {
        return m_rangeOverrideA.load(std::memory_order_acquire);
    }
    int rangeOverrideB() const {
        return m_rangeOverrideB.load(std::memory_order_acquire);
    }

    // Detect file kind by extension/probe. Public for the WindowManager
    // factory entry. Returns AutoDetect if it can't decide.
    static DualSourceKind detectKind(const QString &path);

signals:
    void currentFrameChanged();
    void frameCountChanged();
    void fpsChanged();
    void isPlayingChanged();
    void hwAccelChanged();

private:
    // Construct the right concrete IDualSource for a path.
    // exrLayer is applied to image-seq sources before open(); empty
    // string is the unprefixed default. Ignored for video.
    // imageSeqThreadCount > 0 overrides DualImageSeqSource's default
    // thread pool size; 0 = use default. Used by open() to halve the
    // pool when both sides are image sequences.
    static std::unique_ptr<IDualSource>
    makeSource(const QString &path, DualSourceKind kind,
                 const QString &exrLayer = QString(),
                 int imageSeqThreadCount = 0);

    void clockPumpLoop();

    std::unique_ptr<IDualSource>        m_sourceA;
    std::unique_ptr<IDualSource>        m_sourceB;
    std::unique_ptr<DualPlaybackTimer>  m_timer;
    // Phase 7.7 Stage 6 — owns the dual-side audio path. Two
    // AudioDecoders + one CoreAudioDevice, mixed in the render
    // callback. Driven by play/pause/seek calls below + the master
    // clock pump's drift correction.
    std::unique_ptr<DualAudioMixer>     m_audio;

    int    m_masterFrameCount = 0;
    double m_masterFps        = 0.0;

    // Phase 7.8 — set by WindowManager after timeline rebuild for
    // per-side master→source translation. Read on the pump thread;
    // for v1 there's no mutex (timeline mutations only happen on
    // dual entry / exit, not mid-playback). Edit ops in Stages C+
    // will introduce serialization.
    qcv::TimelineController *m_timeline = nullptr;

    // Generation counter (see timelineGeneration() above). Bumped
    // by the slot connected to m_timeline's timelineChanged signal.
    std::atomic<int> m_timelineGeneration{0};

    // Per-side videoRangeOverride. Set by WindowManager from each
    // MediaItem at dual entry + on signal change. Read by the
    // adapter on each pullA/pullB to populate DualFramePayload.
    // Defaults to 0 (Auto = use AV-detected color_range).
    std::atomic<int> m_rangeOverrideA{0};
    std::atomic<int> m_rangeOverrideB{0};

    // Clock-pump thread — wakes ~60 Hz, advances timer, pushes
    // decode_target to both sources, emits currentFrameChanged.
    std::thread             m_pumpThread;
    std::atomic<bool>       m_open{false};
    std::atomic<bool>       m_stopRequested{false};
    std::condition_variable m_pumpCv;
    std::mutex              m_pumpCvMutex;

    // Cached for emit-on-change.
    std::atomic<int>  m_lastEmittedFrame{-1};
};

} // namespace qcv::dual
