// WindowManager — coordinates UI window + Player child window.
//
// Phase 1.2 scope: two-window topology shell.
//   - Owns the Player window's lifetime.
//   - Tracks the UI window's `centerStage` Item geometry.
//   - Reparents Player on detach / reattach (Guide 01 §1, Guide 03 §3).
//
// Future phases layer on top:
//   - Phase 2: HDR swapchain configuration on the Player window
//     (Guide 06 §1).
//   - Phase 3: PlayerRhiItem fills the Player window with the
//     render pipeline (Guide 01 §5).

#pragma once

#include "annotations/annotation_manager.h"
#include "annotations/viewport_annotator.h"
#include "audio/audio_player.h"
#include "render/safety_overlay.h"
#include "color/ocio_config_manager.h"
#include "color/preset_manager.h"
#include "decode/image_sequence_cache.h"
#include "decode/scrub_decoder.h"
#include "decode/video_decoder.h"
#include "project/project_manager.h"
#include "timeline/timeline_controller.h"
#include "timeline/timeline_thumbnail_cache.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QRect>
#include <atomic>
#include <cstdint>
#include <memory>

// Phase 7.7 — DualPlaybackController is exposed as a Q_PROPERTY
// (so QML can bind to its frame/playing state). Q_OBJECT* properties
// require the full type at moc time, so we include the header.
#include "dual/dual_playback_controller.h"

class QQmlApplicationEngine;
class QQuickWindow;
class QQuickItem;
class QTimer;

namespace qcv {

#if !defined(Q_OS_MACOS) && !defined(__APPLE__)
class WindowManagerDualSourceAdapter;
#endif

class WindowManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool detached READ detached NOTIFY detachedChanged)
    Q_PROPERTY(int compositorMode READ compositorMode WRITE setCompositorMode NOTIFY compositorModeChanged)
    Q_PROPERTY(qreal splitPos READ splitPos WRITE setSplitPos NOTIFY splitPosChanged)
    Q_PROPERTY(int backgroundMode READ backgroundMode WRITE setBackgroundMode NOTIFY backgroundModeChanged)
    Q_PROPERTY(bool loopEnabled READ loopEnabled WRITE setLoopEnabled NOTIFY loopEnabledChanged)
    Q_PROPERTY(bool timelineHoverThumbsEnabled
               READ timelineHoverThumbsEnabled
               WRITE setTimelineHoverThumbsEnabled
               NOTIFY timelineHoverThumbsEnabledChanged)
    // Global A/V-sync compensation, milliseconds. Positive = audio
    // DELAYED (plays later) — compensates for video pipeline +
    // display lag, the most common case. Persisted to QSettings;
    // per-OS default seeded on first launch (see
    // audioSyncOffsetMsDefault).
    Q_PROPERTY(int audioSyncOffsetMs
               READ audioSyncOffsetMs
               WRITE setAudioSyncOffsetMs
               NOTIFY audioSyncOffsetMsChanged)
    Q_PROPERTY(int audioSyncOffsetMsDefault
               READ audioSyncOffsetMsDefault
               CONSTANT)
    // Dual-mode A/V sync offset — separate from the single-mode value
    // because dual's compositing pipeline (per-side decode + the
    // dual-canvas blit + extra OCIO pass) takes meaningfully longer
    // than the single-flow path. Default is per-OS, tuned empirically:
    // Windows ~35 ms (D3D11 dual compositor + RGBA16F canvas roundtrip),
    // macOS ~20 ms (Metal's two-encoder dual path is tighter but still
    // slower than single-flow). Adjusting one does NOT affect the
    // other — the active flow honors its own value.
    Q_PROPERTY(int dualAudioSyncOffsetMs
               READ dualAudioSyncOffsetMs
               WRITE setDualAudioSyncOffsetMs
               NOTIFY dualAudioSyncOffsetMsChanged)
    Q_PROPERTY(int dualAudioSyncOffsetMsDefault
               READ dualAudioSyncOffsetMsDefault
               CONSTANT)
    Q_PROPERTY(int  inPoint  READ inPoint  NOTIFY inOutPointsChanged)
    Q_PROPERTY(int  outPoint READ outPoint NOTIFY inOutPointsChanged)
    Q_PROPERTY(bool hasInOutRange READ hasInOutRange NOTIFY inOutPointsChanged)
    Q_PROPERTY(qcv::VideoDecoder *videoDecoder READ videoDecoder CONSTANT)
    Q_PROPERTY(qcv::VideoDecoder *videoDecoderB READ videoDecoderB CONSTANT)
    Q_PROPERTY(qcv::ScrubDecoder *scrubDecoder READ scrubDecoder CONSTANT)
    // Phase 7.4.a — image-sequence ring-buffer status, exposed to
    // QML so the timeline panel can render a cache-fill indicator.
    // -1 / 0 when no sequence is loaded; the QML side already
    // hides the indicator unless currentMediaKind == ImageSequence.
    Q_PROPERTY(int imageSeqBufferedAhead   READ imageSeqBufferedAhead
               NOTIFY imageSeqBufferStatusChanged)
    Q_PROPERTY(int imageSeqBufferedBehind  READ imageSeqBufferedBehind
               NOTIFY imageSeqBufferStatusChanged)
    // Coverage = farthest cached-frame distance from the playhead
    // in each direction. The strip uses these to draw the actual
    // span buffered (independent of stride / density).
    Q_PROPERTY(int imageSeqBufferedAheadCoverage   READ imageSeqBufferedAheadCoverage
               NOTIFY imageSeqBufferStatusChanged)
    Q_PROPERTY(int imageSeqBufferedBehindCoverage  READ imageSeqBufferedBehindCoverage
               NOTIFY imageSeqBufferStatusChanged)
    Q_PROPERTY(int imageSeqReadAheadFrames  READ imageSeqReadAheadFrames
               NOTIFY imageSeqBufferStatusChanged)
    Q_PROPERTY(int imageSeqReadBehindFrames READ imageSeqReadBehindFrames
               NOTIFY imageSeqBufferStatusChanged)
    Q_PROPERTY(int imageSeqFrameCount      READ imageSeqFrameCount
               NOTIFY imageSeqBufferStatusChanged)
    // 0-based frame indices (relative to the sequence's startFrame)
    // that are missing on disk or failed to load. Used by the
    // timeline cache band to render red ticks at known-bad frames
    // so the user sees gaps without having to scrub onto each one.
    // Pre-populated at sequence open from on-disk gap detection;
    // grows runtime as the cache encounters loader failures.
    Q_PROPERTY(QVariantList imageSeqFailedFrames READ imageSeqFailedFrames
               NOTIFY imageSeqBufferStatusChanged)
    // Phase 7.4.x — cache stride knob. 1 = every frame; 2/3/4 =
    // every Nth on the absolute grid. Surfaced in the inspector
    // panel so users can downshift heavy 4K EXR sequences without
    // running the host out of RAM.
    Q_PROPERTY(int imageSeqCacheStride     READ imageSeqCacheStride
               WRITE setImageSeqCacheStride
               NOTIFY imageSeqCacheStrideChanged)

    // Dual-mode per-side image-seq buffer status. Mirror of the
    // single-flow imageSeqBuffered* properties above, but sourced
    // from DualImageSeqSource on each side of m_dualController.
    // 0 / false when the matching side isn't an image sequence (or
    // when dual mode isn't active). Polled at 33 ms via
    // m_dualBufferPollTimer; emit is delta-gated by cached members.
    // DualImageSeqSource has no cache stride, so raw bufferedAhead
    // == coverage for these — no separate coverage properties.
    Q_PROPERTY(int  dualImageSeqBufferedAheadA  READ dualImageSeqBufferedAheadA
               NOTIFY dualImageSeqStatusChanged)
    Q_PROPERTY(int  dualImageSeqBufferedBehindA READ dualImageSeqBufferedBehindA
               NOTIFY dualImageSeqStatusChanged)
    Q_PROPERTY(int  dualImageSeqFrameCountA     READ dualImageSeqFrameCountA
               NOTIFY dualImageSeqStatusChanged)
    Q_PROPERTY(bool dualImageSeqIsActiveA       READ dualImageSeqIsActiveA
               NOTIFY dualImageSeqStatusChanged)
    Q_PROPERTY(double dualImageSeqFpsA          READ dualImageSeqFpsA
               NOTIFY dualImageSeqStatusChanged)
    Q_PROPERTY(int  dualImageSeqBufferedAheadB  READ dualImageSeqBufferedAheadB
               NOTIFY dualImageSeqStatusChanged)
    Q_PROPERTY(int  dualImageSeqBufferedBehindB READ dualImageSeqBufferedBehindB
               NOTIFY dualImageSeqStatusChanged)
    Q_PROPERTY(int  dualImageSeqFrameCountB     READ dualImageSeqFrameCountB
               NOTIFY dualImageSeqStatusChanged)
    Q_PROPERTY(bool dualImageSeqIsActiveB       READ dualImageSeqIsActiveB
               NOTIFY dualImageSeqStatusChanged)
    Q_PROPERTY(double dualImageSeqFpsB          READ dualImageSeqFpsB
               NOTIFY dualImageSeqStatusChanged)

    // Per-source-channel audio peak meters, sampled at ~30 Hz from
    // the active AudioPlayer / DualAudioMixer side. Each list entry
    // is a float in [0..1]. Names matches Peaks element-wise; e.g.
    // ("L","R","C","LFE","Ls","Rs") for a 5.1 source. The B-side
    // properties carry the dual mode's right side (empty in single).
    Q_PROPERTY(QVariantList audioChannelPeaks READ audioChannelPeaks
               NOTIFY audioMetersChanged)
    Q_PROPERTY(QStringList  audioChannelNames READ audioChannelNames
               NOTIFY audioMetersChanged)
    Q_PROPERTY(QVariantList audioChannelPeaksB READ audioChannelPeaksB
               NOTIFY audioMetersChanged)
    Q_PROPERTY(QStringList  audioChannelNamesB READ audioChannelNamesB
               NOTIFY audioMetersChanged)
    Q_PROPERTY(qcv::OCIOConfigManager *ocio READ ocio CONSTANT)
    Q_PROPERTY(qcv::PresetManager *presets READ presets CONSTANT)
    Q_PROPERTY(qcv::AudioPlayer *audio READ audio CONSTANT)
    Q_PROPERTY(qcv::ProjectManager *project READ project CONSTANT)
    Q_PROPERTY(qcv::TimelineController *timeline READ timeline CONSTANT)
    // Phase 7.6 — Recent lists. Persisted via QSettings; capped
    // at 10 each. Top-of-list is most recent. QML's File menu
    // binds an Instantiator to these for dynamic submenu items.
    Q_PROPERTY(QStringList recentMedia READ recentMedia
               NOTIFY recentMediaChanged)
    Q_PROPERTY(QStringList recentProjects READ recentProjects
               NOTIFY recentProjectsChanged)
    // Polish — font family names registered in main.cpp via
    // QFontDatabase::addApplicationFont; QML's Theme singleton
    // binds to these so call sites can use Theme.fontFamily /
    // monoFamily / iconFamily without guessing what the .ttf
    // registers as.
    Q_PROPERTY(QString interFamily    READ interFamily    CONSTANT)
    Q_PROPERTY(QString monoFamily     READ monoFamily     CONSTANT)
    Q_PROPERTY(QString phosphorFamily READ phosphorFamily CONSTANT)
    // Phase 7.7 — DualPlaybackController exposed when active (otherwise
    // null). QML binds its Q_PROPERTYs (currentFrame / frameCount /
    // fps / isPlaying) for transport feedback in dual mode.
    Q_PROPERTY(qcv::dual::DualPlaybackController *dualController
               READ dualController NOTIFY dualControllerChanged)
    // Phase 7.4.b.2 — gates PlayerRhiItem's GPU texture pool.
    // True while an image-sequence is loaded. The QML binding in
    // PlayerWindow.qml forwards it to PlayerRhiItem::imageSeqActive.
    Q_PROPERTY(bool imageSeqActive READ imageSeqActive
               NOTIFY imageSeqActiveChanged)
    // Audio-only mode flag. Set when the active media is a wav /
    // mp3 / etc. — VideoDecoder is parked in Errored state, audio
    // plays through AudioPlayer, transport drives the timer + audio
    // directly. QML transport bindings (TransportBar.isPlaying,
    // hasMedia) check this so the play/pause icon and slider state
    // reflect what's actually happening.
    Q_PROPERTY(bool audioActive READ audioActive
               NOTIFY audioActiveChanged)
    // Phase 7.4.b.4 — pull-model: the cache itself, exposed to QML
    // so the PlayerWindow.qml binding can forward it to
    // PlayerRhiItem::imageSeqCache. Renderer pulls pixels from
    // this cache directly instead of from the FFmpeg-shaped
    // VideoDecoder::fetchLatest() slot.
    Q_PROPERTY(qcv::ImageSequenceCache *imageSeqCache READ imageSeqCache
               NOTIFY imageSeqCacheChanged)
    // Image-sequence metadata, surfaced for QML transport bindings.
    // Replaces the videoDecoder metadata façade that openExternal
    // populated before Phase 7.4.b.4. Returns 0 when no sequence.
    Q_PROPERTY(int imageSeqWidth      READ imageSeqWidth
               NOTIFY imageSeqMetadataChanged)
    Q_PROPERTY(int imageSeqHeight     READ imageSeqHeight
               NOTIFY imageSeqMetadataChanged)
    Q_PROPERTY(double imageSeqFps     READ imageSeqFps
               NOTIFY imageSeqMetadataChanged)
    Q_PROPERTY(int imageSeqFrameCountMeta READ imageSeqFrameCount
               NOTIFY imageSeqMetadataChanged)
    // Phase 2.6.1: HDR mode of the player window. SDR = sRGB
    // swapchain (current behavior); ExtendedLinearSRgb = scRGB-style
    // FP16 extended-linear sRGB layer that the OS compositor
    // tonemaps to display headroom (macOS EDR / Win/Linux scRGB).
    // Toggle recreates the player swapchain. Guide 06 D2 expands
    // this to 4 EDR variants on macOS + HDR10 PQ on Vulkan in
    // later sub-phases; D1 keeps the choice manual (no auto-follow
    // of the OCIO Output).
    Q_PROPERTY(int hdrMode READ hdrMode WRITE setHdrMode NOTIFY hdrModeChanged)

    // Phase F.2.8 follow-up — uniform post-OCIO brightness. 1.0 is
    // identity, < 1 dims, > 1 brightens. Persisted across sessions
    // in QSettings under "display/brightness". A linear multiplier
    // will clip HDR highlights above the display's headroom — that's
    // a known cost of using this in HDR modes.
    Q_PROPERTY(double brightness READ brightness WRITE setBrightness NOTIFY brightnessChanged)

