// MetalHdrSwapchain — Phase 7.5 B.3 / Guide 19.
//
// Adapted from old QCView's src/hdr/metal_hdr_swapchain.{h,mm} (~183
// LoC). Configures CAMetalLayer pixel format + colorspace +
// wantsExtendedDynamicRangeContent for the active HdrMode. macOS
// uses Apple's EDR (linear-light extended dynamic range) — there's
// no PQ/HDR10 path; that lives on Vulkan (Phase C.3).
//
// Adaptation notes vs old app:
//   - Namespace qcview → qcv
//   - Old class had its own EDRColorspace enum + a separate "enabled"
//     bool. We collapse to the new port's HdrMode enum
//     (iplayer_renderer.h) so the chain from WindowManager flows
//     through one type.
//   - All ImGui-side hooks dropped — the old app had to inform
//     ImGui's Metal backend about the colorspace switch so the
//     UI layer rendered into the right encoding. The native player
//     window has no ImGui; nothing to inform.
//   - Hdr10 maps to ExtendedLinearSRgb on macOS with a one-time
//     warning. macOS doesn't expose a PQ surface format.
//
// Threading: apply() mutates the layer's pixelFormat / colorspace.
// CAMetalLayer mutation is documented as thread-safe for these
// properties, but the render thread is reading the same layer (via
// nextDrawable:) — so we apply on the render thread. The setter
// (setMode) just stores the pending mode atomically; the render
// loop applies before nextDrawable: each frame.

#pragma once

#include "iplayer_renderer.h"   // HdrMode

class QWindow;

namespace qcv {

class MetalHdrSwapchain {
public:
    MetalHdrSwapchain();
    ~MetalHdrSwapchain();

    MetalHdrSwapchain(const MetalHdrSwapchain &)            = delete;
    MetalHdrSwapchain &operator=(const MetalHdrSwapchain &) = delete;

    // `layer` is a CAMetalLayer*, `window` is a QWindow* (used to
    // resolve the NSWindow → NSScreen for headroom queries). Both
    // weak refs — caller owns lifetime.
    void initialize(void *layer, QWindow *window);
    void shutdown();

    // GUI-thread setter — records the mode. apply() picks it up.
    void setMode(HdrMode mode);

    // Render-thread call — if the pending mode differs from the
    // applied mode, mutates the layer and updates state. Cheap when
    // the mode hasn't changed (atomic read + compare).
    void applyPendingOnRenderThread();

    // Diagnostics / capability queries.
    bool   isHdrSupported() const;     // any EDR headroom > 1.0
    float  maxEdrHeadroom() const;     // 1.0 on SDR-only displays

    HdrMode appliedMode() const { return m_appliedMode; }

private:
    void applyImpl(HdrMode mode);

    void                  *m_layer  = nullptr;   // CAMetalLayer*
    QWindow               *m_window = nullptr;   // for NSScreen headroom

    HdrMode                m_appliedMode = HdrMode::SdrSRgb;
    // Atomic-ish: only ever set on GUI thread, read on render thread.
    // HdrMode is an enum class with int underlying type → trivially
    // copyable; aligned access on int-sized types is atomic on ARM64
    // and x86-64. We don't bother with std::atomic to keep the type
    // copyable in struct fields.
    HdrMode                m_pendingMode = HdrMode::SdrSRgb;
};

} // namespace qcv
