// IDualPixbufConverter — task #107 / #108 commit 2.
//
// Abstract bridge between qcv_dual's HW-decoded CVPixelBuffer frames
// and qcv_render's Metal pipelines (CvPixbufMetalBridge + Metal
// YuvRenderer). Lives in qcv_dual so DualCompositor can call into
// it without taking a build dependency on qcv_render — the renderer
// implements the interface and injects an instance via
// DualCompositor::setPixbufConverter() at dual-mode entry.
//
// Lifecycle: caller (MetalPlayerRenderer) owns the impl; passes a
// raw pointer to the compositor. Compositor MUST clear via
// setPixbufConverter(nullptr) before the impl is destroyed.

#pragma once

namespace qcv::dual {

class IDualPixbufConverter {
public:
    virtual ~IDualPixbufConverter() = default;

    // Convert a CVPixelBufferRef (passed as void*) to an RGBA MTLTexture
    // (returned as void* — id<MTLTexture> with +1 retain managed by the
    // converter; caller binds but does not release).
    //
    // Texture lifetime: valid until the next convertToRgba() call with
    // the same `slot`. The converter caches one bridge + one YUV
    // intermediate per slot to avoid recreating CVMetalTextureCache
    // state between frames.
    //
    // Slot 0 = side A; slot 1 = side B. Only those two are supported.
    //
    // `cmdBuffer` is the active id<MTLCommandBuffer> (as void*). The
    // converter encodes a YUV→RGB compute pass on it; caller commits
    // the buffer once all per-frame work is encoded.
    //
    // `rangeOverride` mirrors the per-clip Inspector Range pill: 0 =
    // Auto (use the value the CvPixbufMetalBridge derived from the
    // CVPixelFormatType `*VideoRange` vs `*FullRange` suffix), 1 =
    // Full (stomp fullRange = true), 2 = Limited (stomp fullRange =
    // false). Same logic as single-flow's metal_player_renderer.mm
    // YUV upload helper.
    //
    // `outW`, `outH` receive the source dimensions on success.
    // Returns nullptr if conversion fails (bad format, bridge cache
    // exhausted, etc.); caller falls back to rendering that side
    // transparent for the frame.
    virtual void *convertToRgba(void *cmdBuffer,
                                  void *cvPixelBuffer,
                                  int slot,
                                  int *outW, int *outH,
                                  int rangeOverride = 0) = 0;
};

} // namespace qcv::dual
