#include "vulkan_device.h"

#ifdef __linux__

#include "../utils/debug_utils.h"
#include <cstring>
#include <set>
#include <algorithm>

// VMA implementation
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

namespace qcview {

//=============================================================================
// Debug callback
//=============================================================================

#ifndef NDEBUG
static VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
    void* user_data) {

    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        Debug::Log("Vulkan: " + std::string(callback_data->pMessage));
    }
    return VK_FALSE;
}
#endif

//=============================================================================
// Singleton
//=============================================================================

VulkanDeviceManager& VulkanDeviceManager::Instance() {
    static VulkanDeviceManager instance;
    return instance;
}

VulkanDeviceManager::VulkanDeviceManager() = default;

VulkanDeviceManager::~VulkanDeviceManager() {
    Shutdown();
}

//=============================================================================
// Initialization
//=============================================================================

bool VulkanDeviceManager::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_) {
        return true;
    }

    if (!CreateInstance()) {
        Debug::Log("VulkanDeviceManager: Failed to create Vulkan instance");
        return false;
    }

    if (!SelectPhysicalDevice()) {
        Debug::Log("VulkanDeviceManager: Failed to select physical device");
        Shutdown();
        return false;
    }

    if (!CreateLogicalDevice()) {
        Debug::Log("VulkanDeviceManager: Failed to create logical device");
        Shutdown();
        return false;
    }

    if (!CreateAllocator()) {
        Debug::Log("VulkanDeviceManager: Failed to create VMA allocator");
        Shutdown();
        return false;
    }

    if (!CreateDescriptorPool()) {
        Debug::Log("VulkanDeviceManager: Failed to create descriptor pool");
        Shutdown();
        return false;
    }

    if (!CreatePipelineCache()) {
        Debug::Log("VulkanDeviceManager: Failed to create pipeline cache");
        Shutdown();
        return false;
    }

    if (!CreateTransientCommandPool()) {
        Debug::Log("VulkanDeviceManager: Failed to create transient command pool");
        Shutdown();
        return false;
    }

    initialized_ = true;
    Debug::Log("VulkanDeviceManager: Initialized successfully");
    Debug::Log("  Device: " + device_name_);
    Debug::Log("  VRAM: " + std::to_string(dedicated_video_memory_ / (1024 * 1024)) + " MB");
    Debug::Log("  Graphics queue family: " + std::to_string(graphics_queue_family_));
    Debug::Log("  Compute queue family: " + std::to_string(compute_queue_family_));
    Debug::Log("  Transfer queue family: " + std::to_string(transfer_queue_family_));

    return true;
}

void VulkanDeviceManager::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    if (transient_command_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, transient_command_pool_, nullptr);
        transient_command_pool_ = VK_NULL_HANDLE;
    }

    if (pipeline_cache_ != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
        pipeline_cache_ = VK_NULL_HANDLE;
    }

    if (imgui_descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, imgui_descriptor_pool_, nullptr);
        imgui_descriptor_pool_ = VK_NULL_HANDLE;
    }

    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
        allocator_ = VK_NULL_HANDLE;
    }

    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

#ifndef NDEBUG
    if (debug_messenger_ != VK_NULL_HANDLE) {
        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT");
        if (func) {
            func(instance_, debug_messenger_, nullptr);
        }
        debug_messenger_ = VK_NULL_HANDLE;
    }
#endif

    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    physical_device_ = VK_NULL_HANDLE;
    graphics_queue_ = VK_NULL_HANDLE;
    compute_queue_ = VK_NULL_HANDLE;
    transfer_queue_ = VK_NULL_HANDLE;
    graphics_queue_family_ = UINT32_MAX;
    compute_queue_family_ = UINT32_MAX;
    transfer_queue_family_ = UINT32_MAX;
    initialized_ = false;

    Debug::Log("VulkanDeviceManager: Shutdown complete");
}

//=============================================================================
// Instance Creation
//=============================================================================

