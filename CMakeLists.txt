cmake_minimum_required(VERSION 3.20)
project(ump VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set default build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Windows-specific settings
if(MSVC)
    # Use static runtime for Release builds
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")

    # Set Windows subsystem
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")

    # Enable UTF-8 support
    add_compile_options(/utf-8)

    # Disable C++20 char8_t breaking change (allows u8"" literals to work with char* APIs)
    add_compile_options(/Zc:char8_t-)

    # Disable min/max macros from Windows headers (for OCIO 2.5 compatibility)
    add_compile_definitions(NOMINMAX)

    # Disable specific warnings if needed
    add_compile_options(/wd4996)  # Disable deprecated warnings
endif()

# Find packages
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# Set paths for dependencies on Windows
if(WIN32)
    # FFmpeg paths
    set(FFMPEG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg/include")
    set(FFMPEG_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg/lib")
    set(FFMPEG_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg/bin")

    # Note: LibMPV removed - using D3D11VideoDecoder instead
endif()

# Add subdirectory for external dependencies
add_subdirectory(external)

get_property(all_targets DIRECTORY external PROPERTY BUILDSYSTEM_TARGETS)
message(STATUS "Available targets from external: ${all_targets}")

# Collect source files
file(GLOB_RECURSE SOURCES
    "src/*.cpp"
    "src/*.h"
)

# Exclude OLD files
list(FILTER SOURCES EXCLUDE REGEX ".*_OLD\\.(cpp|h)$")

# Exclude broken hdr_direct_decoder (failed FFmpeg D3D11VA approach, replaced by MF HDR)
list(FILTER SOURCES EXCLUDE REGEX ".*hdr_direct_decoder\\.(cpp|h)$")


# Create executable
add_executable(${PROJECT_NAME} WIN32 ${SOURCES}
    "src/ump.manifest"
    "src/utils/exiftool_helper.h"
    "src/utils/exiftool_helper.cpp"
    "src/project/media_item.h"
    "src/project/project_manager.h"
    "src/project/project_manager.cpp"
    "src/utils/system_pressure_monitor.h"
    "src/utils/system_pressure_monitor.cpp"
    "src/metadata/video_metadata.cpp"
    "src/metadata/exr_metadata.h"
    "src/metadata/exr_metadata.cpp"
    "src/metadata/ffmpeg_metadata_extractor.h"
    "src/metadata/ffmpeg_metadata_extractor.cpp"
    "src/player/media_background_extractor.h"
    "src/player/media_background_extractor.cpp"
    "src/player/conversion_strategy.cpp"
    "src/player/image_sequence_config.h"
    "src/player/image_sequence_config.cpp"
    "src/player/direct_exr_cache.h"
    "src/player/direct_exr_cache.cpp"
    "src/player/shared_memory_pool.h"
    "src/player/shared_memory_pool.cpp"
    "src/player/exr_transcoder.h"
    "src/player/exr_transcoder.cpp"
    "src/gpu/texture_pool.h"
    "src/gpu/texture_pool.cpp"
    "src/player/image_loaders.h"
    "src/player/image_loaders.cpp"
    "src/player/video_decoder_interface.h"
    "src/player/video_decoder_factory.h"
    # video_decoder_factory.cpp removed - now a header-only stub
    # streaming_video_decoder.cpp removed - now a header-only stub (D3D11VideoDecoder is the primary video path)
    "src/player/streaming_video_decoder.h"
    "src/player/image_sequence_decoder.h"
    "src/player/image_sequence_decoder.cpp"
    "src/player/hw_context_manager.h"
    "src/player/hw_context_manager.cpp"
    "src/player/scrub_decoder.h"
    "src/player/scrub_decoder.cpp"
    "src/player/thumbnail_cache.h"
    "src/player/thumbnail_cache.cpp"
    "src/player/playback_timer.h"
    "src/player/playback_timer.cpp"
    # FFmpeg + D3D11VA HDR video decoder (Windows only)
    "src/player/d3d11va_video_decoder.h"
    "src/player/d3d11va_video_decoder.cpp"
    # D3D11 GPU-native video decoder implementing IVideoDecoder (Windows only)
    "src/player/d3d11_video_decoder.h"
    "src/player/d3d11_video_decoder.cpp"
    "src/annotations/annotation_note.h"
    "src/annotations/annotation_manager.h"
    "src/annotations/annotation_manager.cpp"
    "src/annotations/annotation_io.h"
    "src/annotations/annotation_io.cpp"
    "src/annotations/stroke_smoother.h"
    "src/annotations/stroke_smoother.cpp"
    "src/annotations/viewport_annotator.h"
    "src/annotations/viewport_annotator.cpp"
    "src/annotations/annotation_toolbar.h"
    "src/annotations/annotation_toolbar.cpp"
    "src/annotations/annotation_renderer.h"
    "src/annotations/annotation_renderer.cpp"
    "src/annotations/annotation_serializer.h"
    "src/annotations/annotation_serializer.cpp"
    "src/annotations/annotation_exporter.h"
    "src/annotations/annotation_exporter.cpp"
    "src/annotations/markdown_parser.h"
    "src/annotations/markdown_parser.cpp"
    "src/annotations/ink_stroke_modeler_wrapper.h"
    "src/annotations/ink_stroke_modeler_wrapper.cpp"
    "src/annotations/nanovg_context.h"
    "src/annotations/nanovg_context.cpp"
    "src/ui/annotation_panel.h"
    "src/ui/annotation_panel.cpp"
    "src/integrations/frameio_url_parser.h"
    "src/integrations/frameio_url_parser.cpp"
    "src/integrations/frameio_client.h"
    "src/integrations/frameio_client.cpp"
    "src/integrations/frameio_converter.h"
    "src/integrations/frameio_converter.cpp"
    # Transcode system
    "src/transcode/sequential_frame_loader.h"
    "src/transcode/sequential_frame_loader.cpp"
    "src/transcode/ocio_cpu_transform.h"
    "src/transcode/ocio_cpu_transform.cpp"
    "src/transcode/ocio_lut_baker.h"
    "src/transcode/ocio_lut_baker.cpp"
    "src/transcode/ffmpeg_video_encoder.h"
    "src/transcode/ffmpeg_video_encoder.cpp"
    "src/transcode/video_transcoder.h"
    "src/transcode/video_transcoder.cpp"
    "src/transcode/transcode_job.h"
    "src/transcode/transcode_job.cpp"
    "src/transcode/transcode_queue.h"
    "src/transcode/transcode_queue.cpp"
    "src/transcode/transcode_worker_pool.h"
    "src/transcode/transcode_worker_pool.cpp"
    "src/ui/transcode_queue_window.h"
    "src/ui/transcode_queue_window.cpp"
    "src/project/project_manager_transcode.cpp"
    # HDR output via D3D11 (Windows)
    "src/hdr/d3d11_hdr_swapchain.h"
    "src/hdr/d3d11_hdr_swapchain.cpp"
    "src/hdr/hdr_output_manager.h"
    "src/hdr/hdr_output_manager.cpp"
    # D3D11 rendering infrastructure (Windows)
    "src/gpu/d3d11_device_manager.h"
    "src/gpu/d3d11_device_manager.cpp"
    "src/gpu/d3d11_texture_pool.h"
    "src/gpu/d3d11_texture_pool.cpp"
    # Unified dual view compositor (Windows)
    "src/gpu/dual_view_layout.h"
    "src/gpu/dual_view_layout.cpp"
    "src/gpu/d3d11_dual_compositor.h"
    "src/gpu/d3d11_dual_compositor.cpp"
    "src/player/dual_view_pipeline.h"
    "src/player/dual_view_pipeline.cpp"
    # Frame readback and CPU frame pool (for single-decoder dual view)
    "src/gpu/frame_readback.h"
    "src/gpu/frame_readback.cpp"
    "src/player/cpu_frame_pool.h"
    "src/player/cpu_frame_pool.cpp"
    # Unified dual view (single decoder, mpv-style)
    "src/player/unified_dual_view.h"
    "src/player/unified_dual_view.cpp"
    # D3D11 OCIO renderer (Windows)
    "src/color/d3d11_ocio_renderer.h"
    "src/color/d3d11_ocio_renderer.cpp"
    # Playlist single decoder (mpv EDL-style, Windows)
    "src/timeline/playlist_single_decoder.h"
    "src/timeline/playlist_single_decoder.cpp"
    # Windows application icon
    "resources/ump.rc"
)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/imnodes
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/external/glfw/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/glad/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/external/implot
    ${CMAKE_CURRENT_SOURCE_DIR}/external/glm
    ${FFMPEG_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/external/ocio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/include/OpenEXR
    ${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/include/Imath
    ${CMAKE_CURRENT_SOURCE_DIR}/external/tiff/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/png/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/jpeg/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/nlohmann
    ${CMAKE_BINARY_DIR}/_deps/libharu-src/include
    ${CMAKE_BINARY_DIR}/_deps/libharu-build/include
    # OpenTimelineIO headers
    ${CMAKE_BINARY_DIR}/_deps/opentimelineio-src/src
    # ink-stroke-modeler headers
    ${CMAKE_BINARY_DIR}/_deps/ink_stroke_modeler-src
    # imgui_markdown (single-header Markdown rendering for ImGui)
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui_markdown
)

# Link directories for Windows
if(WIN32)
    target_link_directories(${PROJECT_NAME} PRIVATE
        ${FFMPEG_LIB_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/external/tiff/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/external/png/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/external/jpeg/lib
    )
endif()

# Enable OpenTimelineIO support
target_compile_definitions(${PROJECT_NAME} PRIVATE USE_OPENTIMELINEIO)

# Link libraries
target_link_libraries(${PROJECT_NAME}
    OpenGL::GL
    glfw
    glad
    imgui
    implot
    nanovg                                                              # NanoVG vector graphics
    nfd
    opentimelineio                                                      # OpenTimelineIO
    InkStrokeModeler::stroke_modeler                                    # Google's stroke smoothing library
    "${CMAKE_CURRENT_SOURCE_DIR}/external/ocio/lib/OpenColorIO.lib"    # OpenColorIO 2.5
    # FFmpeg libraries
    avcodec
    avformat
    avutil
    avfilter
    swscale
    swresample
    # Note: LibMPV removed - using D3D11VideoDecoder
    # OpenEXR C++ API for direct EXR loading (100% OIIO-free)
    Imath-3_2          # Must be first for half-float support
    Iex-3_4
    IlmThread-3_4
    OpenEXRCore-3_4
    OpenEXRUtil-3_4
    OpenEXR-3_4        # C++ API for DWAB compression
    # Image format libraries (native loaders)
    tiff               # libtiff for TIFF sequences
    libpng16           # libpng for PNG sequences
    jpeg               # libjpeg-turbo for JPEG sequences
    # PDF generation
    hpdf               # libharu for PDF export
)

# Platform-specific libraries
if(WIN32)
    target_link_libraries(${PROJECT_NAME}
        opengl32
        winmm
        dwmapi
        setupapi
        version
        winhttp     # For Frame.io API client
        dxgi        # For GPU/VRAM monitoring
        d3d11       # For D3D11 rendering and HDR swapchain
        d3dcompiler # For HLSL shader compilation
        dxguid      # For D3D11 GUIDs
        dcomp       # For DirectComposition (HDR over OpenGL window)
        avrt        # For MMCSS (Multimedia Class Scheduler Service) thread priority
        ole32       # For COM (WASAPI audio)
    )

    # SoundTouch for pitch-preserving time stretch (tempo control without pitch shift)
    # Used by AudioTimeStretch for adaptive speed control during playback
    find_package(SoundTouch QUIET)
    if(SoundTouch_FOUND)
        target_link_libraries(${PROJECT_NAME} PRIVATE SoundTouch::SoundTouch)
        target_compile_definitions(${PROJECT_NAME} PRIVATE UMP_HAS_SOUNDTOUCH)
        message(STATUS "SoundTouch: Found via CMake package")
    else()
        # Manual path fallback - user should place SoundTouch in external/soundtouch
        set(SOUNDTOUCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/soundtouch")
        if(EXISTS "${SOUNDTOUCH_DIR}/include/SoundTouch.h")
            target_include_directories(${PROJECT_NAME} PRIVATE ${SOUNDTOUCH_DIR}/include)
            target_link_libraries(${PROJECT_NAME} PRIVATE ${SOUNDTOUCH_DIR}/lib/SoundTouch.lib)
            target_compile_definitions(${PROJECT_NAME} PRIVATE UMP_HAS_SOUNDTOUCH)
            message(STATUS "SoundTouch: Found in external/soundtouch")
        else()
            message(STATUS "SoundTouch: NOT FOUND - tempo control will be disabled (audio will pause during throttle)")
        endif()
    endif()
elseif(APPLE)
    find_library(COCOA_LIBRARY Cocoa)
    find_library(IOKIT_LIBRARY IOKit)
    find_library(COREVIDEO_LIBRARY CoreVideo)
    target_link_libraries(${PROJECT_NAME}
        ${COCOA_LIBRARY}
        ${IOKIT_LIBRARY}
        ${COREVIDEO_LIBRARY}
    )
endif()

target_link_libraries(${PROJECT_NAME} Threads::Threads)

# Set target
set_property(TARGET ${PROJECT_NAME} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Copy shaders (only if directory has files)
if(SHADER_FILES)
    message(STATUS "Will copy shaders: ${SHADER_FILES}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders
    )
endif()

# Copy assets
message(STATUS "Setting up asset copying for all build configurations")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
    COMMENT "Copying assets to output directory"
)

# Copy FFmpeg and other runtime dependencies
if(WIN32)
    # Copy FFmpeg executable and DLLs
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg/bin/ffmpeg.exe"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/"
        COMMENT "Copying FFmpeg executable"
    )

    # Copy FFmpeg DLLs
    set(FFMPEG_DLLS
        "avcodec-62.dll"
        "avdevice-62.dll"
        "avfilter-11.dll"
        "avformat-62.dll"
        "avutil-60.dll"
        "swresample-6.dll"
        "swscale-9.dll"
    )

    foreach(DLL_NAME ${FFMPEG_DLLS})
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg/bin/${DLL_NAME}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/"
            COMMENT "Copying FFmpeg DLL: ${DLL_NAME}"
        )
    endforeach()

    # Note: LibMPV DLL copy removed - using D3D11VideoDecoder

    # Copy OCIO DLLs - using explicit list for reliability across Debug/Release
    # Hybrid approach: Core API + C++ API for DWAB support
    set(REQUIRED_DLLS
        "Iex-3_4.dll"
        "IlmThread-3_4.dll"
        "Imath-3_2.dll"
        "OpenColorIO_2_5.dll"
        "OpenEXR-3_4.dll"        # C++ API for DWAB compression (direct, no OIIO)
        "OpenEXRCore-3_4.dll"
        "OpenEXRUtil-3_4.dll"
        "bz2.dll"
        "deflate.dll"
        "exiv2.dll"
        "fmt.dll"
        "jpeg62.dll"
        "libexpat.dll"
        "liblzma.dll"
        "libpng16.dll"
        "tiff.dll"
        "turbojpeg.dll"
        "yaml-cpp.dll"
        "zlib1.dll"
        "zstd.dll"
    )

    foreach(DLL_NAME ${REQUIRED_DLLS})
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/external/ocio/bin/${DLL_NAME}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/"
            COMMENT "Copying required DLL: ${DLL_NAME}"
        )
    endforeach()

    # Copy openjph DLL from vcpkg (JPEG 2000 HTJ2K support)
    set(VCPKG_BIN_DIR "C:/vcpkg/vcpkg/installed/x64-windows/bin")
    if(EXISTS "${VCPKG_BIN_DIR}/openjph.0.25.dll")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VCPKG_BIN_DIR}/openjph.0.25.dll"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/"
            COMMENT "Copying openjph DLL"
        )
    endif()

    # OIIO removed - using direct OpenEXR API only

endif()
