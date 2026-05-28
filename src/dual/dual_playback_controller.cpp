#include "dual_playback_controller.h"

#include "decode/timecode_formatter.h"
#include "dual_image_seq_source.h"
#include "dual_playback_timer.h"
#include "i_dual_scrub_decoder.h"
#include "dual_video_decoder.h"
#include "timeline/timeline_controller.h"
#include "timeline/timeline_flattener.h"
#include "timeline/timeline_types.h"

#include <QFileInfo>
#include <QSettings>
#include <QtLogging>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace qcv::dual {

namespace {

bool looksLikeVideo(const QString &path)
{
    static const QStringList kExts = {
        "mov", "mp4", "mkv", "avi", "mxf", "webm", "ts", "m2t", "m2ts", "m4v"
    };
    return kExts.contains(QFileInfo(path).suffix().toLower());
}

bool looksLikeImageSequenceFile(const QString &path)
{
    static const QStringList kExts = {
        "png", "jpg", "jpeg", "tif", "tiff", "exr", "bmp"
    };
    return kExts.contains(QFileInfo(path).suffix().toLower());
}

} // namespace

DualPlaybackController::DualPlaybackController(QObject *parent)
    : QObject(parent)
    , m_timer(std::make_unique<DualPlaybackTimer>())
    , m_audio(std::make_unique<DualAudioMixer>(this))
{
    // Lazy-init the audio device. Failure (no output device, etc.)
    // is non-fatal — the mixer's open()/play() short-circuit and
    // dual mode keeps working without sound.
    if (!m_audio->initialize()) {
        qWarning("DualPlaybackController: audio mixer init failed; "
                 "dual mode will play silent");
    }
}

DualPlaybackController::~DualPlaybackController()
{
    close();
}

DualSourceKind DualPlaybackController::detectKind(const QString &path)
{
    if (path.isEmpty()) return DualSourceKind::AutoDetect;
    QFileInfo info(path);
    if (info.isDir()) return DualSourceKind::ImageSequence;
    if (looksLikeVideo(path)) return DualSourceKind::Video;
    if (looksLikeImageSequenceFile(path)) return DualSourceKind::ImageSequence;
    return DualSourceKind::AutoDetect;
}

std::unique_ptr<IDualSource>
DualPlaybackController::makeSource(const QString &path, DualSourceKind kind,
                                     const QString &exrLayer,
                                     int imageSeqThreadCount,
                                     bool forceSwForProRes)
{
    if (path.isEmpty()) return nullptr;

    DualSourceKind effective = kind;
    if (effective == DualSourceKind::AutoDetect) {
        effective = detectKind(path);
    }

    switch (effective) {
        case DualSourceKind::Video: {
            auto src = std::make_unique<DualVideoDecoder>();
            // Set the per-instance ProRes force-SW flag BEFORE open()
            // — initFFmpeg reads it when deciding whether to attach
            // Vulkan/D3D11VA. Both A and B set this true in dual mode
            // (the NVIDIA Vulkan device-lost crash trips on whichever
            // side is ProRes — there is no safe side). Non-ProRes
            // sources are unaffected: the decoder's gate is
            // (kIsProRes && this flag). Single-flow ProRes is never
            // forced — the flag is only set here, inside the dual
            // controller.
            src->setForceSoftwareDecodeForProRes(forceSwForProRes);
            if (!src->open(path)) return nullptr;
            return src;
        }
        case DualSourceKind::ImageSequence: {
            auto src = std::make_unique<DualImageSeqSource>();
            // Set thread count + EXR layer BEFORE open() so the
            // worker pool spawns at the right size and the first
            // probed frame goes through the right channel-prefix.
            if (imageSeqThreadCount > 0) {
                src->setThreadCount(imageSeqThreadCount);
            }
            if (!exrLayer.isEmpty()) src->setLayer(exrLayer);
            if (!src->open(path)) return nullptr;
            return src;
        }
        default:
            qWarning("DualPlaybackController: cannot detect source kind for %s",
                     qPrintable(path));
            return nullptr;
    }
}

