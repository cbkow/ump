# ==========================================================================
# QCView macOS Build Configuration
# Metal rendering + VideoToolbox HW decode, Core Audio output
# ==========================================================================

message(STATUS "Platform: macOS (Apple Silicon)")

# Objective-C++ enabled at project() level in root CMakeLists.txt
set(CMAKE_OBJCXX_STANDARD 20)

# Metal rendering backend
add_compile_definitions(QCVIEW_USE_METAL)
message(STATUS "Rendering backend: Metal")

# Apple Silicon only
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build for Apple Silicon only" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version (Ventura)" FORCE)
message(STATUS "Architecture: arm64 (Apple Silicon), deployment target: macOS 13.0")

# Clang flags for code written targeting MSVC
add_compile_options(-fno-char8_t)

# ==========================================================================
# Find Frameworks
# ==========================================================================
find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(METALKIT_FRAMEWORK MetalKit REQUIRED)
find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
find_library(VIDEOTOOLBOX_FRAMEWORK VideoToolbox REQUIRED)
find_library(COREMEDIA_FRAMEWORK CoreMedia REQUIRED)
find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)

message(STATUS "Metal framework: ${METAL_FRAMEWORK}")
message(STATUS "VideoToolbox framework: ${VIDEOTOOLBOX_FRAMEWORK}")

# ==========================================================================
# Find Packages
# ==========================================================================

# FFmpeg (vendored arm64 dylibs)
set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg/macos")
if(EXISTS "${FFMPEG_ROOT}/lib")
    set(FFMPEG_INCLUDE_DIRS "${FFMPEG_ROOT}/include")
    set(FFMPEG_LIBRARY_DIRS "${FFMPEG_ROOT}/lib")
    set(FFMPEG_LIBRARIES avcodec avformat avutil avfilter swscale swresample)
    message(STATUS "FFmpeg: Using vendored arm64 dylibs from ${FFMPEG_ROOT}")
else()
    # Fallback to pkg-config (Homebrew)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED
        libavcodec
        libavformat
        libavutil
        libavfilter
        libswscale
        libswresample
    )
    message(STATUS "FFmpeg: Using system (pkg-config)")
endif()

