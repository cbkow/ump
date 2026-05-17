#include "player_window_factory.h"

#include "iplayer_renderer.h"

#include <QtLogging>

#include <utility>

#if defined(Q_OS_MACOS)
#  include "metal/metal_player_renderer.h"
#elif defined(Q_OS_WIN)
#  include "d3d11/d3d11_player_renderer.h"
#endif

namespace qcv {

namespace {

// ---------------------------------------------------------------------
// Stub renderer — used as the non-Apple fallback while Phase C
// (Vulkan) is still in flight. Holds onto config but does no
// rendering; the window's OS-managed surface backing displays as
// black. Phase C replaces this with VulkanPlayerRenderer.
// ---------------------------------------------------------------------
class StubPlayerRenderer final : public IPlayerRenderer {
public:
    bool init(PlayerWindow * /*window*/) override { return true; }
    void shutdown() override {}
    void requestUpdate() override {}
    void resize(int /*w*/, int /*h*/) override {}

    void setHdrMode(HdrMode mode) override                  { m_hdrMode  = mode; }
    void setImageSeqCache(ImageSequenceCache *c) override   { m_cache    = c; }
    void setVideoDecoder(VideoDecoder *d) override          { m_decoder  = d; }
    void setOcio(OCIOConfigManager *o) override             { m_ocio     = o; }
    void setCompositorMode(CompositorMode m) override       { m_compMode = m; }
    void setSplitPos(float p) override                      { m_splitPos = p; }
    void setBackgroundMode(BackgroundMode m) override       { m_bgMode = m; }
    void setViewportAnnotator(ViewportAnnotator *a) override { m_annotator = a; }
    void setSafetyOverlay(SafetyOverlay *s) override         { m_safety = s; }

private:
    HdrMode             m_hdrMode   = HdrMode::SdrSRgb;
    ImageSequenceCache *m_cache     = nullptr;
    VideoDecoder       *m_decoder   = nullptr;
    OCIOConfigManager  *m_ocio      = nullptr;
    CompositorMode      m_compMode  = CompositorMode::Single;
    float               m_splitPos  = 0.5f;
    BackgroundMode      m_bgMode    = BackgroundMode::Black;
    ViewportAnnotator  *m_annotator = nullptr;
    SafetyOverlay      *m_safety    = nullptr;
};

} // namespace

std::unique_ptr<IPlayerRenderer> createPlayerRenderer()
{
#if defined(Q_OS_MACOS)
    // Phase B.2: MetalPlayerRenderer drives the present loop on its
    // own thread.
    return std::make_unique<MetalPlayerRenderer>();
#elif defined(Q_OS_WIN)
    // Phase F.2.1: D3D11PlayerRenderer with child-HWND + DComp present.
    return std::make_unique<D3D11PlayerRenderer>();
#else
    // Linux / other: stub (Vulkan render path is gated off; see
    // project_overview memory for current state).
    return std::make_unique<StubPlayerRenderer>();
#endif
}

QSurface::SurfaceType playerWindowSurfaceType()
{
#if defined(Q_OS_MACOS)
    return QSurface::MetalSurface;
#elif defined(Q_OS_WIN)
    // The Windows renderer owns its own child HWND + D3D11 swapchain
    // composed via DComp. PlayerWindow itself is never shown (no
    // native HWND), so its surface type is moot — RasterSurface is
    // the cheap/safe choice that doesn't trigger Vulkan/D3D11 surface
    // allocation by Qt.
    return QSurface::RasterSurface;
#else
    return QSurface::VulkanSurface;
#endif
}

} // namespace qcv
