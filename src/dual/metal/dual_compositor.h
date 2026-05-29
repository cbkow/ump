// DualCompositor — Phase 7.7 Stage 3.
//
// Metal renderer for the dual playback island. Pulls QImage RGBA8
// frames from a DualPlaybackController each render-thread tick,
// uploads to per-side cached MTLTextures, and runs an SBS / Wipe /
// Single fragment shader to produce the composite output.
//
// Architecture mirrors the existing qcv::MetalCompositor (single-
// flow) but is fully isolated:
//   - Own render pipeline + sampler.
//   - Own per-side cached MTLTextures (replaceRegion when dimensions
//     match; recreate when they change).
//   - Own DualPlaybackController pointer; pulls frames each draw.
//
// Past-end-of-shorter-side: handled here. When the controller
// reports `aPastEnd(N)` true, we drop A's texture binding for that
// frame; the shader's aActive=0 branch returns transparent.
//
// Stage 3 scope: CPU-upload of QImage RGBA8 each frame. Stage 3b
// (or 4) optimizes by routing through a frame-keyed pool so the
// upload happens on the source's decode thread, not the render
// thread.
//
// Threading: created + used on the render thread. The compositor
// reads the controller's pull methods (mutex-guarded inside the
// sources). No cross-thread state owned here.

#pragma once

namespace qcv::dual {

class DualPlaybackController;
class IDualPixbufConverter;

class DualCompositor {
public:
    enum Mode : int {
        Single     = 0,
        SideBySide = 1,
        SplitWipe  = 2,
        Difference = 3,
    };

    DualCompositor();
    ~DualCompositor();

    DualCompositor(const DualCompositor &)            = delete;
    DualCompositor &operator=(const DualCompositor &) = delete;

    // `targetPixelFormat` is the drawable's MTLPixelFormat as int.
    // `mtlDevicePtr` is the id<MTLDevice> as void*. We pass it in
    // rather than reaching into qcv::MetalDeviceManager directly so
    // qcv_dual doesn't depend on qcv_render — caller owns the device
    // singleton and bridges.
    bool initialize(int targetPixelFormat, void *mtlDevicePtr);
    void shutdown();
    bool isInitialized() const;

    // The controller owns lifecycle of its sources; we just borrow
    // pointers each draw. Caller MUST clear before the controller
    // dies via setController(nullptr).
    void setController(DualPlaybackController *c);

    // Inject the pixbuf converter (qcv_render-side impl). Required
    // for HW-decoded frames (DualFrame::Kind::Metal); CPU-kind
    // frames render fine without one. Caller owns the converter and
    // MUST clear via setPixbufConverter(nullptr) before destroying
    // it. Set once on dual-mode entry, cleared on exit.
    void setPixbufConverter(IDualPixbufConverter *c) { m_pixbufConverter = c; }

    // Run pre-render YUV→RGB compute for any HW-decoded frames.
    // Caller invokes BEFORE opening the render pass / encoder; the
    // converter encodes compute passes on `cmdBuffer` (passed as
    // void*, an id<MTLCommandBuffer>). Cached RGBA textures are
    // bound by the subsequent renderFrame() call. No-op for
    // CPU-kind frames or when no converter is wired.
    void prepareFrames(void *cmdBuffer);

    // Render parameters — set by the player renderer in response to
    // user toggles. Cheap atomic-style stores.
    void setMode(Mode m)              { m_mode = m; }
    void setSplitPos(float p)         { m_splitPos = p; }
    void setSeamHighlight(float h)    { m_seamHighlight = h; }
    void setDiffGain(float g)         { m_diffGain = g; }   // Difference amplify
    Mode mode() const                 { return m_mode; }
    float splitPos() const            { return m_splitPos; }

    // Per-side safety overlay rendering — Stage 7. Stage 3 stub.
    // void setSafetyA(SafetyOverlay *s) { m_safetyA = s; }
    // void setSafetyB(SafetyOverlay *s) { m_safetyB = s; }

    // Pull current frames from controller, upload, encode the
    // SBS/Wipe/Single pass into `encoderPtr` (active id<MTLRender-
    // CommandEncoder> as void*). dstWidth/dstHeight come from the
    // drawable.
    void renderFrame(void *encoderPtr, int dstWidth, int dstHeight);

    // Encode the cold-transition spinner onto an EXISTING render
    // encoder (the caller's drawable pass). Used by the player
    // renderer to show a loading spinner during a blocking media/
    // project open — reuses this compositor's spinner pipeline +
    // shader. `targetPixelFormat` is the encoder's color-attachment
    // format (MTLPixelFormat as int); the pipeline rebakes if it
    // differs from the last build.
    void encodeLoadingSpinner(void *encoderPtr, int dstWidth, int dstHeight,
                              int targetPixelFormat);

    // Impl is exposed for free-function helpers in the .mm (spinner
    // pipeline + encode). Nothing outside dual_compositor.mm
    // references it.
    struct Impl;

private:
    Impl *m_impl = nullptr;

    DualPlaybackController *m_controller       = nullptr;
    IDualPixbufConverter   *m_pixbufConverter  = nullptr;
    Mode  m_mode      = Single;
    float m_splitPos  = 0.5f;
    float m_seamHighlight = 0.0f;
    float m_diffGain  = 1.0f;
};

} // namespace qcv::dual
