#pragma once

#ifdef _WIN32

#include <d3d11_1.h>
#include <wrl/client.h>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <chrono>

namespace qcview {

//=============================================================================
// D3D11TexturePool
//
// Manages a pool of reusable D3D11 textures to avoid frequent allocations.
// Similar to GPUTexturePool but for D3D11 resources.
//
// Features:
// - Automatic texture reuse based on dimensions and format
// - LRU-based eviction of old textures
// - Memory budget management
// - Thread-safe operations
//
// Usage:
//   auto tex = pool.AcquireTexture(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM);
//   // Use texture...
//   pool.ReleaseTexture(tex);  // Returns to pool for reuse
//=============================================================================

struct PooledTexture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    size_t memory_size = 0;
    std::chrono::steady_clock::time_point last_used;
    bool in_use = false;
};

class D3D11TexturePool {
public:
    D3D11TexturePool();
    ~D3D11TexturePool();

    // Delete copy/move
    D3D11TexturePool(const D3D11TexturePool&) = delete;
    D3D11TexturePool& operator=(const D3D11TexturePool&) = delete;

    //=========================================================================
    // Configuration
    //=========================================================================

    // Set maximum memory budget in bytes (default: 512 MB)
    void SetMemoryBudget(size_t budget_bytes);

    // Set maximum age for unused textures in seconds (default: 30s)
    void SetMaxTextureAge(double age_seconds);

    // Set whether to create SRVs automatically (default: true)
    void SetCreateSRV(bool create) { create_srv_ = create; }

    // Set whether to create RTVs automatically (default: false)
    void SetCreateRTV(bool create) { create_rtv_ = create; }

    //=========================================================================
    // Texture Acquisition
    //=========================================================================

    // Acquire a texture with specified dimensions and format.
    // Returns existing pooled texture if available, creates new one otherwise.
    // bind_flags: D3D11_BIND_SHADER_RESOURCE, D3D11_BIND_RENDER_TARGET, etc.
    PooledTexture* AcquireTexture(
        UINT width, UINT height,
        DXGI_FORMAT format,
        UINT bind_flags = D3D11_BIND_SHADER_RESOURCE);

    // Release a texture back to the pool for reuse
    void ReleaseTexture(PooledTexture* texture);

    // Release a texture by its raw pointer
    void ReleaseTexture(ID3D11Texture2D* texture);

    //=========================================================================
    // Pool Management
    //=========================================================================

    // Evict textures that haven't been used recently
    void EvictOldTextures();

    // Evict all textures (for memory pressure)
    void EvictAllTextures();

    // Evict textures until under memory budget
    void EnforceMemoryBudget();

    // Clear the entire pool
    void Clear();

    //=========================================================================
    // Statistics
    //=========================================================================

    struct Stats {
        size_t total_textures = 0;       // Total textures in pool
        size_t textures_in_use = 0;      // Currently acquired textures
        size_t textures_available = 0;   // Available for reuse
        size_t memory_used = 0;          // Total memory used
        size_t memory_budget = 0;        // Memory budget
        uint64_t acquire_count = 0;      // Total acquire calls
        uint64_t reuse_count = 0;        // Times a pooled texture was reused
        uint64_t create_count = 0;       // Times a new texture was created
        uint64_t evict_count = 0;        // Times textures were evicted
    };

    Stats GetStats() const;

private:
    // Key for texture lookup
    struct TextureKey {
        UINT width;
        UINT height;
        DXGI_FORMAT format;
        UINT bind_flags;

        bool operator==(const TextureKey& other) const {
            return width == other.width &&
                   height == other.height &&
                   format == other.format &&
                   bind_flags == other.bind_flags;
        }
    };

    struct TextureKeyHash {
        size_t operator()(const TextureKey& key) const {
            return std::hash<UINT>()(key.width) ^
                   (std::hash<UINT>()(key.height) << 1) ^
                   (std::hash<UINT>()(key.format) << 2) ^
                   (std::hash<UINT>()(key.bind_flags) << 3);
        }
    };

    // Calculate memory size for a texture
    size_t CalculateTextureSize(UINT width, UINT height, DXGI_FORMAT format);

    // Create a new texture
    PooledTexture* CreateTexture(UINT width, UINT height, DXGI_FORMAT format, UINT bind_flags);

    // All pooled textures
    std::vector<std::unique_ptr<PooledTexture>> textures_;

    // Index by key for fast lookup of available textures
    std::unordered_multimap<TextureKey, PooledTexture*, TextureKeyHash> available_index_;

    // Configuration
    size_t memory_budget_ = 512 * 1024 * 1024;  // 512 MB default
    double max_texture_age_ = 30.0;             // 30 seconds
    bool create_srv_ = true;
    bool create_rtv_ = false;

    // Statistics
    size_t total_memory_used_ = 0;
    uint64_t acquire_count_ = 0;
    uint64_t reuse_count_ = 0;
    uint64_t create_count_ = 0;
    uint64_t evict_count_ = 0;

    mutable std::mutex mutex_;
};

} // namespace qcview

#endif // _WIN32
