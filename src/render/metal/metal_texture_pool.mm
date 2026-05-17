// MetalTexturePool — see header for adaptation notes.

#include "metal_texture_pool.h"

#include "metal_device_manager.h"

#import <Metal/Metal.h>

#include <QtLogging>

#include <algorithm>
#include <cstring>

namespace qcv {

namespace {

MTLPixelFormat formatFromInt(int format)
{
    switch (format) {
        case 0:  return MTLPixelFormatRGBA8Unorm;
        case 1:  return MTLPixelFormatRGBA16Float;
        case 2:  return MTLPixelFormatBGRA8Unorm;
        case 3:  return MTLPixelFormatRGBA16Unorm;   // 16-bit linear (deep-color PNG)
        default: return MTLPixelFormatRGBA8Unorm;
    }
}

std::size_t bytesPerPixel(int format)
{
    switch (format) {
        case 0:  return 4;   // RGBA8
        case 1:  return 8;   // RGBA16F
        case 2:  return 4;   // BGRA8
        case 3:  return 8;   // RGBA16Unorm
        default: return 4;
    }
}

} // namespace

MetalTexturePool &MetalTexturePool::instance()
{
    static MetalTexturePool s_instance;
    return s_instance;
}

MetalTexturePool &MetalTexturePool::thumbnailInstance()
{
    static MetalTexturePool s_instance;
    return s_instance;
}

MetalTexturePool::MetalTexturePool() = default;

MetalTexturePool::~MetalTexturePool()
{
    shutdown();
}

bool MetalTexturePool::initialize(std::size_t maxMemoryBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) return true;

    auto &mgr = MetalDeviceManager::instance();
    if (!mgr.isInitialized()) {
        qWarning("MetalTexturePool: MetalDeviceManager not initialized");
        return false;
    }

    m_maxMemoryBytes  = maxMemoryBytes;
    m_usedMemoryBytes = 0;
    m_initialized     = true;

    qInfo("MetalTexturePool: initialized (budget=%zu MB)",
          maxMemoryBytes / (1024 * 1024));
    return true;
}

void MetalTexturePool::shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) return;

    qInfo("MetalTexturePool: shutting down (%zu live textures, %zu in graveyard)",
          m_textures.size(), m_graveyard.size());

    for (auto &kv : m_textures) {
        destroyTexture(kv.second);
    }
    m_textures.clear();
    m_lruOrder.clear();
    m_lruPos.clear();

    for (auto &entry : m_graveyard) {
        destroyTexture(entry.texture);
    }
    m_graveyard.clear();

    m_usedMemoryBytes = 0;
    m_initialized     = false;
}

MetalTexturePool::Handle MetalTexturePool::createTextureFromPixels(
    int width, int height, int pixelFormat,
    const void *pixelData, std::size_t /*pixelDataSize*/)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_initialized) return 0;
    }

    // Texture creation + upload happens OUTSIDE the pool mutex —
    // Metal's API is thread-safe and these calls don't touch pool
    // state. This prevents one caller's upload from blocking another's
    // bookkeeping operations (matches the old app's pattern).
    auto &mgr = MetalDeviceManager::instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.device();

    MTLPixelFormat mtlFmt = formatFromInt(pixelFormat);

    MTLTextureDescriptor *desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:mtlFmt
                                     width:width
                                    height:height
                                 mipmapped:NO];
    desc.usage       = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    desc.storageMode = MTLStorageModeShared;   // Apple Silicon unified memory

    id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
    if (!texture) {
        qWarning("MetalTexturePool: newTextureWithDescriptor failed (%dx%d fmt=%d)",
                 width, height, pixelFormat);
        return 0;
    }

    if (pixelData) {
        const std::size_t bpp = bytesPerPixel(pixelFormat);
        [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                   mipmapLevel:0
                     withBytes:pixelData
                   bytesPerRow:width * bpp];
    }

    const std::size_t newBytes =
        static_cast<std::size_t>(width) * height * bytesPerPixel(pixelFormat);

    std::lock_guard<std::mutex> lock(m_mutex);

    // Evict oldest until budget fits.
    while (m_usedMemoryBytes + newBytes > m_maxMemoryBytes && !m_lruOrder.empty()) {
        Handle oldest = m_lruOrder.front();
        m_lruOrder.pop_front();
        m_lruPos.erase(oldest);

        auto it = m_textures.find(oldest);
        if (it != m_textures.end()) {
            m_usedMemoryBytes -= it->second.byteSize;
            m_graveyard.push_back({it->second, m_graveyardCounter});
            m_textures.erase(it);
        }
    }

    MetalTexture pt;
    pt.texture     = (__bridge_retained void *)texture;   // pool now owns +1
    pt.width       = width;
    pt.height      = height;
    pt.pixelFormat = pixelFormat;
    pt.byteSize    = newBytes;
    pt.valid       = true;

    const Handle id = m_nextId++;
    m_textures[id] = pt;
    m_lruOrder.push_back(id);
    m_lruPos[id]   = std::prev(m_lruOrder.end());
    m_usedMemoryBytes += newBytes;

    return id;
}

