// IPlayerRenderer — Phase 7.5 A.3 / Guide 20.
//
// Per-platform renderer behind the native PlayerWindow. Concrete
// implementations land in src/render/metal/metal_player_renderer.{h,mm}
// (Phase B, macOS) and src/render/vulkan/vulkan_player_renderer.{h,cpp}
// (Phase C, Windows/Linux). Phase A ships stub implementations: they
// own a surface but don't yet render anything beyond the OS-managed
// black backing.
//
// All renderer methods are called from PlayerWindow's render thread
// EXCEPT init() / shutdown() / requestUpdate() / the setters
// (setVideoDecoder, setHdrMode, etc.) which the GUI thread invokes.
// Concrete renderers protect cross-thread state with their own
// locks (the main thread can mutate config while the render thread
// is mid-frame).

#pragma once

#include <QImage>

#include <vector>

namespace qcv {

class PlayerWindow;
class VideoDecoder;
class ImageSequenceCache;
class OCIOConfigManager;
class ViewportAnnotator;
class SafetyOverlay;
class IDualFrameSource;
struct ActiveStroke;

// Compositor mode (matches old PlayerRhiItem::CompositorMode):
//   Single = one source, full frame
//   SideBySide = two sources, split horizontally at splitPos
//   Wipe       = two sources, hard split with draggable wipe
//   Difference = two sources, per-channel abs(A-B) * diffGain (Adobe-style)
enum class CompositorMode {
    Single = 0,
    SideBySide = 1,
    Wipe = 2,
    Difference = 3,
};

// Mirror of WindowManager::HdrMode. Kept in sync at the seam — we
// cannot include window/window_manager.h here without a circular
// dep, so the integer values must stay aligned.
enum class HdrMode {
    SdrSRgb                 = 0,
    SdrDisplayP3            = 1,
    ExtendedLinearSRgb      = 2,
    ExtendedLinearDisplayP3 = 3,
    Hdr10                   = 4,
};

// Viewport background — old QCView's VideoBackgroundType
// (panel_viewport.cpp:1545-1549). Fills behind the source rect so
// letterbox / pillarbox space has something to look at.
enum class BackgroundMode {
    Black           = 0,
    DarkGray        = 1,   // 27/27/27 — old app DEFAULT
    DarkCheckerboard  = 2,
    LightCheckerboard = 3,
};

// Phase 7.7 — top-level flow selection. Single keeps the existing
// slot-path render. Dual delegates the per-frame draw to a
// DualCompositor pulling from a DualPlaybackController.
//
// Renderer transitions are cold: callers tear down the source-side
// state (decoders, cache) on the way out of one mode before
// switching the other in. See window_manager.cpp::setCompositorMode
// for the orchestration.
enum class RendererMode {
    SingleFlow = 0,
    DualFlow   = 1,
};

class IPlayerRenderer {
public:
    virtual ~IPlayerRenderer() = default;

    // ---- Lifecycle (GUI thread) ----
    // Initialize against the native surface backing `window`. After
    // this returns, the render thread is running (or queued to start
    // on first expose).
    virtual bool init(PlayerWindow *window) = 0;

    // Stop the render thread, release surface resources. Safe to call
    // on a not-yet-init'd renderer (no-ops).
    virtual void shutdown() = 0;

    // Wake the render thread to draw exactly one frame's worth. Called
    // from GUI thread when content changes (cache frame ready, OCIO
    // chain rebuild, transport seek, etc.).
    virtual void requestUpdate() = 0;

    // ---- Resize (GUI thread) ----
    // Called from PlayerWindow::resizeEvent. Concrete renderers
    // re-create their swapchain at the new size.
    virtual void resize(int width, int height) = 0;

    // ---- Configuration setters (GUI thread) ----
    virtual void setHdrMode(HdrMode mode)                  = 0;
    // Phase F.2.8 follow-up — uniform brightness multiplier applied
    // post-OCIO (and post-compositor on the bypass path) before the
    // final pixels reach the swapchain. Default 1.0 = identity;
    // values > 1 lift output linearly, < 1 darken. A plain multiplier
    // can blow out HDR highlights above the display's headroom —
    // user-responsible. Default no-op so platforms that haven't
    // wired this yet still link.
    virtual void setBrightness(float /*brightness*/) {}
    virtual void setImageSeqCache(ImageSequenceCache *c)   = 0;
    virtual void setVideoDecoder(VideoDecoder *d)          = 0;
    // Source B decoder — fed into the compositor's srcB slot for
    // SBS / Wipe modes. Default no-op so platforms (stub) can
    // ignore it; the Metal implementation overrides.
    virtual void setVideoDecoderB(VideoDecoder * /*d*/) {}
    virtual void setOcio(OCIOConfigManager *o)             = 0;
    virtual void setCompositorMode(CompositorMode mode)    = 0;
    virtual void setSplitPos(float pos)                    = 0;
    // Wipe split-seam highlight (0 grey / 1 white on hover/drag).
    // Default no-op so platforms that don't draw a seam can ignore it.
    virtual void setSplitSeamHighlight(float /*h*/) {}
    // Difference-mode amplification gain (1.0 = raw abs(A-B)). Default
    // no-op for platforms without the dual difference shader.
    virtual void setDiffGain(float /*g*/) {}
    // Loading indicator — while true the render thread draws an
    // animated spinner over the viewport (the main thread is blocked
    // opening media/project, so only the independent render thread can
    // animate). Default no-op for platforms without a spinner.
    virtual void setLoadingActive(bool /*on*/) {}
    virtual void setBackgroundMode(BackgroundMode mode)    = 0;

