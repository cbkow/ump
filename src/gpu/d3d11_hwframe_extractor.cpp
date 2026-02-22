#include "d3d11_hwframe_extractor.h"

#ifdef _WIN32

#include "../utils/debug_utils.h"

// D3D11 headers must be included BEFORE extern "C" block
// because they contain C++ operator overloads
#include <d3d11.h>
#include <d3d11_1.h>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>
}

namespace qcview {

D3D11HWFrameExtractor::D3D11HWFrameExtractor() = default;

D3D11HWFrameExtractor::~D3D11HWFrameExtractor() {
    Shutdown();
}

bool D3D11HWFrameExtractor::Initialize(ID3D11Device* device) {
    if (initialized_) {
        return true;
    }

    if (!device) {
        Debug::Log("D3D11HWFrameExtractor: Device is null");
        return false;
    }

    device_ = device;
    initialized_ = true;

    Debug::Log("D3D11HWFrameExtractor: Initialized");
    return true;
}

void D3D11HWFrameExtractor::Shutdown() {
    device_.Reset();
    initialized_ = false;
}

bool D3D11HWFrameExtractor::IsD3D11VAFrame(AVFrame* frame) {
    if (!frame) return false;

    // Check if this is a hardware frame in D3D11 format
    return frame->format == AV_PIX_FMT_D3D11;
}

ID3D11Texture2D* D3D11HWFrameExtractor::GetTextureFromFrame(AVFrame* frame, int* out_array_index) {
    if (!frame || !IsD3D11VAFrame(frame)) {
        return nullptr;
    }

    // FFmpeg D3D11VA frame layout:
    // data[0] = ID3D11Texture2D*
    // data[1] = array index (intptr_t cast to uint8_t*)
    ID3D11Texture2D* texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);

    if (out_array_index) {
        *out_array_index = static_cast<int>(reinterpret_cast<intptr_t>(frame->data[1]));
    }

    return texture;
}

std::unique_ptr<ExtractedHWFrame> D3D11HWFrameExtractor::Extract(AVFrame* frame) {
    if (!initialized_ || !frame) {
        return nullptr;
    }

    if (!IsD3D11VAFrame(frame)) {
        // Not a D3D11VA frame - this is software decoded
        return nullptr;
    }

    int array_index = 0;
    ID3D11Texture2D* texture = GetTextureFromFrame(frame, &array_index);

    if (!texture) {
        Debug::Log("D3D11HWFrameExtractor: Failed to get texture from frame");
        return nullptr;
    }

    // Get texture description
    D3D11_TEXTURE2D_DESC tex_desc;
    texture->GetDesc(&tex_desc);

    // Create the result structure
    auto result = std::make_unique<ExtractedHWFrame>();
    result->texture = texture;  // AddRef via ComPtr
    result->width = tex_desc.Width;
    result->height = tex_desc.Height;
    result->format = tex_desc.Format;
    result->array_index = array_index;

    // Determine if HDR based on format
    // P010 is 10-bit HDR, NV12 is 8-bit SDR
    switch (tex_desc.Format) {
        case DXGI_FORMAT_P010:
            result->is_hdr = true;
            result->bit_depth = 10;
            break;
        case DXGI_FORMAT_NV12:
            result->is_hdr = false;
            result->bit_depth = 8;
            break;
        case DXGI_FORMAT_P016:
            result->is_hdr = true;
            result->bit_depth = 16;
            break;
        default:
            Debug::Log("D3D11HWFrameExtractor: Unknown texture format: " +
                      std::to_string(tex_desc.Format));
            result->is_hdr = false;
            result->bit_depth = 8;
            break;
    }

    // Create shader resource views for Y and UV planes
    if (!CreatePlaneSRVs(texture, tex_desc.Format, array_index,
                         result->srv_y.GetAddressOf(),
                         result->srv_uv.GetAddressOf())) {
        Debug::Log("D3D11HWFrameExtractor: Failed to create plane SRVs");
        return nullptr;
    }

    return result;
}

DXGI_FORMAT D3D11HWFrameExtractor::GetYPlaneFormat(DXGI_FORMAT texture_format) const {
    switch (texture_format) {
        case DXGI_FORMAT_NV12:
            return DXGI_FORMAT_R8_UNORM;
        case DXGI_FORMAT_P010:
        case DXGI_FORMAT_P016:
            return DXGI_FORMAT_R16_UNORM;
        default:
            return DXGI_FORMAT_R8_UNORM;
    }
}

