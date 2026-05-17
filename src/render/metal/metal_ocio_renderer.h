// MetalOcioRenderer — Phase 7.5 B.6.4 / Guide 19 §2.4.
//
// Applies the active OCIO color transform via a Metal compute pass.
//
// Architecture: OcioChainBuilder asks OCIO for an MSL_2_0 shader
// function (OCIO supports MSL natively in 2.x). We wrap it in a
// compute kernel template, compile via newLibraryWithSource:, and
// upload the OCIO-emitted LUTs as MTLTextures. Each frame, we
// dispatch the kernel against the source RGBA16F texture and write
// to a persistent output texture; the compositor then samples that.
//
// Adapted from old QCView's src/color/metal_ocio_renderer.{h,mm}
// (~675 LoC). Differences:
//   - Old app's OCIOPipeline class baked MSL via its own logic; new
//     port asks OCIO directly via OcioChainBuilder(Language::Msl_2_0).
//   - Output texture is a single persistent RGBA16F that's reused
//     across frames; reallocated only when source dimensions change.
//   - Dropped: ApplyPassthrough, ApplyLinearToSRGB, CopyTextureSync.
//     Phase B.6.4.b can revisit if needed.
//
// Threading: rebuild() runs on the render thread when the OCIO
// chain generation bumps; apply() runs on the render thread per
// frame.

#pragma once

#include <QString>

#include <atomic>
#include <vector>

namespace qcv {

class OCIOConfigManager;

class MetalOcioRenderer {
public:
    MetalOcioRenderer();
    ~MetalOcioRenderer();

    MetalOcioRenderer(const MetalOcioRenderer &)            = delete;
    MetalOcioRenderer &operator=(const MetalOcioRenderer &) = delete;

    bool initialize();
    void shutdown();
    bool isInitialized() const;

    // Build / rebuild the compute pipeline + LUT textures from the
    // active OCIO chain. Idempotent if the chain hasn't changed
    // (cached by the chain generation atomic). Returns true on success.
    bool rebuild(OCIOConfigManager *ocio);

    // Apply the current pipeline to `sourceMtlTexture` (RGBA16F,
    // already-decoded source frame). Encodes a compute pass on
    // `cmdBuf` (id<MTLCommandBuffer> as void*). On success, returns
    // the output id<MTLTexture> as void* — caller does NOT release;
    // owned by this renderer, reused across frames. Returns nullptr
    // if not built / source mismatched.
    void *apply(void *cmdBuf, void *sourceMtlTexture,
                int width, int height);

    // True if rebuild() succeeded and we have a usable pipeline.
    bool hasPipeline() const;

    // Last error from rebuild(); empty if successful.
    const QString &lastError() const;

private:
    struct Impl;
    Impl *m_impl = nullptr;
};

} // namespace qcv
