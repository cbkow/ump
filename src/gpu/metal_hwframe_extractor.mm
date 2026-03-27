#ifdef __APPLE__

#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>

#include "metal_hwframe_extractor.h"
#include "metal_device_manager.h"
#include "../utils/debug_utils.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

namespace qcview {

MetalHWFrameExtractor::MetalHWFrameExtractor() = default;

MetalHWFrameExtractor::~MetalHWFrameExtractor() {
    Shutdown();
}

bool MetalHWFrameExtractor::Initialize() {
    if (initialized_) return true;

    auto& mgr = MetalDeviceManager::Instance();
    if (!mgr.IsInitialized()) {
        Debug::Log("MetalHWFrameExtractor: MetalDeviceManager not initialized");
        return false;
    }

    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();

    // Create CVMetalTextureCache for zero-copy CVPixelBuffer -> MTLTexture
    CVMetalTextureCacheRef cache = nullptr;
    CVReturn result = CVMetalTextureCacheCreate(
        kCFAllocatorDefault,
        nullptr,
        device,
        nullptr,
        &cache
    );

    if (result != kCVReturnSuccess || !cache) {
        Debug::Log("MetalHWFrameExtractor: Failed to create CVMetalTextureCache (error " +
                   std::to_string(result) + ")");
        return false;
    }

    texture_cache_ = cache;
    initialized_ = true;
    Debug::Log("MetalHWFrameExtractor: Initialized (CVMetalTextureCache created)");
    return true;
}

void MetalHWFrameExtractor::Shutdown() {
    if (!initialized_) return;

    if (texture_cache_) {
        CFRelease(texture_cache_);
        texture_cache_ = nullptr;
    }

    initialized_ = false;
    Debug::Log("MetalHWFrameExtractor: Shutdown");
}

MetalTextureFrame MetalHWFrameExtractor::ExtractFrame(AVFrame* vt_frame) {
    MetalTextureFrame result;

    if (!initialized_ || !vt_frame) return result;
    if (vt_frame->format != AV_PIX_FMT_VIDEOTOOLBOX) {
        Debug::Log("MetalHWFrameExtractor: Frame is not VideoToolbox format");
        return result;
    }

    // Get CVPixelBuffer from AVFrame
    CVPixelBufferRef pixel_buffer = (CVPixelBufferRef)vt_frame->data[3];
    if (!pixel_buffer) {
        Debug::Log("MetalHWFrameExtractor: No CVPixelBuffer in frame");
        return result;
    }

    // Delegate to ExtractFromPixelBuffer (which handles retain internally)
    return ExtractFromPixelBuffer(pixel_buffer);
}

MetalTextureFrame MetalHWFrameExtractor::ExtractFromPixelBuffer(void* pixel_buffer_ptr) {
    MetalTextureFrame result;

    if (!initialized_ || !pixel_buffer_ptr) return result;

    CVPixelBufferRef pixel_buffer = (CVPixelBufferRef)pixel_buffer_ptr;

    // Retain the pixel buffer (it must stay alive while textures are in use)
    CVPixelBufferRetain(pixel_buffer);
    result.pixel_buffer = pixel_buffer;

    size_t width = CVPixelBufferGetWidth(pixel_buffer);
    size_t height = CVPixelBufferGetHeight(pixel_buffer);
    OSType pixel_format = CVPixelBufferGetPixelFormatType(pixel_buffer);

    result.width = static_cast<int>(width);
    result.height = static_cast<int>(height);

    // Determine bit depth, chroma subsampling, and Metal pixel formats
    // from CVPixelBuffer format. All supported formats are biplanar (Y + interleaved UV).
    MTLPixelFormat y_format, uv_format;
    switch (pixel_format) {
        // 4:2:0 8-bit (H.264, HEVC, VP9)
        case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
            y_format = MTLPixelFormatR8Unorm;
            uv_format = MTLPixelFormatRG8Unorm;
            result.bit_depth = 8;
            result.is_full_range = false;
            result.subsampling = ChromaSubsampling::YUV420;
            break;
        case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
            y_format = MTLPixelFormatR8Unorm;
            uv_format = MTLPixelFormatRG8Unorm;
            result.bit_depth = 8;
            result.is_full_range = true;
            result.subsampling = ChromaSubsampling::YUV420;
            break;

        // 4:2:0 10-bit (HEVC 10-bit, VP9 Profile 2)
        case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
            y_format = MTLPixelFormatR16Unorm;
            uv_format = MTLPixelFormatRG16Unorm;
            result.bit_depth = 10;
            result.is_full_range = false;
            result.subsampling = ChromaSubsampling::YUV420;
            break;
        case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
            y_format = MTLPixelFormatR16Unorm;
            uv_format = MTLPixelFormatRG16Unorm;
            result.bit_depth = 10;
            result.is_full_range = true;
            result.subsampling = ChromaSubsampling::YUV420;
            break;

        // 4:2:2 8-bit (ProRes 422 Proxy/LT/Standard)
        case kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange:
            y_format = MTLPixelFormatR8Unorm;
            uv_format = MTLPixelFormatRG8Unorm;
            result.bit_depth = 8;
            result.is_full_range = false;
            result.subsampling = ChromaSubsampling::YUV422;
            break;
        case kCVPixelFormatType_422YpCbCr8BiPlanarFullRange:
            y_format = MTLPixelFormatR8Unorm;
            uv_format = MTLPixelFormatRG8Unorm;
            result.bit_depth = 8;
            result.is_full_range = true;
            result.subsampling = ChromaSubsampling::YUV422;
            break;

        // 4:2:2 10-bit (ProRes 422 HQ)
        case kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange:
            y_format = MTLPixelFormatR16Unorm;
            uv_format = MTLPixelFormatRG16Unorm;
            result.bit_depth = 10;
            result.is_full_range = false;
            result.subsampling = ChromaSubsampling::YUV422;
            break;
        case kCVPixelFormatType_422YpCbCr10BiPlanarFullRange:
            y_format = MTLPixelFormatR16Unorm;
            uv_format = MTLPixelFormatRG16Unorm;
            result.bit_depth = 10;
            result.is_full_range = true;
            result.subsampling = ChromaSubsampling::YUV422;
            break;

        // 4:2:2 16-bit (ProRes 422 high-precision)
        case kCVPixelFormatType_422YpCbCr16BiPlanarVideoRange:
            y_format = MTLPixelFormatR16Unorm;
            uv_format = MTLPixelFormatRG16Unorm;
            result.bit_depth = 16;
            result.is_full_range = false;
            result.subsampling = ChromaSubsampling::YUV422;
            break;

        // 4:4:4 8-bit (ProRes 4444)
        case kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange:
            y_format = MTLPixelFormatR8Unorm;
            uv_format = MTLPixelFormatRG8Unorm;
            result.bit_depth = 8;
            result.is_full_range = false;
            result.subsampling = ChromaSubsampling::YUV444;
            break;
        case kCVPixelFormatType_444YpCbCr8BiPlanarFullRange:
            y_format = MTLPixelFormatR8Unorm;
            uv_format = MTLPixelFormatRG8Unorm;
            result.bit_depth = 8;
            result.is_full_range = true;
            result.subsampling = ChromaSubsampling::YUV444;
            break;

        // 4:4:4 10-bit (ProRes 4444 / 4444 XQ)
        case kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange:
            y_format = MTLPixelFormatR16Unorm;
            uv_format = MTLPixelFormatRG16Unorm;
            result.bit_depth = 10;
            result.is_full_range = false;
            result.subsampling = ChromaSubsampling::YUV444;
            break;
        case kCVPixelFormatType_444YpCbCr10BiPlanarFullRange:
            y_format = MTLPixelFormatR16Unorm;
            uv_format = MTLPixelFormatRG16Unorm;
            result.bit_depth = 10;
            result.is_full_range = true;
            result.subsampling = ChromaSubsampling::YUV444;
            break;

        // 4:4:4 16-bit (ProRes 4444 XQ high-precision)
        case kCVPixelFormatType_444YpCbCr16BiPlanarVideoRange:
            y_format = MTLPixelFormatR16Unorm;
            uv_format = MTLPixelFormatRG16Unorm;
            result.bit_depth = 16;
            result.is_full_range = false;
            result.subsampling = ChromaSubsampling::YUV444;
            break;

        // 4:4:4:4 interleaved with alpha (ProRes 4444)
        // Single plane: A,Y',Cb,Cr per pixel → RGBA16Unorm (R=A, G=Y, B=Cb, A=Cr)
        case kCVPixelFormatType_4444AYpCbCr16: {
            result.bit_depth = 16;
            result.is_full_range = false;
            result.is_interleaved = true;
            result.has_alpha = true;
            result.subsampling = ChromaSubsampling::YUV444;

            CVMetalTextureCacheRef interleaved_cache = (CVMetalTextureCacheRef)texture_cache_;
            CVMetalTextureRef cv_texture = nullptr;
            CVReturn ret = CVMetalTextureCacheCreateTextureFromImage(
                kCFAllocatorDefault,
                interleaved_cache,
                pixel_buffer,
                nullptr,
                MTLPixelFormatRGBA16Unorm,
                width,
                height,
                0,  // plane index (single plane)
                &cv_texture
            );

            if (ret == kCVReturnSuccess && cv_texture) {
                id<MTLTexture> mtl = CVMetalTextureGetTexture(cv_texture);
                [mtl retain];
                result.y_texture = (void*)mtl;
                CFRelease(cv_texture);
                result.valid = true;
            } else {
                Debug::Log("MetalHWFrameExtractor: Failed to create y416 texture (error " +
                           std::to_string(ret) + ")");
                CVPixelBufferRelease(pixel_buffer);
                result.pixel_buffer = nullptr;
            }
            return result;
        }

        // 4:4:4:4 interleaved 8-bit with alpha
        // Single plane: Cb,Y',Cr,A per pixel → RGBA8Unorm (R=Cb, G=Y, B=Cr, A=A)
        case kCVPixelFormatType_4444YpCbCrA8: {
            result.bit_depth = 8;
            result.is_full_range = false;
            result.is_interleaved = true;
            result.has_alpha = true;
            result.subsampling = ChromaSubsampling::YUV444;

            CVMetalTextureCacheRef interleaved_cache = (CVMetalTextureCacheRef)texture_cache_;
            CVMetalTextureRef cv_texture = nullptr;
            CVReturn ret = CVMetalTextureCacheCreateTextureFromImage(
                kCFAllocatorDefault,
                interleaved_cache,
                pixel_buffer,
                nullptr,
                MTLPixelFormatRGBA8Unorm,
                width,
                height,
                0,
                &cv_texture
            );

            if (ret == kCVReturnSuccess && cv_texture) {
                id<MTLTexture> mtl = CVMetalTextureGetTexture(cv_texture);
                [mtl retain];
                result.y_texture = (void*)mtl;
                CFRelease(cv_texture);
                result.valid = true;
            } else {
                Debug::Log("MetalHWFrameExtractor: Failed to create v408 texture (error " +
                           std::to_string(ret) + ")");
                CVPixelBufferRelease(pixel_buffer);
                result.pixel_buffer = nullptr;
            }
            return result;
        }

        default:
            Debug::Log("MetalHWFrameExtractor: Unsupported pixel format: " +
                       std::to_string(pixel_format));
            CVPixelBufferRelease(pixel_buffer);
            result.pixel_buffer = nullptr;
            return result;
    }

    CVMetalTextureCacheRef cache = (CVMetalTextureCacheRef)texture_cache_;

    // Create Y (luma) texture from plane 0
    {
        size_t plane_width = CVPixelBufferGetWidthOfPlane(pixel_buffer, 0);
        size_t plane_height = CVPixelBufferGetHeightOfPlane(pixel_buffer, 0);

        CVMetalTextureRef cv_texture = nullptr;
        CVReturn ret = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault,
            cache,
            pixel_buffer,
            nullptr,
            y_format,
            plane_width,
            plane_height,
            0,  // plane index
            &cv_texture
        );

        if (ret == kCVReturnSuccess && cv_texture) {
            // CVMetalTextureGetTexture returns an unretained reference.
            // In non-ARC, __bridge_retained is a no-op, so we must retain manually
            // to keep the MTLTexture alive after releasing the CVMetalTextureRef.
            id<MTLTexture> y_mtl = CVMetalTextureGetTexture(cv_texture);
            [y_mtl retain];
            result.y_texture = (void*)y_mtl;
            CFRelease(cv_texture);
        } else {
            Debug::Log("MetalHWFrameExtractor: Failed to create Y texture (error " +
                       std::to_string(ret) + ")");
            CVPixelBufferRelease(pixel_buffer);
            result.pixel_buffer = nullptr;
            return result;
        }
    }

