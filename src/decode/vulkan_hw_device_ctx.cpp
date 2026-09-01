#include "vulkan_hw_device_ctx.h"

#if defined(Q_OS_WIN)

#include <QtLogging>

#include <vector>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

#include <vulkan/vulkan.h>

#include "vulkan/vulkan_device_manager.h"

namespace qcv {

// Lifted from video_decoder.cpp (was anonymous-namespace, file-scope).
// The shape — qf[] entries, get_proc_addr explicit, device_features
// zero-init — was hardware-verified on Intel Arc 140T.
AVBufferRef *createSharedVulkanHwDeviceCtx()
{
    auto &dm = qcv::VulkanDeviceManager::instance();
    if (!dm.isInitialized()) {
        qWarning("createSharedVulkanHwDeviceCtx: VulkanDeviceManager not "
                 "initialized — main.cpp's startup init must run first");
        return nullptr;
    }
    // Phase I.B — refuse to wrap a known-poisoned device. The factory
    // is the choke point every decoder open() goes through; gating
    // here means any callsite (single-flow / dual-flow / scrub) falls
    // back to CPU cleanly without each having to check the flag.
    if (dm.isDeviceLost()) {
        qWarning("createSharedVulkanHwDeviceCtx: VkDevice is marked lost — "
                 "refusing to wrap a poisoned device; CPU fallback");
        return nullptr;
    }

    AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if (!ref) {
        qWarning("createSharedVulkanHwDeviceCtx: av_hwdevice_ctx_alloc returned null");
        return nullptr;
    }

    auto *hwctx = reinterpret_cast<AVHWDeviceContext *>(ref->data);
    auto *vk    = reinterpret_cast<AVVulkanDeviceContext *>(hwctx->hwctx);

    // Explicit get_proc_addr — when supplying our own instance,
    // nullptr means "dynamically load libvulkan", which crashes on
    // Windows before any log fires. Routing through the loader's
    // vkGetInstanceProcAddr matches how we created the instance.
    vk->get_proc_addr = vkGetInstanceProcAddr;

    vk->inst     = dm.vkInstance();
    vk->phys_dev = dm.physicalDevice();
    vk->act_dev  = dm.device();

    vk->device_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    vk->device_features.pNext = nullptr;

    vk->enabled_inst_extensions    = nullptr;
    vk->nb_enabled_inst_extensions = 0;

    // Hand over our enabled device-extension list verbatim. The
    // backing storage is a function-local static so the const char*
    // array lives as long as the AVBufferRef.
    static std::vector<const char *> s_devExtPtrs;
    s_devExtPtrs.clear();
    const auto &exts = dm.enabledDeviceExtensions();
    s_devExtPtrs.reserve(exts.size());
    for (const auto &e : exts) {
        s_devExtPtrs.push_back(e.c_str());
    }
    vk->enabled_dev_extensions    = s_devExtPtrs.data();
    vk->nb_enabled_dev_extensions = static_cast<int>(s_devExtPtrs.size());

    // Queue-family table (modern FFmpeg's qf[] layout). Dedup when
    // graphics/compute/transfer overlap; OR codec ops into video
    // decode entry.
    int nq = 0;
    auto pushQf = [&](uint32_t idx, VkQueueFlagBits flags, uint32_t videoCaps) {
        if (idx == UINT32_MAX) return;
        for (int j = 0; j < nq; ++j) {
            if (vk->qf[j].idx == static_cast<int>(idx)) {
                vk->qf[j].flags = static_cast<VkQueueFlagBits>(
                    static_cast<unsigned>(vk->qf[j].flags) | static_cast<unsigned>(flags));
                if (videoCaps) {
                    vk->qf[j].video_caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(
                        static_cast<unsigned>(vk->qf[j].video_caps) | videoCaps);
                }
                return;
            }
        }
        vk->qf[nq].idx        = static_cast<int>(idx);
        vk->qf[nq].num        = 1;
        vk->qf[nq].flags      = flags;
        vk->qf[nq].video_caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(videoCaps);
        ++nq;
    };
    pushQf(dm.graphicsFamily(),    VK_QUEUE_GRAPHICS_BIT,             0);
    pushQf(dm.computeFamily(),     VK_QUEUE_COMPUTE_BIT,              0);
    pushQf(dm.transferFamily(),    VK_QUEUE_TRANSFER_BIT,             0);
    pushQf(dm.videoDecodeFamily(), VK_QUEUE_VIDEO_DECODE_BIT_KHR,
           dm.videoDecodeCodecOps());
    vk->nb_qf = nq;

    // Phase I.D (2026-09-01) — wire FFmpeg per-queue lock callbacks
    // to the app-wide queue mutex. Left NULL, lavu installs its OWN
    // internal mutex, which serializes FFmpeg against FFmpeg only —
    // the renderer compositor dispatch and GUI-thread waitForGpu()
    // still raced FFmpeg vkQueueSubmit (playlist-boundary nvoglv64
    // crash on mixed-resolution ProRes playlists). Deprecated fields,
    // but honored while FF_API_VULKAN_SYNC_QUEUES holds (libavutil 60).
#if FF_API_VULKAN_SYNC_QUEUES
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    vk->lock_queue = [](AVHWDeviceContext *, uint32_t, uint32_t) {
        qcv::VulkanDeviceManager::instance().queueMutex().lock();
    };
    vk->unlock_queue = [](AVHWDeviceContext *, uint32_t, uint32_t) {
        qcv::VulkanDeviceManager::instance().queueMutex().unlock();
    };
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#endif // FF_API_VULKAN_SYNC_QUEUES

    const int initErr = av_hwdevice_ctx_init(ref);
    if (initErr < 0) {
        qWarning("createSharedVulkanHwDeviceCtx: av_hwdevice_ctx_init failed (%d)", initErr);
        av_buffer_unref(&ref);
        return nullptr;
    }
    qInfo("createSharedVulkanHwDeviceCtx: shared Vulkan device handed to FFmpeg "
          "(%d queue families, %d device extensions)",
          nq, static_cast<int>(exts.size()));
    return ref;
}

} // namespace qcv

#endif // Q_OS_WIN
