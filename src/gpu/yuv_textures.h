#pragma once

#ifdef _WIN32

#include <d3d11_1.h>

namespace ump {

//=============================================================================
// YUVTextures
//
// Shared structure for passing raw YUV textures between D3D11VideoDecoder
// and TimelineInteropPipeline. Used in DUAL_VIEW mode where the decoder
// outputs pure YUV and the interop pipeline handles YUV→RGB conversion.
//
// Supports:
// - NV12/P010 (2-plane, hardware decode typical output)
// - Planar YUV (3-plane, YUV420P/YUV422P/YUV444P)
// - YUVA/GBRAP (4-plane, with alpha)
// - 8/10/12-bit depth
//=============================================================================

struct YUVTextures {
    //=========================================================================
    // NV12/P010 path (HW decode typical output)
    // 2-plane: Y plane + interleaved UV plane
    //=========================================================================
    ID3D11ShaderResourceView* y_srv = nullptr;
    ID3D11ShaderResourceView* uv_srv = nullptr;

    //=========================================================================
    // Planar YUV path (SW decode, professional codecs)
    // 3-4 planes: Y, U, V, [A] or G, B, R, [A] for RGB planar
    //=========================================================================
    ID3D11ShaderResourceView* plane_srvs[4] = {nullptr, nullptr, nullptr, nullptr};
    int plane_count = 0;  // 2 for NV12, 3 for planar YUV, 4 for YUVA/GBRAP

    //=========================================================================
    // Format info for shader selection
    //=========================================================================
    int width = 0;          // Luma width
    int height = 0;         // Luma height
    int chroma_w = 1;       // Chroma width divisor (1=4:4:4, 2=4:2:2/4:2:0)
    int chroma_h = 1;       // Chroma height divisor (1=4:4:4/4:2:2, 2=4:2:0)
    int bit_depth = 8;      // 8, 10, or 12

    bool is_nv12 = true;    // True for NV12/P010 (2-plane), false for planar
    bool has_alpha = false; // True for YUVA/GBRAP formats
    bool is_rgb_planar = false;  // True for GBRP/GBRAP (plane order: G, B, R, [A])
    bool is_full_range = false;  // True for full range (0-255/0-1023)
    bool is_hdr = false;         // True for HDR content (PQ EOTF)
    bool is_bt2020 = false;      // True for BT.2020 color primaries

    //=========================================================================
    // Source identification (for debugging/logging)
    //=========================================================================
    int source_frame = -1;  // Frame number in source file

    //=========================================================================
    // Validity check
    //=========================================================================
    bool IsValid() const {
        if (width <= 0 || height <= 0) return false;
        if (is_nv12) {
            return y_srv != nullptr && uv_srv != nullptr;
        } else {
            // Planar: need at least 2 planes (Y+U or equivalent)
            return plane_count >= 2 && plane_srvs[0] != nullptr && plane_srvs[1] != nullptr;
        }
    }

    //=========================================================================
    // Reset to default state
    //=========================================================================
    void Reset() {
        y_srv = nullptr;
        uv_srv = nullptr;
        for (int i = 0; i < 4; i++) {
            plane_srvs[i] = nullptr;
        }
        plane_count = 0;
        width = 0;
        height = 0;
        chroma_w = 1;
        chroma_h = 1;
        bit_depth = 8;
        is_nv12 = true;
        has_alpha = false;
        is_rgb_planar = false;
        is_full_range = false;
        is_hdr = false;
        is_bt2020 = false;
        source_frame = -1;
    }
};

} // namespace ump

#endif // _WIN32
