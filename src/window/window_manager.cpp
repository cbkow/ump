#include "window_manager.h"

#if !defined(Q_OS_MACOS) && !defined(__APPLE__)
#include "window_manager_dual_source_adapter.h"
#endif
#include "native_fullscreen_macos.h"
#include "sparkle_updater_macos.h"
#include "native_fullscreen_win.h"
#include "audio/audio_player.h"
#include "audio/shuttle_audio_engine.h"
#include "color/ocio_config_manager.h"
#include "color/preset_manager.h"
#include "decode/frame_handle.h"
#include "decode/image_loader.h"
#include "decode/image_sequence_cache.h"
#include "decode/scrub_decoder.h"
#include "decode/video_decoder.h"
#include "dual/dual_image_seq_source.h"
#include "dual/dual_playback_controller.h"
#include "dual/dual_playback_timer.h"
#include "project/project_manager.h"
#include "timeline/playback_timer.h"
#include "timeline/timeline_controller.h"

#ifdef QCV_NATIVE_PLAYER
#  include "render/iplayer_renderer.h"
#  include "render/player_window.h"
#  if defined(Q_OS_WIN)
#    include "render/d3d11/d3d11_player_renderer.h"
#  endif
#endif

#include "render/backdrop_image_provider.h"

#include "annotations/annotation_exporter.h"
#include "annotations/annotation_manager.h"
#include "annotations/annotation_serializer.h"
#include "annotations/annotation_thumbnail.h"
#include "annotations/stroke_tessellator.h"
#include "annotations/viewport_annotator.h"

#include <QClipboard>
#include <QColorSpace>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFile>
#include <QFileOpenEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileInfo>
#include <QProcess>
#include <QGuiApplication>
#include <QImage>
#include <QImageWriter>
#include <QPainter>
#include <QFont>
#include <QColor>
#include <QKeyEvent>
#include <QPointer>
#include <QtConcurrent>
#include <QDateTime>
#include <QQmlApplicationEngine>
#include <QSettings>
#include <QStandardPaths>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QSurfaceFormat>
#include <QTimer>
#include <QtLogging>
#include <utility>

#if defined(Q_OS_WIN)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace qcv {

// Forward decl — defined in an anonymous namespace lower in this TU,
// but used by the loadRequested handler in the constructor below.
namespace { QString buildViewportNoticeText(const MediaItem &item); }

WindowManager::WindowManager(QQmlApplicationEngine *engine, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_videoDecoder(new VideoDecoder(this))
    , m_videoDecoderB(new VideoDecoder(this))
    , m_scrubDecoder(new ScrubDecoder(m_videoDecoder, this))
    , m_ocio(new OCIOConfigManager(this))
    , m_presets(new PresetManager(m_ocio, this))
    , m_audio(new AudioPlayer(this))
    , m_project(new ProjectManager(this))
    , m_timeline(new TimelineController(this))
    , m_annotator(std::make_unique<ViewportAnnotator>())
    , m_annotationManager(std::make_unique<AnnotationManager>(this))
    , m_safetyOverlay(std::make_unique<SafetyOverlay>())
    , m_thumbCache(std::make_unique<TimelineThumbnailCache>())
{
    if (!m_audio->initialize()) {
        qWarning("WindowManager: AudioPlayer init failed — playback will be silent");
    }

    // Seed the process-wide scrub-audio mute from the persisted
    // setting — the engines only re-check the flag, never QSettings.
    ShuttleAudioEngine::setGlobalMute(scrubAudioMuted());

    // Restore persisted display brightness (default 1.0 = identity).
    // Renderer setBrightness fires later when the renderer is wired
    // — m_brightness is read directly by the wiring sites.
    m_brightness = QSettings().value(
        QStringLiteral("display/brightness"), 1.0).toDouble();
    if (m_brightness < 0.1) m_brightness = 0.1;
    if (m_brightness > 5.0) m_brightness = 5.0;

    // Phase 3.H.5 — hover-thumbnail cache. Initialized at the
    // timeline's master fps; the worker pool starts immediately so
    // the first hover doesn't pay thread-spawn cost. Drain timer
    // pulls completed uploads off the worker thread onto the GUI
    // thread (cache insertion is mutex-protected but cheap).
    m_thumbCache->initialize(m_timeline ? m_timeline->frameRate()
                                          : 24.0);
    m_thumbUploadDrainTimer = new QTimer(this);
    m_thumbUploadDrainTimer->setInterval(33);   // ~30 Hz
    m_thumbUploadDrainTimer->setTimerType(Qt::CoarseTimer);

    // Audio meter pump — same cadence as the thumb drain. Always
    // running while WindowManager is alive; pollAudioMeters() is a
    // cheap atomic-load loop and the threshold inside suppresses
    // QML rebinds when nothing's moved. Keeping it always-on (vs
    // start/stop on hasAudio) avoids a class of "first frame after
    // open shows wrong values" edge cases for negligible cost.
    m_audioMeterTimer = new QTimer(this);
    m_audioMeterTimer->setInterval(33);
    m_audioMeterTimer->setTimerType(Qt::CoarseTimer);
    connect(m_audioMeterTimer, &QTimer::timeout,
            this, &WindowManager::pollAudioMeters);
    m_audioMeterTimer->start();
    connect(m_thumbUploadDrainTimer, &QTimer::timeout, this, [this] {
        if (!m_thumbCache) return;
        m_thumbCache->processPendingUploads();
        // Re-poll the active hover keys so the renderer picks up the
        // texture handles once the workers finish. Without this a
        // still-cursor user would never see the thumb after the
        // initial sync miss. Both A and B are polled — B's path is
        // empty in non-dual modes so the inner branch is skipped.
        if (!m_hoverActive.load(std::memory_order_acquire)) return;

        bool changed = false;
        auto repoll = [&](const std::string &path, int frame,
                          std::atomic<std::uint64_t> &handleOut,
                          std::atomic<int> &wOut,
                          std::atomic<int> &hOut) {
            if (path.empty()) return;
            int w = 0, h = 0;
            const std::uint64_t h_id =
                m_thumbCache->getThumbnail(path, frame, w, h,
                                            /*allow_fallback=*/true);
            if (h_id != handleOut.load(std::memory_order_relaxed)) {
                handleOut.store(h_id, std::memory_order_release);
                wOut.store(w, std::memory_order_relaxed);
                hOut.store(h, std::memory_order_relaxed);
                changed = true;
            }
        };
        repoll(m_hoverPath,  m_hoverFrame,
               m_currentHoverThumbHandle,
               m_currentHoverThumbW, m_currentHoverThumbH);
        repoll(m_hoverPathB, m_hoverFrameB,
               m_currentHoverThumbHandleB,
               m_currentHoverThumbWB, m_currentHoverThumbHB);
        if (changed) pushHoverThumbToRenderer();
    });
    m_thumbUploadDrainTimer->start();

    // Restore persisted display preferences. QSettings uses the
    // org/app name set in main.cpp (~/Library/Preferences on macOS).
    // Read here so the renderer's initial setBackgroundMode call in
    // initialize() picks up the saved value.
    QSettings settings;
    const int storedBg = settings.value(
        QStringLiteral("display/backgroundMode"),
        static_cast<int>(BackgroundMode::Black)).toInt();
    if (storedBg >= 0 && storedBg <= 3) {
        m_backgroundMode = storedBg;
    }
    // Loop defaults to ON for first-run users — QC playback rhythm
    // is "press play, watch a cycle, scrub, watch again", not
    // "watch once and stop". Setting persists across launches once
    // the user toggles it off explicitly.
    m_loopEnabled =
        settings.value(QStringLiteral("playback/loopEnabled"), true).toBool();
    if (m_timeline) m_timeline->timer()->setLooping(m_loopEnabled);
    if (m_imageSeqCache) m_imageSeqCache->setLooping(m_loopEnabled);

    // Phase 3.H.6 Stage A — annotation backbone wiring.
    //
    // Stroke completion → AnnotationManager: every time the user
    // releases the pointer in annotation mode, the annotator hands
    // us the finalized stroke. We add a note at the current frame
    // (or fold into an existing note) and update its annotation_data
    // with the serialized stroke list. AnnotationManager auto-saves
    // the sidecar.
    if (m_annotator) {
        m_annotator->setStrokeFinalizedCallback(
            [this](std::unique_ptr<ActiveStroke> stroke) {
                onStrokeFinalized(std::move(stroke));
            });
        m_annotator->setEraseAtCallback(
            [this](QPointF normPos) { onEraseAt(normPos); });
        // A new Press cancels any pending debounced annotated-thumb
        // capture. Without this, the 400 ms timer armed by stroke A's
        // release can fire mid-way through stroke B and the
        // captureScreenshot render-fence wait stalls the GUI thread,
        // making stroke B's first events arrive late.
        m_annotator->setStrokeStartedCallback([this] {
            if (m_annotatedSaveTimer && m_annotatedSaveTimer->isActive()) {
                m_annotatedSaveTimer->stop();
            }
        });
    }
    // Annotated-thumbnail debouncer. Stage A captured the thumb
    // synchronously inside onStrokeFinalized which made fast
    // multi-stroke drawing feel laggy (each release blocked the
    // GUI thread up to 250 ms while captureScreenshot waited for
    // the next presented frame). Now onStrokeFinalized just
    // restarts this timer; the actual capture + save happens
    // ~400 ms after the user stops drawing.
    m_annotatedSaveTimer = new QTimer(this);
    m_annotatedSaveTimer->setSingleShot(true);
    m_annotatedSaveTimer->setInterval(400);
    connect(m_annotatedSaveTimer, &QTimer::timeout, this, [this] {
        if (m_pendingAnnotatedTimecode.isEmpty()) return;
        const QString tc = m_pendingAnnotatedTimecode;
        m_pendingAnnotatedTimecode.clear();
        // saveNoteAnnotatedThumbnail offloads the PNG write to a
        // worker thread and posts annotatedThumbReady back on the
        // GUI thread once the file is on disk; emitting here would
        // race the write and refresh NotesPanel against the prior
        // PNG (visible as "one stroke behind").
        saveNoteAnnotatedThumbnail(tc);
    });
    // Lifecycle: load notes when the active media changes; clear
    // when entering modes where annotations are disabled (dual /
    // playlist).
    if (m_project) {
        connect(m_project, &ProjectManager::activeItemIdChanged,
                this, &WindowManager::syncAnnotationManagerToActiveMedia);
        // Phase 7.6 — Recent lists. Each successful media add or
        // project save / open bumps the path to the top of its
        // list (capped to 10).
        connect(m_project, &ProjectManager::mediaFileAdded,
                this, &WindowManager::addRecentMedia);
        connect(m_project, &ProjectManager::projectSaved,
                this, &WindowManager::addRecentProject);
        connect(m_project, &ProjectManager::projectOpened,
                this, &WindowManager::addRecentProject);

        // New Project / Open Project replace the pool wholesale, so the
        // old project's B-source no longer exists. Drop dual view here —
        // BEFORE applyLoadedState's restored-active-item loadRequested
        // fires — so the dual-persist branch in the loadRequested handler
        // sees single mode and doesn't try to re-pair the new project's A
        // with the previous project's B. Fired synchronously (same
        // thread) ahead of loadRequested, so ordering holds.
        connect(m_project, &ProjectManager::projectReplaced, this, [this] {
            // A new project's saved views are unrelated to the old
            // one — drop any binding so we don't "Update" a stale id.
            setActiveDualViewId({});
            if (m_compositorMode != 0 || m_dualController) {
                tearDownDualIslandToSingleState();
                closeActiveMedia();
            }
        });

        // Viewport-notice reconciliation. onVideoMetadataReady re-fires
        // activeItemIdChanged when async metadata lands for the active
        // item; if it resolves to an undecodable raw, (re)show / upgrade
        // the notice. Handles the "viewport loaded before metadata"
        // race — the load-failure path shows a generic notice first,
        // this upgrades it to the camera-named one. Set-only (never
        // clears) so it can't race the load path.
        connect(m_project, &ProjectManager::activeItemIdChanged, this, [this] {
            evaluateViewportNoticeFor(m_project->activeItemId());
            // Per-clip pixel-aspect (anamorphic un-squeeze). This signal
            // fires on load, clip switch, async-metadata-ready (Detected
            // SAR lands), and setPixelAspect edits — so re-pushing the
            // effective ratio here keeps the A side correct in every
            // case. Cheap (atomic stores); no decoder involvement.
            applyPixelAspectToRenderer();
        });
        // B side tracks the dual B source the same way.
        connect(m_project, &ProjectManager::bSourceChanged, this, [this] {
            applyPixelAspectToRenderer();
        });
        // A real per-clip PAR edit (pill / custom field) — re-derive the
        // active clip's note thumbnails so the NotesPanel cards + exports
        // track the new aspect. Dedicated signal (not activeItemIdChanged)
        // so this only fires on actual aspect changes, not every rebind.
        connect(m_project, &ProjectManager::pixelAspectChanged, this,
                [this](const QString &itemId, int, int, int) {
            if (m_project && itemId == audioRoutingScopeMediaItemId()) {
                regenerateNoteThumbnailsForActiveClip();
            }
        });
        // A real per-clip rotation edit. Unconditional re-push (cheap
        // atomics) — the mutator's activeItemIdChanged nudge doesn't
        // fire in playlist mode, where the edited clip's id is the
        // playing clip inside the playlist, not the active item.
        connect(m_project, &ProjectManager::rotationOverrideChanged, this,
                [this](const QString &, int) {
            applyPixelAspectToRenderer();
        });

        // Per-clip audio routing changes — the inspector pill in
        // ImageSequenceInspector / InspectorPanel calls
        // setAudioRoutingMode on ProjectManager which mutates the
        // MediaItem and emits this signal. We route to the right
        // live decoder so the swap is audible immediately rather
        // than waiting for the next media open.
        connect(m_project, &ProjectManager::audioRoutingModeChanged,
                this, [this](const QString &itemId, int mode) {
            // Dual mode: match against A and B independently. A clip
            // can be loaded on both sides simultaneously (rare but
            // possible) — push to whichever side(s) match.
            if (m_dualController && m_dualController->audio()) {
                if (m_project && m_project->activeItemId() == itemId) {
                    m_dualController->audio()->setRoutingModeA(mode);
                }
                if (m_project && m_project->bSourceMediaId() == itemId) {
                    m_dualController->audio()->setRoutingModeB(mode);
                }
                return;
            }
            // Single mode: only push when this id matches the routing
            // scope (active clip's source MediaItem in playlist mode,
            // activeItemId otherwise). The earlier comparison against
            // activeItemId() dropped routing changes in playlist mode
            // because activeItemId is the playlist, not the underlying
            // clip the user edited via the inspector pill.
            if (m_audio && m_project
                && audioRoutingScopeMediaItemId() == itemId) {
                m_audio->setRoutingMode(mode);
            }
        });

        // Per-clip videoRangeOverride changed. Same shape as the
        // audio handler above: dual mode pushes per-side from
        // matching id; single mode pushes to the live VideoDecoder
        // when the changed id is the active item — resolved through
        // audioRoutingScopeMediaItemId() so playlist mode pulls the
        // active clip's source MediaItem rather than the playlist
        // container (whose own videoRangeOverride is always Auto).
        connect(m_project, &ProjectManager::videoRangeOverrideChanged,
                this, [this](const QString &itemId, int range) {
            if (m_dualController) {
                if (m_project && m_project->activeItemId() == itemId) {
                    m_dualController->setRangeOverrideA(range);
                }
                if (m_project && m_project->bSourceMediaId() == itemId) {
                    m_dualController->setRangeOverrideB(range);
                }
                // No explicit requestUpdate — the dual pump's
                // currentFrameChanged at ~60 Hz already triggers a
                // render-thread wake (line 1545's requestUpdate); the
                // adapter reads the new override on the next pull.
                // Latency = one pump tick = ~16 ms, imperceptible.
                return;
            }
            if (m_videoDecoder && m_project
                && audioRoutingScopeMediaItemId() == itemId) {
                m_videoDecoder->setRangeOverride(range);
                // Mirror to the scrub decoder so scrubbed frames match
                // playback levels (it shares the same source/clip).
                if (m_scrubDecoder) m_scrubDecoder->setRangeOverride(range);
            }
        });
    }
    // Notes change → rebuild stored-stroke render mesh for the
    // current frame so the renderer picks up additions / edits /
    // deletes immediately.
    if (m_annotationManager) {
        connect(m_annotationManager.get(),
                &AnnotationManager::notesChanged,
                this, [this] {
            rebuildStoredAnnotationMesh();
            emit notesChanged();    // QML bindings re-fetch notesList
        });
    }
    // annotationsAllowedChanged tracks compositor / playlist / item
    // type. We don't have a single signal that catches all three,
    // so piggyback on the relevant ones.
    connect(this, &WindowManager::compositorModeChanged,
            this, &WindowManager::annotationsAllowedChanged);
    if (m_project) {
        connect(m_project, &ProjectManager::activeItemIdChanged,
                this, &WindowManager::annotationsAllowedChanged);
        // activeItem switches can change the audio-routing scope
        // (single mode: scope IS the active item; entering playlist
        // mode: scope flips to the active playlist clip). Playlist
        // clip-to-clip transitions inside a playlist also need to
        // fire this — handled separately in playlistAdvanceToClip
        // since activeItemId stays on the playlist across those.
        connect(m_project, &ProjectManager::activeItemIdChanged,
                this, &WindowManager::audioRoutingScopeChanged);
    }
    // Frame change → re-evaluate which notes apply to the new
    // frame. Hooked off PlaybackTimer::frameAdvanced (works for
    // both video + image-seq via the unified clock).
    if (m_timeline && m_timeline->timer()) {
        connect(m_timeline->timer(),
                &PlaybackTimer::frameAdvanced,
                this, [this](int) { rebuildStoredAnnotationMesh(); });
    }

    // Phase 7.2.d: timeline controller now owns the canonical
    // playback position via its PlaybackTimer. The renderer +
    // audio drift correction read from m_timeline->timer() rather
    // than computing frame/fps inline. VideoDecoder still drives
    // raw frame decode pacing during playback (per its own decode
    // thread); we mirror its progress into the timer below so the
    // timer is the single point of read for "what time is it?"
    //
    // When image sequences land in 7.4 the direction flips: timer
    // becomes the master and the image-sequence loader pulls
    // frames at the timer's pace. Same controller, same audio
    // sync code; only the mirror direction changes.
    connect(m_videoDecoder, &VideoDecoder::metadataChanged, this, [this] {
        // VideoDecoder populates metadata asynchronously after
        // open(). When it lands, build (or refresh) the timeline
        // around the active project item so duration / fps are
        // canonical. Audio decoder's hasAudio is also known by
        // now since it opens via the same sourcePath signal.
        if (m_suppressTimelineRebuild) {
            // Mid-transition inside startImageSequence — the prior
            // VideoDecoder's close() has cleared metadata to zero,
            // and we're about to populate the timeline directly from
            // the new ImageSequenceCache. Skip the spurious rebuild
            // that would briefly empty the timeline.
            return;
        }
        if (m_dualController) {
            // Dual mode owns the timeline tracks. A stray metadata
            // ping from a previously-closed VideoDecoder would
            // wipe both A + B tracks; guard against it.
            return;
        }
        if (m_timeline && m_timeline->isPlaylistMode()) {
            // Phase 3.H.1 — playlist owns its own timeline shape;
            // a metadataChanged from the first-clip VideoDecoder
            // would wholesale replace it with a SingleMedia
            // timeline. Skip.
            return;
        }
        const QString itemId = m_project ? m_project->activeItemId()
                                          : QString();
        if (itemId.isEmpty()) return;
        const MediaItem *it = m_project->findItem(itemId);
        if (!it) return;
        // Audio-only files don't reach this lambda — VideoDecoder's
        // initFFmpeg rejects them with "No video stream found" so
        // metadataChanged never fires. The audio rebuild path is
        // wired off AudioPlayer::hasAudioChanged below.
        if (it->type == MediaType::Audio) return;

        const double fps = m_videoDecoder->fps();
        const int    fc  = m_videoDecoder->frameCount();
        const double dur = (fps > 0.0 && fc > 0)
                           ? static_cast<double>(fc) / fps
                           : 0.0;
        const bool hasAudio = m_audio && m_audio->hasAudio();
        m_timeline->loadSingleMedia(*it, dur, fps, hasAudio);

        // Mismatched-clip-length: if Source B is already loaded
        // when video A's metadata lands, override timer.duration
        // to the longer side and refresh activity flags.
        applyMasterDuration();
        pushSourceActivity();
    });

    // Audio-only timeline rebuild. AudioPlayer::hasAudioChanged
    // fires after open() resolves the file's duration, which is the
    // moment the timeline has enough info to render an audio clip
    // at full duration. VideoDecoder won't help here — its open()
    // bails on audio-only files (no video stream), so this is the
    // only signal that arrives.
    if (m_audio) {
        connect(m_audio, &AudioPlayer::hasAudioChanged, this, [this] {
            if (!m_timeline || !m_audio || !m_project) return;
            if (m_dualController) return;          // dual rejects audio
            if (m_imageSeqActive || m_playlistActive) return;
            if (!m_audio->hasAudio()) return;      // close path — ignore
            const QString itemId = m_project->activeItemId();
            if (itemId.isEmpty()) return;
            const MediaItem *it = m_project->findItem(itemId);
            if (!it || it->type != MediaType::Audio) return;
            // Synthetic 24 fps keeps timer frame granularity
            // aligned with neighboring video clips. Duration comes
            // straight from AudioDecoder's container probe.
            const double dur = m_audio->duration();
            m_timeline->loadSingleMedia(*it, dur, 24.0,
                                          /*hasAudio=*/true);

            // Mark audio mode + start the wallclock pump. Same
            // QTimer the image-seq path uses — its only job is to
            // call PlaybackTimer::update() at ~30 Hz so wall-clock
            // mode actually advances. Without this the playhead
            // sits at 0 even though audio plays through speakers.
            if (!m_audioActive) {
                m_audioActive = true;
                emit audioActiveChanged();
            }
            m_timeline->timer()->setClockPolicy(PlaybackTimer::WallClock);
            if (!m_imageSeqDriverTimer) {
                m_imageSeqDriverTimer = new QTimer(this);
                m_imageSeqDriverTimer->setTimerType(Qt::PreciseTimer);
                connect(m_imageSeqDriverTimer, &QTimer::timeout, this, [this] {
                    if (m_timeline) m_timeline->timer()->update();
                    pollImageSeqBufferStatus();
                    // Audio mode: feed the master clock into
                    // AudioPlayer so its drift-correction realigns
                    // when the timer wraps for loop. Without this
                    // tick, loop appears to "stop": timer resets to
                    // 0 but audio keeps playing past duration into
                    // empty ring buffer → silence.
                    if (m_audioActive && m_audio
                        && m_audio->hasAudio() && m_timeline) {
                        m_audio->update(m_timeline->timer()->position());
                    }
                });
                m_imageSeqDriverTimer->setInterval(33);
            }
            m_imageSeqDriverTimer->start();

            applyMasterDuration();
            pushSourceActivity();
        });
    }

    // Mirror VideoDecoder's frame progression into the timer so
    // timer.position() is always the truth-of-the-moment for time.
    // Phase 7.4 flips this: image sequences advance the timer
    // directly and the loader follows.
    connect(m_videoDecoder, &VideoDecoder::currentFrameChanged, this, [this] {
        if (!m_timeline) return;
        // Fast-seek mirror gate — the gesture drives the master
        // clock directly per tick. An in-flight decoder publish
        // from an abandoned seek would otherwise drag the timer
        // back to a stale source position, freezing the playhead.
        // The mirror resumes after stopFastSeek clears the flag
        // and the final seekToFrame commits.
        if (m_fastSeekActive) return;
        const double fps = m_videoDecoder->fps();
        if (fps <= 0.0) return;
        const double sourceTime =
            static_cast<double>(m_videoDecoder->currentFrame()) / fps;

        // Phase 3.H.2 — playlist mode: translate decoder source-time
        // → timeline-time via the active clip's offset. Detect
        // boundary cross at the clip's sourceOut and hop to the
        // next clip.
        if (m_playlistActive && !m_playlistAdvancing) {
            // Gate during the 50 ms playlist-seek debounce window
            // (see seekToTime's coalescer). The OLD decoder, still
            // on the previously-active clip, can emit a queued
            // currentFrameChanged before applyPlaylistSeek even
            // runs — translating it through this clip's offsets
            // would overwrite the QML-side optimistic timer update
            // with a stale source-clip pos, reading on screen as
            // the playhead jumping back to the old clip before
            // settling on the new target.
            if (m_pendingPlaylistSeekSec >= 0.0) return;
            // Gate during the in-flight decoder seek (between
            // applyPlaylistSeek issuing seekToFrame and the decoder
            // landing on the target). Skip intermediate keyframe-
            // rounded frames so the playhead doesn't briefly snap
            // to clip.startTime before settling on the user target.
            if (m_playlistSeekTargetFrame >= 0) {
                if (std::abs(m_videoDecoder->currentFrame()
                             - m_playlistSeekTargetFrame) > 1) {
                    return;
                }
                m_playlistSeekTargetFrame = -1;
            }
            const Clip *clip = playlistActiveClip();
            if (clip) {
                // Boundary cross — swap to next clip BEFORE mirroring,
                // so the timer doesn't briefly land past the active
                // clip's window. Skip when clip.sourceOut is 0 — that
                // means the source duration was unknown at playlist
                // build time (metadata extraction in flight); the
                // decoder's EndOfStream will trigger the cross
                // instead.
                if (clip->sourceOut > 0.0
                    && sourceTime >= clip->sourceOut - 0.5 / fps) {
                    // Mid-playlist boundary cross during playback —
                    // always resume. The decoder's isPlaying flag
                    // is unreliable here (EOS may have flipped it).
                    playlistAdvanceToClip(m_playlistCurrentClipIndex + 1,
                                          /*autoplay=*/true);
                    return;
                }
                const double timelinePos = clip->startTime
                                         + (sourceTime - clip->sourceIn);
                m_timeline->timer()->seek(timelinePos);
                return;
            }
        }
        m_timeline->timer()->seek(sourceTime);
    });

    // Mirror VideoDecoder's play/pause state into the timer so
    // QML transport bindings (eventually) show consistent state
    // and so the timer's "playing" flag is meaningful for image
    // sequences in 7.4.
    connect(m_videoDecoder, &VideoDecoder::isPlayingChanged, this, [this] {
        if (!m_timeline) return;
        // Image-seq active → wall-clock driver timer owns the play
        // state; the (now-closed) video decoder must not pause it.
        // This handles the queued isPlayingChanged that fires from
        // the decode thread AFTER playlistAdvanceToClip's image-seq
        // branch already called timer.play() — without this guard
        // playback halts at every video → image-seq boundary cross.
        if (m_imageSeqActive) return;
        if (m_videoDecoder->isPlaying()) m_timeline->timer()->play();
        else                              m_timeline->timer()->pause();
    });

    // Phase 3.H.5 — funnel the canonical "app playing?" state into
    // the thumbnail cache (drops to 1 worker during play to bound
    // I/O contention with the playback decoder) and clear any
    // visible hover overlay so we don't draw a stale frame on top
    // of moving content.
    if (m_timeline) {
        connect(m_timeline->timer(), &PlaybackTimer::playingChanged,
                this, [this] {
            const bool playing = m_timeline->timer()->isPlaying();
            if (m_thumbCache) {
                m_thumbCache->notifyPlaybackState(playing);
            }
            if (playing) clearHoverThumbnail();
        });
    }

    // Phase 7.4.b.4 pull-model: position the cache's playhead and let
    // the renderer pull. PlayerRhiItem connects to imageSeqFrameAdvanced
    // (in QML) and calls update() — the renderer reads the playhead
    // from the cache directly during synchronize/render.
    //
    // Driven off positionChanged (seconds), NOT frameAdvanced. The
    // timer's frameAdvanced fires at the timeline's frameRate, which
    // desyncs the cache whenever timeline fps != cache fps (e.g. a
    // 50 fps sequence on a timeline still set to 24). Converting
    // position × cache.fps() makes the cache's OWN fps the only fps
    // in the conversion. Deduped via m_lastImageSeqFrame so the
    // updatePlayhead + emit only fire when the source frame changes.
    connect(m_timeline->timer(), &PlaybackTimer::positionChanged, this,
            [this] {
        if (!m_imageSeqCache) return;
        const double cFps = m_imageSeqCache->fps();
        if (cFps <= 0.0) return;
        int cFrame;
        if (m_playlistActive) {
            // Playlist: map to the active clip's source-frame range
            // via clip.sourceIn + (position - clip.startTime).
            const Clip *c = playlistActiveClip();
            if (!c || c->mediaKind != ClipMediaKind::ImageSequence) {
                return;
            }
            const double pos = m_timeline->timer()->position();
            const double srcT = qMax(0.0,
                c->sourceIn + (pos - c->startTime));
            cFrame = static_cast<int>(std::lround(srcT * cFps));
        } else {
            cFrame = static_cast<int>(
                std::lround(m_timeline->timer()->position() * cFps));
        }
        cFrame = qMax(0, cFrame);
        if (cFrame == m_lastImageSeqFrame) return;
        m_lastImageSeqFrame = cFrame;
        m_imageSeqCache->updatePlayhead(cFrame);
        emit imageSeqFrameAdvanced(cFrame);
    });
    // Buffer-status pump — polls cache fill counters at the same
    // 30 Hz the playback timer ticks, independent of frame
    // advances. The cache fills async on its own thread, so window-
    // slide changes don't align with frame boundaries; tying the
    // emit to frameAdvanced left the strip frozen during seeks
    // that didn't cross a frame and stale during loading-only
    // moments.
    connect(m_timeline->timer(), &PlaybackTimer::positionChanged, this,
            [this] {
                pollImageSeqBufferStatus();
                pushSourceActivity();
                // Phase 3.H.4 — boundary cross for image-seq clips
                // in playlist mode. Video clips boundary-cross via
                // VideoDecoder::currentFrameChanged (sourceTime
                // crosses sourceOut); image-seq has no decoder,
                // so we drive boundary detection from timer
                // positionChanged. Skipped during scrub-time mid-
                // swap (m_playlistAdvancing).
                if (m_playlistActive && !m_playlistAdvancing
                    && m_timeline) {
                    const Clip *c = playlistActiveClip();
                    if (c && c->mediaKind == ClipMediaKind::ImageSequence) {
                        const double pos = m_timeline->timer()->position();
                        const double endT = c->startTime + c->duration;
                        if (pos >= endT - 0.001) {
                            playlistAdvanceToClip(
                                m_playlistCurrentClipIndex + 1,
                                /*autoplay=*/true);
                        }
                    }
                    // Phase 3.H.4 — prewarm next image-seq clip's
                    // cache once the playhead enters the lead-time
                    // window. Cheap to call; bails out early if
                    // already prewarmed or next clip isn't image-seq.
                    prewarmImageSeqIfNeeded();
                }
            });

    // Phase 3.H.2 / Phase 3.H.4 — playlist binsChanged hook does
    // two things:
    //   1. Pre-edit: full rebuild from MediaItem.playlist.items
    //      (loadPlaylist) when nothing has been edited yet — this
    //      catches metadata arriving async right after
    //      createPlaylist (initial-load durations are 0 until
    //      MetadataService completes).
    //   2. Post-edit: surgical patch — when a clip's source
    //      MediaItem just got metadata, update the clip's
    //      sourceDuration in place via patchClipDurationFromSource.
    //      Skipped when the user has trimmed (clip's range
    //      diverges from full-source).
    connect(m_project, &ProjectManager::binsChanged, this,
            [this] {
                if (!m_playlistActive || !m_timeline) return;
                if (!m_timeline->canUndo()) {
                    rebuildPlaylistTimelineOnly();
                    return;
                }
                // Surgical patch path. Walk track A; for each clip
                // whose source MediaItem now carries a real
                // duration that differs from the clip's cached
                // sourceDuration, ask the timeline to patch it.
                const Timeline &t = m_timeline->timeline();
                if (t.tracks.isEmpty()) return;
                const Track &track = t.tracks.first();
                for (const Clip &c : track.clips) {
                    if (c.mediaItemId.isEmpty()) continue;
                    const MediaItem *src = m_project->findItem(c.mediaItemId);
                    if (!src) continue;
                    double srcDur = src->duration;
                    double srcFps = src->video.frameRate;
                    if (src->type == MediaType::ImageSequence) {
                        if (src->imageSeq.duration > 0.0) {
                            srcDur = src->imageSeq.duration;
                        }
                        if (src->imageSeq.frameRate > 0.0) {
                            srcFps = src->imageSeq.frameRate;
                        }
                    }
                    if (srcDur <= 0.0) continue;
                    m_timeline->patchClipDurationFromSource(
                        track.id, c.id, srcDur, srcFps);
                }
            });

    // H.6 (2026-05-14) — re-apply loop range when the timeline
    // duration changes (clip trim/slip/delete recomputes duration).
    // Without this, the loop end stays at the pre-edit position:
    //   - Single flow: PlaybackTimer's m_duration is already
    //     updated by TimelineController::recalculateDuration, but
    //     m_inPoint/m_outPoint can become invalid relative to the
    //     new duration — pushInOutToTimer re-clamps them.
    //   - Dual flow: DualPlaybackController's listener (registered
    //     in its setTimeline) updates m_masterFrameCount + emits
    //     frameCountChanged. We connect to that separately below
    //     because timelineChanged fires before the controller's
    //     listener (auto-connect order is registration order).
    connect(m_timeline, &TimelineController::timelineChanged, this,
            [this] { pushInOutToTimer(); });

    // Phase 3.H.4 — write timeline edits back to the active
    // playlist's MediaItem. Without this, a drop / trim / move /
    // delete only updates the live timeline; navigating away and
    // re-selecting the playlist rebuilds from the (stale) MediaItem
    // entries and loses the changes. We mirror track A's clips
    // back into MediaItem.playlist.items on every timelineChanged.
    connect(m_timeline, &TimelineController::timelineChanged, this,
            [this] {
                if (!m_playlistActive || !m_project || !m_timeline) return;
                const QString plId = m_timeline->playlistMediaItemId();
                if (plId.isEmpty()) return;
                const Timeline &t = m_timeline->timeline();
                if (t.tracks.isEmpty()) return;
                const Track &track = t.tracks.first();
                QVariantList entries;
                entries.reserve(track.clips.size());
                for (const Clip &c : track.clips) {
                    if (c.isGap) continue;
                    if (c.mediaItemId.isEmpty()) continue;
                    QVariantMap em;
                    em[QStringLiteral("mediaId")] = c.mediaItemId;
                    // Store actual trim points; -1 sentinel for
                    // "use full source" so loadPlaylist resolves
                    // them naturally on reload.
                    const bool fullRange =
                        c.sourceIn <= 0.0001
                        && qFabs(c.sourceOut - c.sourceDuration) < 0.0001;
                    em[QStringLiteral("inPoint")]  = fullRange ? -1.0 : c.sourceIn;
                    em[QStringLiteral("outPoint")] = fullRange ? -1.0 : c.sourceOut;
                    entries.append(em);
                }
                m_project->replacePlaylistItems(plId, entries);
            });

    // Auto-seed the orchestrator with the first non-gap clip whenever
    // the active playlist gains content while no clip is loaded —
    // typically the first drop into an empty playlist. Without this,
    // the play button stays disabled (TransportBar.hasMedia checks
    // videoDecoder->state, which only flips when a clip is opened) and
    // the user has to scrub once to trigger seekToFrame's clip-advance
    // path. Same call startPlaylist makes; the index>=0 guard keeps it
    // a no-op on subsequent inserts and on no-op timelineChanged fires.
    connect(m_timeline, &TimelineController::timelineChanged, this,
            [this] {
                if (!m_playlistActive) return;
                if (m_playlistCurrentClipIndex >= 0) return;
                playlistAdvanceToClip(0, /*autoplay=*/false);
            });

    // Phase 7.1: project bin selection routes to the player. Click
    // a bin row → ProjectManager emits loadRequested → we open the
    // file in VideoDecoder (cascades to ScrubDecoder + AudioPlayer
    // via the existing sourcePathChanged hookups) OR spin up an
    // ImageSequenceCache for sequence items (Phase 7.4.b.4).
    connect(m_project, &ProjectManager::loadRequested, this,
            [this](const MediaItem &item) {
        qInfo("loadRequested: id='%s' name='%s' type=%d path='%s'",
              qPrintable(item.id), qPrintable(item.name),
              static_cast<int>(item.type), qPrintable(item.path));

        // Dual view is a maintained state: loading a new A while in
        // dual SWAPS A and KEEPS B (re-applying B's track edits),
        // rather than reverting to single. We preserve dual only when
        // the new A is itself dual-capable (Video / ImageSequence) and
        // B is still a valid pool item; a Playlist or Audio A, or a
        // missing B, falls back to single (the historical behavior).
        //
        // Either way we tear the dual island down here (so the new A
        // loads cleanly into the single VideoDecoder / ImageSequenceCache
        // below); when preserving we rebuild dual from scratch in
        // finishLoad() once the new A is open. Rebuild-from-scratch reuses
        // setCompositorMode's proven Single→Dual cold transition, so all
        // cache eviction (dual scrub / video / image-seq / GPU pools) and
        // the platform-specific renderer wiring are handled for us.
        const int  prevMode = m_compositorMode;
        bool         preserveDual = false;
        QVariantList clipsB;
        if (m_compositorMode != 0) {
            const bool aDualCapable =
                (item.type == MediaType::Video ||
                 item.type == MediaType::ImageSequence);
            const bool bValid = m_project &&
                m_project->findItem(m_project->bSourceMediaId()) != nullptr;
            preserveDual = aDualCapable && bValid;
            if (preserveDual && m_timeline) {
                // Capture B's track edits before teardown wipes the
                // timeline — same one-liner as saveCurrentDualView().
                clipsB = m_timeline->trackB()
                             .value(QStringLiteral("clips")).toList();
            } else {
                // Collapsing to single (non-dual-capable A, or B gone):
                // the user has left the dual session, so any bound
                // saved view detaches. The preserve branch keeps the
                // binding so an A swap stays bound.
                setActiveDualViewId({});
            }
            tearDownDualIslandToSingleState();
        }

        // Re-enter dual once the new A is loaded into single flow.
        // No-op unless we decided to preserve. Must run AFTER the A load
        // (setCompositorMode snapshots pathA from the just-opened decoder
        // / active image-seq item, and pathB from the project's still-set
        // bSource), and replaceTrackClips must run AFTER setCompositorMode
        // rebuilds the timeline — otherwise the cold transition's
        // loadSecondarySource overwrites the restored B clips. Mirrors
        // loadDualView's ordering.
        auto finishLoad = [this, prevMode, preserveDual, &clipsB] {
            if (!preserveDual) return;
            setCompositorMode(prevMode);
            if (m_timeline)
                m_timeline->replaceTrackClips(QStringLiteral("B"), clipsB);
        };

        // Always tear down whatever was loaded before. Cheap when
        // nothing's there; safe when there is. Removes the
        // video→video / seq→seq / mixed-switch race conditions
        // where the previous decoder's residual signals leak into
        // the new timeline build.
        closeActiveMedia();

        // Any new load clears a prior unsupported-media notice up front
        // (covers image-seq / playlist / decodable-video branches, all
        // of which can return before the video path). The undecodable
        // path below re-sets it.
        clearViewportNotice();

        // Bring the single-flow audio device back online if dual
        // mode shut it down on entry. Idempotent — short-circuits
        // when already initialized. Pairs with the shutdown() in
        // teardownSingleFlowForDual; without this, the
        // sourcePathChanged → m_audio->open(...) chain that fires
        // below silently fails because the device is still down.
        if (m_audio) m_audio->initialize();

        if (item.type == MediaType::ImageSequence) {
            startImageSequence(item);
            finishLoad();
            return;
        }
        if (item.type == MediaType::Playlist) {
            // Playlists are never dual-capable A sources (preserveDual is
            // false here), so this intentionally stays single.
            startPlaylist(item);
            return;
        }
        if (item.type == MediaType::LiveStream) {
            // Live is never dual-capable A in v1 (preserveDual is false
            // here — the type check above excludes it, so any dual
            // island was already torn down). No timeline, no audio
            // path; the receiver feeds m_videoDecoder's publish slot.
            startLiveStream(item);
            return;
        }
        if (!m_videoDecoder) return;
        // ARRIRAW / undecodable raw handling. Two convergent triggers,
        // neither gating the other (see the load-notice design):
        //   • proactive — if async metadata already knows there's no
        //     decoder, skip the decode attempt (no spinner→fail flicker);
        //   • fallback — otherwise attempt open(); a failure here is the
        //     race-free trigger when the viewport loads before metadata.
        // Either way we tear down the previous source and show a centered
        // viewport notice instead of a black/stale frame.
        const bool knownUnsupported =
            item.video.loaded && item.video.unsupportedCodec;
        if (knownUnsupported || !m_videoDecoder->open(item.path)) {
            qWarning("WindowManager: '%s' has no decoder "
                     "(unsupported/raw) — showing viewport notice",
                     qPrintable(item.path));
            m_videoDecoder->close();   // drop any prior source → bg shows
            // close() preserves the Errored state by design; clear it so
            // the status strip doesn't read "ERROR" for a file we're
            // handling gracefully with the viewport notice.
            m_videoDecoder->clearErrorState();
            setViewportNotice(buildViewportNoticeText(item));
            return;
        }
        // (Notice already cleared after closeActiveMedia above.)
        // Kick a seek to frame 0 so the decoder publishes the first
        // frame immediately. open() leaves the decoder paused with
        // its decode loop blocked on play/seek; without this nudge
        // the viewport stays empty until the user hits Space.
        m_videoDecoder->seekToFrame(0);
        finishLoad();
    });

    // Tie ScrubDecoder's + AudioPlayer's lifecycle to VideoDecoder's:
    // open both when a file is loaded, close when the streaming decoder
    // closes.
    //
    // Check sourcePath directly rather than state(): VideoDecoder's
    // open() emits sourcePathChanged BEFORE transitioning to Opening,
    // so reading state() here would see the previous (Idle) value
    // and we'd close ScrubDecoder instead of opening it.
    connect(m_videoDecoder, &VideoDecoder::sourcePathChanged, this, [this] {
        const QString path = m_videoDecoder->sourcePath();
        if (path.isEmpty()) {
            m_scrubDecoder->close();
            if (m_audio) m_audio->close();
        } else {
            m_scrubDecoder->open(path);
            if (m_audio) {
                // Resolve the source-of-truth MediaItem ONCE up front:
                // both the audio stream-count hint (so the dispatch
                // skips a redundant find_stream_info pass on broadcast
                // multi-stream files) AND the per-clip routing mode
                // come from it. In playlist mode the helper returns
                // the active clip's underlying source MediaItem
                // rather than the playlist item that activeItemId()
                // returns — the same id the change-signal handler
                // and the inspector pill mutate.
                const MediaItem *scopeItem = nullptr;
                if (m_project) {
                    scopeItem = m_project->findItem(
                        audioRoutingScopeMediaItemId());
                }
                const int streamCountHint =
                    (scopeItem && scopeItem->video.loaded)
                        ? scopeItem->video.audioStreamCount : -1;

                // Apply the global A/V-sync offset BEFORE open() so
                // the first seek inside the decoder pipeline already
                // honors the user's compensation. Cheap atomic store.
                m_audio->setSyncOffsetMs(audioSyncOffsetMs());
                m_audio->open(path, streamCountHint);
                // Apply the per-clip audio routing mode the user
                // (or Auto-default) saved on this MediaItem. Without
                // this, the decoder would always start in mode 0
                // (Auto) regardless of what the inspector pill or
                // .qcvproj says.
                if (scopeItem) {
                    m_audio->setRoutingMode(
                        static_cast<int>(scopeItem->audioRoutingMode));
                }
                // Re-apply the session review speed — open()
                // constructs a fresh decoder whose TempoStage
                // defaults to 1x. Matters mid-playlist: a clip swap
                // must keep audio tempo in step with the video
                // pacing speed or the sync servo fights a rate
                // mismatch it can't win.
                if (m_reviewSpeed != 1.0) {
                    m_audio->setPlaybackTempo(m_reviewSpeed);
                }
            }
            // Push the MediaItem's videoRangeOverride into the live
            // decoder at open. Without this the decoder defaults to
            // Auto on every reopen and the per-item value (set via
            // the Inspector Range pill, persisted on MediaItem and
            // round-tripped through .qcvproj) only takes effect on
            // the next pill click. Live pill changes flow via the
            // videoRangeOverrideChanged signal handler.
            if (m_videoDecoder && m_project) {
                if (const MediaItem *it = m_project->findItem(
                        audioRoutingScopeMediaItemId())) {
                    m_videoDecoder->setRangeOverride(
                        static_cast<int>(it->videoRangeOverride));
                    if (m_scrubDecoder) {
                        m_scrubDecoder->setRangeOverride(
                            static_cast<int>(it->videoRangeOverride));
                    }
                }
            }
        }
    });

    // Phase 5.0.a smoke wiring: when the video decoder reports
    // playing/paused, mirror to AudioPlayer. No master-clock sync
    // yet — the audio device runs free during playback and may
    // drift relative to video. 5.0.b adds the drift-correction loop.
    //
    // Scrub awareness is handled by seeking the audio decoder on
    // currentFrameChanged WHEN PAUSED. During playback we DON'T
    // re-seek per video frame (that would thrash the codec); the
    // audio decoder runs free off its own ring buffer.
    //
    // Phase 7.2.d: master-clock reads now go through
    // m_timeline->timer() instead of computing frame/fps inline.
    // Functionally identical (the timer is mirrored from
    // VideoDecoder.currentFrame above), but every "what time is
    // it?" goes through one canonical source — which is the path
    // image sequences need in 7.4 since they have no decoder
    // pacing them.
    auto masterPosition = [this]() -> double {
        return m_timeline ? m_timeline->timer()->position() : 0.0;
    };

    // Phase 3.H.4 — audio playback position. In single mode this is
    // the timeline (= source) time. In playlist mode the timeline
    // clock runs in PLAYLIST seconds (e.g. 30s into the second clip
    // == 30s of the timeline) but the audio decoder is opened on
    // the active clip's source file, so it expects SOURCE seconds
    // (relative to that file's start). Translate via the active
    // clip's offset: sourceT = clip.sourceIn + (timelineT - clip.startTime).
    auto audioPosition = [this, masterPosition]() -> double {
        const double mp = masterPosition();
        if (!m_playlistActive) return mp;
        const Clip *c = playlistActiveClip();
        if (!c) return mp;
        const double srcT = c->sourceIn + (mp - c->startTime);
        return std::max(0.0, srcT);
    };

    // Loop wrap for video — when decoder reports EndOfStream and
    // looping is on, seek back to frame 0 and resume. Image-seq
    // and timeline path wraps in PlaybackTimer::update() directly;
    // video has its own decode pipeline so we have to nudge it.
    connect(m_videoDecoder, &VideoDecoder::stateChanged, this, [this] {
        if (!m_videoDecoder) return;
        if (m_videoDecoder->state() != VideoDecoder::EndOfStream) return;

        // Phase 3.H.2 — playlist mode: EOS = clip done. Hop to the
        // next clip (orchestrator handles end-of-playlist loop /
        // pause). Bypasses the single-mode loop-wrap below.
        // Always resume — this only fires when playback was running
        // (decode loop reaches EOS).
        if (m_playlistActive && !m_playlistAdvancing) {
            playlistAdvanceToClip(m_playlistCurrentClipIndex + 1,
                                  /*autoplay=*/true);
            return;
        }

        if (!m_loopEnabled) return;
        // 1-frame sources (a still routed through FFmpeg, a 1-frame
        // movie): the wrap would spin decode→EOS→seek→decode at full
        // speed re-showing the same frame — pacing never engages
        // because each republish resets its baseline. The frame is
        // already displayed; there is nothing to loop.
        if (m_videoDecoder->frameCount() <= 1) return;
        m_videoDecoder->seekToFrame(0);
        // Only auto-resume if the user actually wants playback
        // running. Without this guard, a short clip (e.g. 6 frames
        // at 24 fps loops ~4×/sec) re-enters EndOfStream so often
        // that a user-pressed pause races the handler — pause
        // momentarily flips isPlaying() to false, the handler sees
        // that and toggles right back to playing.
        if (m_userWantsPlayback && !m_videoDecoder->isPlaying()) {
            m_videoDecoder->togglePlayback();
        }
    });

    // Video range-loop wrap: PlaybackTimer wraps for image-seq
    // playback, but the video decoder runs its own clock and
    // doesn't know about our in/out points. Watch
    // currentFrameChanged and snap back to the in point when the
    // playhead steps past out.
    //
    // Playlist mode: in/out points address timeline frames, not
    // a single decoder's source frames; the orchestrator's
    // boundary-cross handles end-of-clip wrap. Skip the single-
    // mode wrap here.
    connect(m_videoDecoder, &VideoDecoder::currentFrameChanged, this, [this] {
        if (m_playlistActive) return;
        if (!m_loopEnabled || !hasInOutRange()) return;
        if (m_imageSeqActive || !m_videoDecoder) return;
        const int cur = m_videoDecoder->currentFrame();
        if (cur > m_outPoint) {
            m_videoDecoder->seekToFrame(m_inPoint);
        }
    });

    connect(m_videoDecoder, &VideoDecoder::isPlayingChanged, this,
            [this, audioPosition] {
        if (!m_audio || !m_audio->hasAudio()) return;
        if (m_videoDecoder->isPlaying()) {
            // Re-align audio decode position before resuming playback —
            // scrubbing while paused may have moved the video forward
            // or backward but the audio ring buffer still has the
            // previous content. In playlist mode the audio file is
            // the active clip's source, so we feed source time, not
            // timeline time.
            m_audio->seek(audioPosition());
            m_audio->play();
        } else {
            m_audio->pause();
        }
    });

    connect(m_videoDecoder, &VideoDecoder::currentFrameChanged, this,
            [this, audioPosition] {
        if (!m_audio || !m_audio->hasAudio()) return;
        // Paused: scrub-align audio every frame change.
        // Playing: feed the master clock to AudioPlayer's drift-
        // correction loop. The loop only re-seeks when |drift| >
        // 150 ms AND the seek cooldown has elapsed, so the codec
        // stays pristine between corrections. Playlist mode
        // translates timeline → active-clip-source time before
        // feeding the audio side.
        if (m_videoDecoder->isPlaying()) {
            m_audio->update(audioPosition());
        } else {
            m_audio->seek(audioPosition());
        }
    });
}

WindowManager::~WindowManager()
{
    // Phase 7.5 B.7: shut down the native renderer's threads BEFORE
    // our member destructors run. The render thread reads
    // m_safetyOverlay / m_annotator / m_imageSeqCache pointers each
    // frame; if WindowManager's unique_ptr members destruct first
    // (the natural reverse-declaration order), those pointers go
    // dangling, the render thread tries to lock a dead mutex, and
    // std::terminate fires.
    //
    // Calling shutdown() here joins the render + upload threads
    // synchronously, then the per-member dtor chain can safely
    // tear down the underlying objects.
#ifdef QCV_NATIVE_PLAYER
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        if (auto *r = pw->renderer()) {
            r->shutdown();
        }
    }
#endif
}

