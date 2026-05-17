# decode subsystem — VideoDecoder + ScrubDecoder + FrameIndex.
#
# Phase 1.8.1: VideoDecoder skeleton, software decode + CPU upload.
# Phase 1.8.2a: VideoToolbox attach with CPU readback.
# Phase 1.8.2b: zero-copy via CVPixelBuffer (FrameHandle::Metal).
# Phase 1.8.2c+: hwaccel zero-copy on Linux/Windows; ScrubDecoder;
#                FrameIndex.

qt_add_library(qcv_decode STATIC
    video_decoder.cpp
    video_decoder.h
    frame_handle.h
    frame_index.cpp
    frame_index.h
    scrub_decoder.cpp
    scrub_decoder.h
    decoder_cleanup_queue.cpp
    decoder_cleanup_queue.h
    timecode_formatter.cpp
    timecode_formatter.h
    image_loader.h
    image_loader.cpp
    png_image_loader.h
    png_image_loader.cpp
    tiff_image_loader.h
    tiff_image_loader.cpp
    jpeg_image_loader.h
    jpeg_image_loader.cpp
    exr_image_loader.h
    exr_image_loader.cpp
    memory_mapped_istream.h
    memory_mapped_istream.cpp
    simple_lru.h
    image_sequence_cache.h
    image_sequence_cache.cpp
    video_image_loader.h
    video_image_loader.cpp
)

if(APPLE)
    target_sources(qcv_decode PRIVATE
        frame_handle.mm
    )
    target_link_libraries(qcv_decode PRIVATE
        "-framework CoreVideo"
    )
else()
    # Phase F.2.0 — non-Apple FrameHandle implementation. Provides the
    # cross-platform methods (cpu / cpuShared / operator= / reset) plus
    # inert stubs for the CoreVideo extern "C" callbacks so the Windows /
    # Linux link succeeds without a CVPixelBuffer dependency.
    target_sources(qcv_decode PRIVATE
        frame_handle_nonapple.cpp
    )
endif()

target_include_directories(qcv_decode
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(qcv_decode
    PUBLIC
        Qt6::Core
        Qt6::Gui
        PkgConfig::FFMPEG
    PRIVATE
        PNG::PNG
        TIFF::TIFF
        JPEG::JPEG
        OpenEXR::OpenEXR
        Imath::Imath
)

# Phase F.2.4 — Vulkan link on non-Apple. qcv_decode owns the
# decode-only VulkanDeviceManager (decode queue family + win32 external
# memory + external semaphore extensions). The renderer-side bridge
# (D3D11VulkanDecodeBridge in qcv_render) accesses Vulkan handles
# through opaque void* getters so the qcv_render link to Vulkan stays
# decode-bridge-scoped, not "Vulkan-render" scoped.
if(NOT APPLE)
    # PUBLIC: VulkanDeviceManager + FrameHandle::Vulkan (F.2.4.2)
    # expose Vulkan types in their headers, so any qcv_decode
    # dependent (qcv_render, qcview) needs the Vulkan include path.
    target_link_libraries(qcv_decode PUBLIC Vulkan::Vulkan)
    target_sources(qcv_decode PRIVATE
        # Phase F.2.4.1 — decode-only VulkanDeviceManager. Lifted from
        # phase-e5-end's render-side device manager with surface /
        # swapchain / VMA stripped out; FFmpeg hybrid extension list
        # and features2 chain preserved verbatim so the device can be
        # shared with FFmpeg via AVVulkanDeviceContext in F.2.4.2.
        vulkan/vulkan_device_manager.h
        vulkan/vulkan_device_manager.cpp
        # Phase F.2.12.a — public helper that wraps VulkanDeviceManager
        # as an AVVulkanDeviceContext. Lifted out of video_decoder.cpp
        # so DualVideoDecoder can reuse the exact same handoff logic.
        vulkan_hw_device_ctx.h
        vulkan_hw_device_ctx.cpp
    )
    if(WIN32)
        # vkGetMemoryWin32HandleKHR / vkGetSemaphoreWin32HandleKHR live
        # behind this platform gate. PUBLIC so the same define is
        # active wherever vulkan.h is reached from a qcv_decode header.
        target_compile_definitions(qcv_decode PUBLIC
            VK_USE_PLATFORM_WIN32_KHR
        )
    endif()
endif()
