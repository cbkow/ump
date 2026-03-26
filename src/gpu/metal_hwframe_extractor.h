#pragma once

#ifdef __APPLE__

#include <cstdint>

// Forward declarations (avoid ObjC in header)
struct AVFrame;

namespace qcview {

//=============================================================================
// MetalHWFrameExtractor
//
// Extracts CVPixelBuffer from VideoToolbox-decoded AVFrames and converts
// them to MTLTexture via CVMetalTextureCache (zero-copy).
//
// Flow:
//   AVFrame (AV_PIX_FMT_VIDEOTOOLBOX) -> CVPixelBufferRef -> CVMetalTexture -> MTLTexture
//=============================================================================

// Chroma subsampling mode — determines UV plane dimensions relative to Y
enum class ChromaSubsampling {
    YUV420,  // UV half width, half height (H.264, HEVC, VP9)
    YUV422,  // UV half width, full height (ProRes 422 variants)
    YUV444   // UV full width, full height (ProRes 4444/XQ)
};

struct MetalTextureFrame {
    void* y_texture = nullptr;     // id<MTLTexture> - luma plane
    void* uv_texture = nullptr;    // id<MTLTexture> - chroma plane (biplanar)
    void* pixel_buffer = nullptr;  // CVPixelBufferRef - retained, must be released
    int width = 0;
    int height = 0;
    int bit_depth = 8;
    bool is_full_range = false;
    ChromaSubsampling subsampling = ChromaSubsampling::YUV420;
    bool valid = false;
};

class MetalHWFrameExtractor {
public:
    MetalHWFrameExtractor();
    ~MetalHWFrameExtractor();

    MetalHWFrameExtractor(const MetalHWFrameExtractor&) = delete;
    MetalHWFrameExtractor& operator=(const MetalHWFrameExtractor&) = delete;

    bool Initialize();
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    // Extract MTLTextures from a VideoToolbox AVFrame (zero-copy)
    MetalTextureFrame ExtractFrame(AVFrame* vt_frame);

    // Extract MTLTextures from a raw CVPixelBufferRef (zero-copy, deferred path)
    // pixel_buffer is a CVPixelBufferRef cast to void*
    MetalTextureFrame ExtractFromPixelBuffer(void* pixel_buffer);

    // Release resources associated with an extracted frame
    void ReleaseFrame(MetalTextureFrame& frame);

private:
    void* texture_cache_ = nullptr;  // CVMetalTextureCacheRef
    bool initialized_ = false;
};

} // namespace qcview

#endif // __APPLE__
