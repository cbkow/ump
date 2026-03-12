# ==========================================================================
# QCView Windows Build Configuration
# OpenGL + D3D11VA rendering, WASAPI audio
# ==========================================================================

message(STATUS "Platform: Windows (OpenGL + D3D11)")

# ==========================================================================
# Compiler Settings
# ==========================================================================
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
# Find Packages
# ==========================================================================
find_package(OpenGL REQUIRED)

# FFmpeg paths (vendored in external/)
set(FFMPEG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg/include")
set(FFMPEG_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg/lib")
set(FFMPEG_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg/bin")

# ==========================================================================
# Source Filtering — exclude Linux/Vulkan-only files
# ==========================================================================
list(FILTER SOURCES EXCLUDE REGEX ".*vulkan_.*")
list(FILTER SOURCES EXCLUDE REGEX ".*pipewire_.*")
list(FILTER SOURCES EXCLUDE REGEX ".*linux_clipboard.*")
list(FILTER SOURCES EXCLUDE REGEX ".*\\.disabled$")
message(STATUS "Windows build: Excluded Vulkan/PipeWire/Linux-only sources")

# ==========================================================================
# Create Executable
# ==========================================================================
add_executable(${PROJECT_NAME} WIN32 ${SOURCES}
    "src/qcview.manifest"
    "resources/qcview.rc"
)

# ==========================================================================
# Include Directories
# ==========================================================================
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

# ==========================================================================
# Link Directories
# ==========================================================================
target_link_directories(${PROJECT_NAME} PRIVATE
    ${FFMPEG_LIB_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/external/tiff/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/external/png/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/external/jpeg/lib
)

# ==========================================================================
# Link Libraries
# ==========================================================================
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

# ==========================================================================
# SoundTouch — pitch-preserving time stretch
# ==========================================================================
set(SOUNDTOUCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/soundtouch")
if(EXISTS "${SOUNDTOUCH_DIR}/include/SoundTouch.h")
    target_include_directories(${PROJECT_NAME} PRIVATE ${SOUNDTOUCH_DIR}/include)
    target_link_libraries(${PROJECT_NAME} "${SOUNDTOUCH_DIR}/lib/SoundTouch.lib")
    target_compile_definitions(${PROJECT_NAME} PRIVATE QCVIEW_HAS_SOUNDTOUCH)
    message(STATUS "SoundTouch: Found in external/soundtouch")
else()
    message(STATUS "SoundTouch: NOT FOUND - tempo control will be disabled")
endif()

# ==========================================================================
# Runtime DLL Copying
# ==========================================================================

# FFmpeg executable (optional — only needed for transcode features)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg/bin/ffmpeg.exe")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg/bin/ffmpeg.exe"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/"
        COMMENT "Copying FFmpeg executable"
    )
endif()

# FFmpeg DLLs
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

# OCIO + OpenEXR + image format DLLs (all from external/ocio/bin)
set(REQUIRED_DLLS
    "Iex-3_4.dll"
    "IlmThread-3_4.dll"
    "Imath-3_2.dll"
    "OpenColorIO_2_5.dll"
    "OpenEXR-3_4.dll"
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

# openjph DLL (JPEG 2000 HTJ2K support — vendored in external/openexr/bin)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/bin/openjph.0.26.dll")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/bin/openjph.0.26.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/"
        COMMENT "Copying openjph DLL"
    )
endif()
