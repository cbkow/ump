#pragma once

#ifdef __APPLE__

#include <imgui.h>
#include <mutex>
#include <unordered_map>
#include <deque>
#include <list>
#include <string>
#include <vector>
#include <cstdint>

namespace qcview {

//=============================================================================
// MetalTexture — A GPU texture managed by the pool
//=============================================================================

struct MetalTexture {
    void* texture = nullptr;        // id<MTLTexture>
    void* descriptor_set = nullptr; // ImGui descriptor (from ImGui_ImplMetal_AddTexture)
    int width = 0;
    int height = 0;
    int pixel_format = 0;           // MTLPixelFormat cast to int
    size_t byte_size = 0;
    bool is_valid = false;
};

//=============================================================================
// MetalTexturePool — Manages GPU textures for frame display
//
// Mirrors VulkanTexturePool interface for cross-platform code.
// Thread-safe. All Metal operations happen on the main thread.
//=============================================================================

class MetalTexturePool {
public:
    static MetalTexturePool& Instance();
    static MetalTexturePool& ThumbnailInstance();  // Separate pool for timeline thumbnails

    MetalTexturePool(const MetalTexturePool&) = delete;
    MetalTexturePool& operator=(const MetalTexturePool&) = delete;

    //=========================================================================
    // Lifecycle
    //=========================================================================

    bool Initialize(size_t max_memory_bytes = 8ULL * 1024 * 1024 * 1024);
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Texture Creation
    //=========================================================================

    // Create texture from CPU pixel data (RGBA8 or RGBA16F)
    // format: 0 = RGBA8Unorm, 1 = RGBA16Float, 2 = BGRA8Unorm
    uint64_t CreateTextureFromPixels(int width, int height, int format,
                                      const void* pixel_data, size_t pixel_data_size);

    // Create empty texture
    uint64_t CreateEmptyTexture(int width, int height, int format);

    // Register an already-created MTLTexture in the pool (zero-copy, takes ownership)
    // The texture must have been retained (+1) by the caller; pool takes ownership.
    // format: 0 = RGBA8Unorm, 1 = RGBA16Float, 2 = BGRA8Unorm
    uint64_t RegisterExistingTexture(void* mtl_texture, int width, int height, int format);

    // Update existing texture with new pixel data
    bool UpdateTexture(uint64_t texture_id, const void* pixel_data, size_t pixel_data_size);

    //=========================================================================
    // Texture Access
    //=========================================================================

    const MetalTexture* GetTexture(uint64_t texture_id) const;
    ImTextureID GetImTextureID(uint64_t texture_id) const;
    bool GetDimensions(uint64_t texture_id, int& width, int& height) const;
    void Touch(uint64_t texture_id);

    //=========================================================================
    // Texture Deletion
    //=========================================================================

    void QueueDelete(uint64_t texture_id);
    void ProcessPendingDeletions();

    //=========================================================================
    // Memory Management
    //=========================================================================

    size_t GetUsedMemory() const;
    size_t GetMaxMemory() const { return max_memory_bytes_; }
    size_t GetTextureCount() const;
    void EvictToFitBytes(size_t required_bytes);

    // Read texture pixels back to CPU (RGBA8 output, converts from RGBA16F if needed)
    // Returns true on success. Output buffer must be width * height * 4 bytes.
    bool ReadbackTexture(uint64_t texture_id, unsigned char* out_rgba8) const;

private:
    MetalTexturePool();
    ~MetalTexturePool();

    void DestroyTexture(MetalTexture& tex);

    bool initialized_ = false;
    size_t max_memory_bytes_ = 0;
    size_t used_memory_bytes_ = 0;

    uint64_t next_id_ = 1;
    std::unordered_map<uint64_t, MetalTexture> textures_;
    std::list<uint64_t> lru_order_;                                    // O(1) splice for Touch
    std::unordered_map<uint64_t, std::list<uint64_t>::iterator> lru_pos_;  // O(1) lookup for Touch

    std::vector<uint64_t> pending_deletions_;
    mutable std::mutex deletion_mutex_;

    struct GraveyardEntry {
        MetalTexture texture;
        uint64_t death_counter;
    };
    std::vector<GraveyardEntry> graveyard_;
    uint64_t graveyard_counter_ = 0;
    static constexpr uint64_t kGraveyardDelayCalls = 3;  // Reduced from 30 — 30 frames of 16MB textures = 480MB limbo

    mutable std::mutex mutex_;
};

} // namespace qcview

#endif // __APPLE__
