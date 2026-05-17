// TimelineFlattener — Phase 7.2.c.
//
// Pure functions over a Timeline. Answer "what clip is visible at
// timeline-time T?" and adjacent queries. No state — the caller
// passes in a Timeline (or a const reference to the controller's
// timeline) and gets back const pointers that are valid as long
// as the timeline doesn't mutate.
//
// Stateless-by-design because the controller is the QObject and
// the source of truth for thread safety; this header lives on top
// without owning any data. That keeps the universal flatten logic
// reusable across the controller + the renderer + the audio mixer
// + future export tooling.
//
// Universal across SourceModes:
//   SingleMedia (1 track / 1 clip) → degenerate; visible clip is
//     just the only clip if startTime==0 and the position falls
//     inside its duration.
//   Playlist (1 track / N clips) → linear search for the clip
//     containing T (a small N — handful of clips at most for
//     review work; binary search is overkill).
//   (Multi-track future)         → walks tracks in zIndex order
//     and returns the topmost visible clip with audio handling
//     analogous to the old app's GetAllAudibleClipsAtTime.

#pragma once

#include "timeline_types.h"

#include <QList>

namespace qcv {

class TimelineFlattener
{
public:
    // Returns the clip on the topmost visible video track whose
    // [startTime, startTime + duration) contains `position`.
    // nullptr if no clip covers that position (gap or empty
    // timeline). The pointer is valid until the timeline mutates;
    // callers that need the data across mutations must copy.
    static const Clip *visibleClipAt(const Timeline &timeline,
                                     double position);

    // Phase 7.8 — per-track variant. Returns the clip on the named
    // track (`trackId == "A"` / `"B"`) whose range contains
    // `position`. Used by the dual playback island to translate
    // master playhead → per-side source frame independently for A
    // and B (their clip layouts can differ once edits are applied).
    static const Clip *visibleClipOnTrack(const Timeline &timeline,
                                            const QString &trackId,
                                            double position);

    // Returns every audio-emitting clip at `position`. Includes
    // the visible video clip (if its track's audioMuted is false
    // AND clip's audioMuted is false) plus all clips on
    // non-muted audio tracks. Used by the audio mixer (Phase 5.x)
    // for multi-track sums in playlist mode and for picking the
    // right audio track for image-sequence mode.
    static QList<const Clip *> audibleClipsAt(const Timeline &timeline,
                                              double position);

    // Convert position-in-timeline → position-in-source-media for
    // a given clip. Returns -1.0 if `position` is outside the
    // clip's range. The audio decoder + scrub decoder seek using
    // this.
    static double timelineToSource(const Clip &clip, double position);

    // Find the next / prev clip boundary after / before
    // `position`. Used by Prev Clip / Next Clip transport
    // buttons (playlist mode) and by loop-region wrap. Returns
    // -1.0 if no boundary in that direction.
    static double nextClipStart(const Timeline &timeline, double position);
    static double prevClipStart(const Timeline &timeline, double position);

private:
    // Helpers — clip selection within a single track. Skips gaps.
    static const Clip *clipInTrackAt(const Track &track, double position);
};

} // namespace qcv
