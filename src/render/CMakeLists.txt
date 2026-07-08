# render subsystem — PlayerRhiItem and the QRhi render pipeline.
#
# Phase 1.3: PlayerRhiItem with a clear-color pass.
# Phase 1.4+: compositor, OCIO, stroke, safety, display passes.
#
# Built as a static QML module (URI = Qcv.Render). The executable
# links against the auto-generated `qcv_renderplugin` target so the
# module's static initializer runs at app startup.

qt_add_library(qcv_render STATIC)

qt_add_qml_module(qcv_render
    URI Qcv.Render
    VERSION 1.0
    SOURCES
        player_rhi_item.cpp
        player_rhi_item.h
        # Timeline waveform experiment — viewport-sized lazy peak
        # strip (first QQuickPaintedItem in the app).
        waveform_strip.cpp
        waveform_strip.h
        test_textures.cpp
        test_textures.h
        ocio_shader_builder.cpp
        ocio_shader_builder.h
        dual_view_layout.cpp
        dual_view_layout.h
        svg_path_parser.cpp
        svg_path_parser.h
        safety_overlay.cpp
        safety_overlay.h
        # Serves the frozen modal backdrop frame to QML (image://qcv/...);
        # see the UI-over-viewport framework in window_manager.cpp.
        backdrop_image_provider.cpp
        backdrop_image_provider.h
        # Inspector media thumbnails (image://thumb/...) — runs the
        # decode loaders' loadThumbnail fast paths on provider threads.
        thumbnail_image_provider.cpp
        thumbnail_image_provider.h
        # Phase 7.5 A.3 — native QWindow + IPlayerRenderer skeleton.
        # Always compiled; runtime selection via QCV_NATIVE_PLAYER
        # define + WindowManager wiring (see src/window/window_manager.cpp).
        iplayer_renderer.h
        # Phase F.2.11.b — pure-virtual frame-pair seam for the
        # Windows D3D11 dual flow. Concrete adapter lives in
        # qcv_window (which links qcv_dual); qcv_render only
        # consumes the abstract interface so the qmltypes cycle
        # stays broken on non-Apple.
        i_dual_frame_source.h
        player_window.h
        player_window.cpp
        player_window_factory.h
        player_window_factory.cpp
)

if(APPLE)
    target_sources(qcv_render PRIVATE
        zero_copy_bridge_metal.h
        zero_copy_bridge_metal.mm
        # Phase 7.5 B.1 / B.2 / B.3 — native Metal player surface.
        # Device manager handles the layer; player_renderer drives the
        # present loop on its own thread; hdr_swapchain reconfigures
        # the layer pixel format + colorspace + EDR.
        metal/metal_device_manager.h
        metal/metal_device_manager.mm
        metal/metal_player_renderer.h
        metal/metal_player_renderer.mm
        metal/metal_hdr_swapchain.h
        metal/metal_hdr_swapchain.mm
        metal/metal_texture_pool.h
        metal/metal_texture_pool.mm
        metal/gpu_upload_thread.h
        metal/gpu_upload_thread.mm
        metal/metal_compositor.h
        metal/metal_compositor.mm
        metal/metal_yuv_renderer.h
        metal/metal_yuv_renderer.mm
        metal/cv_pixbuf_metal_bridge.h
        metal/cv_pixbuf_metal_bridge.mm
        metal/metal_ocio_renderer.h
        metal/metal_ocio_renderer.mm
        metal/metal_annotation_renderer.h
        metal/metal_annotation_renderer.mm
        # Task #107/#108 commit 2 — qcv_render-side adapter that
        # implements qcv::dual::IDualPixbufConverter using the existing
        # CvPixbufMetalBridge + MetalYuvRenderer. Injected into
        # DualCompositor at dual-mode entry by MetalPlayerRenderer.
        metal/dual_pixbuf_converter_impl.h
        metal/dual_pixbuf_converter_impl.mm
    )
    # Phase 7.5 Metal port: enable ARC for metal/*.mm. The existing
    # zero_copy_bridge_metal.mm uses manual reference counting + a
    # PIMPL; the new metal_*.mm files lift PIMPL but rely on ARC for
    # ObjC-typed Impl-member retain semantics (matches the old app's
    # behavior since the old codebase compiled .mm with ARC by
    # default). File-scoped flag avoids touching the existing bridge.
    set_source_files_properties(
        metal/metal_device_manager.mm
        metal/metal_player_renderer.mm
        metal/metal_hdr_swapchain.mm
        metal/metal_texture_pool.mm
        metal/gpu_upload_thread.mm
        metal/metal_compositor.mm
        metal/metal_yuv_renderer.mm
        metal/cv_pixbuf_metal_bridge.mm
        metal/metal_ocio_renderer.mm
        metal/metal_annotation_renderer.mm
        metal/dual_pixbuf_converter_impl.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc"
    )
    target_link_libraries(qcv_render PRIVATE
        "-framework CoreVideo"
        "-framework Cocoa"            # NSView for the CAMetalLayer attachment
        "-framework Metal"
        "-framework QuartzCore"
    )
