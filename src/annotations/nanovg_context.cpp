// NanoVG requires OpenGL — disabled on Vulkan/Metal (annotation rendering uses tessellation path)
#if !defined(QCVIEW_USE_VULKAN) && !defined(QCVIEW_USE_METAL)

#include "nanovg_context.h"

// Include OpenGL headers before nanovg_gl.h
#include <glad/gl.h>

// NanoVG GL3 implementation (NANOVG_GL3_IMPLEMENTATION is defined in CMakeLists.txt)
#include <nanovg_gl.h>

namespace qcview {
namespace Annotations {

NanoVGContext& NanoVGContext::Instance() {
    static NanoVGContext instance;
    return instance;
}

NanoVGContext::~NanoVGContext() {
    Shutdown();
}

bool NanoVGContext::Initialize() {
    if (vg_ != nullptr) {
        // Already initialized
        return true;
    }

    // Create NanoVG context with anti-aliasing and stencil strokes
    // NVG_ANTIALIAS: Use anti-aliased rendering
    // NVG_STENCIL_STROKES: Use stencil buffer for better stroke quality
    vg_ = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);

    if (vg_ == nullptr) {
        return false;
    }

    return true;
}

void NanoVGContext::Shutdown() {
    if (vg_ != nullptr) {
        nvgDeleteGL3(vg_);
        vg_ = nullptr;
    }
}

void NanoVGContext::BeginFrame(float width, float height, float devicePixelRatio) {
    if (vg_ != nullptr) {
        // Save OpenGL state that NanoVG will modify
        glGetIntegerv(GL_BLEND, &saved_blend_);
        glGetIntegerv(GL_BLEND_SRC_RGB, &saved_blend_src_rgb_);
        glGetIntegerv(GL_BLEND_DST_RGB, &saved_blend_dst_rgb_);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &saved_blend_src_alpha_);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &saved_blend_dst_alpha_);
        glGetIntegerv(GL_CULL_FACE, &saved_cull_face_);
        glGetIntegerv(GL_DEPTH_TEST, &saved_depth_test_);
        glGetIntegerv(GL_SCISSOR_TEST, &saved_scissor_test_);
        glGetIntegerv(GL_STENCIL_TEST, &saved_stencil_test_);
        glGetIntegerv(GL_COLOR_WRITEMASK, saved_color_mask_);

        nvgBeginFrame(vg_, width, height, devicePixelRatio);
    }
}

void NanoVGContext::EndFrame() {
    if (vg_ != nullptr) {
        nvgEndFrame(vg_);

        // Restore OpenGL state
        if (saved_blend_) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        glBlendFuncSeparate(saved_blend_src_rgb_, saved_blend_dst_rgb_,
                           saved_blend_src_alpha_, saved_blend_dst_alpha_);
        if (saved_cull_face_) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        if (saved_depth_test_) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        if (saved_scissor_test_) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
        if (saved_stencil_test_) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
        glColorMask(saved_color_mask_[0], saved_color_mask_[1],
                   saved_color_mask_[2], saved_color_mask_[3]);
    }
}

} // namespace Annotations
} // namespace qcview

#else // QCVIEW_USE_VULKAN || QCVIEW_USE_METAL

// Provide stub implementations when NanoVG is not available (Vulkan/Metal backends)
#include "nanovg_context.h"

namespace qcview {
namespace Annotations {

NanoVGContext& NanoVGContext::Instance() {
    static NanoVGContext instance;
    return instance;
}

NanoVGContext::~NanoVGContext() {}
bool NanoVGContext::Initialize() { return false; }
void NanoVGContext::Shutdown() {}
void NanoVGContext::BeginFrame(float, float, float) {}
void NanoVGContext::EndFrame() {}

} // namespace Annotations
} // namespace qcview

#endif // !QCVIEW_USE_VULKAN && !QCVIEW_USE_METAL