public:
    // Stroke-history entry — used by the annotation undo/redo
    // stacks. Defined up here (rather than next to the
    // annotation API in the public block lower down) so the
    // private QList<AnnotationUndoEntry> members further on
    // see a complete type.
    struct AnnotationUndoEntry {
        QString timecode;
        QString priorAnnotationData;   // empty when noteWasNew
        bool    noteWasNew = false;
    };

    // Display modes for the player swapchain. Two axes:
    //   1. SDR vs HDR (8-bit sRGB-encoded vs FP16 extended-linear /
    //      10-bit PQ). HDR variants flip the QRhiSwapChain::Format
    //      via Qt's `_qt_sg_hdr_format` window property.
    //   2. Color gamut (sRGB / Rec.709 vs Display P3 vs Rec.2020).
    //      For SDR this drives QSurfaceFormat::colorSpace; for HDR
    //      the swapchain Format already encodes the gamut.
    //
    // Hdr10 is Vulkan-only (Windows / Linux). The QML picker filters
    // by platform.
    enum HdrMode : int {
        SdrSRgb                   = 0,
        SdrDisplayP3              = 1,
        ExtendedLinearSRgb        = 2,
        ExtendedLinearDisplayP3   = 3,
        Hdr10                     = 4,
    };
    Q_ENUM(HdrMode)

    explicit WindowManager(QQmlApplicationEngine *engine, QObject *parent = nullptr);
    ~WindowManager() override;

    // Called by main() once the UI window is constructed and its QML tree
    // is loaded. Locates the centerStage Item, creates the Player window,
    // and starts geometry tracking.
    bool initialize();

    bool detached() const { return m_detached; }

    int compositorMode() const { return m_compositorMode; }
    void setCompositorMode(int mode);
    int backgroundMode() const { return m_backgroundMode; }
    void setBackgroundMode(int mode);
    bool loopEnabled() const { return m_loopEnabled; }
    void setLoopEnabled(bool on);

    // In/Out point API. Frame numbers; -1 = unset. Setting either
    // pushes the resulting range to PlaybackTimer.setLoopRange and
    // emits inOutPointsChanged so QML markers reposition.
    int  inPoint()  const { return m_inPoint; }
    int  outPoint() const { return m_outPoint; }
    bool hasInOutRange() const {
        return m_inPoint >= 0 && m_outPoint > m_inPoint;
    }
    Q_INVOKABLE void setInPointAtCurrent();
    Q_INVOKABLE void setOutPointAtCurrent();
    Q_INVOKABLE void clearInOutPoints();

    // Phase F.2.7 (Windows) — pointer events from a QML MouseArea
    // inside centerStage. On macOS PlayerWindow is a QWindow and its
    // mousePress/Move/Release virtuals forward to ViewportAnnotator
    // directly. On Windows our D3D11 child HWND uses HTTRANSPARENT
    // and PlayerWindow has no visible surface, so events reach Qt's
    // QML scene instead. `phase` is qcv::PointerPhase cast to int
    // (0=Press, 1=Move, 2=Release). `x`/`y` are QML logical points
    // relative to centerStage; we scale by DPR + offset by
    // centerStage origin to match the renderer's pixel-space
    // viewport rect.
    Q_INVOKABLE void forwardViewportPointer(int phase, qreal x, qreal y,
                                              qint64 timestamp);
private:
    void pushInOutToTimer();
