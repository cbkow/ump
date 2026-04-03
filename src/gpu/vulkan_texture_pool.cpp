#ifdef __linux__

#include "vulkan_texture_pool.h"
#include "vulkan_device.h"
#include "../utils/debug_utils.h"

#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cstring>

namespace qcview {

//=============================================================================
// Singleton
//=============================================================================

VulkanTexturePool& VulkanTexturePool::Instance() {
    static VulkanTexturePool instance;
    return instance;
}

VulkanTexturePool& VulkanTexturePool::ThumbnailInstance() {
    static VulkanTexturePool instance;
    return instance;
}

VulkanTexturePool::VulkanTexturePool() = default;

VulkanTexturePool::~VulkanTexturePool() {
    Shutdown();
}

//=============================================================================
// Lifecycle
//=============================================================================

bool VulkanTexturePool::Initialize(size_t max_memory_bytes) {
    if (initialized_) return true;

    auto& dev = VulkanDeviceManager::Instance();
    if (!dev.IsInitialized()) {
        Debug::Log("VulkanTexturePool: Device not initialized");
        return false;
    }

    max_memory_bytes_ = max_memory_bytes;
    used_memory_bytes_ = 0;
    next_id_ = 1;

    initialized_ = true;
    Debug::Log("VulkanTexturePool: Initialized (max " +
               std::to_string(max_memory_bytes / (1024 * 1024)) + " MB)");
    return true;
}

void VulkanTexturePool::Shutdown() {
    if (!initialized_) return;

    auto& dev = VulkanDeviceManager::Instance();
    if (dev.IsInitialized()) {
        vkDeviceWaitIdle(dev.GetDevice());
    }

    // Destroy all textures
    for (auto& [id, tex] : textures_) {
        DestroyTexture(tex);
    }
    textures_.clear();
    lru_order_.clear();

    // Destroy graveyard textures
    for (auto& entry : graveyard_) {
        DestroyTexture(entry.texture);
    }
    graveyard_.clear();

    // Destroy shared sampler
    if (shared_sampler_ != VK_NULL_HANDLE && dev.IsInitialized()) {
        vkDestroySampler(dev.GetDevice(), shared_sampler_, nullptr);
        shared_sampler_ = VK_NULL_HANDLE;
    }

    used_memory_bytes_ = 0;
    initialized_ = false;
    Debug::Log("VulkanTexturePool: Shutdown");
}

//=============================================================================
// Sampler
//=============================================================================

VkSampler VulkanTexturePool::GetOrCreateSampler() {
    if (shared_sampler_ != VK_NULL_HANDLE) return shared_sampler_;

    auto& dev = VulkanDeviceManager::Instance();

    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxAnisotropy = 1.0f;
    info.maxLod = 1.0f;

    vkCreateSampler(dev.GetDevice(), &info, nullptr, &shared_sampler_);
    return shared_sampler_;
}

//=============================================================================
// Helpers
//=============================================================================

size_t VulkanTexturePool::FormatBytesPerPixel(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            return 4;
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return 8;
        default:
            return 4;
    }
}

//=============================================================================
// Texture Creation (internal)
//=============================================================================

