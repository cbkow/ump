#pragma once

#ifdef _WIN32

#include <d3d11_1.h>
#include <wrl/client.h>
#include <glad/gl.h>
#include <mutex>
#include <memory>

#include "yuv_textures.h"

namespace qcview {

// Forward declarations
class D3D11VideoInterop;
class D3D11YUVRenderer;

//=============================================================================
// TimelineInteropPipeline
//
// Simplified pipeline for DUAL_VIEW timeline mode:
// - Single shared D3D11→GL interop for all clips
// - Direct YUV→RGB render to interop texture (no intermediate buffer)
// - Fast path: YUV SRVs → render → GL texture
//
// Usage:
//   YUVTextures yuv = decoder->GetYUVTextures(frame);
//   GLuint tex = pipeline->RenderYUVToGL(yuv);
//
// Thread Safety:
// - RenderYUVToGL() must be called from render thread only (D3D11 not thread-safe)
//=============================================================================

class TimelineInteropPipeline {
public:
    TimelineInteropPipeline();
    ~TimelineInteropPipeline();

    // Non-copyable
    TimelineInteropPipeline(const TimelineInteropPipeline&) = delete;
    TimelineInteropPipeline& operator=(const TimelineInteropPipeline&) = delete;

    //=========================================================================
    // Lifecycle
    //=========================================================================

    bool Initialize(ID3D11Device* device);
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Main API - Single fast path
    //=========================================================================

    // Render YUV textures directly to interop and return GL texture
    // timeline_frame is used for caching - if same frame requested again, returns cached texture
    // Returns 0 if rendering fails
    GLuint RenderYUVToGL(int timeline_frame, const YUVTextures& yuv);

    // Get cached texture if available (for repeated requests of same frame)
    // Returns 0 if no cached frame or frame doesn't match
    GLuint GetCachedTexture(int timeline_frame) const;

    //=========================================================================
    // Info
    //=========================================================================

    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }
    bool HasZeroCopyInterop() const;
    const char* GetInteropMethodName() const;

private:
    //=========================================================================
    // D3D11 Resources
    //=========================================================================

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

    // YUV→RGB renderer
    std::unique_ptr<D3D11YUVRenderer> yuv_renderer_;

    // D3D11→GL interop (renders directly to this)
    std::unique_ptr<D3D11VideoInterop> interop_;

    // Render target view for interop texture (to render YUV directly to it)
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> interop_rtv_;

    //=========================================================================
    // State
    //=========================================================================

    bool initialized_ = false;
    int width_ = 0;
    int height_ = 0;

    // Last rendered frame caching (avoid re-render for same frame)
    int last_timeline_frame_ = -1;
    GLuint last_gl_texture_ = 0;

    // Mutex for thread safety (minimal - only for state access)
    mutable std::mutex mutex_;

    //=========================================================================
    // Internal
    //=========================================================================

    bool EnsureInteropTexture(int width, int height);
};

} // namespace qcview

#endif // _WIN32