public:

    qreal splitPos() const { return m_splitPos; }
    void setSplitPos(qreal pos);

    // Split-wipe seam highlight — true while the seam handle is
    // hovered or being dragged, so the compositor draws the seam in
    // full white instead of its faint-grey rest color. Transient UI
    // state, forwarded straight to the renderer (no persistence).
    Q_INVOKABLE void setSplitSeamActive(bool active);

    // File-open wrappers that drive the loading spinner. Each flips the
    // renderer's loadingActive flag, defers the (main-thread-blocking)
    // work one event-loop turn so the menu closes + the spinner appears
    // first, runs it, then clears the flag. ALL file-open entry points
    // (menu, recent, transport, drops, dialogs) route through these.
    //   openMediaPaths — add each path to the project AND load (the
    //                    last one becomes the active item).
    //   addMediaPaths  — add to the bin only (no auto-load).
    //   openProjectPath— open a .qcvproj.
    // QStringList args carry a single path too (pass [path]).
    Q_INVOKABLE void openMediaPaths(const QStringList &paths);
    Q_INVOKABLE void addMediaPaths(const QStringList &paths);
    Q_INVOKABLE void openProjectPath(const QString &path);

    VideoDecoder *videoDecoder()  const { return m_videoDecoder;  }
    VideoDecoder *videoDecoderB() const { return m_videoDecoderB; }
    // Open a media file into Source B. Phase B minimum-viable —
    // no audio routing, no scrub decoder for B yet, no in/out
    // points, just a second decoder feeding the compositor's srcB
    // slot. Atomic-pair sync to A is a follow-up.
    // Phase 7.7 Stage 5 — preferred B-drop entry. Adds the file to
    // the project pool (dedupe by path) AND sets ProjectManager's
    // bSource ref. If currently in dual mode, hot-swaps B in the
    // active DualPlaybackController. Returns true on success.
    Q_INVOKABLE bool setBSource(const QString &path);
    Q_INVOKABLE void clearBSource();

    // Pre-Stage-5 fallback. Kept until the QML / single-flow callers
    // are migrated; routes to setBSource internally.
    Q_INVOKABLE bool openSourceB(const QString &path);
    Q_INVOKABLE void closeSourceB();

    // Phase 7.7 Stage 3 — developer-only entry to the dual playback
    // island. Tears down single-flow A/B state, spins up a
    // DualPlaybackController against both paths, switches the
    // renderer to DualFlow mode. Returns true if both sides opened.
    // exitDualTestMode reverses everything — single flow resumes.
    // Stage 4 will replace this with the proper compositor-mode
    // transition handler.
    Q_INVOKABLE bool enterDualTestMode(const QString &pathA,
                                        const QString &pathB);
    Q_INVOKABLE void exitDualTestMode();
    Q_INVOKABLE void dualPlay();
    Q_INVOKABLE void dualPause();
    Q_INVOKABLE void dualSeekToFrame(int frameNumber);

    // Phase 7.8 Stage A — verification knob. Mutates the A or B
    // track's first clip directly: bumps `sourceIn` by `seconds`
    // (head-trim simulation) so the translation in
    // DualPlaybackController can be observed. Real edit ops land
    // in Stage C+ via QUndoCommand subclasses. Returns false if no
    // dual mode active or no track A/B clip.
    Q_INVOKABLE bool testHeadTrim(const QString &trackId, double seconds);

    // Phase 7.8 Stage F — DualPair save / load.
    // saveCurrentDualView snapshots the active dual session
    // (paths + post-edit clip arrays + master fps / duration +
    // in/out points) into a new MediaItem of type DualPair, in
    // the "Dual Views" bin. Returns the new id, or empty on
    // failure. No-op when not in dual mode. The user-supplied
    // name is used as the bin label; pass empty for an auto-name
    // built from the basenames of A and B.
    Q_INVOKABLE QString saveCurrentDualView(const QString &name);
    // loadDualView opens the named DualPair MediaItem: sets A
    // (via setActiveItem on its mediaIdA), sets B (bSource), and
    // enters dual mode. After dual rebuild completes, the saved
    // clip layouts replace the auto-built tracks. Returns true
    // if the item exists + load proceeded.
    Q_INVOKABLE bool    loadDualView(const QString &dualPairId);

    // Delete a MediaItem from the project pool. If the item is the
    // active source, tears down playback first (closeActiveMedia /
    // dual-mode exit) so the player doesn't keep streaming a file
    // that's no longer in the project. If it's the B source in a
    // dual session, clears B. Returns true on success.
    Q_INVOKABLE bool    deleteMediaItem(const QString &id);

    ScrubDecoder *scrubDecoder() const { return m_scrubDecoder; }
    OCIOConfigManager *ocio() const { return m_ocio; }
    PresetManager *presets() const { return m_presets; }
    AudioPlayer *audio() const { return m_audio; }
    ProjectManager *project() const { return m_project; }
    TimelineController *timeline() const { return m_timeline; }
    QStringList recentMedia()    const { ensureRecentsLoaded(); return m_recentMedia; }
    QStringList recentProjects() const { ensureRecentsLoaded(); return m_recentProjects; }
    Q_INVOKABLE void addRecentMedia(const QString &path);
    Q_INVOKABLE void addRecentProject(const QString &path);
    Q_INVOKABLE void clearRecentMedia();
    Q_INVOKABLE void clearRecentProjects();

    // Phase 7.6 — Settings-bound runtime toggle for the
    // timeline's hover-thumbnail preview. Read live (no
    // restart needed); QML's TimelinePanel checks this on
    // every throttled hover request.
    bool timelineHoverThumbsEnabled() const;
    void setTimelineHoverThumbsEnabled(bool on);

    // A/V sync compensation for SINGLE-flow audio. Persisted under
    // audio/syncOffsetMs in QSettings; first launch seeds with
    // audioSyncOffsetMsDefault(). Adjusting does not affect dual.
    int  audioSyncOffsetMs() const;
    void setAudioSyncOffsetMs(int ms);
    int  audioSyncOffsetMsDefault() const;

    // Dual-flow audio sync offset. Independent persisted value under
    // audio/dualSyncOffsetMs so the user can dial in dual's longer
    // composite-to-present latency without disturbing single-flow's
    // tuning. Pushed to DualAudioMixer at dual entry + on slider
    // change while in dual.
    int  dualAudioSyncOffsetMs() const;
    void setDualAudioSyncOffsetMs(int ms);
    int  dualAudioSyncOffsetMsDefault() const;

    // The MediaItem.id whose audioRoutingMode is the source of truth
    // for whatever audio is playing right now. In playlist mode this
    // is the active clip's source MediaItem (clip->mediaItemId), not
    // the playlist's own id — activeItemId() returns the PLAYLIST in
    // playlist mode, so consulting it for routing gives the wrong
    // answer at every clip boundary. Used by the audio-routing apply
    // site, the change-signal dispatch handler, and the inspector
    // pill's edit target / display binding. Returns empty when
    // nothing is loaded (no project, no active item, or playlist
    // mode with no resolved active clip).
    //
    // Q_PROPERTY so the inspector pill can bind reactively. The
    // NOTIFY fires on activeItemIdChanged AND on playlist clip
    // transitions (playlistAdvanceToClip emits it after the index
    // update). Without the clip-transition fire the QML binding
    // would stay stale on cross-clip seeks within a playlist —
    // displayedItem doesn't change in that case so a binding off
    // it alone never re-evaluates.
    Q_PROPERTY(QString audioRoutingScopeMediaItemId
               READ audioRoutingScopeMediaItemId
               NOTIFY audioRoutingScopeChanged)
    Q_INVOKABLE QString audioRoutingScopeMediaItemId() const;

    // Index into the active playlist's `items` list of the clip
    // currently under the playhead, or -1 outside playlist mode / when
    // no clip resolves. The inspector's playlist list binds this to
    // highlight the active clip (used as a tracker). Mapped past
    // timeline gap clips. NOTIFY fires on every clip transition (same
    // site as audioRoutingScopeChanged).
    Q_PROPERTY(int playlistCurrentItemIndex
               READ playlistCurrentItemIndex
               NOTIFY playlistCurrentItemIndexChanged)
    int playlistCurrentItemIndex() const;

    // Phase 7.6 — qcview:// URL builder for "Copy Project Link".
    // Format: qcview://<project-uuid>/<active-media-id>
    // Returns empty when the project hasn't been saved (no UUID
    // surfaced yet — wait, projects always have a UUID from
    // newProject) OR no media item is active. The QML File menu
    // disables the Copy Link entry when this returns empty.
    // Native URL-open handling + Info.plist registration land in
    // a later polish pass once packaging stabilizes.
    Q_INVOKABLE QString buildProjectLink() const;

    // Inbound qcview:// handler. Parses qcview://<uuid>/<mediaId>,
    // scans the Recent Projects list for a project file whose
    // top-level uuid matches, opens it, and activates the
    // referenced media item. Returns true on full success
    // (project opened + media activated), false on any failure
    // (malformed URI, no matching project, etc.).
    //
    // Wired from QEvent::FileOpen at the app event-filter level so
    // it fires whether the link launches a fresh process or hits
    // an already-running instance.
    Q_INVOKABLE bool openProjectLink(const QString &uri);
    Q_INVOKABLE bool    copyProjectLinkToClipboard() const;

    // Polish — font family accessors. main.cpp populates these
    // after QFontDatabase::addApplicationFont() so QML's Theme
    // resolves the actual registered names rather than guessing.
    QString interFamily()    const { return m_interFamily; }
    QString monoFamily()     const { return m_monoFamily; }
    QString phosphorFamily() const { return m_phosphorFamily; }
    void setInterFamily(const QString &v)    { m_interFamily    = v; }
    void setMonoFamily(const QString &v)     { m_monoFamily     = v; }
    void setPhosphorFamily(const QString &v) { m_phosphorFamily = v; }
    qcv::dual::DualPlaybackController *dualController() const {
        return m_dualController.get();
    }
    int  hdrMode() const { return m_hdrMode; }
    void setHdrMode(int mode);

    double brightness() const { return m_brightness; }
    void   setBrightness(double brightness);

    bool imageSeqActive() const { return m_imageSeqActive; }
    bool audioActive()    const { return m_audioActive; }
    ImageSequenceCache *imageSeqCache() const { return m_imageSeqCache.get(); }

    // Image-sequence dimensions / fps — delegate to the cache.
    // Returns 0 when no sequence is loaded.
    int    imageSeqWidth() const;
    int    imageSeqHeight() const;
    double imageSeqFps() const;

    // Image-sequence buffer status accessors. All return 0 when
    // no sequence is loaded so the QML's binding math (% fill,
    // width = ahead * pps, etc.) yields zero-sized indicators.
    int imageSeqBufferedAhead() const;
    int imageSeqBufferedBehind() const;
    QVariantList imageSeqFailedFrames() const;
    int imageSeqBufferedAheadCoverage() const;
    int imageSeqBufferedBehindCoverage() const;
    int imageSeqReadAheadFrames() const;
    int imageSeqReadBehindFrames() const;
    int imageSeqFrameCount() const;
    int imageSeqCacheStride() const;
    Q_INVOKABLE void setImageSeqCacheStride(int stride);

    // Dual-mode per-side image-seq accessors. All zero / false when
    // the matching side isn't a DualImageSeqSource (or when dual
    // mode isn't active). See Q_PROPERTY declarations above for
    // the QML binding shape.
    int    dualImageSeqBufferedAheadA()  const;
    int    dualImageSeqBufferedBehindA() const;
    int    dualImageSeqFrameCountA()     const;
    bool   dualImageSeqIsActiveA()       const;
    double dualImageSeqFpsA()            const;
    int    dualImageSeqBufferedAheadB()  const;
    int    dualImageSeqBufferedBehindB() const;
    int    dualImageSeqFrameCountB()     const;
    bool   dualImageSeqIsActiveB()       const;
    double dualImageSeqFpsB()            const;

    // Per-source-channel audio meter readouts. Single-mode reads
    // from m_audio; dual-mode reads from m_dualController->audio()
    // sides A and B respectively. The B accessors return empty in
    // single mode. Driven by m_audioMeterTimer at ~30 Hz.
    QVariantList audioChannelPeaks()  const;
    QStringList  audioChannelNames()  const;
    QVariantList audioChannelPeaksB() const;
    QStringList  audioChannelNamesB() const;

    // Phase 7.4.b — EXR layer switch. Forwards to the decoder
    // (clears buffer + reprimes) and to ProjectManager so the
    // selection persists in the MediaItem. No-op for non-EXR.
    Q_INVOKABLE void setImageSeqLayer(const QString &layer);

    // Phase 7.4.b.4 — unified transport routing. Image sequences
    // and video share QML transport bindings; this layer dispatches
    // to the right subsystem based on m_imageSeqActive. The video
    // decoder mirrors its own play/pause/seek into PlaybackTimer
    // (existing connects); image-seq drives the timer directly.
    Q_INVOKABLE void togglePlayback();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void seekToFrame(int frame);
    // Same as seekToFrame, plus a deferred-resume: when `resume`
    // is true, play() is called by applyPlaylistSeek AFTER the new
    // seek target has been committed. Used by the playlist branch
    // of QML's doRelease so the decoder doesn't play frames at its
    // OLD position during the 50 ms debounce window (frames from
    // the source clip would otherwise flash in the viewport before
    // the user-target frame lands).
    Q_INVOKABLE void seekToFrameAndResume(int frame, bool resume);
    // Time-based seek — the canonical seek entry point. The timeline
    // is conceptually time (seconds); fps is a per-decoder concern.
    // seekToFrame() is now a thin wrapper that converts via
    // activeClockFps() and delegates here. QML's scrubber calls
    // seekToTime() directly so a click at time T always lands at T,
    // regardless of any source's fps.
    Q_INVOKABLE void seekToTime(double seconds);
    // Time-based counterpart of seekToFrameAndResume — deferred
    // resume for the playlist debounce window.
    Q_INVOKABLE void seekToTimeAndResume(double seconds, bool resume);
    // Unified +/- frame stepper that routes to image-seq timeline or
    // video decoder. Pauses playback first (matches old app's
    // StepFrame which calls Pause then advances).
    Q_INVOKABLE void stepFrames(int delta);
    Q_INVOKABLE void seekToStart();
    Q_INVOKABLE void seekToEnd();

    // Playlist-only — jump to the in-point of the previous / next
    // clip on the timeline. No-op outside playlist mode (sourceMode
    // != 1) or when no prev/next clip exists. The transport bar
    // exposes these via dedicated arrow buttons gated on
    // timeline.sourceMode === 1.
    Q_INVOKABLE void seekToPrevClipStart();
    Q_INVOKABLE void seekToNextClipStart();

    // Playlist-only — seek to the first frame of the clip at
    // `itemIndex` in the playlist's `items` list (skips timeline gap
    // clips when mapping the index). Preserves play/pause state — it
    // shares seek logic with the prev/next-clip buttons. No-op outside
    // playlist mode. The inspector clip list calls this on row click.
    Q_INVOKABLE void seekToPlaylistItem(int itemIndex);

    // Fast-seek (FF/RW) — direction = +1 fast-forward, -1 rewind.
    // Hold-down semantics: caller starts on press, calls stop on
    // release. Speed ramps 2x → 32x doubling each second, matching
    // old app's TimelinePlaybackController::Update FastSeek
    // (timeline_playback_controller.cpp:1796).
    Q_INVOKABLE void startFastSeek(int direction);
    Q_INVOKABLE void stopFastSeek();
    Q_INVOKABLE bool isFastSeeking() const { return m_fastSeekTimer; }

    // Install a window-level keyboard filter so transport keys (A/D
    // for held FF/RW, J/L mirrors) fire regardless of which QML item
    // currently has focus. Without this, clicking on the left rail /
    // color panel / inspector moves focus away from heldKeysFocus
    // and the held-key handler in Main.qml stops receiving events.
    // Skipped when a text input is focused so users can still type
    // letters in the save-dialog name field, etc.
    Q_INVOKABLE void installGlobalKeyFilter(QQuickWindow *window);

    // Set the EXR layer for a specific dual side (A or B). In dual
    // mode the single ImageSequenceCache is closed and the existing
    // setImageSeqLayer no-ops; this routes the layer change to the
    // matching DualImageSeqSource (flushes its cache so workers
    // re-decode at the new layer) and persists the choice on the
    // side's MediaItem so a re-load picks it up. Bumps the dual
    // controller's generation counter so the compositor drops its
    // cached per-side texture and re-uploads from the now-empty
    // source cache. No-op when not in dual mode or the side isn't
    // an image sequence.
    Q_INVOKABLE void setDualImageSeqLayer(const QString &side,
                                            const QString &layer);

    // Per-side dual image-sequence cache stride. Mirrors
    // setDualImageSeqLayer: side "A" → sourceA / activeItem, "B" →
    // sourceB / bSource. Applies to that side's DualImageSeqSource and
    // persists on its MediaItem, independent of the other side. The
    // inspector's stride pill routes here when it represents a dual
    // side; single mode uses setImageSeqCacheStride.
    Q_INVOKABLE void setDualImageSeqStride(const QString &side, int stride);
    Q_INVOKABLE int  dualImageSeqStride(const QString &side) const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

