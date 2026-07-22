// MediaItem — Phase 7.1.
//
// Slim port of the old app's `MediaItem` struct. The old version was
// ~150 fields with legacy duplicates and timeline metadata; this is
// the load-bearing shape for the new app, with hooks reserved for
// later sub-phases (image sequences in 7.4, playlists in 7.5).
//
// Per Guide 16 the `DualView` type is gone — A/B comparison is
// viewport state, not a media-item type. The `Sequence` (EDL) type
// is also gone unless OTIO interop lands.
//
// Phase 7.8 (Guide 21) reintroduces a media-item type for A/B
// comparison, but with DIFFERENT INTENT: `DualPair` is a saved
// session (pathA, pathB, post-edit clip layouts), not a media
// wrapper. Loading it enters dual mode + applies the recorded
// edits. The deprecated `DualView` enum is NOT resurrected.

#pragma once

#include <QList>
#include <QString>
#include <QVariantList>

namespace qcv {

enum class MediaType {
    Video         = 0,
    Audio         = 1,
    Image         = 2,
    ImageSequence = 3,   // wired in Phase 7.4
    Playlist      = 4,   // wired in Phase 7.5
    DualPair      = 5,   // Phase 7.8 — saved A/B comparison
};

// Per-clip YUV range override (Phase 3.G, Guide 07 D10). Default Auto
// uses FFmpeg-detected color_range; Full / Limited force the
// interpretation regardless of stream metadata. Consumed by the YUV→
// RGB conversion in both the CPU swscale path (`VideoDecoder`) and
// the GPU kernel (`MetalYuvRenderer` via `metal_player_renderer`).
// Only meaningful for `MediaType::Video` — image sequences are full-
// range by nature.
enum class VideoRange {
    Auto    = 0,
    Full    = 1,
    Limited = 2,
};

// Per-clip audio channel routing for broadcast deliverables. Picks
// which input channels feed the stereo output of the player. Auto's
// behavior keys off the source's channel count (1ch → mono-to-stereo;
// 2ch → direct; 6ch → 5.1 BS.775 downmix; 8ch → channels 7-8 stereo
// bounce; otherwise channels 1-2 stereo). Inspector exposes the
// non-Auto modes only when the source has enough channels for them.
enum class AudioRoutingMode {
    Auto       = 0,
    Downmix5_1 = 1,   // BS.775 downmix from channels 1-6
    Stereo7_8  = 2,   // Channels 7-8 (zero-indexed 6-7) as L/R
};

// Per-clip pixel-aspect (anamorphic) handling. The viewport otherwise
// assumes square pixels and maps raw decoded width×height to the
// drawable, so SAR ≠ 1:1 content renders squeezed. Default Square
// shows the stored pixels untouched (QC-safe); Detected applies the
// SAR FFmpeg probed (VideoMetadata::sarNum/sarDen); Custom applies a
// user-entered pixel aspect (customParNum/customParDen). Mutated via
// `ProjectManager::setPixelAspect`; the WindowManager pushes the
// resulting effective ratio to the renderer live (no re-decode) and
// re-applies it on every clip load. Mirrors the videoRangeOverride
// rails.
enum class PixelAspectMode {
    Square   = 0,   // force 1:1 (default, QC-safe)
    Detected = 1,   // apply the file's probed SAR
    Custom   = 2,   // apply customParNum/customParDen
};

inline VideoRange videoRangeFromString(const QString &s) {
    if (s == QStringLiteral("full"))    return VideoRange::Full;
    if (s == QStringLiteral("limited")) return VideoRange::Limited;
    return VideoRange::Auto;
}

inline QString videoRangeToString(VideoRange r) {
    switch (r) {
    case VideoRange::Full:    return QStringLiteral("full");
    case VideoRange::Limited: return QStringLiteral("limited");
    case VideoRange::Auto:    break;
    }
    return QStringLiteral("auto");
}

// Adobe project + extended-timecode metadata extracted via exiftool
// (Phase 3.D). The bundled exiftool reads XMP / QuickTime / MXF
// fields that libavformat doesn't surface — including After Effects
// project links, Premiere atom paths (Mac + Windows variants), and
// XMP/MXF/QT timecode triples that help QC reviewers cross-reference
// with the source NLE timeline.
struct AdobeMetadata {
    bool        loaded = false;