bool DualPlaybackController::open(const QString &pathA, DualSourceKind kindA,
                                   const QString &pathB, DualSourceKind kindB,
                                   const QString &exrLayerA,
                                   const QString &exrLayerB)
{
    close();

    // Resolve effective kinds up-front (image-seq spawns its worker
    // pool at open() time; setThreadCount has no effect post-open).
    const DualSourceKind effA =
        (!pathA.isEmpty() && kindA == DualSourceKind::AutoDetect)
            ? detectKind(pathA) : kindA;
    const DualSourceKind effB =
        (!pathB.isEmpty() && kindB == DualSourceKind::AutoDetect)
            ? detectKind(pathB) : kindB;

    // Thread budget — read the user's performance/imageSeqThreads
    // setting (single-flow's knob, default 16, clamped 1-64) and
    // halve it for each dual side. Applied regardless of whether
    // one or both sides are image-seq: if only A is image-seq, B's
    // pool is irrelevant (DualVideoDecoder has its own threading),
    // so A gets HALF the budget. That's by design — dual mode is
    // CPU-heavier than single (two decoders + a compositor + the
    // dual audio mixer) and we don't want image-seq workers
    // saturating the system. Pre-fix this was hard-coded to 4
    // (DualImageSeqSource::kDefaultThreadCount), which couldn't
    // keep up with EXR decode + slow disk on long sequences.
    const int userThreads = std::clamp(
        QSettings().value(
            QStringLiteral("performance/imageSeqThreads"), 16).toInt(),
        1, 64);
    const int seqThreads = std::max(1, userThreads / 2);
    if (effA == DualSourceKind::ImageSequence
        || effB == DualSourceKind::ImageSequence) {
        qInfo("DualPlaybackController: image-seq side(s) get %d worker "
              "threads each (half of performance/imageSeqThreads=%d)",
              seqThreads, userThreads);
    }

    // Both sides force ProRes through software decode in dual mode.
    // The dual Vulkan path is unstable on NVIDIA with ProRes: a seek
    // into the gap region where one side is past-end and the other
    // triggers a VK_ERROR_DEVICE_LOST that disables hwaccel for the
    // whole session. The crash trips regardless of whether ProRes is
    // on A or B, so both sides take the SW path for ProRes specifically.
    // Non-ProRes codecs still HW-accelerate normally on each side.
    // Single-flow ProRes is also unaffected (the flag is per-instance
    // and only gets set inside DualPlaybackController).
    m_sourceA = makeSource(pathA, kindA, exrLayerA, seqThreads,
                            /*forceSwForProRes=*/true);
    if (!m_sourceA && !pathA.isEmpty()) {
        qWarning("DualPlaybackController: source A open failed (%s)",
                 qPrintable(pathA));
        return false;
    }
    m_sourceB = makeSource(pathB, kindB, exrLayerB, seqThreads,
                            /*forceSwForProRes=*/true);
    if (!m_sourceB && !pathB.isEmpty()) {
        qWarning("DualPlaybackController: source B open failed (%s)",
                 qPrintable(pathB));
        m_sourceA.reset();
        return false;
    }

    // Seed freshly-constructed sources with the current overrides so a
    // value set BEFORE open (or via the dual-entry block in
    // WindowManager that pushes both before any frames pull) takes
    // effect on the very first decoded frame.
    if (m_sourceA) m_sourceA->setRangeOverride(
        m_rangeOverrideA.load(std::memory_order_acquire));
    if (m_sourceB) m_sourceB->setRangeOverride(
        m_rangeOverrideB.load(std::memory_order_acquire));

    applyAdaptiveThreading();

    // Master fps = max of both, falling back to whichever is open.
    double fpsA = m_sourceA ? m_sourceA->fps() : 0.0;
    double fpsB = m_sourceB ? m_sourceB->fps() : 0.0;
    if (fpsA <= 0.0 && fpsB <= 0.0) {
        m_masterFps = 24.0;
    } else if (fpsA <= 0.0) {
        m_masterFps = fpsB;
    } else if (fpsB <= 0.0) {
        m_masterFps = fpsA;
    } else {
        m_masterFps = std::max(fpsA, fpsB);
    }

    int countA = m_sourceA ? m_sourceA->frameCount() : 0;
    int countB = m_sourceB ? m_sourceB->frameCount() : 0;
    // Per-side frame counts are in their own source fps, not the
    // master fps. Convert to seconds first, then back to master
    // frames — otherwise mixed-fps pairs (e.g. 23.976 A + 50 B)
    // compute masterFrameCount = max(countA, countB) in disjoint
    // frame-rate spaces, which under-sizes the master range when
    // the longer-duration side has the lower fps. Seek clamps to
    // (masterFrameCount - 1) then snap the playhead back into the
    // truncated range, looking like "playback resumes earlier."
    const double durA = (fpsA > 0.0) ? countA / fpsA : 0.0;
    const double durB = (fpsB > 0.0) ? countB / fpsB : 0.0;
    const double maxDur = std::max(durA, durB);
    m_masterFrameCount = (maxDur > 0.0 && m_masterFps > 0.0)
        ? std::max(1, static_cast<int>(std::round(maxDur * m_masterFps)))
        : 0;

    m_timer->setFps(m_masterFps);
    m_timer->seekToFrame(0);

    m_lastEmittedFrame.store(-1, std::memory_order_release);
    m_stopRequested.store(false, std::memory_order_release);
    m_open.store(true, std::memory_order_release);

    // Stage 6 — open the audio mixer with both side paths. Image-
    // sequence sides have empty audio path (image seqs don't carry
    // audio); only video sides feed audio. Empty/non-audio sides
    // render silent. Only video paths are passed; image-seq sources
    // would yield "no audio stream" warnings if probed.
    if (m_audio) {
        const bool aIsVideo =
            (kindA != DualSourceKind::ImageSequence) &&
            (kindA == DualSourceKind::Video ||
             detectKind(pathA) == DualSourceKind::Video);
        const bool bIsVideo =
            (kindB != DualSourceKind::ImageSequence) &&
            (kindB == DualSourceKind::Video ||
             detectKind(pathB) == DualSourceKind::Video);
        const QString audioA = aIsVideo ? pathA : QString();
        const QString audioB = bIsVideo ? pathB : QString();
        m_audio->open(audioA, audioB);
    }

    // Eager scrub-decoder construction for video sides. Image-seq
    // sources don't get one — the cache is already random-access.
    // Open is synchronous (avformat_open_input + find_stream_info)
    // — small cold-start cost (~50-200 ms × video sides) traded for
    // zero hitch on the user's first scrub gesture. Matches single-
    // flow's eager ScrubDecoder open in WindowManager.
    if (auto *va = dynamic_cast<DualVideoDecoder *>(m_sourceA.get())) {
        m_scrubA = makeDualScrubDecoder(va, this);
        if (!m_scrubA->open(pathA)) {
            qWarning("DualPlaybackController: scrub decoder A open failed (%s)",
                     qPrintable(pathA));
            m_scrubA.reset();
        }
    }
    if (auto *vb = dynamic_cast<DualVideoDecoder *>(m_sourceB.get())) {
        m_scrubB = makeDualScrubDecoder(vb, this);
        if (!m_scrubB->open(pathB)) {
            qWarning("DualPlaybackController: scrub decoder B open failed (%s)",
                     qPrintable(pathB));
            m_scrubB.reset();
        }
    }

    // Pre-warm the scrub path so the user's first scrub gesture is hot:
    // each video side decodes + publishes one frame now, paying the cold
    // first-seek + codec/file warm-up off the gesture. (Image-seq sides
    // already prime their cache at the start frame on open.) Without this
    // the first scrub right after dual entry stalls a beat.
    if (m_scrubA) {
        int sf = translateMasterToSourceFrame(0, 'A');
        if (sf < 0) sf = nextClipSourceFrame(0, 'A');
        if (sf >= 0) m_scrubA->requestFrame(sf);
    }
    if (m_scrubB) {
        int sf = translateMasterToSourceFrame(0, 'B');
        if (sf < 0) sf = nextClipSourceFrame(0, 'B');
        if (sf >= 0) m_scrubB->requestFrame(sf);
    }

    m_pumpThread = std::thread([this] { clockPumpLoop(); });

    qInfo("DualPlaybackController: opened — A=%s (%dx%d, %d frames, %.2f fps), "
          "B=%s (%dx%d, %d frames, %.2f fps), master fps=%.2f, master frames=%d",
          m_sourceA ? qPrintable(QFileInfo(pathA).fileName()) : "(none)",
          m_sourceA ? m_sourceA->width() : 0,
          m_sourceA ? m_sourceA->height() : 0,
          countA, fpsA,
          m_sourceB ? qPrintable(QFileInfo(pathB).fileName()) : "(none)",
          m_sourceB ? m_sourceB->width() : 0,
          m_sourceB ? m_sourceB->height() : 0,
          countB, fpsB,
          m_masterFps, m_masterFrameCount);

    emit fpsChanged();
    emit frameCountChanged();
    emit hwAccelChanged();
    return true;
}