public:

    // Borderless fullscreen — macOS-only NSWindow manipulation so
    // we skip the OS zoom-into-Space animation (Q_OS_MACOS gated
    // in the .mm). Pass the QQuickWindow / ApplicationWindow whose
    // NSWindow we want to flip; QML hands `mainWindow` directly.
    // On non-Apple platforms these are no-ops; QML falls back to
    // setVisibility(FullScreen).
    Q_INVOKABLE bool enterBorderlessFullscreen(QWindow *window);
    Q_INVOKABLE bool exitBorderlessFullscreen(QWindow *window);
    Q_INVOKABLE bool isBorderlessFullscreen(QWindow *window) const;

    // Screenshots — capture the next-presented frame from the
    // native player surface and either copy to clipboard or save
    // to ~/Desktop/qcview-screenshot-<timestamp>.png. Returns true
    // on success. Old-app shortcuts: Ctrl+[ clipboard, Ctrl+]
    // desktop (panel_timeline.cpp screenshot calls).
    Q_INVOKABLE bool screenshotToClipboard();
    Q_INVOKABLE bool screenshotToFile();
    Q_INVOKABLE void closeMedia();

    // ---- Phase 3.H.6 Stage B — notes panel surface for QML. ----
    Q_PROPERTY(QVariantList notesList READ notesList NOTIFY notesChanged)
    Q_PROPERTY(bool annotationsAllowed
               READ annotationsAllowed NOTIFY annotationsAllowedChanged)

    // List of notes for the current media as QVariantMaps (one per
    // note: timecode, frame, timestampSeconds, text, addressed,
    // imagePath as an absolute path). Sorted by timecode. Empty
    // when annotations aren't allowed for the active item.
    QVariantList notesList() const;
    bool annotationsAllowed() const {
        return annotationsAllowedForCurrentMedia();
    }

    // Capture the current frame, save to the sidecar's images/
    // dir, and add a new note at the current frame with no text
    // and no strokes. Used by the panel's "+ Add Note" button.
    Q_INVOKABLE void addNoteAtCurrentFrame();
    Q_INVOKABLE void updateAnnotationNoteText(const QString &timecode,
                                                const QString &text);
    Q_INVOKABLE void updateAnnotationNoteAddressed(const QString &timecode,
                                                     bool addressed);
    Q_INVOKABLE void deleteAnnotationNote(const QString &timecode);
    Q_INVOKABLE void seekToAnnotationNote(const QString &timecode);

    // Phase 3.H.6 Stage D — exports. format ∈ {"markdown","html",
    // "pdf"}. For markdown the path is an output directory (the
    // exporter creates a timestamped subfolder inside it); for
    // html / pdf the path is a single file. Returns true on
    // success.
    Q_INVOKABLE bool exportAnnotationNotes(const QString &format,
                                            const QString &path,
                                            const QString &baseName = QString());

    // Phase 3.H.5 — hover-thumbnail. Called from TimelinePanel's
    // hover handler; the cache produces a thumb texture and the
    // renderer composites it into the bottom-left corner of the
    // viewer (gets OCIO with the main image). No-ops while playing.
    // `timelineSec` is in master-fps timeline coords (matches
    // PlaybackTimer::position).
    Q_INVOKABLE void requestHoverThumbnail(double timelineSec);
    // Cleared on pointer-leave or playback start.
    Q_INVOKABLE void clearHoverThumbnail();

    // Drop keyboard focus from any text-editing widget that currently
    // holds it (TextField / TextInput / TextArea / SpinBox inner
    // editor). Wired from QML click handlers in panels that need
    // Del / Backspace shortcuts to fire — without this, clicking out
    // of a focused text input doesn't actually transfer focus, so the
    // text input goes on swallowing key events. The Shortcut layer
    // can't override that ordering on its own (Qt routes keys to the
    // focused widget before resolving Shortcuts, regardless of
    // ApplicationShortcut context).
    Q_INVOKABLE void dropTextInputFocus();
    // Unified "current frame" / "total frames" for QML displays.
    // Both video and image-seq route through the timer in the
    // image-seq case; for video we still read from VideoDecoder
    // (single source of truth where m_frameIndex is authoritative).
    Q_INVOKABLE int  currentFrameUnified() const;
    Q_INVOKABLE int  frameCountUnified() const;
    Q_INVOKABLE bool isPlayingUnified() const;

    // Phase 7.5 B.6.5 — annotation drawing. Toggle on to put the
    // viewport in Annotation mode (mouse drag draws strokes); off
    // to return to Playback. Tool: 1=Freehand, 2=Rectangle,
    // 3=Oval, 4=Arrow, 5=Line (matches qcv::DrawingTool enum).
    // Color is rgba 0..1.
    Q_INVOKABLE void setAnnotationActive(bool on);
    Q_INVOKABLE bool isAnnotationActive() const;
    // Notes-panel open/close gate. Annotations (stored strokes
    // + input) are completely suppressed when the panel is
    // closed, so the user doesn't accidentally see / edit them
    // outside the review workflow.
    Q_INVOKABLE void setNotesPanelVisible(bool on);
    Q_INVOKABLE void setAnnotationTool(int tool);
    Q_INVOKABLE void setAnnotationColor(double r, double g, double b, double a);
    Q_INVOKABLE void setAnnotationStrokeWidth(double w);

    // Phase 7.5 B.6.6 — safety-guide overlay. setSafetySvg(path)
    // accepts either a built-in name (resolved against the bundled
    // assets/safety/<name>.svg) or an absolute path. setSafetySvg("")
    // clears the overlay.
    Q_INVOKABLE void   setSafetySvg(const QString &nameOrPath);
    Q_INVOKABLE void   setSafetyOpacity(double opacity);
    Q_INVOKABLE void   setSafetyColor(double r, double g, double b);
    Q_INVOKABLE void   setSafetyLineWidth(double pixels);
    Q_INVOKABLE QStringList availableSafetySvgs() const;

    // Phase 3.A — Inspector helper actions.
    // Copy: QGuiApplication clipboard. Reveal: platform file-manager
    // "show this file" (Finder on macOS, Explorer on Windows, the
    // default file-manager on Linux). Reveal does NOT open the file
    // itself — that's openExternalPath below.
    Q_INVOKABLE void copyTextToClipboard(const QString &text);
    Q_INVOKABLE void revealInFileManager(const QString &path);

    // Launch the OS default app for `path` — Adobe project files
    // (.aep / .prproj) open in After Effects / Premiere if installed,
    // otherwise the shell picks whatever's registered. Goes through
    // QDesktopServices::openUrl(QUrl::fromLocalFile) so the URL is
    // built correctly per platform (the QML-side "file://" + path
    // string concat that was here before missed the third slash on
    // Windows local paths and broke entirely for UNC).
    Q_INVOKABLE void openExternalPath(const QString &path);

    // Convert internal `/`-separated path to the OS-native form
    // (backslashes on Windows, no-op elsewhere). Use at every UI
    // display site + every Copy-path action so what the user sees +
    // pastes matches what their OS expects. Qt keeps paths in
    // forward-slash form internally even on Windows because most
    // Qt + FFmpeg APIs accept either; only when the value leaves
    // the app (display, clipboard, ShellExecute arg) does the
    // native form actually matter.
    Q_INVOKABLE QString toNativeSeparators(const QString &path) const;

    // Convert a QUrl (typically from a FileDialog selectedFile or a
    // DropArea drop.urls[i]) to the OS-native file path string our
    // backends (FFmpeg, QFile, ImageSequenceCache) expect. Centralized
    // because three URL shapes need handling and getting it right is
    // surprisingly hairy:
    //   file:///C:/foo  → C:/foo                 (Windows local)
    //   file:///path    → /path                  (POSIX local)
    //   file://server/… → //server/…             (UNC; this is the
    //                                              one most QML
    //                                              call sites missed
    //                                              before, leaving
    //                                              "server/…" which
    //                                              QFile rejects.)
    // Percent-encoded names round-trip through decodeURIComponent.
    Q_INVOKABLE QString urlToOsPath(const QUrl &url) const;

    // Restore + raise the UI window. Called by the single-instance
    // handler in main.cpp when a secondary qcview launch forwards its
    // file args to us — we should pop to the foreground so the user
    // sees the file they just double-clicked land in the viewport.
    Q_INVOKABLE void raiseUiWindow();