DXGI_FORMAT D3D11HWFrameExtractor::GetUVPlaneFormat(DXGI_FORMAT texture_format) const {
    switch (texture_format) {
        case DXGI_FORMAT_NV12:
            return DXGI_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_P010:
        case DXGI_FORMAT_P016:
            return DXGI_FORMAT_R16G16_UNORM;
        default:
            return DXGI_FORMAT_R8G8_UNORM;
    }
}

bool D3D11HWFrameExtractor::CreatePlaneSRVs(ID3D11Texture2D* texture,
                                            DXGI_FORMAT format,
                                            int array_index,
                                            ID3D11ShaderResourceView** srv_y,
                                            ID3D11ShaderResourceView** srv_uv) {
    if (!device_ || !texture || !srv_y || !srv_uv) {
        return false;
    }

    D3D11_TEXTURE2D_DESC tex_desc;
    texture->GetDesc(&tex_desc);

    // Get device context for copy operations
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
    device_->GetImmediateContext(&ctx);

    // D3D11VA decoder textures typically don't have D3D11_BIND_SHADER_RESOURCE
    // and NV12/P010 formats require special handling for SRV creation
    // Solution: Copy to a staging texture in a shader-friendly format
    bool needs_copy = !(tex_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
                      format == DXGI_FORMAT_NV12 ||
                      format == DXGI_FORMAT_P010 ||
                      format == DXGI_FORMAT_P016;

    if (needs_copy) {
        //=====================================================================
        // GPU-to-GPU Zero-Copy Path
        //
        // D3D11VA decoder textures don't have BIND_SHADER_RESOURCE, so we
        // can't create SRVs directly. But we CAN create a NV12/P010 texture
        // WITH BIND_SHADER_RESOURCE and do a GPU-only copy.
        //
        // D3D11.1 supports creating plane SRVs on NV12/P010 textures:
        // - Y plane: R8_UNORM (NV12) or R16_UNORM (P010)
        // - UV plane: R8G8_UNORM (NV12) or R16G16_UNORM (P010)
        //=====================================================================

        // Get plane formats for SRV creation
        DXGI_FORMAT y_format = GetYPlaneFormat(format);
        DXGI_FORMAT uv_format = GetUVPlaneFormat(format);

        // Create or reuse GPU staging texture in same format but with SRV binding
        bool need_new_staging = !gpu_staging_texture_ ||
                                staging_width_ != tex_desc.Width ||
                                staging_height_ != tex_desc.Height ||
                                staging_format_ != tex_desc.Format;

        if (need_new_staging) {
            D3D11_TEXTURE2D_DESC staging_desc = {};
            staging_desc.Width = tex_desc.Width;
            staging_desc.Height = tex_desc.Height;
            staging_desc.MipLevels = 1;
            staging_desc.ArraySize = 1;
            staging_desc.Format = tex_desc.Format;  // Keep NV12/P010 format
            staging_desc.SampleDesc.Count = 1;
            staging_desc.SampleDesc.Quality = 0;
            staging_desc.Usage = D3D11_USAGE_DEFAULT;
            staging_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;  // Key difference!
            staging_desc.CPUAccessFlags = 0;
            staging_desc.MiscFlags = 0;

            HRESULT hr = device_->CreateTexture2D(&staging_desc, nullptr,
                                                   gpu_staging_texture_.ReleaseAndGetAddressOf());
            if (FAILED(hr)) {
                Debug::Log("D3D11HWFrameExtractor: Failed to create GPU staging texture (hr=0x" +
                          std::to_string(hr) + ")");
                return false;
            }

            staging_width_ = tex_desc.Width;
            staging_height_ = tex_desc.Height;
            staging_format_ = tex_desc.Format;

            Debug::Log("D3D11HWFrameExtractor: Created GPU staging texture " +
                       std::to_string(tex_desc.Width) + "x" + std::to_string(tex_desc.Height) +
                       " format=" + std::to_string(tex_desc.Format));
        }

        // GPU-to-GPU copy (zero CPU involvement)
        UINT src_subresource = D3D11CalcSubresource(0, array_index, 1);
        ctx->CopySubresourceRegion(gpu_staging_texture_.Get(), 0, 0, 0, 0,
                                   texture, src_subresource, nullptr);

        // Log first extraction for debugging
        static bool first_extract = true;
        if (first_extract) {
            const char* format_name = "Unknown";
            if (format == DXGI_FORMAT_NV12) format_name = "NV12";
            else if (format == DXGI_FORMAT_P010) format_name = "P010";
            else if (format == DXGI_FORMAT_P016) format_name = "P016";

            Debug::Log("D3D11HWFrameExtractor: First GPU zero-copy extraction");
            Debug::Log("  Format: " + std::string(format_name) + " (" + std::to_string(format) + ")");
            Debug::Log("  Size: " + std::to_string(tex_desc.Width) + "x" + std::to_string(tex_desc.Height));
            Debug::Log("  Array index: " + std::to_string(array_index));
            Debug::Log("  Y format: " + std::to_string(y_format));
            Debug::Log("  UV format: " + std::to_string(uv_format));
            first_extract = false;
        }

        // Create Y plane SRV on GPU staging texture
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
            srv_desc.Format = y_format;
            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv_desc.Texture2D.MostDetailedMip = 0;
            srv_desc.Texture2D.MipLevels = 1;

            HRESULT hr = device_->CreateShaderResourceView(gpu_staging_texture_.Get(), &srv_desc, srv_y);
            if (FAILED(hr)) {
                Debug::Log("D3D11HWFrameExtractor: Failed to create Y SRV (hr=0x" +
                          std::to_string(hr) + ")");
                return false;
            }

            static bool first_y_srv = true;
            if (first_y_srv) {
                Debug::Log("D3D11HWFrameExtractor: Y plane SRV created successfully");
                first_y_srv = false;
            }
        }

        // Create UV plane SRV on GPU staging texture
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
            srv_desc.Format = uv_format;
            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv_desc.Texture2D.MostDetailedMip = 0;
            srv_desc.Texture2D.MipLevels = 1;

            HRESULT hr = device_->CreateShaderResourceView(gpu_staging_texture_.Get(), &srv_desc, srv_uv);
            if (FAILED(hr)) {
                Debug::Log("D3D11HWFrameExtractor: Failed to create UV SRV (hr=0x" +
                          std::to_string(hr) + ")");
                (*srv_y)->Release();
                *srv_y = nullptr;
                return false;
            }

            static bool first_uv_srv = true;
            if (first_uv_srv) {
                Debug::Log("D3D11HWFrameExtractor: UV plane SRV created successfully");
                first_uv_srv = false;
            }
        }

        return true;
    }

    // Direct SRV creation path (for textures that already support it)
    bool is_array = tex_desc.ArraySize > 1;

    //=========================================================================
    // Create Y plane SRV (full resolution luma)
    //=========================================================================
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format = GetYPlaneFormat(format);

        if (is_array) {
            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srv_desc.Texture2DArray.MostDetailedMip = 0;
            srv_desc.Texture2DArray.MipLevels = 1;
            srv_desc.Texture2DArray.FirstArraySlice = array_index;
            srv_desc.Texture2DArray.ArraySize = 1;
        } else {
            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv_desc.Texture2D.MostDetailedMip = 0;
            srv_desc.Texture2D.MipLevels = 1;
        }

        HRESULT hr = device_->CreateShaderResourceView(texture, &srv_desc, srv_y);
        if (FAILED(hr)) {
            Debug::Log("D3D11HWFrameExtractor: Failed to create Y plane SRV (hr=0x" +
                      std::to_string(hr) + "), format=" + std::to_string(format) +
                      ", srv_format=" + std::to_string(srv_desc.Format));
            return false;
        }
    }

    //=========================================================================
    // Create UV plane SRV (half resolution chroma)
    //=========================================================================
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format = GetUVPlaneFormat(format);

        if (is_array) {
            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srv_desc.Texture2DArray.MostDetailedMip = 0;
            srv_desc.Texture2DArray.MipLevels = 1;
            srv_desc.Texture2DArray.FirstArraySlice = array_index;
            srv_desc.Texture2DArray.ArraySize = 1;
        } else {
            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv_desc.Texture2D.MostDetailedMip = 0;
            srv_desc.Texture2D.MipLevels = 1;
        }

        HRESULT hr = device_->CreateShaderResourceView(texture, &srv_desc, srv_uv);
        if (FAILED(hr)) {
            Debug::Log("D3D11HWFrameExtractor: Failed to create UV plane SRV (hr=0x" +
                      std::to_string(hr) + "), format=" + std::to_string(format) +
                      ", srv_format=" + std::to_string(srv_desc.Format));
            (*srv_y)->Release();
            *srv_y = nullptr;
            return false;
        }
    }

    return true;
}

} // namespace qcview

#endif // _WIN32