bool WindowManager::initialize()
{
    if (!m_engine) {
        qWarning("WindowManager: null QML engine");
        return false;
    }

    const auto roots = m_engine->rootObjects();
    if (roots.isEmpty()) {
        qWarning("WindowManager: QML engine has no root objects yet");
        return false;
    }

    // The first root is the UI window (Main.qml's ApplicationWindow).
    m_uiWindow = qobject_cast<QQuickWindow *>(roots.first());
    if (!m_uiWindow) {
        qWarning("WindowManager: first root is not a QQuickWindow");
        return false;
    }
    qInfo("WindowManager: UI swapchain colorspace = %s",
          qPrintable(m_uiWindow->format().colorSpace().description()));

#if defined(Q_OS_WIN)
    // Subclass the UI window's WNDPROC to paint WM_ERASEBKGND with
    // #161616 (Theme.bg). WNDCLASS background brush is ignored on Qt
    // Quick windows because QWindowsWindow handles WM_ERASEBKGND via
    // the GDI default proc → DefWindowProc paints with the class
    // brush, but Qt's window is created with hbrBackground=null.
    // Subclassing and painting the fill ourselves is what reliably
    // hits this surface. Companion fills (D3D11 seed-present, QML
    // centerStage Rectangle) are also #161616 so all three blend
    // during the 1-frame seams during rail-collapse animations.
    {
        HWND hwnd = reinterpret_cast<HWND>(m_uiWindow->winId());
        if (hwnd) {
            static WNDPROC s_origProc = nullptr;
            s_origProc = reinterpret_cast<WNDPROC>(
                GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
            static const auto subclassProc =
                +[](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
                    if (msg == WM_ERASEBKGND) {
                        HDC hdc = reinterpret_cast<HDC>(wp);
                        RECT rc;
                        GetClientRect(h, &rc);
                        HBRUSH fill = CreateSolidBrush(RGB(22, 22, 22));
                        FillRect(hdc, &rc, fill);
                        DeleteObject(fill);
                        return 1;
                    }
                    return CallWindowProcW(s_origProc, h, msg, wp, lp);
                };
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(subclassProc));
        }
    }
    // Reveal the window now that the dark WM_ERASEBKGND subclass is
    // bound. Main.qml starts it hidden on Windows (visible bound to
    // non-windows); winId() above already forced HWND creation while
    // hidden, so the very first paint erases to #161616 instead of the
    // default white flash. No-op if QML somehow already showed it.
    if (m_uiWindow) m_uiWindow->setVisible(true);
#endif

#if defined(Q_OS_MACOS)
    // Green title-bar button → standard zoom, not OS-fullscreen Space.
    // Our borderless-fullscreen path (F key) handles the fullscreen
    // case via styleMask + frame manipulation; the OS Space transition
    // animation isn't something users want triggered by an accidental
    // green-button click. Borderless enter/exit swap collectionBehavior
    // around this default and restore it cleanly.
    qcv::disableSystemFullscreen(m_uiWindow);
#endif

    // Locate the centerStage Item that represents the Player window mount.
    m_centerStage = m_uiWindow->findChild<QQuickItem *>(QStringLiteral("centerStage"));
    if (!m_centerStage) {
        qWarning("WindowManager: centerStage Item not found in UI window QML tree");
        return false;
    }

    // Track centerStage geometry → Player window geometry. UI window
    // tracking is set up here too — both stay live across HDR-mode
    // recreations (the sync targets m_centerStage / m_uiWindow which
    // outlive the player window).
    connect(m_centerStage, &QQuickItem::xChanged,      this, &WindowManager::syncPlayerGeometry);
    connect(m_centerStage, &QQuickItem::yChanged,      this, &WindowManager::syncPlayerGeometry);
    connect(m_centerStage, &QQuickItem::widthChanged,  this, &WindowManager::syncPlayerGeometry);
    connect(m_centerStage, &QQuickItem::heightChanged, this, &WindowManager::syncPlayerGeometry);
    connect(m_uiWindow, &QWindow::xChanged,            this, &WindowManager::syncPlayerGeometry);
    connect(m_uiWindow, &QWindow::yChanged,            this, &WindowManager::syncPlayerGeometry);
    connect(m_uiWindow, &QWindow::widthChanged,        this, &WindowManager::syncPlayerGeometry);
    connect(m_uiWindow, &QWindow::heightChanged,       this, &WindowManager::syncPlayerGeometry);

    return createPlayerWindow();
}

bool WindowManager::createPlayerWindow()
{
#ifdef QCV_NATIVE_PLAYER
    // Phase 7.5 / F.2.1 native path:
    //   - macOS: PlayerWindow is a QWindow subclass with a Metal
    //     CAMetalLayer; the OS manages compositing.
    //   - Windows (Phase F.2.1): PlayerWindow is a QObject scaffold
    //     for the renderer; D3D11PlayerRenderer creates its OWN child
    //     HWND parented to the UI window's HWND, with a DComp target
    //     composing a flip-discard swapchain. PlayerWindow itself
    //     never gets shown/exposed.
    auto *nativePlayer = new qcv::PlayerWindow();
    m_playerWindow = nativePlayer;
    if (auto *r = nativePlayer->renderer()) {
        // Phase B.3: push the active HDR mode to the renderer before
        // first expose. The renderer caches it and applies to the
        // layer when init() resolves the layer pointer.
        r->setHdrMode(static_cast<qcv::HdrMode>(m_hdrMode));
        // Phase F.2.8 follow-up: push the persisted brightness so
        // the renderer's compositor + OCIO PSes pick it up before
        // first present.
        r->setBrightness(static_cast<float>(m_brightness));
        // Phase B.6.3: wire the VideoDecoder once. The renderer
        // pulls FrameHandles via fetchLatest each present; if there's
        // no source open it just returns false and the renderer
        // shows the rainbow sentinel.
        r->setVideoDecoder(m_videoDecoder);
        // Phase B.6.4: wire OCIO. The renderer rebuilds its compute
        // pipeline on chain generation bumps (see MetalOcioRenderer).
        r->setOcio(m_ocio);
        // Phase B.6.5: wire the annotator — same pointer to both the
        // renderer (drawMesh on each frame) and the player window
        // (mouse/tablet events forward to onPointerEvent).
        r->setViewportAnnotator(m_annotator.get());
        // Phase B.6.6: wire the safety overlay.
        r->setSafetyOverlay(m_safetyOverlay.get());
        r->setBackgroundMode(static_cast<qcv::BackgroundMode>(m_backgroundMode));
        r->setCompositorMode(static_cast<qcv::CompositorMode>(m_compositorMode));
        r->setSplitPos(static_cast<float>(m_splitPos));
        r->setDiffGain(static_cast<float>(m_diffGain));
    }
    nativePlayer->setViewportAnnotator(m_annotator.get());

    // Split-wipe seam drag (macOS path). The QML MouseArea over
    // centerStage that drives splitPos on Windows is occluded by the
    // native PlayerWindow on macOS, so PlayerWindow intercepts mouse
    // events itself in Wipe mode. Push initial mode + pos so the very
    // first hover gets the right cursor, then keep them mirrored as
    // the user changes either.
    nativePlayer->setCompositorMode(m_compositorMode);
    nativePlayer->setSplitPos(static_cast<float>(m_splitPos));
    connect(this, &WindowManager::compositorModeChanged,
            nativePlayer, [this, nativePlayer] {
        nativePlayer->setCompositorMode(m_compositorMode);
    });
    connect(this, &WindowManager::splitPosChanged,
            nativePlayer, [this, nativePlayer] {
        nativePlayer->setSplitPos(static_cast<float>(m_splitPos));
    });
    connect(nativePlayer, &qcv::PlayerWindow::splitWipeSeamDragged,
            this, [this](float normalizedX) {
        setSplitPos(static_cast<qreal>(normalizedX));
    });

    // Phase 7.5 B.7: route file drops onto the player surface to
    // the same handler the QML DropArea uses
    // (ProjectManager::addMediaFile + setActiveItem).
    connect(nativePlayer, &qcv::PlayerWindow::filesDropped,
            this, [this](const QList<QUrl> &urls) {
        if (!m_project || urls.isEmpty()) return;
        QString lastId;
        for (const QUrl &u : urls) {
            const QString path = u.toLocalFile();
            if (path.isEmpty()) continue;
            const QString id = m_project->addMediaFile(path);
            if (!id.isEmpty()) lastId = id;
        }
        if (!lastId.isEmpty()) m_project->setActiveItem(lastId);
    });
#else
    // Load the Player window from QML.
    QQmlComponent component(m_engine, QUrl(QStringLiteral("qrc:/qt/qml/Qcv/PlayerWindow.qml")));
    if (component.isError()) {
        qWarning() << "WindowManager: failed to load PlayerWindow.qml:" << component.errorString();
        return false;
    }

    QObject *playerObj = component.create();
    m_playerWindow = qobject_cast<QQuickWindow *>(playerObj);
    if (!m_playerWindow) {
        qWarning("WindowManager: PlayerWindow.qml root is not a Window");
        if (playerObj) playerObj->deleteLater();
        return false;
    }
#endif

    // Parent the Player window's QObject lifetime to this manager so it
    // gets cleaned up correctly. QWindow shadows QObject::setParent, so
    // call it explicitly via the QObject scope.
    m_playerWindow->QObject::setParent(this);

    // Set the swapchain colorspace + HDR format BEFORE the window is
    // shown — Qt's QSGRhiSupport reads `_qt_sg_hdr_format` and the
    // QSurfaceFormat colorspace at swapchain creation time, which
    // happens on first expose. After first show, those reads don't
    // re-run, so changing HDR mode requires this whole window
    // teardown + recreate path (see setHdrMode).
    QSurfaceFormat fmt = m_playerWindow->format();
    const char *hdrTag = "";
    switch (m_hdrMode) {
    case ExtendedLinearSRgb:      hdrTag = "scrgb"; break;
    case ExtendedLinearDisplayP3: hdrTag = "p3";    break;
    case Hdr10:                   hdrTag = "hdr10"; break;
    case SdrSRgb:
    default:                      hdrTag = "";      break;
    }
    if (m_hdrMode == SdrDisplayP3) hdrTag = "";
    m_playerWindow->setProperty("_qt_sg_hdr_format", QByteArray(hdrTag));

    switch (m_hdrMode) {
    case SdrDisplayP3:
        fmt.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
        break;
    case SdrSRgb:
    default:
        fmt.setColorSpace(QColorSpace(QColorSpace::SRgb));
        break;
    }
    m_playerWindow->setFormat(fmt);

#if defined(Q_OS_WIN) && defined(QCV_NATIVE_PLAYER)
    // Phase F.2.1: PlayerWindow itself is never shown on Windows — the
    // D3D11PlayerRenderer creates its own child HWND parented to the
    // UI window's HWND and composes via DComp. Push the UI HWND into
    // the renderer, then explicitly call init() (no exposeEvent will
    // fire because the PlayerWindow is never shown).
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        if (auto *rPtr = pw->renderer()) {
            if (auto *d3dRenderer = dynamic_cast<qcv::D3D11PlayerRenderer *>(rPtr)) {
                d3dRenderer->setParentHwnd(
                    reinterpret_cast<void *>(m_uiWindow->winId()));
                if (!d3dRenderer->init(pw)) {
                    qCritical("WindowManager: D3D11PlayerRenderer init failed");
                }
            }
        }
    }
    syncPlayerGeometry();
    qInfo("WindowManager: player surface ready (Windows D3D11 + DComp child HWND) "
          "— mode=%d, HDR-tag='%s'",
          m_hdrMode, hdrTag);
    return true;
#else
    // Make it a child window of the UI window (Guide 03 D2). macOS path.
    m_playerWindow->setParent(m_uiWindow);

    syncPlayerGeometry();
    m_playerWindow->show();

    qInfo("WindowManager: player window created — mode=%d, HDR-tag='%s', "
          "SDR-colorspace=%s",
          m_hdrMode, hdrTag,
          qPrintable(m_playerWindow->format().colorSpace().description()));
    return true;
#endif
}

void WindowManager::destroyPlayerWindow()
{
    if (!m_playerWindow) return;
    m_playerWindow->hide();
    m_playerWindow->setParent(nullptr);
    // Defer actual delete to the next event-loop tick so any in-
    // flight scene-graph render finishes cleanly. deleteLater is
    // QObject-scoped here; QWindow inherits from QObject.
    m_playerWindow->deleteLater();
    m_playerWindow = nullptr;
}

void WindowManager::syncPlayerGeometry()
{
    if (m_detached || !m_playerWindow || !m_centerStage || !m_uiWindow) {
        return;
    }

    // CRITICAL: When the Player window is a child of the UI window
    // (via QWindow::setParent), its setGeometry / setPosition coords
    // are RELATIVE TO THE PARENT WINDOW's content area — not absolute
    // screen coords. Qt docs are subtle on this; the symptom is the
    // child appearing at a wildly wrong position (or stuck at parent's
    // top-left if the OS clamps).
    //
    // Scene coords are exactly what we want here: (0, 0) in QML scene
    // equals top-left of the parent's content area. mapToScene(0, 0)
    // on the centerStage Item yields its position within that scene,
    // which is also its position relative to the parent window's
    // content. Use those directly.
    //
    // (When detached, the Player window is top-level and setGeometry
    // takes absolute screen coords. The detach() path handles that
    // separately; this function is only called while docked.)

    const QPointF scenePos = m_centerStage->mapToScene(QPointF(0, 0));
    const qreal dpr = m_uiWindow->effectiveDevicePixelRatio();
    // Physical pixel coords for Win32 SetWindowPos; logical coords for
    // Qt's QWindow::setGeometry. The branch below picks per-platform.
    const int x  = static_cast<int>(scenePos.x() * dpr + 0.5);
    const int y  = static_cast<int>(scenePos.y() * dpr + 0.5);
    const int w  = static_cast<int>(m_centerStage->width()  * dpr + 0.5);
    const int h  = static_cast<int>(m_centerStage->height() * dpr + 0.5);

    if (w <= 0 || h <= 0) return;

#if defined(Q_OS_WIN) && defined(QCV_NATIVE_PLAYER)
    // Phase F.2.1: the renderer owns its own child HWND. Route geometry
    // changes through the renderer (which Win32-SetWindowPos's the child
    // + queues a swapchain ResizeBuffers). Coords are physical pixels
    // relative to the UI window's client area, which is what Win32
    // child-HWND positioning expects.
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        if (auto *r = pw->renderer()) {
            if (auto *d3d = dynamic_cast<qcv::D3D11PlayerRenderer *>(r)) {
                d3d->setViewportRect(x, y, w, h);
                return;
            }
        }
    }
    return;
#else
    // macOS: PlayerWindow is a Qt child QWindow; setGeometry uses
    // logical coords. Recompute without the DPR scaling.
    const int lx = static_cast<int>(scenePos.x());
    const int ly = static_cast<int>(scenePos.y());
    const int lw = static_cast<int>(m_centerStage->width());
    const int lh = static_cast<int>(m_centerStage->height());
    m_playerWindow->setGeometry(QRect(lx, ly, lw, lh));
#endif
}

// ---------------------------------------------------------------------
// UI-over-viewport framework — shared "viewport cover" primitive.
//
// The native viewport surface composites ABOVE the Qt scene, so the
// only way for in-scene QML (a modal panel, the unsupported-media
// notice) to appear OVER the video is to hide that surface and let the
// QML layer underneath become the top visible plane. Both consumers
// funnel through recomputeViewportCover(): cover when a modal is open
// OR a notice is showing. This is the one place that touches per-OS
// window visibility.
// ---------------------------------------------------------------------
void WindowManager::recomputeViewportCover()
{
    const bool cover = m_modalActive || !m_viewportNoticeText.isEmpty();
    if (cover == m_viewportCovered) return;
    m_viewportCovered = cover;
    setViewportCovered(cover);
    emit viewportCoveredChanged();
}

void WindowManager::setViewportCovered(bool covered)
{
#if defined(Q_OS_WIN) && defined(QCV_NATIVE_PLAYER)
    // Windows: the renderer owns a child HWND; show/hide it directly.
    // On uncover, syncPlayerGeometry() re-runs setViewportRect (which
    // also re-shows + repositions) to land it back exactly.
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        if (auto *r = pw->renderer()) {
            if (auto *d3d = dynamic_cast<qcv::D3D11PlayerRenderer *>(r)) {
                d3d->setViewportVisible(!covered);
            }
        }
    }
    if (!covered) syncPlayerGeometry();
#elif defined(QCV_NATIVE_PLAYER)
    // macOS: the player is a native child QWindow; hide()/show() it.
    // Re-sync geometry on show so a rail-collapse / resize that landed
    // while hidden is honored.
    if (!m_playerWindow) return;
    if (covered) {
        m_playerWindow->hide();
    } else {
        m_playerWindow->show();
        syncPlayerGeometry();
    }
#else
    Q_UNUSED(covered);
#endif
}

void WindowManager::setViewportInputGated(bool gated)
{
#ifdef QCV_NATIVE_PLAYER
    // macOS owns viewport pointer input in the native PlayerWindow.
    // Windows routes it through QML MouseAreas, which gate themselves on
    // the modalActive property — nothing to do here.
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        pw->setInputGated(gated);
    }
#else
    Q_UNUSED(gated);
#endif
}

void WindowManager::captureBackdrop()
{
#ifdef QCV_NATIVE_PLAYER
    if (!m_backdropProvider) return;
    auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data());
    if (!pw || !pw->renderer()) return;
    // Reuses the screenshot path: a 3-pass offscreen render through OCIO
    // to an SDR sRGB RGBA8 QImage, independent of the live HDR swapchain
    // mode. Blocks ~one frame (bounded by the renderer's 250 ms ceiling).
    // TODO(modal-backdrop): optional clean (no-strokes) capture variant —
    // for now the backdrop inherits whatever the screenshot bakes in.
    QImage img = pw->renderer()->captureScreenshot();
    if (img.isNull()) return;
    m_backdropProvider->setImage(img);
    // Monotonic suffix busts QML's image cache so a new frame loads.
    m_backdropSource =
        QStringLiteral("image://qcv/backdrop/%1").arg(++m_backdropSeq);
    emit backdropSourceChanged();
#endif
}

void WindowManager::beginModal()
{
    if (m_modalActive) return;
    // If playing, pause on the frame we're about to freeze — stops audio
    // and makes the dimmed backdrop match a true still; endModal() resumes.
    // Modal input is gated + the scrim covers the transport, so the play
    // state can't change underneath us while the modal is open.
    m_modalWasPlaying = isPlayingUnified();
    if (m_modalWasPlaying) pause();
    // CAPTURE-FIRST: grab the (now paused) frame while the surface is
    // still live so we have a valid backdrop, THEN cover + gate. Avoids
    // depending on render-loop behavior while hidden and masks the
    // capture's blocking wait (panel + backdrop appear together).
    captureBackdrop();
    m_modalActive = true;
    emit modalActiveChanged();
    recomputeViewportCover();
    setViewportInputGated(true);
}

void WindowManager::endModal()
{
    if (!m_modalActive) return;
    m_modalActive = false;
    emit modalActiveChanged();
    setViewportInputGated(false);
    // Drop the frozen backdrop so it isn't served stale next time. The
    // notice (if any) keeps the cover via recompute below.
    if (!m_backdropSource.isEmpty()) {
        m_backdropSource.clear();
        emit backdropSourceChanged();
    }
    recomputeViewportCover();
    // Resume playback if we paused it on open — after the viewport is
    // restored (recompute above) so play resumes onto the live surface.
    if (m_modalWasPlaying) {
        m_modalWasPlaying = false;
        play();
    }
}

void WindowManager::detach()
{
    if (m_detached || !m_playerWindow || !m_uiWindow) {
        return;
    }

    // Remember the docked geometry for visual continuity if needed.
    m_lastDetachedGeometry = m_playerWindow->geometry();

    // Reparent to top-level. Window flags become decorated; window appears
    // independently in the OS window list.
    m_playerWindow->setParent(nullptr);
    m_playerWindow->setFlags(Qt::Window);

    // Place near the UI window's screen, offset so it's visibly distinct.
    QScreen *screen = m_uiWindow->screen();
    if (screen) {
        const QRect available = screen->availableGeometry();
        const int w = qMax(960, available.width() / 2);
        const int h = qMax(540, available.height() / 2);
        const int x = available.x() + (available.width()  - w) / 2;
        const int y = available.y() + (available.height() - h) / 2;
        m_playerWindow->setGeometry(QRect(x, y, w, h));
    }

    m_detached = true;
    emit detachedChanged();
}

void WindowManager::reattach()
{
    if (!m_detached || !m_playerWindow || !m_uiWindow) {
        return;
    }

    // Reparent back to UI window — becomes a child window again.
    m_playerWindow->setFlags(Qt::FramelessWindowHint);
    m_playerWindow->setParent(m_uiWindow);

    m_detached = false;
    syncPlayerGeometry();
    emit detachedChanged();
}

// ---- Phase 7.7 Stage 4 — cold-transition helpers --------------------

namespace {

// Return the renderer for the current native player window, or
// nullptr if no native player is active.
qcv::IPlayerRenderer *fetchActiveRenderer(QWindow *playerWindow)
{
#ifdef QCV_NATIVE_PLAYER
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(playerWindow)) {
        return pw->renderer();
    }
#else
    Q_UNUSED(playerWindow);
#endif
    return nullptr;
}

// Resolve a MediaItem's applied pixel aspect to a positive rational.
// Square → 1:1; Detected → the probed SAR (1:1 if unprobed / square);
// Custom → the stored custom rational. Used to drive the renderer's
// per-side un-squeeze.
std::pair<int, int> effectivePixelAspectFor(const qcv::MediaItem &it)
{
    switch (it.pixelAspectMode) {
    case qcv::PixelAspectMode::Detected:
        if (it.video.sarNum > 0 && it.video.sarDen > 0)
            return { it.video.sarNum, it.video.sarDen };
        return { 1, 1 };
    case qcv::PixelAspectMode::Custom:
        if (it.customParNum > 0 && it.customParDen > 0)
            return { it.customParNum, it.customParDen };
        return { 1, 1 };
    case qcv::PixelAspectMode::Square:
    default:
        return { 1, 1 };
    }
}

// Resolve a MediaItem's applied display rotation to clockwise
// degrees {0, 90, 180, 270}. Override −1 = Auto → the detected
// display-matrix rotation (video.rotationDeg; <0 = unknown/pre-
// feature cache → 0 until the migration probe lands). Used to drive
// the renderer's per-side quarter-turn.
int effectiveRotationFor(const qcv::MediaItem &it)
{
    const int deg = (it.rotationOverride >= 0) ? it.rotationOverride
                                               : it.video.rotationDeg;
    return (deg == 90 || deg == 180 || deg == 270) ? deg : 0;
}

// Cast the controller's side to DualImageSeqSource, or nullptr if
// the side isn't an image sequence (or the controller is null).
// Used by both the dualImageSeq* Q_PROPERTY getters and the
// pollImageSeqBufferStatus delta detector.
qcv::dual::DualImageSeqSource *dualImageSeqSide(
    qcv::dual::DualPlaybackController *ctl, char side)
{
    if (!ctl) return nullptr;
    qcv::dual::IDualSource *src =
        (side == 'A') ? ctl->sourceA() : ctl->sourceB();
    return dynamic_cast<qcv::dual::DualImageSeqSource *>(src);
}

} // namespace

void WindowManager::teardownSingleFlowForDual()
{
    if (m_videoDecoder && m_videoDecoder->state() != qcv::VideoDecoder::Idle) {
        m_videoDecoder->close();
    }
    if (m_videoDecoderB && m_videoDecoderB->state() != qcv::VideoDecoder::Idle) {
        m_videoDecoderB->close();
    }
    if (m_imageSeqActive) {
        stopImageSequence();
    }
    if (m_audio) {
        // Fully release the single-flow CoreAudio device, not just
        // stop() the AU. DualAudioMixer creates its own device on
        // dual entry and tries to grab the system output; if the
        // single-flow device is still alive (even paused), the
        // first dual-init can fail silently and audio only works
        // on a subsequent dual entry. We re-initialize() the
        // single-flow device in rebuildSingleFlowFromActiveItem.
        m_audio->shutdown();
    }
    if (auto *r = fetchActiveRenderer(m_playerWindow.data())) {
        r->clearSourceAState();
        r->clearSourceBState();
        r->setVideoDecoderB(nullptr);
        r->setImageSeqCache(nullptr);
    }
}