MetalTexturePool::Handle MetalTexturePool::createEmptyTexture(
    int width, int height, int pixelFormat)
{
    return createTextureFromPixels(width, height, pixelFormat, nullptr, 0);
}

MetalTexturePool::Handle MetalTexturePool::registerExistingTexture(
    void *mtlTexture, int width, int height, int pixelFormat)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized || !mtlTexture) return 0;

    const std::size_t newBytes =
        static_cast<std::size_t>(width) * height * bytesPerPixel(pixelFormat);

    while (m_usedMemoryBytes + newBytes > m_maxMemoryBytes && !m_lruOrder.empty()) {
        Handle oldest = m_lruOrder.front();
        m_lruOrder.pop_front();
        m_lruPos.erase(oldest);

        auto it = m_textures.find(oldest);
        if (it != m_textures.end()) {
            m_usedMemoryBytes -= it->second.byteSize;
            m_graveyard.push_back({it->second, m_graveyardCounter});
            m_textures.erase(it);
        }
    }

    MetalTexture pt;
    pt.texture     = mtlTexture;   // assumes caller's reference becomes ours
    pt.width       = width;
    pt.height      = height;
    pt.pixelFormat = pixelFormat;
    pt.byteSize    = newBytes;
    pt.valid       = true;

    const Handle id = m_nextId++;
    m_textures[id] = pt;
    m_lruOrder.push_back(id);
    m_lruPos[id]   = std::prev(m_lruOrder.end());
    m_usedMemoryBytes += newBytes;

    return id;
}

bool MetalTexturePool::updateTexture(Handle h, const void *pixelData,
                                     std::size_t /*pixelDataSize*/)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_textures.find(h);
    if (it == m_textures.end()) return false;

    MetalTexture &tex = it->second;
    id<MTLTexture> mtl = (__bridge id<MTLTexture>)tex.texture;
    const std::size_t bpp = bytesPerPixel(tex.pixelFormat);

    [mtl replaceRegion:MTLRegionMake2D(0, 0, tex.width, tex.height)
           mipmapLevel:0
             withBytes:pixelData
           bytesPerRow:tex.width * bpp];

    // Inline LRU touch — caller already holds the mutex.
    auto pos = m_lruPos.find(h);
    if (pos != m_lruPos.end()) {
        m_lruOrder.splice(m_lruOrder.end(), m_lruOrder, pos->second);
    }
    return true;
}

const MetalTexture *MetalTexturePool::texture(Handle h) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_textures.find(h);
    return it == m_textures.end() ? nullptr : &it->second;
}

bool MetalTexturePool::dimensions(Handle h, int &width, int &height) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_textures.find(h);
    if (it == m_textures.end()) return false;
    width  = it->second.width;
    height = it->second.height;
    return true;
}

void MetalTexturePool::touch(Handle h)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto pos = m_lruPos.find(h);
    if (pos != m_lruPos.end()) {
        m_lruOrder.splice(m_lruOrder.end(), m_lruOrder, pos->second);
    }
}

void MetalTexturePool::queueDelete(Handle h)
{
    std::lock_guard<std::mutex> lock(m_deletionMutex);
    m_pendingDeletions.push_back(h);
}

void MetalTexturePool::processPendingDeletions()
{
    std::vector<Handle> toDelete;
    {
        std::lock_guard<std::mutex> lock(m_deletionMutex);
        toDelete.swap(m_pendingDeletions);
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    for (Handle id : toDelete) {
        auto it = m_textures.find(id);
        if (it == m_textures.end()) continue;
        m_usedMemoryBytes -= it->second.byteSize;
        m_graveyard.push_back({it->second, m_graveyardCounter});
        m_textures.erase(it);

        auto pos = m_lruPos.find(id);
        if (pos != m_lruPos.end()) {
            m_lruOrder.erase(pos->second);
            m_lruPos.erase(pos);
        }
    }

    ++m_graveyardCounter;
    m_graveyard.erase(
        std::remove_if(m_graveyard.begin(), m_graveyard.end(),
            [this](GraveyardEntry &entry) {
                if (m_graveyardCounter - entry.deathCounter >= kGraveyardDelayFrames) {
                    destroyTexture(entry.texture);
                    return true;
                }
                return false;
            }),
        m_graveyard.end());
}

std::size_t MetalTexturePool::usedMemory() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_usedMemoryBytes;
}