    // Adobe project links — pulled from XMP at file-write time.
    QString     aeProjectPath;
    QString     premiereWinPath;
    QString     premiereMacPath;

    // Timecode triples. Each may be empty.
    QString     qtStartTimecode;
    QString     qtTimecode;
    QString     qtCreationDate;
    QString     qtMediaCreateDate;
    QString     mxfStartTimecode;
    QString     mxfTimecodeAtStart;
    QString     xmpStartTimecode;
    QString     xmpAltTimecode;
    QString     userdataTimecode;

    bool hasAnyAdobeProject() const {
        return !aeProjectPath.isEmpty() ||
               !premiereWinPath.isEmpty() ||
               !premiereMacPath.isEmpty();
    }

    bool hasAnyTimecode() const {
        return !qtStartTimecode.isEmpty() || !qtTimecode.isEmpty() ||
               !mxfStartTimecode.isEmpty() || !mxfTimecodeAtStart.isEmpty() ||
               !xmpStartTimecode.isEmpty() || !xmpAltTimecode.isEmpty() ||
               !userdataTimecode.isEmpty();
    }

    QString bestProjectPath() const {
        if (!aeProjectPath.isEmpty())   return aeProjectPath;
        if (!premiereWinPath.isEmpty()) return premiereWinPath;
        if (!premiereMacPath.isEmpty()) return premiereMacPath;
        return {};
    }
};

// Video metadata extracted from the container (Phase 3.B). Adapted
// from old QCView's src/metadata/video_metadata.h — kept the
// inspection-relevant fields, dropped decoder-cache optimization
// state (keyframe_pts, stream_timebase_*, is_intra_codec) which
// belongs in VideoDecoder if we revive it, not the per-item store.
struct VideoMetadata {
    bool        loaded = false;       // true after MetadataService populates

    // File-level
    qint64      fileSize = 0;

    // Video stream
    int         width = 0;
    int         height = 0;
    double      frameRate = 0.0;
    int         totalFrames = 0;
    double      duration = 0.0;       // seconds
    QString     videoCodec;
    // Display-ready codec flavor — "ProRes 422 HQ", "ProRes 4444",
    // h264 "High 4:2:2", etc. ProRes maps the MOV fourcc / stream
    // profile to Apple's marketing names (what export menus say);
    // other codecs use FFmpeg's profile name. Empty when the
    // container doesn't carry one (also: caches saved before this
    // field — no re-probe, the row just stays hidden until the
    // media is re-added).
    QString     codecProfile;
    QString     pixelFormat;          // e.g. "yuv422p10le"
    int         bitDepth = 8;         // detected from pixelFormat
    bool        hasAlpha = false;

    // Sample aspect ratio probed via av_guess_sample_aspect_ratio
    // (reconciles stream + codec SAR). 1:1 for square-pixel content.
    // isAnamorphic == (sarNum != sarDen). Read-only — drives the
    // Inspector's "Detected" pixel-aspect chip; the applied ratio is
    // MediaItem::pixelAspectMode, which defaults to Square regardless.
    int         sarNum = 1;
    int         sarDen = 1;
    bool        isAnamorphic = false;
    // Container display-matrix rotation, normalized to clockwise
    // degrees needed to display the stored frame upright: 0/90/180/
    // 270. Phone footage records landscape and stamps the tkhd
    // matrix (e.g. −90 CCW from av_display_rotation_get → 90 here).
    // −1 = unknown (project cache predates the field); the loader
    // schedules a header-only re-probe and Auto resolution treats
    // <0 as 0 until it lands. Detection only — the applied rotation
    // is MediaItem::rotationOverride (default Auto = this value).
    int         rotationDeg = 0;
    // True when the container has a video stream but FFmpeg has no
    // decoder for it (videoCodec == "unknown") — e.g. ARRIRAW camera
    // raw. Drives the in-viewport "can't play this" notice instead of
    // a silent black frame. cameraVendor/cameraModel come from the
    // container tags (company_name / product_name) so the notice can
    // name the camera ("ALEXA 35 ARRIRAW").
    bool        unsupportedCodec = false;
    QString     cameraVendor;         // container company_name (e.g. "ARRI")
    QString     cameraModel;          // container product_name (e.g. "ALEXA 35")