void WindowManager::rebuildSingleFlowFromActiveItem()
{
    if (!m_project) return;
    const QString activeId = m_project->activeItemId();
    if (activeId.isEmpty()) return;
    const MediaItem *item = m_project->findItem(activeId);
    if (!item) return;

    closeActiveMedia();   // safety — drop any partial state

    // Bring the single-flow audio device back online — it's the
    // counterpart to teardownSingleFlowForDual's shutdown above.
    // Idempotent: initialize() short-circuits if already up.
    if (m_audio) {
        m_audio->initialize();
    }

    if (item->type == MediaType::ImageSequence) {
        startImageSequence(*item);
        return;
    }
    if (m_videoDecoder) {
        if (m_videoDecoder->open(item->path)) {
            // Same first-frame nudge as the loadRequested handler —
            // see comment there.
            m_videoDecoder->seekToFrame(0);
        }
    }
}

void WindowManager::tearDownDualIslandToSingleState()
{
    if (m_compositorMode == 0 && !m_dualController) return;

    // Stop the dual-mode image-seq poll timer BEFORE we tear down
    // m_dualController — the timer's slot reads through m_dualController
    // and would dereference nullptr otherwise.
    if (m_dualBufferPollTimer) m_dualBufferPollTimer->stop();
    // Reset cached dual-side stats so a future re-entry into dual starts
    // from a clean delta-detection baseline.
    m_lastDualAheadA = m_lastDualBehindA = m_lastDualFrameCountA = -1;
    m_lastDualActiveA = false; m_lastDualFpsA = 0.0;
    m_lastDualAheadB = m_lastDualBehindB = m_lastDualFrameCountB = -1;
    m_lastDualActiveB = false; m_lastDualFpsB = 0.0;
    emit dualImageSeqStatusChanged();   // QML strips clear

    if (auto *r = fetchActiveRenderer(m_playerWindow.data())) {
        r->setRendererMode(qcv::RendererMode::SingleFlow);
        r->setDualController(nullptr);
        r->setCompositorMode(qcv::CompositorMode::Single);
#if !defined(Q_OS_MACOS) && !defined(__APPLE__)
        // Detach the renderer's IDualFrameSource pointer BEFORE we drop
        // the adapter that backs it — the render thread may still be
        // mid-frame.
        r->setDualFrameSource(nullptr);
#endif
    }
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(
            m_playerWindow.data())) {
        pw->setDualActive(false);
    }
    if (m_dualController) {
        m_dualController->close();
        m_dualController.reset();
        emit dualControllerChanged();
    }
#if !defined(Q_OS_MACOS) && !defined(__APPLE__)
    m_dualSourceAdapter.reset();
#endif

    if (m_compositorMode != 0) {
        m_compositorMode = 0;
        emit compositorModeChanged();
    }
}

void WindowManager::setCompositorMode(int mode)
{
    if (m_compositorMode == mode) return;
    // Live A is not dual-capable in v1 — see the setBSource guard.
    if (mode != 0 && m_liveActive) {
        qWarning("setCompositorMode: dual modes unavailable while a "
                 "live stream is active (v1)");
        return;
    }
    const bool wasSingle = (m_compositorMode == 0);
    const bool nowSingle = (mode == 0);
    m_compositorMode = mode;

    // Leaving dual entirely detaches any bound saved view. This path
    // is only hit on a genuine user dual-off (the QML compositorMode
    // setter passes 0); the internal source-swap teardown goes
    // through tearDownDualIslandToSingleState() directly and keeps
    // the binding so an A/B swap stays bound to the saved view.
    if (nowSingle) setActiveDualViewId({});

    // Always push the new mode value to the renderer first. The
    // single-flow compositor reads it directly; the dual-flow
    // compositor's setMode mirror is wired inside the renderer's
    // setCompositorMode override.
    if (auto *r = fetchActiveRenderer(m_playerWindow.data())) {
        r->setCompositorMode(static_cast<qcv::CompositorMode>(mode));
    }

    if (wasSingle && !nowSingle) {
        // Single → Dual cold transition. Snapshot paths, tear down
        // single-flow state, spin up the dual island.
        QString pathA;
        if (m_videoDecoder && !m_videoDecoder->sourcePath().isEmpty()) {
            pathA = m_videoDecoder->sourcePath();
        } else if (m_imageSeqActive && m_project) {
            const MediaItem *item =
                m_project->findItem(m_project->activeItemId());
            if (item) pathA = item->path;
        }
        // B path: prefer Project::bSource (Stage 5 canonical store).
        // Fall back to the legacy m_videoDecoderB only when bSource
        // is empty — this lights up dual mode for users who dropped
        // B before Stage 5 went in. Once openSourceB is fully ripped
        // out (Stage 6+) the fallback goes away.
        QString pathB;
        if (m_project && !m_project->bSourcePath().isEmpty()) {
            pathB = m_project->bSourcePath();
        } else if (m_videoDecoderB && !m_videoDecoderB->sourcePath().isEmpty()) {
            pathB = m_videoDecoderB->sourcePath();
        }

        // EXR layer plumbing — pull each side's active layer from
        // the project pool's MediaItem so the dual EXR loader picks
        // the same channel-prefix the user selected in the Inspector
        // for the single-flow view. Empty / non-EXR sides pass an
        // empty string (the default unprefixed RGBA path).
        QString exrLayerA, exrLayerB;
        if (m_project) {
            if (const MediaItem *itemA =
                    m_project->findItem(m_project->activeItemId())) {
                if (itemA->type == MediaType::ImageSequence) {
                    exrLayerA = itemA->imageSeq.layer;
                }
            }
            if (const MediaItem *itemB =
                    m_project->findItem(m_project->bSourceMediaId())) {
                if (itemB->type == MediaType::ImageSequence) {
                    exrLayerB = itemB->imageSeq.layer;
                }
            }
        }

        // Suppress the metadataChanged-driven loadSingleMedia rebuild
        // that fires when m_videoDecoder->close() resets metadata to
        // zero. Without this, the handler wipes the timeline (including
        // the B track that setBSource populated). Reset after the
        // dual-side timeline is rebuilt below.
        m_suppressTimelineRebuild = true;

        teardownSingleFlowForDual();

        m_dualController =
            std::make_unique<qcv::dual::DualPlaybackController>(this);
        if (m_dualController->open(pathA,
                                    qcv::dual::DualSourceKind::AutoDetect,
                                    pathB,
                                    qcv::dual::DualSourceKind::AutoDetect,
                                    exrLayerA,
                                    exrLayerB)) {
            // Apply per-side audio routing modes from each MediaItem
            // before playback starts so the user's saved preference
            // (or Auto-default) is in effect on the first sample.
            // A/V sync offset for dual is independent from single —
            // dual's compositing pipeline is deeper, so its default
            // is tuned higher (Windows ~35 ms vs single's 8 ms).
            if (m_dualController->audio()) {
                m_dualController->audio()->setSyncOffsetMs(
                    dualAudioSyncOffsetMs());
            }
            // Master volume / mute — the transport bar's controls
            // keep binding to the (persisting) single-flow
            // AudioPlayer; mirror its state into the dual mixer so
            // M and the volume slider work in dual mode too. The
            // mixer is the connection context, so these disconnect
            // automatically when the dual island tears down.
            if (m_dualController->audio() && m_audio) {
                auto *mix = m_dualController->audio();
                mix->setMasterVolume(m_audio->volume());
                mix->setMasterMuted(m_audio->muted());
                connect(m_audio, &qcv::AudioPlayer::volumeChanged, mix,
                        [this, mix] {
                    mix->setMasterVolume(m_audio->volume());
                });
                connect(m_audio, &qcv::AudioPlayer::mutedChanged, mix,
                        [this, mix] {
                    mix->setMasterMuted(m_audio->muted());
                });
            }
            if (m_project && m_dualController->audio()) {
                if (const MediaItem *itA =
                        m_project->findItem(m_project->activeItemId())) {
                    m_dualController->audio()->setRoutingModeA(
                        static_cast<int>(itA->audioRoutingMode));
                    // Per-side videoRangeOverride flows the same way:
                    // pull from the source MediaItem now so the YUV→RGB
                    // compute honors the user's saved Range pick from
                    // the first frame. Live changes flow via the
                    // videoRangeOverrideChanged signal handler above.
                    m_dualController->setRangeOverrideA(
                        static_cast<int>(itA->videoRangeOverride));
                }
                if (const MediaItem *itB =
                        m_project->findItem(m_project->bSourceMediaId())) {
                    m_dualController->audio()->setRoutingModeB(
                        static_cast<int>(itB->audioRoutingMode));
                    m_dualController->setRangeOverrideB(
                        static_cast<int>(itB->videoRangeOverride));
                }
            }
#if !defined(Q_OS_MACOS) && !defined(__APPLE__)
            // Phase F.2.11.d — Windows D3D11 dual flow consumes the
            // typed IDualFrameSource seam, not the void* controller
            // pointer. Adapter is owned by WindowManager; renderer
            // holds a non-owning pointer for the dual session's life.
            m_dualSourceAdapter =
                std::make_unique<qcv::WindowManagerDualSourceAdapter>(
                    m_dualController.get());
#endif
            if (auto *r = fetchActiveRenderer(m_playerWindow.data())) {
                r->setDualController(static_cast<void *>(m_dualController.get()));
#if !defined(Q_OS_MACOS) && !defined(__APPLE__)
                r->setDualFrameSource(m_dualSourceAdapter.get());
#endif
                r->setRendererMode(qcv::RendererMode::DualFlow);
                r->setCompositorMode(static_cast<qcv::CompositorMode>(mode));
                r->requestUpdate();
            }
            if (auto *pw = qobject_cast<qcv::PlayerWindow *>(
                    m_playerWindow.data())) {
                pw->setDualActive(true);
            }
            // Rebuild the timeline with both A and B tracks from the
            // dual sources' metadata. This replaces the (wiped)
            // single-flow timeline state with a proper dual model.
            if (m_timeline && m_project) {
                if (auto *srcA = m_dualController->sourceA()) {
                    const MediaItem *itemA =
                        m_project->findItem(m_project->activeItemId());
                    if (itemA) {
                        const double fpsA = srcA->fps();
                        const int    fcA  = srcA->frameCount();
                        const double durA = (fpsA > 0.0 && fcA > 0)
                            ? static_cast<double>(fcA) / fpsA : 0.0;
                        m_timeline->loadSingleMedia(*itemA, durA, fpsA,
                                                      /*hasAudio=*/false);
                    }
                }
                if (auto *srcB = m_dualController->sourceB()) {
                    const MediaItem *itemB =
                        m_project->findItem(m_project->bSourceMediaId());
                    const QString nameB = itemB ? itemB->name
                                                 : QFileInfo(pathB).fileName();
                    const double fpsB = srcB->fps();
                    const int    fcB  = srcB->frameCount();
                    const double durB = (fpsB > 0.0 && fcB > 0)
                        ? static_cast<double>(fcB) / fpsB : 0.0;
                    m_timeline->loadSecondarySource(pathB, nameB, durB, fpsB,
                                                     /*hasAudio=*/false);
                }
            }

            // Re-enable the metadataChanged handler — single-flow
            // VideoDecoder metadata can land later (background scan)
            // and we don't want it to clobber our explicit dual
            // timeline. m_videoDecoder is closed in dual mode so the
            // handler's `activeItemId` check would still re-build —
            // protect with a dual-mode guard inside the handler too.
            m_suppressTimelineRebuild = false;

            // Phase 7.8 Stage A — wire timeline so the dual controller
            // can translate master frame → per-side source frame on
            // every pump tick. Done AFTER the rebuild so the first
            // tick sees populated tracks. With no edits applied the
            // translation is identity (clip startTime=0, sourceIn=0)
            // — Phase 7.7 behavior is preserved.
            m_dualController->setTimeline(m_timeline);

            // Mirror the dual controller's playhead into the timeline
            // timer so QML's scrubber + timeline panel track dual
            // playback position. Also wake the renderer — on Windows
            // the D3D11 render thread is render-on-demand, so without
            // an explicit requestUpdate per pump tick the renderer
            // sleeps and dual playback appears frozen (timeline
            // advances, viewport doesn't). macOS Metal redraws every
            // vsync regardless, so the requestUpdate is a cheap
            // no-op there. Connection auto-disconnects when
            // m_dualController is destroyed on Dual→Single transition.
            // H.6 — re-apply loop range when the dual controller
            // recomputes masterFrameCount in response to a timeline
            // edit. The controller's own timelineChanged listener
            // updates m_masterFrameCount and emits this signal;
            // here we re-push loop bounds so the wrap point follows.
            connect(m_dualController.get(),
                    &qcv::dual::DualPlaybackController::frameCountChanged,
                    this, [this] { pushInOutToTimer(); });

            connect(m_dualController.get(),
                    &qcv::dual::DualPlaybackController::currentFrameChanged,
                    this, [this] {
                if (!m_dualController) return;
                if (m_timeline) {
                    const double fps = m_dualController->fps();
                    if (fps > 0.0) {
                        const double posSec =
                            static_cast<double>(m_dualController->currentFrame())
                            / fps;
                        m_timeline->timer()->seek(posSec);
                    }
                }
                if (auto *r = fetchActiveRenderer(m_playerWindow.data())) {
                    r->requestUpdate();
                }
            });

            // Phase 7.7 — DON'T auto-play on entry. The pump thread
            // advances decode_target on every tick (even while
            // paused), so both decoders fill 8 frames ahead of the
            // current playhead immediately. User clicks play when
            // ready; readahead has had a moment to warm up so first
            // playback frame is already buffered.
            emit dualControllerChanged();

            // Push the persistent loop-enabled + in/out state into
            // the dual timer now that it exists (pushInOutToTimer
            // is a no-op on the dual branch when m_dualController
            // is null, so it skipped during the single-flow phase).
            pushInOutToTimer();

            // H.4 — wake the renderer when a dual source finishes
            // producing a frame (image-seq worker load OR video
            // decode-thread frame). Without this, the render-on-demand
            // loop only redraws on currentFrameChanged (which fires
            // when the master frame *number* changes); the first frame
            // after open, the first frame after each loop wrap, and —
            // critically — a timeline seek while paused stay blank
            // because the renderer pulls before the source catches up
            // and nothing wakes it once the source does. Both
            // DualImageSeqSource and DualVideoDecoder implement the
            // IDualSource::setFrameAvailableCallback hook, so no
            // per-type dispatch is needed. The renderer outlives the
            // controller's sources, so a raw pointer is safe to
            // capture for the source's life.
            if (auto *r = fetchActiveRenderer(m_playerWindow.data())) {
                auto wakeRenderer = [r] { r->requestUpdate(); };
                if (auto *sa = m_dualController->sourceA())
                    sa->setFrameAvailableCallback(wakeRenderer);
                if (auto *sb = m_dualController->sourceB())
                    sb->setFrameAvailableCallback(wakeRenderer);
            }

            // Honor each image-seq side's stored per-item cache stride
            // (dual previously ignored it — single flow applies the
            // same value in startImageSequence).
            applyDualImageSeqStride();

            // Dual-mode image-seq buffer-status poll. DualImageSeqSource
            // doesn't emit Qt signals on cache state change (no QObject
            // inheritance), so we poll at ~30 Hz like the single-flow
            // image-seq path. Started here, stopped in the Dual→Single
            // branch below. Lazy-constructed.
            if (!m_dualBufferPollTimer) {
                m_dualBufferPollTimer = new QTimer(this);
                m_dualBufferPollTimer->setInterval(33);
                m_dualBufferPollTimer->setTimerType(Qt::CoarseTimer);
                connect(m_dualBufferPollTimer, &QTimer::timeout,
                        this, &WindowManager::pollImageSeqBufferStatus);
            }
            m_dualBufferPollTimer->start();

            qInfo("WindowManager: Single→Dual transition complete (mode=%d, "
                  "A='%s', B='%s') — paused for readahead warm-up",
                  mode, qPrintable(pathA), qPrintable(pathB));
        } else {
            qWarning("WindowManager: Single→Dual failed; reverting to single");
            // Clear the suppress flag the dual-entry set at the top — the
            // success branch resets it (line ~1839), but this failure path
            // returns early and would otherwise leave it stuck TRUE,
            // wedging the metadataChanged timeline rebuild for EVERY
            // subsequent load (timeline never returns). Must clear before
            // rebuildSingleFlowFromActiveItem so its rebuild takes effect.
            m_suppressTimelineRebuild = false;
            m_dualController.reset();
            m_compositorMode = 0;
            rebuildSingleFlowFromActiveItem();
            emit compositorModeChanged();
            return;
        }
    } else if (!wasSingle && nowSingle) {
        // Dual → Single cold transition. tearDownDualIslandToSingleState
        // does the platform-complete teardown (poll timer, stat
        // baselines, renderer detach + Windows adapter ordering,
        // controller close/reset); m_compositorMode was already set to 0
        // above, so its trailing compositorModeChanged emit is suppressed
        // (this function emits it at the end). We then rebuild single
        // flow from the active item.
        tearDownDualIslandToSingleState();
        rebuildSingleFlowFromActiveItem();
        if (auto *r = fetchActiveRenderer(m_playerWindow.data())) {
            r->requestUpdate();
        }
        qInfo("WindowManager: Dual→Single transition complete");
    }
    // else: dual ↔ dual (SBS ↔ Wipe ↔ Difference) — already handled by
    // the renderer.setCompositorMode push above.

    emit compositorModeChanged();
}

// In/Out point setters. Pushes a (inSec, outSec) pair to the timer
// so wrap-on-loop honors the range; clears the timer's range when
// either endpoint is unset. Emits inOutPointsChanged for QML
// markers to reposition. Old app: panel_timeline.cpp ~line 1100.
void WindowManager::setInPointAtCurrent()
{
    const int cur = currentFrameUnified();
    const int total = frameCountUnified();
    if (total <= 0) return;
    int newIn = std::max(0, std::min(total - 1, cur));
    // If new in lands at or past current out, push out to end so
    // the range stays valid (matches old app's "I clears O when
    // they'd cross" behavior).
    if (m_outPoint >= 0 && newIn >= m_outPoint) {
        m_outPoint = -1;
    }
    if (newIn == m_inPoint) return;
    m_inPoint = newIn;
    pushInOutToTimer();
    // Auto-enable loop when a range is now established. The user
    // can still toggle loop off independently afterward; we just
    // flip it on once at the moment the range becomes valid.
    if (hasInOutRange() && !m_loopEnabled) {
        setLoopEnabled(true);
    }
    emit inOutPointsChanged();
}

void WindowManager::setOutPointAtCurrent()
{
    const int cur = currentFrameUnified();
    const int total = frameCountUnified();
    if (total <= 0) return;
    int newOut = std::max(0, std::min(total - 1, cur));
    if (m_inPoint >= 0 && newOut <= m_inPoint) {
        m_inPoint = -1;
    }
    if (newOut == m_outPoint) return;
    m_outPoint = newOut;
    pushInOutToTimer();
    if (hasInOutRange() && !m_loopEnabled) {
        setLoopEnabled(true);
    }
    emit inOutPointsChanged();
}

void WindowManager::clearInOutPoints()
{
    if (m_inPoint < 0 && m_outPoint < 0) return;
    m_inPoint = -1;
    m_outPoint = -1;
    pushInOutToTimer();
    emit inOutPointsChanged();
}

void WindowManager::forwardViewportPointer(int phase, qreal x, qreal y,
                                              qint64 timestamp)
{
    if (!m_annotator) return;
    // QML hands us logical points relative to the centerStage QML
    // Item. The renderer's setViewportRect (in drawFrame) is in
    // swapchain pixels, so we scale by the UI window's DPR to land
    // in the same pixel space.
    qreal dpr = 1.0;
    if (m_uiWindow) dpr = m_uiWindow->devicePixelRatio();
    const QPointF px(x * dpr, y * dpr);
    qcv::PointerPhase ph = qcv::PointerPhase::Press;
    switch (phase) {
        case 0: ph = qcv::PointerPhase::Press;   break;
        case 1: ph = qcv::PointerPhase::Move;    break;
        case 2: ph = qcv::PointerPhase::Release; break;
        default: return;
    }
    m_annotator->onPointerEvent(ph, px, timestamp);

    // Wake the render thread so the active stroke is drawn this
    // frame instead of waiting for the next decoded frame or
    // wake-cv timer tick. Mirrors PlayerWindow::mousePressEvent's
    // requestUpdate() call on macOS.
#ifdef QCV_NATIVE_PLAYER
    if (m_playerWindow) {
        if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
            if (auto *r = pw->renderer()) r->requestUpdate();
        }
    }
#endif
}

void WindowManager::pushInOutToTimer()
{
    if (m_timeline && m_timeline->timer()) {
        const double fps = m_timeline->timer()->frameRate();
        if (fps <= 0.0 || !hasInOutRange()) {
            m_timeline->timer()->setLoopRange(0.0, 0.0);
        } else {
            const double inSec  = static_cast<double>(m_inPoint)  / fps;
            // out is inclusive frame; +1 so the wrap fires *after*
            // the last frame is shown.
            const double outSec = static_cast<double>(m_outPoint + 1) / fps;
            m_timeline->timer()->setLoopRange(inSec, outSec);
        }
    }
    // Cache loop range: when set, the image-seq cache evicts +
    // fills inside [in, out] only. Save disk I/O for long
    // sequences where the user is reviewing a small region.
    if (m_imageSeqCache) {
        if (hasInOutRange()) {
            m_imageSeqCache->setLoopRange(m_inPoint, m_outPoint);
        } else {
            m_imageSeqCache->clearLoopRange();
        }
    }

    // Phase 7.7 Stage 9 polish — dual master timer needs the same
    // in/out + loop wiring. Single's PlaybackTimer has separate
    // setLooping(bool) + setLoopRange(seconds); dual's
    // DualPlaybackTimer expresses both as a single loop range
    // (frames), no separate boolean. Mapping:
    //   loopOff           → clearLoopRange (plays through)
    //   loopOn  + range   → setLoopRange(in, out)
    //   loopOn  + no range → setLoopRange(0, masterFrameCount-1)
    if (m_dualController && m_dualController->timer()) {
        auto *t = m_dualController->timer();
        int loIn = -1, loOut = -1;
        if (!m_loopEnabled) {
            t->clearLoopRange();
        } else if (hasInOutRange()) {
            loIn  = m_inPoint;
            loOut = m_outPoint;
            t->setLoopRange(loIn, loOut);
        } else {
            const int total = m_dualController->frameCount();
            if (total > 0) {
                loIn  = 0;
                loOut = total - 1;
                t->setLoopRange(loIn, loOut);
            } else {
                t->clearLoopRange();
            }
        }
        // Push to per-side image-seq sources too — without a loop
        // hint they LRU-evict the loop start as the playhead
        // approaches the loop end, and the wrap stalls on disk
        // re-read. With the hint, modular distances over [loIn,
        // loOut] keep loop-start frames cached and prefetched.
        auto pushToSeq = [&](qcv::dual::IDualSource *s) {
            auto *seq = dynamic_cast<qcv::dual::DualImageSeqSource *>(s);
            if (!seq) return;
            if (loIn >= 0 && loOut >= loIn) seq->setLoopRange(loIn, loOut);
            else                            seq->clearLoopRange();
        };
        pushToSeq(m_dualController->sourceA());
        pushToSeq(m_dualController->sourceB());
    }
}

void WindowManager::setLoopEnabled(bool on)
{
    if (m_loopEnabled == on) return;
    m_loopEnabled = on;
    // Playlist mode: timer-level looping must stay off (see
    // startPlaylist comment); the wrap is app-level. Single mode:
    // the timer / cache loops natively.
    if (m_timeline) {
        m_timeline->timer()->setLooping(m_playlistActive ? false : on);
    }
    if (m_imageSeqCache) m_imageSeqCache->setLooping(on);
    // Re-apply to dual timer (and to single's loop range; pushInOut
    // is idempotent on those).
    pushInOutToTimer();
    QSettings().setValue(QStringLiteral("playback/loopEnabled"), on);
    emit loopEnabledChanged();
}

double WindowManager::currentAudioSourceSeconds() const
{
    const double mp = m_timeline ? m_timeline->timer()->position() : 0.0;
    if (!m_playlistActive) return mp;
    const Clip *c = playlistActiveClip();
    if (!c) return mp;
    return std::max(0.0, c->sourceIn + (mp - c->startTime));
}

void WindowManager::setReviewSpeed(double speed)
{
    // Preset range only — the engines clamp wider, but the feature
    // is "review speeds", not shuttle (that's the A/J/D/L gesture).
    if (speed < 0.25) speed = 0.25;
    if (speed > 4.0)  speed = 4.0;
    if (qFuzzyCompare(m_reviewSpeed, speed)) return;
    m_reviewSpeed = speed;

    // Video pacing: the streaming decoder's publish sleep divides by
    // speed; the wall-clock timer covers image-seq / audio-only /
    // playlist clock paths.
    if (m_videoDecoder) m_videoDecoder->setPlaybackSpeed(speed);
    if (m_timeline)     m_timeline->timer()->setPlaybackSpeed(speed);

    // Audio: constant-pitch tempo stage, then re-anchor at the
    // current master position so old-tempo ring residue is dropped
    // and the sync servo restarts clean.
    if (m_audio && m_audio->hasAudio()) {
        m_audio->setPlaybackTempo(speed);
        m_audio->seek(currentAudioSourceSeconds());
    }

    // Dual island: master timer speed + both mixer sides, then a
    // same-frame seek re-anchors both decoders + per-side servos.
    if (m_dualController) {
        if (m_dualController->timer()) {
            m_dualController->timer()->setSpeed(speed);
        }
        if (m_dualController->audio()) {
            m_dualController->audio()->setTempoBoth(speed);
        }
        m_dualController->seekToFrame(m_dualController->currentFrame());
    }

    qInfo("WindowManager: review speed %.2fx", speed);
    emit reviewSpeedChanged();
}

void WindowManager::cycleReviewSpeed()
{
    // R key: cycle the preset ring. Starts from whatever the current
    // speed is closest to, so external setReviewSpeed values slot in.
    static constexpr double kSpeeds[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
    constexpr int n = static_cast<int>(sizeof(kSpeeds) / sizeof(kSpeeds[0]));
    int nearest = 0;
    double best = 1e9;
    for (int i = 0; i < n; ++i) {
        const double d = std::abs(kSpeeds[i] - m_reviewSpeed);
        if (d < best) { best = d; nearest = i; }
    }
    setReviewSpeed(kSpeeds[(nearest + 1) % n]);
}

void WindowManager::setBackgroundMode(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 3) mode = 3;
    if (m_backgroundMode == mode) return;
    m_backgroundMode = mode;
#ifdef QCV_NATIVE_PLAYER
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        if (auto *r = pw->renderer()) {
            r->setBackgroundMode(static_cast<qcv::BackgroundMode>(mode));
        }
    }
#endif
    QSettings().setValue(QStringLiteral("display/backgroundMode"), mode);
    emit backgroundModeChanged();
}

void WindowManager::setSplitPos(qreal pos)
{
    if (pos < 0.0) pos = 0.0;
    if (pos > 1.0) pos = 1.0;
    if (qFuzzyCompare(m_splitPos, pos)) return;
    m_splitPos = pos;
#ifdef QCV_NATIVE_PLAYER
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        if (auto *r = pw->renderer()) {
            r->setSplitPos(static_cast<float>(pos));
        }
    }
#endif
    emit splitPosChanged();
}

void WindowManager::setDiffGain(qreal gain)
{
    // Difference-mode amplification. 1.0 = raw Adobe-style abs(A-B).
    if (gain < 1.0)  gain = 1.0;
    if (gain > 16.0) gain = 16.0;
    if (qFuzzyCompare(m_diffGain, gain)) return;
    m_diffGain = gain;
#ifdef QCV_NATIVE_PLAYER
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        if (auto *r = pw->renderer()) {
            r->setDiffGain(static_cast<float>(gain));
        }
    }
#endif
    emit diffGainChanged();
}

void WindowManager::checkForUpdates()
{
    qcv::checkForUpdatesNow();
}

bool WindowManager::autoUpdateChecks() const
{
    return qcv::automaticUpdateChecks();
}

void WindowManager::setAutoUpdateChecks(bool enabled)
{
    if (qcv::automaticUpdateChecks() == enabled) return;
    qcv::setAutomaticUpdateChecks(enabled);
    emit autoUpdateChecksChanged();
}

void WindowManager::setSplitSeamActive(bool active)
{
#ifdef QCV_NATIVE_PLAYER
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        if (auto *r = pw->renderer()) {
            r->setSplitSeamHighlight(active ? 1.0f : 0.0f);
        }
    }
#else
    Q_UNUSED(active);
#endif
}

void WindowManager::setLoadingActive(bool on)
{
#ifdef QCV_NATIVE_PLAYER
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        if (auto *r = pw->renderer()) {
            r->setLoadingActive(on);
        }
    }
#else
    Q_UNUSED(on);
#endif
}

namespace {

// Notice copy for an undecodable item. ARRI containers get a camera-
// named, action-oriented message; anything else gets a generic line.
// Called only once we've decided the item can't be decoded.
QString buildViewportNoticeText(const MediaItem &item)
{
    const VideoMetadata &v = item.video;
    const bool isArri =
        v.cameraVendor.compare(QStringLiteral("ARRI"), Qt::CaseInsensitive) == 0
        || v.cameraModel.startsWith(QStringLiteral("ALEXA"), Qt::CaseInsensitive)
        || v.cameraModel.startsWith(QStringLiteral("AMIRA"), Qt::CaseInsensitive);
    if (isArri) {
        const QString cam = v.cameraModel.isEmpty()
            ? QStringLiteral("ARRIRAW")
            : (v.cameraModel + QStringLiteral(" ARRIRAW"));
        return QStringLiteral(
            "%1\n\nUn-debayered camera raw can’t be displayed here.\n"
            "Review the ProRes / dailies, or convert in\n"
            "DaVinci Resolve or the ARRI Reference Tool.").arg(cam);
    }
    return QStringLiteral(
        "Unsupported format\n\nThis file can’t be played here.");
}

} // namespace

void WindowManager::setViewportNotice(const QString &text)
{
    // Phase 3 (hazy-weaving-reddy): the notice is now an in-scene QML
    // card, not a CPU QImage composited into the viewport. We just
    // publish the text and recompute the shared viewport cover — a
    // non-empty notice hides the native surface so the QML card (drawn
    // in the centerStage region) is the top visible plane. This is
    // NON-blocking: no scrim, no input gate, chrome stays live so the
    // user can load other media. The old hidden-surface can't show a
    // stale frame, so the prior clearSourceAState() dance is gone.
    if (text == m_viewportNoticeText) return;
    m_viewportNoticeText = text;
    emit viewportNoticeTextChanged();
    recomputeViewportCover();
}

void WindowManager::clearViewportNotice()
{
    setViewportNotice(QString());
}

void WindowManager::applyPixelAspectToRenderer()
{
    auto *r = fetchActiveRenderer(m_playerWindow.data());
    if (!r || !m_project) return;
    // A side = the active/scope clip (resolves to the underlying clip
    // in playlist mode, the active item otherwise — and the A clip in
    // dual mode). B side = the dual B source (empty in single mode →
    // 1:1, a harmless no-op).
    const MediaItem *a = m_project->findItem(audioRoutingScopeMediaItemId());
    const std::pair<int, int> pa =
        a ? effectivePixelAspectFor(*a) : std::pair<int, int>{ 1, 1 };
    r->setPixelAspectA(pa.first, pa.second);
    r->setRotationA(a ? effectiveRotationFor(*a) : 0);
    const MediaItem *b = m_project->findItem(m_project->bSourceMediaId());
    const std::pair<int, int> pb =
        b ? effectivePixelAspectFor(*b) : std::pair<int, int>{ 1, 1 };
    r->setPixelAspectB(pb.first, pb.second);
    r->setRotationB(b ? effectiveRotationFor(*b) : 0);
    r->requestUpdate();
}

void WindowManager::evaluateViewportNoticeFor(const QString &mediaId)
{
    if (!m_project || mediaId.isEmpty()) return;
    // Only the active (currently-shown) source drives the viewport
    // notice; metadata arriving for a background item must not change
    // what's on screen.
    if (m_project->activeItemId() != mediaId) return;
    const MediaItem *item = m_project->findItem(mediaId);
    if (item && item->video.loaded && item->video.unsupportedCodec) {
        setViewportNotice(buildViewportNoticeText(*item));
    }
    // NOTE: never clear here — clearing is owned by a successful load,
    // so this reconciliation pass can't race the load-failure path
    // that may have just set the notice before metadata arrived.
}

// Brief defer so the menu popup closes and the render thread starts
// drawing the spinner before the main thread blocks in the open call.
static constexpr int kOpenDeferMs = 16;

void WindowManager::openMediaPaths(const QStringList &paths)
{
    if (!m_project || paths.isEmpty()) return;
    setLoadingActive(true);
    const QStringList ps = paths;
    QTimer::singleShot(kOpenDeferMs, this, [this, ps]() {
        QString lastId;
        for (const QString &p : ps) {
            if (p.isEmpty()) continue;
            // URL-shaped entries (srt:// — recents, dropped links)
            // route to the live-stream add; file paths never
            // contain "://".
            const QString id = p.contains(QLatin1String("://"))
                                   ? m_project->addLiveStream(p)
                                   : m_project->addMediaFile(p);
            if (!id.isEmpty()) lastId = id;
        }
        if (!lastId.isEmpty()) m_project->setActiveItem(lastId);
        setLoadingActive(false);
    });
}

void WindowManager::addMediaPaths(const QStringList &paths)
{
    if (!m_project || paths.isEmpty()) return;
    setLoadingActive(true);
    const QStringList ps = paths;
    QTimer::singleShot(kOpenDeferMs, this, [this, ps]() {
        for (const QString &p : ps) {
            if (p.isEmpty()) continue;
            if (p.contains(QLatin1String("://"))) {
                m_project->addLiveStream(p);
            } else {
                m_project->addMediaFile(p);
            }
        }
        setLoadingActive(false);
    });
}

void WindowManager::openProjectPath(const QString &path)
{
    if (!m_project || path.isEmpty()) return;
    setLoadingActive(true);
    const QString p = path;
    QTimer::singleShot(kOpenDeferMs, this, [this, p]() {
        m_project->openProject(p);
        setLoadingActive(false);
    });
}

void WindowManager::applyDualImageSeqStride()
{
    if (!m_dualController || !m_project) return;
    // Side A's source item is the active item; B's is the b-source —
    // same convention as setDualImageSeqLayer. Each side gets its own
    // stored per-item stride (default 1 = every frame).
    auto applySide = [this](char sideCh, const QString &itemId) {
        auto *src = dualImageSeqSide(m_dualController.get(), sideCh);
        if (!src) return;   // side isn't an image sequence
        int stride = 1;
        if (const MediaItem *it = m_project->findItem(itemId))
            stride = std::clamp(it->imageSeq.cacheStride, 1, 4);
        src->setCacheStride(stride);
    };
    applySide('A', m_project->activeItemId());
    applySide('B', m_project->bSourceMediaId());
}

// ---------------------------------------------------------------------------
// Phase 7.4.a — image-sequence playback
// ---------------------------------------------------------------------------

void WindowManager::startImageSequence(
    const MediaItem &item,
    std::unique_ptr<ImageSequenceCache> prewarmedCache)
{
    // Suppress the metadataChanged hook's timeline rebuild while we
    // tear down the previous video decoder; the new sequence's
    // metadata flows directly into loadSingleMedia below, not through
    // the videoDecoder.metadataChanged hook.
    m_suppressTimelineRebuild = true;
    stopImageSequence();
    if (m_videoDecoder) m_videoDecoder->close();

    // Phase 7.4.b.4 — single ImageSequenceCache for ALL formats
    // (EXR/TIFF/PNG/JPEG), mirroring old app's timeline_cache.cpp
    // :330-433 dispatch. Per-format loader plugs into the cache as
    // IImageLoader; the cache owns prefetch / threading / eviction.
    // Phase 3.H.4 — when a prewarmed cache is handed in, skip the
    // make_unique + initialize and reuse it (CacheThread is already
    // filling its read-ahead window).
    std::unique_ptr<ImageSequenceCache> cache = std::move(prewarmedCache);
    if (!cache) {
        cache = std::make_unique<ImageSequenceCache>(
            item.imageSeq.directory.toStdString(),
            item.imageSeq.pattern.toStdString());
        if (!cache->initialize(item.imageSeq.startFrame,
                               item.imageSeq.endFrame,
                               item.imageSeq.frameRate,
                               PipelineMode::NORMAL,
                               item.imageSeq.layer.toStdString())) {
            qWarning("WindowManager: image sequence init failed for '%s'",
                     qPrintable(item.name));
            m_suppressTimelineRebuild = false;
            return;
        }
        // Seed the cache's failed-frame skip list with on-disk gaps
        // discovered at sequence-detection time. The cache would
        // also discover them lazily as prefetch reached each one,
        // but pre-seeding lets the timeline render red ticks the
        // moment the sequence opens.
        if (!item.imageSeq.missingFrames.isEmpty()) {
            std::vector<int> seed;
            seed.reserve(item.imageSeq.missingFrames.size());
            for (int f : item.imageSeq.missingFrames) seed.push_back(f);
            cache->setKnownMissingFrames(seed);
        }
    } else {
        qInfo("WindowManager: using prewarmed cache for '%s'",
              qPrintable(item.name));
    }
    m_suppressTimelineRebuild = false;

    const int    width      = cache->width();
    const int    height     = cache->height();
    const int    frameCount = cache->frameCount();
    const double fps        = cache->fps();
    m_imageSeqCache = std::move(cache);

    // Write probed dimensions back to the MediaItem so the Inspector's
    // Resolution row reads `<W> × <H>` instead of "(probing…)". The
    // import-time detectImageSequence path only fills file-list metadata
    // (pattern, frameCount, fps); width/height are zero until the first
    // frame is decoded and the cache reports its actual dimensions.
    if (m_project) {
        m_project->setSequenceProbedDimensions(item.id, width, height);
    }

    // Re-apply this item's per-session cache-stride choice (set by
    // a prior pill click on this same item). Cache constructor
    // defaults to 1; if the user had picked 2/3/4 for this item
    // before navigating away and back, we want it to land at that
    // stride immediately, not at the default. See
    // ImageSequenceData::cacheStride for why this is in-memory only.
    const int storedStride = std::clamp(item.imageSeq.cacheStride, 1, 4);
    if (storedStride != m_imageSeqCache->cacheStride()) {
        m_imageSeqCache->setCacheStride(storedStride);
    }

    qInfo("startImageSequence: name='%s' "
          "startFrame=%d endFrame=%d frameCount=%d fps=%.3f "
          "(timeline duration will be %.3fs)",
          qPrintable(item.name),
          item.imageSeq.startFrame, item.imageSeq.endFrame,
          frameCount, fps,
          (fps > 0.0) ? frameCount / fps : 0.0);

#ifdef QCV_NATIVE_PLAYER
    // Phase 7.5 B.6.2: native renderer drives uploads off the render
    // thread via MetalGpuUploadThread. Hook the cache → upload thread
    // chain by handing the cache pointer to the renderer; the renderer
    // installs its own setGpuPushCallback.
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        if (auto *r = pw->renderer()) {
            r->setImageSeqCache(m_imageSeqCache.get());
        }
    }