VulkanTexture VulkanTexturePool::CreateVulkanTexture(int width, int height, VkFormat format) {
    auto& dev = VulkanDeviceManager::Instance();
    VulkanTexture tex;
    tex.width = width;
    tex.height = height;
    tex.format = format;
    tex.byte_size = static_cast<size_t>(width) * height * FormatBytesPerPixel(format);

    // Create VkImage
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    VkResult result = vmaCreateImage(dev.GetAllocator(), &image_info, &alloc_info,
                                     &tex.image, &tex.allocation, nullptr);
    if (result != VK_SUCCESS) {
        Debug::Log("VulkanTexturePool: Failed to create image (" +
                   std::to_string(width) + "x" + std::to_string(height) + ")");
        return tex;
    }

    // Create VkImageView
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = tex.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;

    result = vkCreateImageView(dev.GetDevice(), &view_info, nullptr, &tex.image_view);
    if (result != VK_SUCCESS) {
        Debug::Log("VulkanTexturePool: Failed to create image view");
        vmaDestroyImage(dev.GetAllocator(), tex.image, tex.allocation);
        tex.image = VK_NULL_HANDLE;
        tex.allocation = VK_NULL_HANDLE;
        return tex;
    }

    // Get or create shared sampler
    tex.sampler = GetOrCreateSampler();

    // Register with ImGui — returns VkDescriptorSet usable as ImTextureID
    tex.descriptor_set = ImGui_ImplVulkan_AddTexture(
        tex.sampler, tex.image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    tex.is_valid = true;
    return tex;
}

void VulkanTexturePool::UploadPixelsToTexture(VulkanTexture& tex,
                                               const void* pixel_data,
                                               size_t pixel_data_size) {
    auto& dev = VulkanDeviceManager::Instance();

    // Create staging buffer
    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = pixel_data_size;
    buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo staging_alloc_info{};
    staging_alloc_info.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    staging_alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VmaAllocation staging_alloc = VK_NULL_HANDLE;
    VmaAllocationInfo staging_info{};

    VkResult result = vmaCreateBuffer(dev.GetAllocator(), &buf_info, &staging_alloc_info,
                                      &staging_buffer, &staging_alloc, &staging_info);
    if (result != VK_SUCCESS) {
        Debug::Log("VulkanTexturePool: Failed to create staging buffer");
        return;
    }

    // Copy pixel data to staging buffer
    std::memcpy(staging_info.pMappedData, pixel_data, pixel_data_size);

    // Record and execute transfer commands
    VkCommandBuffer cmd = dev.BeginSingleTimeCommands();

    // Transition image to TRANSFER_DST
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = tex.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Copy buffer to image
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {static_cast<uint32_t>(tex.width),
                          static_cast<uint32_t>(tex.height), 1};

    vkCmdCopyBufferToImage(cmd, staging_buffer, tex.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition image to SHADER_READ_ONLY
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    dev.EndSingleTimeCommands(cmd);

    // Destroy staging buffer
    vmaDestroyBuffer(dev.GetAllocator(), staging_buffer, staging_alloc);
}

void VulkanTexturePool::DestroyTexture(VulkanTexture& tex) {
    auto& dev = VulkanDeviceManager::Instance();
    if (!dev.IsInitialized()) return;

    if (tex.descriptor_set != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(tex.descriptor_set);
        tex.descriptor_set = VK_NULL_HANDLE;
    }
    if (tex.image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(dev.GetDevice(), tex.image_view, nullptr);
        tex.image_view = VK_NULL_HANDLE;
    }
    if (tex.image != VK_NULL_HANDLE) {
        vmaDestroyImage(dev.GetAllocator(), tex.image, tex.allocation);
        tex.image = VK_NULL_HANDLE;
        tex.allocation = VK_NULL_HANDLE;
    }
    tex.is_valid = false;
}

//=============================================================================
// Public API — Texture Creation
//=============================================================================

uint64_t VulkanTexturePool::CreateTextureFromPixels(
    int width, int height, VkFormat format,
    const void* pixel_data, size_t pixel_data_size)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return 0;

    // Evict if needed
    size_t needed = static_cast<size_t>(width) * height * FormatBytesPerPixel(format);
    if (used_memory_bytes_ + needed > max_memory_bytes_) {
        EvictToFitBytes(needed);
    }

    VulkanTexture tex = CreateVulkanTexture(width, height, format);
    if (!tex.is_valid) return 0;

    UploadPixelsToTexture(tex, pixel_data, pixel_data_size);

    uint64_t id = next_id_++;
    used_memory_bytes_ += tex.byte_size;
    textures_[id] = std::move(tex);
    lru_order_.push_back(id);

    return id;
}

uint64_t VulkanTexturePool::CreateTextureFromImage(
    VkImage source_image, int width, int height, VkFormat format)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return 0;

    size_t needed = static_cast<size_t>(width) * height * FormatBytesPerPixel(format);
    if (used_memory_bytes_ + needed > max_memory_bytes_) {
        EvictToFitBytes(needed);
    }

    VulkanTexture tex = CreateVulkanTexture(width, height, format);
    if (!tex.is_valid) return 0;

    auto& dev = VulkanDeviceManager::Instance();
    VkCommandBuffer cmd = dev.BeginSingleTimeCommands();

    // Transition source from SHADER_READ_ONLY (YUV render pass output) to TRANSFER_SRC
    VkImageMemoryBarrier src_barrier{};
    src_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    src_barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    src_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    src_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    src_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    src_barrier.image = source_image;
    src_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    src_barrier.subresourceRange.levelCount = 1;
    src_barrier.subresourceRange.layerCount = 1;
    src_barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    src_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    // Transition dest to TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier dst_barrier{};
    dst_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dst_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dst_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dst_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dst_barrier.image = tex.image;
    dst_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    dst_barrier.subresourceRange.levelCount = 1;
    dst_barrier.subresourceRange.layerCount = 1;
    dst_barrier.srcAccessMask = 0;
    dst_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    VkImageMemoryBarrier barriers[] = { src_barrier, dst_barrier };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 2, barriers);

    // Copy image to image
    VkImageCopy region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.layerCount = 1;
    region.extent = { static_cast<uint32_t>(width),
                      static_cast<uint32_t>(height), 1 };

    vkCmdCopyImage(cmd,
        source_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &region);

    // Transition dest to SHADER_READ_ONLY
    dst_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dst_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dst_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &dst_barrier);

    dev.EndSingleTimeCommands(cmd);

    uint64_t id = next_id_++;
    used_memory_bytes_ += tex.byte_size;
    textures_[id] = std::move(tex);
    lru_order_.push_back(id);

    return id;
}