# OpenColorIO 2.5 (vendored arm64)
set(OCIO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/ocio/macos")
if(EXISTS "${OCIO_ROOT}/lib")
    set(OCIO_INCLUDE_DIRS "${OCIO_ROOT}/include")
    set(OCIO_LIBRARY_DIRS "${OCIO_ROOT}/lib")
    set(OCIO_LIBRARIES "OpenColorIO")
    message(STATUS "OpenColorIO: Using vendored arm64 from ${OCIO_ROOT}")
else()
    set(OCIO_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/external/ocio/include")
    set(OCIO_LIBRARY_DIRS "")
    set(OCIO_LIBRARIES "")
    message(STATUS "OpenColorIO: Headers only (vendored dylibs not yet built)")
endif()

# OpenEXR, Imath, image libs, SoundTouch — always use vendored (no Homebrew)
find_package(PkgConfig)

set(OPENEXR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/macos")
set(OPENEXR_INCLUDE_DIRS "${OPENEXR_ROOT}/include" "${OPENEXR_ROOT}/include/OpenEXR" "${OPENEXR_ROOT}/include/Imath")
set(OPENEXR_LIBRARY_DIRS "${OPENEXR_ROOT}/lib")
set(OPENEXR_LIBRARIES
    "${OPENEXR_ROOT}/lib/libOpenEXR-3_3.dylib"
    "${OPENEXR_ROOT}/lib/libOpenEXRCore-3_3.dylib"
    "${OPENEXR_ROOT}/lib/libOpenEXRUtil-3_3.dylib"
    "${OPENEXR_ROOT}/lib/libIex-3_3.dylib"
    "${OPENEXR_ROOT}/lib/libIlmThread-3_3.dylib"
)
set(IMATH_INCLUDE_DIRS "${OPENEXR_ROOT}/include" "${OPENEXR_ROOT}/include/Imath")
set(IMATH_LIBRARY_DIRS "${OPENEXR_ROOT}/lib")
set(IMATH_LIBRARIES "${OPENEXR_ROOT}/lib/libImath-3_2.dylib")
message(STATUS "OpenEXR: Vendored arm64 from ${OPENEXR_ROOT}")

set(TIFF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/tiff/macos")
set(TIFF_INCLUDE_DIRS "${TIFF_ROOT}/include")
set(TIFF_LIBRARIES "${TIFF_ROOT}/lib/libtiff.6.dylib")
message(STATUS "libtiff: Vendored from ${TIFF_ROOT}")

set(PNG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/png/macos")
set(PNG_INCLUDE_DIRS "${PNG_ROOT}/include")
set(PNG_LIBRARIES "${PNG_ROOT}/lib/libpng16.dylib")
message(STATUS "libpng: Vendored from ${PNG_ROOT}")

set(JPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/jpeg/macos")
set(JPEG_INCLUDE_DIRS "${JPEG_ROOT}/include")
set(JPEG_LIBRARIES "${JPEG_ROOT}/lib/libjpeg.dylib")
message(STATUS "libjpeg: Vendored from ${JPEG_ROOT}")

set(SOUNDTOUCH_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/soundtouch/macos")
if(EXISTS "${SOUNDTOUCH_ROOT}/lib")
    set(SOUNDTOUCH_FOUND TRUE)
    set(SOUNDTOUCH_INCLUDE_DIRS "${SOUNDTOUCH_ROOT}/include/soundtouch")
    set(SOUNDTOUCH_LIBRARIES "${SOUNDTOUCH_ROOT}/lib/libSoundTouch.dylib")
    message(STATUS "SoundTouch: Vendored from ${SOUNDTOUCH_ROOT}")
else()
    set(SOUNDTOUCH_FOUND FALSE)
    message(STATUS "SoundTouch: NOT FOUND - tempo control will be disabled")
endif()

# libcurl — ships with macOS, no vendoring needed
find_package(CURL)
if(CURL_FOUND)
    message(STATUS "libcurl: Found (system)")
else()
    message(STATUS "libcurl: NOT FOUND - Frame.io integration disabled")
endif()

# ==========================================================================
# Source Filtering — exclude Windows-only and Linux-only files
# ==========================================================================
list(FILTER SOURCES EXCLUDE REGEX ".*d3d11.*")
list(FILTER SOURCES EXCLUDE REGEX ".*d3d11va.*")
list(FILTER SOURCES EXCLUDE REGEX ".*wasapi.*")
list(FILTER SOURCES EXCLUDE REGEX ".*d3d11_hdr_swapchain.*")
list(FILTER SOURCES EXCLUDE REGEX ".*hdr_output_manager.*")
list(FILTER SOURCES EXCLUDE REGEX ".*vulkan_.*")
list(FILTER SOURCES EXCLUDE REGEX ".*pipewire_.*")
list(FILTER SOURCES EXCLUDE REGEX ".*linux_clipboard.*")
list(FILTER SOURCES EXCLUDE REGEX ".*/single_instance\\.(cpp|h)$")
list(FILTER SOURCES EXCLUDE REGEX ".*video_range_converter.*")
list(FILTER SOURCES EXCLUDE REGEX ".*/texture_pool\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX ".*unified_dual_view\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX ".*qcview\\.manifest$")
list(FILTER SOURCES EXCLUDE REGEX ".*qcview\\.rc$")
list(FILTER SOURCES EXCLUDE REGEX ".*\\.disabled$")
message(STATUS "macOS build: Excluded D3D11/WASAPI/Vulkan/PipeWire/Linux-only sources")

# Collect .mm (Objective-C++) source files
file(GLOB_RECURSE MM_SOURCES "src/*.mm")
list(APPEND SOURCES ${MM_SOURCES})
message(STATUS "Found ${MM_SOURCES} Objective-C++ source files")

# ==========================================================================
# App Bundle Configuration
# ==========================================================================
set(MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in")

add_executable(${PROJECT_NAME} MACOSX_BUNDLE ${SOURCES})

# Set bundle properties
set_target_properties(${PROJECT_NAME} PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER "app.qcview.QCView"
    MACOSX_BUNDLE_BUNDLE_NAME "QCView"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "app.qcview.QCView"
)

# ==========================================================================
# Include Directories
# ==========================================================================
target_include_directories(${PROJECT_NAME} PRIVATE
    ${FFMPEG_INCLUDE_DIRS}
    ${OCIO_INCLUDE_DIRS}
    ${OPENEXR_INCLUDE_DIRS}
    ${IMATH_INCLUDE_DIRS}
)

if(TIFF_INCLUDE_DIRS)
    target_include_directories(${PROJECT_NAME} PRIVATE ${TIFF_INCLUDE_DIRS})
endif()
if(PNG_INCLUDE_DIRS)
    target_include_directories(${PROJECT_NAME} PRIVATE ${PNG_INCLUDE_DIRS})
endif()
if(JPEG_INCLUDE_DIRS)
    target_include_directories(${PROJECT_NAME} PRIVATE ${JPEG_INCLUDE_DIRS})
endif()

# ==========================================================================
# Link Libraries
# ==========================================================================

# macOS frameworks
target_link_libraries(${PROJECT_NAME}
    ${METAL_FRAMEWORK}
    ${METALKIT_FRAMEWORK}
    ${QUARTZCORE_FRAMEWORK}
    ${COREAUDIO_FRAMEWORK}
    ${AUDIOTOOLBOX_FRAMEWORK}
    ${VIDEOTOOLBOX_FRAMEWORK}
    ${COREMEDIA_FRAMEWORK}
    ${COREVIDEO_FRAMEWORK}
    ${COCOA_FRAMEWORK}
    ${IOKIT_FRAMEWORK}
    ${APPKIT_FRAMEWORK}
    ${COREFOUNDATION_FRAMEWORK}
)

# FFmpeg
target_link_libraries(${PROJECT_NAME} ${FFMPEG_LIBRARIES})
target_link_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARY_DIRS})

# OpenColorIO
if(OCIO_LIBRARIES)
    target_link_libraries(${PROJECT_NAME} ${OCIO_LIBRARIES})
    if(OCIO_LIBRARY_DIRS)
        target_link_directories(${PROJECT_NAME} PRIVATE ${OCIO_LIBRARY_DIRS})
    endif()
endif()

# OpenEXR + Imath
if(OPENEXR_LIBRARIES)
    target_link_libraries(${PROJECT_NAME} ${OPENEXR_LIBRARIES})
    if(OPENEXR_LIBRARY_DIRS)
        target_link_directories(${PROJECT_NAME} PRIVATE ${OPENEXR_LIBRARY_DIRS})
    endif()
endif()
if(IMATH_LIBRARIES)
    target_link_libraries(${PROJECT_NAME} ${IMATH_LIBRARIES})
    if(IMATH_LIBRARY_DIRS)
        target_link_directories(${PROJECT_NAME} PRIVATE ${IMATH_LIBRARY_DIRS})
    endif()
endif()

# Image format libraries
if(TIFF_LIBRARIES)
    target_link_libraries(${PROJECT_NAME} ${TIFF_LIBRARIES})
endif()
if(PNG_LIBRARIES)
    target_link_libraries(${PROJECT_NAME} ${PNG_LIBRARIES})
endif()
if(JPEG_LIBRARIES)
    target_link_libraries(${PROJECT_NAME} ${JPEG_LIBRARIES})
endif()

# SoundTouch (optional)
if(SOUNDTOUCH_FOUND)
    target_link_libraries(${PROJECT_NAME} ${SOUNDTOUCH_LIBRARIES})
    if(SOUNDTOUCH_INCLUDE_DIRS)
        target_include_directories(${PROJECT_NAME} PRIVATE ${SOUNDTOUCH_INCLUDE_DIRS})
    endif()
    if(SOUNDTOUCH_LIBRARY_DIRS)
        target_link_directories(${PROJECT_NAME} PRIVATE ${SOUNDTOUCH_LIBRARY_DIRS})
    endif()
    target_compile_definitions(${PROJECT_NAME} PRIVATE QCVIEW_HAS_SOUNDTOUCH)
endif()

# libcurl (Frame.io)
if(CURL_FOUND)
    target_link_libraries(${PROJECT_NAME} CURL::libcurl)
    target_compile_definitions(${PROJECT_NAME} PRIVATE QCVIEW_HAS_CURL)
endif()

# ==========================================================================
# Copy resources into app bundle
# ==========================================================================
set(RESOURCE_DIR "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/Resources")
set(FRAMEWORKS_DIR "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/Frameworks")

# Copy app icon
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    "${CMAKE_SOURCE_DIR}/packaging/macos/QCView.icns" "${RESOURCE_DIR}/QCView.icns"
    COMMENT "Copying app icon to app bundle"
)

# Copy assets
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/assets" "${RESOURCE_DIR}/assets"
    COMMENT "Copying assets to app bundle"
)

# Copy shaders (Metal + any shared)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/shaders" "${RESOURCE_DIR}/shaders"
    COMMENT "Copying shaders to app bundle"
)

# Copy ALL vendored dylibs to Frameworks/
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FRAMEWORKS_DIR}"
    # FFmpeg
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${FFMPEG_ROOT}/lib" "${FRAMEWORKS_DIR}"
    # OCIO
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OCIO_ROOT}/lib" "${FRAMEWORKS_DIR}"
    # OpenEXR + Imath
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OPENEXR_ROOT}/lib" "${FRAMEWORKS_DIR}"
    # libtiff, libpng, libjpeg — copy entire lib dirs to preserve symlink chains
    COMMAND bash -c "cp -a ${TIFF_ROOT}/lib/libtiff*.dylib ${FRAMEWORKS_DIR}/"
    COMMAND bash -c "cp -a ${PNG_ROOT}/lib/libpng*.dylib ${FRAMEWORKS_DIR}/"
    COMMAND bash -c "cp -a ${JPEG_ROOT}/lib/libjpeg*.dylib ${FRAMEWORKS_DIR}/"
    COMMENT "Copying vendored dylibs to Frameworks/"
)
# SoundTouch (conditional)
if(SOUNDTOUCH_FOUND)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND bash -c "cp -a ${SOUNDTOUCH_ROOT}/lib/libSoundTouch*.dylib ${FRAMEWORKS_DIR}/"
        COMMENT "Copying SoundTouch dylib to Frameworks/"
    )
