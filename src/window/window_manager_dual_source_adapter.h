// WindowManagerDualSourceAdapter — Phase F.2.11.b concrete adapter.
//
// Bridges qcv::IDualFrameSource (qcv_render's pure interface) with
// qcv::dual::DualPlaybackController (qcv_dual). Lives in qcv_window
// because qcv_window already links both libraries; qcv_render
// cannot link qcv_dual on non-Apple due to the qmltypes-generation
// cycle (see src/render/CMakeLists.txt:237).
//
// WindowManager owns one of these per active dual session: created
// alongside DualPlaybackController in setCompositorMode's
// Single→Dual branch, destroyed on Dual→Single. The renderer holds
// a non-owning IDualFrameSource* across the session's lifetime.

#pragma once

#include "render/i_dual_frame_source.h"

namespace qcv::dual { class DualPlaybackController; }

namespace qcv {

class WindowManagerDualSourceAdapter final : public IDualFrameSource {
public:
    explicit WindowManagerDualSourceAdapter(
        qcv::dual::DualPlaybackController *controller);
    ~WindowManagerDualSourceAdapter() override = default;

    WindowManagerDualSourceAdapter(
        const WindowManagerDualSourceAdapter &) = delete;
    WindowManagerDualSourceAdapter &operator=(
        const WindowManagerDualSourceAdapter &) = delete;

    // ---- IDualFrameSource ----
    int  currentFrame() override;
    int  timelineGeneration() override;
    bool isScrubbing() override;
    bool aPastEnd(int master) override;
    bool bPastEnd(int master) override;
    DualFramePayload pullA(int master) override;
    DualFramePayload pullB(int master) override;
    bool loopRange(int *outLoIn, int *outLoOut) const override;
    int  sourceWidth(char side)  const override;
    int  sourceHeight(char side) const override;

private:
    qcv::dual::DualPlaybackController *m_controller = nullptr;
};

} // namespace qcv
