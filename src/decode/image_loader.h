// Image loader interface — Phase 7.4.a port from old QCView's
// `image_loader_interface.h`. Defines PixelData (the
// platform-agnostic CPU pixel buffer the loaders emit), sentinel
// factories for gap / broken frames, and IImageLoader (the
// runtime-polymorphic interface every per-format loader implements).
//
// Unlike the old version this header does NOT include GL headers —
// the new app's render layer consumes pixels via Qt + QRhi, so the
// decode layer stays format-only. Conversion to FrameHandle happens
// at the publish boundary (WindowManager).

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qcv {

// PipelineMode — color-depth tier per source, used to pick the
// downstream FP16 vs 8-bit path. Ported from the old app's
// `pipeline_mode.h` (kept inline here since it's only ever used by
// loaders + the renderer adapter).
enum class PipelineMode {
    NORMAL          = 0,    // 8-bit RGBA (sRGB)
    HIGH_RES        = 1,    // 16-bit RGBA (deep-color stills, 16-bit PNG)
    ULTRA_HIGH_RES  = 2,    // 16-bit half-float (EXR)
};

// Cross-platform pixel-format enum. RGBA, channels always 4 to
// keep upload paths uniform — loaders synthesize alpha if missing.
enum class PixelFormat {
    RGBA8,      // 4 B/px, unsigned byte
    RGBA16,     // 8 B/px, unsigned short
    RGBA16F,    // 8 B/px, half float
};

inline std::size_t PixelFormatBytesPerPixel(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::RGBA8:   return 4;
        case PixelFormat::RGBA16:  return 8;
        case PixelFormat::RGBA16F: return 8;
    }
    return 4;
}

// CPU pixel buffer emitted by every IImageLoader. Raw bytes; the
// caller interprets via `pixel_format`.
struct PixelData {
    std::vector<std::uint8_t> pixels;       // raw bytes
    int          width  = 0;
    int          height = 0;
    PixelFormat  pixel_format = PixelFormat::RGBA8;
    PipelineMode pipeline_mode = PipelineMode::NORMAL;
    bool         is_sentinel = false;       // gap/broken marker

    std::size_t byteSize() const { return pixels.size(); }
    void setFormat(PixelFormat fmt) { pixel_format = fmt; }
};

// Files smaller than this are treated as truncated/corrupt — the
// loader bails before opening libpng / OpenEXR / etc. and the
// decoder substitutes a broken-frame sentinel. Same threshold as
// the old app.
inline constexpr std::size_t kBrokenFileThresholdBytes = 15 * 1024;

// Gap sentinel — transparent RGBA8, full source dimensions. Used
// when a frame number's file does not exist (frame missing from
// the on-disk sequence but the timeline expects something).
std::shared_ptr<PixelData> MakeGapSentinel(int w = 1, int h = 1);

// Broken-frame sentinel — solid mid-gray (#787878) at full source
// dimensions. The old app overlays a centered broken.png icon; the
// port starts plain and adds the icon back when we wire a thumbnail
// path in 7.4.b.
std::shared_ptr<PixelData> MakeBrokenSentinel(int w = 1, int h = 1);

inline bool IsSentinel(const std::shared_ptr<PixelData> &pd) {
    return pd && pd->is_sentinel;
}

// Runtime-polymorphic image loader. Per-format implementations live
// alongside this header (png_image_loader.h, exr_image_loader.h
// in 7.4.b, …). Loaders are constructed per-task on the I/O thread
// — see the comment in ImageSequenceDecoder::IOThread about
// "fresh loader per async task" for thread-safety reasoning.
class IImageLoader {
public:
    virtual ~IImageLoader() = default;

    // Load a frame from disk → CPU pixel buffer.
    // `layer` is used by EXR multi-layer loaders; ignored by PNG/TIFF/JPEG.
    // `pipeline_mode` is a hint — loaders are free to upgrade
    // (e.g. a 16-bit PNG ignores NORMAL and emits HIGH_RES anyway).
    virtual std::shared_ptr<PixelData> loadFrame(
        const std::string &path,
        const std::string &layer,
        PipelineMode pipeline_mode) = 0;

    // Optional fast path for thumbnail generation. Default impl
    // just calls loadFrame; PNG/JPEG override with format-specific
    // optimizations (libjpeg DCT scaling, PNG skip-downsampling).
    virtual std::shared_ptr<PixelData> loadThumbnail(
        const std::string &path, int max_size = 320)
    {
        (void)max_size;
        return loadFrame(path, std::string(), PipelineMode::NORMAL);
    }

    // Latency-tier decode hint (2026-07-08 EXR perf audit). N > 0
    // asks the loader to parallelize a SINGLE frame's decode across
    // N internal threads — for latency-critical frames only (seek
    // target, first frame). Read-ahead/playback callers leave it 0:
    // the worker pool already saturates cores frame-parallel, and
    // sharing an internal pool there would cut throughput. Default
    // no-op; only formats with chunked decoders (EXR) implement it.
    virtual void setDecodeThreads(int n) { (void)n; }

    // Fast metadata read — width/height without decoding pixels.
    virtual bool getDimensions(const std::string &path,
                               int &width, int &height) = 0;

    virtual std::string loaderName() const = 0;
};

} // namespace qcv