endif()

# Fix install names and inter-library references
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/fix_dylib_rpaths.sh
        "${FRAMEWORKS_DIR}"
        "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/MacOS/${PROJECT_NAME}"
    COMMENT "Fixing dylib rpaths"
)

# Set rpath for the executable to find dylibs in Frameworks/
set_target_properties(${PROJECT_NAME} PROPERTIES
    INSTALL_RPATH "@executable_path/../Frameworks"
    BUILD_WITH_INSTALL_RPATH TRUE
)

# ==========================================================================
# Code Signing
# ==========================================================================
# Set QCVIEW_CODESIGN_IDENTITY to your Developer ID for distribution builds:
#   cmake .. -DQCVIEW_CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)"
# Defaults to ad-hoc signing (-) for local development.
set(QCVIEW_CODESIGN_IDENTITY "-" CACHE STRING "Code signing identity (use '-' for ad-hoc)")
set(QCVIEW_ENTITLEMENTS "${CMAKE_SOURCE_DIR}/packaging/macos/entitlements.plist")

# Sign vendored dylibs individually, then sign the bundle
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/sign_app.sh
        "${QCVIEW_CODESIGN_IDENTITY}"
        "${QCVIEW_ENTITLEMENTS}"
        "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>"
    COMMENT "Signing app bundle (${QCVIEW_CODESIGN_IDENTITY})"
)
