#pragma once

#ifdef __linux__

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <imgui.h>

#include <mutex>
#include <unordered_map>
#include <deque>
#include <string>
#include <vector>

namespace qcview {

//=============================================================================
// VulkanTexture — A GPU texture (VkImage + VkImageView + VkDescriptorSet)
//
// The VkDescriptorSet is registered with ImGui so it can be used directly
// as an ImTextureID for display.
//=============================================================================

struct VulkanTexture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView image_view = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;  // For ImGui display
    VkSampler sampler = VK_NULL_HANDLE;

    int width = 0;
    int height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    size_t byte_size = 0;  // Total GPU memory for LRU tracking

    bool is_valid = false;
};

//=============================================================================
// VulkanTexturePool — Manages GPU textures for frame display
//
// Thread-safe. All Vulkan operations happen on the main thread via
// ProcessPendingOperations(). Background threads queue upload requests.
//
// Mirrors GPUTexturePool (texture_pool.h) interface where applicable.
//=============================================================================

class VulkanTexturePool {
public:
    static VulkanTexturePool& Instance();
    static VulkanTexturePool& ThumbnailInstance();  // Separate pool for timeline thumbnails

    // Delete copy/move
    VulkanTexturePool(const VulkanTexturePool&) = delete;
    VulkanTexturePool& operator=(const VulkanTexturePool&) = delete;

    //=========================================================================
    // Lifecycle
    //=========================================================================

    bool Initialize(size_t max_memory_bytes = 2ULL * 1024 * 1024 * 1024);  // 2 GB default
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Texture Creation (main thread only — requires Vulkan context)
    //=========================================================================

    // Create a texture from CPU pixel data. Returns texture ID (pool index).
    // Automatically registers with ImGui for display.
    // pixel_data: raw RGBA bytes matching the format
    // format: VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT
    uint64_t CreateTextureFromPixels(
        int width, int height,
        VkFormat format,
        const void* pixel_data,
        size_t pixel_data_size
    );

    // Create a texture by copying from an existing VkImage (GPU→GPU copy).
    // The source image must be in SHADER_READ_ONLY_OPTIMAL or TRANSFER_SRC_OPTIMAL layout.
    // Used by GPU decode path (DMA-BUF import + YUV shader → pool texture).
    uint64_t CreateTextureFromImage(
        VkImage source_image,
        int width, int height,
        VkFormat format
    );

    // Create an empty texture (for later upload)
    uint64_t CreateEmptyTexture(int width, int height, VkFormat format);

    // Update an existing texture with new pixel data
    bool UpdateTexture(uint64_t texture_id, const void* pixel_data, size_t pixel_data_size);

    // Update an existing texture by GPU→GPU copy from a VkImage
    // Source image must be in COLOR_ATTACHMENT_OPTIMAL layout.
    // Returns false if texture_id not found or dimensions mismatch.
    bool UpdateTextureFromImage(uint64_t texture_id, VkImage source_image, int width, int height);

    //=========================================================================
    // Texture Access
    //=========================================================================

    // Get the VulkanTexture for a given ID. Returns nullptr if not found.
    const VulkanTexture* GetTexture(uint64_t texture_id) const;

    // Get ImTextureID for ImGui rendering
    ImTextureID GetImTextureID(uint64_t texture_id) const;

    // Get dimensions
    bool GetDimensions(uint64_t texture_id, int& width, int& height) const;

    // Touch (mark as recently used for LRU)
    void Touch(uint64_t texture_id);

    //=========================================================================
    // Texture Deletion
    //=========================================================================

    // Queue a texture for deletion (thread-safe, deferred to main thread)
    void QueueDelete(uint64_t texture_id);

    // Process queued deletions (call from main thread)
    void ProcessPendingDeletions();

    //=========================================================================
    // Memory Management
    //=========================================================================

    size_t GetUsedMemory() const;
    size_t GetMaxMemory() const { return max_memory_bytes_; }
    size_t GetTextureCount() const;

    // Evict least-recently-used textures to free memory
    void EvictToFitBytes(size_t required_bytes);

private:
    VulkanTexturePool();
    ~VulkanTexturePool();

    // Internal helpers
    VulkanTexture CreateVulkanTexture(int width, int height, VkFormat format);
    void UploadPixelsToTexture(VulkanTexture& tex, const void* pixel_data, size_t pixel_data_size);
    void DestroyTexture(VulkanTexture& tex);
    VkSampler GetOrCreateSampler();

    // Convert PixelFormat-related VkFormat to bytes per pixel
    static size_t FormatBytesPerPixel(VkFormat format);

    // Pool state
    bool initialized_ = false;
    size_t max_memory_bytes_ = 0;
    size_t used_memory_bytes_ = 0;

    // Texture storage: ID → texture
    uint64_t next_id_ = 1;
    std::unordered_map<uint64_t, VulkanTexture> textures_;

    // LRU tracking: front = oldest, back = newest
    std::deque<uint64_t> lru_order_;

    // Pending deletions (thread-safe queue)
    std::vector<uint64_t> pending_deletions_;
    mutable std::mutex deletion_mutex_;

    // Graveyard: textures removed from the pool but awaiting Vulkan resource destruction.
    // Kept alive for a few frames so ImGui draw lists don't reference freed descriptor sets.
    struct GraveyardEntry {
        VulkanTexture texture;
        uint64_t death_counter;  // ProcessPendingDeletions call count when added
    };
    std::vector<GraveyardEntry> graveyard_;
    uint64_t graveyard_counter_ = 0;
    static constexpr uint64_t kGraveyardDelayCalls = 30;  // ~5 frames

    // Shared sampler (linear filtering, clamp-to-edge)
    VkSampler shared_sampler_ = VK_NULL_HANDLE;

    mutable std::mutex mutex_;
};

} // namespace qcview

#endif // __linux__
