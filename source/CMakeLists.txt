cmake_minimum_required(VERSION 3.20)
project(ump VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
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
endif()

# =============================================================================
# GStreamer Configuration (OPTIONAL)
# =============================================================================
option(WITH_GSTREAMER "Build with GStreamer backend support" OFF)

if(WIN32 AND WITH_GSTREAMER)
    # GStreamer paths
    set(GSTREAMER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/gstreamer")
    set(GSTREAMER_INCLUDE_DIR "${GSTREAMER_ROOT}/include")
    set(GSTREAMER_LIB_DIR "${GSTREAMER_ROOT}/lib")
    set(GSTREAMER_BIN_DIR "${GSTREAMER_ROOT}/bin")
    set(GSTREAMER_PLUGIN_DIR "${GSTREAMER_ROOT}/plugins")

    # Verify GStreamer installation (headers are in gstreamer-1.0 subdirectory)
    if(EXISTS "${GSTREAMER_INCLUDE_DIR}/gstreamer-1.0/gst/gst.h")
        set(GSTREAMER_FOUND TRUE)
        message(STATUS "GStreamer found at: ${GSTREAMER_ROOT}")
    else()
        set(GSTREAMER_FOUND FALSE)
        message(WARNING "GStreamer requested but not found at: ${GSTREAMER_ROOT}")
        message(STATUS "  Expected header: ${GSTREAMER_INCLUDE_DIR}/gstreamer-1.0/gst/gst.h")
        message(STATUS "  Download from: https://gstreamer.freedesktop.org/download/")
        set(WITH_GSTREAMER OFF CACHE BOOL "GStreamer not available" FORCE)
    endif()
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


# Create executable
add_executable(${PROJECT_NAME} WIN32 ${SOURCES}
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
    "src/player/video_decoder_factory.cpp"
    "src/player/gstreamer_manager.h"
    "src/player/gstreamer_manager.cpp"
    "src/player/gstreamer_video_decoder.h"
    "src/player/gstreamer_video_decoder.cpp"
    "src/player/streaming_video_decoder.h"
    "src/player/streaming_video_decoder.cpp"
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
    "src/ui/playlist_panel.h"
    "src/ui/playlist_panel.cpp"
    "src/player/playlist_controller.h"
    "src/player/playlist_controller.cpp"
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
    # D3D11 OCIO renderer (Windows)
    "src/color/d3d11_ocio_renderer.h"
    "src/color/d3d11_ocio_renderer.cpp"
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
    ${CMAKE_CURRENT_SOURCE_DIR}/external/miniaudio
    ${CMAKE_BINARY_DIR}/_deps/libharu-src/include
    ${CMAKE_BINARY_DIR}/_deps/libharu-build/include
    # OpenTimelineIO headers
    ${CMAKE_BINARY_DIR}/_deps/opentimelineio-src/src
)

# GStreamer include directories (conditional)
if(WITH_GSTREAMER AND GSTREAMER_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${GSTREAMER_INCLUDE_DIR}
        ${GSTREAMER_INCLUDE_DIR}/gstreamer-1.0
        ${GSTREAMER_INCLUDE_DIR}/glib-2.0
        ${GSTREAMER_LIB_DIR}/glib-2.0/include
    )
endif()

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

# GStreamer link directories (conditional)
if(WITH_GSTREAMER AND GSTREAMER_FOUND)
    target_link_directories(${PROJECT_NAME} PRIVATE
        ${GSTREAMER_LIB_DIR}
        ${GSTREAMER_LIB_DIR}/gstreamer-1.0
    )
endif()

# Enable OpenTimelineIO support
target_compile_definitions(${PROJECT_NAME} PRIVATE USE_OPENTIMELINEIO)

# Enable GStreamer support (conditional)
if(WITH_GSTREAMER AND GSTREAMER_FOUND)
    target_compile_definitions(${PROJECT_NAME} PRIVATE WITH_GSTREAMER)
    message(STATUS "GStreamer backend enabled")
endif()

# Enable Python adapters for AAF/XML import (if available)
if(UMP_PYTHON_ADAPTERS_AVAILABLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE USE_PYTHON_ADAPTERS)
    target_link_libraries(${PROJECT_NAME} python_embed)
    message(STATUS "Python adapters enabled for AAF/XML import")
endif()

# Link libraries
target_link_libraries(${PROJECT_NAME}
    OpenGL::GL
    glfw
    glad
    imgui
    implot
    nfd
    opentimelineio                                                      # OpenTimelineIO
    "${CMAKE_CURRENT_SOURCE_DIR}/external/ocio/lib/OpenColorIO.lib"    # OpenColorIO 2.5
    # FFmpeg libraries
    avcodec
    avformat
    avutil
    avfilter
    swscale
    swresample
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
    )
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

# GStreamer libraries (conditional)
if(WITH_GSTREAMER AND GSTREAMER_FOUND)
    target_link_libraries(${PROJECT_NAME}
        gstreamer-1.0
        gstbase-1.0
        gstvideo-1.0
        gstaudio-1.0
        gstapp-1.0
        gstpbutils-1.0
        glib-2.0
        gobject-2.0
        gmodule-2.0
    )
    message(STATUS "Linking GStreamer libraries")
endif()

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

    # Copy Python distribution for AAF/XML import (if available)
    if(UMP_PYTHON_ADAPTERS_AVAILABLE)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/external/python311"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/python311"
            COMMENT "Copying embedded Python distribution for AAF/XML import"
        )
        # Also copy python311.dll to main exe directory for runtime linking
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/external/python311/python311.dll"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/python311.dll"
            COMMENT "Copying python311.dll to executable directory"
        )
        # Copy our custom AAF mob resolver script (overwrites any bundled version)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/resolve_aaf_mobs.py"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/python311/Lib/site-packages/resolve_aaf_mobs.py"
            COMMENT "Copying resolve_aaf_mobs.py script"
        )
    endif()

    # Copy GStreamer DLLs and plugins (conditional) - WHITELIST approach for smaller distribution
    if(WITH_GSTREAMER AND GSTREAMER_FOUND)
        # =========================================================================
        # GStreamer Core DLLs - only what we actually need
        # =========================================================================
        set(GSTREAMER_REQUIRED_DLLS
            # GStreamer core
            gstreamer-1.0-0.dll
            gstbase-1.0-0.dll
            gstapp-1.0-0.dll
            gstaudio-1.0-0.dll
            gstvideo-1.0-0.dll
            gstpbutils-1.0-0.dll
            gsttag-1.0-0.dll
            gstriff-1.0-0.dll
            gstfft-1.0-0.dll
            gstgl-1.0-0.dll
            gstcontroller-1.0-0.dll
            gstallocators-1.0-0.dll
            gstcodecparsers-1.0-0.dll
            gstcodecs-1.0-0.dll
            gstrtp-1.0-0.dll        # Required by isomp4 (MP4/MOV) plugin
            gstd3d11-1.0-0.dll      # D3D11 helper library (needed by mediafoundation plugin)
            gstd3dshader-1.0-0.dll  # D3D11 shader library (needed by mediafoundation plugin)
            gstdxva-1.0-0.dll       # DXVA helper library (needed by mediafoundation plugin)
            # GLib dependencies
            glib-2.0-0.dll
            gobject-2.0-0.dll
            gmodule-2.0-0.dll
            gio-2.0-0.dll
            gthread-2.0-0.dll
            intl-8.dll
            ffi-7.dll
            pcre2-8-0.dll
            z-1.dll
            # Runtime dependencies
            orc-0.4-0.dll
            libwinpthread-1.dll
            libgcc_s_seh-1.dll
            libstdc++-6.dll
            libiconv-2.dll
            libcharset-1.dll
            # GStreamer's bundled FFmpeg (for libav plugin)
            avcodec-61.dll
            avformat-61.dll
            avutil-59.dll
            avfilter-10.dll
            swresample-5.dll
            swscale-8.dll
            # Audio libraries
            ogg-0.dll
            vorbis-0.dll
            vorbisenc-2.dll
            vorbisfile-3.dll
            FLAC-8.dll
            opus-0.dll
            libmpg123-1.dll
            mp3lame-0.dll
            # Other codec support
            dav1d.dll
            x264-164.dll
            x265.dll
            openh264-7.dll
            openjp2.dll
            png16.dll
            jpeg8.dll
            turbojpeg.dll
            bz2.dll
            xml2-2.dll
            # Subtitle support
            ass-9.dll
            fontconfig-1.dll
            freetype-6.dll
            fribidi-0.dll
            harfbuzz.dll
        )

        foreach(DLL_NAME ${GSTREAMER_REQUIRED_DLLS})
            set(DLL_PATH "${GSTREAMER_BIN_DIR}/${DLL_NAME}")
            if(EXISTS "${DLL_PATH}")
                add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DLL_PATH}"
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/"
                    COMMENT "Copying GStreamer DLL: ${DLL_NAME}"
                )
            else()
                message(STATUS "GStreamer DLL not found (skipping): ${DLL_NAME}")
            endif()
        endforeach()

        # =========================================================================
        # GStreamer Plugins - only what we actually need
        # =========================================================================
        set(GSTREAMER_REQUIRED_PLUGINS
            # Core elements (filesrc, queue, tee, etc.)
            gstcoreelements.dll
            # Playback (decodebin, playbin, uridecodebin)
            gstplayback.dll
            # Auto-detection (autoaudiosink, autovideosink)
            gstautodetect.dll
            # App integration (appsink, appsrc)
            gstapp.dll
            # Audio processing
            gstaudioconvert.dll
            gstaudioresample.dll
            gstvolume.dll
            gstaudiomixer.dll
            gstaudioparsers.dll
            gstaudiorate.dll
            # Video processing
            gstvideoconvertscale.dll
            gstvideofilter.dll
            gstvideorate.dll
            gstvideoparsersbad.dll
            # FFmpeg codecs (most important - handles most formats)
            gstlibav.dll
            # Container formats
            gstisomp4.dll
            gstmatroska.dll
            gstavi.dll
            gstflv.dll
            gstogg.dll
            gstmxf.dll
            gstwavparse.dll
            gstaiff.dll
            gstasf.dll
            # Audio codecs
            gstflac.dll
            gstmpg123.dll
            gstvorbis.dll
            gstopus.dll
            gsta52dec.dll
            gstdtsdec.dll
            gstalaw.dll
            gstmulaw.dll
            # Video codecs
            gstdav1d.dll
            gstopenh264.dll
            gstx264.dll
            gstx265.dll
            gstjpeg.dll
            gstpng.dll
            gstvpx.dll
            gstmediafoundation.dll  # Windows Media Foundation (HEVC via mfh265dec)
            gstd3d11.dll            # D3D11 plugin (needed for MF plugin to register elements)
            # Windows audio output
            gstdirectsound.dll
            gstwasapi.dll
            gstwasapi2.dll
            # Typefinders and utilities
            gsttypefindfunctions.dll
            gstgio.dll
            gstdebug.dll
            # Image sequence support
            gstvideobox.dll
            gstimagefreeze.dll
            # Deinterlacing
            gstdeinterlace.dll
            # GIF support
            gstgif.dll
            # Subtitle support
            gstsubparse.dll
            gstassrender.dll
            gstclosedcaption.dll
            gstdvbsuboverlay.dll
            gstdvdsub.dll
        )

        # Create plugins directory
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/gst-plugins"
            COMMENT "Creating GStreamer plugins directory"
        )

        foreach(PLUGIN_NAME ${GSTREAMER_REQUIRED_PLUGINS})
            set(PLUGIN_PATH "${GSTREAMER_PLUGIN_DIR}/${PLUGIN_NAME}")
            if(EXISTS "${PLUGIN_PATH}")
                add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${PLUGIN_PATH}"
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/gst-plugins/"
                    COMMENT "Copying GStreamer plugin: ${PLUGIN_NAME}"
                )
            else()
                message(STATUS "GStreamer plugin not found (skipping): ${PLUGIN_NAME}")
            endif()
        endforeach()

        # Delete old registry to force fresh scan after build (prevents stale cache issues)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E remove -f "$<TARGET_FILE_DIR:${PROJECT_NAME}>/gst-registry.bin"
            COMMENT "Clearing GStreamer registry cache"
        )

        list(LENGTH GSTREAMER_REQUIRED_DLLS GST_DLL_COUNT)
        list(LENGTH GSTREAMER_REQUIRED_PLUGINS GST_PLUGIN_COUNT)
        message(STATUS "GStreamer: ${GST_DLL_COUNT} DLLs and ${GST_PLUGIN_COUNT} plugins will be copied (whitelist mode)")
    endif()

endif()
