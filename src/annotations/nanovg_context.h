#pragma once

// NanoVG GL3 implementation - must be defined before including nanovg_gl.h
// Note: NANOVG_GL3_IMPLEMENTATION is defined in CMakeLists.txt for the nanovg library
#include <glad/gl.h>
#include <nanovg.h>

namespace ump {
namespace Annotations {

/**
 * Singleton manager for NanoVG context.
 * Ensures a single NanoVG context is shared between display and export rendering,
 * providing visual consistency across both paths.
 */
class NanoVGContext {
public:
    /**
     * Get the singleton instance.
     */
    static NanoVGContext& Instance();

    /**
     * Initialize NanoVG context. Must be called after OpenGL context is created.
     * @return true if initialization succeeded
     */
    bool Initialize();

    /**
     * Shutdown and cleanup NanoVG context. Call before OpenGL context is destroyed.
     */
    void Shutdown();

    /**
     * Get the NanoVG context pointer for rendering.
     * @return NVGcontext pointer, or nullptr if not initialized
     */
    NVGcontext* Get() { return vg_; }

    /**
     * Begin a new frame for rendering.
     * @param width Viewport width in pixels
     * @param height Viewport height in pixels
     * @param devicePixelRatio Device pixel ratio for HiDPI displays (default 1.0)
     */
    void BeginFrame(float width, float height, float devicePixelRatio = 1.0f);

    /**
     * End the current frame and flush rendering.
     */
    void EndFrame();

    /**
     * Check if NanoVG context is initialized.
     */
    bool IsInitialized() const { return vg_ != nullptr; }

private:
    NanoVGContext() = default;
    ~NanoVGContext();

    // Non-copyable
    NanoVGContext(const NanoVGContext&) = delete;
    NanoVGContext& operator=(const NanoVGContext&) = delete;

    NVGcontext* vg_ = nullptr;

    // Saved OpenGL state (restored after NanoVG rendering)
    GLint saved_blend_ = 0;
    GLint saved_blend_src_rgb_ = 0;
    GLint saved_blend_dst_rgb_ = 0;
    GLint saved_blend_src_alpha_ = 0;
    GLint saved_blend_dst_alpha_ = 0;
    GLint saved_cull_face_ = 0;
    GLint saved_depth_test_ = 0;
    GLint saved_scissor_test_ = 0;
    GLint saved_stencil_test_ = 0;
    GLint saved_color_mask_[4] = {0, 0, 0, 0};
};

} // namespace Annotations
} // namespace ump