#endif

    // Build the timeline directly from the cache's metadata —
    // VideoDecoder is no longer used as the metadata façade for
    // image sequences (Phase 7.4.b.4 reset; openExternal is gone).
    // Phase 3.H.4 — in playlist mode the timeline is already
    // playlist-shaped (loadPlaylist built it); skip the
    // loadSingleMedia rewrite, only flip the wall-clock policy
    // so the image-seq driver timer's update() advances time.
    if (m_timeline) {
        if (!m_playlistActive) {
            const double dur = (fps > 0.0 && frameCount > 0)
                               ? static_cast<double>(frameCount) / fps
                               : 0.0;
            const QString itemId = m_project ? m_project->activeItemId()
                                              : QString();
            const MediaItem *mit = (m_project && !itemId.isEmpty())
                                    ? m_project->findItem(itemId)
                                    : nullptr;
            if (mit) {
                m_timeline->loadSingleMedia(*mit, dur, fps,
                                              /*hasAudio=*/false);
            }
        }
        m_timeline->timer()->setClockPolicy(PlaybackTimer::WallClock);
    }

    // Surface metadata + cache pointer to QML so the renderer
    // binding (PlayerWindow.qml -> PlayerRhiItem.imageSeqCache)
    // and the inspector see a consistent view.
    emit imageSeqCacheChanged();
    emit imageSeqMetadataChanged();

    m_lastBufferedAhead    = -1;
    m_lastBufferedBehind   = -1;
    m_lastBufferSize       = -1;
    m_lastFailedFrameCount = -1;

    // Clock pump — drives PlaybackTimer::update() at 30 Hz so the
    // wall clock advances and frame-boundary crossings fire
    // frameAdvanced. Pure clock pump; no publish work runs here.
    if (!m_imageSeqDriverTimer) {
        m_imageSeqDriverTimer = new QTimer(this);
        m_imageSeqDriverTimer->setTimerType(Qt::PreciseTimer);
        connect(m_imageSeqDriverTimer, &QTimer::timeout, this, [this] {
            if (m_timeline) m_timeline->timer()->update();
            // Pump buffer-state polling here too — covers paused
            // states (no positionChanged fires when paused, but the
            // cache continues filling in the background after a
            // seek and the strip needs to reflect that).
            pollImageSeqBufferStatus();
            // Audio mode (re-)uses the same timer; feed its drift-
            // correction so loop-wrap re-aligns AudioPlayer.
            if (m_audioActive && m_audio
                && m_audio->hasAudio() && m_timeline) {
                m_audio->update(m_timeline->timer()->position());
            }
        });
    }
    m_imageSeqDriverTimer->setInterval(33); // ~30 Hz
    m_imageSeqDriverTimer->start();

    // Flip imageSeqActive so QML transport bindings (TransportBar,
    // TimelinePanel) take the image-seq routing branches. Renderer-
    // side gating is purely on m_imageSeqCache != nullptr (set via
    // the Q_PROPERTY binding in PlayerWindow.qml).
    if (!m_imageSeqActive) {
        m_imageSeqActive = true;
        emit imageSeqActiveChanged();
    }

    // Prime: position the playhead at frame 0 so the next render
    // tick picks the right frame to pull from the cache.
    if (m_imageSeqCache) m_imageSeqCache->updatePlayhead(0, /*force_seek=*/true);
    m_imageSeqRebuffering = false;   // fresh media, fresh brake state

    // Mismatched-clip-length: if Source B is already loaded when a
    // sequence is loaded into A, override timer.duration to the
    // longer of the two and refresh per-side activity.
    applyMasterDuration();
    pushSourceActivity();
}

void WindowManager::pollImageSeqBufferStatus()
{
    // Single-flow cache poll (existing path). Short-circuits when no
    // single-flow image-seq is active.
    if (m_imageSeqCache) {
        const int newAhead  = m_imageSeqCache->bufferedAhead();
        const int newBehind = m_imageSeqCache->bufferedBehind();
        const int newSize   = m_imageSeqCache->bufferSize();
        const int newFailed = m_imageSeqCache->failedFrameCount();
        if (newAhead  != m_lastBufferedAhead  ||
            newBehind != m_lastBufferedBehind ||
            newSize   != m_lastBufferSize     ||
            newFailed != m_lastFailedFrameCount) {
            m_lastBufferedAhead    = newAhead;
            m_lastBufferedBehind   = newBehind;
            m_lastBufferSize       = newSize;
            m_lastFailedFrameCount = newFailed;
            emit imageSeqBufferStatusChanged();
        }

        // ---- Rebuffer brake (2026-07-08 EXR perf audit). ----
        // When the playhead reaches the cache edge and keeps moving,
        // the keep-window slides with it — and every in-flight decode
        // (16 × ~250 ms deep on 4K DWAB) completes into a position
        // that is already behind the window and is EVICTED ON
        // ARRIVAL. All workers burn at 100% while effective refill
        // drops to ~2 fps; the cache can never catch a moving
        // playhead again. Holding the timer freezes the window, the
        // same workers refill at full rate, and playback resumes
        // with ~2/3 s of runway. (This is the overrun handling the
        // old app had; the port carried only a write-only flag.)
        PlaybackTimer *timer =
            (m_timeline && !m_dualController) ? m_timeline->timer()
                                              : nullptr;
        if (timer) {
            constexpr int kEngageAhead = 2;   // starved
            constexpr int kResumeAhead = 24;  // ~1 s @ 24 fps
            if (!m_imageSeqRebuffering) {
                // Near-end guard: a non-looping playhead approaching
                // the final frames legitimately has no read-ahead.
                const int playhead   = m_imageSeqCache->currentPlayhead();
                const int frameCount = m_imageSeqCache->frameCount();
                const bool nearEnd = !m_loopEnabled
                    && playhead >= frameCount - kEngageAhead - 2;
                if (timer->isPlaying() && !nearEnd
                    && newAhead <= kEngageAhead) {
                    m_imageSeqRebuffering = true;
                    timer->pause();
                    qInfo("WindowManager: image-seq rebuffer HOLD "
                          "(ahead=%d playhead=%d)", newAhead, playhead);
                }
            } else if (!m_userWantsPlayback) {
                // User paused during the hold — playback is theirs
                // again; don't auto-resume.
                m_imageSeqRebuffering = false;
            } else if (newAhead >= kResumeAhead) {
                m_imageSeqRebuffering = false;
                timer->play();
                qInfo("WindowManager: image-seq rebuffer RESUME "
                      "(ahead=%d)", newAhead);
            }
        }
    }

    // Dual-mode per-side poll. Cheap when m_dualController is null
    // (early return). Each side dynamic_casts to DualImageSeqSource —
    // null means that side isn't an image sequence; cached fields
    // get zeroed/false, and the QML strip's visibility gate hides it.
    // Emit dualImageSeqStatusChanged only on delta to avoid QML
    // binding storms during steady-state playback.
    if (!m_dualController) return;
    auto *sa = dualImageSeqSide(m_dualController.get(), 'A');
    auto *sb = dualImageSeqSide(m_dualController.get(), 'B');
    const int    nAheadA  = sa ? sa->bufferedAhead()  : 0;
    const int    nBehindA = sa ? sa->bufferedBehind() : 0;
    const int    nFcA     = sa ? sa->frameCount()     : 0;
    const bool   nActA    = sa != nullptr;
    const double nFpsA    = sa ? sa->fps()            : 0.0;
    const int    nAheadB  = sb ? sb->bufferedAhead()  : 0;
    const int    nBehindB = sb ? sb->bufferedBehind() : 0;
    const int    nFcB     = sb ? sb->frameCount()     : 0;
    const bool   nActB    = sb != nullptr;
    const double nFpsB    = sb ? sb->fps()            : 0.0;
    if (nAheadA  == m_lastDualAheadA  && nBehindA == m_lastDualBehindA  &&
        nFcA     == m_lastDualFrameCountA && nActA == m_lastDualActiveA &&
        qFuzzyCompare(nFpsA, m_lastDualFpsA)                             &&
        nAheadB  == m_lastDualAheadB  && nBehindB == m_lastDualBehindB  &&
        nFcB     == m_lastDualFrameCountB && nActB == m_lastDualActiveB &&
        qFuzzyCompare(nFpsB, m_lastDualFpsB)) {
        return;
    }
    m_lastDualAheadA      = nAheadA;
    m_lastDualBehindA     = nBehindA;
    m_lastDualFrameCountA = nFcA;
    m_lastDualActiveA     = nActA;
    m_lastDualFpsA        = nFpsA;
    m_lastDualAheadB      = nAheadB;
    m_lastDualBehindB     = nBehindB;
    m_lastDualFrameCountB = nFcB;
    m_lastDualActiveB     = nActB;
    m_lastDualFpsB        = nFpsB;
    emit dualImageSeqStatusChanged();
}

QVariantList WindowManager::imageSeqFailedFrames() const
{
    QVariantList out;
    if (!m_imageSeqCache) return out;
    const std::vector<int> frames = m_imageSeqCache->failedFrames();
    out.reserve(static_cast<int>(frames.size()));
    for (int f : frames) out.append(f);
    return out;
}

int WindowManager::imageSeqBufferedAhead() const
{
    return m_imageSeqCache ? m_imageSeqCache->bufferedAhead() : 0;
}

int WindowManager::imageSeqBufferedBehind() const
{
    return m_imageSeqCache ? m_imageSeqCache->bufferedBehind() : 0;
}

int WindowManager::imageSeqBufferedAheadCoverage() const
{
    return m_imageSeqCache ? m_imageSeqCache->bufferedAheadCoverage() : 0;
}

int WindowManager::imageSeqBufferedBehindCoverage() const
{
    return m_imageSeqCache ? m_imageSeqCache->bufferedBehindCoverage() : 0;
}

int WindowManager::imageSeqReadAheadFrames() const
{
    return m_imageSeqCache ? m_imageSeqCache->readAheadFrames() : 0;
}

int WindowManager::imageSeqReadBehindFrames() const
{
    return m_imageSeqCache ? m_imageSeqCache->readBehindFrames() : 0;
}

int WindowManager::imageSeqFrameCount() const
{
    return m_imageSeqCache ? m_imageSeqCache->frameCount() : 0;
}

// ---------------------------------------------------------------------------
// Dual-mode per-side image-seq cache accessors. Mirror of the single-flow
// imageSeqBuffered* getters above, but sourced from DualImageSeqSource via
// the controller. 0 / false when the matching side isn't an image sequence
// (or when dual mode isn't active). Polled at 33 ms via m_dualBufferPollTimer
// → pollImageSeqBufferStatus(). DualImageSeqSource::bufferedAhead/Behind
// report COVERAGE (span to the farthest cached frame), so a cache-stride
// window draws the full band — no separate coverage accessors needed.
//
// Helper `dualImageSeqSide` lives in the anonymous namespace at the top
// of this file (next to fetchActiveRenderer) so both these getters and
// pollImageSeqBufferStatus can use it.
// ---------------------------------------------------------------------------

int WindowManager::dualImageSeqBufferedAheadA() const
{
    auto *s = dualImageSeqSide(m_dualController.get(), 'A');
    return s ? s->bufferedAhead() : 0;
}

int WindowManager::dualImageSeqBufferedBehindA() const
{
    auto *s = dualImageSeqSide(m_dualController.get(), 'A');
    return s ? s->bufferedBehind() : 0;
}

int WindowManager::dualImageSeqFrameCountA() const
{
    auto *s = dualImageSeqSide(m_dualController.get(), 'A');
    return s ? s->frameCount() : 0;
}

bool WindowManager::dualImageSeqIsActiveA() const
{
    return dualImageSeqSide(m_dualController.get(), 'A') != nullptr;
}

double WindowManager::dualImageSeqFpsA() const
{
    auto *s = dualImageSeqSide(m_dualController.get(), 'A');
    return s ? s->fps() : 0.0;
}

int WindowManager::dualImageSeqBufferedAheadB() const
{
    auto *s = dualImageSeqSide(m_dualController.get(), 'B');
    return s ? s->bufferedAhead() : 0;
}

int WindowManager::dualImageSeqBufferedBehindB() const
{
    auto *s = dualImageSeqSide(m_dualController.get(), 'B');
    return s ? s->bufferedBehind() : 0;
}

int WindowManager::dualImageSeqFrameCountB() const
{
    auto *s = dualImageSeqSide(m_dualController.get(), 'B');
    return s ? s->frameCount() : 0;
}

bool WindowManager::dualImageSeqIsActiveB() const
{
    return dualImageSeqSide(m_dualController.get(), 'B') != nullptr;
}

double WindowManager::dualImageSeqFpsB() const
{
    auto *s = dualImageSeqSide(m_dualController.get(), 'B');
    return s ? s->fps() : 0.0;
}

int WindowManager::imageSeqCacheStride() const
{
    // Live cache wins when present (cheapest + always coherent).
    // Otherwise fall back to the active item's stored preference so
    // the inspector pill highlights correctly the moment a sequence
    // is loaded — startImageSequence applies this same value to the
    // freshly-spun-up cache. Default 1 (Every Frame) when nothing
    // active.
    if (m_imageSeqCache) return m_imageSeqCache->cacheStride();
    if (m_project) {
        const MediaItem *it = m_project->findItem(m_project->activeItemId());
        if (it) return it->imageSeq.cacheStride;
    }
    return 1;
}

QVariantList WindowManager::audioChannelPeaks() const
{
    if (m_dualController && m_dualController->audio()) {
        return m_dualController->audio()->audioChannelPeaksA();
    }
    return m_audio ? m_audio->audioChannelPeaks() : QVariantList();
}

QStringList WindowManager::audioChannelNames() const
{
    if (m_dualController && m_dualController->audio()) {
        return m_dualController->audio()->audioChannelNamesA();
    }
    return m_audio ? m_audio->audioChannelNames() : QStringList();
}

QVariantList WindowManager::audioChannelPeaksB() const
{
    if (m_dualController && m_dualController->audio()) {
        return m_dualController->audio()->audioChannelPeaksB();
    }
    return QVariantList();
}

QStringList WindowManager::audioChannelNamesB() const
{
    if (m_dualController && m_dualController->audio()) {
        return m_dualController->audio()->audioChannelNamesB();
    }
    return QStringList();
}

void WindowManager::pollAudioMeters()
{
    // Drain a single peak vector (A peaks then B peaks interleaved)
    // and only emit the change signal when any sample moved by more
    // than ~0.5 dB linear (~0.005). Suppresses pointless QML rebinds
    // when audio is paused (peaks repeat) or sitting at silence.
    QVariantList aPeaks = audioChannelPeaks();
    QVariantList bPeaks = audioChannelPeaksB();
    std::vector<float> next;
    next.reserve(aPeaks.size() + bPeaks.size());
    for (const QVariant &v : aPeaks) next.push_back(v.toFloat());
    for (const QVariant &v : bPeaks) next.push_back(v.toFloat());

    bool changed = (next.size() != m_lastEmittedPeaks.size());
    if (!changed) {
        for (size_t i = 0; i < next.size(); ++i) {
            if (std::fabs(next[i] - m_lastEmittedPeaks[i]) > 0.005f) {
                changed = true;
                break;
            }
        }
    }
    if (!changed) return;

    m_lastEmittedPeaks = std::move(next);
    emit audioMetersChanged();
}

void WindowManager::setImageSeqCacheStride(int stride)
{
    stride = std::clamp(stride, 1, 4);

    // Write through to the active item's MediaItem so the choice
    // sticks for this item across pool selection within the session.
    // Switching to another item picks up that item's stored value;
    // returning here picks this one back up. Lost on app close (the
    // field is in-memory only — see ImageSequenceData::cacheStride).
    if (m_project) {
        if (MediaItem *it = m_project->findItem(m_project->activeItemId())) {
            it->imageSeq.cacheStride = stride;
        }
    }

    // Dual mode routes through setDualImageSeqStride (per-side), so
    // this single-flow setter is a no-op on the live source there.
    if (!m_imageSeqCache) {
        // No live cache to update; emit so the inspector pill rebinds
        // against the new stored value.
        emit imageSeqCacheStrideChanged();
        return;
    }
    if (stride == m_imageSeqCache->cacheStride()) {
        // Already at this stride. Re-emit so any QML binding that
        // was stale (e.g. raced a previous startImageSequence apply)
        // gets a chance to re-evaluate.
        emit imageSeqCacheStrideChanged();
        return;
    }
    m_imageSeqCache->setCacheStride(stride);
    emit imageSeqCacheStrideChanged();
    // Bump the buffer-status notify too — the cache indicator
    // recolors immediately as old-grid frames evict and new-grid
    // ones queue up.
    emit imageSeqBufferStatusChanged();
}

void WindowManager::setImageSeqLayer(const QString &layer)
{
    if (!m_imageSeqCache) return;
    const std::string s = layer.toStdString();
    if (s == m_imageSeqCache->exrLayer()) return;
    m_imageSeqCache->setExrLayer(s);
    // Persist the selection on the MediaItem so a re-load picks
    // up the user's last-chosen layer. Re-emits activeItemIdChanged
    // through ProjectManager so the inspector dropdown's binding
    // refreshes.
    if (m_project) {
        m_project->setSequenceLayer(m_project->activeItemId(), layer);
    }
    emit imageSeqBufferStatusChanged();
}

void WindowManager::setDualImageSeqLayer(const QString &side,
                                            const QString &layer)
{
    if (!m_dualController) return;
    const bool isA = (side == QLatin1String("A"));
    qcv::dual::IDualSource *src =
        isA ? m_dualController->sourceA() : m_dualController->sourceB();
    auto *seq = dynamic_cast<qcv::dual::DualImageSeqSource *>(src);
    if (!seq) return;     // side isn't an image sequence — nothing to do

    seq->setLayer(layer); // flushes the source's cache; workers refill
                          // at the new layer on next decode tick

    // Persist on the MediaItem so a future re-open / DualPair load
    // picks up the user's choice.
    if (m_project) {
        const QString id = isA ? m_project->activeItemId()
                               : m_project->bSourceMediaId();
        if (!id.isEmpty()) m_project->setSequenceLayer(id, layer);
    }

    // Bump the controller's generation so the compositor drops its
    // cached per-side texture next render — without this the
    // previously-displayed pixels would linger on screen until the
    // ring buffer fills again from the new layer (looks like the
    // layer pick had no effect for ~1s).
    m_dualController->bumpTimelineGeneration();
}

void WindowManager::setDualImageSeqStride(const QString &side, int stride)
{
    stride = std::clamp(stride, 1, 4);
    if (!m_dualController) return;
    const bool isA = (side == QLatin1String("A"));
    auto *seq = dualImageSeqSide(m_dualController.get(), isA ? 'A' : 'B');
    if (!seq) return;     // side isn't an image sequence — nothing to do

    seq->setCacheStride(stride);

    // Persist on the side's source MediaItem so a re-open / DualPair
    // load restores the choice — independent of the other side.
    if (m_project) {
        const QString id = isA ? m_project->activeItemId()
                               : m_project->bSourceMediaId();
        if (MediaItem *it = m_project->findItem(id)) {
            it->imageSeq.cacheStride = stride;
        }
    }
    emit imageSeqCacheStrideChanged();
    emit dualImageSeqStatusChanged();
}

int WindowManager::dualImageSeqStride(const QString &side) const
{
    const bool isA = (side == QLatin1String("A"));
    if (m_dualController) {
        if (auto *seq = dualImageSeqSide(m_dualController.get(), isA ? 'A' : 'B'))
            return seq->cacheStride();
    }
    // Fall back to the side's stored item stride (e.g. before the
    // source spins up) so the pill highlights correctly.
    if (m_project) {
        const QString id = isA ? m_project->activeItemId()
                               : m_project->bSourceMediaId();
        if (const MediaItem *it = m_project->findItem(id))
            return std::clamp(it->imageSeq.cacheStride, 1, 4);
    }
    return 1;
}

void WindowManager::closeActiveMedia()
{
    // Single chokepoint: tear down whichever subsystem owns the
    // currently-loaded media so a subsequent load starts from a
    // clean slate. Order matters — renderer pointers nulled first
    // (else a mid-tear-down render samples a half-destroyed cache),
    // then the cache / decoder, then the timeline.
    qInfo("closeActiveMedia: imageSeqActive=%s videoSourcePath='%s'",
          m_imageSeqActive ? "yes" : "no",
          m_videoDecoder
            ? qPrintable(m_videoDecoder->sourcePath())
            : "");

    // Defensive: if a fast-seek gesture is in flight (user holding
    // A/D), stop it before tearing down the decoders it might be
    // about to seek. This also clears m_fastSeekDeferDecode so a
    // subsequent load doesn't inherit the previous gesture's flag.
    stopFastSeek();

    // Live-stream teardown FIRST: the live worker publishes into
    // m_videoDecoder's slot, so it must be joined before the
    // m_videoDecoder->close() below clears that slot. Also the
    // disconnect-on-switch contract: the sender is a one-connection
    // listener — never hold the socket for a source we're leaving.
    stopLiveStream();

    // Phase 3.H.2 — drop the playlist orchestrator state so the
    // boundary handlers don't re-fire mid-tear-down. closeActiveMedia
    // runs at the start of every load path, so the new load is free
    // to flip m_playlistActive back on.
    m_playlistActive = false;
    m_playlistCurrentClipIndex = -1;
    m_playlistAdvancing = false;
    cancelImageSeqPrewarm();
    // Leaving playlist mode → restore the user's loop preference on
    // the timer (playlist mode forced timer-level loop off).
    if (m_timeline) m_timeline->timer()->setLooping(m_loopEnabled);

    if (m_imageSeqCache) {
        stopImageSequence();
    }

    // Audio-only mode teardown — stop the wallclock pump (unless
    // image-seq is still using it, which won't happen here since
    // we just stopped it above) and clear the flag so the next
    // load enters fresh.
    if (m_audioActive) {
        if (m_imageSeqDriverTimer) m_imageSeqDriverTimer->stop();
        m_audioActive = false;
        emit audioActiveChanged();
    }

    if (m_videoDecoder &&
        !m_videoDecoder->sourcePath().isEmpty()) {
        m_videoDecoder->close();
    }

    // Dual-source policy: media change for A invalidates B too.
    // Cleaner than trying to keep B alive across an A swap (the
    // alignment / playhead state would be ambiguous). User reloads
    // B if they wanted to keep it.
    if (m_videoDecoderB &&
        !m_videoDecoderB->sourcePath().isEmpty()) {
        m_videoDecoderB->close();
    }

    // Flush renderer-side cached frames + pending atomic-pair
    // handles for BOTH sides so the next session starts clean. Also
    // restore both activity flags so the next single-source load
    // starts with a renderable canvas.
#ifdef QCV_NATIVE_PLAYER
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        if (auto *r = pw->renderer()) {
            r->clearSourceAState();
            r->clearSourceBState();
            r->setVideoDecoderB(nullptr);
            r->setSourceActivity(true, true);
        }
    }
#endif

    if (m_timeline) {
        m_timeline->clear();
    }

    // In/Out are per-clip — discard with the media.
    if (m_inPoint != -1 || m_outPoint != -1) {
        m_inPoint = -1;
        m_outPoint = -1;
        pushInOutToTimer();
        emit inOutPointsChanged();
    }
}

// ---- Phase 3.H.2 — playlist orchestrator helpers --------------------

const Clip *WindowManager::playlistActiveClip() const
{
    if (!m_playlistActive || !m_timeline) return nullptr;
    const Timeline &t = m_timeline->timeline();
    if (t.tracks.isEmpty()) return nullptr;
    const Track &track = t.tracks.first();
    if (m_playlistCurrentClipIndex < 0 ||
        m_playlistCurrentClipIndex >= track.clips.size()) {
        return nullptr;
    }
    return &track.clips[m_playlistCurrentClipIndex];
}

const Clip *WindowManager::clipAtTimelineSec(double tSec) const
{
    if (!m_playlistActive || !m_timeline) return nullptr;
    const Timeline &t = m_timeline->timeline();
    if (t.tracks.isEmpty()) return nullptr;
    const Track &track = t.tracks.first();
    for (const Clip &c : track.clips) {
        if (c.isGap) continue;
        if (tSec >= c.startTime && tSec < c.startTime + c.duration) {
            return &c;
        }
    }
    return nullptr;
}

int WindowManager::playlistFindNonGapClip(int from, int direction) const
{
    if (!m_timeline) return -1;
    const Timeline &t = m_timeline->timeline();
    if (t.tracks.isEmpty()) return -1;
    const Track &track = t.tracks.first();
    int i = from;
    while (i >= 0 && i < track.clips.size()) {
        if (!track.clips[i].isGap) return i;
        i += direction;
    }
    return -1;
}

int WindowManager::playlistAdvanceToClip(int trackClipIndex, bool autoplay,
                                         double targetTimelinePos)
{
    if (!m_timeline || !m_videoDecoder) return -1;
    const Timeline &t = m_timeline->timeline();
    if (t.tracks.isEmpty()) return -1;

    // Skip gaps if we landed on one.
    int idx = playlistFindNonGapClip(trackClipIndex, +1);
    if (idx < 0) {
        // No more clips ahead. Loop or pause.
        if (m_loopEnabled) {
            idx = playlistFindNonGapClip(0, +1);
            if (idx < 0) return -1;
        } else {
            if (m_videoDecoder->isPlaying()) {
                m_videoDecoder->pause();
            }
            return -1;
        }
    }

    const Track &track = t.tracks.first();
    const Clip &clip = track.clips[idx];

    // Re-entry guard: open() emits its own currentFrameChanged for
    // the initial seek; without this flag the boundary handler
    // would re-fire mid-swap.
    m_playlistAdvancing = true;

    if (clip.mediaKind == ClipMediaKind::ImageSequence) {
        // Image-seq clip in playlist mode. Tear down whichever
        // backend was active, spin up the cache, then resume
        // playback (autoplay) — the wall-clock driver timer needs
        // an explicit timer.play() since image-seq has no decoder
        // to fire isPlayingChanged.
        m_videoDecoder->close();
        const MediaItem *src = m_project ? m_project->findItem(clip.mediaItemId)
                                          : nullptr;
        if (!src) {
            qWarning("playlistAdvanceToClip — clip's source mediaItem "
                     "missing from pool ('%s')",
                     qPrintable(clip.mediaItemId));
            m_playlistAdvancing = false;
            return -1;
        }
        // Phase 3.H.4 — prewarm-hit fast path. If the next-clip
        // cache was built ~5 s ahead, hand it to startImageSequence
        // so it skips the cold initialize. The CacheThread has been
        // filling the read-ahead window since prewarm started.
        std::unique_ptr<ImageSequenceCache> prewarmed;
        if (m_imageSeqCachePrewarm
            && m_imageSeqCachePrewarmedIndex == idx) {
            prewarmed = std::move(m_imageSeqCachePrewarm);
            m_imageSeqCachePrewarmedIndex = -1;
        }
        const bool prewarmHit = static_cast<bool>(prewarmed);
        startImageSequence(*src, std::move(prewarmed));
        // Boundary auto-resume. Seek the timer to the clip's
        // startTime (or to targetTimelinePos if a user-initiated
        // cross-clip seek passed one through) so the cache picks
        // up at the right source frame. Without this the timer
        // would sit paused at the previous clip's end + the next
        // frame never advances until the user hits Space.
        if (m_timeline) {
            const double timelineSeekTo =
                (targetTimelinePos >= 0.0) ? targetTimelinePos
                                            : clip.startTime;
            m_timeline->timer()->seek(timelineSeekTo);
            // Force the cache's playhead to the right source frame
            // IMMEDIATELY. frameAdvanced fires on integer master-
            // frame crossings, not on seek; without this nudge the
            // cache would still think it's at the previous clip's
            // end-frame for ~1 master-frame tick, so the renderer
            // holds a stale or empty frame at the boundary.
            if (m_imageSeqCache && m_imageSeqCache->fps() > 0.0) {
                const double srcT = clip.sourceIn
                                  + (timelineSeekTo - clip.startTime);
                const int cFrame = std::max(0, static_cast<int>(
                    std::round(srcT * m_imageSeqCache->fps())));
                m_imageSeqCache->updatePlayhead(cFrame, /*force_seek=*/true);
            }
            if (autoplay) {
                m_timeline->timer()->play();
            }
        }
    } else {
        // Video / Audio clip — synchronous re-open on m_videoDecoder.
        // Phase 3.H.4 — if the previous clip was an image sequence,
        // tear down the cache + driver timer first so it doesn't
        // keep pumping wall-clock advance behind the video path.
        if (m_imageSeqActive) {
            stopImageSequence();
        }
        if (!m_videoDecoder->open(clip.mediaPath)) {
            qWarning("playlistAdvanceToClip — failed to open '%s'",
                     qPrintable(clip.mediaPath));
            m_playlistAdvancing = false;
            return -1;
        }
        // Seek to the clip's source-in frame (or directly to the
        // user-target source frame if applyPlaylistSeek passed a
        // targetTimelinePos). Combining the open's initial seek
        // with the user-target seek avoids decoding + presenting
        // an intermediate frame at clip start that would otherwise
        // briefly flash in the viewport before the user-target
        // frame lands.
        const double fps = m_videoDecoder->fps();
        const double timelineSeekTo =
            (targetTimelinePos >= 0.0) ? targetTimelinePos
                                        : clip.startTime;
        if (fps > 0.0) {
            const double srcT = clip.sourceIn
                              + (timelineSeekTo - clip.startTime);
            const int targetFrame = static_cast<int>(std::round(srcT * fps));
            m_videoDecoder->seekToFrame(targetFrame);
        } else {
            m_videoDecoder->seekToFrame(0);
        }
        if (autoplay) {
            m_videoDecoder->play();
        }
        // Reset the timeline timer to the new clip's startTime (or
        // to the user target). Without this a loop wrap from the
        // last clip back to clip 0 leaves m_position at duration
        // (clamped, since playlist mode disables timer-level
        // looping). The first mirror seek fixes it eventually, but
        // until then the QML transport reads stale position.
        if (m_timeline) m_timeline->timer()->seek(timelineSeekTo);
    }

    m_playlistCurrentClipIndex = idx;
    m_playlistAdvancing = false;

    // Push the NEW clip's videoRangeOverride to the live decoder. The
    // sourcePathChanged lambda above ran *inside* m_videoDecoder->open
    // BEFORE m_playlistCurrentClipIndex was updated, so its scope-item
    // lookup resolved to the previous clip (or to the playlist itself
    // on first-clip entry). Re-pushing here lands the correct value
    // for both the boundary advance and the cold-start case. Image-
    // seq sources have no YUV-range concept; skip them. Dual mode
    // doesn't reach this path (playlists are single-flow only).
    if (m_videoDecoder && m_project && clip.mediaKind != ClipMediaKind::ImageSequence) {
        if (const MediaItem *src = m_project->findItem(clip.mediaItemId)) {
            m_videoDecoder->setRangeOverride(
                static_cast<int>(src->videoRangeOverride));
            if (m_scrubDecoder) {
                m_scrubDecoder->setRangeOverride(
                    static_cast<int>(src->videoRangeOverride));
            }
        }
    }

    // Notify the inspector pill (and any other binding on the
    // audio routing scope) that the active clip changed. In playlist
    // mode, scope id == playlistActiveClip()->mediaItemId; without
    // this emit the QML binding never re-evaluates on cross-clip
    // transitions because activeItemId stays on the playlist.
    emit audioRoutingScopeChanged();
    emit playlistCurrentItemIndexChanged();
    return idx;
}

void WindowManager::rebuildPlaylistTimelineOnly()
{
    if (!m_project || !m_timeline) return;
    if (m_timeline->playlistMediaItemId().isEmpty()) return;

    const MediaItem *item = m_project->findItem(
        m_timeline->playlistMediaItemId());
    if (!item || item->type != MediaType::Playlist) return;

    QList<MediaItem>  resolved;
    QList<InOutRange> trims;
    for (const PlaylistEntry &entry : item->playlist.items) {
        const MediaItem *src = m_project->findItem(entry.mediaId);
        if (!src) continue;
        resolved.append(*src);
        trims.append(entry.range);
    }

    // Preserve the active clip index across the rebuild — the new
    // timeline will have the same clip ordering (+gaps), so the
    // index remains valid. We rebuild the timeline data; the
    // active decoder is untouched.
    const int savedActiveIndex = m_playlistCurrentClipIndex;
    m_timeline->loadPlaylist(item->name, resolved, trims,
                             item->playlist.masterFps,
                             item->playlist.defaultGapFrames,
                             item->playlist.canvasWidth,
                             item->playlist.canvasHeight,
                             item->id);
    m_playlistCurrentClipIndex = savedActiveIndex;
}

void WindowManager::prewarmImageSeqIfNeeded()
{
    if (!m_playlistActive || !m_timeline || !m_project) return;
    const Timeline &t = m_timeline->timeline();
    if (t.tracks.isEmpty()) return;
    const Track &track = t.tracks.first();

    // Find next non-gap clip after the current one. If we're at
    // the last clip and loop is on, the "next" is clip 0 — and the
    // boundary is the timeline end, not the next clip's startTime.
    int nextIdx = playlistFindNonGapClip(
        m_playlistCurrentClipIndex + 1, +1);
    double prewarmBoundarySec = 0.0;
    if (nextIdx < 0 || nextIdx >= track.clips.size()) {
        if (!m_loopEnabled) {
            if (m_imageSeqCachePrewarm) cancelImageSeqPrewarm();
            return;
        }
        nextIdx = playlistFindNonGapClip(0, +1);
        if (nextIdx < 0 || nextIdx >= track.clips.size()
            || nextIdx == m_playlistCurrentClipIndex) {
            // No valid wrap target (or wrap to self).
            if (m_imageSeqCachePrewarm) cancelImageSeqPrewarm();
            return;
        }
        prewarmBoundarySec = m_timeline->duration();
    } else {
        prewarmBoundarySec = track.clips[nextIdx].startTime;
    }
    const Clip &nextClip = track.clips[nextIdx];

    if (nextClip.mediaKind != ClipMediaKind::ImageSequence) {
        // Next clip isn't image-seq — drop stale prewarm.
        if (m_imageSeqCachePrewarm) cancelImageSeqPrewarm();
        return;
    }

    // Already prewarming this clip? Nothing to do.
    if (m_imageSeqCachePrewarm
        && m_imageSeqCachePrewarmedIndex == nextIdx) {
        return;
    }

    // Time-to-boundary check.
    const double pos = m_timeline->timer()->position();
    const double leadTime = prewarmBoundarySec - pos;
    if (leadTime > kImageSeqPrewarmLeadSec || leadTime < 0.0) {
        // Too far away (or already past it) — wait.
        return;
    }

    // Targeting a different clip than what's prewarmed → reset.
    if (m_imageSeqCachePrewarm) cancelImageSeqPrewarm();

    const MediaItem *src = m_project->findItem(nextClip.mediaItemId);
    if (!src) return;

    qInfo("prewarmImageSeq: starting prewarm clip=%d path='%s' "
          "(pos=%.2fs boundary=%.2fs lead=%.2fs)",
          nextIdx, qPrintable(nextClip.mediaPath),
          pos, nextClip.startTime, leadTime);

    auto cache = std::make_unique<ImageSequenceCache>(
        src->imageSeq.directory.toStdString(),
        src->imageSeq.pattern.toStdString());
    if (!cache->initialize(src->imageSeq.startFrame,
                           src->imageSeq.endFrame,
                           src->imageSeq.frameRate,
                           PipelineMode::NORMAL,
                           src->imageSeq.layer.toStdString())) {
        qWarning("prewarmImageSeq: cache init failed");
        return;
    }

    // Set playhead to the clip's source-in frame so the CacheThread
    // fills the right window (clips with non-zero sourceIn would
    // otherwise see frames load at the wrong offset).
    const double srcFps = src->imageSeq.frameRate > 0.0
                          ? src->imageSeq.frameRate
                          : m_timeline->timer()->frameRate();
    int firstFrame = 0;
    if (srcFps > 0.0) {
        firstFrame = std::max(0, static_cast<int>(
            std::round(nextClip.sourceIn * srcFps)));
    }
    cache->updatePlayhead(firstFrame, /*force_seek=*/true);

    m_imageSeqCachePrewarm        = std::move(cache);
    m_imageSeqCachePrewarmedIndex = nextIdx;
}

void WindowManager::cancelImageSeqPrewarm()
{
    if (m_imageSeqCachePrewarm) {
        m_imageSeqCachePrewarm->shutdown();
        m_imageSeqCachePrewarm.reset();
    }
    m_imageSeqCachePrewarmedIndex = -1;
}