void DualPlaybackController::close()
{
    if (!m_open.exchange(false, std::memory_order_acq_rel) &&
        !m_pumpThread.joinable()) {
        return;
    }
    m_stopRequested.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_pumpCvMutex);
    }
    m_pumpCv.notify_all();
    if (m_pumpThread.joinable()) m_pumpThread.join();

    if (m_audio) m_audio->close();

    // Scrub decoders MUST tear down before their paired streaming
    // sources — DualScrubDecoder::workerLoop reads
    // m_streaming->ptsForFrameNumber(), so the streaming decoder's
    // FFmpeg state must outlive the scrub worker. close() joins
    // the scrub worker thread internally.
    if (m_scrubA) { m_scrubA->close(); m_scrubA.reset(); }
    if (m_scrubB) { m_scrubB->close(); m_scrubB.reset(); }
    m_scrubActive.store(false, std::memory_order_release);

    if (m_sourceA) m_sourceA->close();
    if (m_sourceB) m_sourceB->close();
    m_sourceA.reset();
    m_sourceB.reset();

    m_masterFrameCount = 0;
    m_masterFps = 0.0;
    m_lastEmittedFrame.store(-1, std::memory_order_release);
}

bool DualPlaybackController::swapB(const QString &path, DualSourceKind kind)
{
    if (!m_open.load(std::memory_order_acquire)) return false;

    // Tear down B's scrub decoder BEFORE the streaming source it
    // points at (close()-then-reset on the streaming pointer would
    // leave the scrub worker mid-call into a destroyed object).
    if (m_scrubB) { m_scrubB->close(); m_scrubB.reset(); }

    // Close existing B (decoder thread join inside its own close()).
    if (m_sourceB) {
        m_sourceB->close();
        m_sourceB.reset();
    }
    if (path.isEmpty()) {
        // Cleared B — render-side will see hasFrame=false and draw
        // transparent on that side. No scrub decoder rebuild.
        emit frameCountChanged();
        emit hwAccelChanged();
        return true;
    }

    // swapB replaces B mid-session; force-SW for ProRes on B mirrors
    // the dual-entry default in open().
    auto src = makeSource(path, kind, /*exrLayer=*/QString(),
                           /*imageSeqThreadCount=*/0,
                           /*forceSwForProRes=*/true);
    if (!src) {
        qWarning("DualPlaybackController::swapB: open failed for %s",
                 qPrintable(path));
        return false;
    }
    m_sourceB = std::move(src);

    // Build a scrub decoder for the new B if it's video. Image-seq
    // B uses the cache directly for scrub.
    if (auto *vb = dynamic_cast<DualVideoDecoder *>(m_sourceB.get())) {
        m_scrubB = makeDualScrubDecoder(vb, this);
        if (!m_scrubB->open(path)) {
            qWarning("DualPlaybackController::swapB: scrub B open failed");
            m_scrubB.reset();
        }
    }

    // Recompute master frame count (might extend past prior max).
    // Per-side counts are in their own fps — must convert via
    // duration before comparing in master-fps space, otherwise
    // mixed-fps pairs under-size the master range. See open() for
    // the full rationale.
    const double fpsA = m_sourceA ? m_sourceA->fps() : 0.0;
    const double fpsB = m_sourceB ? m_sourceB->fps() : 0.0;
    int countA = m_sourceA ? m_sourceA->frameCount() : 0;
    int countB = m_sourceB ? m_sourceB->frameCount() : 0;
    const double durA = (fpsA > 0.0) ? countA / fpsA : 0.0;
    const double durB = (fpsB > 0.0) ? countB / fpsB : 0.0;
    const double maxDur = std::max(durA, durB);
    m_masterFrameCount = (maxDur > 0.0 && m_masterFps > 0.0)
        ? std::max(1, static_cast<int>(std::round(maxDur * m_masterFps)))
        : 0;

    // Sync the new B to the current playhead so it starts buffering
    // around where the user is, not at frame 0; and pre-warm its scrub
    // decoder (if video) so the next scrub gesture is hot.
    if (m_timer) {
        const int master = m_timer->currentFrame();
        m_sourceB->setDecodeTarget(master);
        if (m_scrubB) {
            int sf = translateMasterToSourceFrame(master, 'B');
            if (sf < 0) sf = nextClipSourceFrame(master, 'B');
            if (sf >= 0) m_scrubB->requestFrame(sf);
        }
    }

    emit frameCountChanged();
    emit hwAccelChanged();
    qInfo("DualPlaybackController::swapB: now using %s",
          qPrintable(QFileInfo(path).fileName()));
    return true;
}

