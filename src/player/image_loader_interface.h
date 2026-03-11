#pragma once

#include <string>
#include <vector>
#include <memory>
#include <glad/gl.h>
#include "pipeline_mode.h"

namespace qcview {

//=============================================================================
// Platform-Agnostic Pixel Format
//=============================================================================

// Cross-platform pixel format enum - replaces direct GL enum usage in data layer
enum class PixelFormat {
    RGBA8,      // 4 bytes/pixel, unsigned byte
    RGBA16,     // 8 bytes/pixel, unsigned short
    RGBA16F,    // 8 bytes/pixel, half float
};

// Convert PixelFormat to GL enums (available on all platforms since GL headers are still included)
inline GLenum PixelFormatToGLFormat(PixelFormat fmt) { (void)fmt; return GL_RGBA; }
inline GLenum PixelFormatToGLType(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::RGBA8:  return GL_UNSIGNED_BYTE;
        case PixelFormat::RGBA16: return GL_UNSIGNED_SHORT;
        case PixelFormat::RGBA16F: return GL_HALF_FLOAT;
    }
    return GL_UNSIGNED_BYTE;
}

inline PixelFormat GLTypeToPixelFormat(GLenum gl_type) {
    switch (gl_type) {
        case GL_UNSIGNED_SHORT: return PixelFormat::RGBA16;
        case GL_HALF_FLOAT:     return PixelFormat::RGBA16F;
        default:                return PixelFormat::RGBA8;
    }
}

inline size_t PixelFormatBytesPerPixel(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::RGBA8:  return 4;
        case PixelFormat::RGBA16: return 8;
        case PixelFormat::RGBA16F: return 8;
    }
    return 4;
}

//=============================================================================
// Universal Pixel Data (Type-Erased)
//=============================================================================

// Supports all pipeline modes: RGBA8, RGBA16, RGBA16F
// Raw bytes stored in uint8_t vector, interpreted based on pixel_format
struct PixelData {
    std::vector<uint8_t> pixels;        // Raw bytes (RGBA8: 4 bytes/px, RGBA16/16F: 8 bytes/px)
    int width = 0;
    int height = 0;
    PixelFormat pixel_format = PixelFormat::RGBA8;  // Platform-agnostic format
    GLenum gl_format = GL_RGBA;          // Always GL_RGBA (4 channels) - derived from pixel_format
    GLenum gl_type = GL_UNSIGNED_BYTE;   // GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT, or GL_HALF_FLOAT
    PipelineMode pipeline_mode = PipelineMode::NORMAL;
    bool is_sentinel = false;            // True for gap/broken sentinel frames

    size_t ByteSize() const { return pixels.size(); }

    // Helper to set format consistently
    void SetFormat(PixelFormat fmt) {
        pixel_format = fmt;
        gl_format = PixelFormatToGLFormat(fmt);
        gl_type = PixelFormatToGLType(fmt);
    }
};

//=============================================================================
// Sentinel Constants & Factories
//=============================================================================

// Files smaller than this are considered corrupt/broken (e.g. truncated renders)
static constexpr size_t kBrokenFileThresholdBytes = 15 * 1024;  // 15 KB

// Minimal LRU weight so sentinels don't evict real frames
static constexpr size_t kSentinelCacheByteSize = 4;

// Transparent RGBA8 sentinel — represents a gap (file doesn't exist in sequence)
// Full-size buffer so dimensions propagate correctly through the display pipeline
inline std::shared_ptr<PixelData> MakeGapSentinel(int w = 1, int h = 1) {
    auto pd = std::make_shared<PixelData>();
    pd->width = w;
    pd->height = h;
    pd->SetFormat(PixelFormat::RGBA8);
    pd->pipeline_mode = PipelineMode::NORMAL;
    pd->is_sentinel = true;
    pd->pixels.resize(static_cast<size_t>(w) * h * 4, 0);  // All zeros (transparent RGBA8)
    return pd;
}

// Broken/corrupt file sentinel — gray #787878 background with centered broken.png icon
// PNG loaded lazily on first call, cached for process lifetime.
// Falls back to plain gray if PNG missing. Thread-safe.
std::shared_ptr<PixelData> MakeBrokenSentinel(int w = 1, int h = 1);

// Check if a PixelData is a sentinel (gap or broken frame)
inline bool IsSentinel(const std::shared_ptr<PixelData>& pd) {
    return pd && pd->is_sentinel;
}

//=============================================================================
// Abstract Image Loader Interface
//=============================================================================

// Runtime-polymorphic image loader
// Replaces compile-time template-based LoaderPolicy
class IImageLoader {
public:
    virtual ~IImageLoader() = default;

    // Load frame from disk → CPU pixel buffer
    // Thread-safe: Called from background I/O threads
    virtual std::shared_ptr<PixelData> LoadFrame(
        const std::string& path,
        const std::string& layer,        // Used for EXR multi-layer, ignored for TIFF/PNG/JPEG
        PipelineMode pipeline_mode       // Determines output format (RGBA8/16/16F)
    ) = 0;

    // Load thumbnail (optimized low-resolution decode)
    // Bypasses expensive color management and uses format-specific optimizations
    // For JPEG: Uses libjpeg DCT scaling (1/2, 1/4, 1/8 resolution)
    // For TIFF/PNG/EXR: Skips color management, loads at full res then downsamples
    // Always returns RGBA8 format regardless of source bit depth
    // max_size: Maximum dimension (width or height) - actual size maintains aspect ratio
    virtual std::shared_ptr<PixelData> LoadThumbnail(
        const std::string& path,
        int max_size = 320              // Maximum width or height
    ) {
        // Default implementation: Just call LoadFrame and let caller resize
        // Loaders can override for optimized thumbnail generation
        return LoadFrame(path, "", PipelineMode::NORMAL);
    }

    // Get dimensions without full load (fast metadata read)
    virtual bool GetDimensions(const std::string& path, int& width, int& height) = 0;

    // Loader name for debugging/logging
    virtual std::string GetLoaderName() const = 0;
};

} // namespace qcview
