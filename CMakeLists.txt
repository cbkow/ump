cmake_minimum_required(VERSION 3.20)
if(APPLE)
    project(qcview VERSION 1.1.6 LANGUAGES CXX C OBJC OBJCXX)
else()
    project(qcview VERSION 1.1.6 LANGUAGES CXX C)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set default build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ==========================================================================
# Platform detection
# ==========================================================================
if(UNIX AND NOT APPLE)
    set(QCVIEW_LINUX TRUE)
else()
    set(QCVIEW_LINUX FALSE)
endif()

find_package(Threads REQUIRED)

# ==========================================================================
# External dependencies (GLFW, ImGui, ImPlot, NanoVG, OTIO, etc.)
# ==========================================================================
add_subdirectory(external)

get_property(all_targets DIRECTORY external PROPERTY BUILDSYSTEM_TARGETS)
message(STATUS "Available targets from external: ${all_targets}")

# ==========================================================================
# Collect all source files, then let platform cmake filter them
# ==========================================================================
file(GLOB_RECURSE SOURCES
    "src/*.cpp"
    "src/*.h"
)

# ==========================================================================
# Platform-specific configuration
# Each platform cmake file handles: compiler flags, packages, source
# filtering, executable creation, includes, link libraries, and packaging.
# ==========================================================================
if(QCVIEW_LINUX)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/linux.cmake)
elseif(WIN32)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows.cmake)
elseif(APPLE)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos.cmake)
endif()

# ==========================================================================
# Common include directories (cross-platform)
# ==========================================================================
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/imnodes
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
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

# ==========================================================================
# Common link libraries (cross-platform)
# ==========================================================================
if(NOT APPLE)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/external/glfw/include
    )
endif()

target_link_libraries(${PROJECT_NAME}
    glad
    imgui
    implot
    nanovg                                                              # NanoVG vector graphics
    nfd
    opentimelineio                                                      # OpenTimelineIO
    InkStrokeModeler::stroke_modeler                                    # Google's stroke smoothing library
    hpdf                                                                # libharu for PDF export
    Threads::Threads
)
if(NOT APPLE)
    target_link_libraries(${PROJECT_NAME} glfw)
endif()

# Enable OpenTimelineIO support
target_compile_definitions(${PROJECT_NAME} PRIVATE USE_OPENTIMELINEIO)

# ==========================================================================
# Set target properties
# ==========================================================================
set_property(TARGET ${PROJECT_NAME} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# ==========================================================================
# Copy assets (cross-platform)
# ==========================================================================
# On macOS, assets go to Contents/Resources/ (handled in cmake/macos.cmake).
# The cross-platform copy to the executable dir is only needed on Windows/Linux.
if(NOT APPLE)
    message(STATUS "Setting up asset copying for all build configurations")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
        COMMENT "Copying assets to output directory"
    )
endif()

# Copy shaders (only if directory has files)
if(SHADER_FILES)
    message(STATUS "Will copy shaders: ${SHADER_FILES}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders
    )
endif()
