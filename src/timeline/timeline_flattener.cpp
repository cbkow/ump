#include "timeline_flattener.h"

#include <limits>

namespace qcv {

namespace {

// Half-open range test [startTime, startTime + duration). Closing
// the right side avoids the ambiguity at clip boundaries — at the
// exact end of clip A which starts clip B, the next clip wins.
inline bool clipContains(const Clip &clip, double position)
{
    return position >= clip.startTime
        && position <  clip.startTime + clip.duration;
}

} // namespace

const Clip *TimelineFlattener::clipInTrackAt(const Track &track, double position)
{
    // Linear scan. Old app does the same — playlists in review
    // workflows are small (rarely more than a few dozen clips),
    // and binary search with std::upper_bound buys nothing
    // measurable while costing readability and complicating gap
    // skipping.
    for (const Clip &c : track.clips) {
        if (c.isGap) continue;
        if (clipContains(c, position)) return &c;
    }
    return nullptr;
}

const Clip *TimelineFlattener::visibleClipAt(const Timeline &timeline,
                                             double position)
{
    // Walk video tracks in descending zIndex (topmost first).
    // For SingleMedia + Playlist we have one track; the loop is
    // trivial. For future multi-track configurations the highest
    // zIndex wins — same rule the old app's TimelineFlattener
    // applies in dual-view mode.
    int bestZ = std::numeric_limits<int>::min();
    const Clip *best = nullptr;
    for (const Track &track : timeline.tracks) {
        if (!track.isVideo || !track.visible) continue;
        if (track.zIndex < bestZ) continue;
        if (const Clip *c = clipInTrackAt(track, position)) {
            if (track.zIndex > bestZ || best == nullptr) {
                best = c;
                bestZ = track.zIndex;
            }
        }
    }
    return best;
}

const Clip *TimelineFlattener::visibleClipOnTrack(const Timeline &timeline,
                                                    const QString &trackId,
                                                    double position)
{
    for (const Track &track : timeline.tracks) {
        if (track.id != trackId) continue;
        if (!track.isVideo || !track.visible) continue;
        return clipInTrackAt(track, position);
    }
    return nullptr;
}

QList<const Clip *>
TimelineFlattener::audibleClipsAt(const Timeline &timeline, double position)
{
    QList<const Clip *> out;

    // Video tracks: include the topmost visible clip's audio if
    // the track + clip aren't audio-muted. Lower-z video tracks
    // are video-occluded but their audio still contributes
    // (matches old app behavior — useful for layered comping
    // where you want the under-track's dialogue audible).
    for (const Track &track : timeline.tracks) {
        if (!track.isVideo) continue;
        if (track.audioMuted) continue;
        if (const Clip *c = clipInTrackAt(track, position)) {
            if (!c->audioMuted && c->hasAudio) out.append(c);
        }
    }

    // Audio tracks: include every non-muted audio track's clip
    // at this position.
    for (const Track &track : timeline.tracks) {
        if (track.isVideo) continue;
        if (track.muted) continue;
        if (const Clip *c = clipInTrackAt(track, position)) {
            if (!c->audioMuted) out.append(c);
        }
    }
    return out;
}

double TimelineFlattener::timelineToSource(const Clip &clip, double position)
{
    if (!clipContains(clip, position)) return -1.0;
    const double offset = position - clip.startTime;
    return clip.sourceIn + offset;
}

double TimelineFlattener::nextClipStart(const Timeline &timeline,
                                        double position)
{
    double best = std::numeric_limits<double>::max();
    bool found = false;
    for (const Track &track : timeline.tracks) {
        for (const Clip &c : track.clips) {
            if (c.isGap) continue;
            if (c.startTime > position && c.startTime < best) {
                best = c.startTime;
                found = true;
            }
        }
    }
    return found ? best : -1.0;
}

double TimelineFlattener::prevClipStart(const Timeline &timeline,
                                        double position)
{
    double best = -std::numeric_limits<double>::max();
    bool found = false;
    for (const Track &track : timeline.tracks) {
        for (const Clip &c : track.clips) {
            if (c.isGap) continue;
            if (c.startTime < position && c.startTime > best) {
                best = c.startTime;
                found = true;
            }
        }
    }
    return found ? best : -1.0;
}

} // namespace qcv
