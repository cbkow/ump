#pragma once

#ifdef _WIN32

#include <d3d11_1.h>
#include <wrl/client.h>
#include <memory>

// Forward declarations for FFmpeg types
struct AVFrame;
struct AVBufferRef;

namespace qcview {

//=============================================================================
// ExtractedHWFrame
//
// Contains D3D11 resources extracted from an FFmpeg hardware-decoded frame.
// Includes the texture and shader resource views for Y and UV planes.
//=============================================================================

struct ExtractedHWFrame {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_y;   // Y plane (luma)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_uv;  // UV plane (chroma, subsampled)
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // DXGI_FORMAT_NV12 or DXGI_FORMAT_P010
    int array_index = 0;     // For texture arrays (FFmpeg D3D11VA uses arrays)
    bool is_hdr = false;     // P010 = true (10-bit HDR content)
    int bit_depth = 8;       // 8 for NV12, 10 for P010
};

//=============================================================================
// D3D11HWFrameExtractor
//
// Extracts ID3D11Texture2D from FFmpeg AVFrame (D3D11VA hardware decode output)
// and creates shader resource views for the Y and UV planes.
//
// FFmpeg D3D11VA uses texture arrays:
// - AVFrame->data[0] = ID3D11Texture2D pointer
// - AVFrame->data[1] = array index (cast from intptr_t)
//
// The SRVs use D3D11_SRV_DIMENSION_TEXTURE2DARRAY to access the specific slice.
//=============================================================================

class D3D11HWFrameExtractor {
public:
    D3D11HWFrameExtractor();
    ~D3D11HWFrameExtractor();

    // Delete copy/move
    D3D11HWFrameExtractor(const D3D11HWFrameExtractor&) = delete;
    D3D11HWFrameExtractor& operator=(const D3D11HWFrameExtractor&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    // Initialize with D3D11 device
    bool Initialize(ID3D11Device* device);

    // Shutdown and release resources
    void Shutdown();

    // Check if initialized
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Frame Extraction
    //=========================================================================

    // Extract D3D11 texture and create SRVs from AVFrame
    // Returns nullptr if frame is not a D3D11VA hardware frame
    std::unique_ptr<ExtractedHWFrame> Extract(AVFrame* frame);

    // Check if an AVFrame is a D3D11VA hardware frame
    static bool IsD3D11VAFrame(AVFrame* frame);

    // Get the D3D11 texture from an AVFrame (no SRV creation)
    static ID3D11Texture2D* GetTextureFromFrame(AVFrame* frame, int* out_array_index = nullptr);

private:
    //=========================================================================
    // SRV Creation
    //=========================================================================

    // Create Y and UV plane SRVs for NV12/P010 texture
    bool CreatePlaneSRVs(ID3D11Texture2D* texture,
                         DXGI_FORMAT format,
                         int array_index,
                         ID3D11ShaderResourceView** srv_y,
                         ID3D11ShaderResourceView** srv_uv);

    // Get the appropriate SRV format for Y plane based on texture format
    DXGI_FORMAT GetYPlaneFormat(DXGI_FORMAT texture_format) const;

    // Get the appropriate SRV format for UV plane based on texture format
    DXGI_FORMAT GetUVPlaneFormat(DXGI_FORMAT texture_format) const;

    //=========================================================================
    // State
    //=========================================================================

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    bool initialized_ = false;

    // GPU staging texture for zero-copy extraction
    // D3D11VA decoder textures don't have BIND_SHADER_RESOURCE, so we copy
    // to this texture which does. The copy is GPU-to-GPU (no CPU involvement).
    // We keep the same NV12/P010 format and create plane SRVs from it.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> gpu_staging_texture_;
    UINT staging_width_ = 0;
    UINT staging_height_ = 0;
    DXGI_FORMAT staging_format_ = DXGI_FORMAT_UNKNOWN;
};

} // namespace qcview

#endif // _WIN32
