#ifdef _WIN32

#include "timeline_interop_pipeline.h"
#include "d3d11_video_interop.h"
#include "d3d11_yuv_renderer.h"
#include "../utils/debug_utils.h"

namespace qcview {

//=============================================================================
// Constructor/Destructor
//=============================================================================

TimelineInteropPipeline::TimelineInteropPipeline() = default;

TimelineInteropPipeline::~TimelineInteropPipeline() {
    Shutdown();
}

//=============================================================================
// Lifecycle
//=============================================================================

bool TimelineInteropPipeline::Initialize(ID3D11Device* device) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_) {
        return true;
    }

    if (!device) {
        Debug::Log("TimelineInteropPipeline: Null device");
        return false;
    }

    device_ = device;
    device_->GetImmediateContext(&context_);

    // Create YUV renderer
    yuv_renderer_ = std::make_unique<D3D11YUVRenderer>();
    if (!yuv_renderer_->Initialize(device)) {
        Debug::Log("TimelineInteropPipeline: Failed to initialize YUV renderer");
        Shutdown();
        return false;
    }

    // Create interop (texture will be created on first use when we know dimensions)
    interop_ = std::make_unique<D3D11VideoInterop>();
    if (!interop_->Initialize(device)) {
        Debug::Log("TimelineInteropPipeline: Failed to initialize interop");
        Shutdown();
        return false;
    }

    initialized_ = true;
    Debug::Log("TimelineInteropPipeline: Initialized, interop=" +
               std::string(interop_->GetInteropMethodName()));

    return true;
}

void TimelineInteropPipeline::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) return;

    Debug::Log("TimelineInteropPipeline: Shutting down...");

    // Reset cached texture references BEFORE destroying interop
    // This prevents dangling references after re-initialization
    last_gl_texture_ = 0;
    last_timeline_frame_ = -1;

    if (interop_) {
        interop_->Shutdown();
        interop_.reset();
    }

    if (yuv_renderer_) {
        yuv_renderer_->Shutdown();
        yuv_renderer_.reset();
    }

    context_.Reset();
    device_.Reset();

    width_ = 0;
    height_ = 0;
    initialized_ = false;
}

//=============================================================================
// Main API
//=============================================================================

GLuint TimelineInteropPipeline::RenderYUVToGL(int timeline_frame, const YUVTextures& yuv) {
    if (!initialized_ || !yuv.IsValid()) {
        return 0;
    }

    // Check if same frame - return cached texture
    if (timeline_frame >= 0 && timeline_frame == last_timeline_frame_ && last_gl_texture_ != 0) {
        return last_gl_texture_;
    }

    // Ensure interop texture exists and matches dimensions
    if (!EnsureInteropTexture(yuv.width, yuv.height)) {
        return 0;
    }

    // Lock interop for D3D11 rendering
    if (!interop_->LockForD3D11()) {
        Debug::Log("TimelineInteropPipeline: Failed to lock for D3D11");
        return 0;
    }

    // Render YUV directly to interop texture
    YUVRenderParams params;
    params.width = yuv.width;
    params.height = yuv.height;
    params.bit_depth = yuv.bit_depth;
    params.is_hdr = yuv.is_hdr;
    params.is_full_range = yuv.is_full_range;
    params.color_space = yuv.is_bt2020 ? YUVColorSpace::BT_2020 : YUVColorSpace::BT_709;
    params.plane_count = yuv.plane_count > 0 ? yuv.plane_count : (yuv.is_nv12 ? 2 : 3);
    params.has_alpha = yuv.has_alpha;
    params.is_rgb_planar = yuv.is_rgb_planar;

    // Get the current buffer's RTV from interop (tracks triple-buffer rotation)
    // CRITICAL: Do NOT use our own interop_rtv_ - it only points to ONE buffer
    // but the interop rotates through THREE buffers. Using GetRTV() ensures we
    // render to the same buffer that GetGLTexture() will return.
    ID3D11RenderTargetView* current_rtv = interop_->GetRTV();
    if (!current_rtv) {
        Debug::Log("TimelineInteropPipeline: Interop RTV is null");
        interop_->UnlockForGL();
        return 0;
    }

    bool render_ok = false;

    if (yuv.is_nv12) {
        // 2-plane NV12/P010 path
        render_ok = yuv_renderer_->Render(yuv.y_srv, yuv.uv_srv,
                                           current_rtv, params);
    } else if (yuv.plane_count == 4 && yuv.has_alpha) {
        // 4-plane YUVA/GBRAP path
        render_ok = yuv_renderer_->Render(yuv.plane_srvs[0], yuv.plane_srvs[1],
                                           yuv.plane_srvs[2], yuv.plane_srvs[3],
                                           current_rtv, params);
    } else if (yuv.plane_count >= 3) {
        // 3-plane planar YUV path
        render_ok = yuv_renderer_->Render(yuv.plane_srvs[0], yuv.plane_srvs[1],
                                           yuv.plane_srvs[2], current_rtv, params);
    } else {
        // Fallback: assume NV12-like with plane_srvs
        render_ok = yuv_renderer_->Render(yuv.plane_srvs[0], yuv.plane_srvs[1],
                                           current_rtv, params);
    }

    if (!render_ok) {
        interop_->UnlockForGL();
        return 0;
    }

    // Unlock for GL access
    // Note: UnlockForGL() sets gl_texture_ to the buffer we just rendered to
    // and advances write_index_ internally - no need to call AdvanceBuffers()
    if (!interop_->UnlockForGL()) {
        Debug::Log("TimelineInteropPipeline: Failed to unlock for GL");
        return 0;
    }

    // Ensure GL texture upload is complete (important for CPU fallback path)
    // This prevents flickering from async texture uploads
    if (!interop_->HasZeroCopyInterop()) {
        glFinish();
    }

    // Cache the result - GetGLTexture() returns the buffer we just rendered to
    last_timeline_frame_ = timeline_frame;
    last_gl_texture_ = interop_->GetGLTexture();

    return last_gl_texture_;
}

GLuint TimelineInteropPipeline::GetCachedTexture(int timeline_frame) const {
    if (timeline_frame >= 0 && timeline_frame == last_timeline_frame_ && last_gl_texture_ != 0) {
        return last_gl_texture_;
    }
    return 0;
}

//=============================================================================
// Info
//=============================================================================

bool TimelineInteropPipeline::HasZeroCopyInterop() const {
    return interop_ && interop_->HasZeroCopyInterop();
}

const char* TimelineInteropPipeline::GetInteropMethodName() const {
    return interop_ ? interop_->GetInteropMethodName() : "none";
}

//=============================================================================
// Internal
//=============================================================================

bool TimelineInteropPipeline::EnsureInteropTexture(int width, int height) {
    if (!interop_) return false;

    // Check if we need to (re)create the texture
    if (width_ == width && height_ == height && interop_->GetRTV()) {
        return true;  // Already have correct size
    }

    // Destroy old texture
    interop_->DestroySharedTexture();

    // Create new interop texture (includes triple-buffered RTVs internally)
    if (!interop_->CreateSharedTexture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
        Debug::Log("TimelineInteropPipeline: Failed to create interop texture " +
                   std::to_string(width) + "x" + std::to_string(height));
        return false;
    }

    // Verify interop has valid RTV
    if (!interop_->GetRTV()) {
        Debug::Log("TimelineInteropPipeline: Interop RTV is null after texture creation");
        return false;
    }

    width_ = width;
    height_ = height;

    Debug::Log("TimelineInteropPipeline: Created interop texture " +
               std::to_string(width) + "x" + std::to_string(height));

    return true;
}

} // namespace qcview

#endif // _WIN32