uint64_t VulkanTexturePool::CreateEmptyTexture(int width, int height, VkFormat format) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return 0;

    size_t needed = static_cast<size_t>(width) * height * FormatBytesPerPixel(format);
    if (used_memory_bytes_ + needed > max_memory_bytes_) {
        EvictToFitBytes(needed);
    }

    VulkanTexture tex = CreateVulkanTexture(width, height, format);
    if (!tex.is_valid) return 0;

    // Transition to shader-read layout even though empty
    auto& dev = VulkanDeviceManager::Instance();
    VkCommandBuffer cmd = dev.BeginSingleTimeCommands();

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = tex.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    dev.EndSingleTimeCommands(cmd);

    uint64_t id = next_id_++;
    used_memory_bytes_ += tex.byte_size;
    textures_[id] = std::move(tex);
    lru_order_.push_back(id);

    return id;
}

bool VulkanTexturePool::UpdateTexture(uint64_t texture_id,
                                       const void* pixel_data,
                                       size_t pixel_data_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(texture_id);
    if (it == textures_.end()) return false;

    UploadPixelsToTexture(it->second, pixel_data, pixel_data_size);
    Touch(texture_id);
    return true;
}

bool VulkanTexturePool::UpdateTextureFromImage(uint64_t texture_id,
                                                VkImage source_image,
                                                int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(texture_id);
    if (it == textures_.end()) return false;

    VulkanTexture& tex = it->second;
    if (tex.width != width || tex.height != height) return false;

    auto& dev = VulkanDeviceManager::Instance();
    VkCommandBuffer cmd = dev.BeginSingleTimeCommands();

    // Transition source from SHADER_READ_ONLY (YUV render pass output) to TRANSFER_SRC
    VkImageMemoryBarrier src_barrier{};
    src_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    src_barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    src_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    src_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    src_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    src_barrier.image = source_image;
    src_barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    src_barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    src_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    // Transition dest to TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier dst_barrier{};
    dst_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dst_barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dst_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dst_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dst_barrier.image = tex.image;
    dst_barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    dst_barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dst_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    VkImageMemoryBarrier barriers[] = { src_barrier, dst_barrier };
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 2, barriers);

    VkImageCopy region{};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent = { static_cast<uint32_t>(width),
                      static_cast<uint32_t>(height), 1 };

    vkCmdCopyImage(cmd,
        source_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &region);

    // Transition dest back to SHADER_READ_ONLY
    dst_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dst_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dst_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &dst_barrier);

    dev.EndSingleTimeCommands(cmd);

    Touch(texture_id);
    return true;
}

//=============================================================================
// Public API — Texture Access
//=============================================================================

const VulkanTexture* VulkanTexturePool::GetTexture(uint64_t texture_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(texture_id);
    if (it == textures_.end()) return nullptr;
    return &it->second;
}

ImTextureID VulkanTexturePool::GetImTextureID(uint64_t texture_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(texture_id);
    if (it == textures_.end() || !it->second.is_valid) return ImTextureID{};
    return reinterpret_cast<ImTextureID>(it->second.descriptor_set);
}

bool VulkanTexturePool::GetDimensions(uint64_t texture_id, int& width, int& height) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(texture_id);
    if (it == textures_.end()) return false;
    width = it->second.width;
    height = it->second.height;
    return true;
}

