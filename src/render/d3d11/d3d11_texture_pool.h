// D3D11TexturePool — Phase F.2.2.
//
// Singleton pool of GPU textures with LRU eviction + a 3-frame
// graveyard before ID3D11Texture2D release. Public API mirrors
// MetalTexturePool exactly — cross-platform consumers
// (TimelineThumbnailCache, ImageSequenceCache + GpuUploadThread,
// hover-thumb pipeline) link the same way on macOS and Windows.
//
// Format enum is identical to Metal:
//   0 = RGBA8Unorm    → DXGI_FORMAT_R8G8B8A8_UNORM
//   1 = RGBA16Float   → DXGI_FORMAT_R16G16B16A16_FLOAT
//   2 = BGRA8Unorm    → DXGI_FORMAT_B8G8R8A8_UNORM
//   3 = RGBA16Unorm   → DXGI_FORMAT_R16G16B16A16_UNORM
//
// Textures created with D3D11_USAGE_DEFAULT — uploads via
// ID3D11DeviceContext::UpdateSubresource, sampling via SRV in the
// compositor / OCIO / annotation passes (F.2.3+).

#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace qcv {

// Public view of a pool-managed texture. `texture` is an
// ID3D11Texture2D *; `srv` is an ID3D11ShaderResourceView *. Both
// returned as void* so consumers can include this header without
// dragging in <d3d11.h>.
struct D3D11Texture {
    void  *texture     = nullptr;
    void  *srv         = nullptr;
    int    width       = 0;
    int    height      = 0;
    int    pixelFormat = 0;
    std::size_t byteSize = 0;
    bool   valid       = false;
};

class D3D11TexturePool {
public:
    using Handle = std::uint64_t;

    static D3D11TexturePool &instance();
    static D3D11TexturePool &thumbnailInstance();

    D3D11TexturePool(const D3D11TexturePool &)            = delete;
    D3D11TexturePool &operator=(const D3D11TexturePool &) = delete;

    // ---- Lifecycle ----
    // Depends on D3D11DeviceManager being initialized first.
    bool initialize(std::size_t maxMemoryBytes = 8ULL * 1024 * 1024 * 1024);
    void shutdown();
    bool isInitialized() const { return m_initialized; }

    // ---- Creation ----
    Handle createTextureFromPixels(int width, int height, int pixelFormat,
                                   const void *pixelData,
                                   std::size_t pixelDataSize);
    Handle createEmptyTexture(int width, int height, int pixelFormat);

    bool updateTexture(Handle h, const void *pixelData,
                       std::size_t pixelDataSize);

    // ---- Access ----
    const D3D11Texture *texture(Handle h) const;
    bool dimensions(Handle h, int &width, int &height) const;
    void touch(Handle h);

    // ---- Deletion ----
    void queueDelete(Handle h);
    void processPendingDeletions();

    // ---- Memory ----
    std::size_t usedMemory() const;
    std::size_t maxMemory() const { return m_maxMemoryBytes; }
    std::size_t textureCount() const;
    void        evictToFitBytes(std::size_t requiredBytes);

    // CPU readback. `outRgba8` must be width*height*4 bytes.
    bool readbackTexture(Handle h, unsigned char *outRgba8) const;

private:
    D3D11TexturePool();
    ~D3D11TexturePool();

    void destroyTexture(D3D11Texture &tex);

    bool        m_initialized      = false;
    std::size_t m_maxMemoryBytes   = 0;
    std::size_t m_usedMemoryBytes  = 0;

    Handle      m_nextId = 1;
    std::unordered_map<Handle, D3D11Texture> m_textures;

    std::list<Handle>                                          m_lruOrder;
    std::unordered_map<Handle, std::list<Handle>::iterator>    m_lruPos;

    std::vector<Handle>  m_pendingDeletions;
    mutable std::mutex   m_deletionMutex;

    struct GraveyardEntry {
        D3D11Texture  texture;
        std::uint64_t deathCounter;
    };
    std::vector<GraveyardEntry> m_graveyard;
    std::uint64_t               m_graveyardCounter = 0;
    static constexpr std::uint64_t kGraveyardDelayFrames = 3;

    mutable std::mutex m_mutex;
};

} // namespace qcv