void DualPlaybackController::applyAdaptiveThreading()
{
    // Pre-open halving happens in open() now (it has to — image-seq
    // sources spawn their worker pool at open() time, so post-open
    // setThreadCount no-ops). This method is kept for symmetry with
    // any future post-open adjustments (e.g. swapB into a second
    // image-seq retroactively halving the pool); today it's a stub.
}

void DualPlaybackController::play()
{
    if (!m_open.load(std::memory_order_acquire)) return;
    if (m_timer->isPlaying()) return;
    m_timer->play();
    if (m_audio) m_audio->play();
    {
        std::lock_guard<std::mutex> lk(m_pumpCvMutex);
    }
    m_pumpCv.notify_one();
    emit isPlayingChanged();
}

void DualPlaybackController::pause()
{
    if (!m_open.load(std::memory_order_acquire)) return;
    if (!m_timer->isPlaying()) return;
    m_timer->pause();
    if (m_audio) m_audio->pause();
    emit isPlayingChanged();
}

void DualPlaybackController::seekToFrame(int frameNumber)
{
    if (!m_open.load(std::memory_order_acquire)) return;
    if (frameNumber < 0) frameNumber = 0;
    if (m_masterFrameCount > 0 && frameNumber >= m_masterFrameCount) {
        frameNumber = m_masterFrameCount - 1;
    }
    m_timer->seekToFrame(frameNumber);

    // Phase 7.8 — translate the master frame to per-side source
    // frame BEFORE seeking. Without this, the source decoder seeks
    // to (master frame as source frame) — wrong position when
    // sourceIn != 0. Then the pump's setDecodeTarget(translated)
    // would trigger a SECOND seek to fix it. Two seeks back-to-
    // back leaves a brief window where the compositor's cachedX
    // (last-good-frame for transient hiccups) shows pre-edit
    // content — that's the "flash frame" the user sees.
    if (m_sourceA) {
        int sf = translateMasterToSourceFrame(frameNumber, 'A');
        if (sf < 0) sf = nextClipSourceFrame(frameNumber, 'A');
        if (sf >= 0) m_sourceA->seekTo(sf);
    }
    if (m_sourceB) {
        int sf = translateMasterToSourceFrame(frameNumber, 'B');
        if (sf < 0) sf = nextClipSourceFrame(frameNumber, 'B');
        if (sf >= 0) m_sourceB->seekTo(sf);
    }

    // Audio mirrors the master playhead, but per-side translated:
    // each side seeks to its own source-frame offset so trimmed /
    // slipped clips play the right audio range. -1 means "this
    // side is in a timeline gap" → the mixer mutes it until master
    // re-enters a clip range.
    if (m_audio) {
        auto perSideSeconds = [this, frameNumber](char side,
                                                  IDualSource *src) -> double {
            if (!src || src->fps() <= 0.0) return -1.0;
            const int sf = translateMasterToSourceFrame(frameNumber, side);
            if (sf < 0) return -1.0;
            return static_cast<double>(sf) / src->fps();
        };
        m_audio->seekPerSide(perSideSeconds('A', m_sourceA.get()),
                              perSideSeconds('B', m_sourceB.get()));
    }

    {
        std::lock_guard<std::mutex> lk(m_pumpCvMutex);
    }
    m_pumpCv.notify_one();
}

