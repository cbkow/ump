#pragma once

#ifdef __linux__

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>
#include <mutex>

struct AVFrame;

namespace qcview {

//=============================================================================
// VulkanHWFrameImporter
//
// Imports VA-API hardware-decoded frames into Vulkan via DMA-BUF.
//
// Flow:
//   VA-API AVFrame (AV_PIX_FMT_VAAPI)
//       → av_hwframe_map() to AV_PIX_FMT_DRM_PRIME
//       → AVDRMFrameDescriptor (DMA-BUF fds)
//       → VkImage import via VK_EXT_external_memory_dma_buf
//       → VkImageView per plane (Y, UV for NV12/P010)
//
// The imported VkImages are temporary — the caller should use VulkanYUVRenderer
// to convert to an owned RGBA16F VkImage, then call ReleaseImportedFrame().
//
// Usage:
//   VulkanHWFrameImporter importer;
//   importer.Initialize();
//   auto frame = importer.ImportFrame(av_frame);
//   // Use frame.plane_views with VulkanYUVRenderer
//   importer.ReleaseImportedFrame(frame);
//=============================================================================

class VulkanHWFrameImporter {
public:
    VulkanHWFrameImporter();
    ~VulkanHWFrameImporter();

    VulkanHWFrameImporter(const VulkanHWFrameImporter&) = delete;
    VulkanHWFrameImporter& operator=(const VulkanHWFrameImporter&) = delete;

    bool Initialize();
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Imported Frame
    //=========================================================================

    struct ImportedFrame {
        // Per-plane VkImage/view for YUV renderer
        static constexpr int kMaxPlanes = 4;
        VkImage images[kMaxPlanes] = {};
        VkDeviceMemory memory[kMaxPlanes] = {};
        VkImageView views[kMaxPlanes] = {};
        int num_planes = 0;

        // Frame metadata for YUV renderer
        int width = 0;
        int height = 0;
        int bit_depth = 8;
        bool is_nv12 = true;  // true = NV12/P010 (2-plane), false = planar

        // DRM-mapped AVFrame (must be unreffed after use)
        AVFrame* drm_frame = nullptr;

        bool valid = false;
    };

    //=========================================================================
    // Import / Release
    //=========================================================================

    // Import a VA-API AVFrame into Vulkan VkImages
    // The AVFrame must have format AV_PIX_FMT_VAAPI
    // Returns ImportedFrame with plane VkImageViews ready for YUV rendering
    ImportedFrame ImportFrame(AVFrame* vaapi_frame);

    // Release an imported frame's Vulkan resources
    // Must be called after YUV rendering is complete (i.e., after vkQueueSubmit + fence wait)
    void ReleaseImportedFrame(ImportedFrame& frame);

private:
    bool initialized_ = false;

    // Reusable command resources
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
};

} // namespace qcview

#endif // __linux__