public slots:
    void detach();
    void reattach();

signals:
    void recentMediaChanged();
    void recentProjectsChanged();
    void detachedChanged();
    void compositorModeChanged();
    void dualControllerChanged();
    void backgroundModeChanged();
    void loopEnabledChanged();
    void timelineHoverThumbsEnabledChanged();
    void audioSyncOffsetMsChanged();
    void dualAudioSyncOffsetMsChanged();
    // Fires when audioRoutingScopeMediaItemId() may return a
    // different value: activeItemId switches, OR (in playlist mode)
    // the active playlist clip transitions. Bound by the inspector
    // routing pill so its display + click-target follow whatever
    // clip is actually playing.
    void audioRoutingScopeChanged();
    void playlistCurrentItemIndexChanged();
    void inOutPointsChanged();
    void splitPosChanged();
    void hdrModeChanged();
    void brightnessChanged();
    // Phase 3.H.6 — re-emitted from AnnotationManager.notesChanged
    // for QML bindings, plus fires when the active media swaps to
    // a different sidecar. annotationsAllowedChanged tracks the
    // single-mode gate (compositor / playlist / active type).
    void notesChanged();
    void annotationsAllowedChanged();
    // Phase 3.H.6 Stage E — fired when ONLY the on-disk
    // annotated thumbnail file content changed (debounced
    // post-stroke save). The notes-list structure is
    // unchanged, so the model must NOT rebuild. The Notes
    // panel listens per-card and bumps a local thumbVersion
    // counter to force an in-place Image reload, which keeps
    // the old pixmap visible while the new one decodes async
    // → no flash on stroke / erase commits.
    void annotatedThumbReady(const QString &timecode);
    // Phase 3.H.6 — F key from the C++ event filter. Main.qml
    // listens and calls root.toggleFullscreen() (which is
    // QML-side state). Routed through C++ so the filter's
    // text-input bailout still applies.
    void fullscreenToggleRequested();
    void imageSeqBufferStatusChanged();
    void imageSeqCacheStrideChanged();
    void imageSeqActiveChanged();
    // Dual-mode per-side image-seq buffer status notify. Fired by
    // pollImageSeqBufferStatus when a delta is detected on either
    // side. Shared across all dualImageSeq* properties so QML
    // bindings re-evaluate together.
    void dualImageSeqStatusChanged();
    void audioActiveChanged();
    // Bumped at ~30 Hz by m_audioMeterTimer when peaks have moved.
    // Inspector level meters bind via Q_PROPERTY notify.
    void audioMetersChanged();
    // Phase 7.4.b.4 pull-model: image-seq playhead crossed a frame
    // boundary. PlayerRhiItem connects to this and calls update()
    // so the renderer pulls the new frame from the cache. No
    // payload — the renderer reads the playhead from the cache.
    void imageSeqFrameAdvanced(int frame);
    // Cache pointer changed (set on startImageSequence, cleared on
    // stopImageSequence). QML binds this to PlayerRhiItem::imageSeqCache.
    void imageSeqCacheChanged();
    // Width/height/fps/frameCount changed (sequence opened/closed).
    void imageSeqMetadataChanged();