void DualPlaybackController::togglePlayback()
{
    if (isPlaying()) pause();
    else play();
}

// ---------------------------------------------------------------------------
// Scrub gesture API. Mirrors single-flow's WindowManager → ScrubDecoder
// pattern: pause streaming + audio, fire scrub-decoder requests during
// drag, warm-seek on release. Idempotent — safe to call beginScrub
// while already scrubbing.
// ---------------------------------------------------------------------------

void DualPlaybackController::beginScrub()
{
    if (!m_open.load(std::memory_order_acquire)) return;
    if (m_scrubActive.exchange(true, std::memory_order_acq_rel)) return;

    // Park each video source's streaming decode thread. Codec contexts
    // stay hot for the release-time seek. Image-seq sources don't have
    // a streaming decode loop — they're random-access via the cache,
    // so the scrubActive flag doesn't apply.
    if (auto *va = dynamic_cast<DualVideoDecoder *>(m_sourceA.get())) {
        va->setScrubActive(true);
    }
    if (auto *vb = dynamic_cast<DualVideoDecoder *>(m_sourceB.get())) {
        vb->setScrubActive(true);
    }

    // Pause audio for the duration of the gesture. Mute state on
    // m_mutedA / m_mutedB atomics is preserved (DualAudioMixer::pause
    // only toggles m_playing). QML caller resumes via
    // WindowManager.play() on release if was-playing.
    if (m_audio) m_audio->pause();
}

void DualPlaybackController::requestScrubFrame(int masterFrame)
{
    if (!m_open.load(std::memory_order_acquire)) return;
    if (!m_scrubActive.load(std::memory_order_acquire)) return;

    // Move the dual timer so the compositor's pullFrameA/B sees the
    // scrub position. Critical for image-seq sides: their fallthrough
    // path is m_sourceA->getBufferedFrame(translatedSrcFrame), where
    // translatedSrcFrame is derived from m_timer->currentFrame() via
    // translateMasterToSourceFrame — if the timer stays stale the
    // cache gets pulled at the OLD frame even though the right one
    // is loaded. Video sides return the scrub slot directly so they
    // happen to work without this, but we want a coherent position
    // for the QML scrubber readout too.
    //
    // We use the timer's own seekToFrame, NOT controller::seekToFrame —
    // the controller version would also call each source's seekTo()
    // which sets m_pendingSeekTarget and wakes the streaming decode
    // thread through the scrub gate (pendingSeek bypasses the
    // !scrubActive predicate). The timer-only update keeps the
    // streaming decoders parked.
    if (m_timer) m_timer->seekToFrame(masterFrame);

    // Per-side dispatch. Video sides → scrub decoder. Image-seq sides
    // → setDecodeTarget so the cache prefetch window follows the
    // user's scrub (cache reads remain random-access; this just keeps
    // the LRU centered around the drag position).
    auto dispatch = [&](char side,
                        IDualSource *src,
                        IDualScrubDecoder *scrub) {
        if (!src) return;
        int sf = translateMasterToSourceFrame(masterFrame, side);
        if (sf < 0) sf = nextClipSourceFrame(masterFrame, side);
        if (sf < 0) return;   // gap with no upcoming clip
        if (scrub) {
            scrub->requestFrame(sf);
        } else {
            // Image-seq side — keep the cache window aligned.
            src->setDecodeTarget(sf);
        }
    };
    dispatch('A', m_sourceA.get(), m_scrubA.get());
    dispatch('B', m_sourceB.get(), m_scrubB.get());
}

