# ==========================================================================
# QCView Linux Build Configuration
# Vulkan rendering + VAAPI HW decode (DMA-BUF zero-copy), PipeWire audio
# ==========================================================================

message(STATUS "Platform: Linux")

option(QCVIEW_USE_VULKAN "Use Vulkan rendering backend (Linux)" ON)
if(QCVIEW_USE_VULKAN)
    message(STATUS "Rendering backend: Vulkan")
    add_compile_definitions(QCVIEW_USE_VULKAN)
endif()

# GCC/Clang compatibility flags for code written targeting MSVC
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-fno-char8_t -fpermissive)
endif()

# ==========================================================================
# Find Packages
# ==========================================================================
find_package(PkgConfig REQUIRED)

# Vulkan
if(QCVIEW_USE_VULKAN)
    find_package(Vulkan REQUIRED)
    message(STATUS "Vulkan found: ${Vulkan_LIBRARIES}")
    message(STATUS "Vulkan include: ${Vulkan_INCLUDE_DIRS}")

    # Find shaderc for runtime GLSL->SPIR-V compilation (ships with Vulkan SDK)
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
else()
    find_package(OpenGL REQUIRED)
endif()

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

# ==========================================================================
# Source Filtering — exclude Windows-only files
# ==========================================================================
list(FILTER SOURCES EXCLUDE REGEX ".*d3d11.*")
list(FILTER SOURCES EXCLUDE REGEX ".*d3d11va.*")
list(FILTER SOURCES EXCLUDE REGEX ".*wasapi.*")
list(FILTER SOURCES EXCLUDE REGEX ".*d3d11_hdr_swapchain.*")
list(FILTER SOURCES EXCLUDE REGEX ".*hdr_output_manager.*")
list(FILTER SOURCES EXCLUDE REGEX ".*qcview\\.manifest$")
list(FILTER SOURCES EXCLUDE REGEX ".*qcview\\.rc$")
list(FILTER SOURCES EXCLUDE REGEX ".*\\.disabled$")
message(STATUS "Linux build: Excluded D3D11/WASAPI/Windows-only sources")

# ==========================================================================
# Create Executable
# ==========================================================================
add_executable(${PROJECT_NAME} ${SOURCES})

# ==========================================================================
# Include Directories
# ==========================================================================
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
if(QCVIEW_USE_VULKAN)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${Vulkan_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/vma
    )
endif()

# ==========================================================================
# Link Libraries
# ==========================================================================

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

# GPU backend
if(QCVIEW_USE_VULKAN)
    target_link_libraries(${PROJECT_NAME} Vulkan::Vulkan)
    if(SHADERC_LIB)
        target_link_libraries(${PROJECT_NAME} ${SHADERC_LIB})
    endif()
else()
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

# ==========================================================================
# Install rules (DEB/RPM packaging)
# ==========================================================================
include(GNUInstallDirs)

# Binary
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Assets (OCIO configs, fonts, icons, safety overlays, etc.)
install(DIRECTORY assets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/qcview/assets)

# Shaders
install(DIRECTORY shaders/ DESTINATION ${CMAKE_INSTALL_DATADIR}/qcview/shaders)

# Vendored OCIO library (matches the headers we built against)
install(FILES external/ocio/linux/lib/libOpenColorIO.so.2.5.1
        DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink
    libOpenColorIO.so.2.5.1 \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/libOpenColorIO.so.2.5)")

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

# CPack — generates .deb and .rpm
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
