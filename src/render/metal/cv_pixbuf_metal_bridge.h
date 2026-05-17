// CvPixbufMetalBridge — Phase 7.5 B.6.3.
//
// Wraps a CVPixelBufferRef (from FrameHandle::Kind::Metal — i.e.,
// hwaccel video out of FFmpeg+VideoToolbox) as a pair of MTLTextures
// for the YUV→RGBA compute pass to consume.
//
// Mirrors src/render/zero_copy_bridge_metal.{h,mm} but emits raw
// MTLTextures instead of QRhiTextures. Phase D will collapse the two
// when the QQuickRhiItem player is stripped (see Guide 18 cleanup
// discipline).
//
// Threading: created + used on the render thread. The internal
// CVMetalTextureCache is single-thread-owned per the Apple docs.

#pragma once

#include <cstdint>
#include <memory>

namespace qcv {

class FrameHandle;

class CvPixbufMetalBridge {
public:
    // Format / range / matrix metadata extracted from the pixel buffer.
    // Maps to the YUV renderer's parameters: subsampling (0/1/2),
    // is_full_range, is_bt2020 (matrix index 2), bit_depth.
    struct PlaneSet {
        // Both raw id<MTLTexture> as void*, valid only until the next
        // bridge() call (the bridge releases prior frame's wrappers
        // on each call).
        void *yTexture  = nullptr;
        void *uvTexture = nullptr;        // nil for packed/interleaved layouts
        void *packedTexture = nullptr;    // for AYpCbCr16 (Y416/Y408)

        int  width      = 0;     // source dimensions (Y plane / packed)
        int  height     = 0;
        int  bitDepth   = 8;
        int  subsampling = 0;    // 0 = 4:2:0, 1 = 4:2:2, 2 = 4:4:4
        bool fullRange  = false;
        bool isBt2020   = false; // matrix == BT.2020
        bool isHdr      = false; // PQ / HLG transfer

        bool isPacked() const { return packedTexture != nullptr; }
        bool valid() const    { return yTexture != nullptr || packedTexture != nullptr; }
    };

    enum class Result {
        Ok,
        Unsupported,   // unknown CVPixelFormat — caller should fall back
        Failed,
    };

    CvPixbufMetalBridge();
    ~CvPixbufMetalBridge();

    CvPixbufMetalBridge(const CvPixbufMetalBridge &)            = delete;
    CvPixbufMetalBridge &operator=(const CvPixbufMetalBridge &) = delete;

    bool initialize();
    void shutdown();
    bool isInitialized() const;

    // Bridge a FrameHandle (must be Kind::Metal). On success, `out`
    // is populated; ownership of the underlying CVMetalTextureRefs
    // stays with the bridge (released on the next bridge() / dtor).
    Result bridge(const FrameHandle &handle, PlaneSet *out);

private:
    struct Impl;
    Impl *m_impl = nullptr;
};

} // namespace qcv
