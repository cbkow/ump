#include "vulkan_device_manager.h"

#include <QString>
#include <QtLogging>

#include <algorithm>
#include <cstring>
#include <vector>

namespace qcv {

namespace {

constexpr uint32_t kApiVersion = VK_API_VERSION_1_3;

#ifndef NDEBUG
constexpr bool kEnableValidation = true;
#else
constexpr bool kEnableValidation = false;
#endif

VKAPI_ATTR VkBool32 VKAPI_CALL debugMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT *data,
    void * /*userData*/)
{
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        qCritical("Vulkan validation [ERROR] %s", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        qWarning("Vulkan validation [WARN] %s", data->pMessage);
    }
    return VK_FALSE;
}

bool layerAvailable(const char *name)
{
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const auto &l : layers) {
        if (std::strcmp(l.layerName, name) == 0) return true;
    }
    return false;
}

bool extensionAvailable(const char *name,
                          const std::vector<VkExtensionProperties> &exts)
{
    for (const auto &e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

} // namespace

VulkanDeviceManager &VulkanDeviceManager::instance()
{
    static VulkanDeviceManager s;
    return s;
}

VulkanDeviceManager::VulkanDeviceManager()  = default;
VulkanDeviceManager::~VulkanDeviceManager() { shutdown(); }

bool VulkanDeviceManager::initialize()
{
    std::lock_guard lock(m_mutex);
    if (m_initialized) return true;

    if (!createInstance()) {
        qCritical("VulkanDeviceManager: instance creation failed");
        return false;
    }
    if (!selectPhysicalDevice() || !createLogicalDevice()) {
        destroyDevice();
        destroyInstance();
        return false;
    }

    m_initialized = true;
    // Phase I.B — bump the generation counter on every successful
    // (re)create. Renderer-side bridges can read it to detect when
    // their cached Vulkan refs (VkImage handles, samplers, etc.)
    // are stale. Stays at 1 across the session until I.C ships
    // recreateDevice().
    m_deviceGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_deviceLost.store(false, std::memory_order_release);
    qInfo("VulkanDeviceManager: initialized — %s (api %u.%u.%u)",
          m_deviceProps.deviceName,
          VK_VERSION_MAJOR(m_deviceProps.apiVersion),
          VK_VERSION_MINOR(m_deviceProps.apiVersion),
          VK_VERSION_PATCH(m_deviceProps.apiVersion));
    return true;
}

void VulkanDeviceManager::markDeviceLost()
{
    // Sticky one-way flag. exchange returns the prior value — log
    // the first transition only, so a cascade of N FFmpeg DEVICE_LOST
    // messages produces one log line, not N.
    const bool wasLost = m_deviceLost.exchange(true,
                                                std::memory_order_acq_rel);
    if (!wasLost) {
        qCritical("VulkanDeviceManager: VK_ERROR_DEVICE_LOST observed — "
                  "hwaccel disabled for the rest of this session "
                  "(restart to recover; auto-recovery is Phase I.C)");
    }
}

void VulkanDeviceManager::shutdown()
{
    std::lock_guard lock(m_mutex);
    if (!m_initialized) return;

    waitForGpu();
    destroyDevice();
    destroyInstance();

    m_graphicsFamily = m_computeFamily = m_transferFamily = UINT32_MAX;
    m_videoDecodeFamily   = UINT32_MAX;
    m_videoDecodeCodecOps = 0;
    m_graphicsQueue = m_computeQueue = m_transferQueue = VK_NULL_HANDLE;
    m_videoDecodeQueue = VK_NULL_HANDLE;
    m_deviceProps    = {};
    m_enabledDeviceExtensions.clear();
    m_initialized    = false;
}

void VulkanDeviceManager::waitForGpu()
{
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
    }
}

const char *VulkanDeviceManager::deviceName() const
{
    return m_deviceProps.deviceName;
}