    // Color (Phase 3.E surfaces the auto-preset wiring)
    QString     colorspace;           // "bt709", "bt2020nc", …
    QString     colorPrimaries;       // "bt709", "bt2020", …
    QString     colorTransfer;        // "bt709", "smpte2084", …
    QString     colorRange;           // "limited" | "full" | "unknown"
    QString     nclcTag;              // "1-1-1" etc.; empty if unset
    bool        isHdrContent = false; // PQ / HLG transfer

    // Audio (set when stream present)
    QString     audioCodec;           // empty if no audio
    int         audioSampleRate = 0;
    int         audioChannels = 0;
    // Number of distinct audio streams in the container — load-
    // bearing for the AudioPlayer / DualAudioMixer dispatch between
    // single-stream (AudioDecoder) and multi-stream
    // (MultiStreamAudioDecoder). Broadcast 5.1+stereo masters land
    // here as 8 (8 mono streams) or 7 (6 mono + 1 stereo). Lets the
    // audio open path skip its own probe — same `find_stream_info`
    // pass the metadata extractor already ran. 0 when no audio.
    int         audioStreamCount = 0;
    // FFmpeg channel-layout string from av_channel_layout_describe.
    // Examples: "stereo", "mono", "5.1(side)", "7.1", "octagonal".
    // Surfaced in the Inspector audio section so reviewers can see
    // the source's intended layout, and used by AudioRoutingMode::Auto
    // (alongside audioChannels) to pick a sensible default downmix.
    QString     audioChannelLayoutName;

    // Embedded timecode (any container source — QT, MXF, etc.)
    bool        hasEmbeddedTimecode = false;
    QString     embeddedTimecode;     // "01:00:00:00" form
};

// Range markers (Phase 7.3). -1.0 = unset (full clip).
struct InOutRange {
    double inPoint  = -1.0;
    double outPoint = -1.0;
};

// Image-sequence specifics. Kept inline so a MediaItem is one
// object regardless of type — saves on lifetime juggling, and the
// fields default to zero when type != ImageSequence.
struct ImageSequenceData {
    QString  pattern;        // e.g. "shot_%04d.exr"
    QString  ffmpegPattern;  // full path pattern for libavformat input
    QString  directory;
    int      frameCount = 0;
    int      startFrame = 1;
    int      endFrame   = 1;
    double   frameRate  = 23.976;
    double   duration   = 0.0;
    int      width  = 0;
    int      height = 0;
    QString  format;         // "EXR", "TIFF", "PNG", "JPEG", …
    QString  layer;          // currently-selected EXR layer
    QStringList availableLayers;  // discovered EXR layers / parts
    // EXR-only compression name probed from the first frame's header
    // ("ZIP", "ZIPS", "DWAA", "DWAB", "PIZ", "RLE", "PXR24", "B44",
    // "B44A", "NONE"). Empty for non-EXR formats and for read errors;
    // inspector hides the row when empty. Populated at import time
    // via EXRImageLoader::compressionName.
    QString  compression;
    QString  audioFile;      // optional companion .wav
    // 0-based frame indices (relative to startFrame) of files that
    // are missing from disk in the [startFrame..endFrame] span.
    // Populated at detection time; surfaced to the timeline so the
    // user sees red ticks where files are missing without having to
    // scrub onto each one. In-memory only; not persisted to project
    // files (re-detected on each open).
    QList<int> missingFrames;