private slots:
    void syncPlayerGeometry();

private:
    // Builds the player QQuickWindow and parents/configures it as
    // the docked child. Used both during initial startup and to
    // rebuild the window when HDR mode changes (Qt only reads the
    // `_qt_sg_hdr_format` property at swapchain creation time, so
    // a real flip requires destroying + recreating the window).
    bool createPlayerWindow();
    void destroyPlayerWindow();

    // Phase 7.4.b.4 — image-sequence playback. Builds an
    // ImageSequenceCache for the given MediaItem; the renderer pulls
    // pixels from it per present (PlayerRhiItem::imageSeqCache).
    // No publish event, no driver-timer-based decoder tick — only
    // a timer pump that advances PlaybackTimer's wall clock.
    // Phase 3.H.4 — `prewarmedCache` (when non-null) is used as-is
    // instead of constructing + initializing a fresh one. Used by
    // the playlist orchestrator's prewarm-hit path so a 6K-EXR
    // clip's read-ahead window is already populated when the
    // boundary cross fires. Caller MUST pre-call
    // ImageSequenceCache::updatePlayhead to seed the right window.
    void startImageSequence(const MediaItem &item,
                            std::unique_ptr<ImageSequenceCache>
                                prewarmedCache = nullptr);
    void stopImageSequence();

    // Phase 3.H.1 — kick a Playlist load. Resolves entries against
    // the pool, builds the playlist timeline, and (for Stage 1) opens
    // ONLY the first non-gap clip's source on m_videoDecoder so
    // playback runs inside that clip's range. Boundary-crossing
    // playback lands in 3.H.2 and replaces the inner-loop step here.
    void startPlaylist(const MediaItem &item);

    // Phase 3.H.2 — rebuild the playlist timeline from the active
    // MediaItem WITHOUT reloading any decoder. Used to refresh
    // 0-duration clips when their source metadata arrives async
    // shortly after createPlaylist. Caller must guarantee the
    // timeline is in playlist mode + the user hasn't edited yet.
    void rebuildPlaylistTimelineOnly();

    // Phase 3-prep — single chokepoint for "tear down whatever
    // media is currently active." Called at the start of every
    // load path (loadRequested, file dialog, drag-drop, closeMedia)
    // so subsequent state transitions never observe stale playback /
    // cache / timeline pointers.
    void closeActiveMedia();

    // Phase 7.7 Stage 4 — cold-transition helpers. teardown closes
    // the single-flow A + B decoders, image-seq cache, and audio,
    // and clears the renderer's single-flow state (so the dual
    // compositor's drawFrame branch starts from a clean slate).
    // rebuild reopens A from whatever MediaItem is currently active
    // in the project, replicating the loadRequested path.
    void teardownSingleFlowForDual();
    void rebuildSingleFlowFromActiveItem();

    // Platform-complete teardown of the dual island back to single-flow
    // renderer state, WITHOUT rebuilding any single-flow media (no
    // decoder reopen, no loadRequested re-emit). Sets m_compositorMode
    // to 0. Shared by the loadRequested handler (which then loads the
    // new A itself), setCompositorMode's Dual→Single branch (which then
    // calls rebuildSingleFlowFromActiveItem), and the projectReplaced
    // handler. Consolidating here also guarantees the Windows
    // setDualFrameSource(nullptr)-before-adapter-reset ordering on every
    // teardown path.
    void tearDownDualIslandToSingleState();

    // Polls the image-seq cache's buffered counters and emits
    // imageSeqBufferStatusChanged on any change. Driven from the
    // 30 Hz pump and from positionChanged so the cache strip
    // refreshes during seeks (frameAdvanced doesn't fire when a
    // sub-frame seek lands on the same integer frame index) and
    // during paused-fill (cache thread continues working after the
    // user releases the scrubber).
    void pollImageSeqBufferStatus();

    // Mismatched-clip-length handling for dual-source playback.
    // Each helper below covers one piece:
    //
    //   sourceDurationA / sourceDurationB
    //     Natural duration of each side in seconds (image-seq cache
    //     for A when active, video decoder otherwise). 0 when no
    //     media is loaded for that side.
    //
    //   applyMasterDuration
    //     Overrides timer.duration to max(durA, durB) when both
    //     sides are loaded — so the timeline + transport reflect
    //     the longer clip rather than truncating at the shorter.
    //     Called whenever a side's media changes.
    //
    //   pushSourceActivity
    //     Compares master playhead to each side's natural duration
    //     and pushes per-side activity flags to the renderer. The
    //     compositor renders an inactive side's region transparent
    //     (background shows through) so the longer clip plays alone
    //     past the shorter clip's end. Connected to
    //     PlaybackTimer::positionChanged.
    double sourceDurationA() const;
    double sourceDurationB() const;
    void   applyMasterDuration();
    void   pushSourceActivity();

private:
    QQmlApplicationEngine *m_engine;
    QPointer<QQuickWindow> m_uiWindow;
    // Phase 7.5: switched from QPointer<QQuickWindow> to QPointer<QWindow>
    // because the player window can now be either:
    //   - a QQuickWindow (legacy QML path, QCV_NATIVE_PLAYER undef)
    //   - a qcv::PlayerWindow (native QWindow subclass, QCV_NATIVE_PLAYER def)
    // All call sites already used QWindow-base methods (setFormat,
    // setParent, show/hide, setGeometry, setFlags); only the cast in
    // createPlayerWindow's QML path was QQuickWindow-specific.
    QPointer<QWindow>      m_playerWindow;
    QPointer<QQuickItem>   m_centerStage;
    QRect                  m_lastDetachedGeometry;
    bool                   m_detached = false;
    int                    m_compositorMode = 0;   // PlayerRhiItem::Single
    int                    m_backgroundMode = 0;   // BackgroundMode::Black
    bool                   m_loopEnabled    = false;
    // User-intent flag for the loop EOS handler. The video decoder's
    // own isPlaying() can flip to false momentarily during the EOS
    // → seek(0) → resume cycle; a short looped clip cycles fast
    // enough (~4 Hz for 6 frames at 24 fps) that a user-pressed
    // space races with the EOS auto-resume. We capture intent
    // explicitly so the handler can defer when the user has paused.
    bool                   m_userWantsPlayback = false;
    int                    m_inPoint        = -1;
    int                    m_outPoint       = -1;
    qreal                  m_splitPos = 0.5;
    VideoDecoder          *m_videoDecoder = nullptr;   // owned via QObject parent
    VideoDecoder          *m_videoDecoderB = nullptr;  // owned via QObject parent — Source B
    // Phase 7.7 Stage 3 — dual playback island controller. Owned via
    // unique_ptr (parented to `this` would also work but ordering of
    // destruction relative to the renderer pointer matters; this lets
    // exitDualTestMode tear it down explicitly before the renderer
    // is destroyed).
    std::unique_ptr<qcv::dual::DualPlaybackController> m_dualController;
#if !defined(Q_OS_MACOS) && !defined(__APPLE__)
    // Phase F.2.11.d — qcv_render-side IDualFrameSource adapter for the
    // Windows D3D11 dual flow. Wraps m_dualController; macOS Metal uses
    // the existing setDualController(void *) seam, so the adapter
    // doesn't exist on Apple. Created in setCompositorMode after the
    // controller opens; reset in the Dual→Single branch before the
    // controller does (renderer holds a non-owning IDualFrameSource *).
    std::unique_ptr<qcv::WindowManagerDualSourceAdapter> m_dualSourceAdapter;