    // Create UV (chroma) texture from plane 1
    {
        size_t plane_width = CVPixelBufferGetWidthOfPlane(pixel_buffer, 1);
        size_t plane_height = CVPixelBufferGetHeightOfPlane(pixel_buffer, 1);

        CVMetalTextureRef cv_texture = nullptr;
        CVReturn ret = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault,
            cache,
            pixel_buffer,
            nullptr,
            uv_format,
            plane_width,
            plane_height,
            1,  // plane index
            &cv_texture
        );

        if (ret == kCVReturnSuccess && cv_texture) {
            id<MTLTexture> uv_mtl = CVMetalTextureGetTexture(cv_texture);
            [uv_mtl retain];
            result.uv_texture = (void*)uv_mtl;
            CFRelease(cv_texture);
        } else {
            Debug::Log("MetalHWFrameExtractor: Failed to create UV texture (error " +
                       std::to_string(ret) + ")");
            // Clean up Y texture
            if (result.y_texture) {
                [(id<MTLTexture>)result.y_texture release];
                result.y_texture = nullptr;
            }
            CVPixelBufferRelease(pixel_buffer);
            result.pixel_buffer = nullptr;
            return result;
        }
    }

    result.valid = true;
    return result;
}

void MetalHWFrameExtractor::ReleaseFrame(MetalTextureFrame& frame) {
    // MTLTexture is an ObjC object, not a CF type — use release, not CFRelease
    if (frame.y_texture) {
        [(id<MTLTexture>)frame.y_texture release];
        frame.y_texture = nullptr;
    }
    if (frame.uv_texture) {
        [(id<MTLTexture>)frame.uv_texture release];
        frame.uv_texture = nullptr;
    }
    if (frame.pixel_buffer) {
        CVPixelBufferRelease((CVPixelBufferRef)frame.pixel_buffer);
        frame.pixel_buffer = nullptr;
    }
    frame.valid = false;
}

} // namespace qcview

#endif // __APPLE__