std::size_t MetalTexturePool::textureCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_textures.size();
}

void MetalTexturePool::evictToFitBytes(std::size_t requiredBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    while (m_usedMemoryBytes + requiredBytes > m_maxMemoryBytes &&
           !m_lruOrder.empty()) {
        Handle oldest = m_lruOrder.front();
        m_lruOrder.pop_front();
        m_lruPos.erase(oldest);

        auto it = m_textures.find(oldest);
        if (it != m_textures.end()) {
            m_usedMemoryBytes -= it->second.byteSize;
            m_graveyard.push_back({it->second, m_graveyardCounter});
            m_textures.erase(it);
        }
    }
}

void MetalTexturePool::destroyTexture(MetalTexture &tex)
{
    if (tex.texture) {
        // Match __bridge_retained on insertion: __bridge_transfer
        // hands ownership back to ARC so the next strong-ref drop
        // releases.
        id<MTLTexture> _ = (__bridge_transfer id<MTLTexture>)tex.texture;
        (void)_;
        tex.texture = nullptr;
    }
    tex.valid = false;
}

bool MetalTexturePool::readbackTexture(Handle h, unsigned char *outRgba8) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_textures.find(h);
    if (it == m_textures.end() || !it->second.valid || !it->second.texture) {
        return false;
    }

    const MetalTexture &tex = it->second;
    id<MTLTexture> mtl = (__bridge id<MTLTexture>)tex.texture;
    const int w = tex.width;
    const int h_ = tex.height;
    const MTLPixelFormat fmt = mtl.pixelFormat;
    const MTLRegion region = MTLRegionMake2D(0, 0, w, h_);

    if (fmt == MTLPixelFormatRGBA8Unorm || fmt == MTLPixelFormatRGBA8Unorm_sRGB) {
        [mtl getBytes:outRgba8
          bytesPerRow:w * 4
           fromRegion:region
          mipmapLevel:0];
        return true;
    }
    if (fmt == MTLPixelFormatBGRA8Unorm || fmt == MTLPixelFormatBGRA8Unorm_sRGB) {
        [mtl getBytes:outRgba8
          bytesPerRow:w * 4
           fromRegion:region
          mipmapLevel:0];
        // BGRA → RGBA channel swap.
        for (int i = 0; i < w * h_; ++i) {
            std::swap(outRgba8[i * 4 + 0], outRgba8[i * 4 + 2]);
        }
        return true;
    }
    if (fmt == MTLPixelFormatRGBA16Float) {
        std::vector<std::uint16_t> halfData(static_cast<std::size_t>(w) * h_ * 4);
        [mtl getBytes:halfData.data()
          bytesPerRow:w * 8
           fromRegion:region
          mipmapLevel:0];

        // IEEE-754 half-float → float32 → 8-bit clamped, per channel.
        for (int i = 0; i < w * h_ * 4; ++i) {
            const std::uint16_t bits = halfData[i];
            const std::uint32_t sign     = (bits >> 15) & 0x1;
            const std::uint32_t exponent = (bits >> 10) & 0x1F;
            const std::uint32_t mantissa = bits & 0x3FF;

            std::uint32_t f32Bits;
            float val;
            if (exponent == 0) {
                if (mantissa == 0) {
                    f32Bits = sign << 31;
                    std::memcpy(&val, &f32Bits, sizeof(float));
                } else {
                    val = 5.9604645e-8f * mantissa;
                    if (sign) val = -val;
                }
            } else if (exponent == 31) {
                f32Bits = (sign << 31) | 0x7F800000 | (mantissa << 13);
                std::memcpy(&val, &f32Bits, sizeof(float));
            } else {
                f32Bits = (sign << 31) | ((exponent - 15 + 127) << 23) | (mantissa << 13);
                std::memcpy(&val, &f32Bits, sizeof(float));
            }
            val = std::clamp(val, 0.0f, 1.0f);
            outRgba8[i] = static_cast<unsigned char>(val * 255.0f + 0.5f);
        }
        return true;
    }

    qWarning("MetalTexturePool::readbackTexture: unsupported pixel format %lu",
             static_cast<unsigned long>(fmt));
    return false;
}

} // namespace qcv