#endif
    ScrubDecoder          *m_scrubDecoder = nullptr;   // owned via QObject parent
    OCIOConfigManager     *m_ocio = nullptr;           // owned via QObject parent
    PresetManager         *m_presets = nullptr;        // owned via QObject parent
    AudioPlayer           *m_audio = nullptr;          // owned via QObject parent
    ProjectManager        *m_project = nullptr;        // owned via QObject parent
    // Phase 7.6 — Recent lists. Loaded on demand from QSettings;
    // mutated by addRecent* + persisted same step. Mutable so
    // const getters can populate them on first read.
    mutable QStringList    m_recentMedia;
    mutable QStringList    m_recentProjects;
    mutable bool           m_recentsLoaded = false;
    void ensureRecentsLoaded() const;
    QString                m_interFamily    = QStringLiteral("sans-serif");
    QString                m_monoFamily     = QStringLiteral("monospace");
    QString                m_phosphorFamily = QStringLiteral("Phosphor");
    TimelineController    *m_timeline = nullptr;       // owned via QObject parent
    int                    m_hdrMode = SdrSRgb;
    double                 m_brightness = 1.0;

    // Phase 7.5 B.6.5 — annotator owned by WindowManager so its
    // lifetime spans player-window recreates (HDR mode switches).
    // Wired into the player window (input forwarding) and the native
    // renderer (stroke rendering) on createPlayerWindow.
    std::unique_ptr<ViewportAnnotator> m_annotator;
    // Phase 3.H.6 Stage A — per-media notes. Owns the on-disk
    // sidecar (.qcview/<media>/notes.json + images/). Lifecycle
    // tracks the active MediaItem; clears on dual / playlist
    // modes (annotations are gated to single video / single image
    // sequence only). Strokes finalize through the annotator's
    // callback into addNote / updateNoteAnnotationData.
    std::unique_ptr<AnnotationManager>  m_annotationManager;
    // Debounce timer for the annotated thumbnail save. captureScreenshot
    // blocks up to 250 ms waiting for the next presented frame; doing
    // that on every stroke release feels laggy when the user draws
    // multiple strokes in quick succession. Each stroke restarts the
    // timer; we save once after ~400 ms of quiet so the captured frame
    // includes all strokes drawn in this burst.
    QTimer                              *m_annotatedSaveTimer = nullptr;
    QString                              m_pendingAnnotatedTimecode;
    QList<AnnotationUndoEntry>           m_annotationUndoStack;
    QList<AnnotationUndoEntry>           m_annotationRedoStack;
    bool                                 m_notesPanelVisible = false;

    // Phase 7.5 B.6.6 — safety-guide overlay. Owned by WindowManager;
    // wired into the renderer on createPlayerWindow.
    std::unique_ptr<SafetyOverlay>     m_safetyOverlay;

    // Phase 7.4.b.4 — image-sequence path. Single ImageSequenceCache
    // for all formats (EXR/TIFF/PNG/JPEG), mirroring old app's
    // timeline_cache.cpp:330-433 dispatch. Per-format loader plugs
    // INTO the cache as IImageLoader.
    std::unique_ptr<ImageSequenceCache>   m_imageSeqCache;
    // Phase 3.H.5 — separate, fixed-size LRU cache for the hover-
    // thumbnail corner overlay. Multiple workers, dedicated FFmpeg
    // contexts; bounds I/O contention against the main playback
    // path. Notified on play/pause to drop to 1 worker during
    // playback. The atomic handle is read by the render thread
    // each frame (0 = no overlay).
    std::unique_ptr<TimelineThumbnailCache> m_thumbCache;
    std::atomic<std::uint64_t>            m_currentHoverThumbHandle{0};
    std::atomic<int>                      m_currentHoverThumbW{0};
    std::atomic<int>                      m_currentHoverThumbH{0};
    // Phase 3.H.5 dual-view extension — B-side hover thumb. Only
    // populated when m_dualController is active. Renderer composites
    // into the bottom-right corner symmetrically with A's BL.
    std::atomic<std::uint64_t>            m_currentHoverThumbHandleB{0};
    std::atomic<int>                      m_currentHoverThumbWB{0};
    std::atomic<int>                      m_currentHoverThumbHB{0};
    std::string                           m_hoverPathB;
    int                                   m_hoverFrameB = 0;
    QTimer                               *m_thumbUploadDrainTimer = nullptr;

    // Audio peak-level pump. Created on first audio open; ticks at
    // ~30 Hz polling AudioPlayer / DualAudioMixer for per-channel
    // peaks and emits audioMetersChanged when any value moved past
    // a small dB threshold. Stopped when no audio is loaded.
    QTimer                               *m_audioMeterTimer = nullptr;
    // Snapshot of the last emitted peak vector (interleaved A then
    // B) so the timer suppresses no-change emits.
    std::vector<float>                    m_lastEmittedPeaks;
    void pollAudioMeters();
    // Last-requested hover key, kept so the drain timer can re-poll
    // the cache after the worker has finished (the QML side fires
    // requestHoverThumbnail() per pointer-move; if the user holds
    // the cursor still while the worker is mid-decode, we'd miss
    // the texture without this re-poll).
    std::string                           m_hoverPath;
    int                                   m_hoverFrame = 0;
    std::atomic<bool>                     m_hoverActive{false};

    // Resolves a timeline position to (source_path, source_frame).
    // For image-seq sources, the path is the per-frame file path
    // (with `?layer=` for EXR multi-layer); source_frame is 0.
    // Returns false if no resolvable source at that timeline pos.
    // `side` selects which timeline track to resolve from in dual
    // mode ("A" or "B"); single + playlist modes only have track A
    // and resolveHoverKey returns false for any other side.
    bool resolveHoverKey(double timelineSec,
                         const QString &side,
                         std::string &outPath, int &outFrame) const;

    // Pushes the current hover-thumb atomic state to the renderer
    // so it can composite the thumb into the corner ROI on its
    // next draw. Called from requestHoverThumbnail / clearHover-
    // Thumbnail / the upload-drain re-poll.
    void pushHoverThumbToRenderer();

    // Forward the loading-spinner flag to the active player renderer.
    void setLoadingActive(bool on);

    // Apply each dual image-sequence side's stored per-item cache
    // stride to its DualImageSeqSource. Called on dual entry so dual
    // honors the same stride preference single flow does. No-op when
    // not in dual mode or a side isn't an image sequence.
    void applyDualImageSeqStride();

    // ---- Phase 3.H.6 annotation wiring (Stage A) ----
    // Annotations are gated to single video / single image-seq
    // only. Returns true when the active item is one of those AND
    // we're not in dual / playlist mode.
    bool annotationsAllowedForCurrentMedia() const;

    // Pushes the active media path into AnnotationManager so its
    // sidecar (.qcview/<media>/notes.json) gets loaded; clears
    // notes when annotations aren't allowed for the new media.
    void syncAnnotationManagerToActiveMedia();

    // Phase 3.H.6 Stage E — eraser tool. Bridge from
    // ViewportAnnotator's eraseAt callback. `normalizedPos` is in
    // the same 0..1 viewport coords strokes are stored in. Hits
    // are decided by stroke bounding-box + a small tolerance; the
    // first match in the current frame's notes is removed and the
    // annotated thumb refreshes.
    void onEraseAt(QPointF normalizedPos);

    Q_INVOKABLE bool annotationUndo();
    Q_INVOKABLE bool annotationRedo();
    Q_INVOKABLE bool annotationUndoAvailable() const {
        return !m_annotationUndoStack.isEmpty();
    }
    Q_INVOKABLE bool annotationRedoAvailable() const {
        return !m_annotationRedoStack.isEmpty();
    }
    Q_INVOKABLE void cancelActiveStroke();

    // Bridge from ViewportAnnotator's stroke-finalized callback into
    // AnnotationManager: ensures a note exists at the current frame
    // (creates one if not), serializes the new stroke into the note's
    // annotation_data alongside any pre-existing strokes.
    void onStrokeFinalized(std::unique_ptr<ActiveStroke> stroke);

    // Two thumbnails per note:
    //   - "<imagesFolder>/note_<TC>.png"            (clean: bare frame)
    //   - "<imagesFolder>/note_<TC>_annotated.png"  (with strokes baked in)
    // The clean PNG is the source of truth for re-rasterizing the
    // annotated overlay after stroke edits / undo / redo, and it's
    // what export pipelines should embed when the user wants the
    // bare frame. Old app uses the same naming convention so port
    // sidecars round-trip with old-app sidecars.
    //
    // saveNoteCleanThumbnail is idempotent — writes only when the
    // file doesn't already exist. The renderer's stored-stroke
    // mesh must NOT include this note's strokes at the moment of
    // capture (caller's responsibility — e.g. capture before
    // updateNoteAnnotationData pushes new strokes through).
    void saveNoteCleanThumbnail(const QString &timecode);
    // saveNoteAnnotatedThumbnail always overwrites; expects the
    // stored-stroke mesh to be up-to-date so the captured frame
    // includes the strokes.
    void saveNoteAnnotatedThumbnail(const QString &timecode);

    // Build the combined stored-stroke list for the current frame
    // and push it to the renderer for compositing on top of the
    // canvas. Re-runs on note changes + on frame change.
    void rebuildStoredAnnotationMesh();
    // Pure clock pump — drives PlaybackTimer::update() at 30 Hz so
    // wall-clock advances cross frame boundaries (frameAdvanced
    // signal). NO publish work happens here — the renderer pulls
    // frames from m_imageSeqCache on its own thread per present.
    QTimer                               *m_imageSeqDriverTimer = nullptr;

    // Fast-seek state — single timer drives both rewind and FF; the
    // sign of m_fastSeekDir picks direction (-1/+1). Time anchors
    // are in steady_clock seconds; recomputed each tick.
    QTimer                               *m_fastSeekTimer = nullptr;
    int                                   m_fastSeekDir = 0;
    double                                m_fastSeekSpeed = 0.0;
    QElapsedTimer                         m_fastSeekClock;
    qint64                                m_fastSeekStartMs = 0;
    qint64                                m_fastSeekLastMs  = 0;
    double                                m_fastSeekPositionSec = 0.0;
    // True between startFastSeek and stopFastSeek. Two jobs:
    //   1. Suppress the VideoDecoder.currentFrameChanged → timer
    //      mirror so an in-flight publish from an abandoned seek
    //      can't drag the timer back to a stale source position
    //      (timer must lead during the gesture).
    //   2. Anchor the "did anything happen during the gesture"
    //      check that drives the final seekToFrame on release.
    bool                                  m_fastSeekActive = false;

    // Phase 7.4.b.2 Stage A — buffer-status emit throttle. We only
    // emit imageSeqBufferStatusChanged when (ahead, behind, size)
    // actually changes; previously emitted at 60 Hz unconditionally,
    // which cascaded QML rebindings on the cache strip + inspector
    // every tick.
    int                                   m_lastBufferedAhead   = -1;
    int                                   m_lastBufferedBehind  = -1;
    int                                   m_lastBufferSize      = -1;
    int                                   m_lastFailedFrameCount = -1;
    // Dual-mode per-side image-seq cache stats — populated by
    // pollImageSeqBufferStatus when m_dualController is alive.
    // -1 sentinel for delta detection on first poll.
    int                                   m_lastDualAheadA      = -1;
    int                                   m_lastDualBehindA     = -1;
    int                                   m_lastDualFrameCountA = -1;
    bool                                  m_lastDualActiveA     = false;
    double                                m_lastDualFpsA        = 0.0;
    int                                   m_lastDualAheadB      = -1;
    int                                   m_lastDualBehindB     = -1;
    int                                   m_lastDualFrameCountB = -1;
    bool                                  m_lastDualActiveB     = false;
    double                                m_lastDualFpsB        = 0.0;
    // Polls m_dualController's sources every 33 ms while dual is
    // active. Started in setCompositorMode Single→Dual entry,
    // stopped in Dual→Single exit. Fires pollImageSeqBufferStatus.
    QTimer                               *m_dualBufferPollTimer = nullptr;

    bool                                  m_imageSeqActive      = false;
    // Last source frame pushed to the image-seq cache. The cache
    // mirror runs off PlaybackTimer::positionChanged and converts
    // position × cache.fps() — this dedupes so updatePlayhead +
    // imageSeqFrameAdvanced only fire when the source frame actually
    // changes. -1 = nothing pushed yet.
    int                                   m_lastImageSeqFrame   = -1;
    // Audio-only mode: timer runs in wall-clock mode driven by the
    // wallclock pump (m_imageSeqDriverTimer reused), and transport
    // toggles drive both timer + AudioPlayer directly.
    bool                                  m_audioActive         = false;

    // True while startImageSequence is mid-flight (the brief gap
    // where the previous video decoder closes and the new sequence
    // metadata hasn't been pushed yet). The metadataChanged hook
    // checks it and skips rebuilding the timeline with the cleared
    // metadata that the prior decoder's close() emits, so the UI
    // doesn't briefly empty out before the real sequence metadata
    // lands.
    bool                                  m_suppressTimelineRebuild = false;

    // ---- Phase 3.H.2 — playlist orchestrator state ----
    // True when a Playlist MediaItem is the active source. Drives
    // boundary-cross detection in the currentFrameChanged handlers
    // and routes EndOfStream to the next-clip swap rather than the
    // single-mode loop wrap.
    bool                                  m_playlistActive          = false;
    // Index into the playlist's track-A clip vector (gaps included)
    // pointing at the currently-loaded clip. -1 when no clip is
    // loaded yet (e.g., empty playlist or before first advance).
    int                                   m_playlistCurrentClipIndex = -1;
    // Re-entry guard for the boundary-cross hot path. open() runs
    // synchronously and emits its own currentFrameChanged from the
    // initial seek-to-zero, which would re-enter the boundary check
    // before we've finished swapping. Set true around the swap so
    // the handler short-circuits.
    bool                                  m_playlistAdvancing       = false;
    // Coalesce rapid playlist seekToFrame requests. Each user click
    // on the timeline calls seekToFrame; in playlist mode that may
    // trigger playlistAdvanceToClip — full VideoDecoder + audio
    // teardown + reopen, ~200-500 ms of GUI-thread work on a
    // multi-stream broadcast file. Without coalescing, a 6-7 click
    // burst stacks decoder cleanups in the async cleanup queue
    // faster than they drain (the cascade behind the rapid-seek
    // crash). Per-click handler stores the latest target frame and
    // (re)starts the 50 ms single-shot timer; the timer's slot
    // services only the final stored target. 50 ms is invisible
    // compared to the per-click decoder cost it replaces.
    QTimer                               *m_playlistSeekDebounce    = nullptr;
    // Pending playlist seek target, in TIMELINE SECONDS. Negative =
    // nothing pending. Stored in seconds (not frames) so the seek
    // path stays fps-agnostic end to end.
    double                                m_pendingPlaylistSeekSec  = -1.0;
    // Target SOURCE frame of an in-flight playlist seek. Set by
    // applyPlaylistSeek right before m_videoDecoder->seekToFrame(),
    // cleared by the currentFrameChanged mirror once the decoder
    // lands within tolerance. While >= 0 the mirror skips updates
    // so the playhead doesn't briefly jump to the new clip's
    // sourceIn from keyframe-rounded intermediate frames before
    // settling at the user-target source frame.
    int                                   m_playlistSeekTargetFrame = -1;
    // Pair of state for the deferred-resume seek path used by the
    // playlist branch of seekToFrame. The QML release handler used to
    // call WindowManager.videoDecoder.play() immediately after a
    // playlist seekToFrame(), which meant the decoder was playing
    // from its OLD position for the entire 50 ms debounce window —
    // visible as a frame from the current clip flashing in the
    // viewport before the user-target frame lands. Holding the
    // resume here defers the play() until applyPlaylistSeek has
    // actually committed the new seek target.
    bool                                  m_resumePlayAfterPlaylistSeek = false;
    // Takes a TIMELINE position in SECONDS (the debounce stores
    // m_pendingPlaylistSeekSec). Resolves the target clip, swaps if
    // needed, seeks the active decoder to the right source frame.
    void applyPlaylistSeek(double pos);

    // Framerate of whatever clock the active mode runs on — used
    // only to convert the legacy frame-based seekToFrame() into the
    // time-based seekToTime(). Dual → masterFps; single video →
    // decoder fps; image-seq / audio / playlist → timeline timer
    // fps. NOT a master-of-all-modes fps; each mode owns its own.
    double activeClockFps() const;

    // Phase 3.H.4 — pre-warm cache for the next image-seq clip.
    // Initialized ~5 s before the boundary so the CacheThread has
    // time to populate the read-ahead window (~3 s of frames at
    // 24 fps default config). On boundary cross, this cache is
    // moved into m_imageSeqCache directly — no cold initialize.
    // m_imageSeqCachePrewarmedIndex is the track-A clip index this
    // cache was built for; -1 when none.
    std::unique_ptr<ImageSequenceCache>   m_imageSeqCachePrewarm;
    int                                   m_imageSeqCachePrewarmedIndex = -1;
    static constexpr double               kImageSeqPrewarmLeadSec = 5.0;

    void prewarmImageSeqIfNeeded();
    void cancelImageSeqPrewarm();

    // Resolve the currently-loaded playlist clip from the timeline.
    // Returns nullptr if not in playlist mode or index out of range.
    const Clip *playlistActiveClip() const;
    // Open clip at trackClipIndex on m_videoDecoder. Skips gaps by
    // advancing forward / backward; handles end-of-playlist wrap
    // (loop) or pause (no loop). Returns the index actually loaded
    // (post gap-skip) or -1 if no clip could be loaded.
    // `autoplay` controls whether to call play() after the open +
    // seek settles. EOS-driven boundary crosses pass true; initial
    // load and user-driven scrubs pass false (or the playing state
    // they captured before the seek).
    // targetTimelinePos: when >= 0, seek the freshly-opened decoder
    // directly to the source frame corresponding to this timeline
    // position instead of to clip.sourceIn. Used by applyPlaylistSeek
    // for user-initiated cross-clip seeks so the decoder doesn't
    // decode + present the clip's first frame just to be re-seeked
    // moments later (read on screen as a "frame 1" flash in the
    // viewport before the user-target frame lands).
    int  playlistAdvanceToClip(int trackClipIndex, bool autoplay,
                               double targetTimelinePos = -1.0);
    // Find the next non-gap clip index after `from` in the given
    // direction (+1 forward, -1 backward). Returns -1 if none.
    int  playlistFindNonGapClip(int from, int direction) const;