void WindowManager::startPlaylist(const MediaItem &item)
{
    if (!m_project || !m_timeline) return;

    // Resolve the entries against the pool. Skip any that have
    // gone stale (referenced media removed) — keep their slot in
    // the timeline as a placeholder gap so timeline indices line
    // up with playlist.items[] for inspector / save round-trips.
    QList<MediaItem>   resolved;
    QList<InOutRange>  trims;
    QStringList        missing;
    for (const PlaylistEntry &entry : item.playlist.items) {
        const MediaItem *src = m_project->findItem(entry.mediaId);
        if (!src) {
            missing.append(entry.mediaId);
            continue;
        }
        resolved.append(*src);
        trims.append(entry.range);
    }
    if (!missing.isEmpty()) {
        qWarning("WindowManager::startPlaylist — %d entries missing "
                 "from pool (likely removed): %s",
                 int(missing.size()),
                 qPrintable(missing.join(", ")));
    }

    m_timeline->loadPlaylist(item.name, resolved, trims,
                             item.playlist.masterFps,
                             item.playlist.defaultGapFrames,
                             item.playlist.canvasWidth,
                             item.playlist.canvasHeight,
                             item.id);

    // Phase 3.H.2 — flip the orchestrator state on. From here, the
    // currentFrameChanged / EndOfStream handlers route through the
    // playlist boundary-cross logic instead of single-mode mirror /
    // loop wrap.
    m_playlistActive = true;
    m_playlistCurrentClipIndex = -1;

    // Phase 3.H.4 — playlist mode disables the timer's own looping.
    // The wrap is application-level via playlistAdvanceToClip's loop
    // branch; if the timer auto-wraps m_position, positionChanged
    // fires AFTER the wrap (with pos≈0), so the boundary detection
    // misses the last clip's end and we never advance to clip 0.
    if (m_timeline) m_timeline->timer()->setLooping(false);

    // Load the first non-gap clip via the orchestrator. Empty
    // playlist short-circuits to no-op. Initial load doesn't
    // autoplay — same convention as single-clip loads (open paused;
    // user hits Space).
    if (playlistAdvanceToClip(0, /*autoplay=*/false) < 0) {
        qInfo("WindowManager::startPlaylist — empty playlist '%s'",
              qPrintable(item.name));
    }
}

void WindowManager::stopImageSequence()
{
    if (m_imageSeqDriverTimer) m_imageSeqDriverTimer->stop();

#ifdef QCV_NATIVE_PLAYER
    // Detach the cache pointer from the native renderer first so it
    // doesn't query a cache mid-tear-down.
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        if (auto *r = pw->renderer()) {
            r->setImageSeqCache(nullptr);
        }
    }
#endif

    if (m_imageSeqCache) {
        m_imageSeqCache->shutdown();
        m_imageSeqCache.reset();
        emit imageSeqCacheChanged();
        emit imageSeqMetadataChanged();
    }
    m_lastBufferedAhead    = -1;
    m_lastBufferedBehind   = -1;
    m_lastBufferSize       = -1;
    m_lastFailedFrameCount = -1;
    if (m_imageSeqActive) {
        m_imageSeqActive = false;
        emit imageSeqActiveChanged();
    }
}

// ---------------------------------------------------------------------------
// v2.2.3 — live srt:// receiver mode (Blender-bridge stage 2).
// ---------------------------------------------------------------------------

void WindowManager::startLiveStream(const MediaItem &item)
{
    qInfo("startLiveStream: '%s' (%s)",
          qPrintable(item.name), qPrintable(item.path));

    // closeActiveMedia() already ran in the dispatch path; this is
    // belt-and-braces for any future direct caller.
    stopLiveStream();

    if (!m_videoDecoder) {
        qWarning("startLiveStream: no sink VideoDecoder — aborting");
        return;
    }

    m_liveDecoder = std::make_unique<LiveStreamDecoder>();
    m_liveDecoder->setSink(m_videoDecoder);

    // Per-frame renderer nudge. Metal's present loop free-runs and
    // fetches every vsync, but D3D11 renders on demand — without this
    // the Windows viewport would only repaint on unrelated UI events.
    // Same cross-thread contract as the dual sources' frame callback.
    if (auto *r = fetchActiveRenderer(m_playerWindow.data())) {
        m_liveDecoder->setFrameCallback([r] { r->requestUpdate(); });
    }

    if (!m_liveDecoder->open(item.path)) {
        m_liveDecoder.reset();
        setViewportNotice(QStringLiteral("Could not open stream\n%1")
                              .arg(item.path));
        return;
    }

    m_liveActive = true;
    emit liveActiveChanged();
    emit liveDecoderChanged();
}

void WindowManager::stopLiveStream()
{
    if (!m_liveDecoder) return;

    // close() joins the receive/decode worker, so after this line
    // nothing publishes into m_videoDecoder's slot. The renderer is
    // left alone here — closeActiveMedia's clearSourceAState() (or
    // the next source's first publish) replaces the last frame.
    m_liveDecoder->close();
    m_liveDecoder.reset();
    emit liveDecoderChanged();

    if (m_liveActive) {
        m_liveActive = false;
        emit liveActiveChanged();
    }
}

// ---------------------------------------------------------------------------
// Unified transport routing — image-seq vs video.
// ---------------------------------------------------------------------------

void WindowManager::togglePlayback()
{
    // Capture user intent BEFORE flipping decoder state — the EOS
    // loop-wrap handler reads this to decide whether to auto-resume
    // after seek(0) when looping a short clip. Without this, a
    // user-pressed pause races with the EOS handler and never
    // sticks (see m_userWantsPlayback comment in the header).
    const bool wasPlaying =
        m_dualController ? m_dualController->isPlaying() :
        m_imageSeqActive ? (m_timeline && m_timeline->timer()->isPlaying()) :
        m_audioActive    ? (m_timeline && m_timeline->timer()->isPlaying()) :
        (m_videoDecoder && m_videoDecoder->isPlaying());
    m_userWantsPlayback = !wasPlaying;

    // Phase 7.7 — in dual mode, transport drives the dual controller.
    if (m_dualController) {
        m_dualController->togglePlayback();
        return;
    }
    // B follows whatever A is doing — including in hybrid mode
    // (image-seq A + video B). The image-seq path runs the timer
    // for A; we also have to tell B's decoder to play/pause.
    const bool bLoaded = m_videoDecoderB
                      && !m_videoDecoderB->sourcePath().isEmpty();
    if (m_imageSeqActive) {
        if (m_timeline) m_timeline->timer()->togglePlayPause();
        if (bLoaded) m_videoDecoderB->togglePlayback();
        return;
    }
    if (m_audioActive) {
        // Audio-only: drive the timer's wallclock state and the
        // AudioPlayer side-by-side. (VideoDecoder is parked in
        // Errored state — its togglePlayback can't reliably drive
        // either side here.)
        if (m_timeline) m_timeline->timer()->togglePlayPause();
        if (m_audio) {
            if (m_audio->isPlaying()) m_audio->pause();
            else                       m_audio->play();
        }
        return;
    }
    if (m_videoDecoder) m_videoDecoder->togglePlayback();
    if (bLoaded) m_videoDecoderB->togglePlayback();
}

void WindowManager::play()
{
    m_userWantsPlayback = true;
    if (m_dualController) {
        m_dualController->play();
        return;
    }
    const bool bLoaded = m_videoDecoderB
                      && !m_videoDecoderB->sourcePath().isEmpty();
    if (m_imageSeqActive) {
        if (m_timeline) m_timeline->timer()->play();
        if (bLoaded) m_videoDecoderB->play();
        return;
    }
    if (m_audioActive) {
        if (m_timeline) m_timeline->timer()->play();
        if (m_audio) m_audio->play();
        return;
    }
    if (m_videoDecoder) m_videoDecoder->play();
    if (bLoaded) m_videoDecoderB->play();
}

void WindowManager::pause()
{
    m_userWantsPlayback = false;
    if (m_dualController) {
        m_dualController->pause();
        return;
    }
    const bool bLoaded = m_videoDecoderB
                      && !m_videoDecoderB->sourcePath().isEmpty();
    if (m_imageSeqActive) {
        if (m_timeline) m_timeline->timer()->pause();
        if (bLoaded) m_videoDecoderB->pause();
        return;
    }
    if (m_audioActive) {
        if (m_timeline) m_timeline->timer()->pause();
        if (m_audio) m_audio->pause();
        return;
    }
    if (m_videoDecoder) m_videoDecoder->pause();
    if (bLoaded) m_videoDecoderB->pause();
}

void WindowManager::scrubToTimelineFrame(int frame)
{
    if (!m_playlistActive || !m_timeline) return;
    const Timeline &t = m_timeline->timeline();
    if (t.tracks.isEmpty()) return;
    const double masterFps = m_timeline->timer()->frameRate();
    if (masterFps <= 0.0) return;
    const double pos = static_cast<double>(frame) / masterFps;

    const Track &track = t.tracks.first();
    int target = -1;
    for (int i = 0; i < track.clips.size(); ++i) {
        const Clip &c = track.clips[i];
        if (pos >= c.startTime && pos < c.startTime + c.duration) {
            target = i;
            break;
        }
    }
    if (target < 0) {
        target = playlistFindNonGapClip(track.clips.size() - 1, -1);
        if (target < 0) return;
    }
    if (track.clips[target].isGap) {
        const int next = playlistFindNonGapClip(target, +1);
        target = (next >= 0)
               ? next
               : playlistFindNonGapClip(target, -1);
        if (target < 0) return;
    }

    // Cross-clip scrub: swap active decoder (paused) before
    // requesting the scrub frame. The swap pays the open cost
    // (~50–100 ms) which is the one unavoidable stall on cross-
    // clip drag — Stage 2.b prewarm shaves most of this off.
    //
    // Pass `pos` so the freshly-opened decoder's initial seek lands
    // at the user-target source frame instead of clip.sourceIn —
    // otherwise the videoDecoder publishes the new clip's first
    // frame, the renderer paints it, and only then does the
    // scrubDecoder's preview frame arrive (read on screen as a
    // frame-1 flash on mouse-down).
    if (target != m_playlistCurrentClipIndex) {
        playlistAdvanceToClip(target, /*autoplay=*/false, /*targetTimelinePos=*/pos);
    }

    // Same-clip (or post-swap) scrub.
    const Clip *now = playlistActiveClip();
    if (!now) return;

    // Phase 3.H.4 — image-seq clip: the cache is the scrub
    // engine. Seeking the timer fires the frameAdvanced hook
    // (which translates → cache playhead via clip.sourceIn).
    if (now->mediaKind == ClipMediaKind::ImageSequence) {
        m_timeline->timer()->seek(pos);
        return;
    }

    // Video clip: ScrubDecoder gets a fast, single-frame fetch in
    // the active clip's source coordinates.
    if (!m_videoDecoder) return;
    const double clipFps = m_videoDecoder->fps();
    if (clipFps <= 0.0) return;
    const double srcT = now->sourceIn + (pos - now->startTime);
    const int srcFrame = static_cast<int>(std::round(srcT * clipFps));

    if (m_scrubDecoder) {
        m_scrubDecoder->requestFrame(srcFrame);
    }
    // Reflect the requested timeline position in the timer so QML
    // scrubbers and timecode readouts see immediate feedback.
    m_timeline->timer()->seek(pos);
}

void WindowManager::beginScrubAudio(double timelineSeconds)
{
    m_scrubVelClock.start();
    m_scrubVelLastMs  = 0;
    m_scrubVelLastSec = timelineSeconds;
    m_scrubVelEma     = 0.0;
    m_scrubAudioActive = true;

    // Speed 0 at press: the engine's hold detection keeps it silent
    // until the drag actually moves.
    if (m_dualController) {
        m_dualController->beginShuttle(0.0);
        return;
    }
    if (m_playlistActive && m_audio) {
        const Clip *c = clipAtTimelineSec(timelineSeconds);
        m_audio->beginShuttle(
            c ? c->mediaPath : QString(),
            c ? c->sourceIn + (timelineSeconds - c->startTime) : 0.0,
            0.0, m_audio->routingMode());
    } else if (m_audio && m_audio->hasAudio() && m_videoDecoder) {
        m_audio->beginShuttle(m_videoDecoder->sourcePath(),
                              timelineSeconds, 0.0,
                              m_audio->routingMode());
    } else {
        m_scrubAudioActive = false;   // image-seq / no audio source
    }
}

void WindowManager::scrubAudioMove(double timelineSeconds)
{
    if (!m_scrubAudioActive) return;

    // Drag-velocity estimate: signed source-seconds per wall second,
    // EMA-smoothed with a dt-scaled alpha (tau ≈ 40 ms) so mouse
    // event jitter doesn't warble the grain pitch. Tau was 80 ms
    // when jitter above 1x was audible; with the engine's 1x pitch
    // cap only sub-1x drags hear the estimate, so a faster tracker
    // wins. The engine's hold detection covers the stationary case
    // (no move events arrive here to decay the estimate).
    const qint64 now = m_scrubVelClock.elapsed();
    const double dt  = static_cast<double>(now - m_scrubVelLastMs) * 1e-3;
    if (dt > 0.0005) {
        double v = (timelineSeconds - m_scrubVelLastSec) / dt;
        v = std::clamp(v, -32.0, 32.0);
        const double alpha = 1.0 - std::exp(-dt / 0.040);
        m_scrubVelEma += alpha * (v - m_scrubVelEma);
        m_scrubVelLastMs  = now;
        m_scrubVelLastSec = timelineSeconds;
    }
    const double speed = m_scrubVelEma;

    if (m_dualController) {
        const double fps = m_dualController->fps();
        if (fps > 0.0) {
            m_dualController->shuttleTick(
                static_cast<int>(std::lround(timelineSeconds * fps)),
                speed);
        }
        return;
    }
    if (!m_audio || !m_audio->shuttleActive()) return;
    if (m_playlistActive) {
        const Clip *c = clipAtTimelineSec(timelineSeconds);
        m_audio->shuttleTarget(
            c ? c->mediaPath : QString(),
            c ? c->sourceIn + (timelineSeconds - c->startTime) : 0.0,
            speed);
    } else if (m_videoDecoder) {
        m_audio->shuttleTarget(m_videoDecoder->sourcePath(),
                               timelineSeconds, speed);
    }
}

void WindowManager::endScrubAudio()
{
    if (!m_scrubAudioActive) return;
    m_scrubAudioActive = false;
    if (m_dualController) m_dualController->endShuttle();
    if (m_audio && m_audio->shuttleActive()) m_audio->endShuttle();
}

void WindowManager::beginEditScrub()
{
    // Edits are a paused activity — stop playback so the streaming
    // decoder doesn't overwrite the scrub-preview frame. pause() is a
    // safe no-op when already paused.
    pause();

    if (m_dualController) {
        // Capture the pre-edit playhead BEFORE beginScrub so endScrub
        // can warm-seek both sides back to it (restores the displayed
        // frame without the edit having moved the CTI).
        m_editScrubRestoreMaster = m_dualController->currentFrame();
        m_dualController->beginScrub();
        m_editScrubActive = true;
        return;
    }
    if (m_playlistActive && m_videoDecoder) {
        // The shared ScrubDecoder is already open on the active clip;
        // remember the displayed source frame to repaint on release.
        m_editScrubRestoreFrame = m_videoDecoder->currentFrame();
        m_editScrubActive = true;
    }
}

void WindowManager::previewEditScrubFrame(const QString &trackId,
                                          const QString &clipId,
                                          int sourceFrame)
{
    if (!m_editScrubActive || sourceFrame < 0) return;

    if (m_dualController) {
        const char side = (trackId == QLatin1String("B")) ? 'B' : 'A';
        m_dualController->requestScrubFrameForSide(side, sourceFrame);
        return;
    }
    if (m_playlistActive && m_scrubDecoder) {
        // Only drive feedback when the edited clip is the one the
        // single, shared ScrubDecoder is currently open on — otherwise
        // we'd decode a different clip's media. Editing a non-active
        // clip simply shows no preview (the prior behavior).
        const Clip *active = playlistActiveClip();
        if (!active || active->id != clipId) return;
        m_scrubDecoder->requestFrame(sourceFrame);
    }
}

void WindowManager::endEditScrub()
{
    if (!m_editScrubActive) return;
    m_editScrubActive = false;

    if (m_dualController) {
        // Wakes the streaming decoders, clears the per-side overrides,
        // and warm-seeks both sides + the master timer back to the
        // pre-edit playhead.
        m_dualController->endScrub(m_editScrubRestoreMaster);
        return;
    }
    if (m_playlistActive && m_scrubDecoder && m_editScrubRestoreFrame >= 0) {
        // Latest-wins repaint of the pre-edit frame; the timer never
        // moved during the edit.
        m_scrubDecoder->requestFrame(m_editScrubRestoreFrame);
    }
    m_editScrubRestoreFrame = -1;
}

double WindowManager::activeClockFps() const
{
    // Mirrors currentFrameUnified()'s fps basis so the legacy
    // frame-based seekToFrame() round-trips losslessly through
    // seekToTime(). Each mode owns its own clock — there is no
    // single global fps.
    if (m_dualController) {
        const double f = m_dualController->fps();
        return f > 0.0 ? f : 24.0;
    }
    if ((m_imageSeqActive || m_audioActive || m_playlistActive)
        && m_timeline) {
        const double f = m_timeline->timer()->frameRate();
        return f > 0.0 ? f : 24.0;
    }
    if (m_videoDecoder && !m_videoDecoder->sourcePath().isEmpty()) {
        const double f = m_videoDecoder->fps();
        if (f > 0.0) return f;
    }
    if (m_timeline) {
        const double f = m_timeline->timer()->frameRate();
        if (f > 0.0) return f;
    }
    return 24.0;
}

void WindowManager::seekToFrame(int frame)
{
    const double fps = activeClockFps();
    seekToTime(fps > 0.0 ? static_cast<double>(frame) / fps : 0.0);
}

void WindowManager::seekToFrameAndResume(int frame, bool resume)
{
    const double fps = activeClockFps();
    seekToTimeAndResume(
        fps > 0.0 ? static_cast<double>(frame) / fps : 0.0, resume);
}

void WindowManager::seekToTimeAndResume(double seconds, bool resume)
{
    // Pause the decoder NOW so it doesn't keep publishing frames at
    // its current (pre-seek) position during the 50 ms playlist
    // debounce window. applyPlaylistSeek will resume after committing
    // the new seek target.
    if (resume && m_playlistActive && m_videoDecoder
        && m_videoDecoder->isPlaying()) {
        m_videoDecoder->pause();
    }
    m_resumePlayAfterPlaylistSeek = resume;
    seekToTime(seconds);
}

void WindowManager::seekToTime(double seconds)
{
    // Canonical seek. `seconds` is a TIMELINE position. Each branch
    // converts to its own decoder's frame space using THAT decoder's
    // fps — never a global one.
    if (seconds < 0.0) seconds = 0.0;

    if (m_dualController) {
        const double fps = m_dualController->fps();
        if (fps > 0.0) {
            m_dualController->seekToFrame(
                static_cast<int>(std::lround(seconds * fps)));
        }
        return;
    }

    const bool bLoaded = m_videoDecoderB
                      && !m_videoDecoderB->sourcePath().isEmpty();

    if (m_imageSeqActive) {
        // The timer is the image-seq scrub engine; it owns position
        // in seconds. The frameAdvanced / positionChanged mirror
        // routes to the cache using the cache's own fps.
        if (m_timeline) m_timeline->timer()->seek(seconds);
        if (bLoaded) {
            const double fpsB = m_videoDecoderB->fps();
            if (fpsB > 0.0) {
                m_videoDecoderB->seekToFrame(
                    static_cast<int>(std::lround(seconds * fpsB)));
            }
        }
        return;
    }

    if (m_audioActive) {
        // Audio-only seek: drive the timer so the playhead snaps,
        // then nudge AudioPlayer to the new position so play-after-
        // scrub is in sync. Both speak seconds.
        if (m_timeline) m_timeline->timer()->seek(seconds);
        if (m_audio) m_audio->seek(seconds);
        return;
    }

    // Phase 3.H.2 — playlist mode: `seconds` is a TIMELINE position.
    // Resolve the target clip, swap if needed, seek the active
    // decoder to the right SOURCE frame (applyPlaylistSeek does the
    // per-clip-fps conversion). The timer mirrors timeline-time on
    // the next currentFrameChanged.
    //
    // Coalesce via 50 ms single-shot debounce. A rapid burst of
    // user clicks on the timeline (the original repro for the
    // teardown cascade crash) collapses to one playlistAdvanceToClip
    // for the latest target instead of stacking 6-7 full VideoDecoder
    // + audio teardown+reopen cycles into the cleanup queue. The lazy
    // initialization keeps the timer cost out of single-mode and
    // dual-mode paths that don't need it.
    if (m_playlistActive && m_timeline) {
        if (!m_playlistSeekDebounce) {
            m_playlistSeekDebounce = new QTimer(this);
            m_playlistSeekDebounce->setSingleShot(true);
            m_playlistSeekDebounce->setInterval(50);
            connect(m_playlistSeekDebounce, &QTimer::timeout,
                    this, [this] {
                if (m_pendingPlaylistSeekSec >= 0.0) {
                    const double s = m_pendingPlaylistSeekSec;
                    m_pendingPlaylistSeekSec = -1.0;
                    applyPlaylistSeek(s);
                }
            });
        }
        m_pendingPlaylistSeekSec = seconds;
        m_playlistSeekDebounce->start();   // restarts if already pending
        return;
    }

    // Single video. B (legacy non-dual-controller second decoder)
    // converts with ITS own fps — fixes a latent bug where B used to
    // be seeked with A's frame number.
    if (m_videoDecoder) {
        const double fps = m_videoDecoder->fps();
        if (fps > 0.0) {
            m_videoDecoder->seekToFrame(
                static_cast<int>(std::lround(seconds * fps)));
        }
    }
    if (bLoaded) {
        const double fpsB = m_videoDecoderB->fps();
        if (fpsB > 0.0) {
            m_videoDecoderB->seekToFrame(
                static_cast<int>(std::lround(seconds * fpsB)));
        }
    }
}

void WindowManager::applyPlaylistSeek(double pos)
{
    // Debounced playlist seek body — extracted from seekToTime so
    // the same logic runs whether the user single-clicked (timer
    // fires once) or chained 7 clicks in a burst (timer fires once
    // for the latest target). Runs only in playlist mode; bails out
    // gracefully if the playlist was torn down between debounce
    // start and timer fire. `pos` is a TIMELINE position in seconds.
    if (!m_playlistActive || !m_timeline) return;
    const Timeline &t = m_timeline->timeline();
    if (t.tracks.isEmpty()) return;

    const Track &track = t.tracks.first();
    int target = -1;
    for (int i = 0; i < track.clips.size(); ++i) {
        const Clip &c = track.clips[i];
        if (pos >= c.startTime
            && pos <  c.startTime + c.duration) {
            target = i;
            break;
        }
    }
    if (target < 0) {
        // Past the last clip — clamp to the last non-gap clip.
        target = playlistFindNonGapClip(track.clips.size() - 1, -1);
        if (target < 0) return;
    }
    // Land on the nearest non-gap clip if the playhead is inside
    // a gap.
    if (track.clips[target].isGap) {
        const int next = playlistFindNonGapClip(target, +1);
        target = (next >= 0)
               ? next
               : playlistFindNonGapClip(target, -1);
        if (target < 0) return;
    }

    const bool wasPlaying = m_timeline->timer()->isPlaying();
    if (target != m_playlistCurrentClipIndex) {
        // Pass `pos` so playlistAdvanceToClip seeks the freshly-
        // opened decoder directly to the user-target source frame
        // instead of clip.sourceIn first. Without this the decoder
        // decodes + presents a clip-start frame, then re-seeks to
        // the user target — visible as a frame-1 flash in the
        // viewport during cross-clip seeks.
        playlistAdvanceToClip(target, wasPlaying, pos);
    }
    // Seek the (possibly newly-loaded) decoder to the right source
    // frame.
    const Clip *now = playlistActiveClip();
    if (now && m_videoDecoder) {
        const double clipFps = m_videoDecoder->fps();
        if (clipFps > 0.0) {
            const double srcT = now->sourceIn
                              + (pos - now->startTime);
            const int srcFrame = static_cast<int>(
                std::round(srcT * clipFps));
            // Arm the mirror gate: skip currentFrameChanged events
            // until the decoder lands within tolerance of srcFrame.
            // Otherwise intermediate keyframe-rounded frames during
            // the seek translate to clip.startTime via the mirror's
            // (sourceTime - sourceIn) math, briefly snapping the
            // playhead to the new clip's beginning.
            m_playlistSeekTargetFrame = srcFrame;
            m_videoDecoder->seekToFrame(srcFrame);
        }
        // Reflect the requested timeline position in the timer so
        // QML scrubbers see immediate feedback even before the
        // decoder publishes the new frame.
        m_timeline->timer()->seek(pos);
    }
    // Deferred resume: if QML's release handler asked to play after
    // the seek (via seekToFrameAndResume), do it now — the decoder
    // has its target queued and performSeek will publish the user-
    // target frame as the FIRST visible frame at the new position.
    if (m_resumePlayAfterPlaylistSeek) {
        m_resumePlayAfterPlaylistSeek = false;
        if (m_videoDecoder) m_videoDecoder->play();
    }
}

void WindowManager::closeMedia()
{
    closeActiveMedia();
}

void WindowManager::stepFrames(int delta)
{
    if (delta == 0) return;
    // Pause first — frame-stepping while playing is ambiguous (the
    // wall clock would race the step). Old app's StepFrame:
    // panel_timeline.cpp ~line 671.
    if (isPlayingUnified()) {
        if (m_dualController) {
            m_dualController->pause();
        } else if (m_imageSeqActive) {
            if (m_timeline) m_timeline->timer()->pause();
        } else if (m_audioActive) {
            if (m_timeline) m_timeline->timer()->pause();
            if (m_audio)    m_audio->pause();
        } else if (m_videoDecoder) {
            m_videoDecoder->togglePlayback();
        }
    }
    const int target = currentFrameUnified() + delta;
    const int total  = frameCountUnified();
    int clamped = target;
    if (total > 0) {
        if (clamped < 0) clamped = 0;
        if (clamped > total - 1) clamped = total - 1;
    }
    seekToFrame(clamped);
}

void WindowManager::seekToStart()
{
    seekToFrame(0);
}

void WindowManager::seekToEnd()
{
    const int total = frameCountUnified();
    if (total > 0) seekToFrame(total - 1);
}

// Playlist clip navigation. Two-part fix vs. the previous
// TimelineFlattener::nextClipStart / prevClipStart path:
//
// 1) Index-based lookup. m_playlistCurrentClipIndex (kept in sync by
//    applyPlaylistSeek + playlistAdvanceToClip) is authoritative —
//    avoids the strict `>` / `<` comparisons against a quantized
//    currentFrameUnified() position.
//
// 2) Boundary-safe seek frame. The seek target frame is computed so
//    its round-trip through applyPlaylistSeek (pos = frame/masterFps)
//    lands *inside* the destination clip. For Next, that means
//    ceil(targetSec * fps) — round() can produce a frame whose
//    pos sits one master-frame BEFORE the clip's startTime when the
//    startTime isn't an exact multiple of 1/masterFps (e.g.,
//    22.02 s at 24 fps quantizes to 22.0 s, which still resolves
//    to the previous clip). The result was: Next would silently
//    snap the decoder to the last frame of the current clip on
//    every press, never crossing the boundary.
//
//    For Prev's "snap to current clip start" leg, we apply the same
//    ceil bias for the same reason — without it, seeking to the
//    current clip's startTime can land in the previous clip.
//
// Semantics preserved:
//   next: jump to the next non-gap clip's startTime.
//   prev: mid-clip jumps to the current clip's startTime first;
//         pressed again at the current clip's startTime, goes to the
//         prior non-gap clip's startTime.
namespace {
// Bias targetSec so static_cast<int>() yields the smallest frame whose
// frame/fps >= targetSec — i.e., ceil(targetSec * fps). Lands inside the
// destination clip rather than one master-frame short of its start.
int frameAtOrAfter(double targetSec, double fps)
{
    return static_cast<int>(std::ceil(targetSec * fps));
}
}

void WindowManager::seekToPrevClipStart()
{
    if (!m_timeline) return;
    if (m_timeline->sourceModeInt() != 1) return;
    const double fps = m_timeline->frameRate();
    if (fps <= 0.0) return;
    const Timeline &tl = m_timeline->timeline();
    if (tl.tracks.isEmpty()) return;
    const Track &track = tl.tracks.first();
    const int curIdx = m_playlistCurrentClipIndex;
    if (curIdx < 0 || curIdx >= track.clips.size()) return;

    const double curClipStart = track.clips[curIdx].startTime;
    const double curPosSec    = m_timeline->timer()
                                ? m_timeline->timer()->position()
                                : 0.0;
    // "At start" tolerance must absorb frameAtOrAfter's ceil overshoot —
    // a prior Prev that snapped to curClipStart can land up to one
    // master-frame past it (e.g., target 22.02 s at 24 fps → frame 529
    // → pos 22.0417, ~+1 master-frame). A 0.5-frame threshold would
    // treat that landing as "mid-clip" and bounce back to curClipStart
    // forever. Using a strict `>` against one full master-frame past
    // curClipStart means: any landing within one master-frame of the
    // start is considered "at start" → Prev again advances to the
    // previous clip.
    const double oneFrame = 1.0 / fps;
    double targetSec = -1.0;
    if (curPosSec > curClipStart + oneFrame) {
        targetSec = curClipStart;
    } else {
        const int prevIdx = playlistFindNonGapClip(curIdx - 1, -1);
        if (prevIdx < 0) return;
        targetSec = track.clips[prevIdx].startTime;
    }
    seekToFrame(frameAtOrAfter(targetSec, fps));
}

void WindowManager::seekToNextClipStart()
{
    if (!m_timeline) return;
    if (m_timeline->sourceModeInt() != 1) return;
    const double fps = m_timeline->frameRate();
    if (fps <= 0.0) return;
    const Timeline &tl = m_timeline->timeline();
    if (tl.tracks.isEmpty()) return;
    const Track &track = tl.tracks.first();
    const int curIdx = m_playlistCurrentClipIndex;
    if (curIdx < 0) return;
    const int nextIdx = playlistFindNonGapClip(curIdx + 1, +1);
    if (nextIdx < 0 || nextIdx >= track.clips.size()) return;
    const double nextSec = track.clips[nextIdx].startTime;
    seekToFrame(frameAtOrAfter(nextSec, fps));
}

int WindowManager::playlistCurrentItemIndex() const
{
    if (!m_timeline || m_timeline->sourceModeInt() != 1) return -1;
    const int curIdx = m_playlistCurrentClipIndex;
    if (curIdx < 0) return -1;
    const Timeline &tl = m_timeline->timeline();
    if (tl.tracks.isEmpty()) return -1;
    const Track &track = tl.tracks.first();
    if (curIdx >= track.clips.size() || track.clips[curIdx].isGap) return -1;
    // The playlist `items` list carries no gap entries, so the item
    // index is the count of non-gap clips preceding the current clip.
    int itemIdx = 0;
    for (int i = 0; i < curIdx; ++i) {
        if (!track.clips[i].isGap) ++itemIdx;
    }
    return itemIdx;
}