void DualPlaybackController::endScrub(int finalMasterFrame)
{
    if (!m_open.load(std::memory_order_acquire)) return;
    if (!m_scrubActive.exchange(false, std::memory_order_acq_rel)) return;

    // Wake streaming decoders — they parked on the scrubActive gate
    // in their CV predicate. They'll now see needsMoreFrames() and
    // re-engage.
    if (auto *va = dynamic_cast<DualVideoDecoder *>(m_sourceA.get())) {
        va->setScrubActive(false);
    }
    if (auto *vb = dynamic_cast<DualVideoDecoder *>(m_sourceB.get())) {
        vb->setScrubActive(false);
    }

    // Warm-seek both sources to the landing position. seekToFrame
    // routes through each IDualSource::seekTo, which the existing
    // path does for transport seeks. Streaming codec contexts are
    // still hot — this is the cheap warm seek, not a cold open.
    seekToFrame(finalMasterFrame);

    // Audio resume is the QML caller's responsibility via
    // WindowManager.play() — they know wasPlayingBeforeScrub.
}

int DualPlaybackController::currentFrame() const
{
    return m_timer ? m_timer->currentFrame() : 0;
}

int DualPlaybackController::frameCount() const { return m_masterFrameCount; }
double DualPlaybackController::fps()        const { return m_masterFps; }

QString DualPlaybackController::hwAccel() const
{
    auto label = [](IDualSource *s) -> QString {
        if (!s) return QStringLiteral("—");
        const QString name = s->hwAccelName();
        return name.isEmpty() ? QStringLiteral("software") : name;
    };
    return QStringLiteral("A: %1  |  B: %2")
        .arg(label(m_sourceA.get()), label(m_sourceB.get()));
}
bool   DualPlaybackController::isPlaying() const
{
    return m_timer && m_timer->isPlaying();
}

std::shared_ptr<DualFrame>
DualPlaybackController::pullFrameA(int masterFrame) const
{
    if (!m_sourceA) return nullptr;
    // Scrub gate — when scrubActive AND this side is a video source,
    // the per-side scrub publish slot supersedes the streaming ring.
    // Image-seq sources fall through to the cache's normal random-
    // access read (the cache IS the scrub engine there).
    if (m_scrubActive.load(std::memory_order_acquire)) {
        if (auto *va = dynamic_cast<DualVideoDecoder *>(m_sourceA.get())) {
            auto f = va->getScrubFrame();
            if (f) return f;
            // No scrub frame yet — fall through; compositor's cachedA
            // (last good) keeps painting until the worker publishes.
        }
    }
    const int srcFrame = translateMasterToSourceFrame(masterFrame, 'A');
    if (srcFrame < 0) return nullptr;
    // getClosestFrame == exact for the video decoder; for image-seq it
    // returns the nearest cached frame so a sparse cache-stride window
    // still displays (no blank on off-grid targets).
    return m_sourceA->getClosestFrame(srcFrame);
}

std::shared_ptr<DualFrame>
DualPlaybackController::pullFrameB(int masterFrame) const
{
    if (!m_sourceB) return nullptr;
    if (m_scrubActive.load(std::memory_order_acquire)) {
        if (auto *vb = dynamic_cast<DualVideoDecoder *>(m_sourceB.get())) {
            auto f = vb->getScrubFrame();
            if (f) return f;
        }
    }
    const int srcFrame = translateMasterToSourceFrame(masterFrame, 'B');
    if (srcFrame < 0) return nullptr;
    return m_sourceB->getClosestFrame(srcFrame);
}

QString DualPlaybackController::formatTimecode(int masterFrame) const
{
    if (m_masterFps <= 0.0 || masterFrame < 0) return QString();
    // Pick a (num, den) that approximates the master fps with
    // common video rates. Non-DF — dual mode doesn't carry per-
    // source DF flags. 23.976 → (24000, 1001), 29.97 → (30000,
    // 1001), etc.
    const double fps = m_masterFps;
    int num = 0, den = 1;
    if (std::abs(fps - 23.976) < 0.01)      { num = 24000; den = 1001; }
    else if (std::abs(fps - 29.97)  < 0.01) { num = 30000; den = 1001; }
    else if (std::abs(fps - 47.952) < 0.01) { num = 48000; den = 1001; }
    else if (std::abs(fps - 59.94)  < 0.01) { num = 60000; den = 1001; }
    else                                     { num = static_cast<int>(std::lround(fps));
                                               den = 1; }
    TimecodeFormatter f(num, den, /*dropFrame=*/false);
    if (!f.isValid()) return QString();
    return f.format(masterFrame);
}