    // Phase 7.5 B.6.5: the renderer queries the active stroke each
    // frame to draw it. WindowManager hands the same annotator to
    // PlayerWindow (for input forwarding) and to the renderer.
    virtual void setViewportAnnotator(ViewportAnnotator *) = 0;

    // Phase 7.5 B.6.6: the renderer queries the safety-overlay each
    // frame for its line mesh.
    virtual void setSafetyOverlay(SafetyOverlay *) = 0;

    // Flush renderer-side state for one side. Called when its
    // media changes (close, swap, etc.) so stale pending
    // FrameHandles / cached MTLTextures don't leak into the next
    // session and trip the atomic-pair gate. Default no-op for
    // platforms (stub) that don't keep per-side state.
    virtual void clearSourceAState() {}
    virtual void clearSourceBState() {}

    // Mismatched-clip-length handling. WindowManager monitors the
    // master playhead vs each side's natural duration and pushes
    // the per-side activity flag. When a side is inactive the
    // compositor renders that region as transparent (background
    // shows through) — distinct from "no source loaded," which
    // the compositor would mirror across into the other side.
    // Default no-op so platforms (stub) without dual-source state
    // can ignore.
    virtual void setSourceActivity(bool /*aActive*/, bool /*bActive*/) {}

    // Phase 7.7 — top-level flow selector. Default SingleFlow keeps
    // existing behavior unchanged; DualFlow routes drawFrame to the
    // dual-island compositor. Default no-op for stub renderers.
    virtual void setRendererMode(RendererMode /*m*/) {}

    // Phase 7.7 — opaque pointer to the dual::DualPlaybackController
    // the dual flow pulls from. void* keeps qcv_render's interface
    // clean of qcv_dual types; the Metal impl reinterpret_casts.
    // Pass nullptr to detach. Default no-op.
    virtual void setDualController(void * /*controllerPtr*/) {}

    // Phase F.2.11.b — Windows DualFlow seam. The renderer pulls
    // per-side frames through this typed interface instead of
    // reinterpret-casting the void* (which on Windows would require
    // qcv_render to link qcv_dual and close the qmltypes-generation
    // cycle). qcv_window provides the concrete adapter wrapping
    // dual::DualPlaybackController; the D3D11 renderer reads its
    // virtual methods at render-thread tick time. macOS Metal uses
    // setDualController(void*) above and ignores this; default
    // no-op so platforms that haven't migrated still link.
    virtual void setDualFrameSource(IDualFrameSource * /*source*/) {}

    // Capture a snapshot of the next presented frame. Blocks the
    // calling thread until the render thread services the request
    // (typically one vsync, ~16 ms; bounded by an internal timeout).
    // Returns an empty QImage if capture isn't supported (e.g.,
    // stub renderer) or the timeout elapsed. SDR formats only for
    // first pass — HDR (RGBA16Float) drawables clamp to 8-bit
    // sRGB; a tonemap pass is a follow-up.
    virtual QImage captureScreenshot() { return QImage(); }

    // Phase 3.H.5 — hover-thumbnail handle from
    // TimelineThumbnailCache (uint64_t handle into
    // MetalTexturePool::thumbnailInstance()). Renderer composites
    // into the bottom-left corner of the canvas, INSIDE the OCIO
    // pass so the thumb shares the main image's color treatment.
    // Pass 0 to clear. Default no-op for stub renderers.
    virtual void setHoverThumbnail(unsigned long long /*handle*/,
                                    int /*srcW*/, int /*srcH*/) {}
    // Phase 3.H.5 dual-view extension — B-side hover thumb (bottom-
    // right corner). Only set when the dual compositor is active;
    // single + playlist modes pass 0 here.
    virtual void setHoverThumbnailB(unsigned long long /*handle*/,
                                     int /*srcW*/, int /*srcH*/) {}

    // Phase 3.H.6 — list of stored strokes to draw on top of the
    // canvas (in addition to the in-flight live stroke from the
    // ViewportAnnotator). WindowManager rebuilds the list on
    // frame change + on note add/edit/delete; the renderer
    // tessellates each per draw using current dst size. Pass an
    // empty vector to clear. Default no-op for stub renderers.
    virtual void setStoredAnnotationStrokes(
        const std::vector<ActiveStroke> & /*strokes*/) {}
};

} // namespace qcv
