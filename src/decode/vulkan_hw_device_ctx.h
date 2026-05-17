// vulkan_hw_device_ctx — Phase F.2.12.a.
//
// Public helper that wraps qcv::VulkanDeviceManager as an
// AVVulkanDeviceContext, so FFmpeg decodes (Vulkan hwaccel) land on
// the same VkDevice the D3D11VulkanDecodeBridge consumes from. Lifted
// out of video_decoder.cpp's anonymous namespace so dual-flow's
// DualVideoDecoder can call the exact same helper — no copy-paste,
// no parallel implementation.
//
// Windows-only: this header gates everything on Q_OS_WIN because the
// Vulkan-decode handoff is a Windows-specific tier (macOS uses
// VideoToolbox; Linux Vulkan-decode is behind QCV_BUILD_LINUX_VULKAN
// and currently inactive). FFmpeg + Vulkan headers stay private to
// the cpp; this header forward-declares AVBufferRef only.

#pragma once

#include <QtGlobal>

#if defined(Q_OS_WIN)

extern "C" {
struct AVBufferRef;
}

namespace qcv {

// Allocates an AVBufferRef wrapping an AVVulkanDeviceContext that
// points at the shared VkDevice / VkInstance / physical device owned
// by VulkanDeviceManager. Returns nullptr on failure (caller is
// expected to fall back to software decode).
//
// Lifecycle: caller owns the returned buffer ref and must release
// with `av_buffer_unref` (or hand it to FFmpeg via
// `m_cctx->hw_device_ctx = av_buffer_ref(ref)`).
//
// Requires VulkanDeviceManager::instance().isInitialized() at call
// time — main.cpp's startup init must have run.
AVBufferRef *createSharedVulkanHwDeviceCtx();

} // namespace qcv

#endif // Q_OS_WIN