endif()

# Phase F.2.0 — native D3D11 renderer for the player surface on Windows.
# Mirrors the if(APPLE) Metal block above structurally. Sources land in
# F.2.1 alongside the device manager + player renderer.
#
# Vulkan stays linked on non-Apple for the FFmpeg hwaccel decode path
# only (Phase F.2.4 reintroduces shared-Vulkan-context plumbing for the
# decoder); the renderer itself is D3D11. Linux Vulkan-render path is
# behind QCV_BUILD_LINUX_VULKAN (default OFF) and unused for now.
if(WIN32)
    # Phase F.2.1 — D3D11 native player surface on Windows.
    # D3D11DeviceManager owns the device + immediate context + DXGI
    # factory; D3D11PlayerRenderer owns the child HWND + DComp +
    # flip-discard swapchain and drives the render thread.
    # Phase F.2.2 — D3D11 texture pool + GPU upload thread. Mirror
    # MetalTexturePool / MetalGpuUploadThread so cross-platform
    # consumers (ImageSequenceCache push callbacks,
    # TimelineThumbnailCache, hover-thumb pipeline) link the same way.
    target_sources(qcv_render PRIVATE
        d3d11/d3d11_device_manager.h
        d3d11/d3d11_device_manager.cpp
        d3d11/d3d11_player_renderer.h
        d3d11/d3d11_player_renderer.cpp
        d3d11/d3d11_texture_pool.h
        d3d11/d3d11_texture_pool.cpp
        d3d11/d3d11_gpu_upload_thread.h
        d3d11/d3d11_gpu_upload_thread.cpp
        # Phase F.2.3 — D3D11 single-source compositor (fullscreen
        # pixel-shader with letterbox/pillarbox + bg fill).
        d3d11/d3d11_compositor.h
        d3d11/d3d11_compositor.cpp
        # Phase F.2.4.3 — Vulkan→D3D11 cross-API bridge for decoded
        # frames. Stage A is a logging stub; Stage B/C fill in the
        # VkImage allocation, NT handle export, ID3D11 import + sync.
        # F.2.13 split: the heavy compute pipeline moved to
        # d3d11_vulkan_yuv_compositor.{h,cpp}; the bridge is now a
        # per-stream cache that points at the shared compositor.
        d3d11/d3d11_vulkan_decode_bridge.h
        d3d11/d3d11_vulkan_decode_bridge.cpp
        d3d11/d3d11_vulkan_yuv_compositor.h
        d3d11/d3d11_vulkan_yuv_compositor.cpp
        # Phase F.2.5 — D3D11 OCIO color pipeline. Mirrors the
        # MetalOcioRenderer: OcioChainBuilder emits HLSL via
        # GPU_LANGUAGE_HLSL_SM_5_0; we wrap it in a thin PS that
        # samples the composited intermediate (RGBA16F at swapchain
        # size) and writes to the swapchain back buffer.
        d3d11/d3d11_ocio_renderer.h
        d3d11/d3d11_ocio_renderer.cpp
        # Phase F.2.7 — D3D11 annotation + safety-overlay renderer.
        # Consumes TessellatedMesh from the cross-platform tessellator
        # (annotations/stroke_tessellator.{h,cpp}) — same data shape
        # MetalAnnotationRenderer eats. Pixel-coords → NDC vertex
        # shader, per-vertex RGBA passthrough fragment, source-over
        # alpha blend.
        d3d11/d3d11_annotation_renderer.h
        d3d11/d3d11_annotation_renderer.cpp
        # Phase F.2.8 — D3D11 HDR via scRGB linear. Owns the
        # IDXGISwapChain1 + DComp visual + format/colorspace
        # transitions. Mirrors MetalHdrSwapchain's role.
        d3d11/d3d11_hdr_swapchain.h
        d3d11/d3d11_hdr_swapchain.cpp
        # Phase F.2.11.c — D3D11 dual compositor. HLSL port of
        # qcv::dual::DualCompositor (Metal). Pulls per-side frames
        # through the qcv_render-side IDualFrameSource seam so
        # qcv_render still doesn't link qcv_dual on non-Apple.
        d3d11/d3d11_dual_compositor.h
        d3d11/d3d11_dual_compositor.cpp
        # Phase F.2.11.x — IDropTarget forwarding for the child HWND.
        # Without it OS drag-drop dies at the child HWND boundary
        # (OLE drag-drop is HWND-scoped; HTTRANSPARENT only redirects
        # mouse hit-tests, not RegisterDragDrop lookups).
        d3d11/d3d11_drop_target.h
        d3d11/d3d11_drop_target.cpp
    )
    target_link_libraries(qcv_render PRIVATE
        ole32.lib    # OleInitialize / RegisterDragDrop / OleUninitialize
        shell32.lib  # DragQueryFileW
    )
    target_link_libraries(qcv_render PRIVATE
        d3d11.lib
        dxgi.lib
        dcomp.lib
        d3dcompiler.lib
    )
