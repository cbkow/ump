// VulkanDeviceManager — Phase F.2.4.1 (decode-only).
//
// Singleton owning the Vulkan instance, physical device, logical device,
// and queue families needed by FFmpeg's Vulkan hwaccel decode path.
// This is the post-revert / D3D11-renderer-era version: NO surface,
// NO swapchain, NO render-side resources. The renderer is D3D11; this
// device exists only so FFmpeg can decode straight into VkImages that
// we then bridge to D3D11 textures via NT shared handles (F.2.4.3).
//
// Lifted from the phase-e5-end VulkanDeviceManager (commit 537d37c,
// src/render/vulkan/vulkan_device_manager.h) and stripped of:
//   - VkSurfaceKHR / probe surface
//   - swapchain extension (instance + device)
//   - VK_EXT_swapchain_colorspace / VK_EXT_hdr_metadata
//   - VK_KHR_win32_surface
//   - VMA allocator (decode-only doesn't allocate VkImages itself —
//     FFmpeg's hwframes_ctx handles allocation)
//
// Kept verbatim:
//   - FFmpeg hybrid device-extension list (push_descriptor,
//     external_memory_win32, external_semaphore_win32, video_decode_*,
//     etc. — FFmpeg's standalone Vulkan hwdevice context probes for
//     these; mirroring it lets us share OUR VkDevice via
//     AVVulkanDeviceContext::enabled_dev_extensions)
//   - VkPhysicalDeviceFeatures2 chain covering Vulkan 1.1/1.2/1.3 +
//     per-extension feature structs FFmpeg requires
//
// Threading: initialize() / shutdown() are GUI-thread before decode
// starts. After init, the VkDevice + VkQueues are read by the decode
// thread + the renderer-side bridge thread.

#pragma once

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace qcv {

class VulkanDeviceManager {
public:
    static VulkanDeviceManager &instance();

    VulkanDeviceManager(const VulkanDeviceManager &)            = delete;
    VulkanDeviceManager &operator=(const VulkanDeviceManager &) = delete;

    // Stand up VkInstance + VkPhysicalDevice + VkDevice. No surface.
    // Returns false on failure; isInitialized() stays false. Safe to
    // call once per process (subsequent calls are no-ops returning the
    // first call's success state).
    bool initialize();
    void shutdown();
    bool isInitialized() const { return m_initialized; }

    // Vulkan handles. Lifetimes match the singleton.
    VkInstance       vkInstance()     const { return m_instance; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkDevice         device()         const { return m_device; }

    // Queue family indices. Compute and transfer fall back to graphics
    // when no dedicated family exists. videoDecodeFamily is UINT32_MAX
    // when the physical device has no VK_QUEUE_VIDEO_DECODE_BIT_KHR
    // queue family — FFmpeg then falls back to its compute-shader
    // codec path (ProRes / FFv1) using the compute queue.
    uint32_t graphicsFamily()      const { return m_graphicsFamily;      }
    uint32_t computeFamily()       const { return m_computeFamily;       }
    uint32_t transferFamily()      const { return m_transferFamily;      }
    uint32_t videoDecodeFamily()   const { return m_videoDecodeFamily;   }
    // VkVideoCodecOperationFlagsKHR bitfield (h264=1, h265=2, av1=4).
    // Zero when no video decode queue (or queue exists but advertises
    // no codecs — never observed in practice).
    uint32_t videoDecodeCodecOps() const { return m_videoDecodeCodecOps; }

    bool hasVideoDecodeQueue() const { return m_videoDecodeFamily != UINT32_MAX; }

    VkQueue graphicsQueue()    const { return m_graphicsQueue;    }
    VkQueue computeQueue()     const { return m_computeQueue;     }
    VkQueue transferQueue()    const { return m_transferQueue;    }
    VkQueue videoDecodeQueue() const { return m_videoDecodeQueue; }

    // Device extensions actually enabled at vkCreateDevice (after
    // filtering against what the physical device advertises). The
    // FFmpeg AVVulkanDeviceContext::enabled_dev_extensions field
    // takes this list verbatim — that's the contract for shared
    // device handoff.
    const std::vector<std::string> &enabledDeviceExtensions() const {
        return m_enabledDeviceExtensions;
    }

    // Submit an empty buffer and wait for all prior GPU work. Used
    // before CPU readback in diagnostics + before destroying resources
    // that may still be referenced by in-flight command buffers.
    void waitForGpu();

    // Diagnostics. Pointer is owned by VkPhysicalDeviceProperties — do
    // not free; valid between initialize() and shutdown().
    const char *deviceName() const;

    // Phase I.B — device-lost tracking. Any consumer that observes
    // VK_ERROR_DEVICE_LOST (either through FFmpeg's av_log or a
    // direct vkQueueSubmit return) calls markDeviceLost(); the flag
    // is sticky. createSharedVulkanHwDeviceCtx checks isDeviceLost()
    // and refuses to wrap a poisoned device → callers fall back to
    // CPU. VideoDecoder's decode loop also polls per-iteration and
    // releases its cached AVHWDeviceContext when set, then errors
    // out cleanly so subsequent reopens don't reattach the bad ref.
    //
    // Sticky until I.C ships recreateDevice() — for I.B the only
    // path back to working hwaccel is to restart the app.
    void markDeviceLost();
    bool isDeviceLost() const {
        return m_deviceLost.load(std::memory_order_acquire);
    }

    // Bumped on every successful initialize() / recreateDevice().
    // Renderer-side bridges can use it to invalidate cached Vulkan
    // refs when the device is replaced. For I.B this only changes
    // at startup (one bump in initialize()); I.C will bump it again
    // on each recreate.
    int deviceGeneration() const {
        return m_deviceGeneration.load(std::memory_order_acquire);
    }

    // Phase I.C stub — returns false (real recreation deferred).
    // Documented here so I.B-era callers can write code against the
    // final API even though the recreate is a no-op for now.
    bool recreateDevice() { return false; }

private:
    VulkanDeviceManager();
    ~VulkanDeviceManager();

    bool createInstance();
    bool selectPhysicalDevice();
    bool createLogicalDevice();
    void destroyDevice();
    void destroyInstance();

    VkInstance       m_instance       = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice         m_device         = VK_NULL_HANDLE;

    uint32_t m_graphicsFamily      = UINT32_MAX;
    uint32_t m_computeFamily       = UINT32_MAX;
    uint32_t m_transferFamily      = UINT32_MAX;
    uint32_t m_videoDecodeFamily   = UINT32_MAX;
    uint32_t m_videoDecodeCodecOps = 0;

    VkQueue m_graphicsQueue    = VK_NULL_HANDLE;
    VkQueue m_computeQueue     = VK_NULL_HANDLE;
    VkQueue m_transferQueue    = VK_NULL_HANDLE;
    VkQueue m_videoDecodeQueue = VK_NULL_HANDLE;

    std::vector<std::string> m_enabledDeviceExtensions;

    VkPhysicalDeviceProperties m_deviceProps{};

    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;

    bool               m_initialized = false;
    mutable std::mutex m_mutex;

    // Phase I.B — see markDeviceLost / isDeviceLost / deviceGeneration
    // accessors above. Both atomics so the decoder thread + the
    // FFmpeg log thread + the GUI thread can all touch them without
    // taking m_mutex.
    std::atomic<bool> m_deviceLost{false};
    std::atomic<int>  m_deviceGeneration{0};
};

} // namespace qcv
