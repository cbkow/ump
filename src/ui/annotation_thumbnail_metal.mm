#ifdef __APPLE__

#import <Metal/Metal.h>

#include "annotation_thumbnail_metal.h"
#include "../gpu/metal_device_manager.h"
#include "../utils/debug_utils.h"

namespace qcview {

void* CreateStandaloneAnnotationThumbnailMetal(int width, int height,
                                               const void* rgba_pixels) {
    if (width <= 0 || height <= 0) return nullptr;

    auto& mgr = MetalDeviceManager::Instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.GetDevice();
    if (!device) {
        Debug::Log("AnnotationThumbnailMetal: device not available");
        return nullptr;
    }

    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;

    id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
    if (!texture) {
        Debug::Log("AnnotationThumbnailMetal: newTextureWithDescriptor failed "
                   + std::to_string(width) + "x" + std::to_string(height));
        return nullptr;
    }

    if (rgba_pixels) {
        [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                   mipmapLevel:0
                     withBytes:rgba_pixels
                   bytesPerRow:width * 4];
    }

    // Project uses MRC (see DestroyTexture in metal_texture_pool.mm).
    // newTextureWithDescriptor returns a +1 retained object; hand that
    // reference to the caller.
    return (void*)texture;
}

void DestroyStandaloneAnnotationThumbnailMetal(void* mtl_texture) {
    if (!mtl_texture) return;
    [(id)mtl_texture release];
}

} // namespace qcview

#endif // __APPLE__