int DualPlaybackController::translateMasterToSourceFrame(int masterFrame,
                                                           char trackSide) const
{
    // Identity when no timeline is wired — preserves Phase 7.7
    // behavior so tests/early-stage paths still work.
    if (!m_timeline) return masterFrame;
    if (masterFrame < 0) return -1;

    const double fps = (m_masterFps > 0.0) ? m_masterFps : 24.0;
    const double masterSec = static_cast<double>(masterFrame) / fps;

    const QString trackId = (trackSide == 'A')
        ? QStringLiteral("A") : QStringLiteral("B");

    const Clip *clip = TimelineFlattener::visibleClipOnTrack(
        m_timeline->timeline(), trackId, masterSec);
    if (!clip) return -1;   // gap → caller renders transparent

    const double sourceSec = (masterSec - clip->startTime) + clip->sourceIn;

    // Use the source's actual fps (most accurate); fall back to
    // clip's cached sourceFps. Don't fall back to master fps —
    // mixed-fps pairs (e.g. 23.976 A + 50 B) would silently translate
    // the source frame at the WRONG rate, putting decoders at random
    // positions inside the clip. Better to refuse the translation
    // and surface the missing-metadata bug than to corrupt position.
    const IDualSource *src = (trackSide == 'A') ? m_sourceA.get()
                                                 : m_sourceB.get();
    double srcFps = src ? src->fps() : 0.0;
    if (srcFps <= 0.0) srcFps = clip->sourceFps;
    if (srcFps <= 0.0) {
        static std::atomic<bool> sWarned{false};
        if (!sWarned.exchange(true)) {
            qWarning("DualPlaybackController::translateMasterToSourceFrame: "
                     "side=%c has no fps (source.fps=0, clip.sourceFps=0) — "
                     "refusing to translate. Decoder will hold its previous "
                     "position. This warning fires once per session.",
                     trackSide);
        }
        return -1;
    }

    const int srcFrame = static_cast<int>(std::lround(sourceSec * srcFps));
    return std::max(0, srcFrame);
}

int DualPlaybackController::nextClipSourceFrame(int masterFrame,
                                                    char trackSide) const
{
    if (!m_timeline) return -1;
    const QString trackId = (trackSide == 'A')
        ? QStringLiteral("A") : QStringLiteral("B");
    const double fps = (m_masterFps > 0.0) ? m_masterFps : 24.0;
    const double sec = static_cast<double>(masterFrame) / fps;

    // Find the upcoming non-gap clip with the smallest startTime
    // that's >= sec. (If a clip CONTAINS sec, translateMasterToSourceFrame
    // would have returned a valid frame already and we wouldn't be
    // calling this; here we only care about clips strictly ahead.)
    const Clip *next = nullptr;
    for (const Track &track : m_timeline->timeline().tracks) {
        if (track.id != trackId) continue;
        for (const Clip &c : track.clips) {
            if (c.isGap) continue;
            if (c.startTime + c.duration <= sec) continue;   // already past
            if (!next || c.startTime < next->startTime) next = &c;
        }
        break;
    }
    if (!next) return -1;

    const IDualSource *src = (trackSide == 'A') ? m_sourceA.get()
                                                 : m_sourceB.get();
    double srcFps = src ? src->fps() : 0.0;
    if (srcFps <= 0.0) srcFps = next->sourceFps;
    if (srcFps <= 0.0) {
        // Match translateMasterToSourceFrame's policy — refuse to
        // guess when neither source nor clip has a known fps.
        // Decoder won't pre-warm; first frame after the clip
        // boundary will be a normal seek instead.
        return -1;
    }

    // Decoder pre-warms around the clip's start-source frame.
    return std::max(0, static_cast<int>(std::lround(next->sourceIn * srcFps)));
}

int DualPlaybackController::firstClipSourceFrame(char trackSide) const
{
    if (!m_timeline) return -1;
    const QString trackId = (trackSide == 'A')
        ? QStringLiteral("A") : QStringLiteral("B");
    const Clip *first = nullptr;
    for (const Track &track : m_timeline->timeline().tracks) {
        if (track.id != trackId) continue;
        for (const Clip &c : track.clips) {
            if (c.isGap) continue;
            if (!first || c.startTime < first->startTime) first = &c;
        }
        break;
    }
    if (!first) return -1;
    const IDualSource *src = (trackSide == 'A') ? m_sourceA.get()
                                                 : m_sourceB.get();
    double srcFps = src ? src->fps() : 0.0;
    if (srcFps <= 0.0) srcFps = first->sourceFps;
    if (srcFps <= 0.0) return -1;
    return std::max(0, static_cast<int>(std::lround(first->sourceIn * srcFps)));
}

void DualPlaybackController::setTimeline(qcv::TimelineController *t)
{
    m_timeline = t;
    if (t) {
        // Bump the generation on every timeline mutation so the
        // compositor drops its stale cached MTLTextures. Direct
        // connection — runs on whichever thread emitted the
        // signal; atomic counter is thread-safe.
        QObject::connect(t, &qcv::TimelineController::timelineChanged,
                          this, [this, t]() {
            m_timelineGeneration.fetch_add(1,
                std::memory_order_acq_rel);

            // H.6 (2026-05-14) — recompute master frame count from
            // the timeline's effective duration so clip trim/slip
            // updates the loop wrap point + scrubber end. Without
            // this, masterFrameCount stays at max(sourceA,sourceB)
            // even after the timeline shortens, and playback wraps
            // past the visible timeline end (every side renders
            // transparent past the new end → blank viewport).
            const double dur = t->duration();
            if (dur > 0.0 && m_masterFps > 0.0) {
                const int newCount = std::max(1, static_cast<int>(
                    std::round(dur * m_masterFps)));
                if (newCount != m_masterFrameCount) {
                    m_masterFrameCount = newCount;
                    emit frameCountChanged();
                }
            }
        });
    }
}

