// MetalYuvRenderer — Phase 7.5 B.6.3 / Guide 19 §2.1.
//
// YUV → RGBA16F compute pass for video frames produced by FFmpeg+
// VideoToolbox hwaccel. Two pipelines:
//   - Biplanar (NV12 / P010 / P210): Y plane + interleaved UV plane
//     covering 4:2:0, 4:2:2, 4:4:4 chroma subsampling.
//   - Interleaved (Y416 / Y408): single RGBA-typed texture where
//     channels are A, Y, Cb, Cr — used by ProRes 4444 alpha video.
//
// Both pipelines write into a private RGBA16F output texture sized
// to match the source. The texture's lifetime is the caller's; the
// renderer creates it fresh each call.
//
// Adapted from old QCView's src/gpu/metal_yuv_renderer.{h,mm} (~626
// LoC). The MSL compute kernels are byte-identical (color-matrix
// constants, video-range math, BT.709 / BT.2020 branches). Async
// variants from the old app are dropped — the new render-thread
// architecture runs YUV→RGBA inline each present, so per-frame
// sync is simpler than the old "decode thread → async compute →
// pool register" dance.

#pragma once

namespace qcv {

class MetalYuvRenderer {
public:
    MetalYuvRenderer();
    ~MetalYuvRenderer();

    MetalYuvRenderer(const MetalYuvRenderer &)            = delete;
    MetalYuvRenderer &operator=(const MetalYuvRenderer &) = delete;

    bool initialize();
    void shutdown();
    bool isInitialized() const;

    // Biplanar YUV → RGBA16F. yTexture is single-channel R8/R16,
    // uvTexture is two-channel RG8/RG16. subsampling: 0=4:2:0,
    // 1=4:2:2, 2=4:4:4. Returns a +1-retained id<MTLTexture> as
    // void*; caller takes ownership (__bridge_transfer back to ARC).
    //
    // `cbPtr` is the caller's id<MTLCommandBuffer>. The compute
    // encoder is opened on that buffer; we don't commit it here.
    // The caller is responsible for committing once all per-frame
    // work (YUV-A, YUV-B, composite, OCIO, present) is encoded.
    // Single-cb-per-frame keeps Metal's intra-buffer dependency
    // tracking active so the compositor sees these RGBA outputs
    // without an explicit CPU-side waitUntilCompleted (which
    // previously stalled the render thread, doubling the stall
    // in dual-source mode).
    void *renderToRgba(void *cbPtr,
                       void *yTexture, void *uvTexture,
                       int width, int height,
                       int bitDepth, bool isFullRange,
                       bool isBt2020, bool isHdr,
                       int subsampling = 0);

    // Interleaved 4444+alpha (Y416 layout: R=A, G=Y, B=Cb, A=Cr) →
    // RGBA16F. Encodes into the caller-supplied command buffer;
    // see renderToRgba note above.
    void *renderInterleavedToRgba(void *cbPtr, void *inTexture,
                                  int width, int height,
                                  int bitDepth, bool isFullRange,
                                  bool isBt2020, bool isHdr);

    // Impl is exposed for free-function helpers in the .mm. Nothing
    // in the header references it; consumers can ignore.
    struct Impl;

private:
    bool createComputePipelines();

    Impl *m_impl = nullptr;
};

} // namespace qcv
