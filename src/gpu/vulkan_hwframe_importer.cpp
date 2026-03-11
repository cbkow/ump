#include "vulkan_hwframe_importer.h"

#ifdef __linux__

#include "vulkan_device.h"
#include "../utils/debug_utils.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
}

#include <libdrm/drm_fourcc.h>
#include <unistd.h>
#include <cstring>

namespace qcview {

//=============================================================================
// Constructor / Destructor
//=============================================================================

VulkanHWFrameImporter::VulkanHWFrameImporter() = default;

VulkanHWFrameImporter::~VulkanHWFrameImporter() {
    Shutdown();
}

//=============================================================================
// Initialize
//=============================================================================

bool VulkanHWFrameImporter::Initialize() {
    if (initialized_) return true;

    auto& dev = VulkanDeviceManager::Instance();
    if (!dev.IsInitialized()) {
        Debug::Log("VulkanHWFrameImporter: VulkanDeviceManager not initialized");
        return false;
    }

    if (!dev.HasDMABufImport()) {
        Debug::Log("VulkanHWFrameImporter: DMA-BUF import extensions not available");
        return false;
    }

    // Create command pool for transition commands
    command_pool_ = dev.CreateCommandPool(dev.GetGraphicsQueueFamily(),
                                           VK_COMMAND_POOL_CREATE_TRANSIENT_BIT);
    if (command_pool_ == VK_NULL_HANDLE) {
        Debug::Log("VulkanHWFrameImporter: Failed to create command pool");
        return false;
    }

    initialized_ = true;
    Debug::Log("VulkanHWFrameImporter: Initialized");
    return true;
}

//=============================================================================
// Shutdown
//=============================================================================

void VulkanHWFrameImporter::Shutdown() {
    if (!initialized_) return;

    auto device = VulkanDeviceManager::Instance().GetDevice();

    if (command_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
    }

    initialized_ = false;
    Debug::Log("VulkanHWFrameImporter: Shutdown");
}

//=============================================================================
// DRM fourcc → VkFormat mapping
//=============================================================================

static VkFormat DrmFourccToVkFormat(uint32_t fourcc, int plane_index) {
    switch (fourcc) {
        // Multi-plane NV12/P010 (single layer, 2 planes)
        case DRM_FORMAT_NV12:
            return (plane_index == 0) ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
        case DRM_FORMAT_P010:
            return (plane_index == 0) ? VK_FORMAT_R16_UNORM : VK_FORMAT_R16G16_UNORM;

        // Per-layer formats (when driver exports NV12 as separate layers)
        case DRM_FORMAT_R8:       return VK_FORMAT_R8_UNORM;      // Y plane
        case DRM_FORMAT_GR88:     return VK_FORMAT_R8G8_UNORM;    // UV plane (8-bit)
        case DRM_FORMAT_R16:      return VK_FORMAT_R16_UNORM;     // Y plane (10/16-bit)
        case DRM_FORMAT_GR1616:   return VK_FORMAT_R16G16_UNORM;  // UV plane (10/16-bit)

        // Planar YUV (each plane is single-channel)
        case DRM_FORMAT_YUV420:
        case DRM_FORMAT_YUV422:
        case DRM_FORMAT_YUV444:
            return VK_FORMAT_R8_UNORM;

        default: {
            // Log as actual fourcc chars for debugging
            char cc[5] = {};
            cc[0] = fourcc & 0xFF;
            cc[1] = (fourcc >> 8) & 0xFF;
            cc[2] = (fourcc >> 16) & 0xFF;
            cc[3] = (fourcc >> 24) & 0xFF;
            Debug::Log("VulkanHWFrameImporter: Unknown DRM fourcc '" +
                       std::string(cc) + "' (0x" + std::to_string(fourcc) + ")");
            return (plane_index == 0) ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
        }
    }
}

// Check if the DRM descriptor represents an NV12-like 2-plane YUV format
static bool IsNV12Like(const AVDRMFrameDescriptor* desc) {
    if (!desc) return false;

    // Single layer with NV12/P010 fourcc
    if (desc->nb_layers == 1) {
        uint32_t fmt = desc->layers[0].format;
        return fmt == DRM_FORMAT_NV12 || fmt == DRM_FORMAT_P010;
    }

    // Two layers: R8 + GR88 (NV12 split) or R16 + GR1616 (P010 split)
    if (desc->nb_layers == 2) {
        uint32_t f0 = desc->layers[0].format;
        uint32_t f1 = desc->layers[1].format;
        return (f0 == DRM_FORMAT_R8 && f1 == DRM_FORMAT_GR88) ||
               (f0 == DRM_FORMAT_R16 && f1 == DRM_FORMAT_GR1616);
    }

    return false;
}

static int DetectBitDepth(const AVDRMFrameDescriptor* desc) {
    if (!desc || desc->nb_layers == 0) return 8;
    uint32_t fmt = desc->layers[0].format;
    if (fmt == DRM_FORMAT_P010 || fmt == DRM_FORMAT_R16 || fmt == DRM_FORMAT_GR1616)
        return 10;
    return 8;
}

//=============================================================================
// ImportFrame
//=============================================================================

VulkanHWFrameImporter::ImportedFrame VulkanHWFrameImporter::ImportFrame(AVFrame* vaapi_frame) {
    ImportedFrame result{};

    if (!initialized_ || !vaapi_frame) return result;

    if (vaapi_frame->format != AV_PIX_FMT_VAAPI) {
        Debug::Log("VulkanHWFrameImporter: Frame is not VA-API format");
        return result;
    }

    auto& dev = VulkanDeviceManager::Instance();
    VkDevice device = dev.GetDevice();

    // Map VA-API frame to DRM PRIME (DMA-BUF)
    AVFrame* drm_frame = av_frame_alloc();
    if (!drm_frame) {
        Debug::Log("VulkanHWFrameImporter: Failed to allocate DRM frame");
        return result;
    }

    // Tell av_hwframe_map we want DRM PRIME output
    drm_frame->format = AV_PIX_FMT_DRM_PRIME;

    // Map to DRM PRIME to get DMA-BUF file descriptors
    int ret = av_hwframe_map(drm_frame, vaapi_frame,
                              AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("VulkanHWFrameImporter: av_hwframe_map failed: " + std::string(errbuf));
        av_frame_free(&drm_frame);
        return result;
    }

    if (drm_frame->format != AV_PIX_FMT_DRM_PRIME) {
        Debug::Log("VulkanHWFrameImporter: Mapped frame is not DRM_PRIME format (got " +
                   std::to_string(drm_frame->format) + ")");
        av_frame_unref(drm_frame);
        av_frame_free(&drm_frame);
        return result;
    }

    auto* drm_desc = reinterpret_cast<AVDRMFrameDescriptor*>(drm_frame->data[0]);
    if (!drm_desc || drm_desc->nb_layers == 0) {
        Debug::Log("VulkanHWFrameImporter: No DRM layers");
        av_frame_unref(drm_frame);
        av_frame_free(&drm_frame);
        return result;
    }

    result.width = vaapi_frame->width;
    result.height = vaapi_frame->height;
    result.drm_frame = drm_frame;

    // Detect format from DRM descriptor
    result.is_nv12 = IsNV12Like(drm_desc);
    result.bit_depth = DetectBitDepth(drm_desc);

    // Import each plane as a separate VkImage
    // For NV12/P010: layer 0 has 2 planes (Y and UV)
    // For planar YUV: typically 3 separate layers/planes
    int total_planes = 0;
    for (int li = 0; li < drm_desc->nb_layers && total_planes < ImportedFrame::kMaxPlanes; li++) {
        const AVDRMLayerDescriptor& layer = drm_desc->layers[li];

        for (int pi = 0; pi < layer.nb_planes && total_planes < ImportedFrame::kMaxPlanes; pi++) {
            const AVDRMPlaneDescriptor& plane = layer.planes[pi];
            const AVDRMObjectDescriptor& obj = drm_desc->objects[plane.object_index];

            VkFormat plane_format = DrmFourccToVkFormat(layer.format, pi);

            // Determine plane dimensions
            int plane_width = result.width;
            int plane_height = result.height;

            // UV/chroma planes are half-resolution for 4:2:0 formats
            bool is_chroma_plane = false;
            if (result.is_nv12) {
                // NV12: single layer with pi==1, or second layer (GR88/GR1616)
                is_chroma_plane = (pi == 1) ||
                                  (layer.format == DRM_FORMAT_GR88) ||
                                  (layer.format == DRM_FORMAT_GR1616);
            } else if (total_planes > 0) {
                is_chroma_plane = true;  // Planar YUV420 chroma
            }
            if (is_chroma_plane) {
                plane_width /= 2;
                plane_height /= 2;
            }

            // Create VkImage from DMA-BUF fd
            // Use VK_EXT_external_memory_dma_buf + VK_EXT_image_drm_format_modifier

            uint64_t modifier = obj.format_modifier;

            // Subresource layout for the plane
            VkSubresourceLayout drm_plane_layout = {};
            drm_plane_layout.offset = plane.offset;
            drm_plane_layout.rowPitch = plane.pitch;
            drm_plane_layout.size = 0;  // Unknown, not needed for import
            drm_plane_layout.arrayPitch = 0;
            drm_plane_layout.depthPitch = 0;

            // DRM format modifier info
            VkImageDrmFormatModifierExplicitCreateInfoEXT drm_mod_info = {};
            drm_mod_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
            drm_mod_info.drmFormatModifier = modifier;
            drm_mod_info.drmFormatModifierPlaneCount = 1;
            drm_mod_info.pPlaneLayouts = &drm_plane_layout;

            // External memory image create info
            VkExternalMemoryImageCreateInfo ext_mem_info = {};
            ext_mem_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
            ext_mem_info.pNext = &drm_mod_info;
            ext_mem_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

            VkImageCreateInfo image_info = {};
            image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            image_info.pNext = &ext_mem_info;
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.format = plane_format;
            image_info.extent = { static_cast<uint32_t>(plane_width),
                                  static_cast<uint32_t>(plane_height), 1 };
            image_info.mipLevels = 1;
            image_info.arrayLayers = 1;
            image_info.samples = VK_SAMPLE_COUNT_1_BIT;
            image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
            image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
            image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VkImage image = VK_NULL_HANDLE;
            VkResult vr = vkCreateImage(device, &image_info, nullptr, &image);
            if (vr != VK_SUCCESS) {
                Debug::Log("VulkanHWFrameImporter: vkCreateImage failed for plane " +
                           std::to_string(total_planes) + " (result=" + std::to_string(vr) + ")");
                // Clean up already-imported planes
                ReleaseImportedFrame(result);
                return result;
            }

            // Get memory requirements
            VkMemoryRequirements mem_reqs;
            vkGetImageMemoryRequirements(device, image, &mem_reqs);

            // Import DMA-BUF fd as VkDeviceMemory
            VkImportMemoryFdInfoKHR import_fd_info = {};
            import_fd_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
            import_fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            // dup() the fd because Vulkan takes ownership
            import_fd_info.fd = dup(obj.fd);
            if (import_fd_info.fd < 0) {
                Debug::Log("VulkanHWFrameImporter: dup() failed for DMA-BUF fd");
                vkDestroyImage(device, image, nullptr);
                ReleaseImportedFrame(result);
                return result;
            }

            // Find suitable memory type
            VkPhysicalDeviceMemoryProperties mem_props;
            vkGetPhysicalDeviceMemoryProperties(dev.GetPhysicalDevice(), &mem_props);

            uint32_t memory_type_index = UINT32_MAX;
            for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
                if (mem_reqs.memoryTypeBits & (1u << i)) {
                    memory_type_index = i;
                    break;
                }
            }

            if (memory_type_index == UINT32_MAX) {
                Debug::Log("VulkanHWFrameImporter: No suitable memory type found");
                close(import_fd_info.fd);
                vkDestroyImage(device, image, nullptr);
                ReleaseImportedFrame(result);
                return result;
            }

            VkMemoryAllocateInfo alloc_info = {};
            alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc_info.pNext = &import_fd_info;
            alloc_info.allocationSize = mem_reqs.size;
            alloc_info.memoryTypeIndex = memory_type_index;

            // Use dedicated allocation for imported memory
            VkMemoryDedicatedAllocateInfo dedicated_info = {};
            dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
            dedicated_info.image = image;
            import_fd_info.pNext = &dedicated_info;

            VkDeviceMemory memory = VK_NULL_HANDLE;
            vr = vkAllocateMemory(device, &alloc_info, nullptr, &memory);
            if (vr != VK_SUCCESS) {
                Debug::Log("VulkanHWFrameImporter: vkAllocateMemory failed for plane " +
                           std::to_string(total_planes) + " (result=" + std::to_string(vr) + ")");
                // fd is consumed by vkAllocateMemory even on failure per spec,
                // but to be safe:
                vkDestroyImage(device, image, nullptr);
                ReleaseImportedFrame(result);
                return result;
            }

            vr = vkBindImageMemory(device, image, memory, 0);
            if (vr != VK_SUCCESS) {
                Debug::Log("VulkanHWFrameImporter: vkBindImageMemory failed");
                vkFreeMemory(device, memory, nullptr);
                vkDestroyImage(device, image, nullptr);
                ReleaseImportedFrame(result);
                return result;
            }

            // Create image view
            VkImageViewCreateInfo view_info = {};
            view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image = image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = plane_format;
            view_info.components = { VK_COMPONENT_SWIZZLE_IDENTITY,
                                     VK_COMPONENT_SWIZZLE_IDENTITY,
                                     VK_COMPONENT_SWIZZLE_IDENTITY,
                                     VK_COMPONENT_SWIZZLE_IDENTITY };
            view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view_info.subresourceRange.baseMipLevel = 0;
            view_info.subresourceRange.levelCount = 1;
            view_info.subresourceRange.baseArrayLayer = 0;
            view_info.subresourceRange.layerCount = 1;

            VkImageView view = VK_NULL_HANDLE;
            vr = vkCreateImageView(device, &view_info, nullptr, &view);
            if (vr != VK_SUCCESS) {
                Debug::Log("VulkanHWFrameImporter: vkCreateImageView failed");
                vkFreeMemory(device, memory, nullptr);
                vkDestroyImage(device, image, nullptr);
                ReleaseImportedFrame(result);
                return result;
            }

            result.images[total_planes] = image;
            result.memory[total_planes] = memory;
            result.views[total_planes] = view;
            total_planes++;
        }
    }

