// IDualSource — Phase 7.7 dual playback island.
//
// Abstract source for the dual-source flow. Concrete impls:
//   - DualVideoDecoder      (FFmpeg video, 16-frame ring buffer)
//   - DualImageSeqSource    (parallel image-seq cache; Stage 2)
//
// The compositor pulls per-side via getBufferedFrame(N). The master
// clock (DualPlaybackTimer) advances both sources' decode_target each
// tick; sources independently fill ahead until their ring is half-full
// past the target, then idle. No wall-clock sleep in any source —
// pacing is the timer's job.
//
// Stage 1: only DualVideoDecoder exists. Frames are RGBA8 QImages
// (CPU/sws_scale path). Stage 3 introduces a pool-handle return type
// and Metal hwaccel.

#pragma once

#include <QImage>
#include <QString>
#include <cstdint>
#include <functional>
#include <memory>

namespace qcv::dual {

// Buffered frame from a dual-flow source. Tagged-kind so we can
// carry either CPU pixels (image-seq, software-decoded video) OR a
// platform-native HW frame handle (CVPixelBuffer on macOS, AVVkFrame
// on Windows via FFmpeg's Vulkan hwaccel) without forcing a CPU
// bounce on the HW-decoded path. The compositor branches on `kind`:
//   - Cpu    → cached MTLTexture via replaceRegion (existing path).
//              The QImage's format determines the texture format:
//              Format_RGBA8888 → MTLPixelFormatRGBA8Unorm
//              Format_RGBA16FPx4 → MTLPixelFormatRGBA16Float
//              EXR sources land here as RGBA16F so OCIO sees full
//              FP16 precision instead of an 8-bit clamp.
//   - Metal  → IDualPixbufConverter routes through CvPixbufMetalBridge
//              + MetalYuvRenderer for zero-copy GPU YUV→RGB (macOS).
//   - Vulkan → AVFrame* with AVVkFrame in data[0]; consumed by the
//              D3D11VulkanDecodeBridge import path on Windows. Mirrors
//              FrameHandle::Vulkan exactly so dual reuses single-flow's
//              bridge instead of inventing a parallel readback path.
struct DualFrame {
    enum class Kind {
        Empty,
        Cpu,     // rgba populated; format selects RGBA8 vs RGBA16F upload
        Metal,   // cvPixelBuffer populated (CVPixelBufferRef as void*)
        Vulkan,  // avFrame populated (AVFrame* owning AVVkFrame ref)
    };

    int  frameNumber = -1;
    int  width  = 0;
    int  height = 0;
    Kind kind   = Kind::Empty;

    // Per-side videoRangeOverride from the source MediaItem (0 = Auto,
    // 1 = Full, 2 = Limited). Mirrors the Inspector Range pill, same
    // semantics as single-flow's VideoDecoder::rangeOverride(). For
    // Cpu kind the decoder has ALREADY applied this when running
    // sws_setColorspaceDetails, so downstream consumers don't need to
    // re-apply. For Metal kind the cvPixelBuffer is still in YUV; the
    // compositor passes this value to IDualPixbufConverter so the
    // YUV→RGB pass honors the override.
    int rangeOverride = 0;

    // Cpu kind. QImage::format() == Format_RGBA8888 for SDR sources
    // (PNG/JPEG/TIFF/software-decoded video); Format_RGBA16FPx4 for
    // EXR (preserves linear FP16 through OCIO).
    std::shared_ptr<QImage> rgba;

    // Metal kind. shared_ptr<void> with a CFRelease deleter — see
    // dual_cv_helpers.mm for the deleter implementation. The pointer
    // is a CVPixelBufferRef.
    std::shared_ptr<void> cvPixelBuffer;

    // Vulkan kind. shared_ptr<void> with an av_frame_free deleter (see
    // DualVideoDecoder's Windows publish path). The pointer is the
    // owning AVFrame* whose data[0] holds the AVVkFrame returned by
    // FFmpeg's Vulkan hwaccel. Renderer downcasts to AVFrame* on the
    // qcv_dual side and feeds the bridge. Kept as shared_ptr<void> so
    // i_dual_source.h doesn't need to drag <libavutil/frame.h> in.
    std::shared_ptr<void> avFrame;

    bool valid() const {
        switch (kind) {
        case Kind::Cpu:    return rgba && !rgba->isNull();
        case Kind::Metal:  return cvPixelBuffer.get() != nullptr;
        case Kind::Vulkan: return avFrame.get() != nullptr;
        case Kind::Empty:  return false;
        }
        return false;
    }
};

class IDualSource {
public:
    virtual ~IDualSource() = default;

    // ---- Lifecycle (GUI thread) ----
    virtual bool open(const QString &path) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // ---- Decode-ahead control ----
    // Called by DualPlaybackTimer each master-clock tick. The source
    // adjusts its decode-ahead window around `frameNumber` and wakes
    // its decode thread if more frames are needed.
    virtual void setDecodeTarget(int frameNumber) = 0;

    // Big-seek to `frameNumber`. Clears the ring buffer and re-bursts
    // up to target. Decode thread takes care of keyframe-at-or-before
    // search internally.
    virtual void seekTo(int frameNumber) = 0;

    // ---- Render-thread pull ----
    // Returns the buffered frame with exact match `frameNumber`, or
    // nullptr if not in the buffer.
    virtual std::shared_ptr<DualFrame> getBufferedFrame(int frameNumber) const = 0;
    virtual bool hasFrame(int frameNumber) const = 0;

    // ---- Diagnostics (Stage 1 verification surface) ----
    virtual int  bufferedAhead() const = 0;
    virtual int  bufferedBehind() const = 0;
    virtual int  bufferCount() const = 0;
    virtual void getBufferedRange(int &startFrame, int &endFrame) const = 0;

    // ---- Metadata ----
    virtual int    width() const = 0;
    virtual int    height() const = 0;
    virtual double fps() const = 0;
    virtual int    frameCount() const = 0;
    virtual QString path() const = 0;

    // Human-readable hwaccel name ("vulkan", "videotoolbox", etc.) or
    // an empty string when no hwaccel is attached (CPU decode / image
    // sequence). Default impl returns empty so sources that don't
    // care don't need to override.
    virtual QString hwAccelName() const { return QString(); }

    // Per-side videoRangeOverride from the source MediaItem (0 = Auto,
    // 1 = Full, 2 = Limited). DualPlaybackController pushes this from
    // its own setRangeOverrideA/B atomics so the source applies it at
    // decode/convert time. Default no-op for image-seq sources (no YUV
    // range concept); DualVideoDecoder overrides to atomic-store and
    // invalidate the ring on change so cached CPU-converted frames
    // don't linger with the wrong range.
    virtual void setRangeOverride(int /*v*/) {}

    // Optional wake-on-frame hook. Fired (typically on the source's
    // decode / worker thread) once a newly decoded or loaded frame is
    // available for pull. The owner (WindowManager) installs a
    // callback that calls the renderer's requestUpdate() so the
    // render-on-demand loop redraws after an async decode completes.
    //
    // Without this, a timeline seek while paused can leave the
    // viewport on a stale frame: currentFrameChanged fires (and
    // requests one redraw) the instant the master frame number
    // changes, but the decoder hasn't produced the target frame yet
    // — the redraw pulls Empty / stale and nothing wakes the
    // renderer once the real frame lands. Both concrete sources
    // implement this; default no-op keeps the interface optional.
    using FrameAvailableCallback = std::function<void()>;
    virtual void setFrameAvailableCallback(FrameAvailableCallback /*cb*/) {}
};

} // namespace qcv::dual