endif()
if(NOT APPLE)
    # Vulkan: decode-only consumer on Windows (per F.2.4 plan); the
    # renderer is D3D11. The bridge in src/render/d3d11/ exports NT
    # handles from VkImages and imports them into D3D11 textures, so
    # qcv_render reaches into Vulkan headers — but only for the
    # external-handle path, never for surfaces / swapchains.
    # On Linux this is the placeholder for a future Vulkan render path.
    target_link_libraries(qcv_render PRIVATE
        Vulkan::Vulkan
    )
    if(WIN32)
        # vkGetMemoryWin32HandleKHR + the Win32 external-handle structs
        # live behind this platform gate.
        target_compile_definitions(qcv_render PRIVATE
            VK_USE_PLATFORM_WIN32_KHR
        )
        # Phase F.2.4.3 Stage C.2 — shaderc compiles the GLSL compute
        # shader for YUV→RGB into SPIR-V at app startup. vcpkg's
        # shaderc port doesn't ship the combined "shaderc_combined.lib";
        # we link the individual pieces (shaderc + shaderc_util +
        # glslang + SPIRV + SPIRV-Tools + SPIRV-Tools-opt + the
        # default-resource-limits helper). Order matters for static
        # linking: leaf-most last.
        find_package(unofficial-shaderc CONFIG QUIET)
        if(TARGET unofficial::shaderc::shaderc)
            target_link_libraries(qcv_render PRIVATE
                unofficial::shaderc::shaderc)
            message(STATUS "qcv_render: shaderc via CMake config target")
        else()
            target_link_libraries(qcv_render PRIVATE
                shaderc shaderc_util
                glslang-default-resource-limits glslang SPIRV
                SPIRV-Tools-opt SPIRV-Tools)
            message(STATUS "qcv_render: shaderc via individual libs")
        endif()
    endif()
endif()

# Compile QRhi shaders to .qsb (Qt Shader Bundle) and embed in resources.
# Output path used at runtime: ":/qcv/render/shaders/<name>.qsb"
qt_add_shaders(qcv_render "qcv_render_shaders"
    PREFIX
        "/qcv/render"
    FILES
        shaders/fullscreen.vert
        shaders/compositor.frag
        shaders/passthrough.frag
        shaders/yuv_to_rgb_biplanar.frag
        shaders/yuv_to_rgb_ayuv.frag
)

target_include_directories(qcv_render
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(qcv_render
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        qcv_decode              # PlayerRhiItem holds VideoDecoder*
        qcv_color               # OcioShaderBuilder needs OCIOConfigManager
        # Phase 7.5 A.3: PlayerWindow forwards pointer events to
        # ViewportAnnotator. PUBLIC so the annotator setter exposed
        # in player_window.h is reachable from callers.
        qcv_annotations
    PRIVATE
        Qt6::GuiPrivate           # rhi/qrhi.h — renderer methods (beginPass, etc.)
        Qt6::ShaderToolsPrivate   # rhi/qshaderbaker.h — Phase 2.3 OCIO shader baking
        PkgConfig::OPENCOLORIO    # OCIO C++ API in OcioShaderBuilder
        qcv_audio                 # WaveformStrip → WaveformProbeEngine
)

# qcv_dual link is Apple-only. MetalPlayerRenderer's dual-flow drawFrame
# branch consumes DualCompositor + DualPlaybackController from qcv_dual.
# Linking qcv_dual on non-Apple produces a QML metatype generation cycle
# in Qt 6.11 (cross-module qmltypes resolution loops between
# qcv_dual_json_file_list and qcv_render.lib). The Apple-only gate breaks
# the cycle without regressing macOS. Windows's D3D11PlayerRenderer
# (Phase F.2.3+) will inline its own dual-flow path or use a runtime
# downcast pattern to avoid pulling qcv_dual back into the link.
if(APPLE)
    target_link_libraries(qcv_render PUBLIC qcv_dual)
endif()
