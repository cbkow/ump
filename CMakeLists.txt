cmake_minimum_required(VERSION 3.20)
project(qcview VERSION 1.0.4 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# GCC/Clang compatibility flags for code written targeting MSVC
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-fno-char8_t -fpermissive)
endif()

# Set default build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ==========================================================================
# Platform detection and Vulkan option
# ==========================================================================
if(UNIX AND NOT APPLE)
    set(QCVIEW_LINUX TRUE)
    option(QCVIEW_USE_VULKAN "Use Vulkan rendering backend (Linux)" ON)
    message(STATUS "Platform: Linux")
    if(QCVIEW_USE_VULKAN)
        message(STATUS "Rendering backend: Vulkan")
        add_compile_definitions(QCVIEW_USE_VULKAN)
    endif()
else()
    set(QCVIEW_LINUX FALSE)
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

# ==========================================================================
# Find packages
# ==========================================================================
if(NOT QCVIEW_LINUX OR NOT QCVIEW_USE_VULKAN)
    find_package(OpenGL REQUIRED)
endif()
find_package(Threads REQUIRED)

# Vulkan on Linux
if(QCVIEW_LINUX AND QCVIEW_USE_VULKAN)
    find_package(Vulkan REQUIRED)
    message(STATUS "Vulkan found: ${Vulkan_LIBRARIES}")
    message(STATUS "Vulkan include: ${Vulkan_INCLUDE_DIRS}")

    # Find shaderc for runtime GLSL->SPIR-V compilation (ships with Vulkan SDK)
    # Prefer shared library to avoid static dependency chain (glslang, SPIRV-Tools)
    find_library(SHADERC_LIB shaderc_shared HINTS ${Vulkan_INCLUDE_DIRS}/../lib)
    if(NOT SHADERC_LIB)
        find_library(SHADERC_LIB shaderc HINTS ${Vulkan_INCLUDE_DIRS}/../lib)
    endif()
    if(NOT SHADERC_LIB)
        find_library(SHADERC_LIB shaderc_combined HINTS ${Vulkan_INCLUDE_DIRS}/../lib)
    endif()
    if(SHADERC_LIB)
        message(STATUS "shaderc found: ${SHADERC_LIB}")
    else()
        message(STATUS "shaderc not found - runtime shader compilation will not be available")
    endif()
endif()