void WindowManager::seekToPlaylistItem(int itemIndex)
{
    if (!m_timeline || m_timeline->sourceModeInt() != 1) return;
    if (itemIndex < 0) return;
    const double fps = m_timeline->frameRate();
    if (fps <= 0.0) return;
    const Timeline &tl = m_timeline->timeline();
    if (tl.tracks.isEmpty()) return;
    const Track &track = tl.tracks.first();
    // Map the gap-free item index to the matching non-gap track clip,
    // then seek to its start (same boundary-safe frame math the
    // prev/next-clip buttons use). seekToFrame preserves play/pause.
    int seen = -1;
    for (int i = 0; i < track.clips.size(); ++i) {
        if (track.clips[i].isGap) continue;
        if (++seen == itemIndex) {
            seekToFrame(frameAtOrAfter(track.clips[i].startTime, fps));
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Fast-seek (FF/RW) — direct port of old app's TimelinePlaybackController
// ::StartRewind / StartFastForward / UpdateFastSeek (constants from
// timeline_playback_controller.h:384-386 — 2x init, 32x cap, doubles
// per second). Single QTimer drives both directions; the sign of
// m_fastSeekDir picks behavior.
// ---------------------------------------------------------------------------

namespace {
constexpr double kFastSeekInitialSpeed = 2.0;
constexpr double kFastSeekMaxSpeed     = 32.0;
constexpr double kFastSeekAcceleration = 2.0;
constexpr int    kFastSeekTickMs       = 33;
}

void WindowManager::startFastSeek(int direction)
{
    if (direction == 0) return;
    direction = (direction > 0) ? 1 : -1;

    // If already fast-seeking in the same direction, no-op (button
    // re-press / repeat). If opposite direction, restart from
    // initial speed so the user gets a snappy reverse.
    if (m_fastSeekTimer && m_fastSeekDir == direction) return;

    // Shuttle and review speed are mutually exclusive transports —
    // entering the gesture snaps playback rate back to 1x so the
    // release commit resumes at normal speed.
    if (m_reviewSpeed != 1.0) setReviewSpeed(1.0);

    // Pause normal playback first — match old app pattern.
    if (isPlayingUnified()) {
        if (m_dualController) {
            m_dualController->pause();
        } else if (m_imageSeqActive) {
            if (m_timeline) m_timeline->timer()->pause();
        } else if (m_audioActive) {
            if (m_timeline) m_timeline->timer()->pause();
            if (m_audio)    m_audio->pause();
        } else if (m_videoDecoder) {
            m_videoDecoder->togglePlayback();
        }
    }

    m_fastSeekDir   = direction;
    m_fastSeekSpeed = kFastSeekInitialSpeed;
    // Mark gesture in-flight. Used to gate the VideoDecoder mirror
    // so an in-flight publish from before the gesture can't drag
    // the timer back to a stale source position.
    m_fastSeekActive = true;
    m_fastSeekClock.start();
    m_fastSeekStartMs = 0;
    m_fastSeekLastMs  = 0;

    // Anchor position in seconds. Dual mode reads from the dual
    // controller's master clock; playlist + image-seq go through
    // the timeline timer (master fps); single video computes from
    // current frame + source fps.
    if (m_dualController) {
        const double fps = (m_dualController->fps() > 0.0)
                           ? m_dualController->fps() : 30.0;
        m_fastSeekPositionSec =
            static_cast<double>(std::max(0, m_dualController->currentFrame()))
            / fps;
    } else if (m_playlistActive && m_timeline) {
        m_fastSeekPositionSec = m_timeline->timer()->position();
    } else if (m_imageSeqActive && m_timeline) {
        m_fastSeekPositionSec = m_timeline->timer()->position();
    } else if (m_audioActive && m_timeline) {
        m_fastSeekPositionSec = m_timeline->timer()->position();
    } else if (m_videoDecoder) {
        const double fps = (m_videoDecoder->fps() > 0.0)
                           ? m_videoDecoder->fps() : 30.0;
        m_fastSeekPositionSec =
            static_cast<double>(std::max(0, m_videoDecoder->currentFrame())) /
            fps;
    } else {
        m_fastSeekPositionSec = 0.0;
    }

    // Shuttle audio: skim-style grains following the gesture position
    // (ShuttleAudioEngine; natural pitch above 1x, varispeed below).
    // Replaces the old audio-only per-tick m_audio->seek storm (a
    // destructive ring+codec flush every 33 ms). Routing per mode:
    //   dual     → controller translates master→per-side source and
    //              drives the mixer's two engines
    //   playlist → timeline pos maps to (clip file, source sec);
    //              gaps ride as an empty path (silence, cursor keeps
    //              tracking)
    //   single video / audio-only → the decoder's sourcePath
    const double initialSigned =
        static_cast<double>(direction) * kFastSeekInitialSpeed;
    if (m_dualController) {
        m_dualController->beginShuttle(initialSigned);
    } else if (m_playlistActive && m_audio) {
        const Clip *c = clipAtTimelineSec(m_fastSeekPositionSec);
        m_audio->beginShuttle(
            c ? c->mediaPath : QString(),
            c ? c->sourceIn + (m_fastSeekPositionSec - c->startTime) : 0.0,
            initialSigned, m_audio->routingMode());
    } else if (m_audio && m_audio->hasAudio() && m_videoDecoder) {
        m_audio->beginShuttle(m_videoDecoder->sourcePath(),
                              m_fastSeekPositionSec,
                              initialSigned,
                              m_audio->routingMode());
    }

    if (!m_fastSeekTimer) {
        m_fastSeekTimer = new QTimer(this);
        m_fastSeekTimer->setTimerType(Qt::PreciseTimer);
        connect(m_fastSeekTimer, &QTimer::timeout, this, [this] {
            if (m_fastSeekDir == 0) return;
            const qint64 nowMs = m_fastSeekClock.elapsed();
            const double delta =
                static_cast<double>(nowMs - m_fastSeekLastMs) * 1e-3;
            const double elapsed =
                static_cast<double>(nowMs - m_fastSeekStartMs) * 1e-3;
            m_fastSeekLastMs = nowMs;

            // Speed ramp — geometric, doubles every second.
            m_fastSeekSpeed = kFastSeekInitialSpeed *
                              std::pow(kFastSeekAcceleration, elapsed);
            if (m_fastSeekSpeed > kFastSeekMaxSpeed)
                m_fastSeekSpeed = kFastSeekMaxSpeed;

            const double signedSpeed =
                m_fastSeekSpeed * static_cast<double>(m_fastSeekDir);
            m_fastSeekPositionSec += delta * signedSpeed;

            // Resolve target frame, clamp, seek. We use seekToFrame
            // (rather than a position-seconds path) so video,
            // image-seq, and dual all go through the same chokepoint.
            double fps = 0.0;
            int total = frameCountUnified();
            if (m_dualController) {
                fps = m_dualController->fps();
            } else if (m_playlistActive && m_timeline) {
                // Phase 3.H.4 — playlist mode uses the master fps
                // for fast-seek so position math runs against the
                // whole timeline, not the active clip's source.
                fps = m_timeline->timer()->frameRate();
            } else if (m_imageSeqActive && m_timeline) {
                fps = m_timeline->timer()->frameRate();
            } else if (m_audioActive && m_timeline) {
                fps = m_timeline->timer()->frameRate();
            } else if (m_videoDecoder) {
                fps = m_videoDecoder->fps();
            }
            if (fps <= 0.0 || total <= 0) return;

            // Clamp position to media bounds; if the user hits an
            // edge we hold there until they release.
            const double maxSec =
                static_cast<double>(total - 1) / fps;
            if (m_fastSeekPositionSec < 0.0) m_fastSeekPositionSec = 0.0;
            if (m_fastSeekPositionSec > maxSec)
                m_fastSeekPositionSec = maxSec;

            const int target = static_cast<int>(
                std::lround(m_fastSeekPositionSec * fps));

            // Timer-leads-decoder model. The master clock advances
            // every tick regardless of decode progress; decoders
            // chase async. Same dynamic dual already had via
            // DualPlaybackTimer. Works for any codec — intra at 8K
            // can't keep up either, but the timeline cursor stays
            // smooth and the user sees frames whenever the decoder
            // happens to publish.
            //
            // Mode-specific routing: dual already does timer-then-
            // sources internally; image-seq + audio drive the
            // wallclock pump directly; single video goes timer
            // first, then queues an async decoder seek; playlist
            // skips per-tick decoder seeks (cross-clip swap is
            // sync + expensive — we defer that single hit to the
            // gesture's release).
            if (m_dualController) {
                m_dualController->seekToFrame(target);
            } else if (m_imageSeqActive) {
                if (m_timeline) m_timeline->timer()->seek(m_fastSeekPositionSec);
            } else if (m_audioActive) {
                if (m_timeline) m_timeline->timer()->seek(m_fastSeekPositionSec);
                // Audio follows via the shuttle grain engine below —
                // the old per-tick m_audio->seek here was a
                // destructive ring+codec flush storm.
            } else if (m_playlistActive) {
                if (m_timeline) m_timeline->timer()->seek(m_fastSeekPositionSec);
            } else if (m_videoDecoder) {
                if (m_timeline) m_timeline->timer()->seek(m_fastSeekPositionSec);
                m_videoDecoder->seekToFrame(target);
            }

            // Feed the integrated position + signed ramp speed to the
            // shuttle audio (no-op unless beginShuttle ran at
            // gesture start).
            if (m_dualController) {
                m_dualController->shuttleTick(target, signedSpeed);
            } else if (m_playlistActive && m_audio
                       && m_audio->shuttleActive()) {
                const Clip *c = clipAtTimelineSec(m_fastSeekPositionSec);
                m_audio->shuttleTarget(
                    c ? c->mediaPath : QString(),
                    c ? c->sourceIn + (m_fastSeekPositionSec - c->startTime)
                      : 0.0,
                    signedSpeed);
            } else if (m_audio && m_audio->shuttleActive()
                       && m_videoDecoder) {
                m_audio->shuttleTarget(m_videoDecoder->sourcePath(),
                                       m_fastSeekPositionSec,
                                       signedSpeed);
            }
        });
    }
    m_fastSeekTimer->setInterval(kFastSeekTickMs);
    m_fastSeekTimer->start();
}

// Phase B — Source B open / close. Minimum-viable wiring: a
// second VideoDecoder pumps frames into the renderer's `srcB`
// slot. No audio, no scrub decoder, no atomic-pair sync to A
// yet — those are follow-up phases.
// ---- Phase 7.7 Stage 5 — Source B persistence + drop entry ----------

bool WindowManager::setBSource(const QString &path)
{
    if (!m_project) {
        qWarning("setBSource: no project");
        return false;
    }
    if (path.isEmpty()) {
        clearBSource();
        return true;
    }

    // Reject audio media. Dual-view compares two visual streams; an
    // audio-only B-side has no frames to composite and the dual
    // controller's source machinery doesn't have an audio path.
    // Audio is still fine in the Audio bin and on playlists.
    // v1 blocks live streams from dual entirely (both sides): the
    // dual controller is a synced clock pump over two seekable
    // sources, and a free-running live source breaks that model.
    // Revisit as a dedicated compositor-pairing mode (live A +
    // user-scrubbed B) once the live item has matured.
    if (path.contains(QLatin1String("://"))) {
        qWarning("setBSource: live streams are not dual-capable in v1 (%s)",
                 qPrintable(path));
        return false;
    }

    qcv::MediaType kind = qcv::MediaType::Video;
    qcv::ProjectManager::detectType(path, &kind);
    if (kind == qcv::MediaType::Audio) {
        qWarning("setBSource: audio media not allowed as B-source (%s)",
                 qPrintable(path));
        return false;
    }

    // Add to bins (dedupe by path inside ProjectManager::addMediaFile).
    const QString id = m_project->addMediaFile(path);
    if (id.isEmpty()) {
        qWarning("setBSource: addMediaFile failed for %s", qPrintable(path));
        return false;
    }
    m_project->setBSourceMediaId(id);

    // If we're already in dual mode, hot-swap B in the running
    // controller AND populate the timeline's secondary track.
    // In single mode B is on the project but NOT in the timeline —
    // single mode is single-source only.
    //
    // Use the dual SOURCE's metadata for the timeline, not the
    // MediaItem's. The MediaItem's duration/fps come from
    // MetadataService asynchronously and are 0 right after
    // addMediaFile; the dual source's open() probes them
    // synchronously, so they're valid immediately after swapB.
    if (m_dualController) {
        m_dualController->swapB(path);
        // Re-install the render-wake callback on the freshly-created
        // B source. swapB() constructs a brand-new IDualSource; the
        // original install only ran for the open()-time sources in
        // the Single→Dual block. Without this a hot-swapped B never
        // wakes the render-on-demand loop after an async decode, so
        // paused seeks leave the B side stuck on a stale frame.
        if (auto *r = fetchActiveRenderer(m_playerWindow.data())) {
            if (auto *sb = m_dualController->sourceB()) {
                sb->setFrameAvailableCallback(
                    [r] { r->requestUpdate(); });
            }
        }
        // Re-apply the new B item's saved audio routing mode — swapB
        // rebuilt the mixer's B decoder, which starts at Auto.
        if (m_dualController->audio()) {
            if (const MediaItem *itB = m_project->findItem(id)) {
                m_dualController->audio()->setRoutingModeB(
                    static_cast<int>(itB->audioRoutingMode));
            }
        }
        if (m_timeline) {
            if (auto *src = m_dualController->sourceB()) {
                const double fps = src->fps();
                const int    fc  = src->frameCount();
                const double dur = (fps > 0.0 && fc > 0)
                    ? static_cast<double>(fc) / fps : 0.0;
                const MediaItem *item = m_project->findItem(id);
                const QString name = item ? item->name
                                           : QFileInfo(path).fileName();
                m_timeline->loadSecondarySource(path, name, dur, fps,
                                                  /*hasAudio=*/false);
            }
        }
    }
    qInfo("WindowManager: setBSource -> %s (id=%s)",
          qPrintable(QFileInfo(path).fileName()), qPrintable(id));
    return true;
}

void WindowManager::clearBSource()
{
    if (!m_project) return;
    m_project->clearBSource();
    if (m_dualController) {
        m_dualController->swapB(QString());
    }
    qInfo("WindowManager: clearBSource");
}

bool WindowManager::openSourceB(const QString &path)
{
    if (!m_videoDecoderB) return false;
    if (!m_videoDecoderB->open(path)) {
        qWarning("WindowManager: openSourceB failed: %s", qPrintable(path));
        return false;
    }
#ifdef QCV_NATIVE_PLAYER
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        if (auto *r = pw->renderer()) {
            r->setVideoDecoderB(m_videoDecoderB);
        }
    }
#endif
    // VideoDecoder::open auto-starts playback — match A's current
    // state so B doesn't loop on its own while A is paused.
    // "A's playing" means the timer when image-seq is active, or
    // the video decoder otherwise.
    const bool aPlaying =
        m_imageSeqActive
        ? (m_timeline && m_timeline->timer()->isPlaying())
        : (m_videoDecoder && m_videoDecoder->isPlaying());
    if (!aPlaying) m_videoDecoderB->pause();
    // Prime B with a frame at A's current playhead so it lines up
    // visually as soon as the load completes.
    m_videoDecoderB->seekToFrame(currentFrameUnified());
    // Promote into the timeline model — TimelineController gets a
    // sibling "B" track so QML and (later) edit ops see two lanes.
    if (m_timeline) {
        const double fps = m_videoDecoderB->fps();
        const int    fc  = m_videoDecoderB->frameCount();
        const double dur = (fps > 0.0 && fc > 0)
                           ? static_cast<double>(fc) / fps
                           : 0.0;
        m_timeline->loadSecondarySource(path, QString(), dur, fps,
                                        /*hasAudio=*/false);
    }
    // Mismatched-clip-length: extend the timeline to the longer
    // side and recompute per-side activity for the new pair.
    applyMasterDuration();
    pushSourceActivity();
    return true;
}

void WindowManager::closeSourceB()
{
    if (!m_videoDecoderB) return;
    if (!m_videoDecoderB->sourcePath().isEmpty()) {
        m_videoDecoderB->close();
    }
#ifdef QCV_NATIVE_PLAYER
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        if (auto *r = pw->renderer()) {
            r->setVideoDecoderB(nullptr);
        }
    }
#endif
    // Drop the B track from the timeline model — QML's two-lane
    // view collapses back to one.
    if (m_timeline) m_timeline->clearSecondarySource();
    // Restore single-source duration (max collapses to durA) and
    // re-enable both sides so any subsequent single-source draw
    // sees the canvas active.
    applyMasterDuration();
    pushSourceActivity();
}

// ---- Phase 7.7 Stage 3 — dual playback island test entry --------

bool WindowManager::enterDualTestMode(const QString &pathA,
                                        const QString &pathB)
{
    if (m_dualController) {
        qWarning("enterDualTestMode: already in dual test mode");
        return false;
    }

    // Stage 3 doesn't yet tear down the single-flow A decoder — the
    // renderer just stops drawing its single-flow pipeline when
    // RendererMode::DualFlow is set. That keeps this entry minimally
    // invasive for verification. Stage 4 wires the proper cold
    // teardown when the user toggles compositorMode.
    m_dualController = std::make_unique<qcv::dual::DualPlaybackController>(this);
    if (!m_dualController->open(pathA, qcv::dual::DualSourceKind::AutoDetect,
                                  pathB, qcv::dual::DualSourceKind::AutoDetect)) {
        qWarning("enterDualTestMode: open failed");
        m_dualController.reset();
        return false;
    }
    if (m_dualController->audio()) {
        m_dualController->audio()->setSyncOffsetMs(dualAudioSyncOffsetMs());
        // Master volume/mute mirror — same wiring as the real dual
        // entry in setCompositorMode.
        if (m_audio) {
            auto *mix = m_dualController->audio();
            mix->setMasterVolume(m_audio->volume());
            mix->setMasterMuted(m_audio->muted());
            connect(m_audio, &qcv::AudioPlayer::volumeChanged, mix,
                    [this, mix] { mix->setMasterVolume(m_audio->volume()); });
            connect(m_audio, &qcv::AudioPlayer::mutedChanged, mix,
                    [this, mix] { mix->setMasterMuted(m_audio->muted()); });
        }
    }

    // Wire renderer: switch to DualFlow + plant the controller pointer
    // so MetalPlayerRenderer's drawFrame branch can pull frames.
#ifdef QCV_NATIVE_PLAYER
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        if (auto *r = pw->renderer()) {
            r->setDualController(static_cast<void *>(m_dualController.get()));
            r->setRendererMode(qcv::RendererMode::DualFlow);
            r->requestUpdate();
        }
        pw->setDualActive(true);
    }
#endif

    // Same as setCompositorMode: leave paused for readahead warm-up.
    qInfo("WindowManager: entered dual test mode (paused for readahead)");
    return true;
}

void WindowManager::exitDualTestMode()
{
    if (!m_dualController) return;

#ifdef QCV_NATIVE_PLAYER
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        if (auto *r = pw->renderer()) {
            r->setRendererMode(qcv::RendererMode::SingleFlow);
            r->setDualController(nullptr);
            r->requestUpdate();
        }
        pw->setDualActive(false);
    }
#endif
    m_dualController->close();
    m_dualController.reset();
    qInfo("WindowManager: exited dual test mode");
}

void WindowManager::dualPlay()
{
    if (m_dualController) m_dualController->play();
}

void WindowManager::dualPause()
{
    if (m_dualController) m_dualController->pause();
}

void WindowManager::dualSeekToFrame(int frameNumber)
{
    if (m_dualController) m_dualController->seekToFrame(frameNumber);
}

// Phase 7.8 Stage F — save the current dual session state as a
// new DualPair MediaItem. Caller is the QML "Save as Dual View"
// button, which prompts the user for a name and passes it here.
QString WindowManager::saveCurrentDualView(const QString &name)
{
    if (!m_dualController || !m_project || !m_timeline) {
        qWarning("saveCurrentDualView: not in dual mode");
        return {};
    }

    DualPairData data;
    data.mediaIdA = m_project->activeItemId();
    data.mediaIdB = m_project->bSourceMediaId();
    data.masterFps      = m_dualController->fps();
    data.masterDuration = m_timeline->timeline().duration;
    data.clipsA = m_timeline->trackA().value(
                      QStringLiteral("clips")).toList();
    data.clipsB = m_timeline->trackB().value(
                      QStringLiteral("clips")).toList();
    data.inPoint  = m_inPoint;
    data.outPoint = m_outPoint;

    QString resolvedName = name;
    if (resolvedName.trimmed().isEmpty()) {
        const MediaItem *itemA = m_project->findItem(data.mediaIdA);
        const MediaItem *itemB = m_project->findItem(data.mediaIdB);
        const QString aName = itemA ? itemA->name : QStringLiteral("(none)");
        const QString bName = itemB ? itemB->name : QStringLiteral("(none)");
        resolvedName = QStringLiteral("%1 vs %2").arg(aName, bName);
    }

    const QString id = m_project->addDualPairItem(resolvedName, data);
    // Bind the live session to the just-created item so a subsequent
    // save offers "Update" (overwrite this copy) rather than spawning
    // yet another duplicate.
    setActiveDualViewId(id);
    qInfo("WindowManager: saved DualPair '%s' (id=%s)",
          qPrintable(resolvedName), qPrintable(id));
    return id;
}

// Phase 7.8 Stage F — overwrite an existing saved DualView in place.
// Snapshots the live dual session exactly like saveCurrentDualView,
// then writes it back into the bound item (id + name preserved).
bool WindowManager::updateDualView(const QString &dualPairId)
{
    if (!m_dualController || !m_project || !m_timeline) {
        qWarning("updateDualView: not in dual mode");
        return false;
    }
    if (dualPairId.isEmpty()) return false;
    const MediaItem *existing = m_project->findItem(dualPairId);
    if (!existing || existing->type != MediaType::DualPair) {
        qWarning("updateDualView: id '%s' not a DualPair",
                 qPrintable(dualPairId));
        return false;
    }

    DualPairData data;
    data.mediaIdA = m_project->activeItemId();
    data.mediaIdB = m_project->bSourceMediaId();
    data.masterFps      = m_dualController->fps();
    data.masterDuration = m_timeline->timeline().duration;
    data.clipsA = m_timeline->trackA().value(
                      QStringLiteral("clips")).toList();
    data.clipsB = m_timeline->trackB().value(
                      QStringLiteral("clips")).toList();
    data.inPoint  = m_inPoint;
    data.outPoint = m_outPoint;

    const bool ok = m_project->updateDualPairItem(dualPairId, data);
    if (ok) {
        // Keep the binding pointed at this item (it already is, but be
        // explicit so the Update affordance stays live).
        setActiveDualViewId(dualPairId);
        qInfo("WindowManager: updated DualPair '%s' (id=%s)",
              qPrintable(existing->name), qPrintable(dualPairId));
    }
    return ok;
}

void WindowManager::setActiveDualViewId(const QString &id)
{
    if (m_activeDualViewId == id) return;
    m_activeDualViewId = id;
    emit activeDualViewIdChanged();
}

bool WindowManager::loadDualView(const QString &dualPairId)
{
    if (!m_project) return false;
    const MediaItem *item = m_project->findItem(dualPairId);
    if (!item || item->type != MediaType::DualPair) {
        qWarning("loadDualView: id '%s' not a DualPair",
                 qPrintable(dualPairId));
        return false;
    }
    const DualPairData &data = item->dualPair;
    const MediaItem *itemA = m_project->findItem(data.mediaIdA);
    const MediaItem *itemB = m_project->findItem(data.mediaIdB);
    if (!itemA) {
        qWarning("loadDualView: source A '%s' missing from pool",
                 qPrintable(data.mediaIdA));
        return false;
    }

    // Step 1: load A as the active item (single mode). If we're
    // already in dual, this auto-reverts to single first.
    m_project->setActiveItem(data.mediaIdA);

    // Step 2: set B (project-level state). Doesn't enter dual yet.
    if (itemB) {
        setBSource(itemB->path);
    } else {
        clearBSource();
    }

    // Step 3: enter dual mode (Single→Dual cold transition).
    setCompositorMode(1);

    // Step 4: replace the auto-built tracks with the saved clip
    // layouts. This bypasses undo (load is a fresh state).
    if (m_timeline) {
        if (!data.clipsA.isEmpty()) {
            m_timeline->replaceTrackClips(QStringLiteral("A"), data.clipsA);
        }
        if (!data.clipsB.isEmpty()) {
            m_timeline->replaceTrackClips(QStringLiteral("B"), data.clipsB);
        }
        // Restore in/out (master frame numbers).
        m_inPoint  = data.inPoint;
        m_outPoint = data.outPoint;
        emit inOutPointsChanged();
    }
    // Bind the session to this saved view. Set LAST so the
    // intermediate single-revert from setActiveItem (step 1), which
    // clears the binding via setCompositorMode(0), doesn't wipe it.
    setActiveDualViewId(dualPairId);
    qInfo("WindowManager: loaded DualPair '%s'", qPrintable(item->name));
    return true;
}

bool WindowManager::deleteMediaItems(const QStringList &ids)
{
    if (!m_project) return false;

    QStringList valid;
    QString firstName;
    for (const QString &id : ids) {
        const MediaItem *item = m_project->findItem(id);
        if (!item) continue;
        if (valid.isEmpty()) firstName = item->name;

        // If the user is deleting the currently-displayed item, tear
        // down playback first so we don't leave the decoder streaming
        // a file that no longer exists in the project. In dual mode,
        // exit dual back to single (matches the loadRequested
        // behavior on click).
        if (id == m_project->activeItemId()) {
            if (m_compositorMode != 0) {
                setCompositorMode(0);
            }
            closeActiveMedia();
        }
        if (id == m_project->bSourceMediaId()) {
            clearBSource();
        }
        valid << id;
    }

    const int removed = m_project->removeMediaItems(valid);
    if (removed <= 0) return false;

    const QString msg = (removed == 1)
        ? tr("Removed %1").arg(firstName)
        : tr("Removed %1 items").arg(removed);
    toastAction(msg, 1, tr("Undo"),
                QStringLiteral("undo-media-delete"));
    return true;
}

// Phase 7.8 Stage A verification — hand-mutates the timeline so we
// can observe the master→source translation in action without UI
// drag handles (Stage C lands those). NOT thread-safe vs. the dual
// pump thread; for diagnostics only.
bool WindowManager::testHeadTrim(const QString &trackId, double seconds)
{
    if (!m_dualController) {
        qWarning("testHeadTrim: not in dual mode");
        return false;
    }
    if (!m_timeline) return false;
    Timeline t = m_timeline->timeline();   // copy
    bool found = false;
    for (Track &track : t.tracks) {
        if (track.id != trackId) continue;
        if (track.clips.isEmpty()) break;
        Clip &c = track.clips[0];
        c.sourceIn = std::max(0.0, c.sourceIn + seconds);
        found = true;
        qInfo("testHeadTrim: track=%s sourceIn now=%.4fs",
              qPrintable(trackId), c.sourceIn);
        break;
    }
    if (!found) return false;
    // Apply via the controller's internal accessor — for Stage A we
    // expose a one-shot setter on TimelineController. (Stage C
    // replaces this with proper QUndoCommands.)
    m_timeline->_replaceTimelineForTesting(t);
    return true;
}

double WindowManager::sourceDurationA() const
{
    if (m_imageSeqActive && m_imageSeqCache) {
        const double fps = m_imageSeqCache->fps();
        const int    fc  = m_imageSeqCache->frameCount();
        return (fps > 0.0 && fc > 0) ? static_cast<double>(fc) / fps : 0.0;
    }
    if (m_videoDecoder && !m_videoDecoder->sourcePath().isEmpty()) {
        const double fps = m_videoDecoder->fps();
        const int    fc  = m_videoDecoder->frameCount();
        return (fps > 0.0 && fc > 0) ? static_cast<double>(fc) / fps : 0.0;
    }
    return 0.0;
}

double WindowManager::sourceDurationB() const
{
    if (!m_videoDecoderB || m_videoDecoderB->sourcePath().isEmpty()) {
        return 0.0;
    }
    const double fps = m_videoDecoderB->fps();
    const int    fc  = m_videoDecoderB->frameCount();
    return (fps > 0.0 && fc > 0) ? static_cast<double>(fc) / fps : 0.0;
}

void WindowManager::applyMasterDuration()
{
    if (!m_timeline || !m_timeline->timer()) return;
    const double durA = sourceDurationA();
    const double durB = sourceDurationB();
    const double master = std::max(durA, durB);
    // Only override when both sides are loaded — otherwise leave
    // whatever the single-source path (loadSingleMedia / video
    // open) configured. Single-source drives stay authoritative.
    if (durA > 0.0 && durB > 0.0 && master > 0.0) {
        m_timeline->timer()->setDuration(master);
    }
}

void WindowManager::pushSourceActivity()
{
#ifdef QCV_NATIVE_PLAYER
    auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data());
    if (!pw || !pw->renderer()) return;

    const double durA = sourceDurationA();
    const double durB = sourceDurationB();
    bool aActive = true;
    bool bActive = true;
    if (durA > 0.0 && durB > 0.0 && m_timeline && m_timeline->timer()) {
        // Master clock = timer position. Currently this is reliable
        // in image-seq mode (timer drives playback) and in video
        // mode (constructor mirrors video.currentFrame into the
        // timer). Half-frame tolerance per side so we don't flip
        // off on the very last frame.
        const double pos = m_timeline->timer()->position();
        const double fpsA = m_imageSeqActive
            ? (m_imageSeqCache ? m_imageSeqCache->fps() : 0.0)
            : (m_videoDecoder ? m_videoDecoder->fps() : 0.0);
        const double fpsB = m_videoDecoderB ? m_videoDecoderB->fps() : 0.0;
        const double tolA = fpsA > 0.0 ? 0.5 / fpsA : 0.0;
        const double tolB = fpsB > 0.0 ? 0.5 / fpsB : 0.0;
        aActive = pos < (durA + tolA);
        bActive = pos < (durB + tolB);
    }
    pw->renderer()->setSourceActivity(aActive, bActive);
#endif
}

// Screenshot helpers — pull a QImage from the renderer (blocks
// the GUI thread for one render frame; bounded by the renderer's
// 250 ms timeout) and either copy to clipboard or write a PNG to
// the Desktop with a timestamped filename.
bool WindowManager::screenshotToClipboard()
{
#ifdef QCV_NATIVE_PLAYER
    auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data());
    if (!pw || !pw->renderer()) return false;
    QImage img = pw->renderer()->captureScreenshot();
    if (img.isNull()) {
        qWarning("WindowManager: screenshot capture returned empty");
        toast(tr("Nothing to capture — load media first"), 1);
        return false;
    }
    QGuiApplication::clipboard()->setImage(img);
    qInfo("WindowManager: screenshot %dx%d copied to clipboard",
          img.width(), img.height());
    // Instant (in-memory) — no worker needed; just flash the result.
    emit exportFinished(true, tr("Screenshot copied to clipboard"));
    return true;
#else
    return false;
#endif
}

bool WindowManager::screenshotToFile()
{
#ifdef QCV_NATIVE_PLAYER
    auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data());
    if (!pw || !pw->renderer()) return false;
    QImage img = pw->renderer()->captureScreenshot();
    if (img.isNull()) {
        qWarning("WindowManager: screenshot capture returned empty");
        toast(tr("Nothing to capture — load media first"), 1);
        return false;
    }
    const QString desktop =
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (desktop.isEmpty()) {
        qWarning("WindowManager: no DesktopLocation");
        toast(tr("Couldn't resolve the Desktop folder"), 2);
        return false;
    }
    // Resolve the user-chosen export format. This affects ONLY this
    // desktop export — note thumbnails / annotation baselines stay
    // PNG. JPEG is lossy + alpha-less (fine: captures clear opaque)
    // and takes a quality; PNG/TIFF are lossless. The Qt token below
    // is the writer format ("JPEG"/"PNG"/"TIFF"); the file extension
    // and probe key are lower-case.
    QString token = screenshotFormat();            // "png"|"jpeg"|"tiff"
    QByteArray qtFormat = token.toUpper().toLatin1();
    QString ext = (token == QLatin1String("jpeg"))
                      ? QStringLiteral("jpg") : token;
    int quality = (token == QLatin1String("jpeg")) ? 95 : -1;

    // Runtime guard: the JPEG/TIFF writers live in Qt image-format
    // plugins that must be present in the deployed bundle. If the
    // chosen format isn't writable on this build, fall back to PNG
    // (always built into QtGui) so we never emit a silent empty file.
    if (!QImageWriter::supportedImageFormats().contains(token.toLatin1())) {
        qWarning("WindowManager: '%s' writer unavailable — falling back "
                 "to PNG (is the Qt Image Formats plugin bundled?)",
                 qPrintable(token));
        token = QStringLiteral("png");
        qtFormat = "PNG";
        ext = QStringLiteral("png");
        quality = -1;
    }

    const QString stamp = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
    const QString fileName =
        QStringLiteral("qcview-screenshot-%1.%2").arg(stamp, ext);
    const QString path =
        desktop + QLatin1Char('/') + fileName;

    // Offload the encode + disk write to a worker thread so the GUI
    // never stalls on the (~100 ms HD) encode — same pattern as the
    // note thumbnails. The capture above already deep-copied the
    // frame, so `img` is safe to hand to the worker by value (QImage
    // is copy-on-write). exportStarted/Finished drive the StatusStrip
    // chip; Finished is marshalled back to the GUI thread.
    emit exportStarted(tr("Saving screenshot…"));
    const QString fmtName = QString::fromLatin1(qtFormat);   // "PNG"|"JPEG"|"TIFF"
    (void)QtConcurrent::run(
        [this, img, path, fmtName, qtFormat, quality]() {
            const bool ok = img.save(path, qtFormat.constData(), quality);
            if (ok)
                qInfo("WindowManager: screenshot saved to %s",
                      qPrintable(path));
            else
                qWarning("WindowManager: screenshot save failed: %s",
                         qPrintable(path));
            // Keep the toast SHORT — no filename — so it can't overflow
            // the StatusStrip row and eat the right margin. The format
            // confirms TIFF/JPEG actually took (vs the PNG fallback).
            QMetaObject::invokeMethod(this, [this, ok, fmtName]() {
                emit exportFinished(
                    ok,
                    ok ? tr("Screenshot saved (%1)").arg(fmtName)
                       : tr("Screenshot export failed"));
            }, Qt::QueuedConnection);
        });
    return true;
#else
    return false;
#endif
}

// Borderless-fullscreen Q_INVOKABLE wrappers. The platform-specific
// helpers live in native_fullscreen_macos.mm / native_fullscreen_win.cpp;
// Q_INVOKABLE methods on the header are unconditional so QML doesn't
// have to branch.
bool WindowManager::enterBorderlessFullscreen(QWindow *window)
{
#ifdef Q_OS_MACOS
    return qcv::enterBorderlessFullscreen(window);
#elif defined(Q_OS_WIN)
    return qcv::enterBorderlessFullscreenWin(window);
#else
    Q_UNUSED(window);
    return false;
#endif
}

bool WindowManager::exitBorderlessFullscreen(QWindow *window)
{
#ifdef Q_OS_MACOS
    return qcv::exitBorderlessFullscreen(window);
#elif defined(Q_OS_WIN)
    return qcv::exitBorderlessFullscreenWin(window);
#else
    Q_UNUSED(window);
    return false;
#endif
}

bool WindowManager::isBorderlessFullscreen(QWindow *window) const
{
#ifdef Q_OS_MACOS
    return qcv::isBorderlessFullscreen(window);
#else
    Q_UNUSED(window);
    return false;
#endif
}

void WindowManager::stopFastSeek()
{
    if (!m_fastSeekTimer) return;
    m_fastSeekTimer->stop();
    // Tear down shuttle audio BEFORE the final commit so the commit
    // seek + (possible) play() below re-seat the normal pipeline.
    if (m_dualController) m_dualController->endShuttle();
    if (m_audio && m_audio->shuttleActive()) m_audio->endShuttle();
    const int prevDir = m_fastSeekDir;
    m_fastSeekDir   = 0;
    m_fastSeekSpeed = 0.0;
    const bool wasActive = m_fastSeekActive;
    // Clear the mirror gate FIRST so the final seekToFrame's
    // post-decode mirror update lands cleanly.
    m_fastSeekActive = false;
    // Final commit. Routes correctly per mode — dual seeks both
    // sides, playlist swaps clips if needed, single re-issues to
    // the decoder (idempotent if last tick already targeted there),
    // image-seq nudges the cache window, audio re-seats both timer
    // and audio decoder.
    if (wasActive && prevDir != 0) {
        double fps = 0.0;
        if (m_dualController) {
            fps = m_dualController->fps();
        } else if (m_timeline && (m_imageSeqActive || m_audioActive
                                   || m_playlistActive)) {
            fps = m_timeline->timer()->frameRate();
        } else if (m_videoDecoder) {
            fps = m_videoDecoder->fps();
        }
        const int total = frameCountUnified();
        if (fps > 0.0 && total > 0) {
            int target = static_cast<int>(
                std::lround(m_fastSeekPositionSec * fps));
            target = std::clamp(target, 0, total - 1);
            seekToFrame(target);
        }
    }
    // Don't delete the timer — startFastSeek reuses it.
}

void WindowManager::installGlobalKeyFilter(QQuickWindow *window)
{
    // Install on QGuiApplication, not the specific window, so transport
    // keys also fire when focus is on the native player QWindow (Phase
    // 7.5 surface) — that's a separate top-level window and its events
    // never touch the main QQuickWindow's filter chain. The `window`
    // parameter is kept for backward-compat with the QML caller but
    // ignored.
    Q_UNUSED(window);
    if (qApp) {
        // Avoid double-install (Q_INVOKABLE could fire on hot reload).
        qApp->removeEventFilter(this);
        qApp->installEventFilter(this);
    }
}

bool WindowManager::eventFilter(QObject *watched, QEvent *event)
{
    const QEvent::Type t = event->type();

    // QEvent::FileOpen — Cocoa delivers qcview://… URL launches and
    // double-clicked / Finder-"Open With" file opens through this
    // single event type. url() is set for URL-scheme launches, file()
    // for document opens. (Cocoa does NOT pass file args through argv
    // for these — the CLI-args path in main.cpp only fires for direct
    // command-line launches, hence the dedicated event hook here.)
    //
    // Both branches defer the actual work via QTimer::singleShot(0) so
    // the WM is fully initialized; at cold launch the FileOpen can
    // arrive before QML is visible / ProjectManager has finished
    // hydrating, and routing through addMediaFile mid-init can race
    // the bin / inspector bindings.
    if (t == QEvent::FileOpen) {
        auto *foe = static_cast<QFileOpenEvent *>(event);
        const QString urlStr = foe->url().toString();
        if (!urlStr.isEmpty()
            && urlStr.startsWith(QStringLiteral("qcview://"),
                                  Qt::CaseInsensitive)) {
            QTimer::singleShot(0, this, [this, urlStr] {
                openProjectLink(urlStr);
            });
            return true;
        }

        const QString filePath = foe->file();
        if (!filePath.isEmpty()) {
            QTimer::singleShot(0, this, [this, filePath] {
                if (!m_project) return;
                // .qcv / .qcvproj → openProject; everything else
                // (video, image, image-seq member) → addMediaFile,
                // matching the CLI-args dispatch in main.cpp:357-382.
                const QString lower = filePath.toLower();
                if (lower.endsWith(QStringLiteral(".qcvproj"))
                    || lower.endsWith(QStringLiteral(".qcv"))) {
                    m_project->openProject(filePath);
                    return;
                }
                const QString id = m_project->addMediaFile(filePath);
                if (!id.isEmpty()) m_project->setActiveItem(id);
            });
            return true;
        }
        return QObject::eventFilter(watched, event);
    }

    // Click-away releases text-editor focus, app-wide. QML's
    // MouseArea-based surfaces (timeline scrub, bin rows, the
    // viewport) never TAKE focus on click, so a focused note
    // TextArea kept activeFocus after the user clicked away — and
    // the text-item bail below then routed every transport key INTO
    // the note as typed text. Any press outside the focused editor's
    // (clip-chain-visible) bounds drops its focus BEFORE the press
    // is delivered; the editor's own onActiveFocusChanged commit
    // handlers fire (note text save, rename commit, spinbox
    // editingFinished), and whatever was clicked can then claim
    // focus normally. Presses INSIDE the editor pass through
    // untouched (caret placement / text selection).
    if (t == QEvent::MouseButtonPress) {
        auto *win = qobject_cast<QQuickWindow *>(watched);
        if (win) {
            if (QObject *focused = QGuiApplication::focusObject()) {
                const bool isTextItem =
                    focused->property("cursorPosition").isValid()
                    && focused->property("selectionStart").isValid();
                auto *qitem = qobject_cast<QQuickItem *>(focused);
                if (isTextItem && qitem) {
                    bool inside = false;
                    if (qitem->window() == win) {
                        const auto *me = static_cast<QMouseEvent *>(event);
                        // Editor bounds in scene coords, intersected
                        // with every clipping ancestor (a long note
                        // TextArea extends past its ScrollView's
                        // viewport — the unclipped rect would claim
                        // clicks on content visually below it).
                        QRectF sceneRect = qitem->mapRectToScene(
                            QRectF(0, 0, qitem->width(), qitem->height()));
                        for (QQuickItem *p = qitem->parentItem(); p;
                             p = p->parentItem()) {
                            if (p->clip()) {
                                sceneRect &= p->mapRectToScene(
                                    QRectF(0, 0, p->width(), p->height()));
                            }
                        }
                        inside = sceneRect.contains(me->scenePosition());
                    }
                    if (!inside) {
                        qitem->setFocus(false, Qt::MouseFocusReason);
                    }
                }
            }
        }
        return QObject::eventFilter(watched, event);
    }

    if (t != QEvent::KeyPress && t != QEvent::KeyRelease) {
        return QObject::eventFilter(watched, event);
    }

    auto *ke = static_cast<QKeyEvent *>(event);

    // Bail when a text input has focus so users can still type
    // letters in note bodies, save-dialog name fields, etc. Use
    // QGuiApplication::focusObject() instead of routing through
    // watched->focusObject() — the former returns the focused
    // object across all windows regardless of which QObject the
    // event happened to be filtered against, which fixes the case
    // where Qt delivers the key event to a non-QQuickWindow
    // ancestor in the chain.
    //
    // Detection is property-based: cursorPosition + selectionStart
    // are the canonical pair for a text-editing item. This catches
    // QQuickTextInput / QQuickTextEdit / TextField / TextArea (and
    // the inner TextInput inside an *editing* SpinBox) at the exact
    // moment they hold focus. Crucially this does NOT match
    // QQuickComboBox, QQuickSpinBox, QQuickSlider, or QQuickButton
    // — clicking a closed FlatComboBox or FlatSpinBox parks focus
    // on the control itself (not its inner text), so plain-letter
    // shortcuts (Space, K, M, B, F, T, I, O, Q, E…) keep working.
    if (QObject *focused = QGuiApplication::focusObject()) {
        const bool isTextItem =
            focused->property("cursorPosition").isValid()
            && focused->property("selectionStart").isValid();
        if (isTextItem) {
            // Defensive: only bail when the text item is actually
            // visible AND has activeFocus. QML can leave a hidden
            // TextField as the focusObject after a `visible: false`
            // binding tears its row down — the dangling object then
            // swallows every shortcut until something else explicitly
            // takes focus. Verified failure mode during fast-loop
            // playback where Repeater-instantiated rename / inline-
            // editor delegates churn while focus is on one of them.
            const QVariant visProp = focused->property("visible");
            const QVariant focProp = focused->property("activeFocus");
            const bool visible = !visProp.isValid() || visProp.toBool();
            const bool reallyFocused =
                !focProp.isValid() || focProp.toBool();
            if (visible && reallyFocused) {
                return QObject::eventFilter(watched, event);
            }
            // Stale-focus path: a hidden / non-active object claims
            // focus. Logging alone isn't enough — Qt still routes
            // the key event to the focusObject natively, so an
            // invisible TextField can swallow Delete / Backspace
            // before any QML Shortcut sees it (this is what was
            // breaking select+delete on the playlist timeline).
            // Drop the dangling focus so the next event delivery
            // round routes through the active QQuickWindow's normal
            // focus chain.
            if (auto *qitem = qobject_cast<QQuickItem *>(focused)) {
                qitem->setFocus(false, Qt::OtherFocusReason);
            } else {
                focused->setProperty("focus", false);
            }
            // Throttle the log — without this the spam buries every
            // other warning while the user mouses around.
            static QPointer<QObject> s_lastReported;
            if (s_lastReported != focused) {
                s_lastReported = focused;
                const char *cls = focused->metaObject()
                                    ? focused->metaObject()->className()
                                    : "?";
                qWarning("eventFilter: cleared stale text-focus on %s "
                         "(visible=%d activeFocus=%d)",
                         cls, int(visible), int(reallyFocused));
            }
        }
    }

    // Hold-to-fast-seek: A/J = rewind, D/L = fast-forward. Press +
    // release; ignore auto-repeat (timer drives the actual scrub).
    if (!ke->isAutoRepeat()) {
        int direction = 0;
        switch (ke->key()) {
        case Qt::Key_A:
        case Qt::Key_J: direction = -1; break;
        case Qt::Key_D:
        case Qt::Key_L: direction = 1;  break;
        }
        if (direction != 0) {
            if (t == QEvent::KeyPress) startFastSeek(direction);
            else                       stopFastSeek();
            return true;
        }
    }

    // The rest are single-press transport bindings — handle KeyPress
    // only, drop KeyRelease. Routed through the application-level
    // event filter (rather than QML Shortcut) because QML's Shortcut
    // resolution gets unreliable on macOS once focus drops onto a
    // child item — clicking the timeline panel breaks plain-letter
    // shortcuts. The filter runs before any QML routing, so it
    // works regardless of focus.
    if (t != QEvent::KeyPress) {
        return QObject::eventFilter(watched, event);
    }

    const auto mods = ke->modifiers();
    const bool plain = (mods == Qt::NoModifier
                        || mods == Qt::KeypadModifier);

    if (plain) {
        // Q/E (and ←/→) step by one frame — allow auto-repeat for
        // hold-to-step.
        switch (ke->key()) {
        case Qt::Key_Q:
        case Qt::Key_Left:  stepFrames(-1); return true;
        case Qt::Key_E:
        case Qt::Key_Right: stepFrames(1);  return true;
        default: break;
        }
        if (ke->isAutoRepeat()) {
            return QObject::eventFilter(watched, event);
        }
        switch (ke->key()) {
        case Qt::Key_Space:
        case Qt::Key_K:
        case Qt::Key_W:
        case Qt::Key_S:
            togglePlayback();
            return true;
        case Qt::Key_Home: seekToStart(); return true;
        case Qt::Key_End:  seekToEnd();   return true;
        case Qt::Key_M:
            if (m_audio) m_audio->setMuted(!m_audio->muted());
            return true;
        case Qt::Key_B:
            setBackgroundMode((m_backgroundMode + 1) % 4);
            return true;
        case Qt::Key_T:
            screenshotToFile();
            return true;
        case Qt::Key_I:
            setInPointAtCurrent();
            return true;
        case Qt::Key_O:
            setOutPointAtCurrent();
            return true;
        case Qt::Key_V:
            // Loop toggle. V (not L) because L is already bound to
            // fast-forward-hold for JKL-muscle-memory users — Guide 02
            // promised L for loop, but the JKL alias predates and wins.
            setLoopEnabled(!loopEnabled());
            return true;
        case Qt::Key_R:
            // Review speed: cycle 0.5 → 0.75 → 1 → 1.25 → 1.5 → 2.
            // Shift+R (below) snaps back to 1x.
            cycleReviewSpeed();
            return true;
        case Qt::Key_F:
            emit fullscreenToggleRequested();
            return true;
        case Qt::Key_Up:
            if (m_audio) {
                m_audio->setVolume(
                    std::min(1.0f, m_audio->volume() + 0.05f));
            }
            return true;
        case Qt::Key_Down:
            if (m_audio) {
                m_audio->setVolume(
                    std::max(0.0f, m_audio->volume() - 0.05f));
            }
            return true;
        default: break;
        }
    } else if (mods == Qt::ShiftModifier) {
        if (ke->key() == Qt::Key_I) {
            clearInOutPoints();
            return true;
        }
        if (ke->key() == Qt::Key_R) {
            setReviewSpeed(1.0);
            return true;
        }
    } else if (mods == Qt::ControlModifier) {
        // Ctrl/Cmd+T — screenshot to clipboard. Plain T goes to the
        // desktop file; Ctrl+T copies the same RGBA frame to the
        // system clipboard for paste-anywhere workflows.
        if (ke->key() == Qt::Key_T) {
            screenshotToClipboard();
            return true;
        }
        // Cmd+Z (annotation undo). Only fires when annotation
        // mode is active so it doesn't intercept the user's
        // cmd+Z in any future text-editing contexts beyond the
        // ones the bailout above already exempts.
        if (ke->key() == Qt::Key_Z
            && isAnnotationActive()) {
            annotationUndo();
            return true;
        }
    } else if (mods == (Qt::ControlModifier | Qt::ShiftModifier)) {
        // Cmd+Shift+Z — redo.
        if (ke->key() == Qt::Key_Z
            && isAnnotationActive()) {
            annotationRedo();
            return true;
        }
    }
    // Esc — cancel an in-flight stroke (annotation mode only).
    // Does NOT consume the event when no stroke is in flight, so
    // other Esc handlers (fullscreen exit, inline rename) keep
    // working.
    if (mods == Qt::NoModifier && ke->key() == Qt::Key_Escape
        && isAnnotationActive() && m_annotator
        && m_annotator->activeStroke()) {
        cancelActiveStroke();
        return true;
    }

    return QObject::eventFilter(watched, event);
}

