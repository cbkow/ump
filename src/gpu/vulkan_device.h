#pragma once

#ifdef __linux__

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <mutex>
#include <string>
#include <vector>
#include <functional>

namespace qcview {

//=============================================================================
// VulkanDeviceManager
//
// Centralized Vulkan device and memory management for the entire application.
// Singleton pattern ensures all components share the same device.
// Mirrors D3D11DeviceManager interface where applicable.
//
// Usage:
//   auto& mgr = VulkanDeviceManager::Instance();
//   if (mgr.Initialize()) {
//       auto device = mgr.GetDevice();
//       auto allocator = mgr.GetAllocator();
//       // Create resources...
//   }
//=============================================================================

class VulkanDeviceManager {
public:
    static VulkanDeviceManager& Instance();

    // Delete copy/move
    VulkanDeviceManager(const VulkanDeviceManager&) = delete;
    VulkanDeviceManager& operator=(const VulkanDeviceManager&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    bool Initialize();
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Device Access
    //=========================================================================

    VkInstance GetInstance() const { return instance_; }
    VkPhysicalDevice GetPhysicalDevice() const { return physical_device_; }
    VkDevice GetDevice() const { return device_; }
    VmaAllocator GetAllocator() const { return allocator_; }

    //=========================================================================
    // Queue Access
    //=========================================================================

    VkQueue GetGraphicsQueue() const { return graphics_queue_; }
    VkQueue GetComputeQueue() const { return compute_queue_; }
    VkQueue GetTransferQueue() const { return transfer_queue_; }
    uint32_t GetGraphicsQueueFamily() const { return graphics_queue_family_; }
    uint32_t GetComputeQueueFamily() const { return compute_queue_family_; }
    uint32_t GetTransferQueueFamily() const { return transfer_queue_family_; }

    //=========================================================================
    // Command Pool & Buffer Helpers
    //=========================================================================

    // Create a command pool for the given queue family
    VkCommandPool CreateCommandPool(uint32_t queue_family,
                                     VkCommandPoolCreateFlags flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

    // Begin a single-use command buffer (from a transient pool)
    VkCommandBuffer BeginSingleTimeCommands();

    // End and submit a single-use command buffer, wait for completion
    void EndSingleTimeCommands(VkCommandBuffer cmd);

    // End and submit with a fence (doesn't block). Caller must wait on fence
    // and free the command buffer after the fence signals.
    VkFence EndSingleTimeCommandsFenced(VkCommandBuffer cmd);

    // End and submit without waiting (fire-and-forget).
    // The command buffer from the previous call is freed (GPU is done with it
    // since commands are serialized on the same queue).
    void EndSingleTimeCommandsAsync(VkCommandBuffer cmd);

    //=========================================================================
    // Descriptor Pool
    //=========================================================================

    VkDescriptorPool GetImGuiDescriptorPool() const { return imgui_descriptor_pool_; }

    // Allocate a descriptor set for a texture (for ImGui texture display)
    VkDescriptorSet AllocateTextureDescriptorSet(VkDescriptorSetLayout layout);

    //=========================================================================
    // Pipeline Cache
    //=========================================================================

    VkPipelineCache GetPipelineCache() const { return pipeline_cache_; }

    //=========================================================================
    // Extension Query
    //=========================================================================

    bool HasExtension(const std::string& extension_name) const;

    // DMA-BUF import extensions (Phase 3)
    bool HasDMABufImport() const;

    //=========================================================================
    // Debug/Info
    //=========================================================================

    std::string GetDeviceName() const;
    std::string GetDriverVersion() const;
    size_t GetDedicatedVideoMemory() const { return dedicated_video_memory_; }

    //=========================================================================
    // Synchronization Helpers
    //=========================================================================

    VkFence CreateFence(bool signaled = false);
    VkSemaphore CreateSemaphore();

    // Wait for and reset a fence
    void WaitForFence(VkFence fence, uint64_t timeout = UINT64_MAX);
    void ResetFence(VkFence fence);

private:
    VulkanDeviceManager();
    ~VulkanDeviceManager();

    bool CreateInstance();
    bool SelectPhysicalDevice();
    bool CreateLogicalDevice();
    bool CreateAllocator();
    bool CreateDescriptorPool();
    bool CreatePipelineCache();
    bool CreateTransientCommandPool();

    // Instance
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;

    // Physical device
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties device_properties_{};
    VkPhysicalDeviceMemoryProperties memory_properties_{};

    // Logical device
    VkDevice device_ = VK_NULL_HANDLE;

    // Queues
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue compute_queue_ = VK_NULL_HANDLE;
    VkQueue transfer_queue_ = VK_NULL_HANDLE;
    uint32_t graphics_queue_family_ = UINT32_MAX;
    uint32_t compute_queue_family_ = UINT32_MAX;
    uint32_t transfer_queue_family_ = UINT32_MAX;

    // Memory allocator
    VmaAllocator allocator_ = VK_NULL_HANDLE;

    // Descriptor pool (for ImGui textures)
    VkDescriptorPool imgui_descriptor_pool_ = VK_NULL_HANDLE;

    // Pipeline cache
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;

    // Transient command pool (for single-use commands on graphics queue)
    VkCommandPool transient_command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer prev_async_cmd_ = VK_NULL_HANDLE;  // Previous fire-and-forget cmd (freed on next async submit)

    // Device info
    std::string device_name_;
    size_t dedicated_video_memory_ = 0;
    std::vector<std::string> enabled_device_extensions_;

    bool initialized_ = false;

    mutable std::mutex mutex_;
};

} // namespace qcview

#endif // __linux__