// ---------------------------------------------------------------------
// createInstance
// ---------------------------------------------------------------------
bool VulkanDeviceManager::createInstance()
{
    VkApplicationInfo app{};
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "QCView";
    app.applicationVersion = VK_MAKE_VERSION(2, 0, 0);
    app.pEngineName        = "QCView";
    app.engineVersion      = VK_MAKE_VERSION(2, 0, 0);
    app.apiVersion         = kApiVersion;

    // Decode-only path: NO surface / swapchain instance extensions.
    // Debug-utils is the only instance extension we want, and only
    // when validation is enabled.
    std::vector<const char *> exts;
    std::vector<const char *> layers;
    if (kEnableValidation && layerAvailable("VK_LAYER_KHRONOS_validation")) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &app;
    ci.enabledLayerCount       = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames     = layers.data();
    ci.enabledExtensionCount   = static_cast<uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();

    VkResult r = vkCreateInstance(&ci, nullptr, &m_instance);
    if (r != VK_SUCCESS) {
        qCritical("vkCreateInstance failed: %d", static_cast<int>(r));
        return false;
    }

    if (kEnableValidation && !layers.empty()) {
        auto pfnCreate = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        if (pfnCreate) {
            VkDebugUtilsMessengerCreateInfoEXT dci{};
            dci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            dci.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dci.messageType =
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dci.pfnUserCallback = debugMessengerCallback;
            pfnCreate(m_instance, &dci, nullptr, &m_debugMessenger);
        }
    }
    return true;
}