bool VulkanDeviceManager::CreateInstance() {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "QCView";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 3);
    app_info.pEngineName = "QCView Engine";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_2;

    // Required instance extensions
    std::vector<const char*> instance_extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        // GLFW will add its own surface extensions
    };

    // Add platform surface extension
    // GLFW handles this, but we need the Vulkan surface extensions
    // On Wayland: VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME
    // On X11: VK_KHR_XCB_SURFACE_EXTENSION_NAME or VK_KHR_XLIB_SURFACE_EXTENSION_NAME

    // Query available extensions first
    uint32_t available_ext_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &available_ext_count, nullptr);
    std::vector<VkExtensionProperties> available_extensions(available_ext_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &available_ext_count, available_extensions.data());

    auto has_instance_ext = [&](const char* name) {
        return std::any_of(available_extensions.begin(), available_extensions.end(),
            [name](const VkExtensionProperties& ext) {
                return strcmp(ext.extensionName, name) == 0;
            });
    };

    // Add Wayland/X11 surface extensions if available
    if (has_instance_ext("VK_KHR_wayland_surface")) {
        instance_extensions.push_back("VK_KHR_wayland_surface");
    }
    if (has_instance_ext("VK_KHR_xcb_surface")) {
        instance_extensions.push_back("VK_KHR_xcb_surface");
    }
    if (has_instance_ext("VK_KHR_xlib_surface")) {
        instance_extensions.push_back("VK_KHR_xlib_surface");
    }

    // External memory extensions (needed for DMA-BUF import in Phase 3)
    if (has_instance_ext(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME)) {
        instance_extensions.push_back(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
    }
    if (has_instance_ext(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
        instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    }

    // Validation layers in debug builds
    std::vector<const char*> validation_layers;
#ifndef NDEBUG
    // Check if validation layer is available
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> available_layers(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, available_layers.data());

    bool has_validation = std::any_of(available_layers.begin(), available_layers.end(),
        [](const VkLayerProperties& layer) {
            return strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        });

    if (has_validation) {
        validation_layers.push_back("VK_LAYER_KHRONOS_validation");
        if (has_instance_ext(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
    }
#endif

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<uint32_t>(instance_extensions.size());
    create_info.ppEnabledExtensionNames = instance_extensions.data();
    create_info.enabledLayerCount = static_cast<uint32_t>(validation_layers.size());
    create_info.ppEnabledLayerNames = validation_layers.data();

    VkResult result = vkCreateInstance(&create_info, nullptr, &instance_);
    if (result != VK_SUCCESS) {
        Debug::Log("VulkanDeviceManager: vkCreateInstance failed (result=" +
                   std::to_string(result) + ")");
        return false;
    }

#ifndef NDEBUG
    // Setup debug messenger
    if (has_validation && has_instance_ext(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        VkDebugUtilsMessengerCreateInfoEXT debug_info{};
        debug_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debug_info.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug_info.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug_info.pfnUserCallback = VulkanDebugCallback;

        auto func = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT");
        if (func) {
            func(instance_, &debug_info, nullptr, &debug_messenger_);
        }
    }
#endif

    Debug::Log("VulkanDeviceManager: Vulkan instance created");
    return true;
}

//=============================================================================
// Physical Device Selection
//=============================================================================

bool VulkanDeviceManager::SelectPhysicalDevice() {
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
    if (device_count == 0) {
        Debug::Log("VulkanDeviceManager: No Vulkan-capable GPUs found");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());

    // Prefer discrete GPU, fall back to integrated
    VkPhysicalDevice best_device = VK_NULL_HANDLE;
    int best_score = -1;

    for (auto dev : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);

        // Check queue families
        uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &queue_family_count, queue_families.data());

        bool has_graphics = false;
        for (const auto& qf : queue_families) {
            if (qf.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                has_graphics = true;
                break;
            }
        }

        if (!has_graphics) continue;

        int score = 0;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 100;

        // Prefer higher API version
        score += VK_API_VERSION_MINOR(props.apiVersion) * 10;

        if (score > best_score) {
            best_score = score;
            best_device = dev;
        }
    }

    if (best_device == VK_NULL_HANDLE) {
        Debug::Log("VulkanDeviceManager: No suitable GPU found");
        return false;
    }

    physical_device_ = best_device;
    vkGetPhysicalDeviceProperties(physical_device_, &device_properties_);
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties_);

    device_name_ = device_properties_.deviceName;

    // Calculate dedicated VRAM
    for (uint32_t i = 0; i < memory_properties_.memoryHeapCount; ++i) {
        if (memory_properties_.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            dedicated_video_memory_ = std::max(dedicated_video_memory_,
                static_cast<size_t>(memory_properties_.memoryHeaps[i].size));
        }
    }

    Debug::Log("VulkanDeviceManager: Selected GPU: " + device_name_);
    return true;
}

//=============================================================================
// Logical Device Creation
//=============================================================================

bool VulkanDeviceManager::CreateLogicalDevice() {
    // Find queue families
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, queue_families.data());

    // Find graphics queue family
    for (uint32_t i = 0; i < queue_family_count; ++i) {
        if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphics_queue_family_ = i;
            break;
        }
    }

    // Find dedicated compute queue (prefer one that's not graphics)
    compute_queue_family_ = graphics_queue_family_;  // fallback
    for (uint32_t i = 0; i < queue_family_count; ++i) {
        if ((queue_families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            !(queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            compute_queue_family_ = i;
            break;
        }
    }

    // Find dedicated transfer queue (prefer one that's not graphics or compute)
    transfer_queue_family_ = graphics_queue_family_;  // fallback
    for (uint32_t i = 0; i < queue_family_count; ++i) {
        if ((queue_families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
            !(queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            !(queue_families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            transfer_queue_family_ = i;
            break;
        }
    }

    // Collect unique queue families
    std::set<uint32_t> unique_families = {
        graphics_queue_family_,
        compute_queue_family_,
        transfer_queue_family_
    };

    float queue_priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queue_create_infos;
    for (uint32_t family : unique_families) {
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;
        queue_create_infos.push_back(queue_info);
    }

    // Device extensions
    std::vector<const char*> device_extensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    // Query available device extensions
    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> available_extensions(ext_count);
    vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &ext_count, available_extensions.data());

    auto has_device_ext = [&](const char* name) {
        return std::any_of(available_extensions.begin(), available_extensions.end(),
            [name](const VkExtensionProperties& ext) {
                return strcmp(ext.extensionName, name) == 0;
            });
    };

    // DMA-BUF import extensions (Phase 3 - enable now if available)
    if (has_device_ext(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
        device_extensions.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    }
    if (has_device_ext(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) {
        device_extensions.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    }
    if (has_device_ext(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME)) {
        device_extensions.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    }
    if (has_device_ext(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME)) {
        device_extensions.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
    }
    if (has_device_ext(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) {
        device_extensions.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    }
    if (has_device_ext(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME)) {
        device_extensions.push_back(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
    }

    // Maintenance extensions
    if (has_device_ext(VK_KHR_MAINTENANCE1_EXTENSION_NAME)) {
        device_extensions.push_back(VK_KHR_MAINTENANCE1_EXTENSION_NAME);
    }

    // HDR swapchain extensions (Phase 6)
    if (has_device_ext(VK_EXT_HDR_METADATA_EXTENSION_NAME)) {
        device_extensions.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
    }
    if (has_device_ext(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
        device_extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    }

    // Store enabled extensions for query
    for (const char* ext : device_extensions) {
        enabled_device_extensions_.push_back(ext);
    }

    // Device features
    VkPhysicalDeviceFeatures device_features{};
    device_features.samplerAnisotropy = VK_TRUE;
    device_features.fillModeNonSolid = VK_TRUE;  // For wireframe debug

    VkDeviceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount = static_cast<uint32_t>(queue_create_infos.size());
    create_info.pQueueCreateInfos = queue_create_infos.data();
    create_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
    create_info.ppEnabledExtensionNames = device_extensions.data();
    create_info.pEnabledFeatures = &device_features;

    VkResult result = vkCreateDevice(physical_device_, &create_info, nullptr, &device_);
    if (result != VK_SUCCESS) {
        Debug::Log("VulkanDeviceManager: vkCreateDevice failed (result=" +
                   std::to_string(result) + ")");
        return false;
    }

    // Get queue handles
    vkGetDeviceQueue(device_, graphics_queue_family_, 0, &graphics_queue_);
    vkGetDeviceQueue(device_, compute_queue_family_, 0, &compute_queue_);
    vkGetDeviceQueue(device_, transfer_queue_family_, 0, &transfer_queue_);

    Debug::Log("VulkanDeviceManager: Logical device created");
    return true;
}

//=============================================================================
// VMA Allocator
//=============================================================================

bool VulkanDeviceManager::CreateAllocator() {
    VmaAllocatorCreateInfo alloc_info{};
    alloc_info.vulkanApiVersion = VK_API_VERSION_1_2;
    alloc_info.physicalDevice = physical_device_;
    alloc_info.device = device_;
    alloc_info.instance = instance_;

    VkResult result = vmaCreateAllocator(&alloc_info, &allocator_);
    if (result != VK_SUCCESS) {
        Debug::Log("VulkanDeviceManager: vmaCreateAllocator failed (result=" +
                   std::to_string(result) + ")");
        return false;
    }

    Debug::Log("VulkanDeviceManager: VMA allocator created");
    return true;
}

//=============================================================================
// Descriptor Pool (for ImGui + texture display)
//=============================================================================

bool VulkanDeviceManager::CreateDescriptorPool() {
    // Pool sizes for ImGui and our texture descriptors
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 100 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 100 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 100 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 100 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 100 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 100 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 100 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 100 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 100 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 100 },
    };

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 10000;
    pool_info.poolSizeCount = static_cast<uint32_t>(std::size(pool_sizes));
    pool_info.pPoolSizes = pool_sizes;

    VkResult result = vkCreateDescriptorPool(device_, &pool_info, nullptr, &imgui_descriptor_pool_);
    if (result != VK_SUCCESS) {
        Debug::Log("VulkanDeviceManager: vkCreateDescriptorPool failed");
        return false;
    }

    return true;
}

//=============================================================================
// Pipeline Cache
//=============================================================================

bool VulkanDeviceManager::CreatePipelineCache() {
    VkPipelineCacheCreateInfo cache_info{};
    cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

    VkResult result = vkCreatePipelineCache(device_, &cache_info, nullptr, &pipeline_cache_);
    if (result != VK_SUCCESS) {
        Debug::Log("VulkanDeviceManager: vkCreatePipelineCache failed");
        return false;
    }

    return true;
}

//=============================================================================
// Transient Command Pool
//=============================================================================

bool VulkanDeviceManager::CreateTransientCommandPool() {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = graphics_queue_family_;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                      VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkResult result = vkCreateCommandPool(device_, &pool_info, nullptr, &transient_command_pool_);
    if (result != VK_SUCCESS) {
        Debug::Log("VulkanDeviceManager: Failed to create transient command pool");
        return false;
    }

    return true;
}

//=============================================================================
// Command Pool & Buffer Helpers
//=============================================================================

VkCommandPool VulkanDeviceManager::CreateCommandPool(uint32_t queue_family,
                                                       VkCommandPoolCreateFlags flags) {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = queue_family;
    pool_info.flags = flags;

    VkCommandPool pool = VK_NULL_HANDLE;
    VkResult result = vkCreateCommandPool(device_, &pool_info, nullptr, &pool);
    if (result != VK_SUCCESS) {
        Debug::Log("VulkanDeviceManager: CreateCommandPool failed");
        return VK_NULL_HANDLE;
    }

    return pool;
}

VkCommandBuffer VulkanDeviceManager::BeginSingleTimeCommands() {
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandPool = transient_command_pool_;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &alloc_info, &cmd);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(cmd, &begin_info);
    return cmd;
}

void VulkanDeviceManager::EndSingleTimeCommands(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);

    VkFence fence = CreateFence(false);

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;

    vkQueueSubmit(graphics_queue_, 1, &submit_info, fence);
    vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);

    vkDestroyFence(device_, fence, nullptr);
    vkFreeCommandBuffers(device_, transient_command_pool_, 1, &cmd);
}

VkFence VulkanDeviceManager::EndSingleTimeCommandsFenced(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);

    VkFence fence = CreateFence(false);

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;

    vkQueueSubmit(graphics_queue_, 1, &submit_info, fence);

    // Caller is responsible for waiting on fence, then:
    //   vkWaitForFences(device, 1, &fence, VK_TRUE, timeout);
    //   vkDestroyFence(device, fence, nullptr);
    //   vkFreeCommandBuffers(device, pool, 1, &cmd);
    return fence;
}

void VulkanDeviceManager::EndSingleTimeCommandsAsync(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);

    // Free the previous async command buffer — the GPU is guaranteed to be done
    // with it since commands are serialized on the same queue.
    if (prev_async_cmd_ != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device_, transient_command_pool_, 1, &prev_async_cmd_);
    }

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;

    vkQueueSubmit(graphics_queue_, 1, &submit_info, VK_NULL_HANDLE);

    prev_async_cmd_ = cmd;
}

