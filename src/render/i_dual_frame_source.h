// IDualFrameSource — Phase F.2.11.b seam.
//
// Pure-virtual frame-pair source consumed by the D3D11 dual flow.
// Defined in qcv_render so the renderer can call its methods without
// linking qcv_dual (which would close the qmltypes-generation cycle
// qcv_dual → qcv_timeline → qcv_render on Windows; see
// src/render/CMakeLists.txt:237 for the gate). The concrete adapter
// lives in qcv_window, where qcv_dual is already linked
// (src/window/CMakeLists.txt:46).
//
// On macOS the equivalent role is filled by the existing
// `IPlayerRenderer::setDualController(void *)` seam — Metal's renderer
// `reinterpret_cast`s to `dual::DualPlaybackController *` directly
// because qcv_render PUBLIC-links qcv_dual on Apple. Don't change the
// Metal path; this interface is the Windows alternative.

#pragma once

#include <cstdint>
#include <memory>

namespace qcv {

// One side's frame at a given master frame, as the D3D11 compositor
// needs it. Two tiers mirror single-flow's FrameHandle:
//   - Cpu kinds (RGBA8 / RGBA16F): `cpuBits` + `cpuStride` borrowed
//     from the source frame; lifetime tied to `keepAlive`.
//   - VulkanShared: `avFrameOwning` is the AVFrame* whose data[0]
//     holds an AVVkFrame on the shared VkDevice. The compositor feeds
//     it to D3D11VulkanDecodeBridge::consume — same path single-flow
//     uses. Renderer doesn't dereference the pointer; it goes opaque
//     through the bridge.
struct DualFramePayload {
    enum class Kind {
        Empty,         // no frame (past-end, decoder not ready, etc.)
        CpuRgba8,      // QImage::Format_RGBA8888 — SDR sources
        CpuRgba16F,    // QImage::Format_RGBA16FPx4 — EXR sources
        VulkanShared,  // FFmpeg Vulkan hwaccel — AVVkFrame in AVFrame->data[0]
    };

    Kind kind   = Kind::Empty;
    int  width  = 0;
    int  height = 0;

    // Cpu kinds: borrowed pointer into the source frame's pixel
    // buffer. Lifetime tied to `keepAlive`; the renderer must hold
    // `keepAlive` until UpdateSubresource completes. Stride is
    // bytes-per-row (QImage::bytesPerLine() on the source side).
    const uint8_t *cpuBits   = nullptr;
    int            cpuStride = 0;

    // VulkanShared kind: opaque AVFrame* (treated as void* so this
    // header doesn't need <libavutil/frame.h>). Lifetime tied to
    // `keepAlive` (the shared_ptr<DualFrame> the adapter built);
    // the renderer's bridge consumes the AVFrame and drops the
    // payload, which drops the av_frame_free chain.
    void *avFrameOwning = nullptr;

    // Type-erased lifetime anchor. The adapter sets this to whatever
    // shared_ptr keeps the source pixels alive (typically the
    // `shared_ptr<DualFrame>` returned from the controller's pull).
    // The renderer drops it once it has finished uploading.
    std::shared_ptr<void> keepAlive;

    // Per-side videoRangeOverride from the source MediaItem (mirrors
    // the Inspector Range pill: 0 = Auto, 1 = Full, 2 = Limited).
    // The compositor forwards this to D3D11VulkanDecodeBridge::
    // consumeAVFrame so the YUV→RGB compute honors the user's
    // override, same as single-flow does via VideoDecoder::
    // rangeOverride(). Adapter populates per-pull from the dual
    // controller's atomics.
    int rangeOverride = 0;
};

class IDualFrameSource {
public:
    virtual ~IDualFrameSource() = default;

    // Master playhead. Driven by the dual playback timer; the
    // renderer reads it at the top of each render-thread tick so
    // both sides pull from the same frame number (the structural
    // frame-sync gate).
    virtual int currentFrame() = 0;

    // Monotonic counter bumped on every timeline mutation
    // (`TimelineController::timelineChanged`). The compositor uses
    // it to drop cached SRV textures so an edit doesn't display
    // stale pixels — see guide 23 §4.2.
    virtual int timelineGeneration() = 0;

    // True during a live scrub/edit session (beginScrub..endScrub). The
    // compositor reads this to KEEP its last-good per-side cache across
    // the generation bumps that a slip/trim edit fires every drag delta
    // — otherwise it drops the cache each tick and flickers black on a
    // null pull. Fresh scrub frames stream in during a scrub, so holding
    // the last good frame is correct.
    virtual bool isScrubbing() = 0;

    // True when `master` is past the end of side A / B's natural
    // duration. Sides past their end render transparent (background
    // shows through) rather than holding a stale frame.
    virtual bool aPastEnd(int master) = 0;
    virtual bool bPastEnd(int master) = 0;

    // Pull side A / B's frame at `master`. May return Kind::Empty
    // (decoder not ready yet, past-end without cached frame, etc.) —
    // the caller falls back to its last-good-frame hold in that case.
    virtual DualFramePayload pullA(int master) = 0;
    virtual DualFramePayload pullB(int master) = 0;

    // H.5 — active loop range, if any. Used by the compositor to
    // distinguish a loop wrap (large backward master-frame jump
    // from near loOut to near loIn) from a true seek so it can
    // keep the last-good-frame hold across the wrap. Returns
    // false + leaves outputs untouched if no loop range is set.
    virtual bool loopRange(int *outLoIn, int *outLoOut) const = 0;

    // H.9 — per-side source dimensions. Used by the renderer's
    // dual-flow safety / annotation overlays so each side's
    // markers aspect-fit to that side's source AR (mixed-aspect
    // dual produces correct safe-action / safe-title rects per
    // side instead of one canvas-fit overlay). `side` is 'A' or
    // 'B'. Returns 0 when the side has no source loaded.
    virtual int sourceWidth(char side)  const = 0;
    virtual int sourceHeight(char side) const = 0;
};

} // namespace qcv