    result.num_planes = total_planes;

    if (total_planes == 0) {
        Debug::Log("VulkanHWFrameImporter: No planes imported");
        av_frame_unref(drm_frame);
        av_frame_free(&drm_frame);
        result.drm_frame = nullptr;
        return result;
    }

    // Transition imported images to SHADER_READ_ONLY_OPTIMAL
    VkCommandBuffer cmd = dev.BeginSingleTimeCommands();

    for (int i = 0; i < total_planes; i++) {
        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        barrier.dstQueueFamilyIndex = dev.GetGraphicsQueueFamily();
        barrier.image = result.images[i];
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
            0,
            0, nullptr,
            0, nullptr,
            1, &barrier);
    }

    dev.EndSingleTimeCommands(cmd);

    result.valid = true;
    return result;
}

//=============================================================================
// ReleaseImportedFrame
//=============================================================================

void VulkanHWFrameImporter::ReleaseImportedFrame(ImportedFrame& frame) {
    VkDevice device = VulkanDeviceManager::Instance().GetDevice();

    for (int i = 0; i < ImportedFrame::kMaxPlanes; i++) {
        if (frame.views[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(device, frame.views[i], nullptr);
            frame.views[i] = VK_NULL_HANDLE;
        }
        if (frame.images[i] != VK_NULL_HANDLE) {
            vkDestroyImage(device, frame.images[i], nullptr);
            frame.images[i] = VK_NULL_HANDLE;
        }
        if (frame.memory[i] != VK_NULL_HANDLE) {
            vkFreeMemory(device, frame.memory[i], nullptr);
            frame.memory[i] = VK_NULL_HANDLE;
        }
    }

    if (frame.drm_frame) {
        av_frame_unref(frame.drm_frame);
        av_frame_free(&frame.drm_frame);
        frame.drm_frame = nullptr;
    }

    frame.valid = false;
    frame.num_planes = 0;
}

} // namespace qcview

#endif // __linux__