    // Per-item cache stride preference. 1 = every frame (default);
    // 2/3/4 = every Nth frame on the absolute grid (lighter caching
    // for heavy 4K EXR sequences). User picks via the inspector pill;
    // sticks across pool selection within a session so switching to
    // another item and back retains the choice. In-memory only — not
    // persisted to .qcvproj (a stride preference per-item per-session
    // is the right scope; if you reopen the project tomorrow you're
    // probably re-evaluating the heaviness of each clip anyway).
    int cacheStride = 1;
};

// Playlist entry — ordered ref to a media_pool item with optional
// per-entry trim. media_id is the MediaItem.id of the referenced
// pool entry (NOT the playlist's own id).
struct PlaylistEntry {
    QString    mediaId;
    InOutRange range;
};

// Phase 3.H.1 — Playlist save state. Only used when MediaItem.type
// == Playlist. Carries the playlist-level settings (master fps,
// canvas size, default inter-clip gap) plus the ordered list of
// entries. Per Guide 09 D7 the master fps is the timeline's clock;
// per-clip native fps is honored at decoder dispatch (not via
// resampling). Per D9 there's no per-playlist gap-fill policy —
// gaps render transparent and composite against the player's
// background setting.
struct PlaylistData {
    double               masterFps        = 24.0;   // D7
    int                  canvasWidth      = 1920;   // D12
    int                  canvasHeight     = 1080;
    int                  defaultGapFrames = 10;     // D3
    bool                 loop             = false;  // transport-level
    QList<PlaylistEntry> items;
};

// Phase 7.8 — DualPair save state. Only used when MediaItem.type
// == DualPair. Records both source ids, the master playback
// settings, and the post-edit clip layouts for tracks A and B.
//
// `clipsA` and `clipsB` are stored as QVariantLists of clipMaps
// (the same shape `TimelineController::trackA()` returns) so we
// can stay decoupled from `qcv::Clip` (which lives in the
// timeline subsystem). On load, the lists are converted back to
// Clip objects and pushed into the dual session's timeline.
struct DualPairData {
    QString      mediaIdA;        // ref to A's MediaItem
    QString      mediaIdB;        // ref to B's MediaItem
    double       masterFps      = 24.0;
    double       masterDuration = 0.0;
    QVariantList clipsA;          // array of clip maps (post-edit)
    QVariantList clipsB;
    int          inPoint  = -1;   // master in/out (frame numbers)
    int          outPoint = -1;
};

struct MediaItem {
    QString    id;             // UUID, stable across launches
    QString    name;           // user-visible label (defaults to filename)
    QString    path;           // absolute path on disk
    MediaType  type = MediaType::Video;
    double     duration = 0.0;
    bool       hasAudio = false;
    QString    thumbnailPath;  // optional cached thumbnail (Phase 7+)
    InOutRange range;          // user-set in/out within this item

    // Type-specific extras. Inert for types that don't use them.
    ImageSequenceData       imageSeq;
    PlaylistData            playlist;

    // Phase 3.B — populated by MetadataService::extract() shortly
    // after add. `video.loaded` flips true once extraction completes;
    // the Inspector binds to it via the activeItemMap.
    VideoMetadata           video;

    // Phase 3.D — Adobe project links + extended timecodes via
    // exiftool. Runs in parallel with FFmpeg extraction; flips
    // `adobe.loaded` independently.
    AdobeMetadata           adobe;

    // Phase 3.G — per-clip YUV range override. Default Auto follows
    // the detected `video.colorRange`; Full/Limited override the
    // YUV→RGB conversion. Mutated via
    // `ProjectManager::setVideoRangeOverride` so the inspector can
    // edit it without re-extracting metadata.
    VideoRange              videoRangeOverride = VideoRange::Auto;

    // Per-clip pixel-aspect override. Default Square renders the stored
    // (square-pixel) frame untouched. Detected applies video.sar*;
    // Custom applies customParNum/customParDen. Mutated via
    // `ProjectManager::setPixelAspect`; WindowManager pushes the
    // effective ratio to the renderer live and re-applies on load.
    // Persisted with the project — same expectation as
    // videoRangeOverride. customPar* are ignored unless mode == Custom.
    PixelAspectMode         pixelAspectMode = PixelAspectMode::Square;
    int                     customParNum = 1;
    int                     customParDen = 1;

    // Per-clip display-rotation override. −1 = Auto (apply the
    // detected video.rotationDeg); 0/90/180/270 force that clockwise
    // display rotation regardless of container metadata (covers
    // sideways-mounted cameras with no display matrix). Mutated via
    // `ProjectManager::setRotationOverride`; WindowManager pushes the
    // effective quarter-turn to the renderer live (no re-decode),
    // riding the pixel-aspect rails. Persisted with the project.
    int                     rotationOverride = -1;

    // Per-clip audio channel routing for broadcast deliverables.
    // Mutated via `ProjectManager::setAudioRoutingMode`; the
    // WindowManager hooks the change signal to reconfigure the live
    // AudioDecoder's SwrContext so the new routing takes effect
    // immediately. Persisted with the project — a reviewer who
    // picked "5.1 Downmix" once on a master expects that to stick.
    AudioRoutingMode        audioRoutingMode = AudioRoutingMode::Auto;

    // Phase 7.8 — DualPair save state. Populated when type ==
    // DualPair; default-constructed otherwise.
    DualPairData            dualPair;
};

} // namespace qcv