//=============================================================================
// Descriptor Set Allocation
//=============================================================================

VkDescriptorSet VulkanDeviceManager::AllocateTextureDescriptorSet(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = imgui_descriptor_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &layout;

    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkResult result = vkAllocateDescriptorSets(device_, &alloc_info, &descriptor_set);
    if (result != VK_SUCCESS) {
        Debug::Log("VulkanDeviceManager: Failed to allocate descriptor set");
        return VK_NULL_HANDLE;
    }

    return descriptor_set;
}

//=============================================================================
// Extension Query
//=============================================================================

bool VulkanDeviceManager::HasExtension(const std::string& extension_name) const {
    return std::any_of(enabled_device_extensions_.begin(), enabled_device_extensions_.end(),
        [&](const std::string& ext) { return ext == extension_name; });
}

bool VulkanDeviceManager::HasDMABufImport() const {
    return HasExtension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
           HasExtension(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
           HasExtension(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
}

//=============================================================================
// Debug/Info
//=============================================================================

std::string VulkanDeviceManager::GetDeviceName() const {
    return device_name_;
}

std::string VulkanDeviceManager::GetDriverVersion() const {
    uint32_t version = device_properties_.driverVersion;
    // NVIDIA uses a different encoding
    if (device_properties_.vendorID == 0x10DE) {
        return std::to_string((version >> 22) & 0x3FF) + "." +
               std::to_string((version >> 14) & 0xFF) + "." +
               std::to_string((version >> 6) & 0xFF) + "." +
               std::to_string(version & 0x3F);
    }
    return std::to_string(VK_API_VERSION_MAJOR(version)) + "." +
           std::to_string(VK_API_VERSION_MINOR(version)) + "." +
           std::to_string(VK_API_VERSION_PATCH(version));
}

//=============================================================================
// Synchronization Helpers
//=============================================================================

VkFence VulkanDeviceManager::CreateFence(bool signaled) {
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (signaled) {
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    }

    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(device_, &fence_info, nullptr, &fence);
    return fence;
}

VkSemaphore VulkanDeviceManager::CreateSemaphore() {
    VkSemaphoreCreateInfo sem_info{};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkSemaphore semaphore = VK_NULL_HANDLE;
    vkCreateSemaphore(device_, &sem_info, nullptr, &semaphore);
    return semaphore;
}

void VulkanDeviceManager::WaitForFence(VkFence fence, uint64_t timeout) {
    vkWaitForFences(device_, 1, &fence, VK_TRUE, timeout);
}

void VulkanDeviceManager::ResetFence(VkFence fence) {
    vkResetFences(device_, 1, &fence);
}

} // namespace qcview

#endif // __linux__
