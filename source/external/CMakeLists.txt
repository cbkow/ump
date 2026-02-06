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

# Add D3D11 backend on Windows for full D3D11 rendering mode
if(WIN32)
    list(APPEND IMGUI_SOURCES imgui/backends/imgui_impl_dx11.cpp)
endif()

add_library(imgui ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC
    imgui
    imgui/backends
    glfw/include
)
target_link_libraries(imgui glfw)

# Link D3D11 libraries for ImGui on Windows
if(WIN32)
    target_link_libraries(imgui d3d11 d3dcompiler)
endif()

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
# OPENTIMELINEIO - Timeline interchange format
# ========================================
FetchContent_Declare(
    opentimelineio
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/OpenTimelineIO.git
    GIT_TAG v0.17.0
    GIT_SHALLOW TRUE
)
# Configure OTIO options before making it available
set(OTIO_CXX_INSTALL OFF CACHE BOOL "Don't install OTIO" FORCE)
set(OTIO_PYTHON_INSTALL OFF CACHE BOOL "Don't build Python bindings" FORCE)
set(OTIO_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)
set(OTIO_FIND_IMATH OFF CACHE BOOL "Don't require Imath" FORCE)
set(OTIO_DEPENDENCIES_INSTALL OFF CACHE BOOL "Don't install dependencies" FORCE)
FetchContent_MakeAvailable(opentimelineio)
message(STATUS "OpenTimelineIO configured")

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
        IMPORTED_LOCATION_RELEASE "${OCIO_ROOT}/bin/OpenColorIO_2_5.dll"
        IMPORTED_IMPLIB_RELEASE "${OCIO_ROOT}/lib/OpenColorIO.lib"
        IMPORTED_LOCATION_DEBUG "${OCIO_ROOT}/bin/OpenColorIO_2_5.dll"
        IMPORTED_IMPLIB_DEBUG "${OCIO_ROOT}/lib/OpenColorIO.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${OCIO_ROOT}/include"
        MAP_IMPORTED_CONFIG_DEBUG Release
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
        MAP_IMPORTED_CONFIG_MINSIZEREL Release
    )
    message(STATUS "OpenColorIO target configured")
else()
    # Create dummy target so build doesn't fail
    add_library(OpenColorIO INTERFACE)
    message(WARNING "OpenColorIO not found - created dummy target")
endif()