int WindowManager::currentFrameUnified() const
{
    if (m_dualController) return m_dualController->currentFrame();
    // Phase 3.H.4 — playlist mode reports a TIMELINE frame index
    // (master fps). Step / fast-seek / home / end all use this and
    // expect to operate on the whole playlist, not the active
    // clip's source range.
    if (m_playlistActive && m_timeline) {
        const auto *t = m_timeline->timer();
        const double fps = t ? t->frameRate() : 0.0;
        if (fps > 0.0) {
            return std::max(0, static_cast<int>(
                std::lround(t->position() * fps)));
        }
    }
    if (m_imageSeqActive) {
        return m_timeline ? m_timeline->timer()->currentFrame() : 0;
    }
    if (m_audioActive) {
        return m_timeline ? m_timeline->timer()->currentFrame() : 0;
    }
    return m_videoDecoder ? std::max(0, m_videoDecoder->currentFrame()) : 0;
}

int WindowManager::frameCountUnified() const
{
    if (m_dualController) return m_dualController->frameCount();
    if (m_playlistActive && m_timeline) {
        const auto *t = m_timeline->timer();
        const double fps = t ? t->frameRate() : 0.0;
        if (fps > 0.0) {
            return std::max(0, static_cast<int>(
                std::lround(t->duration() * fps)));
        }
    }
    if (m_imageSeqActive) {
        return m_imageSeqCache ? m_imageSeqCache->frameCount() : 0;
    }
    if (m_audioActive && m_timeline) {
        const auto *t = m_timeline->timer();
        const double fps = t ? t->frameRate() : 0.0;
        if (fps > 0.0) {
            return std::max(0, static_cast<int>(
                std::lround(t->duration() * fps)));
        }
    }
    return m_videoDecoder ? m_videoDecoder->frameCount() : 0;
}

bool WindowManager::isPlayingUnified() const
{
    if (m_dualController) return m_dualController->isPlaying();
    if (m_imageSeqActive) {
        return m_timeline ? m_timeline->timer()->isPlaying() : false;
    }
    if (m_audioActive) {
        return m_timeline ? m_timeline->timer()->isPlaying() : false;
    }
    return m_videoDecoder ? m_videoDecoder->isPlaying() : false;
}

// ---------------------------------------------------------------------------
// Annotation drawing (Phase 7.5 B.6.5).
// ---------------------------------------------------------------------------

void WindowManager::setAnnotationActive(bool on)
{
    if (!m_annotator) return;
    // Phase 3.H.6 — gate to single video / single image-seq only.
    // QML side hides the toggle in dual / playlist, but the
    // Q_INVOKABLE could still get called programmatically.
    if (on && !annotationsAllowedForCurrentMedia()) {
        m_annotator->setMode(ViewportMode::Playback);
        return;
    }
    m_annotator->setMode(on ? ViewportMode::Annotation : ViewportMode::Playback);
    if (on && m_annotator->activeTool() == DrawingTool::None) {
        // First-time activation defaults to Freehand so the user
        // sees something on first drag without having to set a tool.
        m_annotator->setActiveTool(DrawingTool::Freehand);
    }
    if (on) {
        // Drop any visible hover-preview overlay. The user is
        // about to be drawing on the canvas; the timeline
        // hover-thumb sits in front of the viewport corner and
        // would distract.
        clearHoverThumbnail();
    }
}

bool WindowManager::isAnnotationActive() const
{
    return m_annotator && m_annotator->isAnnotationMode();
}

void WindowManager::setNotesPanelVisible(bool on)
{
    if (m_notesPanelVisible == on) return;
    m_notesPanelVisible = on;
    if (!on) {
        // Closing the notes panel exits annotation mode + drops
        // any in-flight stroke; rebuild then pushes an empty
        // stored-strokes vector to the renderer so the visible
        // image is clean.
        if (m_annotator) m_annotator->cancelActiveStroke();
        // Cancel any pending annotated-thumb save. If the timer
        // fired after the gate empties m_storedStrokes, it would
        // capture a clean frame and overwrite the
        // "_annotated.png" with that clean copy — silently
        // breaking the note's thumbnail.
        if (m_annotatedSaveTimer) m_annotatedSaveTimer->stop();
        m_pendingAnnotatedTimecode.clear();
        setAnnotationActive(false);
    }
    rebuildStoredAnnotationMesh();
}

void WindowManager::setAnnotationTool(int tool)
{
    if (!m_annotator) return;
    if (tool < 0 || tool > 6) return;
    m_annotator->setActiveTool(static_cast<DrawingTool>(tool));
}

void WindowManager::setAnnotationColor(double r, double g, double b, double a)
{
    if (!m_annotator) return;
    m_annotator->setDrawingColor(QColor::fromRgbF(r, g, b, a));
}

void WindowManager::setAnnotationStrokeWidth(double w)
{
    if (!m_annotator) return;
    m_annotator->setStrokeWidth(static_cast<float>(w));
}

// ---------------------------------------------------------------------------
// Safety overlay (Phase 7.5 B.6.6).
// ---------------------------------------------------------------------------

namespace {

// Mirrors resolveOcioAssetsDir() in ocio_config_manager.cpp.
QString resolveSafetyAssetsDir()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/../Resources/assets/safety"),  // macOS bundle
        appDir + QStringLiteral("/assets/safety"),                // future Linux/Win
        appDir + QStringLiteral("/../../assets/safety"),          // out-of-tree build
    };
    for (const QString &path : candidates) {
        const QString canonical = QDir(path).canonicalPath();
        if (!canonical.isEmpty() && QFileInfo(canonical).isDir()) {
            return canonical;
        }
    }
    return {};
}

} // namespace

// Wake the native player so a state-only edit (like a safety
// opacity slider drag while paused) actually redraws under the
// render-on-demand loop on Windows. macOS continuous loop catches
// state changes naturally; the call is harmless there.
static void wakeNativePlayer(QPointer<QObject> playerWindow)
{
#ifdef QCV_NATIVE_PLAYER
    if (!playerWindow) return;
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(playerWindow.data())) {
        if (auto *r = pw->renderer()) r->requestUpdate();
    }
#else
    Q_UNUSED(playerWindow);
#endif
}

void WindowManager::setSafetySvg(const QString &nameOrPath)
{
    if (!m_safetyOverlay) return;
    if (nameOrPath.isEmpty()) {
        m_safetyOverlay->clear();
        wakeNativePlayer(m_playerWindow);
        return;
    }

    // Absolute / on-disk path: use as-is.
    if (QFileInfo::exists(nameOrPath)) {
        m_safetyOverlay->loadFile(nameOrPath);
        wakeNativePlayer(m_playerWindow);
        return;
    }
    // Otherwise resolve as a built-in name (with or without .svg extension).
    const QString safetyDir = resolveSafetyAssetsDir();
    if (safetyDir.isEmpty()) {
        qWarning("WindowManager::setSafetySvg: safety assets dir not found");
        return;
    }
    QString filename = nameOrPath;
    if (!filename.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive)) {
        filename += QStringLiteral(".svg");
    }
    const QString candidate = QDir(safetyDir).filePath(filename);
    if (!QFileInfo::exists(candidate)) {
        qWarning("WindowManager::setSafetySvg: '%s' not found in %s",
                 qPrintable(filename), qPrintable(safetyDir));
        return;
    }
    m_safetyOverlay->loadFile(candidate);
    wakeNativePlayer(m_playerWindow);
}

void WindowManager::setSafetyOpacity(double opacity)
{
    if (!m_safetyOverlay) return;
    m_safetyOverlay->setOpacity(static_cast<float>(opacity));
    wakeNativePlayer(m_playerWindow);
}

void WindowManager::setSafetyColor(double r, double g, double b)
{
    if (!m_safetyOverlay) return;
    m_safetyOverlay->setColor(QColor::fromRgbF(r, g, b, 1.0));
    wakeNativePlayer(m_playerWindow);
}

void WindowManager::setSafetyLineWidth(double pixels)
{
    if (!m_safetyOverlay) return;
    m_safetyOverlay->setLineWidth(static_cast<float>(pixels));
    wakeNativePlayer(m_playerWindow);
}

void WindowManager::copyTextToClipboard(const QString &text)
{
    if (text.isEmpty()) return;
    if (auto *cb = QGuiApplication::clipboard()) {
        cb->setText(text);
    }
}

void WindowManager::openExternalPath(const QString &path)
{
    if (path.isEmpty()) return;
    // QUrl::fromLocalFile builds the right shape per platform:
    //   Windows local C:/foo → file:///C:/foo (three slashes)
    //   POSIX /foo → file:///foo
    //   Windows UNC //server/share → file://server/share
    // QDesktopServices::openUrl then defers to ShellExecuteEx /
    // open(1) / xdg-open, which launches the OS-default app for the
    // file's extension (After Effects / Premiere for .aep / .prproj
    // when installed, otherwise whatever the user has associated).
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

QString WindowManager::toNativeSeparators(const QString &path) const
{
    return QDir::toNativeSeparators(path);
}

void WindowManager::raiseUiWindow()
{
    if (!m_uiWindow) return;
    // If the user minimized us before the second-instance launch, lift
    // back to whatever non-minimized visibility we were in (Maximized
    // → Maximized, otherwise Windowed). showNormal would force-window
    // a maximized session.
    if (m_uiWindow->visibility() == QWindow::Minimized) {
        m_uiWindow->setVisibility(QWindow::AutomaticVisibility);
    }
    m_uiWindow->raise();
    m_uiWindow->requestActivate();
}

QString WindowManager::urlToOsPath(const QUrl &url) const
{
    // Three URL shapes the FileDialog / DropArea hand us:
    //   file:///C:/foo  → C:/foo                 (Windows local)
    //   file:///path    → /path                  (POSIX local)
    //   file://server/… → //server/…             (UNC; Win network)
    // QUrl::toLocalFile DOES handle UNC correctly on Qt 6 — empty
    // host case strips the leading "/" on Windows; non-empty host
    // produces "//host/share/path". Use it when QUrl is properly
    // formed; fall back to manual parsing of the string form for
    // edge cases (e.g. paths that round-tripped through plain text).
    QString path = url.toLocalFile();
    if (!path.isEmpty()) return path;

    // Manual fallback: parse the string form.
    QString s = url.toString();
    if (s.startsWith(QStringLiteral("file:///"))) {
        // Local: strip "file:///" + leading slash on Windows.
#ifdef Q_OS_WIN
        s = s.mid(8);
#else
        s = s.mid(7);
#endif
    } else if (s.startsWith(QStringLiteral("file://"))) {
        // UNC: keep the // prefix, strip "file:" only.
        s = QStringLiteral("//") + s.mid(7);
    }
    return s;
}

void WindowManager::revealInFileManager(const QString &path)
{
    if (path.isEmpty()) return;
#if defined(Q_OS_MACOS)
    // open -R selects + reveals in Finder; doesn't open the file.
    QProcess::startDetached(QStringLiteral("open"),
                            {QStringLiteral("-R"), path});
#elif defined(Q_OS_WIN)
    // explorer /select,"C:\path\to\file" highlights the file in a
    // new Explorer window. Two gotchas force us off the regular
    // startDetached(prog, args) form:
    //   1. Explorer parses /select, with a comma-glued path; the
    //      path itself needs Windows backslashes.
    //   2. QProcess auto-quotes any single arg that contains a
    //      space — turning "/select,C:\Some Path\foo" into
    //      `explorer "/select,C:\Some Path\foo"`. Explorer then
    //      can't parse the comma-form and silently opens Documents.
    //
    // Bypass the arg list via setNativeArguments so we control the
    // command-line string exactly. The path is wrapped in inner
    // quotes (`"…"`) so explorer's own parser handles spaces; the
    // /select, prefix sits outside the quotes.
    const QString winPath = QDir::toNativeSeparators(path);
    QProcess proc;
    proc.setProgram(QStringLiteral("explorer"));
    proc.setNativeArguments(
        QStringLiteral("/select,\"%1\"").arg(winPath));
    proc.startDetached();
#else
    // Linux: most file managers don't have a portable "select"
    // syntax. Open the parent directory instead — closest equivalent.
    QProcess::startDetached(QStringLiteral("xdg-open"),
                            {QFileInfo(path).absolutePath()});
#endif
}

QStringList WindowManager::availableSafetySvgs() const
{
    QStringList names;
    const QString dir = resolveSafetyAssetsDir();
    if (dir.isEmpty()) return names;
    const QStringList files = QDir(dir).entryList({QStringLiteral("*.svg")},
                                                   QDir::Files,
                                                   QDir::Name);
    names.reserve(files.size());
    for (const QString &f : files) {
        names << QFileInfo(f).completeBaseName();
    }
    return names;
}

int WindowManager::imageSeqWidth() const
{
    return m_imageSeqCache ? m_imageSeqCache->width() : 0;
}

int WindowManager::imageSeqHeight() const
{
    return m_imageSeqCache ? m_imageSeqCache->height() : 0;
}

double WindowManager::imageSeqFps() const
{
    return m_imageSeqCache ? m_imageSeqCache->fps() : 0.0;
}

void WindowManager::setHdrMode(int mode)
{
    if (mode == m_hdrMode) return;
    m_hdrMode = mode;

#ifdef QCV_NATIVE_PLAYER
    // Phase 7.5 B.3 native path: the CAMetalLayer can be
    // reconfigured live — just push the new mode to the renderer.
    // No teardown / recreate needed (no Qt scenegraph in the loop).
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        if (auto *r = pw->renderer()) {
            r->setHdrMode(static_cast<qcv::HdrMode>(mode));
        }
    }
#else
    // Qt only reads `_qt_sg_hdr_format` and QSurfaceFormat::colorSpace
    // at swapchain creation (qsgrhisupport.cpp:1536, called once
    // per cd->swapchain in qsgrenderloop.cpp:541). After first show
    // they're sticky. setVisible(false)/setVisible(true) doesn't
    // reliably destroy the swapchain — the obscurity event is
    // queued and may race with the show. The robust path is to
    // tear down the QQuickWindow entirely and recreate it with the
    // new HDR property baked in before first expose. Brief black
    // flash is expected (Guide 06 §1).
    destroyPlayerWindow();
    createPlayerWindow();
#endif

    emit hdrModeChanged();
}

void WindowManager::setBrightness(double brightness)
{
    // Clamp into a sane range. Linear (not stops). 1.0 = identity.
    if (brightness < 0.1) brightness = 0.1;
    if (brightness > 5.0) brightness = 5.0;
    if (std::abs(brightness - m_brightness) < 1e-6) return;
    m_brightness = brightness;

    // Persist immediately so the slider position survives restarts.
    QSettings().setValue(QStringLiteral("display/brightness"), m_brightness);

#ifdef QCV_NATIVE_PLAYER
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        if (auto *r = pw->renderer()) {
            r->setBrightness(static_cast<float>(m_brightness));
        }
    }
#endif

    emit brightnessChanged();
}

// ---------------------------------------------------------------------------
// Phase 3.H.5 — hover-thumbnail cache façade.
// ---------------------------------------------------------------------------

bool WindowManager::resolveHoverKey(double timelineSec,
                                     const QString &side,
                                     std::string &outPath,
                                     int &outFrame) const
{
    if (!m_timeline || !m_project) return false;

    auto encodeImageSeq = [&](const MediaItem &src, double srcTime,
                              std::string &p, int &f) -> bool {
        const auto &seq = src.imageSeq;
        if (seq.directory.isEmpty() || seq.pattern.isEmpty()
            || seq.frameRate <= 0.0) {
            return false;
        }
        // Convert timeline-relative time to a per-frame file path.
        // Frame index inside the sequence = startFrame + round(t*fps).
        int idxIn = static_cast<int>(std::round(srcTime * seq.frameRate));
        if (idxIn < 0) idxIn = 0;
        const int span = std::max(1, seq.endFrame - seq.startFrame + 1);
        if (idxIn > span - 1) idxIn = span - 1;
        const int fileFrame = seq.startFrame + idxIn;
        char fileName[1024];
        std::snprintf(fileName, sizeof(fileName),
                      seq.pattern.toStdString().c_str(), fileFrame);
        std::string full =
            seq.directory.toStdString() + "/" + fileName;
        if (!seq.layer.isEmpty()) {
            full += "?layer=" + seq.layer.toStdString();
        }
        p = std::move(full);
        f = 0;
        return true;
    };

    // Walk a track's clips for the one containing timelineSec and
    // resolve its source path + frame. Used by playlist + dual.
    auto resolveFromTrack = [&](const Track &track) -> bool {
        for (const Clip &c : track.clips) {
            if (timelineSec < c.startTime) continue;
            if (timelineSec >= c.startTime + c.duration) continue;
            const double srcTime =
                c.sourceIn + (timelineSec - c.startTime);
            if (c.mediaKind == ClipMediaKind::Video) {
                const double fps =
                    c.sourceFps > 0.0 ? c.sourceFps
                                       : m_timeline->frameRate();
                if (fps <= 0.0) return false;
                outPath  = c.mediaPath.toStdString();
                outFrame = std::max(
                    0, static_cast<int>(std::round(srcTime * fps)));
                return true;
            }
            if (c.mediaKind == ClipMediaKind::ImageSequence) {
                const MediaItem *src =
                    m_project->findItem(c.mediaItemId);
                if (!src) return false;
                return encodeImageSeq(*src, srcTime, outPath, outFrame);
            }
            return false;       // Audio-only clips: no thumbnail.
        }
        return false;
    };

    // Dual mode: route per-track. The track-B clip in the
    // controller is currently hardcoded to ClipMediaKind::Video
    // (see TimelineController::setSecondarySource), which loses
    // the image-seq distinction. So we dispatch path/encoding off
    // the IDualSource subclass (DualVideoDecoder vs
    // DualImageSeqSource), but consult the Track for this side
    // first so per-side trim / slide / slip edits map timelineSec
    // through the clip's (sourceIn, startTime, duration) window —
    // the same way playlist mode does via resolveFromTrack.
    // Without that, edits on a dual clip are silently ignored by
    // the hover thumbnail and the user sees raw source frames.
    if (m_dualController) {
        dual::IDualSource *src =
            (side == QStringLiteral("A"))
                ? m_dualController->sourceA()
                : (side == QStringLiteral("B"))
                    ? m_dualController->sourceB()
                    : nullptr;
        if (!src || !src->isOpen()) return false;

        const double sideFps = src->fps();
        const int    sideMax = src->frameCount() - 1;
        if (sideFps <= 0.0 || sideMax < 0) return false;

        // Map timelineSec → source-relative time. Default is the
        // raw passthrough (used as a fallback when no track exists
        // for this side, and a harmless no-op when the user hasn't
        // trimmed: sourceIn=0, startTime=0 → srcTime=timelineSec).
        // When a clip is found, srcTime + srcWindowEnd reflect the
        // edit, so trims clip the overlay range and slips shift the
        // displayed frame.
        double srcTime      = timelineSec;
        double srcWindowEnd =
            static_cast<double>(sideMax + 1) / sideFps;

        const Timeline &tl = m_timeline->timeline();
        const Track *sideTrack = nullptr;
        for (const Track &t : tl.tracks) {
            if (t.id == side) { sideTrack = &t; break; }
        }
        if (sideTrack && !sideTrack->clips.isEmpty()) {
            const Clip *hit = nullptr;
            for (const Clip &c : sideTrack->clips) {
                if (timelineSec < c.startTime) continue;
                if (timelineSec >= c.startTime + c.duration) continue;
                hit = &c;
                break;
            }
            // Outside any clip on this side (gap, or past trim) —
            // return false so the corner overlay clears. Matches
            // playlist-mode behaviour.
            if (!hit) return false;
            srcTime      = hit->sourceIn + (timelineSec - hit->startTime);
            srcWindowEnd = hit->sourceIn + hit->duration;
        }
        // Past this side's window — same return-false-to-clear
        // contract. With a clip, this is past the trim; without
        // one, past the source's natural duration (mismatched A/B
        // lengths leave a gap on the shorter side).
        if (srcTime >= srcWindowEnd) return false;

        if (auto *seq = dynamic_cast<dual::DualImageSeqSource *>(src)) {
            int frame = static_cast<int>(
                std::round(srcTime * sideFps));
            if (frame < 0) frame = 0;
            if (frame > sideMax) frame = sideMax;
            QString filePath = seq->frameFilePath(frame);
            if (filePath.isEmpty()) return false;
            QString full = filePath;
            if (!seq->layer().isEmpty()) {
                full += QStringLiteral("?layer=") + seq->layer();
            }
            outPath  = full.toStdString();
            outFrame = 0;
            return true;
        }

        // Video (or any non-image-seq IDualSource) — use the
        // source path + frame-number-as-string convention that
        // VideoImageLoader expects.
        int frame = static_cast<int>(
            std::round(srcTime * sideFps));
        if (frame < 0) frame = 0;
        if (frame > sideMax) frame = sideMax;
        outPath  = src->path().toStdString();
        outFrame = frame;
        return true;
    }

    // Non-dual: only "A" exists.
    if (side != QStringLiteral("A")) return false;

    // Playlist mode: the active item is a playlist; resolve to the
    // clip under timelineSec, then dispatch by clip kind.
    if (m_playlistActive) {
        const Timeline &t = m_timeline->timeline();
        if (t.tracks.isEmpty()) return false;
        return resolveFromTrack(t.tracks.first());
    }

    // Single-mode: route by the active item's type.
    const QString itemId = m_project->activeItemId();
    if (itemId.isEmpty()) return false;
    const MediaItem *it = m_project->findItem(itemId);
    if (!it) return false;

    if (it->type == MediaType::Video || it->type == MediaType::Image) {
        const double fps = m_timeline->frameRate();
        if (fps <= 0.0) return false;
        outPath  = m_videoDecoder ?
                    m_videoDecoder->sourcePath().toStdString() :
                    std::string();
        if (outPath.empty()) outPath = it->path.toStdString();
        outFrame = std::max(
            0, static_cast<int>(std::round(timelineSec * fps)));
        return !outPath.empty();
    }
    if (it->type == MediaType::ImageSequence) {
        return encodeImageSeq(*it, timelineSec, outPath, outFrame);
    }
    return false;
}

void WindowManager::requestHoverThumbnail(double timelineSec)
{
    if (!m_thumbCache || !m_thumbCache->isEnabled()) return;
    if (isPlayingUnified()) {
        clearHoverThumbnail();
        return;
    }

    auto requestSide = [&](const QString &side,
                           std::string &outPath, int &outFrame,
                           std::atomic<std::uint64_t> &handleOut,
                           std::atomic<int> &wOut,
                           std::atomic<int> &hOut) {
        std::string path;
        int frame = 0;
        if (!resolveHoverKey(timelineSec, side, path, frame)) {
            outPath.clear();
            outFrame = 0;
            handleOut.store(0, std::memory_order_release);
            wOut.store(0, std::memory_order_relaxed);
            hOut.store(0, std::memory_order_relaxed);
            return;
        }
        outPath  = path;
        outFrame = frame;
        int w = 0, h = 0;
        const std::uint64_t handle = m_thumbCache->getThumbnail(
            path, frame, w, h, /*allow_fallback=*/true);
        handleOut.store(handle, std::memory_order_release);
        wOut.store(w, std::memory_order_relaxed);
        hOut.store(h, std::memory_order_relaxed);
    };

    requestSide(QStringLiteral("A"),
                m_hoverPath, m_hoverFrame,
                m_currentHoverThumbHandle,
                m_currentHoverThumbW, m_currentHoverThumbH);

    // Dual mode: also request B. Non-dual leaves B atomics zeroed,
    // so the renderer's BR overlay simply doesn't draw.
    if (m_dualController) {
        requestSide(QStringLiteral("B"),
                    m_hoverPathB, m_hoverFrameB,
                    m_currentHoverThumbHandleB,
                    m_currentHoverThumbWB, m_currentHoverThumbHB);
    } else {
        m_hoverPathB.clear();
        m_hoverFrameB = 0;
        m_currentHoverThumbHandleB.store(0, std::memory_order_release);
        m_currentHoverThumbWB.store(0, std::memory_order_relaxed);
        m_currentHoverThumbHB.store(0, std::memory_order_relaxed);
    }

    m_hoverActive = !m_hoverPath.empty() || !m_hoverPathB.empty();
    pushHoverThumbToRenderer();
}

void WindowManager::clearHoverThumbnail()
{
    m_hoverActive = false;
    m_currentHoverThumbHandle.store(0, std::memory_order_release);
    m_currentHoverThumbW.store(0, std::memory_order_relaxed);
    m_currentHoverThumbH.store(0, std::memory_order_relaxed);
    m_currentHoverThumbHandleB.store(0, std::memory_order_release);
    m_currentHoverThumbWB.store(0, std::memory_order_relaxed);
    m_currentHoverThumbHB.store(0, std::memory_order_relaxed);
    m_hoverPathB.clear();
    m_hoverFrameB = 0;
    pushHoverThumbToRenderer();
}

void WindowManager::toast(const QString &message, int kind)
{
    emit toastRequested(message, kind);
}

void WindowManager::toastAction(const QString &message, int kind,
                                const QString &actionLabel,
                                const QString &actionId)
{
    emit toastActionRequested(message, kind, actionLabel, actionId);
}

void WindowManager::invokeToastAction(const QString &actionId)
{
    if (actionId == QLatin1String("undo-media-delete")) {
        const QStringList names =
            m_project ? m_project->restoreLastRemovedItems()
                      : QStringList{};
        if (names.isEmpty()) {
            // Buffer gone — project switched under the toast, or a
            // double-fire. Rare; say so rather than silently no-op.
            toast(tr("Nothing to restore"), 1);
        } else if (names.size() == 1) {
            toast(tr("Restored %1").arg(names.first()), 0);
        } else {
            toast(tr("Restored %1 items").arg(names.size()), 0);
        }
    } else if (actionId == QLatin1String("undo-preset-delete")) {
        const QString name =
            m_presets ? m_presets->undoDeleteLast() : QString();
        if (name.isEmpty()) {
            toast(tr("Nothing to restore"), 1);
        } else {
            toast(tr("Preset restored: %1").arg(name), 0);
        }
    } else {
        qWarning("WindowManager::invokeToastAction: unknown action '%s'",
                 qPrintable(actionId));
    }
}

void WindowManager::dropTextInputFocus()
{
    QObject *focused = QGuiApplication::focusObject();
    if (!focused) return;
    // Same property-pair detection the global key filter uses to
    // identify a text-editing item. Catches QQuickTextInput /
    // QQuickTextEdit / TextField / TextArea / the inner editor of
    // an editing FlatSpinBox; deliberately doesn't match buttons,
    // closed combo boxes, sliders, plain Items, etc., so we only
    // clear focus when it would actually be swallowing keystrokes.
    const bool isTextItem =
        focused->property("cursorPosition").isValid()
        && focused->property("selectionStart").isValid();
    if (!isTextItem) return;
    if (auto *qitem = qobject_cast<QQuickItem *>(focused)) {
        qitem->setFocus(false, Qt::OtherFocusReason);
    } else {
        focused->setProperty("focus", false);
    }
}

void WindowManager::pushHoverThumbToRenderer()
{
#ifdef QCV_NATIVE_PLAYER
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        if (auto *r = pw->renderer()) {
            r->setHoverThumbnail(
                static_cast<unsigned long long>(
                    m_currentHoverThumbHandle.load(
                        std::memory_order_acquire)),
                m_currentHoverThumbW.load(std::memory_order_relaxed),
                m_currentHoverThumbH.load(std::memory_order_relaxed));
            r->setHoverThumbnailB(
                static_cast<unsigned long long>(
                    m_currentHoverThumbHandleB.load(
                        std::memory_order_acquire)),
                m_currentHoverThumbWB.load(std::memory_order_relaxed),
                m_currentHoverThumbHB.load(std::memory_order_relaxed));
            r->requestUpdate();
        }
    }
#endif
}

// ---------------------------------------------------------------------------
// Phase 3.H.6 — annotation backbone wiring (Stage A).
// ---------------------------------------------------------------------------

bool WindowManager::annotationsAllowedForCurrentMedia() const
{
    // No dual mode (per-side annotation context is ambiguous).
    if (m_dualController) return false;
    // No playlist (per-clip notes get scrambled across clip moves /
    // reorders / trims; needs a separate design).
    if (m_playlistActive) return false;
    if (!m_project) return false;
    const QString itemId = m_project->activeItemId();
    if (itemId.isEmpty()) return false;
    const MediaItem *it = m_project->findItem(itemId);
    if (!it) return false;
    return it->type == MediaType::Video
        || it->type == MediaType::ImageSequence;
}

void WindowManager::syncAnnotationManagerToActiveMedia()
{
    if (!m_annotationManager) return;
    if (!annotationsAllowedForCurrentMedia()) {
        // No saving — clearNotes wipes in-memory state without
        // touching disk. Critical: drops the prior media's notes
        // before they can leak into the new media's sidecar.
        m_annotationManager->clearNotes();
        // Bail any in-progress annotation mode; the QML toggle is
        // already hidden but the Q_INVOKABLE state needs syncing.
        if (m_annotator) m_annotator->setMode(ViewportMode::Playback);
        rebuildStoredAnnotationMesh();
        return;
    }
    if (!m_project) return;
    const QString itemId = m_project->activeItemId();
    const MediaItem *it = itemId.isEmpty() ? nullptr
                                            : m_project->findItem(itemId);
    if (!it) {
        m_annotationManager->clearNotes();
        rebuildStoredAnnotationMesh();
        return;
    }
    // For image sequences, the "media path" is the directory (the
    // sidecar layout was designed around that — `.qcview/<dir-name>/`).
    // For video / images, it's the file path. Old app uses the same
    // routing (annotation_io.cpp:37).
    const QString mediaPath =
        (it->type == MediaType::ImageSequence
         && !it->imageSeq.directory.isEmpty())
            ? it->imageSeq.directory
            : it->path;
    if (mediaPath.isEmpty()) {
        m_annotationManager->clearNotes();
        rebuildStoredAnnotationMesh();
        return;
    }
    // loadNotesForMedia both stores the path AND reads the
    // sidecar (.qcview/<media>/notes.json). setMediaPath alone
    // would just stash the path without populating notes — that
    // was the Stage A bug where opening a video skipped over any
    // notes the user (or the old app) had previously saved.
    m_annotationManager->loadNotesForMedia(mediaPath);
    // notesChanged → rebuildStoredAnnotationMesh fires from the
    // signal hookup in the ctor.

    // Self-heal stale `_annotated` thumbnails: older projects (and any
    // notes captured before the pixel-aspect feature) have square
    // `_annotated` PNGs. If this clip is anamorphic, re-derive them from
    // the clean frames at the current aspect. Square clips (PAR 1:1)
    // already have correct square thumbnails, so skip the churn.
    {
        const auto par = effectivePixelAspectFor(*it);
        if (par.first != par.second) regenerateNoteThumbnailsForActiveClip();
    }
}

void WindowManager::onStrokeFinalized(std::unique_ptr<ActiveStroke> stroke)
{
    if (!stroke || !m_annotationManager || !m_annotator) return;
    if (!annotationsAllowedForCurrentMedia()) return;

    const int frame = currentFrameUnified();
    const double fps = m_timeline ? m_timeline->frameRate() : 24.0;
    const double timestampSec = (fps > 0.0) ? frame / fps : 0.0;

    // SMPTE timecode — used as the note's primary key. Match the
    // old app's HH:MM:SS:FF format. TimecodeFormatter handles DF
    // for NTSC fps; non-DF rates fall through fine.
    TimecodeFormatter tc(static_cast<int>(std::round(fps * 1000.0)),
                          1000, /*dropFrame=*/false);
    const QString timecode = tc.isValid() ? tc.format(frame)
                                            : QStringLiteral("00:00:00:00");

    // Add the note (no-op if one already exists at that timecode —
    // AnnotationManager dedups). Then merge the new stroke into the
    // note's existing strokes list and reserialize.
    AnnotationNote *existing =
        m_annotationManager->noteAtTimecode(timecode);
    const bool freshNote = (existing == nullptr);

    // Capture the pre-edit state for undo BEFORE we mutate.
    // priorAnnotationData is the strokes-JSON the note had before
    // this stroke was added; empty when the note was newly
    // created (undo deletes the note in that case).
    AnnotationUndoEntry undoEntry;
    undoEntry.timecode = timecode;
    undoEntry.priorAnnotationData =
        existing ? existing->annotation_data : QString();
    undoEntry.noteWasNew = freshNote;
    m_annotationUndoStack.append(undoEntry);
    m_annotationRedoStack.clear();   // any new edit invalidates redo

    if (freshNote) {
        m_annotationManager->addNote(timestampSec, timecode, frame,
                                      QString());
        existing = m_annotationManager->noteAtTimecode(timecode);
        if (!existing) {
            m_annotationUndoStack.removeLast(); // bail cleanly
            return;
        }
    }

    // Clean baseline FIRST — before updateNoteAnnotationData
    // pushes the just-finalized stroke into the renderer's stored
    // mesh. captureScreenshot at this moment grabs a frame that
    // does NOT contain this stroke (it was popped from
    // active_stroke_ a few lines back; nothing else has it yet).
    // saveNoteCleanThumbnail is idempotent so subsequent strokes
    // on the same note don't clobber the original baseline.
    saveNoteCleanThumbnail(timecode);

    std::vector<ActiveStroke> strokes;
    if (!existing->annotation_data.isEmpty()) {
        strokes = AnnotationSerializer::jsonStringToStrokes(
            existing->annotation_data);
    }
    strokes.push_back(*stroke);
    const QString json =
        AnnotationSerializer::strokesToJsonString(strokes);
    m_annotationManager->updateNoteAnnotationData(timecode, json);
    // notesChanged → rebuildStoredAnnotationMesh + autosave fire
    // synchronously from inside AnnotationManager. The renderer's
    // m_storedStrokes now includes this stroke, so the next
    // captureScreenshot frame will have it baked in.
    //
    // Debounce the annotated-thumbnail capture rather than
    // running it inline — on a multi-stroke flurry we'd block
    // the GUI thread up to 250 ms per release waiting for the
    // capture's render fence. The timer fires ~400 ms after the
    // user stops drawing; one save covers the whole burst, and
    // the captured frame has every stroke baked in by then.
    m_pendingAnnotatedTimecode = timecode;
    if (m_annotatedSaveTimer) m_annotatedSaveTimer->start();
}

