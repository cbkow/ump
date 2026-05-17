// Timeline data types — Phase 7.2.a.
//
// OTIO-shaped local C++ structs. The OpenTimelineIO library is NOT
// linked (see project_otio_library_dropped.md memory note); we model
// the same conceptual shape — Tracks of Items where each Item is
// either a Clip or a Gap — directly in plain C++. Save/load to
// .qcvproj is plain JSON in 7.8 with an OTIO-compatible schema, so a
// future writer to .otio is a small mapping function.
//
// This file is data-only. No QObject, no Q_PROPERTY, no behavior.
// The TimelineController (7.2.c) wraps these structs and exposes the
// QObject signal/slot surface to QML and the renderer.
//
// Universality: every playback case is internally a timeline.
//   SingleMedia  → 1 Track (locked) with 1 Clip spanning duration.
//   Playlist     → 1 Track with N Clips and inter-Clip Gaps.
//   (Dual-source  → 2 Tracks at the compositor level — viewport
//                   state per Guide 16, NOT a Timeline SourceMode.)

#pragma once

#include <QList>
#include <QString>

namespace qcv {

// Source mode of the active timeline. Drives editing rules and
// transport-control visibility. DualView is intentionally absent
// per Guide 16 — A/B comparison is viewport state on the
// compositor's Source A / Source B slots, not a timeline mode.
enum class SourceMode : int {
    SingleMedia = 0,    // 1 video track + optional audio. Locked.
    Playlist    = 1,    // 1 video track, N clips, ripple-edited.
};

// What kind of media a Clip points at. Drives the decoder choice
// when the controller resolves a clip → frame source.
enum class ClipMediaKind : int {
    Video         = 0,
    ImageSequence = 1,    // wired in Phase 7.4
    Audio         = 2,
    // (no DualView / Playlist here — those are Timeline shapes,
    // not clip kinds; a Clip always references a single media.)
};

// Single item on a track. A clip with `isGap == true` is a "gap"
// (empty space) — same struct, different intent. Mirrors the old
// app's OTIOClip (which also used an is_gap flag) and keeps the
// JSON shape simple. If the variant approach pays off later we
// can split into Clip / Gap subtypes; for now a flag is plenty.
struct Clip {
    QString  id;
    QString  name;

    // Timeline coordinates, all in seconds.
    double   startTime = 0.0;     // position on the track (global timeline time)
    double   duration  = 0.0;     // length on the track

    // Source-media trim. Where in the source file this clip
    // pulls from. For gaps these are unused.
    double   sourceIn  = 0.0;
    double   sourceOut = 0.0;

    bool     isGap = false;

    // Media reference — what the clip points at. Empty for gaps.
    ClipMediaKind mediaKind = ClipMediaKind::Video;
    QString       mediaPath;        // absolute path on disk
    QString       mediaItemId;      // ProjectManager MediaItem id

    // Source metadata snapshot, populated when the clip is linked
    // to a real file. Not authoritative — the live VideoDecoder is
    // the source of truth during playback. These cached values let
    // the timeline render correctly before the decoder spins up.
    double   sourceFps      = 0.0;
    int      sourceWidth    = 0;
    int      sourceHeight   = 0;
    double   sourceDuration = 0.0;
    bool     hasAudio       = false;

    // Image-sequence fields (only meaningful when mediaKind is
    // ImageSequence; wired in 7.4). Pattern + frame range so the
    // sequence decoder can resolve a clip-time → frame-file.
    QString  sequencePattern;
    int      sequenceStartFrame = 1;
    int      sequenceEndFrame   = 1;
    QString  sequenceExrLayer;     // EXR layer if multi-layer

    // Per-clip audio mute. The track has its own mute too; either
    // path silences. Useful for "play this video clip but solo
    // the audio track underneath."
    bool     audioMuted = false;
};

// One track holds an ordered list of Clips (some of which may be
// gaps). Tracks have a kind (video / audio) and visibility / mute
// state. `locked == true` means no editing — used in SingleMedia
// where the track is just a wrapper around the source file.
struct Track {
    QString      id;
    QString      name;
    bool         isVideo  = true;
    bool         visible  = true;     // eye toggle (video tracks)
    bool         muted    = false;    // mute toggle (audio tracks)
    bool         audioMuted = false;  // for video tracks: silence embedded audio
    bool         locked   = false;    // no editing allowed
    int          zIndex   = 0;        // higher = on top in flatten
    QList<Clip>  clips;
};

// Edit-policy parameterizes mode-specific behavior on top of the
// universal Track / Clip shape. Phase 7.2.a defines the shape;
// 7.5 (Playlist) actually uses the ripple fields. SingleMedia
// uses the default (locked, no ripple).
struct EditPolicy {
    bool   ripple        = false;     // Playlist: drag/trim ripples downstream
    double interClipGap  = 0.0;       // seconds of fixed gap between clips
    bool   allowOverlaps = false;     // tracks reject overlapping clips
};

// The whole timeline. The TimelineController (7.2.c) owns one of
// these and exposes operations. SourceMode + EditPolicy together
// describe the editing rules; the universal data shape stays the
// same regardless.
struct Timeline {
    QString         name;
    SourceMode      mode = SourceMode::SingleMedia;
    EditPolicy      policy;

    double          frameRate = 24.0;
    double          duration  = 0.0;     // computed from tracks; cache here

    // Output canvas for the compositor. For SingleMedia these come
    // from the source file; for Playlist they're chosen at create
    // time and clips conform.
    int             canvasWidth  = 1920;
    int             canvasHeight = 1080;

    QList<Track>    tracks;
};

} // namespace qcv