void VulkanTexturePool::Touch(uint64_t texture_id) {
    // Move to back of LRU (most recently used)
    auto it = std::find(lru_order_.begin(), lru_order_.end(), texture_id);
    if (it != lru_order_.end()) {
        lru_order_.erase(it);
        lru_order_.push_back(texture_id);
    }
}

//=============================================================================
// Public API — Deletion
//=============================================================================

void VulkanTexturePool::QueueDelete(uint64_t texture_id) {
    std::lock_guard<std::mutex> lock(deletion_mutex_);
    pending_deletions_.push_back(texture_id);
}

void VulkanTexturePool::ProcessPendingDeletions() {
    // Step 1: Move pending textures from pool → graveyard
    // (immediately frees memory budget + LRU slot, but defers Vulkan resource destruction)
    std::vector<uint64_t> to_move;
    {
        std::lock_guard<std::mutex> lock(deletion_mutex_);
        to_move.swap(pending_deletions_);
    }

    if (!to_move.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint64_t id : to_move) {
            auto it = textures_.find(id);
            if (it != textures_.end()) {
                used_memory_bytes_ -= it->second.byte_size;
                graveyard_.push_back({std::move(it->second), graveyard_counter_});
                textures_.erase(it);

                auto lru_it = std::find(lru_order_.begin(), lru_order_.end(), id);
                if (lru_it != lru_order_.end()) {
                    lru_order_.erase(lru_it);
                }
            }
        }
    }

    // Step 2: Destroy graveyard entries that are old enough
    graveyard_counter_++;
    auto git = graveyard_.begin();
    while (git != graveyard_.end()) {
        if (graveyard_counter_ - git->death_counter >= kGraveyardDelayCalls) {
            DestroyTexture(git->texture);
            git = graveyard_.erase(git);
        } else {
            ++git;
        }
    }
}

//=============================================================================
// Memory Management
//=============================================================================

size_t VulkanTexturePool::GetUsedMemory() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return used_memory_bytes_;
}

size_t VulkanTexturePool::GetTextureCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return textures_.size();
}

void VulkanTexturePool::EvictToFitBytes(size_t required_bytes) {
    // Called with mutex_ held

    // Step 1: Drain pending deletions queue to free budget space.
    // These textures were QueueDelete'd but not yet processed, so they still
    // count in used_memory_bytes_. Processing them first avoids evicting
    // live textures (like thumbnails) unnecessarily.
    {
        std::vector<uint64_t> to_delete;
        {
            std::lock_guard<std::mutex> lock(deletion_mutex_);
            to_delete.swap(pending_deletions_);
        }
        for (uint64_t id : to_delete) {
            auto it = textures_.find(id);
            if (it != textures_.end()) {
                used_memory_bytes_ -= it->second.byte_size;
                // Move to graveyard so ImGui draw lists don't reference freed descriptor sets
                graveyard_.push_back({std::move(it->second), graveyard_counter_});
                textures_.erase(it);

                auto lru_it = std::find(lru_order_.begin(), lru_order_.end(), id);
                if (lru_it != lru_order_.end()) {
                    lru_order_.erase(lru_it);
                }
            }
        }
    }

    // Step 2: If still over budget, flush graveyard to reclaim GPU memory
    if (used_memory_bytes_ + required_bytes > max_memory_bytes_ && !graveyard_.empty()) {
        for (auto& entry : graveyard_) {
            DestroyTexture(entry.texture);
        }
        graveyard_.clear();
    }

    // Step 3: Only evict live textures as last resort
    while (used_memory_bytes_ + required_bytes > max_memory_bytes_ && !lru_order_.empty()) {
        uint64_t oldest_id = lru_order_.front();
        lru_order_.pop_front();

        auto it = textures_.find(oldest_id);
        if (it != textures_.end()) {
            Debug::Log("VulkanTexturePool: Evicting texture " + std::to_string(oldest_id) +
                       " (" + std::to_string(it->second.width) + "x" +
                       std::to_string(it->second.height) + ", " +
                       std::to_string(it->second.byte_size / 1024) + "KB)");
            used_memory_bytes_ -= it->second.byte_size;
            DestroyTexture(it->second);
            textures_.erase(it);
        }
    }
}

} // namespace qcview

#endif // __linux__
