// MetalTexturePool — Phase 7.5 B.4 / Guide 19 §2.1.
//
// Direct lift from old QCView's src/gpu/metal_texture_pool.{h,mm}
// (~418 LoC). Singleton pool of GPU textures with:
//   - LRU + bytes-budgeted eviction
//   - 3-frame graveyard before MTLTexture release (avoids tearing
//     down a texture mid-GPU-use, which stalls the queue)
//   - O(1) Touch via std::list splice + iterator map
//   - Lock-free pixel upload (MTL APIs are thread-safe; only the pool
//     bookkeeping holds a mutex)
//
// The graveyard delay is 3 GPU frames in the new port (matches the
// old app's reduced value — the old comment notes the original 30
// kept ~480MB in limbo at peak).
//
// Adaptation notes vs old app:
//   - Namespace qcview → qcv
//   - ImGui-side pieces dropped: descriptor_set field,
//     GetImTextureID(), ThumbnailInstance(). The new port has no
//     ImGui; thumbnail panel will get its own pool when it ports.
//   - Public MetalTexture struct keeps width/height/format/bytes for
//     consumers; the texture pointer is void* (bridged on the .mm
//     side, where ARC manages retain/release).
//   - Singleton via instance(); shutdown() must be called explicitly
//     before MetalDeviceManager::shutdown() since the pool's textures
//     reference the device.

#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace qcv {

// Public view of a pool-managed texture. The `texture` pointer is a
// bridged id<MTLTexture> (caller does (__bridge id<MTLTexture>)tex
// on the .mm side); pool owns the underlying ObjC retain.
struct MetalTexture {
    void  *texture     = nullptr;
    int    width       = 0;
    int    height      = 0;
    int    pixelFormat = 0;     // 0 = RGBA8Unorm, 1 = RGBA16Float, 2 = BGRA8Unorm
    std::size_t byteSize = 0;
    bool   valid       = false;
};

class MetalTexturePool {
public:
    using Handle = std::uint64_t;

    static MetalTexturePool &instance();
    // Separate pool for hover-thumbnail textures. Keeps the timeline
    // thumbnail cache's RGBA8 ~230 KB textures from competing for
    // budget with the main playback path's full-resolution
    // RGBA16/16F textures.
    static MetalTexturePool &thumbnailInstance();

    MetalTexturePool(const MetalTexturePool &)            = delete;
    MetalTexturePool &operator=(const MetalTexturePool &) = delete;

    // ---- Lifecycle ----
    bool initialize(std::size_t maxMemoryBytes = 8ULL * 1024 * 1024 * 1024);
    void shutdown();
    bool isInitialized() const { return m_initialized; }

    // ---- Creation ----
    // pixelFormat: 0 = RGBA8Unorm, 1 = RGBA16Float,
    //              2 = BGRA8Unorm, 3 = RGBA16Unorm
    // pixelData may be nullptr (creates an empty texture).
    Handle createTextureFromPixels(int width, int height, int pixelFormat,
                                   const void *pixelData,
                                   std::size_t pixelDataSize);
    Handle createEmptyTexture(int width, int height, int pixelFormat);

    // Take ownership of an externally-created MTLTexture. Caller must
    // have +1 retained the object (or hand a strong reference owned
    // by ARC); the pool absorbs it.
    Handle registerExistingTexture(void *mtlTexture, int width, int height,
                                   int pixelFormat);

    // Update existing texture pixels in place.
    bool updateTexture(Handle h, const void *pixelData,
                       std::size_t pixelDataSize);

    // ---- Access ----
    const MetalTexture *texture(Handle h) const;
    bool dimensions(Handle h, int &width, int &height) const;
    void touch(Handle h);

    // ---- Deletion ----
    // Defers actual release by `kGraveyardDelayFrames` calls to
    // processPendingDeletions(); makes it safe to delete a texture
    // that the GPU may still be reading from this frame.
    void queueDelete(Handle h);
    void processPendingDeletions();

    // ---- Memory ----
    std::size_t usedMemory() const;
    std::size_t maxMemory() const { return m_maxMemoryBytes; }
    std::size_t textureCount() const;
    void        evictToFitBytes(std::size_t requiredBytes);

    // CPU readback. `outRgba8` must be width*height*4 bytes. Handles
    // RGBA8 / BGRA8 (with swizzle) / RGBA16F (with half→8-bit
    // conversion). Returns false on unsupported format.
    bool readbackTexture(Handle h, unsigned char *outRgba8) const;

private:
    MetalTexturePool();
    ~MetalTexturePool();

    void destroyTexture(MetalTexture &tex);

    bool        m_initialized      = false;
    std::size_t m_maxMemoryBytes   = 0;
    std::size_t m_usedMemoryBytes  = 0;

    Handle      m_nextId = 1;
    std::unordered_map<Handle, MetalTexture> m_textures;

    // O(1) Touch: list of IDs in LRU order, plus a hash map from ID
    // to its position in the list (for splice-to-back).
    std::list<Handle>                                          m_lruOrder;
    std::unordered_map<Handle, std::list<Handle>::iterator>    m_lruPos;

    std::vector<Handle>  m_pendingDeletions;
    mutable std::mutex   m_deletionMutex;

    struct GraveyardEntry {
        MetalTexture  texture;
        std::uint64_t deathCounter;
    };
    std::vector<GraveyardEntry> m_graveyard;
    std::uint64_t               m_graveyardCounter = 0;
    static constexpr std::uint64_t kGraveyardDelayFrames = 3;

    mutable std::mutex m_mutex;
};

} // namespace qcv
