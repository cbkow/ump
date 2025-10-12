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

    # Disable specific warnings if needed
    add_compile_options(/wd4996)  # Disable deprecated warnings
endif()

# Find packages
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# Set paths for dependencies on Windows
if(WIN32)
    # Adjust these paths based on where you install the libraries
    set(MPV_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/mpv/include")
    set(MPV_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/mpv/lib")
    set(MPV_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/mpv/bin")

    # FFmpeg paths
    set(FFMPEG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg/include")
    set(FFMPEG_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg/lib")
    set(FFMPEG_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ffmpeg/bin")

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
    "src/utils/gpu_scheduler.h"
    "src/utils/gpu_scheduler.cpp"
    "src/utils/system_pressure_monitor.h"
    "src/utils/system_pressure_monitor.cpp"
    "src/metadata/video_metadata.cpp"
    "src/player/media_background_extractor.h"
    "src/player/media_background_extractor.cpp"
    "src/player/conversion_strategy.cpp"
    "src/player/image_sequence_config.h"
    "src/player/image_sequence_config.cpp"
    "src/player/direct_exr_cache.h"
    "src/player/direct_exr_cache.cpp"
    "src/player/exr_transcoder.h"
    "src/player/exr_transcoder.cpp"
    "src/gpu/texture_pool.h"
    "src/gpu/texture_pool.cpp"
    "src/player/image_loaders.h"
    "src/player/image_loaders.cpp"
    "src/player/thumbnail_cache.h"
    "src/player/thumbnail_cache.cpp"
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
    ${MPV_INCLUDE_DIR}
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
)

# Link directories for Windows
if(WIN32)
    target_link_directories(${PROJECT_NAME} PRIVATE
        ${MPV_LIB_DIR}
        ${FFMPEG_LIB_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/openexr/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/external/tiff/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/external/png/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/external/jpeg/lib
    )
endif()

# Link libraries
target_link_libraries(${PROJECT_NAME}
    OpenGL::GL
    glfw
    glad
    imgui
    implot
    nfd
    OpenColorIO    # Add OpenColorIO
    # FFmpeg libraries
    avcodec
    avformat
    avutil
    avfilter
    swscale
    # OpenEXR C++ API for direct EXR loading (100% OIIO-free)
    Imath-3_2          # Must be first for half-float support
    Iex-3_3
    IlmThread-3_3
    OpenEXRCore-3_3
    OpenEXRUtil-3_3
    OpenEXR-3_3        # C++ API for DWAB compression
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
        ${MPV_LIB_DIR}/libmpv.dll.a  # Use the .dll.a import library
        opengl32
        winmm
        dwmapi
        setupapi
        version
        winhttp  # For Frame.io API client
        dxgi     # For GPU/VRAM monitoring
    )
elseif(APPLE)
    find_library(COCOA_LIBRARY Cocoa)
    find_library(IOKIT_LIBRARY IOKit)
    find_library(COREVIDEO_LIBRARY CoreVideo)
    target_link_libraries(${PROJECT_NAME} 
        mpv
        ${COCOA_LIBRARY} 
        ${IOKIT_LIBRARY} 
        ${COREVIDEO_LIBRARY}
    )
else()
    target_link_libraries(${PROJECT_NAME} mpv)
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

# MPV DLL copy (this should work fine)
if(WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MPV_BIN_DIR}/libmpv-2.dll"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
    )
    
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
        "Iex-3_3.dll"
        "IlmThread-3_3.dll"
        "Imath-3_2.dll"
        "OpenColorIO_2_4.dll"
        "OpenEXR-3_3.dll"        # C++ API for DWAB compression (direct, no OIIO)
        "OpenEXRCore-3_3.dll"
        "OpenEXRUtil-3_3.dll"
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

    # OIIO removed - using direct OpenEXR API only

endif()
