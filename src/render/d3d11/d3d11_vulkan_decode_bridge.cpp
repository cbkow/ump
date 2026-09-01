#include "d3d11_vulkan_decode_bridge.h"

#include "d3d11_vulkan_yuv_compositor.h"
#include "decode/frame_handle.h"
#include "decode/vulkan/vulkan_device_manager.h"
#include "render/d3d11/d3d11_device_manager.h"

#include <QtLogging>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
}

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace qcv {

namespace {

// For multi-plane VkFormats FFmpeg emits for NV12 / P010 / P012 / P016
// (h264/h265/av1 hwaccel), map the multi-plane format to the per-plane
// VkFormats we use when creating PLANE_0/PLANE_1 aspect views.
// Returns true on a known biplanar format; out params filled.
bool biplanarPlaneFormats(VkFormat multiPlane,
                           VkFormat &plane0Fmt,
                           VkFormat &plane1Fmt)
{
    switch (multiPlane) {
        case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
        case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
        case VK_FORMAT_G8_B8R8_2PLANE_444_UNORM:
            plane0Fmt = VK_FORMAT_R8_UNORM;
            plane1Fmt = VK_FORMAT_R8G8_UNORM;
            return true;
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16:
            plane0Fmt = VK_FORMAT_R10X6_UNORM_PACK16;
            plane1Fmt = VK_FORMAT_R10X6G10X6_UNORM_2PACK16;
            return true;
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16:
        case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16:
            plane0Fmt = VK_FORMAT_R12X4_UNORM_PACK16;
            plane1Fmt = VK_FORMAT_R12X4G12X4_UNORM_2PACK16;
            return true;
        case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:
        case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM:
        case VK_FORMAT_G16_B16R16_2PLANE_444_UNORM:
            plane0Fmt = VK_FORMAT_R16_UNORM;
            plane1Fmt = VK_FORMAT_R16G16_UNORM;
            return true;
        default:
            plane0Fmt = VK_FORMAT_UNDEFINED;
            plane1Fmt = VK_FORMAT_UNDEFINED;
            return false;
    }
}

uint32_t findMemoryType(VkPhysicalDevice phys, uint32_t typeBits,
                         VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(phys, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return UINT32_MAX;
}

} // namespace

struct D3D11VulkanDecodeBridge::Impl {
    bool       initialized      = false;
    uint64_t   frameSequence    = 0;
    bool       loggedFirstFrame = false;
    bool       allocFailed      = false;
    int        cachedSwFormat   = -1;
    int        cachedW          = 0;
    int        cachedH          = 0;

    // Shared compute pipeline + cross-API timeline fence — owned by
    // D3D11PlayerRenderer, injected at initialize(). Multiple bridges
    // (single-flow's + dual's slot A + slot B) point at the same
    // instance. Non-owning; must outlive this bridge.
    D3D11VulkanYuvCompositor *yuv = nullptr;

    // Per-stream shared RGBA16F texture. D3D11 owns the allocation
    // (export direction is D3D11→Vulkan; see memory:
    // project_intel_arc_d3d11_export_constraint.md). Vulkan imports the
    // memory via VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT.
    struct SharedRgba {
        // D3D11 side — owns the allocation.
        ComPtr<ID3D11Texture2D>          d3dTex;
        ComPtr<ID3D11ShaderResourceView> d3dSrv;
        HANDLE                           ntHandle = nullptr;

        // Vulkan side — imports the same memory.
        VkImage         vkImage  = VK_NULL_HANDLE;
        VkDeviceMemory  vkMemory = VK_NULL_HANDLE;
        VkImageView     vkView   = VK_NULL_HANDLE;

        int        width      = 0;
        int        height     = 0;
        VkFormat   vkFormat   = VK_FORMAT_UNDEFINED;
        DXGI_FORMAT dxgiFormat = DXGI_FORMAT_UNKNOWN;
    };
    SharedRgba    output;

    // Phase I.D.2 — parked outputs, keyed (width<<32|height). See the
    // park-and-reuse comment in consumeAVFrame. Destroyed only in
    // reset()/shutdown.
    std::unordered_map<uint64_t, SharedRgba> outputCache;

    // Per-frame VkImageView resolution — keyed by VkImage so FFmpeg's
    // ~26-image pool warms the cache in steady-state. Each decoder
    // instance has its own pool with distinct VkImage identities
    // WHILE IT IS ALIVE — but Vulkan is allowed to reuse VkImage
    // handle values after vkDestroyImage. Two decoders in sequence
    // therefore CAN collide on handle values; the cache must be
    // invalidated when the source AVHWFramesContext changes (tracked
    // via lastHwfc below) or stale views will alias the new images.
    // Phase H.1 (2026-05-14): sequential-load chroma corruption fix.
    struct CachedPlaneViews {
        VkImageView color  = VK_NULL_HANDLE;
        VkImageView plane0 = VK_NULL_HANDLE;
        VkImageView plane1 = VK_NULL_HANDLE;
    };
    std::unordered_map<VkImage, CachedPlaneViews> viewCache;

    // Tracks the AVHWFramesContext pointer the cache was built
    // against. Pointer compare only — we never dereference after the
    // change check. A different pointer means a different frame pool,
    // hence different VkImages, hence the cache must drop.
    void *lastHwfc = nullptr;

    ImportedFrame currentFrame{};
};

D3D11VulkanDecodeBridge::D3D11VulkanDecodeBridge()
    : m_impl(std::make_unique<Impl>()) {}

D3D11VulkanDecodeBridge::~D3D11VulkanDecodeBridge() { shutdown(); }

namespace {

// Allocate a shared RGBA16F texture in D3D11 + import the same memory
// into Vulkan via VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT.
// Returns true on success; populates `out` fully.
bool allocateSharedRgba(int width, int height,
                         D3D11VulkanDecodeBridge::Impl::SharedRgba &out)
{
    out = {};
    out.width      = width;
    out.height     = height;
    out.vkFormat   = VK_FORMAT_R16G16B16A16_SFLOAT;
    out.dxgiFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    auto *d3dDev = static_cast<ID3D11Device *>(
        qcv::D3D11DeviceManager::instance().device());
    if (!d3dDev) {
        qWarning("D3D11VulkanDecodeBridge: D3D11 device not available");
        return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = static_cast<UINT>(width);
    td.Height           = static_cast<UINT>(height);
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = out.dxgiFormat;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE
                        | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED_NTHANDLE
                        | D3D11_RESOURCE_MISC_SHARED;
    if (FAILED(d3dDev->CreateTexture2D(&td, nullptr,
                                         out.d3dTex.GetAddressOf()))) {
        qWarning("D3D11VulkanDecodeBridge: CreateTexture2D failed (%dx%d RGBA16F shared)",
                 width, height);
        return false;
    }
    if (FAILED(d3dDev->CreateShaderResourceView(out.d3dTex.Get(), nullptr,
                                                 out.d3dSrv.GetAddressOf()))) {
        qWarning("D3D11VulkanDecodeBridge: CreateShaderResourceView (D3D11) failed");
        out.d3dTex.Reset();
        return false;
    }

    // Export NT handle via IDXGIResource1::CreateSharedHandle.
    ComPtr<IDXGIResource1> dxgiRes;
    if (FAILED(out.d3dTex->QueryInterface(IID_PPV_ARGS(dxgiRes.GetAddressOf())))) {
        qWarning("D3D11VulkanDecodeBridge: IDXGIResource1 QueryInterface failed");
        out.d3dSrv.Reset();
        out.d3dTex.Reset();
        return false;
    }
    if (FAILED(dxgiRes->CreateSharedHandle(
            nullptr,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            nullptr,
            &out.ntHandle))) {
        qWarning("D3D11VulkanDecodeBridge: IDXGIResource1::CreateSharedHandle failed");
        out.d3dSrv.Reset();
        out.d3dTex.Reset();
        return false;
    }

    auto &vkMgr = qcv::VulkanDeviceManager::instance();
    VkDevice device = vkMgr.device();

    VkExternalMemoryImageCreateInfo extInfo{};
    extInfo.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    extInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT_KHR;

    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext         = &extInfo;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = out.vkFormat;
    ici.extent        = { static_cast<uint32_t>(width),
                          static_cast<uint32_t>(height), 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_STORAGE_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &ici, nullptr, &out.vkImage) != VK_SUCCESS) {
        qWarning("D3D11VulkanDecodeBridge: vkCreateImage (RGBA16F import) failed");
        CloseHandle(out.ntHandle); out.ntHandle = nullptr;
        out.d3dSrv.Reset();
        out.d3dTex.Reset();
        return false;
    }

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(device, out.vkImage, &memReq);

    VkImportMemoryWin32HandleInfoKHR importInfo{};
    importInfo.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT_KHR;
    importInfo.handle     = out.ntHandle;

    VkMemoryDedicatedAllocateInfo dedInfo{};
    dedInfo.sType  = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedInfo.image  = out.vkImage;
    dedInfo.pNext  = &importInfo;

    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext           = &dedInfo;
    mai.allocationSize  = memReq.size;
    mai.memoryTypeIndex = findMemoryType(
        vkMgr.physicalDevice(), memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) {
        qWarning("D3D11VulkanDecodeBridge: no DEVICE_LOCAL memory type for import");
        vkDestroyImage(device, out.vkImage, nullptr); out.vkImage = VK_NULL_HANDLE;
        CloseHandle(out.ntHandle); out.ntHandle = nullptr;
        out.d3dSrv.Reset(); out.d3dTex.Reset();
        return false;
    }
    VkResult r = vkAllocateMemory(device, &mai, nullptr, &out.vkMemory);
    if (r != VK_SUCCESS) {
        qWarning("D3D11VulkanDecodeBridge: vkAllocateMemory (NT-handle import) "
                 "failed: VkResult=%d", static_cast<int>(r));
        vkDestroyImage(device, out.vkImage, nullptr); out.vkImage = VK_NULL_HANDLE;
        CloseHandle(out.ntHandle); out.ntHandle = nullptr;
        out.d3dSrv.Reset(); out.d3dTex.Reset();
        return false;
    }
    if (vkBindImageMemory(device, out.vkImage, out.vkMemory, 0) != VK_SUCCESS) {
        qWarning("D3D11VulkanDecodeBridge: vkBindImageMemory (imported) failed");
        vkFreeMemory(device, out.vkMemory, nullptr); out.vkMemory = VK_NULL_HANDLE;
        vkDestroyImage(device, out.vkImage, nullptr); out.vkImage = VK_NULL_HANDLE;
        CloseHandle(out.ntHandle); out.ntHandle = nullptr;
        out.d3dSrv.Reset(); out.d3dTex.Reset();
        return false;
    }

    VkImageViewCreateInfo vci{};
    vci.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image      = out.vkImage;
    vci.viewType   = VK_IMAGE_VIEW_TYPE_2D;
    vci.format     = out.vkFormat;
    vci.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    vci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.baseMipLevel   = 0;
    vci.subresourceRange.levelCount     = 1;
    vci.subresourceRange.baseArrayLayer = 0;
    vci.subresourceRange.layerCount     = 1;
    if (vkCreateImageView(device, &vci, nullptr, &out.vkView) != VK_SUCCESS) {
        qWarning("D3D11VulkanDecodeBridge: vkCreateImageView (imported) failed");
        // Memory + image stay alive — dispatch needs the view though,
        // so the caller will treat this as alloc-failure.
    }

    return true;
}

// Lazily create a VkImageView over `image` keyed by the requested aspect.
VkImageView resolveCachedView(D3D11VulkanDecodeBridge::Impl &impl,
                                VkImage image,
                                VkImageAspectFlags aspect,
                                VkFormat format)
{
    if (image == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    auto &entry = impl.viewCache[image];
    VkImageView *slot = nullptr;
    if (aspect == VK_IMAGE_ASPECT_PLANE_0_BIT)      slot = &entry.plane0;
    else if (aspect == VK_IMAGE_ASPECT_PLANE_1_BIT) slot = &entry.plane1;
    else                                             slot = &entry.color;
    if (*slot != VK_NULL_HANDLE) return *slot;

    VkDevice device = qcv::VulkanDeviceManager::instance().device();
    VkImageViewCreateInfo vci{};
    vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image    = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = format;
    vci.subresourceRange.aspectMask = aspect;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &vci, nullptr, slot) != VK_SUCCESS) {
        *slot = VK_NULL_HANDLE;
        qWarning("D3D11VulkanDecodeBridge: cached view create failed "
                 "(image=%p aspect=0x%x fmt=%d)",
                 image, aspect, static_cast<int>(format));
        return VK_NULL_HANDLE;
    }
    return *slot;
}

void destroyCachedViews(D3D11VulkanDecodeBridge::Impl &impl)
{
    VkDevice device = qcv::VulkanDeviceManager::instance().isInitialized()
        ? qcv::VulkanDeviceManager::instance().device()
        : VK_NULL_HANDLE;
    if (device != VK_NULL_HANDLE) {
        for (auto &kv : impl.viewCache) {
            if (kv.second.color  != VK_NULL_HANDLE) vkDestroyImageView(device, kv.second.color,  nullptr);
            if (kv.second.plane0 != VK_NULL_HANDLE) vkDestroyImageView(device, kv.second.plane0, nullptr);
            if (kv.second.plane1 != VK_NULL_HANDLE) vkDestroyImageView(device, kv.second.plane1, nullptr);
        }
    }
    impl.viewCache.clear();
}

} // namespace

bool D3D11VulkanDecodeBridge::initialize(D3D11VulkanYuvCompositor *yuvCompositor)
{
    if (m_impl->initialized) return true;

    if (!yuvCompositor || !yuvCompositor->isInitialized()) {
        qWarning("D3D11VulkanDecodeBridge: yuv compositor not initialized");
        return false;
    }

    auto &vk = VulkanDeviceManager::instance();
    auto &d3d = D3D11DeviceManager::instance();
    if (!vk.isInitialized()) {
        qWarning("D3D11VulkanDecodeBridge: VulkanDeviceManager not initialized");
        return false;
    }
    if (!d3d.isInitialized()) {
        qWarning("D3D11VulkanDecodeBridge: D3D11DeviceManager not initialized");
        return false;
    }

    m_impl->yuv         = yuvCompositor;
    m_impl->initialized = true;
    qInfo("D3D11VulkanDecodeBridge: initialized (per-stream cache; shared "
          "compute pipeline at %p)", static_cast<void*>(yuvCompositor));
    return true;
}

void D3D11VulkanDecodeBridge::shutdown()
{
    if (!m_impl || !m_impl->initialized) return;
    reset();
    m_impl->yuv         = nullptr;
    m_impl->initialized = false;
}

bool D3D11VulkanDecodeBridge::isInitialized() const
{
    return m_impl && m_impl->initialized;
}

const D3D11VulkanDecodeBridge::ImportedFrame *
D3D11VulkanDecodeBridge::consume(const FrameHandle &fh, int rangeOverride)
{
    if (fh.kind() != FrameHandle::Kind::Vulkan) return nullptr;
    return consumeAVFrame(fh.vulkanAvFrame(), rangeOverride);
}

const D3D11VulkanDecodeBridge::ImportedFrame *
D3D11VulkanDecodeBridge::consumeAVFrame(AVFrame *avFrame, int rangeOverride)
{
    if (!m_impl || !m_impl->initialized) return nullptr;
    if (!avFrame) return nullptr;
    if (!m_impl->yuv) return nullptr;

    auto *hwfc = reinterpret_cast<AVHWFramesContext *>(
        avFrame->hw_frames_ctx ? avFrame->hw_frames_ctx->data : nullptr);
    const AVPixelFormat sw_format = hwfc ? hwfc->sw_format : AV_PIX_FMT_NONE;

    // H.1 (2026-05-14) — Invalidate per-VkImage view cache when the
    // source frame pool changes. Vulkan reuses VkImage handle values
    // after destroy; without this drop, a stale VkImageView from the
    // previous decoder's images can hit on a new decoder's image
    // → undefined sampling → green/magenta chroma corruption.
    if (hwfc != m_impl->lastHwfc) {
        // Phase I.D — drain in-flight compute before destroying the
        // old pool views: the PREVIOUS dispatch may still be executing
        // with these VkImageViews bound (the timeline wait inside
        // dispatch() happens after this point, not before it).
        if (!m_impl->viewCache.empty()) {
            qcv::VulkanDeviceManager::instance().waitForGpu();
        }
        destroyCachedViews(*m_impl);
        m_impl->lastHwfc = hwfc;
        if (hwfc) {
            qInfo("D3D11VulkanDecodeBridge: viewCache invalidated for new "
                  "frame pool (hwfc=%p)", static_cast<void *>(hwfc));
        }
    }

    const bool needRealloc =
        m_impl->output.d3dTex == nullptr ||
        m_impl->cachedSwFormat != sw_format ||
        m_impl->cachedW        != avFrame->width ||
        m_impl->cachedH        != avFrame->height;

    if (m_impl->allocFailed &&
        m_impl->cachedSwFormat == sw_format &&
        m_impl->cachedW        == avFrame->width &&
        m_impl->cachedH        == avFrame->height) {
        return nullptr;
    }

    if (needRealloc) {
        // Phase I.D.2 (2026-09-01) — park-and-reuse instead of
        // destroy-and-realloc. The old path called reset() here:
        // device-wide idle + destroy of the (possibly still D3D11-
        // referenced) shared texture + NT-handle close on EVERY
        // resolution change. Mixed-resolution ProRes playlists churn
        // that at every clip boundary, and the NVIDIA driver
        // eventually faulted on it (nvlddmkm event 153 bursts at
        // boundaries, then VK_ERROR_DEVICE_LOST; the pure-FFmpeg
        // open/decode/close churn was proven clean standalone). Now
        // the current output is PARKED per-resolution and revived on
        // the next visit; nothing GPU-visible is destroyed until
        // reset()/shutdown. Cost: one RGBA16F surface (9-17 MB) per
        // distinct resolution in the session — bounded and tiny next
        // to the decode pools.
        const auto keyOf = [](int w, int h) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(w)) << 32)
                 | static_cast<uint32_t>(h);
        };
        if (m_impl->output.d3dTex) {
            m_impl->outputCache[keyOf(m_impl->output.width,
                                        m_impl->output.height)]
                = std::move(m_impl->output);
            m_impl->output = {};
        }
        const uint64_t key = keyOf(avFrame->width, avFrame->height);
        if (auto it = m_impl->outputCache.find(key);
            it != m_impl->outputCache.end()) {
            m_impl->output = std::move(it->second);
            m_impl->outputCache.erase(it);
            qInfo("D3D11VulkanDecodeBridge: output cache HIT %dx%d "
                  "(%zu parked)",
                  avFrame->width, avFrame->height,
                  m_impl->outputCache.size());
        } else if (!allocateSharedRgba(avFrame->width, avFrame->height,
                                        m_impl->output)) {
            qWarning("D3D11VulkanDecodeBridge: shared RGBA16F alloc failed at "
                     "%dx%d — further frames at this dim will be dropped",
                     avFrame->width, avFrame->height);
            m_impl->cachedSwFormat = sw_format;
            m_impl->cachedW        = avFrame->width;
            m_impl->cachedH        = avFrame->height;
            m_impl->allocFailed    = true;
            return nullptr;
        } else {
            qInfo("D3D11VulkanDecodeBridge: shared RGBA16F allocated %dx%d "
                  "(D3D11 owns → Vulkan import OK; sw_format=%s)",
                  avFrame->width, avFrame->height,
                  av_get_pix_fmt_name(sw_format));
        }
        m_impl->cachedSwFormat = sw_format;
        m_impl->cachedW        = avFrame->width;
        m_impl->cachedH        = avFrame->height;
        m_impl->allocFailed    = false;
    }

    if (m_impl->output.vkView == VK_NULL_HANDLE) {
        // Alloc succeeded but the imported view failed earlier.
        return nullptr;
    }

    // Resolve the four input plane views via the per-stream cache, then
    // derive push constants from AVFrame metadata, and finally hand
    // everything to the shared compositor's dispatch.
    auto *vkfctx = hwfc
        ? reinterpret_cast<AVVulkanFramesContext *>(hwfc->hwctx)
        : nullptr;
    auto *vkf = reinterpret_cast<AVVkFrame *>(avFrame->data[0]);
    if (!vkfctx || !vkf) return nullptr;

    int planeCount = 0;
    for (int i = 0; i < 4; ++i) {
        if (vkf->img[i] == VK_NULL_HANDLE) break;
        ++planeCount;
    }

    bool     isBiplanar = false;
    VkFormat biplanePlane0Fmt = VK_FORMAT_UNDEFINED;
    VkFormat biplanePlane1Fmt = VK_FORMAT_UNDEFINED;
    if (planeCount == 1) {
        isBiplanar = biplanarPlaneFormats(vkfctx->format[0],
                                            biplanePlane0Fmt,
                                            biplanePlane1Fmt);
    }

    if (!isBiplanar && planeCount < 3) {
        // Unknown layout — can't dispatch.
        return nullptr;
    }

    const bool hasAlpha = (!isBiplanar && planeCount >= 4);
    VkImageView samplerViews[4] = { VK_NULL_HANDLE, VK_NULL_HANDLE,
                                     VK_NULL_HANDLE, VK_NULL_HANDLE };
    if (isBiplanar) {
        samplerViews[0] = resolveCachedView(*m_impl, vkf->img[0],
                                              VK_IMAGE_ASPECT_PLANE_0_BIT,
                                              biplanePlane0Fmt);
        samplerViews[1] = resolveCachedView(*m_impl, vkf->img[0],
                                              VK_IMAGE_ASPECT_PLANE_1_BIT,
                                              biplanePlane1Fmt);
        // Padding entries — shader's isBiplanar branch ignores them,
        // but the descriptor set still needs valid views in bindings
        // 2/3, so duplicate plane[0].
        samplerViews[2] = samplerViews[0];
        samplerViews[3] = samplerViews[0];
        if (!samplerViews[0] || !samplerViews[1]) return nullptr;
    } else {
        for (int i = 0; i < planeCount; ++i) {
            samplerViews[i] = resolveCachedView(*m_impl, vkf->img[i],
                                                  VK_IMAGE_ASPECT_COLOR_BIT,
                                                  vkfctx->format[i]);
            if (!samplerViews[i]) return nullptr;
        }
        if (!hasAlpha) samplerViews[3] = samplerViews[2];
    }

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(sw_format);
    const int bitDepth = desc ? desc->comp[0].depth : 8;

    D3D11VulkanYuvCompositor::DispatchParams dp{};
    dp.samplerViews[0] = samplerViews[0];
    dp.samplerViews[1] = samplerViews[1];
    dp.samplerViews[2] = samplerViews[2];
    dp.samplerViews[3] = samplerViews[3];
    dp.outputImage = m_impl->output.vkImage;
    dp.outputView  = m_impl->output.vkView;
    dp.width       = avFrame->width;
    dp.height      = avFrame->height;
    if (isBiplanar)               dp.bitScale = 1.0f;
    else if (bitDepth >= 16)      dp.bitScale = 1.0f;
    else if (bitDepth == 12)      dp.bitScale = 65535.0f / 4095.0f;
    else if (bitDepth == 10)      dp.bitScale = 65535.0f / 1023.0f;
    else                          dp.bitScale = 1.0f;

    switch (avFrame->colorspace) {
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: dp.colorSpace = 0; break;  // BT.601
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: dp.colorSpace = 2; break;  // BT.2020
        case AVCOL_SPC_BT709:
        default:                  dp.colorSpace = 1; break;  // BT.709
    }
    // Honor the per-clip Inspector pill: override the AV-detected
    // range when the user picked Full / Limited (otherwise Auto =
    // detect from the frame's color_range tag). Same logic as the
    // macOS path at metal_player_renderer.mm:1198-1204.
    dp.isRgb      = (desc && (desc->flags & AV_PIX_FMT_FLAG_RGB)) ? 1 : 0;
    if (rangeOverride == 1) {
        dp.range = 1;   // Full / PC-range
    } else if (rangeOverride == 2) {
        dp.range = 0;   // Limited / TV-range
    } else if (dp.isRgb) {
        // Auto for RGB = follow the tag, else full (shown as stored).
        // Untagged RGB is conventionally full; the YUV fallback below
        // (untagged → limited) must NOT leak into the RGB branch or
        // every untagged RGB source gets a levels stretch nobody asked
        // for. Mirrors VideoDecoder::publishCpuFrame on the CPU path.
        dp.range = (avFrame->color_range == AVCOL_RANGE_MPEG) ? 0 : 1;
    } else {
        dp.range = (avFrame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
    }
    dp.hasAlpha   = hasAlpha ? 1 : 0;
    dp.isBiplanar = isBiplanar ? 1 : 0;

    if (!m_impl->yuv->dispatch(dp)) {
        return nullptr;
    }

    if (!m_impl->loggedFirstFrame) {
        qInfo("  output  D3D11 tex=%p srv=%p  Vulkan img=%p mem=%p view=%p",
              m_impl->output.d3dTex.Get(), m_impl->output.d3dSrv.Get(),
              m_impl->output.vkImage, m_impl->output.vkMemory,
              m_impl->output.vkView);
        m_impl->loggedFirstFrame = true;
    }

    auto &out = m_impl->currentFrame;
    out.planes.clear();
    ImportedPlane ip{};
    ip.texture    = m_impl->output.d3dTex.Get();
    ip.srv        = m_impl->output.d3dSrv.Get();
    ip.width      = m_impl->output.width;
    ip.height     = m_impl->output.height;
    ip.dxgiFormat = static_cast<int>(m_impl->output.dxgiFormat);
    out.planes.push_back(ip);
    out.pictureWidth  = avFrame->width;
    out.pictureHeight = avFrame->height;
    out.avSwFormat    = sw_format;
    out.frameSequence = ++m_impl->frameSequence;
    return &out;
}