std::shared_ptr<DualFrame> DualPlaybackController::pullFrameA() const
{
    if (!m_timer) return nullptr;
    return pullFrameA(m_timer->currentFrame());
}

std::shared_ptr<DualFrame> DualPlaybackController::pullFrameB() const
{
    if (!m_timer) return nullptr;
    return pullFrameB(m_timer->currentFrame());
}

bool DualPlaybackController::aPastEnd(int masterFrame) const
{
    if (!m_sourceA) return true;
    // Phase 7.8 — "past end" semantics extend to any time master
    // is outside this side's clip range on the timeline. Gaps
    // before/after/between clips all render transparent on that
    // side. Falls back to source-frameCount when no timeline is
    // wired (Phase 7.7 behavior, identity translation).
    if (m_timeline) {
        const double fps = (m_masterFps > 0.0) ? m_masterFps : 24.0;
        const double sec = static_cast<double>(masterFrame) / fps;
        return TimelineFlattener::visibleClipOnTrack(
                   m_timeline->timeline(),
                   QStringLiteral("A"), sec) == nullptr;
    }
    const int count = m_sourceA->frameCount();
    return count > 0 && masterFrame >= count;
}

bool DualPlaybackController::bPastEnd(int masterFrame) const
{
    if (!m_sourceB) return true;
    if (m_timeline) {
        const double fps = (m_masterFps > 0.0) ? m_masterFps : 24.0;
        const double sec = static_cast<double>(masterFrame) / fps;
        return TimelineFlattener::visibleClipOnTrack(
                   m_timeline->timeline(),
                   QStringLiteral("B"), sec) == nullptr;
    }
    const int count = m_sourceB->frameCount();
    return count > 0 && masterFrame >= count;
}

void DualPlaybackController::clockPumpLoop()
{
    constexpr auto kPumpInterval = std::chrono::milliseconds(16);   // ~60 Hz

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        const int target = m_timer->update();

        // Per-side translation — when a clip's startTime / sourceIn
        // is non-zero (post-edit), the master frame maps to a
        // different source frame on each side. Identity when no
        // timeline wired (Phase 7.7 behavior).
        //
        // In a gap (no clip covers master), pre-warm the decoder
        // by pointing it at the next upcoming clip's start frame.
        // Without this pre-warm, when master enters the clip the
        // decoder has to seek + burst-decode from frame 0 → first-
        // frame stall. With pre-warm, the decoder is already
        // buffering around the right area when the clip becomes
        // visible.
        // Loop pre-warm: when a side is past-end (or in a gap with
        // no upcoming clip) AND the master timer has a loop range,
        // point the cache at the FIRST clip's start so it's ready
        // for the wrap. Without this, mismatched-duration pairs
        // (e.g. short image-seq A + long video B in loop mode)
        // would stop prefetching A during master frames past A's
        // end, and the wrap to master=0 would show a cold-start
        // miss on A.
        const bool hasLoop =
            m_timer && m_timer->hasLoopRange();
        int translatedA = -1, translatedB = -1;
        if (m_sourceA) {
            translatedA = translateMasterToSourceFrame(target, 'A');
            int decodeA = translatedA;
            if (decodeA < 0) decodeA = nextClipSourceFrame(target, 'A');
            if (decodeA < 0 && hasLoop) decodeA = firstClipSourceFrame('A');
            if (decodeA >= 0) m_sourceA->setDecodeTarget(decodeA);
        }
        if (m_sourceB) {
            translatedB = translateMasterToSourceFrame(target, 'B');
            int decodeB = translatedB;
            if (decodeB < 0) decodeB = nextClipSourceFrame(target, 'B');
            if (decodeB < 0 && hasLoop) decodeB = firstClipSourceFrame('B');
            if (decodeB >= 0) m_sourceB->setDecodeTarget(decodeB);
        }

        // Edit-aware audio sync. Pass per-side translated source
        // seconds; the mixer flips its gap flags so processAudio
        // mutes that side when master is outside the clip range,
        // and re-seeks the decoder on a gap → clip transition.
        // Negative position = "in gap on this side".
        if (m_audio) {
            auto sec = [](IDualSource *src, int sf) -> double {
                if (!src || sf < 0 || src->fps() <= 0.0) return -1.0;
                return static_cast<double>(sf) / src->fps();
            };
            m_audio->updatePerSide(sec(m_sourceA.get(), translatedA),
                                     sec(m_sourceB.get(), translatedB));
        }

        const int prev = m_lastEmittedFrame.exchange(target,
                                                      std::memory_order_acq_rel);
        if (prev != target) {
            // Coalesce signal emission on UI thread via Qt's queued
            // connection — emit unconditionally, the slot will be
            // dispatched cross-thread.
            emit currentFrameChanged();
        }

        std::unique_lock<std::mutex> lk(m_pumpCvMutex);
        m_pumpCv.wait_for(lk, kPumpInterval, [this] {
            return m_stopRequested.load(std::memory_order_acquire);
        });
    }
}

} // namespace qcv::dual