// ---------------------------------------------------------------------
// selectPhysicalDevice (no surface, no swapchain requirement)
// ---------------------------------------------------------------------
bool VulkanDeviceManager::selectPhysicalDevice()
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) {
        qCritical("VulkanDeviceManager: no Vulkan-capable physical devices");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    auto scoreDevice = [&](VkPhysicalDevice d) -> int {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);

        // Require Vulkan 1.2 (timeline semaphores, etc.).
        if (VK_VERSION_MAJOR(p.apiVersion) < 1 ||
            (VK_VERSION_MAJOR(p.apiVersion) == 1 && VK_VERSION_MINOR(p.apiVersion) < 2)) {
            return -1;
        }

        // Require a graphics queue family (FFmpeg's Vulkan hwaccel
        // uses graphics queue for inter-frame work even when video
        // decode happens on the dedicated decode queue).
        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qfCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qfCount);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qfCount, qfs.data());
        bool hasGraphics = false;
        for (const auto &qf : qfs) {
            if (qf.queueFlags & VK_QUEUE_GRAPHICS_BIT) { hasGraphics = true; break; }
        }
        if (!hasGraphics) return -1;

        int score = 0;
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)   score += 1000;
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 100;
        score += static_cast<int>(p.limits.maxImageDimension2D / 1024);
        return score;
    };

    int           bestScore = -1;
    VkPhysicalDevice best   = VK_NULL_HANDLE;
    for (auto d : devices) {
        const int s = scoreDevice(d);
        if (s > bestScore) {
            bestScore = s;
            best      = d;
        }
    }
    if (best == VK_NULL_HANDLE) {
        qCritical("VulkanDeviceManager: no physical device meets requirements");
        return false;
    }

    m_physicalDevice = best;
    vkGetPhysicalDeviceProperties(m_physicalDevice, &m_deviceProps);

    // Resolve queue families with the *2 query so we can chain
    // VkQueueFamilyVideoPropertiesKHR and pick up the codec-operation
    // bitfield for video-decode-capable queue families.
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties2(m_physicalDevice, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties2>          qfs2(qfCount);
    std::vector<VkQueueFamilyVideoPropertiesKHR>   videoProps(qfCount);
    for (uint32_t i = 0; i < qfCount; ++i) {
        videoProps[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
        videoProps[i].pNext = nullptr;
        qfs2[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        qfs2[i].pNext = &videoProps[i];
    }
    vkGetPhysicalDeviceQueueFamilyProperties2(m_physicalDevice, &qfCount, qfs2.data());

    for (uint32_t i = 0; i < qfCount; ++i) {
        const auto flags = qfs2[i].queueFamilyProperties.queueFlags;
        if (m_graphicsFamily == UINT32_MAX && (flags & VK_QUEUE_GRAPHICS_BIT)) {
            m_graphicsFamily = i;
        }
        if ((flags & VK_QUEUE_COMPUTE_BIT) && !(flags & VK_QUEUE_GRAPHICS_BIT)
            && m_computeFamily == UINT32_MAX) {
            m_computeFamily = i;
        }
        if ((flags & VK_QUEUE_TRANSFER_BIT)
            && !(flags & VK_QUEUE_GRAPHICS_BIT)
            && !(flags & VK_QUEUE_COMPUTE_BIT)
            && m_transferFamily == UINT32_MAX) {
            m_transferFamily = i;
        }
        if ((flags & VK_QUEUE_VIDEO_DECODE_BIT_KHR)
            && m_videoDecodeFamily == UINT32_MAX) {
            m_videoDecodeFamily   = i;
            m_videoDecodeCodecOps = videoProps[i].videoCodecOperations;
        }
    }
    if (m_computeFamily  == UINT32_MAX) m_computeFamily  = m_graphicsFamily;
    if (m_transferFamily == UINT32_MAX) m_transferFamily = m_graphicsFamily;

    qInfo("VulkanDeviceManager: queues — graphics=%u compute=%u transfer=%u "
          "video_decode=%s (codec_ops=0x%x)",
          m_graphicsFamily, m_computeFamily, m_transferFamily,
          m_videoDecodeFamily == UINT32_MAX
              ? "unavailable"
              : QString::number(m_videoDecodeFamily).toUtf8().constData(),
          m_videoDecodeCodecOps);
    return true;
}

// ---------------------------------------------------------------------
// createLogicalDevice (FFmpeg hybrid extension list + features chain)
// ---------------------------------------------------------------------
bool VulkanDeviceManager::createLogicalDevice()
{
    const float prio = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueCis;
    auto pushQueue = [&](uint32_t family) {
        if (family == UINT32_MAX) return;
        for (const auto &q : queueCis) {
            if (q.queueFamilyIndex == family) return;
        }
        VkDeviceQueueCreateInfo qci{};
        qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = family;
        qci.queueCount       = 1;
        qci.pQueuePriorities = &prio;
        queueCis.push_back(qci);
    };
    pushQueue(m_graphicsFamily);
    pushQueue(m_computeFamily);
    pushQueue(m_transferFamily);
    pushQueue(m_videoDecodeFamily);

    // FFmpeg's hybrid device extension list. Add each only if the
    // physical device advertises it; FFmpeg gracefully degrades when
    // optional extensions are missing. Two of these
    // (external_memory_win32 + external_semaphore_win32) are also the
    // primitives the renderer-side bridge uses for the Vulkan→D3D11
    // NT-handle handoff in F.2.4.3.
    std::vector<const char *> deviceExts;
    {
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> avail(extCount);
        vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, avail.data());

        constexpr const char *kFfmpegHybridExts[] = {
            "VK_KHR_push_descriptor",
            "VK_EXT_shader_atomic_float",
            "VK_KHR_cooperative_matrix",
            "VK_KHR_shader_subgroup_rotate",
            "VK_EXT_host_image_copy",
            "VK_KHR_workgroup_memory_explicit_layout",
            "VK_KHR_shader_expect_assume",
            "VK_EXT_external_memory_host",
            "VK_KHR_external_memory_win32",
            "VK_KHR_external_semaphore_win32",
            "VK_KHR_video_queue",
            "VK_KHR_video_decode_queue",
            "VK_KHR_video_decode_h264",
            "VK_KHR_video_decode_h265",
            "VK_KHR_video_decode_av1",
        };
        for (const char *want : kFfmpegHybridExts) {
            if (extensionAvailable(want, avail)) {
                deviceExts.push_back(want);
            } else {
                qInfo("VulkanDeviceManager: optional ext %s not available", want);
            }
        }
    }

    // VkPhysicalDeviceFeatures2 chain covering 1.1/1.2/1.3 core
    // features + per-extension feature structs FFmpeg's Vulkan
    // hwaccel walks. Query first to fill the booleans with what
    // the device actually supports, then pass the same chain
    // back into vkCreateDevice. Conditional structs are chained
    // only when the corresponding extension is enabled — otherwise
    // vkCreateDevice rejects them.
    auto isExtEnabled = [&](const char *name) -> bool {
        for (const char *e : deviceExts) {
            if (std::strcmp(e, name) == 0) return true;
        }
        return false;
    };

    VkPhysicalDeviceVulkan11Features v11Features{};
    v11Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceVulkan12Features v12Features{};
    v12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceVulkan13Features v13Features{};
    v13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomicFloatFeatures{};
    atomicFloatFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coopMatFeatures{};
    coopMatFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;
    VkPhysicalDeviceShaderSubgroupRotateFeaturesKHR subgroupRotateFeatures{};
    subgroupRotateFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_ROTATE_FEATURES_KHR;
    VkPhysicalDeviceHostImageCopyFeaturesEXT hostImageCopyFeatures{};
    hostImageCopyFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT;
    VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wgMemLayoutFeatures{};
    wgMemLayoutFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR;
    VkPhysicalDeviceShaderExpectAssumeFeaturesKHR expectAssumeFeatures{};
    expectAssumeFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_EXPECT_ASSUME_FEATURES_KHR;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

    void **pNext = &features2.pNext;
    auto chain = [&](void *next, void *nextChain) {
        *pNext = next;
        pNext  = static_cast<void **>(nextChain);
    };

    chain(&v11Features, &v11Features.pNext);
    chain(&v12Features, &v12Features.pNext);
    chain(&v13Features, &v13Features.pNext);
    if (isExtEnabled("VK_EXT_shader_atomic_float"))
        chain(&atomicFloatFeatures, &atomicFloatFeatures.pNext);
    if (isExtEnabled("VK_KHR_cooperative_matrix"))
        chain(&coopMatFeatures, &coopMatFeatures.pNext);
    if (isExtEnabled("VK_KHR_shader_subgroup_rotate"))
        chain(&subgroupRotateFeatures, &subgroupRotateFeatures.pNext);
    if (isExtEnabled("VK_EXT_host_image_copy"))
        chain(&hostImageCopyFeatures, &hostImageCopyFeatures.pNext);
    if (isExtEnabled("VK_KHR_workgroup_memory_explicit_layout"))
        chain(&wgMemLayoutFeatures, &wgMemLayoutFeatures.pNext);
    if (isExtEnabled("VK_KHR_shader_expect_assume"))
        chain(&expectAssumeFeatures, &expectAssumeFeatures.pNext);

    vkGetPhysicalDeviceFeatures2(m_physicalDevice, &features2);

    VkDeviceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext                   = &features2;
    ci.queueCreateInfoCount    = static_cast<uint32_t>(queueCis.size());
    ci.pQueueCreateInfos       = queueCis.data();
    ci.enabledExtensionCount   = static_cast<uint32_t>(deviceExts.size());
    ci.ppEnabledExtensionNames = deviceExts.data();
    ci.pEnabledFeatures        = nullptr;

    VkResult r = vkCreateDevice(m_physicalDevice, &ci, nullptr, &m_device);
    if (r != VK_SUCCESS) {
        qCritical("vkCreateDevice failed: %d", static_cast<int>(r));
        return false;
    }
    vkGetDeviceQueue(m_device, m_graphicsFamily, 0, &m_graphicsQueue);
    vkGetDeviceQueue(m_device, m_computeFamily,  0, &m_computeQueue);
    vkGetDeviceQueue(m_device, m_transferFamily, 0, &m_transferQueue);
    if (m_videoDecodeFamily != UINT32_MAX) {
        vkGetDeviceQueue(m_device, m_videoDecodeFamily, 0, &m_videoDecodeQueue);
    }

    m_enabledDeviceExtensions.clear();
    m_enabledDeviceExtensions.reserve(deviceExts.size());
    for (const char *e : deviceExts) {
        m_enabledDeviceExtensions.emplace_back(e);
    }
    qInfo("VulkanDeviceManager: %zu device extensions enabled",
          m_enabledDeviceExtensions.size());
    return true;
}

void VulkanDeviceManager::destroyDevice()
{
    if (m_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }
    m_physicalDevice = VK_NULL_HANDLE;
}

void VulkanDeviceManager::destroyInstance()
{
    if (m_debugMessenger != VK_NULL_HANDLE && m_instance != VK_NULL_HANDLE) {
        auto pfnDestroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (pfnDestroy) pfnDestroy(m_instance, m_debugMessenger, nullptr);
        m_debugMessenger = VK_NULL_HANDLE;
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
}

} // namespace qcv