void D3D11VulkanDecodeBridge::reset()
{
    if (!m_impl) return;
    auto &vkMgr = qcv::VulkanDeviceManager::instance();
    VkDevice device = vkMgr.isInitialized() ? vkMgr.device() : VK_NULL_HANDLE;
    if (device != VK_NULL_HANDLE) {
        // Phase I.D — queue-locked wait-idle. reset() runs on the
        // RENDER thread on every resolution change (needRealloc in
        // consumeAVFrame) while the new clip decode thread is already
        // submitting Vulkan work — an unsynchronized vkDeviceWaitIdle
        // here was the mixed-resolution-playlist driver crash.
        vkMgr.waitForGpu();
    }
    destroyCachedViews(*m_impl);
    const auto destroyOutput = [device](Impl::SharedRgba &o) {
        if (device != VK_NULL_HANDLE) {
            if (o.vkView   != VK_NULL_HANDLE) vkDestroyImageView(device, o.vkView, nullptr);
            if (o.vkImage  != VK_NULL_HANDLE) vkDestroyImage(device, o.vkImage, nullptr);
            if (o.vkMemory != VK_NULL_HANDLE) vkFreeMemory(device, o.vkMemory, nullptr);
        }
        o.vkView   = VK_NULL_HANDLE;
        o.vkImage  = VK_NULL_HANDLE;
        o.vkMemory = VK_NULL_HANDLE;
        o.d3dSrv.Reset();
        o.d3dTex.Reset();
        if (o.ntHandle) { CloseHandle(o.ntHandle); o.ntHandle = nullptr; }
        o.width = o.height = 0;
    };
    destroyOutput(m_impl->output);
    // Phase I.D.2 — parked per-resolution outputs die here too.
    for (auto &kv : m_impl->outputCache) destroyOutput(kv.second);
    m_impl->outputCache.clear();

    m_impl->cachedSwFormat = -1;
    m_impl->cachedW        = 0;
    m_impl->cachedH        = 0;
    m_impl->allocFailed    = false;
    m_impl->currentFrame   = {};
    m_impl->frameSequence  = 0;
    m_impl->loggedFirstFrame = false;
    m_impl->lastHwfc       = nullptr;  // H.1 — re-arm hwfc-change detection
}

} // namespace qcv
