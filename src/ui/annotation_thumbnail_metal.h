#pragma once

#ifdef __APPLE__

namespace qcview {

// Standalone Metal textures for AnnotationPanel note thumbnails.
// These bypass MetalTexturePool so they are never evicted under frame/video
// memory pressure. Caller owns the returned pointer and must destroy it.

// Creates an RGBA8Unorm MTLTexture and uploads the given pixels.
// Returns a retained id<MTLTexture> cast to void*, or nullptr on failure.
void* CreateStandaloneAnnotationThumbnailMetal(int width, int height,
                                               const void* rgba_pixels);

// Releases a texture returned by CreateStandaloneAnnotationThumbnailMetal.
// Safe to call with nullptr.
void DestroyStandaloneAnnotationThumbnailMetal(void* mtl_texture);

} // namespace qcview

#endif // __APPLE__
