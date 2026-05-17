// ZeroCopyBridgeMetal — Phase 1.8.2b.
//
// Wraps a CVPixelBufferRef from VideoToolbox as a set of QRhiTextures,
// using IOSurface → MTLTexture under the hood (zero-copy). Each plane
// of the source CVPixelBuffer maps to a QRhiTexture; the YUV→RGB
// conversion is done by a Pass-0 shader the renderer adds in front
// of the existing compositor.
//
// Lifetime contract:
// - One ZeroCopyBridgeMetal per QRhi instance. Created lazily on the
//   render thread when the first Metal-kind FrameHandle arrives.
// - Caller owns the CVPixelBuffer and must keep it alive at least
//   until the QRhiTextures returned here are no longer in use by the
//   GPU. The render thread holds the FrameHandle until next fetch.
// - The bridge holds an internal cache of CVMetalTextureRefs which
//   it releases on next bridge() call (replacing previous frame's
//   wrappers). On destruction all refs are released.
//
// Supported pixel formats (Phase 1.8.2b):
//   kCVPixelFormatType_420YpCbCr8BiPlanar*    (NV12 — H.264, HEVC SDR)
//   kCVPixelFormatType_422YpCbCr10            (P210-style — ProRes 422)
//
// Anything else returns Result::Unsupported and the caller should
// fall back to the CPU readback path (1.8.2a behavior).

#pragma once

#include <QSize>
#include <memory>
#include <vector>

class QRhi;
class QRhiTexture;

namespace qcv {

class FrameHandle;

class ZeroCopyBridgeMetal
{
public:
    enum class Result {
        Ok,             // out has 2 plane textures (Y, UV) populated
        Unsupported,    // pixel format not handled — caller falls back
        Failed,         // pixel buffer lock or texture create failed
    };

    enum class YuvLayout {
        Unknown    = 0,
        NV12_8bit  = 1,   // biplanar, Y r8, UV rg8,   4:2:0 (H.264, HEVC SDR)
        P210_10bit = 2,   // biplanar, Y r16, UV rg16, 4:2:2 (10 in 16 bits)
        P010_10bit = 3,   // biplanar, Y r16, UV rg16, 4:2:0 (10 in 16 bits)
        AYpCbCr16  = 4,   // packed,   single rgba16, A/Y/Cb/Cr (ProRes 4444)
    };

    struct Bridged {
        // For biplanar layouts: yPlane = Y, uvPlane = UV.
        // For packed layouts (AYpCbCr16): yPlane = the single packed
        // plane, uvPlane = nullptr. The renderer dispatches to a
        // different shader pipeline based on `layout`.
        QRhiTexture *yPlane  = nullptr;
        QRhiTexture *uvPlane = nullptr;
        YuvLayout    layout  = YuvLayout::Unknown;
        int          colorMatrix = 1;     // 0=BT.601, 1=BT.709, 2=BT.2020
        int          fullRange  = 0;      // 1 if PC-range YCbCr
    };

    explicit ZeroCopyBridgeMetal(QRhi *rhi);
    ~ZeroCopyBridgeMetal();

    ZeroCopyBridgeMetal(const ZeroCopyBridgeMetal &) = delete;
    ZeroCopyBridgeMetal &operator=(const ZeroCopyBridgeMetal &) = delete;

    // Wrap the given Metal-kind FrameHandle's CVPixelBuffer as plane
    // textures. The previous frame's wrappers are released before
    // building new ones. On Result::Unsupported / Failed, *out is
    // left in a default state.
    Result bridge(const FrameHandle &handle, Bridged *out);

    // Static format probe — lets the decoder ask "if I publish this
    // CVPixelBuffer as a Metal handle, can the renderer's bridge
    // handle its pixel format?" before deciding whether to take the
    // zero-copy path or fall back to av_hwframe_transfer_data + CPU
    // upload. Pass a CVPixelBufferRef as void*; safe to call from
    // any thread.
    static bool canBridge(void *cvPixelBuffer);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace qcv