public:
    // Phase 3.H.2 — fast scrub path during drag in playlist mode.
    // Mirrors the single-mode pattern: lightweight ScrubDecoder
    // preview while dragging; the heavy commit happens via
    // WindowManager::seekToFrame on release. `frame` is in TIMELINE
    // (master-fps) coordinates; the active clip is swapped on cross
    // and `srcFrame = (timelinePos - clipStart + sourceIn) * srcFps`
    // is fed to ScrubDecoder. No-op outside playlist mode.
    Q_INVOKABLE void scrubToTimelineFrame(int frame);

    // ---- Live viewport feedback during clip slip/trim edits ----
    // The QML timeline edit gesture brackets a drag with these:
    // beginEditScrub on drag-start, previewEditScrubFrame per delta
    // (sourceFrame is the edited clip's target source frame — the
    // playhead-translated frame for slip, the dragged edge for trim),
    // endEditScrub on release/cancel to restore the playhead frame.
    // Routes per mode: dual → DualPlaybackController per-side scrub
    // (no master-clock move); playlist video → the shared ScrubDecoder.
    // sourceFrame < 0 is a no-op (slip with playhead off the clip).
    // Only active in dual or playlist mode; no plain-single path.
    Q_INVOKABLE void beginEditScrub();
    Q_INVOKABLE void previewEditScrubFrame(const QString &trackId,
                                           const QString &clipId,
                                           int sourceFrame);
    Q_INVOKABLE void endEditScrub();

private:
    bool m_editScrubActive       = false;
    int  m_editScrubRestoreMaster = 0;   // dual: pre-edit master frame
    int  m_editScrubRestoreFrame  = -1;  // playlist: pre-edit source frame

public:
};

} // namespace qcv
