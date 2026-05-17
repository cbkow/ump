#include "window_manager_dual_source_adapter.h"

#include "dual/dual_playback_controller.h"
#include "dual/dual_playback_timer.h"
#include "dual/i_dual_source.h"

#include <QImage>

namespace qcv {

namespace {

DualFramePayload payloadFromDualFrame(
    const std::shared_ptr<qcv::dual::DualFrame> &frame)
{
    DualFramePayload out;
    if (!frame || !frame->valid()) return out;

    switch (frame->kind) {
    case qcv::dual::DualFrame::Kind::Cpu: {
        const QImage *img = frame->rgba.get();
        if (!img || img->isNull()) return out;
        const bool isFp16 = (img->format() == QImage::Format_RGBA16FPx4);
        out.kind = isFp16 ? DualFramePayload::Kind::CpuRgba16F
                          : DualFramePayload::Kind::CpuRgba8;
        out.width     = img->width();
        out.height    = img->height();
        out.cpuBits   = img->constBits();
        out.cpuStride = static_cast<int>(img->bytesPerLine());
        // Anchor lifetime to the DualFrame — keeps the QImage alive
        // until the renderer drops the payload.
        out.keepAlive = std::static_pointer_cast<void>(frame);
        break;
    }
    case qcv::dual::DualFrame::Kind::Metal:
        // macOS-only DualFrame kind. The adapter is the Windows
        // path; if a Metal-kind frame ever reaches us it means
        // DualVideoDecoder is producing zero-copy CVPixelBuffers
        // on a platform that doesn't have the bridge — treat as
        // empty, the renderer falls back to last-good-frame hold.
        break;
    case qcv::dual::DualFrame::Kind::Vulkan: {
        // F.2.12.d — pass the AVFrame* opaquely. The renderer's
        // D3D11VulkanDecodeBridge consumes it via FrameHandle::Vulkan
        // (the exact same import path single-flow uses). keepAlive
        // anchors the DualFrame so the av_frame_free deleter doesn't
        // fire until the bridge has copied the bits.
        void *avFrame = frame->avFrame.get();
        if (!avFrame) return out;
        out.kind          = DualFramePayload::Kind::VulkanShared;
        out.width         = frame->width;
        out.height        = frame->height;
        out.avFrameOwning = avFrame;
        out.keepAlive     = std::static_pointer_cast<void>(frame);
        break;
    }
    case qcv::dual::DualFrame::Kind::Empty:
        break;
    }
    return out;
}

} // namespace

WindowManagerDualSourceAdapter::WindowManagerDualSourceAdapter(
    qcv::dual::DualPlaybackController *controller)
    : m_controller(controller)
{
}

int WindowManagerDualSourceAdapter::currentFrame()
{
    return m_controller ? m_controller->currentFrame() : 0;
}

int WindowManagerDualSourceAdapter::timelineGeneration()
{
    return m_controller ? m_controller->timelineGeneration() : 0;
}

bool WindowManagerDualSourceAdapter::aPastEnd(int master)
{
    return m_controller ? m_controller->aPastEnd(master) : true;
}

bool WindowManagerDualSourceAdapter::bPastEnd(int master)
{
    return m_controller ? m_controller->bPastEnd(master) : true;
}

DualFramePayload WindowManagerDualSourceAdapter::pullA(int master)
{
    if (!m_controller) return {};
    DualFramePayload p = payloadFromDualFrame(m_controller->pullFrameA(master));
    // Stuff the per-side videoRangeOverride into the payload so the
    // compositor's bridge call honors the Inspector pill on the A
    // side's source MediaItem. Mirrors single-flow's read of
    // VideoDecoder::rangeOverride() inside D3D11PlayerRenderer.
    p.rangeOverride = m_controller->rangeOverrideA();
    return p;
}

DualFramePayload WindowManagerDualSourceAdapter::pullB(int master)
{
    if (!m_controller) return {};
    DualFramePayload p = payloadFromDualFrame(m_controller->pullFrameB(master));
    p.rangeOverride = m_controller->rangeOverrideB();
    return p;
}

bool WindowManagerDualSourceAdapter::loopRange(int *outLoIn,
                                                 int *outLoOut) const
{
    if (!m_controller) return false;
    auto *t = m_controller->timer();
    if (!t || !t->hasLoopRange()) return false;
    if (outLoIn)  *outLoIn  = t->loopIn();
    if (outLoOut) *outLoOut = t->loopOut();
    return true;
}

int WindowManagerDualSourceAdapter::sourceWidth(char side) const
{
    if (!m_controller) return 0;
    auto *s = (side == 'A') ? m_controller->sourceA()
                            : m_controller->sourceB();
    return s ? s->width() : 0;
}

int WindowManagerDualSourceAdapter::sourceHeight(char side) const
{
    if (!m_controller) return 0;
    auto *s = (side == 'A') ? m_controller->sourceA()
                            : m_controller->sourceB();
    return s ? s->height() : 0;
}

} // namespace qcv