# Set paths for dependencies on Windows
if(WIN32)
    # FFmpeg paths
    set(FFMPEG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg/include")
    set(FFMPEG_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg/lib")
    set(FFMPEG_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg/bin")

endif()

# Linux: Find system packages via pkg-config
if(QCVIEW_LINUX)
    find_package(PkgConfig REQUIRED)

    # FFmpeg
    pkg_check_modules(FFMPEG REQUIRED
        libavcodec
        libavformat
        libavutil
        libavfilter
        libswscale
        libswresample
    )

    # OpenColorIO 2.5 (vendored, matching Windows build)
    set(OCIO_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/external/ocio/linux/include")
    set(OCIO_LIBRARY_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/external/ocio/linux/lib")
    set(OCIO_LIBRARIES "OpenColorIO")

    # OpenEXR
    pkg_check_modules(OPENEXR REQUIRED OpenEXR)
    pkg_check_modules(IMATH REQUIRED Imath)

    # Image format libraries
    pkg_check_modules(TIFF REQUIRED libtiff-4)
    pkg_check_modules(PNG REQUIRED libpng)
    pkg_check_modules(JPEG REQUIRED libjpeg)

    # SoundTouch
    pkg_check_modules(SOUNDTOUCH soundtouch)
    if(SOUNDTOUCH_FOUND)
        message(STATUS "SoundTouch: Found via pkg-config")
    else()
        message(STATUS "SoundTouch: NOT FOUND - tempo control will be disabled")
    endif()

    # PipeWire (Linux audio output)
    pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
    if(PIPEWIRE_FOUND)
        message(STATUS "PipeWire: Found (${PIPEWIRE_VERSION})")
    endif()

    # VA-API + DRM (for HW decode DMA-BUF import)
    pkg_check_modules(VAAPI libva libva-drm)
    pkg_check_modules(DRM libdrm)
    if(VAAPI_FOUND AND DRM_FOUND)
        message(STATUS "VA-API + DRM: Found (DMA-BUF import enabled)")
    else()
        message(STATUS "VA-API/DRM: NOT FOUND - HW decode will use CPU fallback")
    endif()

    # Wayland client (for native clipboard support)
    pkg_check_modules(WAYLAND_CLIENT wayland-client)
    if(WAYLAND_CLIENT_FOUND)
        message(STATUS "wayland-client: Found (native clipboard support)")
    else()
        message(STATUS "wayland-client: NOT FOUND - clipboard may not work on Wayland")
    endif()

    # libcurl (for Frame.io HTTP client)
    pkg_check_modules(CURL libcurl)
    if(CURL_FOUND)
        message(STATUS "libcurl: Found (Frame.io integration enabled)")
    else()
        message(STATUS "libcurl: NOT FOUND - Frame.io integration disabled")
    endif()

    # sd-bus (for XDG Desktop Portal accent color query)
    pkg_check_modules(SYSTEMD libsystemd)
    if(SYSTEMD_FOUND)
        message(STATUS "libsystemd: Found (accent color via D-Bus portal)")
    else()
        message(STATUS "libsystemd: NOT FOUND - system accent color unavailable")
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

# ==========================================================================
# Platform-specific source file filtering
# ==========================================================================
if(QCVIEW_LINUX)
    # Exclude Windows-only D3D11 files on Linux
    # These are guarded with #ifdef _WIN32 but we exclude them entirely to avoid
    # pulling in Windows headers during compilation
    list(FILTER SOURCES EXCLUDE REGEX ".*d3d11.*")
    list(FILTER SOURCES EXCLUDE REGEX ".*d3d11va.*")
    list(FILTER SOURCES EXCLUDE REGEX ".*wasapi.*")
    list(FILTER SOURCES EXCLUDE REGEX ".*d3d11_hdr_swapchain.*")
    list(FILTER SOURCES EXCLUDE REGEX ".*hdr_output_manager.*")
    list(FILTER SOURCES EXCLUDE REGEX ".*qcview\\.manifest$")
    list(FILTER SOURCES EXCLUDE REGEX ".*qcview\\.rc$")
    # Exclude disabled CUDA/OpenCL files
    list(FILTER SOURCES EXCLUDE REGEX ".*\\.disabled$")
    message(STATUS "Linux build: Excluded D3D11/WASAPI/Windows-only sources")
elseif(WIN32)
    # Exclude Vulkan files on Windows
    list(FILTER SOURCES EXCLUDE REGEX ".*vulkan_.*")
    list(FILTER SOURCES EXCLUDE REGEX ".*pipewire_.*")
    # Exclude disabled files
    list(FILTER SOURCES EXCLUDE REGEX ".*\\.disabled$")
endif()

# Create executable
# Note: GLOB_RECURSE already collects all src/*.cpp/*.h files.
# The explicit file list below is kept for IDE project file generation compatibility.
# On Linux, WIN32 subsystem flag and .manifest/.rc are omitted.
if(WIN32)
    add_executable(${PROJECT_NAME} WIN32 ${SOURCES}
        "src/qcview.manifest"
        "resources/qcview.rc"
    )
elseif(QCVIEW_LINUX)
    add_executable(${PROJECT_NAME} ${SOURCES})
endif()

# Note: vulkan_device.h/cpp are picked up by GLOB_RECURSE and
# not filtered out on Linux, so no explicit target_sources needed.

# Note: GLOB_RECURSE already collected all source files above.
# The platform-specific filtering (list FILTER) handles include/exclude.
# No additional target_sources needed - GLOB + filtering is the source of truth.

# Include directories (cross-platform)
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

# Platform-specific include directories
if(WIN32)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${FFMPEG_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/ocio/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/include/OpenEXR
        ${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/include/Imath
        ${CMAKE_CURRENT_SOURCE_DIR}/external/tiff/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/png/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/jpeg/include
    )
elseif(QCVIEW_LINUX)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${FFMPEG_INCLUDE_DIRS}
        ${OCIO_INCLUDE_DIRS}
        ${OPENEXR_INCLUDE_DIRS}
        ${IMATH_INCLUDE_DIRS}
        ${TIFF_INCLUDE_DIRS}
        ${PNG_INCLUDE_DIRS}
        ${JPEG_INCLUDE_DIRS}
        ${PIPEWIRE_INCLUDE_DIRS}
    )
    # Vulkan + VMA includes
    if(QCVIEW_USE_VULKAN)
        target_include_directories(${PROJECT_NAME} PRIVATE
            ${Vulkan_INCLUDE_DIRS}
            ${CMAKE_CURRENT_SOURCE_DIR}/external/vma
        )
    endif()
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

# Enable OpenTimelineIO support
target_compile_definitions(${PROJECT_NAME} PRIVATE USE_OPENTIMELINEIO)

# ==========================================================================
# Link libraries - Cross-platform core
# ==========================================================================
target_link_libraries(${PROJECT_NAME}
    glfw
    glad
    imgui
    implot
    nanovg                                                              # NanoVG vector graphics
    nfd
    opentimelineio                                                      # OpenTimelineIO
    InkStrokeModeler::stroke_modeler                                    # Google's stroke smoothing library
    # PDF generation
    hpdf                                                                # libharu for PDF export
)

# ==========================================================================
# Platform-specific link libraries
# ==========================================================================
if(WIN32)
    target_link_libraries(${PROJECT_NAME}
        OpenGL::GL
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
        # Windows system libraries
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
        target_link_libraries(${PROJECT_NAME} SoundTouch::SoundTouch)
        target_compile_definitions(${PROJECT_NAME} PRIVATE QCVIEW_HAS_SOUNDTOUCH)
        message(STATUS "SoundTouch: Found via CMake package")
    else()
        # Manual path fallback - user should place SoundTouch in external/soundtouch
        set(SOUNDTOUCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/soundtouch")
        if(EXISTS "${SOUNDTOUCH_DIR}/include/SoundTouch.h")
            target_include_directories(${PROJECT_NAME} PRIVATE ${SOUNDTOUCH_DIR}/include)
            target_link_libraries(${PROJECT_NAME} ${SOUNDTOUCH_DIR}/lib/SoundTouch.lib)
            target_compile_definitions(${PROJECT_NAME} PRIVATE QCVIEW_HAS_SOUNDTOUCH)
            message(STATUS "SoundTouch: Found in external/soundtouch")
        else()
            message(STATUS "SoundTouch: NOT FOUND - tempo control will be disabled (audio will pause during throttle)")
        endif()
    endif()

elseif(QCVIEW_LINUX)
    # System libraries via pkg-config
    target_link_libraries(${PROJECT_NAME}
        ${FFMPEG_LIBRARIES}
        ${OCIO_LIBRARIES}
        ${OPENEXR_LIBRARIES}
        ${IMATH_LIBRARIES}
        ${TIFF_LIBRARIES}
        ${PNG_LIBRARIES}
        ${JPEG_LIBRARIES}
    )
    target_link_directories(${PROJECT_NAME} PRIVATE
        ${FFMPEG_LIBRARY_DIRS}
        ${OCIO_LIBRARY_DIRS}
        ${OPENEXR_LIBRARY_DIRS}
        ${IMATH_LIBRARY_DIRS}
    )

    # GPU backend libraries
    if(QCVIEW_USE_VULKAN)
        target_link_libraries(${PROJECT_NAME} Vulkan::Vulkan)
        if(SHADERC_LIB)
            target_link_libraries(${PROJECT_NAME} ${SHADERC_LIB})
        endif()
    else()
        find_package(OpenGL REQUIRED)
        target_link_libraries(${PROJECT_NAME} OpenGL::GL)
    endif()

    # X11 needed by GLFW/ImGui on Linux (even with Vulkan backend)
    find_package(X11 REQUIRED)
    target_link_libraries(${PROJECT_NAME} ${X11_LIBRARIES})

    # SoundTouch
    if(SOUNDTOUCH_FOUND)
        target_link_libraries(${PROJECT_NAME} ${SOUNDTOUCH_LIBRARIES})
        target_include_directories(${PROJECT_NAME} PRIVATE ${SOUNDTOUCH_INCLUDE_DIRS})
        target_compile_definitions(${PROJECT_NAME} PRIVATE QCVIEW_HAS_SOUNDTOUCH)
    endif()

    # PipeWire (audio output)
    if(PIPEWIRE_FOUND)
        target_link_libraries(${PROJECT_NAME} ${PIPEWIRE_LIBRARIES})
    endif()

    # VA-API + DRM (DMA-BUF import for HW decode)
    if(VAAPI_FOUND AND DRM_FOUND)
        target_link_libraries(${PROJECT_NAME} ${VAAPI_LIBRARIES} ${DRM_LIBRARIES})
        target_include_directories(${PROJECT_NAME} PRIVATE ${VAAPI_INCLUDE_DIRS} ${DRM_INCLUDE_DIRS})
    endif()

    # Wayland client (native clipboard)
    if(WAYLAND_CLIENT_FOUND)
        target_link_libraries(${PROJECT_NAME} ${WAYLAND_CLIENT_LIBRARIES})
        target_include_directories(${PROJECT_NAME} PRIVATE ${WAYLAND_CLIENT_INCLUDE_DIRS})
    endif()

    # libcurl (Frame.io HTTP client)
    if(CURL_FOUND)
        target_link_libraries(${PROJECT_NAME} ${CURL_LIBRARIES})
        target_include_directories(${PROJECT_NAME} PRIVATE ${CURL_INCLUDE_DIRS})
        target_compile_definitions(${PROJECT_NAME} PRIVATE QCVIEW_HAS_CURL)
    endif()

    # libsystemd (sd-bus for XDG portal accent color)
    if(SYSTEMD_FOUND)
        target_link_libraries(${PROJECT_NAME} ${SYSTEMD_LIBRARIES})
        target_include_directories(${PROJECT_NAME} PRIVATE ${SYSTEMD_INCLUDE_DIRS})
        target_compile_definitions(${PROJECT_NAME} PRIVATE QCVIEW_HAS_SDBUS)
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
    set(VCPKG_BIN_DIR "C:/Users/chris/vcpkg/installed/x64-windows/bin")
    if(EXISTS "${VCPKG_BIN_DIR}/openjph.0.26.dll")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VCPKG_BIN_DIR}/openjph.0.26.dll"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/"
            COMMENT "Copying openjph DLL"
        )
    endif()

    # OIIO removed - using direct OpenEXR API only

endif()

# ==========================================================================
# Install rules (Linux packaging)
# ==========================================================================
if(QCVIEW_LINUX)
    include(GNUInstallDirs)

    # Binary
    install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # Assets (OCIO configs, fonts, icons, safety overlays, etc.)
    install(DIRECTORY assets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/qcview/assets)

    # Shaders
    install(DIRECTORY shaders/ DESTINATION ${CMAKE_INSTALL_DATADIR}/qcview/shaders)

    # Vendored OCIO library (matches the headers we built against)
    install(FILES external/ocio/linux/lib/libOpenColorIO.so.2.5.0
            DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink
        libOpenColorIO.so.2.5.0 \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/libOpenColorIO.so.2.5)")

    # Desktop entry
    install(FILES packaging/app.qcview.QCView.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

    # Icons (multiple sizes for different contexts)
    foreach(SIZE 16 32 48 64 128 256 512)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/qcview_${SIZE}.png")
            install(FILES "assets/icons/qcview_${SIZE}.png"
                    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${SIZE}x${SIZE}/apps"
                    RENAME "app.qcview.QCView.png")
        endif()
    endforeach()

    # ======================================================================
    # CPack — generates .deb and .rpm
    # ======================================================================
    set(CPACK_PACKAGE_NAME "qcview")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_FILE_NAME "QCView-${PROJECT_VERSION}-amd64")
    set(CPACK_PACKAGE_CONTACT "cbkow")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "QCView - Professional video and image sequence player")
    set(CPACK_PACKAGE_DESCRIPTION "QCView is a professional video and image sequence player with OCIO color management, HDR output, hardware-accelerated decode, and annotation tools.")
    set(CPACK_PACKAGE_HOMEPAGE_URL "https://qcview.app")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_STRIP_FILES TRUE)

    # DEB-specific
    set(CPACK_DEBIAN_FILE_NAME "QCView-${PROJECT_VERSION}-amd64.deb")
    set(CPACK_DEBIAN_PACKAGE_SECTION "video")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS
        "libglfw3 (>= 3.3), libavcodec-extra | libavcodec60 | libavcodec61, libavformat60 | libavformat61, libavutil58 | libavutil59, libswscale7 | libswscale8, libswresample4 | libswresample5, libopenexr-3-1-30 | libopenexr-3-2-1, libpipewire-0.3-0, libva2, libdrm2, libcurl4 | libcurl4t64, libsoundtouch1, libvulkan1, libwayland-client0, libsystemd0, libtiff6 | libtiff5, libpng16-16 | libpng16-16t64, libjpeg62-turbo | libjpeg-turbo8")

    # RPM-specific
    set(CPACK_RPM_PACKAGE_LICENSE "Proprietary")
    set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
    set(CPACK_RPM_PACKAGE_AUTOREQ ON)
    set(CPACK_RPM_PACKAGE_REQUIRES
        "glfw >= 3.3, ffmpeg-libs, openexr-libs, pipewire-libs, libva, libdrm, libcurl, soundtouch, vulkan-loader, wayland, systemd-libs")

    set(CPACK_GENERATOR "DEB;RPM")
    include(CPack)
endif()
