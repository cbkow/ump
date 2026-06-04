// MetalPlayerRenderer — Phase 7.5 B.2 / Guide 19 §2.1.
//
// Native macOS player renderer. Drives a present loop on a dedicated
// thread; vsync pacing comes from CAMetalLayer's drawable backpressure
// (presentDrawable: blocks waiting for the GPU when
// maximumDrawableCount drawables are already in flight).
//
// Phase B.2 scope: animated clear color cycle — proves the loop runs.
// Subsequent sub-phases layer compositor, OCIO, stroke, safety passes
// in front of presentDrawable:.
//
// Threading:
//   GUI thread:     init / shutdown / resize / setters (the IPlayerRenderer
//                   methods PlayerWindow calls)
//   Render thread:  the present loop. Owns drawable acquisition, render
//                   command encoding, presentDrawable:, commit.
//
// Cross-thread state is the layer pointer + the `m_stopRequested` flag.
// Layer is set once at init and not mutated after; flag is std::atomic.

#pragma once

#include "iplayer_renderer.h"

#include "annotations/active_stroke.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class QWindow;

namespace qcv {

class MetalPlayerRenderer final : public IPlayerRenderer {
public:
    MetalPlayerRenderer();
    ~MetalPlayerRenderer() override;

    MetalPlayerRenderer(const MetalPlayerRenderer &)            = delete;
    MetalPlayerRenderer &operator=(const MetalPlayerRenderer &) = delete;

    // ---- IPlayerRenderer ----
    bool init(PlayerWindow *window) override;
    void shutdown() override;
    void requestUpdate() override;
    void resize(int width, int height) override;

    void setHdrMode(HdrMode mode) override;
    void setBrightness(float brightness) override;
    void setImageSeqCache(ImageSequenceCache *c) override;
    void setVideoDecoder(VideoDecoder *d) override;
    void setVideoDecoderB(VideoDecoder *d) override;
    void clearSourceAState() override;
    void clearSourceBState() override;
    void setSourceActivity(bool aActive, bool bActive) override;
    void setPixelAspectA(int num, int den) override;
    void setPixelAspectB(int num, int den) override;
    void setOcio(OCIOConfigManager *o) override;
    void setCompositorMode(CompositorMode mode) override;
    void setSplitPos(float pos) override;
    void setSplitSeamHighlight(float h) override;
    void setDiffGain(float g) override;
    void setLoadingActive(bool on) override;
    void setViewportNotice(const QImage &card) override;
    void setBackgroundMode(BackgroundMode mode) override;
    void setViewportAnnotator(ViewportAnnotator *a) override;
    void setSafetyOverlay(SafetyOverlay *s) override;
    void setRendererMode(RendererMode m) override;
    void setDualController(void *controllerPtr) override;
    void setHoverThumbnail(unsigned long long handle,
                            int srcW, int srcH) override;
    void setHoverThumbnailB(unsigned long long handle,
                             int srcW, int srcH) override;
    void setStoredAnnotationStrokes(
        const std::vector<ActiveStroke> &strokes) override;
    QImage captureScreenshot() override;

private:
    void renderThreadProc();
    void drawFrame();

    struct Impl;
    std::unique_ptr<Impl>     m_impl;
    std::thread               m_renderThread;
    std::atomic<bool>         m_stopRequested{false};
    std::atomic<bool>         m_running{false};

    HdrMode             m_hdrMode   = HdrMode::SdrSRgb;
    // Per-side activity flags — see IPlayerRenderer::setSourceActivity.
    // Atomic because GUI thread sets them (WindowManager push on
    // master-clock cross over each side's natural duration) and
    // the render thread reads them in drawFrame.
    std::atomic<bool>   m_aActive{true};
    std::atomic<bool>   m_bActive{true};
    // Per-side pixel aspect (anamorphic un-squeeze). Packed num/den as
    // two atomics each; GUI thread sets them via setPixelAspect*, the
    // render thread reads them in drawFrame to widen the effective
    // source dimensions. Default 1/1 = square (unchanged behavior).
    std::atomic<int>    m_parNumA{1};
    std::atomic<int>    m_parDenA{1};
    std::atomic<int>    m_parNumB{1};
    std::atomic<int>    m_parDenB{1};
    ImageSequenceCache *m_cache     = nullptr;
    VideoDecoder       *m_decoderB  = nullptr;
    // Tracks the cache the upload thread is bound to + its last-seen
    // generation. Mismatch on either drives a clearAll on the upload
    // side so we don't flash stale GPU textures when:
    //   - the user switches sequences (cache pointer changes), or
    //   - the user big-seeks / changes EXR layer (gen bumps).
    ImageSequenceCache *m_cacheBoundToUpload = nullptr;
    int                 m_lastCacheGen       = 0;
    VideoDecoder       *m_decoder   = nullptr;
    OCIOConfigManager  *m_ocio      = nullptr;
    CompositorMode      m_compMode  = CompositorMode::Single;
    float               m_splitPos  = 0.5f;
    BackgroundMode      m_bgMode    = BackgroundMode::Black;
    ViewportAnnotator  *m_annotator = nullptr;
    SafetyOverlay      *m_safety    = nullptr;

    // Phase 7.7 — top-level flow flag + opaque pointer to the
    // DualPlaybackController. Atomic so GUI thread can flip mode
    // while render thread is mid-frame.
    std::atomic<bool>   m_loadingActive{false};
    std::atomic<int>    m_rendererMode{static_cast<int>(RendererMode::SingleFlow)};
    std::atomic<void *> m_dualControllerPtr{nullptr};

    // Viewport notice (ARRIRAW / unsupported-media message). The GUI
    // thread stashes a pre-rendered RGBA8888 card here via
    // setViewportNotice(); the render thread uploads it to a
    // thumbnail-pool texture lazily (m_noticeDirty) and composites it
    // centered over the background in the same overlay pass as the
    // spinner. Empty pixels / m_noticeActive=false clears it.
    std::mutex           m_noticeMutex;
    std::vector<uint8_t> m_noticePixels;       // RGBA8888; empty = none
    int                  m_noticeW = 0;
    int                  m_noticeH = 0;
    bool                 m_noticeDirty = false; // re-upload pending
    std::atomic<bool>    m_noticeActive{false};

    // Phase 3.H.5 — hover-thumbnail handle. WindowManager pushes
    // the latest TimelineThumbnailCache handle here when the user
    // hovers the timeline; render thread reads it each frame and
    // composites the thumbnail into a corner ROI of compositeRaw
    // (so it gets OCIO with the main image). 0 = no overlay.
    std::atomic<unsigned long long> m_hoverThumbHandle{0};
    std::atomic<int>                m_hoverThumbW{0};
    std::atomic<int>                m_hoverThumbH{0};
    std::atomic<unsigned long long> m_hoverThumbHandleB{0};
    std::atomic<int>                m_hoverThumbWB{0};
    std::atomic<int>                m_hoverThumbHB{0};

    // Phase 3.H.6 — stored strokes for the current frame. GUI thread
    // pushes via setStoredAnnotationStrokes; render thread copies
    // under m_storedStrokesMutex once per draw and tessellates each.
    std::mutex                      m_storedStrokesMutex;
    std::vector<ActiveStroke>       m_storedStrokes;
};

} // namespace qcv