void WindowManager::onEraseAt(QPointF normalizedPos)
{
    if (!m_annotationManager) return;
    if (!annotationsAllowedForCurrentMedia()) return;
    const int frame = currentFrameUnified();

    // Find a note at the current frame whose stroke list contains
    // a hit. Bounding-box hit-test with a normalized tolerance
    // (≈ 19 px on a 1920-wide source). Cheap, forgiving, doesn't
    // need shape-specific math.
    auto notes = m_annotationManager->notes();
    for (const auto &n : notes) {
        if (n.frame != frame) continue;
        if (n.annotation_data.isEmpty()) continue;
        auto strokes =
            AnnotationSerializer::jsonStringToStrokes(n.annotation_data);
        const double tol = 0.012;       // normalized
        int hitIdx = -1;
        for (int i = static_cast<int>(strokes.size()) - 1;
             i >= 0; --i) {
            const auto &s = strokes[i];
            if (s.points.empty()) continue;
            double minX = s.points.front().x();
            double maxX = minX;
            double minY = s.points.front().y();
            double maxY = minY;
            for (const QPointF &p : s.points) {
                minX = std::min(minX, p.x());
                maxX = std::max(maxX, p.x());
                minY = std::min(minY, p.y());
                maxY = std::max(maxY, p.y());
            }
            const double pad = std::max(tol,
                static_cast<double>(s.strokeWidth) / 1920.0);
            if (normalizedPos.x() >= minX - pad
                && normalizedPos.x() <= maxX + pad
                && normalizedPos.y() >= minY - pad
                && normalizedPos.y() <= maxY + pad) {
                hitIdx = i;
                break;
            }
        }
        if (hitIdx < 0) continue;

        // Push undo entry, then commit.
        AnnotationUndoEntry undo;
        undo.timecode             = n.timecode;
        undo.priorAnnotationData  = n.annotation_data;
        undo.noteWasNew           = false;
        m_annotationUndoStack.append(undo);
        m_annotationRedoStack.clear();

        strokes.erase(strokes.begin() + hitIdx);
        const QString json = strokes.empty()
            ? QString()
            : AnnotationSerializer::strokesToJsonString(strokes);
        m_annotationManager->updateNoteAnnotationData(n.timecode, json);
        // Refresh the annotated thumb via the same debounce path
        // stroke-add uses so an erase sweep doesn't fire one save
        // per hit.
        m_pendingAnnotatedTimecode = n.timecode;
        if (m_annotatedSaveTimer) m_annotatedSaveTimer->start();
        return;        // one stroke per pointer event
    }
}

bool WindowManager::annotationUndo()
{
    if (!m_annotationManager) return false;
    if (m_annotationUndoStack.isEmpty()) return false;
    AnnotationUndoEntry e = m_annotationUndoStack.takeLast();

    AnnotationUndoEntry redo;
    redo.timecode = e.timecode;
    if (const AnnotationNote *cur =
            m_annotationManager->noteAtTimecode(e.timecode)) {
        redo.priorAnnotationData = cur->annotation_data;
        redo.noteWasNew = false;
    } else {
        redo.priorAnnotationData = QString();
        redo.noteWasNew = true;
    }
    m_annotationRedoStack.append(redo);

    if (e.noteWasNew) {
        m_annotationManager->deleteNote(e.timecode);
    } else {
        m_annotationManager->updateNoteAnnotationData(
            e.timecode, e.priorAnnotationData);
    }
    m_pendingAnnotatedTimecode = e.timecode;
    if (m_annotatedSaveTimer) m_annotatedSaveTimer->start();
    return true;
}

bool WindowManager::annotationRedo()
{
    if (!m_annotationManager) return false;
    if (m_annotationRedoStack.isEmpty()) return false;
    AnnotationUndoEntry e = m_annotationRedoStack.takeLast();

    AnnotationUndoEntry undo;
    undo.timecode = e.timecode;
    if (const AnnotationNote *cur =
            m_annotationManager->noteAtTimecode(e.timecode)) {
        undo.priorAnnotationData = cur->annotation_data;
        undo.noteWasNew = false;
    } else {
        undo.priorAnnotationData = QString();
        undo.noteWasNew = true;
    }
    m_annotationUndoStack.append(undo);

    if (e.noteWasNew) {
        // Redo'ing an undo-of-newly-created-note: recreate it.
        const double fps = m_timeline ? m_timeline->frameRate() : 24.0;
        TimecodeFormatter tc(static_cast<int>(std::round(fps * 1000.0)),
                              1000, /*dropFrame=*/false);
        const int frame = tc.isValid() ? tc.parse(e.timecode) : 0;
        const double ts = (fps > 0.0) ? frame / fps : 0.0;
        m_annotationManager->addNote(ts, e.timecode, frame, QString());
        if (!e.priorAnnotationData.isEmpty()) {
            m_annotationManager->updateNoteAnnotationData(
                e.timecode, e.priorAnnotationData);
        }
    } else {
        m_annotationManager->updateNoteAnnotationData(
            e.timecode, e.priorAnnotationData);
    }
    m_pendingAnnotatedTimecode = e.timecode;
    if (m_annotatedSaveTimer) m_annotatedSaveTimer->start();
    return true;
}

void WindowManager::cancelActiveStroke()
{
    if (m_annotator) m_annotator->cancelActiveStroke();
}

QVariantList WindowManager::notesList() const
{
    QVariantList out;
    if (!m_annotationManager || !annotationsAllowedForCurrentMedia()) {
        return out;
    }
    const QString imagesDir = m_annotationManager->imagesFolder();
    const auto notes = m_annotationManager->notes();
    out.reserve(static_cast<int>(notes.size()));
    for (const auto &n : notes) {
        QVariantMap m;
        m[QStringLiteral("timecode")]         = n.timecode;
        m[QStringLiteral("frame")]            = n.frame;
        m[QStringLiteral("timestampSeconds")] = n.timestamp_seconds;
        m[QStringLiteral("text")]             = n.text;
        m[QStringLiteral("addressed")]        = n.addressed;
        m[QStringLiteral("hasStrokes")]       = !n.annotation_data.isEmpty();
        // image_path is sidecar-relative ("images/note_<TC>.png");
        // join with the absolute images folder so QML's Image item
        // can load it directly. We expose two paths:
        //   - cleanImagePath: the bare-frame baseline (export
        //     pipelines + undo/redo regenerate from this).
        //   - imagePath: prefers the "_annotated.png" sibling when
        //     present; falls back to the clean PNG. This is what
        //     the panel card displays so notes with strokes show
        //     the strokes baked in.
        QString cleanAbs;
        if (!n.image_path.isEmpty() && !imagesDir.isEmpty()) {
            const QString trimmed =
                n.image_path.startsWith(QStringLiteral("images/"))
                    ? n.image_path.mid(QStringLiteral("images/").size())
                    : n.image_path;
            cleanAbs = imagesDir + QLatin1Char('/') + trimmed;
        }
        QString annotatedAbs;
        if (!cleanAbs.isEmpty()) {
            // Insert "_annotated" before ".png".
            const QString suffix = QStringLiteral(".png");
            if (cleanAbs.endsWith(suffix, Qt::CaseInsensitive)) {
                annotatedAbs = cleanAbs.left(cleanAbs.size()
                                              - suffix.size())
                              + QStringLiteral("_annotated") + suffix;
                if (!QFileInfo::exists(annotatedAbs)) {
                    annotatedAbs.clear();
                }
            }
        }
        // Cache-buster: when an annotated thumb is regenerated
        // (e.g. user adds another stroke), the absolute path is
        // unchanged — QML's Image item with cache:false still
        // dedups by URL string. Append the file mtime as a query
        // suffix so each rewrite gives a new URL and forces a
        // re-fetch.
        auto withMtimeStamp = [](const QString &abs) -> QString {
            if (abs.isEmpty()) return abs;
            const QFileInfo fi(abs);
            if (!fi.exists()) return abs;
            return abs + QStringLiteral("?ts=")
                 + QString::number(
                     fi.lastModified().toMSecsSinceEpoch());
        };
        m[QStringLiteral("cleanImagePath")] = cleanAbs;
        m[QStringLiteral("imagePath")]      = withMtimeStamp(
            annotatedAbs.isEmpty() ? cleanAbs : annotatedAbs);
        out.append(m);
    }
    return out;
}

void WindowManager::addNoteAtCurrentFrame()
{
    if (!m_annotationManager || !annotationsAllowedForCurrentMedia()) return;

    const int frame = currentFrameUnified();
    const double fps = m_timeline ? m_timeline->frameRate() : 24.0;
    const double timestampSec = (fps > 0.0) ? frame / fps : 0.0;

    TimecodeFormatter tc(static_cast<int>(std::round(fps * 1000.0)),
                          1000, /*dropFrame=*/false);
    const QString timecode = tc.isValid() ? tc.format(frame)
                                            : QStringLiteral("00:00:00:00");

    AnnotationNote *existing =
        m_annotationManager->noteAtTimecode(timecode);
    if (!existing) {
        m_annotationManager->addNote(timestampSec, timecode, frame,
                                      QString());
    }
    // No strokes yet → only the clean thumbnail matters here.
    saveNoteCleanThumbnail(timecode);
}

namespace {
// Filesystem-safe stem for a timecode. Replaces `:` (HH:MM:SS:FF)
// and `;` (drop-frame separator) with `_`. The timecode stays
// canonical in the JSON; only filenames swap separators.
QString timecodeFileStem(const QString &timecode) {
    return QString(timecode)
        .replace(QLatin1Char(':'), QLatin1Char('_'))
        .replace(QLatin1Char(';'), QLatin1Char('_'));
}
} // namespace

void WindowManager::saveNoteCleanThumbnail(const QString &timecode)
{
    if (!m_annotationManager) return;
#ifdef QCV_NATIVE_PLAYER
    const QString imagesDir = m_annotationManager->imagesFolder();
    if (imagesDir.isEmpty()) return;
    const QString stem = timecodeFileStem(timecode);
    const QString rel  = QStringLiteral("images/note_%1.png").arg(stem);
    const QString abs  = imagesDir + QLatin1Char('/')
                       + QStringLiteral("note_%1.png").arg(stem);

    // Idempotent — once we have a clean baseline don't overwrite
    // it. Old-app behavior; the clean PNG is the source of truth
    // for regenerating the annotated overlay after edits / undo /
    // redo, so re-capturing later (with potentially different
    // background or post-state) would corrupt the baseline.
    if (QFileInfo::exists(abs)) {
        // Make sure note.image_path points at the clean PNG even
        // if it was set to something else by an earlier code path.
        m_annotationManager->updateNoteImagePath(timecode, rel);
        return;
    }

    auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data());
    if (!pw || !pw->renderer()) {
        qWarning("saveNoteCleanThumbnail: no player/renderer");
        return;
    }
    const QImage img = pw->renderer()->captureScreenshot();
    if (img.isNull()) {
        qWarning("saveNoteCleanThumbnail: captureScreenshot returned empty QImage");
        return;
    }
    qInfo("saveNoteCleanThumbnail: %dx%d → %s", img.width(), img.height(),
          qPrintable(abs));

    // Keep the just-captured clean frame in memory so the annotated
    // recomposite (fired ~400 ms later) can proceed even if the async PNG
    // write below hasn't reached disk yet. Captured only on a NEW clean
    // (this function is idempotent), which is exactly the racy first-stroke.
    m_lastCleanThumbImage = img;
    m_lastCleanThumbTc    = timecode;

    QDir().mkpath(imagesDir);
    // Offload PNG encode + write to a worker thread so the GUI
    // thread isn't blocked between strokes (HD encode runs ~100 ms).
    // Defer updateNoteImagePath (which fires notesChanged → QML
    // re-binds Image.source) until AFTER the write completes —
    // otherwise QML's async PNG decoder hits the file mid-write on
    // slow disks (e.g. mapped SMB share) and fails with "Unable to
    // read image data". On local SSDs the race is tight enough to
    // not fire, which is why macOS doesn't notice.
    const QString tc = timecode;
    QtConcurrent::run([this, img, abs, tc, rel]() {
        const bool ok = img.save(abs, "PNG");
        if (!ok) {
            qWarning("WindowManager: clean thumb save failed: %s",
                     qPrintable(abs));
            return;
        }
        qInfo("saveNoteCleanThumbnail: write OK %s", qPrintable(abs));
        QMetaObject::invokeMethod(this, [this, tc, rel] {
            if (m_annotationManager)
                m_annotationManager->updateNoteImagePath(tc, rel);
        }, Qt::QueuedConnection);
    });
#else
    Q_UNUSED(timecode);
#endif
}

void WindowManager::saveNoteAnnotatedThumbnail(const QString &timecode)
{
    if (!m_annotationManager) return;
#ifdef QCV_NATIVE_PLAYER
    const QString imagesDir = m_annotationManager->imagesFolder();
    if (imagesDir.isEmpty()) return;

    const QString stem     = timecodeFileStem(timecode);
    const QString cleanAbs = imagesDir + QLatin1Char('/')
                           + QStringLiteral("note_%1.png").arg(stem);
    const QString abs      = imagesDir + QLatin1Char('/')
                           + QStringLiteral("note_%1_annotated.png").arg(stem);

    // Recomposite from the durable square clean frame — NOT a fresh GPU
    // capture. This makes the annotated thumbnail match the un-squeezed
    // viewport (pixel aspect applied) with uniform stroke thickness, and
    // lets it be regenerated later (e.g. on a PAR change) without a decode.
    QImage clean(cleanAbs);
    if (clean.isNull() && m_lastCleanThumbTc == timecode
        && !m_lastCleanThumbImage.isNull()) {
        // Clean write may still be in flight right after the first stroke.
        clean = m_lastCleanThumbImage;
    }
    if (clean.isNull()) {
        qWarning("saveNoteAnnotatedThumbnail: clean frame not ready for %s",
                 qPrintable(timecode));
        return;   // retry on the next stroke / regen trigger
    }

    std::vector<ActiveStroke> strokes;
    if (const AnnotationNote *n = m_annotationManager->noteAtTimecode(timecode)) {
        strokes = AnnotationSerializer::jsonStringToStrokes(n->annotation_data);
    }

    int parNum = 1, parDen = 1;
    if (m_project) {
        if (const MediaItem *it =
                m_project->findItem(audioRoutingScopeMediaItemId())) {
            const auto par = effectivePixelAspectFor(*it);
            parNum = par.first;
            parDen = par.second;
        }
    }

    const QImage img = renderNoteThumbnail(clean, strokes, parNum, parDen);
    if (img.isNull()) return;
    qInfo("saveNoteAnnotatedThumbnail: %dx%d → %s", img.width(), img.height(),
          qPrintable(abs));

    QDir().mkpath(imagesDir);
    // Offload PNG encode + write to a worker thread. annotatedThumbReady
    // fires on the GUI thread once the file is on disk so NotesPanel's
    // refresh sees the just-saved PNG, not the prior one. Also emit
    // notesChanged so notesList re-resolves imagePath to annotatedAbs.
    const QString tc = timecode;
    QtConcurrent::run([this, img, abs, tc]() {
        if (!img.save(abs, "PNG")) {
            qWarning("WindowManager: annotated thumb save failed: %s",
                     qPrintable(abs));
            return;
        }
        QMetaObject::invokeMethod(this, [this, tc] {
            emit notesChanged();        // forces notesList re-eval
            emit annotatedThumbReady(tc);
        }, Qt::QueuedConnection);
    });
    // image_path stays pointed at the clean PNG; the annotated
    // sibling is discovered by NotesPanel via filename convention.
#else
    Q_UNUSED(timecode);
#endif
}

void WindowManager::regenerateNoteThumbnailsForActiveClip()
{
#ifdef QCV_NATIVE_PLAYER
    if (!m_annotationManager) return;
    const QString imagesDir = m_annotationManager->imagesFolder();
    if (imagesDir.isEmpty()) return;

    int parNum = 1, parDen = 1;
    if (m_project) {
        if (const MediaItem *it =
                m_project->findItem(audioRoutingScopeMediaItemId())) {
            const auto par = effectivePixelAspectFor(*it);
            parNum = par.first;
            parDen = par.second;
        }
    }

    const std::vector<AnnotationNote> notes = m_annotationManager->notes();
    if (notes.empty()) return;

    // All image work off the GUI thread. Each note recomposites from its own
    // durable square clean PNG, so this needs no decoder and no current
    // frame — every note on the clip re-derives to the new aspect at once.
    QtConcurrent::run([this, notes, imagesDir, parNum, parDen]() {
        for (const AnnotationNote &n : notes) {
            const QString stem = timecodeFileStem(n.timecode);
            const QImage clean(imagesDir + QLatin1Char('/')
                               + QStringLiteral("note_%1.png").arg(stem));
            if (clean.isNull()) continue;   // legacy note with no clean source
            const std::vector<ActiveStroke> strokes =
                AnnotationSerializer::jsonStringToStrokes(n.annotation_data);
            const QImage img = renderNoteThumbnail(clean, strokes, parNum, parDen);
            if (img.isNull()) continue;
            img.save(imagesDir + QLatin1Char('/')
                     + QStringLiteral("note_%1_annotated.png").arg(stem), "PNG");
        }
        QMetaObject::invokeMethod(this, [this] {
            emit notesChanged();
        }, Qt::QueuedConnection);
    });
#endif
}

void WindowManager::updateAnnotationNoteText(const QString &timecode,
                                              const QString &text)
{
    if (!m_annotationManager) return;
    m_annotationManager->updateNoteText(timecode, text);
}

void WindowManager::updateAnnotationNoteAddressed(const QString &timecode,
                                                    bool addressed)
{
    if (!m_annotationManager) return;
    m_annotationManager->updateNoteAddressed(timecode, addressed);
}

void WindowManager::deleteAnnotationNote(const QString &timecode)
{
    if (!m_annotationManager) return;

    // Wipe both thumbnails (clean baseline + annotated overlay)
    // BEFORE the JSON entry goes away. Image deletion is
    // intentionally only done here — the user-click delete path —
    // and not inside AnnotationManager::deleteNote, which is also
    // reached by annotationUndo() (where the PNGs must survive so
    // a subsequent redo can restore them).
    const QString imagesDir = m_annotationManager->imagesFolder();
    if (!imagesDir.isEmpty()) {
        const QString stem = timecodeFileStem(timecode);
        const QString cleanAbs = imagesDir + QLatin1Char('/')
                               + QStringLiteral("note_%1.png").arg(stem);
        const QString annotAbs = imagesDir + QLatin1Char('/')
                               + QStringLiteral("note_%1_annotated.png").arg(stem);
        // Best-effort: silent on missing files (fresh notes that
        // never got an annotated capture won't have the sibling).
        if (QFileInfo::exists(cleanAbs) && !QFile::remove(cleanAbs)) {
            qWarning("deleteAnnotationNote: failed to remove %s",
                     qPrintable(cleanAbs));
        }
        if (QFileInfo::exists(annotAbs) && !QFile::remove(annotAbs)) {
            qWarning("deleteAnnotationNote: failed to remove %s",
                     qPrintable(annotAbs));
        }
    }

    m_annotationManager->deleteNote(timecode);
}

void WindowManager::seekToAnnotationNote(const QString &timecode)
{
    if (!m_annotationManager) return;
    const AnnotationNote *n = m_annotationManager->noteAtTimecode(timecode);
    if (!n) return;
    seekToFrame(n->frame);
}

bool WindowManager::exportAnnotationNotes(const QString &format,
                                           const QString &path,
                                           const QString &baseName)
{
    if (!m_annotationManager) return false;
    if (!m_annotationManager->hasNotes()) return false;
    if (path.isEmpty()) return false;

    AnnotationExporter exp(this);
    exp.setManager(m_annotationManager.get());
    // mediaName: prefer the active item's display name; fall back
    // to the path's basename. mediaPath: file path (or sequence
    // dir for image-seq) — used as a metadata line in exports.
    QString mediaName, mediaPath;
    if (m_project) {
        const QString itemId = m_project->activeItemId();
        if (const MediaItem *it = itemId.isEmpty()
                                    ? nullptr
                                    : m_project->findItem(itemId)) {
            mediaName = it->name;
            mediaPath = (it->type == MediaType::ImageSequence
                          && !it->imageSeq.directory.isEmpty())
                         ? it->imageSeq.directory
                         : it->path;
        }
    }
    if (mediaName.isEmpty()) mediaName = QFileInfo(mediaPath).baseName();
    exp.setMediaName(mediaName);
    exp.setMediaPath(mediaPath);

    const QString fmt = format.toLower();
    if (fmt == QStringLiteral("markdown") || fmt == QStringLiteral("md")) {
        return exp.exportMarkdown(path, baseName);
    }

    // Normalize the file path for single-file formats. Qt's QML
    // FileDialog reuses its selectedFile across opens, so flipping
    // format mid-session can produce paths like "MyName.html.pdf"
    // when the user named an HTML export earlier and then chose
    // PDF. Strip a stale single-format extension before re-adding
    // the right one.
    auto normalizeExt = [](QString p, const QString &target) {
        static const QStringList knownExts = {
            QStringLiteral(".html"),
            QStringLiteral(".md"),
            QStringLiteral(".pdf"),
            QStringLiteral(".docx"),
        };
        if (p.endsWith(target, Qt::CaseInsensitive)) {
            // Strip a stale inner extension between stem and target.
            QString stem = p.left(p.size() - target.size());
            for (const QString &e : knownExts) {
                if (e.compare(target, Qt::CaseInsensitive) == 0) continue;
                if (stem.endsWith(e, Qt::CaseInsensitive)) {
                    stem = stem.left(stem.size() - e.size());
                    break;
                }
            }
            return stem + target;
        }
        // Strip any stale extension, then append the target.
        for (const QString &e : knownExts) {
            if (e.compare(target, Qt::CaseInsensitive) == 0) continue;
            if (p.endsWith(e, Qt::CaseInsensitive)) {
                p = p.left(p.size() - e.size());
                break;
            }
        }
        return p + target;
    };

    if (fmt == QStringLiteral("html")) {
        return exp.exportHtml(normalizeExt(path, QStringLiteral(".html")));
    }
    if (fmt == QStringLiteral("pdf")) {
        return exp.exportPdf(normalizeExt(path, QStringLiteral(".pdf")));
    }
    if (fmt == QStringLiteral("docx")) {
        return exp.exportDocx(normalizeExt(path, QStringLiteral(".docx")));
    }
    qWarning("WindowManager: unknown export format '%s'",
             qPrintable(format));
    return false;
}

void WindowManager::rebuildStoredAnnotationMesh()
{
    // Collect the strokes for the current frame. Renderer
    // tessellates per draw using its own dst size; we just hand it
    // the source-coordinate (normalized) ActiveStroke list. Gated
    // on the notes-panel visibility — when the panel is closed we
    // push an empty list so the player surface stays clean.
    std::vector<ActiveStroke> strokesForFrame;
    if (m_notesPanelVisible
        && m_annotationManager
        && annotationsAllowedForCurrentMedia()) {
        const int frame = currentFrameUnified();
        const auto notes = m_annotationManager->notes();
        for (const auto &n : notes) {
            if (n.frame != frame) continue;
            if (n.annotation_data.isEmpty()) continue;
            auto fromJson = AnnotationSerializer::jsonStringToStrokes(
                n.annotation_data);
            for (auto &s : fromJson) {
                strokesForFrame.push_back(std::move(s));
            }
        }
    }

#ifdef QCV_NATIVE_PLAYER
    if (auto *pw = qobject_cast<qcv::PlayerWindow *>(m_playerWindow.data())) {
        if (auto *r = pw->renderer()) {
            r->setStoredAnnotationStrokes(strokesForFrame);
            r->requestUpdate();
        }
    }
#endif
}

// ---- Phase 7.6 — Recent Media / Recent Projects ----------------
// Lazy-loaded from QSettings on first read; persisted on every
// mutation. Each list is capped at 10 entries, dedup-by-path,
// most-recent-first.
namespace {
constexpr int     kRecentCap          = 10;
constexpr auto    kRecentMediaKey     = "recents/media";
constexpr auto    kRecentProjectsKey  = "recents/projects";

void bumpToFront(QStringList &list, const QString &path) {
    if (path.isEmpty()) return;
    list.removeAll(path);
    list.prepend(path);
    while (list.size() > kRecentCap) list.removeLast();
}
} // namespace

void WindowManager::ensureRecentsLoaded() const
{
    if (m_recentsLoaded) return;
    QSettings s;
    m_recentMedia    = s.value(QString::fromLatin1(kRecentMediaKey)).toStringList();
    m_recentProjects = s.value(QString::fromLatin1(kRecentProjectsKey)).toStringList();
    m_recentsLoaded  = true;
}

void WindowManager::addRecentMedia(const QString &path)
{
    ensureRecentsLoaded();
    if (path.isEmpty()) return;
    const QStringList before = m_recentMedia;
    bumpToFront(m_recentMedia, path);
    if (before == m_recentMedia) return;
    QSettings s;
    s.setValue(QString::fromLatin1(kRecentMediaKey), m_recentMedia);
    emit recentMediaChanged();
}

void WindowManager::addRecentProject(const QString &path)
{
    ensureRecentsLoaded();
    if (path.isEmpty()) return;
    const QStringList before = m_recentProjects;
    bumpToFront(m_recentProjects, path);
    if (before == m_recentProjects) return;
    QSettings s;
    s.setValue(QString::fromLatin1(kRecentProjectsKey), m_recentProjects);
    emit recentProjectsChanged();
}

void WindowManager::clearRecentMedia()
{
    if (m_recentMedia.isEmpty() && m_recentsLoaded) return;
    m_recentMedia.clear();
    m_recentsLoaded = true;
    QSettings s;
    s.remove(QString::fromLatin1(kRecentMediaKey));
    emit recentMediaChanged();
}

void WindowManager::clearRecentProjects()
{
    if (m_recentProjects.isEmpty() && m_recentsLoaded) return;
    m_recentProjects.clear();
    m_recentsLoaded = true;
    QSettings s;
    s.remove(QString::fromLatin1(kRecentProjectsKey));
    emit recentProjectsChanged();
}

bool WindowManager::timelineHoverThumbsEnabled() const
{
    QSettings s;
    return s.value(QStringLiteral("ui/timelineHoverThumbs"), true).toBool();
}

void WindowManager::setTimelineHoverThumbsEnabled(bool on)
{
    QSettings s;
    const bool prev =
        s.value(QStringLiteral("ui/timelineHoverThumbs"), true).toBool();
    if (prev == on) return;
    s.setValue(QStringLiteral("ui/timelineHoverThumbs"), on);
    if (!on) clearHoverThumbnail();
    emit timelineHoverThumbsEnabledChanged();
}

bool WindowManager::timelineWaveformsEnabled() const
{
    QSettings s;
    return s.value(QStringLiteral("ui/timelineWaveforms"), true).toBool();
}

void WindowManager::setTimelineWaveformsEnabled(bool on)
{
    QSettings s;
    const bool prev =
        s.value(QStringLiteral("ui/timelineWaveforms"), true).toBool();
    if (prev == on) return;
    s.setValue(QStringLiteral("ui/timelineWaveforms"), on);
    emit timelineWaveformsEnabledChanged();
}

bool WindowManager::alwaysOpenMinimal() const
{
    QSettings s;
    return s.value(QStringLiteral("ui/alwaysOpenMinimal"), false).toBool();
}

void WindowManager::setAlwaysOpenMinimal(bool on)
{
    QSettings s;
    const bool prev =
        s.value(QStringLiteral("ui/alwaysOpenMinimal"), false).toBool();
    if (prev == on) return;
    s.setValue(QStringLiteral("ui/alwaysOpenMinimal"), on);
    emit alwaysOpenMinimalChanged();
}

QString WindowManager::screenshotFormat() const
{
    QSettings s;
    return s.value(QStringLiteral("export/screenshotFormat"),
                   QStringLiteral("png")).toString();
}

void WindowManager::setScreenshotFormat(const QString &fmt)
{
    // Normalize to a known lower-case token; ignore anything else so
    // a stray value can never reach QImageWriter.
    const QString f = fmt.trimmed().toLower();
    if (f != QLatin1String("png") && f != QLatin1String("jpeg")
        && f != QLatin1String("tiff")) {
        return;
    }
    QSettings s;
    const QString prev =
        s.value(QStringLiteral("export/screenshotFormat"),
                QStringLiteral("png")).toString();
    if (prev == f) return;
    s.setValue(QStringLiteral("export/screenshotFormat"), f);
    emit screenshotFormatChanged();
}

int WindowManager::audioSyncOffsetMsDefault() const
{
    // Single-flow default. The new port's video pipeline is far
    // tighter than the old app's (Vulkan-decode → D3D11 present
    // skips the ~28 ms D3D11 decoder buffering the old app baked
    // into its 35 ms default), so single-flow's right offset is
    // small. macOS' VideoToolbox + CAMetalLayer present path adds
    // a few extra ms vs Windows' Vulkan-decode → D3D11 present, so
    // the right offset is a touch higher there.
#if defined(Q_OS_MACOS)
    return 11;
#else
    return 7;
#endif
}

int WindowManager::audioSyncOffsetMs() const
{
    QSettings s;
    return s.value(QStringLiteral("audio/syncOffsetMs"),
                     audioSyncOffsetMsDefault()).toInt();
}

void WindowManager::setAudioSyncOffsetMs(int ms)
{
    if (ms < -100) ms = -100;
    if (ms >  100) ms =  100;
    QSettings s;
    const int prev =
        s.value(QStringLiteral("audio/syncOffsetMs"),
                  audioSyncOffsetMsDefault()).toInt();
    if (prev == ms) return;
    s.setValue(QStringLiteral("audio/syncOffsetMs"), ms);

    // Single-flow only — dual has its own offset (see
    // setDualAudioSyncOffsetMs). We intentionally don't force a
    // re-seek here; the next codec seek (next scrub / next play
    // tick) re-anchors with the new offset on its own.
    if (m_audio) m_audio->setSyncOffsetMs(ms);
    emit audioSyncOffsetMsChanged();
}

int WindowManager::dualAudioSyncOffsetMsDefault() const
{
    // Dual's pipeline is meaningfully deeper than single-flow on
    // both platforms — extra per-side decode, the dual-canvas
    // composite, and an OCIO pass over a larger intermediate add
    // measurable latency on top of whatever single-flow takes.
    // macOS sits a couple ms higher than Windows for the same
    // reason single-flow does (VideoToolbox + CAMetalLayer present).
#if defined(Q_OS_MACOS)
    return 17;
#else
    return 15;
#endif
}

int WindowManager::dualAudioSyncOffsetMs() const
{
    QSettings s;
    return s.value(QStringLiteral("audio/dualSyncOffsetMs"),
                     dualAudioSyncOffsetMsDefault()).toInt();
}

void WindowManager::setDualAudioSyncOffsetMs(int ms)
{
    if (ms < -100) ms = -100;
    if (ms >  100) ms =  100;
    QSettings s;
    const int prev =
        s.value(QStringLiteral("audio/dualSyncOffsetMs"),
                  dualAudioSyncOffsetMsDefault()).toInt();
    if (prev == ms) return;
    s.setValue(QStringLiteral("audio/dualSyncOffsetMs"), ms);

    // Dual-flow only. Push to the live mixer if dual is active.
    // Persists either way so a value tuned while dual was closed
    // still takes effect the next time the user enters dual mode.
    if (m_dualController && m_dualController->audio()) {
        m_dualController->audio()->setSyncOffsetMs(ms);
    }
    emit dualAudioSyncOffsetMsChanged();
}

bool WindowManager::scrubAudioMuted() const
{
    QSettings s;
    return s.value(QStringLiteral("audio/scrubMuted"), false).toBool();
}

void WindowManager::setScrubAudioMuted(bool muted)
{
    QSettings s;
    const bool prev =
        s.value(QStringLiteral("audio/scrubMuted"), false).toBool();
    if (prev == muted) return;
    s.setValue(QStringLiteral("audio/scrubMuted"), muted);

    // One process-wide flag reaches all engines (single AudioPlayer +
    // dual mixer's two); a gesture in flight goes silent/audible on
    // the grain thread's next loop iteration.
    ShuttleAudioEngine::setGlobalMute(muted);
    emit scrubAudioMutedChanged();
}

QString WindowManager::audioRoutingScopeMediaItemId() const
{
    // Playlist mode: each clip points at its underlying source
    // MediaItem via clip->mediaItemId; that's where the user's saved
    // routing preference lives. activeItemId() returns the playlist's
    // own MediaItem in this mode, whose audioRoutingMode is unrelated
    // to whatever clip is currently playing.
    if (m_playlistActive) {
        if (const Clip *c = playlistActiveClip()) {
            if (!c->mediaItemId.isEmpty()) return c->mediaItemId;
        }
        // Fall through to activeItemId() when no clip resolves —
        // returns the playlist id, which is at least non-empty and
        // keeps the inspector pill bindings from going blank during
        // mid-load states.
    }
    return m_project ? m_project->activeItemId() : QString();
}

bool WindowManager::openProjectLink(const QString &uri)
{
    if (!m_project) return false;

    // Parse qcview://<uuid>[/<mediaId>] — accept missing scheme
    // prefix so callers can pass a bare uuid/mediaId pair too.
    QString body = uri.trimmed();
    if (body.startsWith(QStringLiteral("qcview://"),
                        Qt::CaseInsensitive)) {
        body.remove(0, QStringLiteral("qcview://").size());
    }
    if (body.isEmpty()) {
        qWarning("openProjectLink: empty URI");
        return false;
    }

    // v2.2.3 — stream form: qcview://stream?url=<pct-encoded>[&name=…].
    // Adds (dedupes) the live stream into the CURRENT project and
    // activates it. This is the hook the qcbridge host panel uses for
    // "open in QCView".
    if (body.startsWith(QStringLiteral("stream"), Qt::CaseInsensitive)) {
        const QUrlQuery q(QUrl(uri).query());
        const QString streamUrl =
            q.queryItemValue(QStringLiteral("url"),
                             QUrl::FullyDecoded).trimmed();
        const QString streamName =
            q.queryItemValue(QStringLiteral("name"), QUrl::FullyDecoded);
        if (streamUrl.isEmpty()) {
            qWarning("openProjectLink: stream link missing url= ('%s')",
                     qPrintable(uri));
            return false;
        }
        const QString id = m_project->addLiveStream(streamUrl, streamName);
        if (id.isEmpty()) return false;
        m_project->setActiveItem(id);
        qInfo("openProjectLink: live stream via deep link — %s",
              qPrintable(streamUrl));
        return true;
    }
    const int slashIdx = body.indexOf(QLatin1Char('/'));
    const QString uuid    = (slashIdx < 0) ? body
                                           : body.left(slashIdx);
    const QString mediaId = (slashIdx < 0) ? QString()
                                           : body.mid(slashIdx + 1);
    if (uuid.isEmpty()) {
        qWarning("openProjectLink: missing uuid in '%s'",
                 qPrintable(uri));
        return false;
    }

    // Find a recent project whose JSON `uuid` field matches. Recents
    // are capped at 10, so reading + parsing each is cheap. Earliest
    // match wins (recents are most-recent-first; if the same UUID
    // appears at two paths, the user's last-opened wins).
    ensureRecentsLoaded();
    QString matchedPath;
    for (const QString &path : m_recentProjects) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = f.readAll();
        f.close();
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            continue;
        }
        const QString got =
            doc.object().value(QStringLiteral("uuid")).toString();
        if (got == uuid) {
            matchedPath = path;
            break;
        }
    }
    if (matchedPath.isEmpty()) {
        qWarning("openProjectLink: no recent project with uuid='%s' "
                 "(qcview:// only resolves links to projects already "
                 "in this user's Recent list)",
                 qPrintable(uuid));
        return false;
    }

    if (!m_project->openProject(matchedPath)) {
        qWarning("openProjectLink: openProject('%s') failed",
                 qPrintable(matchedPath));
        return false;
    }
    if (!mediaId.isEmpty()) {
        m_project->setActiveItem(mediaId);
    }
    qInfo("openProjectLink: opened '%s', active='%s'",
          qPrintable(matchedPath), qPrintable(mediaId));
    return true;
}

QString WindowManager::buildProjectLink() const
{
    if (!m_project) return {};
    const QString uuid    = m_project->projectUuid();
    const QString mediaId = m_project->activeItemId();
    if (uuid.isEmpty() || mediaId.isEmpty()) return {};
    return QStringLiteral("qcview://%1/%2").arg(uuid, mediaId);
}

bool WindowManager::copyProjectLinkToClipboard() const
{
    const QString link = buildProjectLink();
    if (link.isEmpty()) return false;
    QGuiApplication::clipboard()->setText(link);
    qInfo("WindowManager: project link copied — %s",
          qPrintable(link));
    return true;
}

} // namespace qcv
