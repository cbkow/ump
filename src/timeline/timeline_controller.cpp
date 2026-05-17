#include "timeline_controller.h"

#include "timeline_edits.h"

#include "playback_timer.h"
#include "timeline_flattener.h"
#include "project/media_item.h"

#include <QUuid>
#include <QFileInfo>
#include <QtMath>
#include <algorithm>

namespace qcv {

namespace {
// Track ids reserved for the dual-source roles. The primary track
// is the one the user "actively" loaded (image-seq or video A);
// the secondary is video B in the compositor's srcB slot. These
// strings are how QML and the controller identify the two lanes
// without leaking a numeric index that would shift if (later) we
// add audio tracks between them.
constexpr const char *kTrackIdA = "A";
constexpr const char *kTrackIdB = "B";

QVariantMap clipToMap(const Clip &c)
{
    QVariantMap m;
    m.insert(QStringLiteral("id"),          c.id);
    m.insert(QStringLiteral("name"),        c.name);
    m.insert(QStringLiteral("startTime"),   c.startTime);
    m.insert(QStringLiteral("duration"),    c.duration);
    m.insert(QStringLiteral("sourceIn"),    c.sourceIn);
    m.insert(QStringLiteral("sourceOut"),   c.sourceOut);
    m.insert(QStringLiteral("isGap"),       c.isGap);
    m.insert(QStringLiteral("mediaKind"),   static_cast<int>(c.mediaKind));
    m.insert(QStringLiteral("mediaPath"),   c.mediaPath);
    m.insert(QStringLiteral("mediaItemId"), c.mediaItemId);
    m.insert(QStringLiteral("sourceFps"),      c.sourceFps);
    m.insert(QStringLiteral("sourceDuration"), c.sourceDuration);
    m.insert(QStringLiteral("hasAudio"),       c.hasAudio);
    return m;
}

QVariantMap trackToMap(const Track &t)
{
    QVariantMap m;
    m.insert(QStringLiteral("id"),       t.id);
    m.insert(QStringLiteral("name"),     t.name);
    m.insert(QStringLiteral("isVideo"),  t.isVideo);
    m.insert(QStringLiteral("visible"),  t.visible);
    m.insert(QStringLiteral("locked"),   t.locked);
    m.insert(QStringLiteral("zIndex"),   t.zIndex);
    QVariantList clips;
    clips.reserve(t.clips.size());
    for (const Clip &c : t.clips) clips.append(clipToMap(c));
    m.insert(QStringLiteral("clips"), clips);
    return m;
}
} // namespace

TimelineController::TimelineController(QObject *parent)
    : QObject(parent)
    , m_timer(new PlaybackTimer(this))
{
}

TimelineController::~TimelineController() = default;

void TimelineController::loadSingleMedia(const MediaItem &item,
                                          double durationSeconds,
                                          double frameRate,
                                          bool   hasAudio)
{
    // Per Guide 02 §1: SingleMedia timeline = 1 locked video
    // track with one clip spanning the whole source. Audio (if
    // present in the source file) lives on its own track so the
    // flattener's audibleClipsAt logic finds it independently
    // of the video track's mute state.
    m_timer->pause();
    m_timer->seek(0.0);

    m_timeline = Timeline{};
    m_playlistMediaItemId.clear();
    m_timeline.name      = item.name;
    m_timeline.mode      = SourceMode::SingleMedia;
    m_timeline.policy    = EditPolicy{};   // default: no ripple, no overlaps
    m_timeline.frameRate = (frameRate > 0.0) ? frameRate : 24.0;
    m_timeline.duration  = (durationSeconds > 0.0) ? durationSeconds : 0.0;

    Track videoTrack;
    // Track id "A" identifies this as the primary / Source A track.
    // Dual-source mode adds a sibling "B" track via
    // loadSecondarySource; QML keys off these ids to render two
    // stacked lanes.
    videoTrack.id       = QString::fromLatin1(kTrackIdA);
    videoTrack.name     = QStringLiteral("A");
    videoTrack.isVideo  = true;
    videoTrack.visible  = true;
    videoTrack.locked   = true;          // SingleMedia → no editing
    videoTrack.zIndex   = 1;

    Clip videoClip;
    videoClip.id          = QUuid::createUuid().toString(QUuid::WithoutBraces);
    videoClip.name        = item.name;
    videoClip.startTime   = 0.0;
    videoClip.duration    = m_timeline.duration;
    videoClip.sourceIn    = 0.0;
    videoClip.sourceOut   = m_timeline.duration;
    // Phase 7.4: image sequences route through the same single-clip
    // shape but flag their media kind so the renderer / scrubber
    // know to dispatch to the ImageSequenceDecoder rather than
    // VideoDecoder. Audio also gets its own kind so the renderer
    // can skip video work and the timeline panel can style the
    // clip distinctly later if needed.
    videoClip.mediaKind   =
        item.type == MediaType::ImageSequence ? ClipMediaKind::ImageSequence :
        item.type == MediaType::Audio         ? ClipMediaKind::Audio
                                              : ClipMediaKind::Video;
    videoClip.mediaPath   = item.path;
    videoClip.mediaItemId = item.id;
    videoClip.sourceFps      = m_timeline.frameRate;
    videoClip.sourceDuration = m_timeline.duration;
    videoClip.hasAudio       = hasAudio;
    if (item.type == MediaType::ImageSequence) {
        videoClip.sequencePattern    = item.imageSeq.pattern;
        videoClip.sequenceStartFrame = item.imageSeq.startFrame;
        videoClip.sequenceEndFrame   = item.imageSeq.endFrame;
    }
    videoTrack.clips.append(videoClip);
    m_timeline.tracks.append(videoTrack);

    // Add a separate audio track only when the source actually
    // has audio. The audio mixer (Phase 5.x in the timeline-aware
    // shape) reads audibleClipsAt and would double-count if the
    // video clip's audio AND the audio track both fired; they
    // share the same source path. For SingleMedia we model the
    // audio as part of the video clip (hasAudio flag) and don't
    // create a separate audio track. Multi-source / playlist
    // mode introduces real audio tracks.

    m_timer->setDuration(m_timeline.duration);
    m_timer->setFrameRate(m_timeline.frameRate);
    m_timer->seek(0.0);

    emit timelineChanged();
    emit currentMediaKindChanged();
}

void TimelineController::loadPlaylist(const QString          &playlistName,
                                       const QList<MediaItem> &items,
                                       const QList<InOutRange>&trims,
                                       double                  masterFps,
                                       int                     gapFrames,
                                       int                     canvasW,
                                       int                     canvasH,
                                       const QString          &playlistMediaItemId)
{
    m_timer->pause();
    m_timer->seek(0.0);

    m_timeline = Timeline{};
    m_timeline.name      = playlistName;
    m_timeline.mode      = SourceMode::Playlist;
    m_timeline.policy.ripple        = true;
    m_timeline.policy.allowOverlaps = false;
    m_timeline.frameRate = (masterFps > 0.0) ? masterFps : 24.0;
    m_timeline.canvasWidth  = (canvasW > 0) ? canvasW : 1920;
    m_timeline.canvasHeight = (canvasH > 0) ? canvasH : 1080;

    const double gapDur = (gapFrames > 0)
                         ? (static_cast<double>(gapFrames)
                            / m_timeline.frameRate)
                         : 0.0;

    Track videoTrack;
    videoTrack.id      = QString::fromLatin1(kTrackIdA);
    videoTrack.name    = QStringLiteral("A");
    videoTrack.isVideo = true;
    videoTrack.visible = true;
    videoTrack.locked  = false;     // playlist clips are editable
    videoTrack.zIndex  = 1;

    double cursor = 0.0;
    for (int i = 0; i < items.size(); ++i) {
        const MediaItem &mi = items[i];

        // Resolve the clip's source duration. Prefer ImageSequence's
        // computed value; fall back to the top-level MediaItem.duration
        // (set by VideoMetadata extraction); fall back to 0 (timeline
        // will reflow once metadata arrives — see binsChanged hook).
        double sourceDur = 0.0;
        if (mi.type == MediaType::ImageSequence) {
            sourceDur = mi.imageSeq.duration > 0.0
                      ? mi.imageSeq.duration : mi.duration;
        } else {
            sourceDur = mi.duration > 0.0 ? mi.duration
                      : mi.video.duration;
        }

        // Apply trim. -1 inPoint ⇒ no trim (use full source).
        const InOutRange r = (i < trims.size()) ? trims[i] : InOutRange{};
        const double srcIn  = (r.inPoint  >= 0.0) ? r.inPoint  : 0.0;
        const double srcOut = (r.outPoint >= 0.0) ? r.outPoint : sourceDur;
        const double clipDur = qMax(0.0, srcOut - srcIn);

        Clip c;
        c.id          = QUuid::createUuid().toString(QUuid::WithoutBraces);
        c.name        = mi.name;
        c.startTime   = cursor;
        c.duration    = clipDur;
        c.sourceIn    = srcIn;
        c.sourceOut   = srcOut;
        c.mediaPath   = mi.path;
        c.mediaItemId = mi.id;
        if (mi.type == MediaType::ImageSequence) {
            c.mediaKind            = ClipMediaKind::ImageSequence;
            c.sequencePattern      = mi.imageSeq.pattern;
            c.sequenceStartFrame   = mi.imageSeq.startFrame;
            c.sequenceEndFrame     = mi.imageSeq.endFrame;
            c.sourceFps            = mi.imageSeq.frameRate > 0.0
                                     ? mi.imageSeq.frameRate
                                     : m_timeline.frameRate;
        } else if (mi.type == MediaType::Audio) {
            c.mediaKind = ClipMediaKind::Audio;
            c.sourceFps = m_timeline.frameRate;
        } else {
            c.mediaKind = ClipMediaKind::Video;
            c.sourceFps = mi.video.frameRate > 0.0
                          ? mi.video.frameRate
                          : m_timeline.frameRate;
        }
        c.sourceDuration = sourceDur;
        c.hasAudio       = mi.hasAudio;
        videoTrack.clips.append(c);
        cursor += clipDur;

        // Inter-clip gap (skip after the last clip).
        if (gapDur > 0.0 && i + 1 < items.size()) {
            Clip g;
            g.id        = QUuid::createUuid().toString(QUuid::WithoutBraces);
            g.name      = QStringLiteral("Gap");
            g.startTime = cursor;
            g.duration  = gapDur;
            g.isGap     = true;
            videoTrack.clips.append(g);
            cursor += gapDur;
        }
    }

    m_timeline.duration = cursor;
    m_timeline.tracks.append(videoTrack);

    // Stash the source MediaItem id on the timeline name path so
    // QML can route inspector queries back to the playlist record.
    // (We don't have a Timeline::mediaItemId field; the active
    // path/id getters walk the first clip, which works because
    // the playlist's clips have their own per-clip ids — not the
    // playlist's. Keep the id retrievable via a Q_INVOKABLE.)
    m_playlistMediaItemId = playlistMediaItemId;

    m_timer->setDuration(m_timeline.duration);
    m_timer->setFrameRate(m_timeline.frameRate);
    m_timer->seek(0.0);

    emit timelineChanged();
    emit currentMediaKindChanged();
}

int TimelineController::currentMediaKindInt() const
{
    const Clip *c = TimelineFlattener::visibleClipAt(
        m_timeline, m_timer ? m_timer->position() : 0.0);
    return c ? static_cast<int>(c->mediaKind)
             : static_cast<int>(ClipMediaKind::Video);
}

void TimelineController::clear()
{
    m_timer->pause();
    m_timeline = Timeline{};
    m_playlistMediaItemId.clear();
    m_timer->setDuration(0.0);
    m_timer->seek(0.0);
    emit timelineChanged();
}

void TimelineController::setFrameRate(double fps)
{
    if (fps <= 0.0) return;
    if (m_timeline.frameRate <= 0.0) return;
    if (qFuzzyCompare(m_timeline.frameRate, fps)) return;

    // Total frame count is invariant under fps re-tuning. Compute
    // it from the OLD fps + duration, then re-derive the new
    // duration. Same trick for the position so the same frame stays
    // visible under the playhead — useful when a user discovers
    // their renders are 30 fps after importing as 24.
    const double frameCountFloat =
        m_timeline.duration * m_timeline.frameRate;
    const double oldFraction =
        m_timeline.duration > 0.0
        ? (m_timer ? m_timer->position() : 0.0) / m_timeline.duration
        : 0.0;

    m_timeline.frameRate = fps;
    m_timeline.duration  = frameCountFloat / fps;

    // Update the single clip's bookkeeping too — flattener +
    // future clips read duration / sourceFps from the clip, not
    // just the timeline.
    if (!m_timeline.tracks.isEmpty() &&
        !m_timeline.tracks.first().clips.isEmpty()) {
        Clip &c = m_timeline.tracks.first().clips.first();
        c.duration       = m_timeline.duration;
        c.sourceOut      = m_timeline.duration;
        c.sourceFps      = fps;
        c.sourceDuration = m_timeline.duration;
    }

    if (m_timer) {
        m_timer->setFrameRate(fps);
        m_timer->setDuration(m_timeline.duration);
        m_timer->seek(oldFraction * m_timeline.duration);
    }
    emit timelineChanged();
}

QString TimelineController::activeMediaPath() const
{
    if (m_timeline.tracks.isEmpty()) return {};
    const Track &t = m_timeline.tracks.first();
    if (t.clips.isEmpty()) return {};
    return t.clips.first().mediaPath;
}

QString TimelineController::activeMediaItemId() const
{
    if (m_timeline.tracks.isEmpty()) return {};
    const Track &t = m_timeline.tracks.first();
    if (t.clips.isEmpty()) return {};
    return t.clips.first().mediaItemId;
}

Clip TimelineController::currentVisibleClip() const
{
    const Clip *c = TimelineFlattener::visibleClipAt(
        m_timeline, m_timer->position());
    return c ? *c : Clip{};
}

void TimelineController::loadSecondarySource(const QString &mediaPath,
                                              const QString &displayName,
                                              double durationSeconds,
                                              double frameRate,
                                              bool   hasAudio)
{
    if (mediaPath.isEmpty()) return;

    // Drop any prior B track first so we don't accumulate. The
    // playhead and timer are owned by track A; loading B does not
    // touch them.
    for (int i = m_timeline.tracks.size() - 1; i >= 0; --i) {
        if (m_timeline.tracks[i].id == QString::fromLatin1(kTrackIdB)) {
            m_timeline.tracks.removeAt(i);
        }
    }

    const double durB = (durationSeconds > 0.0) ? durationSeconds : 0.0;
    const double fpsB = (frameRate > 0.0)
                       ? frameRate
                       : (m_timeline.frameRate > 0.0
                          ? m_timeline.frameRate
                          : 24.0);
    const QString name = displayName.isEmpty()
                        ? QFileInfo(mediaPath).fileName()
                        : displayName;

    Track b;
    b.id       = QString::fromLatin1(kTrackIdB);
    b.name     = QStringLiteral("B");
    b.isVideo  = true;
    b.visible  = true;
    b.locked   = true;     // V1: no editing surface yet
    b.zIndex   = 0;        // below A in the (future) flatten

    Clip c;
    c.id          = QUuid::createUuid().toString(QUuid::WithoutBraces);
    c.name        = name;
    c.startTime   = 0.0;
    c.duration    = durB;
    c.sourceIn    = 0.0;
    c.sourceOut   = durB;
    c.mediaKind   = ClipMediaKind::Video;   // B is video-only for now
    c.mediaPath   = mediaPath;
    // mediaItemId stays empty — B is not in ProjectManager's bin.
    c.sourceFps      = fpsB;
    c.sourceDuration = durB;
    c.hasAudio       = hasAudio;
    b.clips.append(c);
    m_timeline.tracks.append(b);

    // Master timeline duration tracks the longer side. Push the
    // updated duration into the timer too — without this, when A
    // is shorter than B (e.g. A=EXR 63 frames, B=video 115), the
    // timer's internal duration stays clamped at A's value
    // (set during loadSingleMedia) and the playhead stops at A's
    // end instead of traversing the gap into B's range.
    m_timeline.duration = std::max(m_timeline.duration, durB);
    if (m_timer) {
        m_timer->setDuration(m_timeline.duration);
    }

    emit timelineChanged();
}

void TimelineController::clearSecondarySource()
{
    bool removed = false;
    for (int i = m_timeline.tracks.size() - 1; i >= 0; --i) {
        if (m_timeline.tracks[i].id == QString::fromLatin1(kTrackIdB)) {
            m_timeline.tracks.removeAt(i);
            removed = true;
        }
    }
    if (!removed) return;

    // Recompute cached duration without B. Walks track A's clips
    // because (V1) A is single-clip but the math is general — when
    // playlist mode lands, A may have many clips and we want the
    // tail end of the last one.
    double durA = 0.0;
    for (const Track &t : m_timeline.tracks) {
        for (const Clip &c : t.clips) {
            durA = std::max(durA, c.startTime + c.duration);
        }
    }
    m_timeline.duration = durA;

    emit timelineChanged();
}

QVariantList TimelineController::trackList() const
{
    QVariantList out;
    out.reserve(m_timeline.tracks.size());
    for (const Track &t : m_timeline.tracks) {
        if (!t.isVideo) continue;     // audio lanes hidden until UI grows one
        out.append(trackToMap(t));
    }
    return out;
}

// ---- Phase 7.8 Stage C — edit op API --------------------------

void TimelineController::applyClipReplacement(const QString &trackId,
                                                 const QString &clipId,
                                                 const Clip &newState)
{
    for (Track &track : m_timeline.tracks) {
        if (track.id != trackId) continue;
        for (Clip &c : track.clips) {
            if (c.id != clipId) continue;
            c = newState;
            // Phase 3.H.4 Stage B (lean-in) — every edit in
            // playlist mode auto-ripples so the timeline stays
            // contiguous. Single / dual modes keep the old
            // free-shift semantic (slide in dual mustn't touch
            // sibling positions).
            if (m_timeline.mode == SourceMode::Playlist) {
                recomputeContiguousStartTimes(trackId);
                return;
            }
            recalculateDuration();
            emit timelineChanged();
            return;
        }
    }
}

void TimelineController::recalculateDuration()
{
    double maxEnd = 0.0;
    for (const Track &t : m_timeline.tracks) {
        for (const Clip &c : t.clips) {
            maxEnd = std::max(maxEnd, c.startTime + c.duration);
        }
    }
    if (m_timeline.duration != maxEnd) {
        m_timeline.duration = maxEnd;
        if (m_timer) m_timer->setDuration(maxEnd);
    }
}

void TimelineController::trimClipHead(const QString &trackId,
                                        const QString &clipId,
                                        double newStartSec,
                                        double newSourceInSec)
{
    m_undoStack.push(new TrimClipHeadEdit(this, trackId, clipId,
                                            newStartSec, newSourceInSec));
}

void TimelineController::trimClipTail(const QString &trackId,
                                        const QString &clipId,
                                        double newSourceOutSec)
{
    m_undoStack.push(new TrimClipTailEdit(this, trackId, clipId,
                                            newSourceOutSec));
}

void TimelineController::slideClip(const QString &trackId,
                                     const QString &clipId,
                                     double newStartSec)
{
    m_undoStack.push(new SlideClipEdit(this, trackId, clipId, newStartSec));
}

void TimelineController::slipClip(const QString &trackId,
                                    const QString &clipId,
                                    double newSourceInSec,
                                    double newSourceOutSec)
{
    m_undoStack.push(new SlipClipEdit(this, trackId, clipId,
                                          newSourceInSec, newSourceOutSec));
}

void TimelineController::splitClipAt(const QString &trackId, double atSec)
{
    // Find the clip that covers atSec on the named track.
    QString clipId;
    for (const Track &track : m_timeline.tracks) {
        if (track.id != trackId) continue;
        for (const Clip &c : track.clips) {
            if (c.isGap) continue;
            if (atSec > c.startTime && atSec < c.startTime + c.duration) {
                clipId = c.id;
                break;
            }
        }
        break;
    }
    if (clipId.isEmpty()) return;
    m_undoStack.push(new SplitClipEdit(this, trackId, clipId, atSec));
}

void TimelineController::deleteClip(const QString &trackId,
                                       const QString &clipId)
{
    m_undoStack.push(new DeleteClipEdit(this, trackId, clipId));
}

void TimelineController::moveClipToIndex(const QString &trackId,
                                            const QString &clipId,
                                            int            newIndex)
{
    int fromIndex = -1;
    int trackSize = 0;
    for (const Track &track : m_timeline.tracks) {
        if (track.id != trackId) continue;
        trackSize = track.clips.size();
        for (int i = 0; i < track.clips.size(); ++i) {
            if (track.clips[i].id == clipId) { fromIndex = i; break; }
        }
        break;
    }
    if (fromIndex < 0) return;
    const int to = std::clamp(newIndex, 0, trackSize);
    if (to == fromIndex || to == fromIndex + 1) return;
    m_undoStack.push(new MoveClipEdit(this, trackId, fromIndex, to));
}

QString TimelineController::insertClipAtIndex(const QString    &trackId,
                                               int               atIndex,
                                               const QVariantMap &clipSpec)
{
    if (clipSpec.isEmpty()) return {};

    Clip c;
    c.id          = QUuid::createUuid().toString(QUuid::WithoutBraces);
    c.name        = clipSpec.value(QStringLiteral("name")).toString();
    c.mediaPath   = clipSpec.value(QStringLiteral("mediaPath")).toString();
    c.mediaItemId = clipSpec.value(QStringLiteral("mediaItemId")).toString();
    c.mediaKind   = static_cast<ClipMediaKind>(
        clipSpec.value(QStringLiteral("mediaKind"),
                       static_cast<int>(ClipMediaKind::Video)).toInt());
    c.sourceFps      = clipSpec.value(QStringLiteral("sourceFps"),
                                      m_timeline.frameRate).toDouble();
    c.sourceDuration = clipSpec.value(QStringLiteral("sourceDuration"),
                                      0.0).toDouble();
    c.sourceIn       = clipSpec.value(QStringLiteral("sourceIn"),
                                      0.0).toDouble();
    c.sourceOut      = clipSpec.value(QStringLiteral("sourceOut"),
                                      c.sourceDuration).toDouble();
    c.duration       = qMax(0.0, c.sourceOut - c.sourceIn);
    c.startTime      = 0.0;     // recomputeContiguousStartTimes will fix
    c.hasAudio       = clipSpec.value(QStringLiteral("hasAudio"),
                                      false).toBool();
    c.sequencePattern    = clipSpec.value(QStringLiteral("sequencePattern"))
                                   .toString();
    c.sequenceStartFrame = clipSpec.value(
        QStringLiteral("sequenceStartFrame"), 1).toInt();
    c.sequenceEndFrame   = clipSpec.value(
        QStringLiteral("sequenceEndFrame"), 1).toInt();

    if (c.mediaPath.isEmpty() || c.duration <= 0.0) {
        qWarning("TimelineController::insertClipAtIndex — invalid spec "
                 "(path='%s' dur=%.3f)",
                 qPrintable(c.mediaPath), c.duration);
        return {};
    }
    // Reject audio in playlist tracks for v2.0. Belt + suspenders
    // against the QML drop guard at TimelinePanel.qml — covers any
    // programmatic / project-load callers that build a spec with
    // ClipMediaKind::Audio. Standalone audio (single-media load)
    // is unaffected; that path doesn't go through insertClipAtIndex.
    if (c.mediaKind == ClipMediaKind::Audio) {
        qWarning("TimelineController::insertClipAtIndex — audio media "
                 "not allowed in playlists (path='%s')",
                 qPrintable(c.mediaPath));
        return {};
    }

    // Clamp index to a valid insert position [0, clips.size()].
    int idx = atIndex;
    for (const Track &track : m_timeline.tracks) {
        if (track.id != trackId) {
            continue;
        }
        idx = std::clamp(atIndex, 0,
                         static_cast<int>(track.clips.size()));
        break;
    }

    m_undoStack.push(new InsertClipEdit(this, trackId, idx, c));
    return c.id;
}

int TimelineController::snapEdgeIndexAtTime(const QString &trackId,
                                              double timeSec,
                                              double tolSec) const
{
    if (tolSec <= 0.0) tolSec = 0.0001;
    for (const Track &track : m_timeline.tracks) {
        if (track.id != trackId) continue;
        if (track.clips.isEmpty()) {
            // Empty track: only edge is t=0.
            return (qFabs(timeSec) <= tolSec) ? 0 : -1;
        }
        // Walk every clip's start + end. Track the closest edge
        // whose distance falls within tolerance.
        int    bestIdx  = -1;
        double bestDist = tolSec;
        // t=0 (track start)
        double d = qFabs(timeSec - 0.0);
        if (d <= bestDist) { bestDist = d; bestIdx = 0; }
        for (int i = 0; i < track.clips.size(); ++i) {
            const Clip &c = track.clips[i];
            const double s = c.startTime;
            const double e = c.startTime + c.duration;
            // start of clip i → insert AT i
            d = qFabs(timeSec - s);
            if (d <= bestDist) { bestDist = d; bestIdx = i; }
            // end of clip i → insert AT i+1
            d = qFabs(timeSec - e);
            if (d <= bestDist) { bestDist = d; bestIdx = i + 1; }
        }
        return bestIdx;
    }
    return -1;
}

void TimelineController::recomputeContiguousStartTimes(const QString &trackId)
{
    for (Track &track : m_timeline.tracks) {
        if (track.id != trackId) continue;
        double cursor = 0.0;
        for (Clip &c : track.clips) {
            c.startTime = cursor;
            cursor += c.duration;
        }
        // Update timeline.duration from the track's contiguous total.
        recalculateDuration();
        if (m_timer) m_timer->setDuration(m_timeline.duration);
        emit timelineChanged();
        return;
    }
}

bool TimelineController::patchClipDurationFromSource(
    const QString &trackId,
    const QString &clipId,
    double         newSourceDuration,
    double         newSourceFps)
{
    if (newSourceDuration <= 0.0) return false;
    for (Track &track : m_timeline.tracks) {
        if (track.id != trackId) continue;
        for (Clip &c : track.clips) {
            if (c.id != clipId) continue;
            // Only patch when the clip currently uses the full
            // source range — otherwise the user has trimmed and
            // the placeholder might already match their intent.
            const bool fullRange =
                c.sourceIn <= 0.0001
                && (c.sourceOut <= 0.0
                    || qFabs(c.sourceOut - c.sourceDuration) < 0.0001);
            if (!fullRange) return false;
            // Skip no-op patches (within 1 ms) so we don't churn
            // the QML model.
            if (qFabs(c.sourceDuration - newSourceDuration) < 0.001
                && (newSourceFps <= 0.0
                    || qFabs(c.sourceFps - newSourceFps) < 0.001)) {
                return false;
            }
            c.sourceDuration = newSourceDuration;
            c.sourceOut      = newSourceDuration;
            c.duration       = newSourceDuration;
            if (newSourceFps > 0.0) c.sourceFps = newSourceFps;
            recomputeContiguousStartTimes(trackId);
            return true;
        }
    }
    return false;
}

void TimelineController::applyClipInsertAt(const QString &trackId,
                                              int index,
                                              const Clip &clip)
{
    for (Track &track : m_timeline.tracks) {
        if (track.id != trackId) continue;
        const int safeIdx = std::clamp(index, 0,
                                          static_cast<int>(track.clips.size()));
        track.clips.insert(safeIdx, clip);
        // Phase 3.H.4 — playlist mode: ripple after insert.
        if (m_timeline.mode == SourceMode::Playlist) {
            recomputeContiguousStartTimes(trackId);
            return;
        }
        recalculateDuration();
        emit timelineChanged();
        return;
    }
}

void TimelineController::applyClipInsertAfter(const QString &trackId,
                                                  const QString &afterClipId,
                                                  const Clip &clip)
{
    for (Track &track : m_timeline.tracks) {
        if (track.id != trackId) continue;
        for (int i = 0; i < track.clips.size(); ++i) {
            if (track.clips[i].id != afterClipId) continue;
            track.clips.insert(i + 1, clip);
            if (m_timeline.mode == SourceMode::Playlist) {
                recomputeContiguousStartTimes(trackId);
                return;
            }
            recalculateDuration();
            emit timelineChanged();
            return;
        }
    }
}

void TimelineController::applyClipRemove(const QString &trackId,
                                            const QString &clipId)
{
    for (Track &track : m_timeline.tracks) {
        if (track.id != trackId) continue;
        for (int i = 0; i < track.clips.size(); ++i) {
            if (track.clips[i].id != clipId) continue;
            track.clips.removeAt(i);
            if (m_timeline.mode == SourceMode::Playlist) {
                recomputeContiguousStartTimes(trackId);
                return;
            }
            recalculateDuration();
            emit timelineChanged();
            return;
        }
    }
}

namespace {
// Reverse of clipToMap above. Reconstructs a Clip from a
// QVariantMap that came out of trackA().clips / saved DualPair.
Clip clipFromMap(const QVariantMap &m)
{
    Clip c;
    c.id          = m.value(QStringLiteral("id")).toString();
    c.name        = m.value(QStringLiteral("name")).toString();
    c.startTime   = m.value(QStringLiteral("startTime")).toDouble();
    c.duration    = m.value(QStringLiteral("duration")).toDouble();
    c.sourceIn    = m.value(QStringLiteral("sourceIn")).toDouble();
    c.sourceOut   = m.value(QStringLiteral("sourceOut")).toDouble();
    c.isGap       = m.value(QStringLiteral("isGap")).toBool();
    c.mediaKind   = static_cast<ClipMediaKind>(
                       m.value(QStringLiteral("mediaKind")).toInt());
    c.mediaPath   = m.value(QStringLiteral("mediaPath")).toString();
    c.mediaItemId = m.value(QStringLiteral("mediaItemId")).toString();
    c.sourceFps   = m.value(QStringLiteral("sourceFps")).toDouble();
    c.sourceDuration = m.value(QStringLiteral("sourceDuration")).toDouble();
    c.hasAudio    = m.value(QStringLiteral("hasAudio")).toBool();
    return c;
}
} // namespace

void TimelineController::replaceTrackClips(const QString &trackId,
                                              const QVariantList &clipMaps)
{
    for (Track &track : m_timeline.tracks) {
        if (track.id != trackId) continue;
        track.clips.clear();
        for (const QVariant &v : clipMaps) {
            track.clips.append(clipFromMap(v.toMap()));
        }
        recalculateDuration();
        emit timelineChanged();
        return;
    }
}

namespace {
// Helper — find the named clip on the named track. Returns nullptr
// if not found. Called from preview hooks below to read current
// state before mutating.
Clip *findMutableClip(Timeline &t, const QString &trackId,
                        const QString &clipId)
{
    for (Track &track : t.tracks) {
        if (track.id != trackId) continue;
        for (Clip &c : track.clips) {
            if (c.id == clipId) return &c;
        }
    }
    return nullptr;
}
} // namespace

void TimelineController::previewTrimHead(const QString &trackId,
                                            const QString &clipId,
                                            double newStartSec,
                                            double newSourceInSec)
{
    Clip *c = findMutableClip(m_timeline, trackId, clipId);
    if (!c) return;
    const double oldEnd = c->startTime + c->duration;
    c->startTime = newStartSec;
    c->sourceIn  = newSourceInSec;
    c->duration  = std::max(0.0, oldEnd - newStartSec);
    recalculateDuration();
    emit timelineChanged();
}

void TimelineController::previewTrimTail(const QString &trackId,
                                            const QString &clipId,
                                            double newSourceOutSec)
{
    Clip *c = findMutableClip(m_timeline, trackId, clipId);
    if (!c) return;
    c->sourceOut = newSourceOutSec;
    c->duration  = std::max(0.0, newSourceOutSec - c->sourceIn);
    recalculateDuration();
    emit timelineChanged();
}

void TimelineController::previewSlide(const QString &trackId,
                                        const QString &clipId,
                                        double newStartSec)
{
    Clip *c = findMutableClip(m_timeline, trackId, clipId);
    if (!c) return;
    c->startTime = std::max(0.0, newStartSec);
    recalculateDuration();
    emit timelineChanged();
}

void TimelineController::previewSlip(const QString &trackId,
                                       const QString &clipId,
                                       double newSourceInSec,
                                       double newSourceOutSec)
{
    Clip *c = findMutableClip(m_timeline, trackId, clipId);
    if (!c) return;
    c->sourceIn  = std::max(0.0, newSourceInSec);
    c->sourceOut = std::max(c->sourceIn, newSourceOutSec);
    // duration is unchanged on slip — don't recalculate.
    emit timelineChanged();
}

void TimelineController::restoreClipFromMap(const QString &trackId,
                                               const QString &clipId,
                                               const QVariantMap &snapshot)
{
    Clip *c = findMutableClip(m_timeline, trackId, clipId);
    if (!c) return;
    if (snapshot.contains(QStringLiteral("startTime")))
        c->startTime = snapshot.value(QStringLiteral("startTime")).toDouble();
    if (snapshot.contains(QStringLiteral("duration")))
        c->duration = snapshot.value(QStringLiteral("duration")).toDouble();
    if (snapshot.contains(QStringLiteral("sourceIn")))
        c->sourceIn = snapshot.value(QStringLiteral("sourceIn")).toDouble();
    if (snapshot.contains(QStringLiteral("sourceOut")))
        c->sourceOut = snapshot.value(QStringLiteral("sourceOut")).toDouble();
    recalculateDuration();
    emit timelineChanged();
}

QVariantMap TimelineController::trackA() const
{
    for (const Track &t : m_timeline.tracks) {
        if (t.id == QString::fromLatin1(kTrackIdA)) return trackToMap(t);
    }
    return {};
}

QVariantMap TimelineController::trackB() const
{
    for (const Track &t : m_timeline.tracks) {
        if (t.id == QString::fromLatin1(kTrackIdB)) return trackToMap(t);
    }
    return {};
}

bool TimelineController::hasTrackB() const
{
    for (const Track &t : m_timeline.tracks) {
        if (t.id == QString::fromLatin1(kTrackIdB)) return true;
    }
    return false;
}

} // namespace qcv
