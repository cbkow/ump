// VideoImageLoader — port of old QCView's image_loaders.cpp
// VideoImageLoader. Self-contained FFmpeg single-frame loader for
// thumbnail extraction; each instance owns its own AVFormatContext +
// AVCodecContext so multiple workers can decode concurrently with no
// shared mutex.
//
// Used by TimelineThumbnailCache: each worker constructs one
// VideoImageLoader per source video path and caches it for the
// session. loadThumbnail() takes a frame-number-as-string in the
// `path` parameter (mirrors the IImageLoader interface where image-
// sequence loaders take an actual file path); internally we seek to
// timestamp = (frame + 0.5) / fps + container_start_offset, decode
// forward to the closest frame, and downsample to max_size via
// SWS_POINT.
//
// Adaptations vs old app:
//   - namespace qcview → qcv
//   - Debug::Log → qDebug
//   - Method names PascalCase → camelCase (loadFrame / loadThumbnail
//     / getDimensions / loaderName) to match the new IImageLoader
//   - ConversionStrategy / format-specific YUV→RGB matrix support
//     dropped for this port — the old app's strategy module ports
//     separately if/when full-quality thumbnails are needed for
//     ProRes 4444/422. The default swscale path is fine for
//     hover-preview tiles.
//
// Thread safety: each LoadFrame holds an internal mutex while the
// FFmpeg context is in use, so two threads sharing one instance is
// safe-but-serial. The cache always uses a per-worker instance, so
// the mutex is uncontended at runtime.

#pragma once

#include "image_loader.h"

#include <mutex>

extern "C" {
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct SwsContext;
}

namespace qcv {

class VideoImageLoader : public IImageLoader {
public:
    // Initialize with video path + (optional) hint metadata. The
    // loader auto-detects fps / duration from the container; the
    // hint values are used only when detection returns 0.
    VideoImageLoader(const std::string &videoPath,
                     double fpsHint = 24.0,
                     double durationHint = 0.0);
    ~VideoImageLoader() override;

    VideoImageLoader(const VideoImageLoader &)            = delete;
    VideoImageLoader &operator=(const VideoImageLoader &) = delete;

    // `path` must be the frame number as a decimal string ("0",
    // "42"); `layer` is ignored.
    std::shared_ptr<PixelData> loadFrame(
        const std::string &path,
        const std::string &layer,
        PipelineMode pipeline_mode) override;

    // Same input convention as loadFrame; output is RGBA8 capped
    // at max_size on the longer edge (aspect preserved).
    std::shared_ptr<PixelData> loadThumbnail(
        const std::string &path, int max_size = 320) override;

    bool getDimensions(const std::string &path,
                       int &width, int &height) override;

    std::string loaderName() const override { return "Video"; }

    double frameRate()  const { return m_fps; }
    double duration()   const { return m_duration; }
    int    frameCount() const;
    bool   isInitialized() const { return m_initialized; }

private:
    bool initializeFfmpeg();
    void cleanupFfmpeg();

    // Allocates an AVFrame via the cache, calls extractFrame, returns
    // the result.
    std::shared_ptr<PixelData> extractFrame(int frameNumber,
                                            PipelineMode mode,
                                            int max_size);

    // Seek to `timestampSec`, walk decoded frames forward, return
    // the closest one. Returns true on success.
    bool seekAndDecodeFrame(double timestampSec, AVFrame *output);

    // YUV → RGBA8 via swscale (cached SwsContext). Optionally
    // downsamples to max_size during the same pass (SWS_POINT).
    bool convertFrameToPixels(AVFrame *frame,
                              std::vector<std::uint8_t> &outPixels,
                              int &outW, int &outH,
                              PipelineMode mode,
                              int max_size);

    std::string m_videoPath;
    double      m_fps      = 24.0;
    double      m_duration = 0.0;
    int         m_width    = 0;
    int         m_height   = 0;
    // Container start_time in AV_TIME_BASE units; non-zero on HEVC
    // and other formats whose first PTS != 0.
    long long   m_startTime = 0;

    AVFormatContext *m_formatCtx       = nullptr;
    AVCodecContext  *m_codecCtx        = nullptr;
    int              m_videoStreamIdx  = -1;

    // Cached swscale ctx — re-allocate only when src/dst geometry
    // changes (per-frame allocation otherwise leaks heavily).
    SwsContext *m_swsCtx     = nullptr;
    int         m_swsSrcW    = 0;
    int         m_swsSrcH    = 0;
    int         m_swsSrcFmt  = -1;
    int         m_swsDstW    = 0;
    int         m_swsDstH    = 0;

    std::mutex m_mutex;
    bool       m_initialized = false;
};

} // namespace qcv
