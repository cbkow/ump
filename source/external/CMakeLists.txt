# GLFW - Build from source
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(glfw)

# GLAD
add_library(glad glad/src/gl.c)
target_include_directories(glad PUBLIC glad/include)

# ImGui with docking
set(IMGUI_SOURCES
    imgui/imgui.cpp
    imgui/imgui_demo.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_glfw.cpp
    imgui/backends/imgui_impl_opengl3.cpp
)
add_library(imgui ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC 
    imgui
    imgui/backends
    glfw/include
)
target_link_libraries(imgui glfw)

# Enable docking features
target_compile_definitions(imgui PUBLIC IMGUI_ENABLE_DOCKING)

# ImPlot - Plotting library for ImGui
set(IMPLOT_SOURCES
    implot/implot.cpp
    implot/implot_items.cpp
)
add_library(implot ${IMPLOT_SOURCES})
target_include_directories(implot PUBLIC implot)
target_link_libraries(implot imgui)

# Native file browsers:
include(FetchContent)

# Add nativefiledialog-extended
FetchContent_Declare(
    nfd
    GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
    GIT_TAG v1.1.1
)
FetchContent_MakeAvailable(nfd)

# Add libharu (PDF generation library)
FetchContent_Declare(
    libharu
    GIT_REPOSITORY https://github.com/libharu/libharu.git
    GIT_TAG v2.4.5
)
# Configure libharu options before making it available
set(LIBHPDF_STATIC ON CACHE BOOL "Build static library" FORCE)
set(LIBHPDF_SHARED OFF CACHE BOOL "Don't build shared library" FORCE)
set(DEVPAK OFF CACHE BOOL "Don't create DevPackage" FORCE)
# Point libharu to existing PNG and ZLIB
set(PNG_PNG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/png/include" CACHE PATH "PNG include dir" FORCE)
set(PNG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/png/include" CACHE PATH "PNG include dir" FORCE)
set(PNG_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/png/lib/libpng16.lib" CACHE FILEPATH "PNG library" FORCE)
set(ZLIB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ocio/include" CACHE PATH "ZLIB include dir" FORCE)
set(ZLIB_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/ocio/lib/zlib.lib" CACHE FILEPATH "ZLIB library" FORCE)
FetchContent_MakeAvailable(libharu)

# ========================================
# OPENCOLORIO - Manual Setup  
# ========================================
set(OCIO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ocio")

# Verify files exist
if(NOT EXISTS "${OCIO_ROOT}/lib/OpenColorIO.lib")
    message(WARNING "OpenColorIO.lib not found at ${OCIO_ROOT}/lib/. Make sure you copied the vcpkg files.")
    message(STATUS "Expected path: ${OCIO_ROOT}/lib/OpenColorIO.lib")
    # Don't fail the build, just warn
else()
    message(STATUS "Found OpenColorIO.lib")
endif()

if(NOT EXISTS "${OCIO_ROOT}/include/OpenColorIO/OpenColorIO.h")
    message(WARNING "OpenColorIO headers not found at ${OCIO_ROOT}/include/.")
    message(STATUS "Expected path: ${OCIO_ROOT}/include/OpenColorIO/OpenColorIO.h")
else()
    message(STATUS "Found OpenColorIO headers")
endif()

# Only create the target if files exist
if(EXISTS "${OCIO_ROOT}/lib/OpenColorIO.lib")
    # Create imported target
    add_library(OpenColorIO SHARED IMPORTED)
    set_target_properties(OpenColorIO PROPERTIES
        IMPORTED_LOCATION "${OCIO_ROOT}/bin/OpenColorIO.dll"
        IMPORTED_IMPLIB "${OCIO_ROOT}/lib/OpenColorIO.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${OCIO_ROOT}/include"
    )
    message(STATUS "OpenColorIO target configured")
else()
    # Create dummy target so build doesn't fail
    add_library(OpenColorIO INTERFACE)
    message(WARNING "OpenColorIO not found - created dummy target")
endif()