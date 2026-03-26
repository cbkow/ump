# GLFW - Build from source (not needed on macOS — native NSWindow/MTKView used instead)
if(NOT APPLE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(glfw)
endif()

# GLAD (OpenGL loader - still built on Vulkan path for header compatibility)
add_library(glad glad/src/gl.c)
target_include_directories(glad PUBLIC glad/include)

# ImGui with docking
set(IMGUI_SOURCES
    imgui/imgui.cpp
    imgui/imgui_demo.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
)

# Platform backend: native macOS (imgui_impl_osx) or GLFW (Linux/Windows)
if(APPLE)
    list(APPEND IMGUI_SOURCES imgui/backends/imgui_impl_osx.mm)
else()
    list(APPEND IMGUI_SOURCES imgui/backends/imgui_impl_glfw.cpp)
endif()

# OpenGL3 backend — Windows always, Linux when not using Vulkan
if(WIN32 OR (QCVIEW_LINUX AND NOT QCVIEW_USE_VULKAN))
    list(APPEND IMGUI_SOURCES imgui/backends/imgui_impl_opengl3.cpp)
endif()

# D3D11 backend on Windows
if(WIN32)
    list(APPEND IMGUI_SOURCES imgui/backends/imgui_impl_dx11.cpp)
endif()

# Vulkan backend on Linux
if(QCVIEW_LINUX AND QCVIEW_USE_VULKAN)
    list(APPEND IMGUI_SOURCES imgui/backends/imgui_impl_vulkan.cpp)
endif()

# Metal backend on macOS
if(APPLE)
    list(APPEND IMGUI_SOURCES imgui/backends/imgui_impl_metal.mm)
endif()

add_library(imgui ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC
    imgui
    imgui/backends
)

# GLFW: link on non-Apple, headers-only on Apple (source files still #include it)
if(NOT APPLE)
    target_include_directories(imgui PUBLIC glfw/include)
    target_link_libraries(imgui glfw)
else()
    target_include_directories(imgui PUBLIC glfw/include)
endif()

# Link D3D11 libraries for ImGui on Windows
if(WIN32)
    target_link_libraries(imgui d3d11 d3dcompiler)
endif()

# Link Vulkan for ImGui on Linux
if(QCVIEW_LINUX AND QCVIEW_USE_VULKAN)
    find_package(Vulkan REQUIRED)
    target_link_libraries(imgui Vulkan::Vulkan)
    target_include_directories(imgui PUBLIC ${Vulkan_INCLUDE_DIRS})
endif()

# Link Metal for ImGui on macOS
if(APPLE)
    find_library(METAL_LIB Metal)
    find_library(QUARTZCORE_LIB QuartzCore)
    find_library(GAMECONTROLLER_LIB GameController)
    target_link_libraries(imgui ${METAL_LIB} ${QUARTZCORE_LIB} ${GAMECONTROLLER_LIB})
    # Enable ObjC ARC for .mm files in ImGui
    set_source_files_properties(imgui/backends/imgui_impl_metal.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
    set_source_files_properties(imgui/backends/imgui_impl_osx.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
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
    EXCLUDE_FROM_ALL
)
# Configure libharu options before making it available
set(LIBHPDF_STATIC ON CACHE BOOL "Build static library" FORCE)
set(LIBHPDF_SHARED OFF CACHE BOOL "Don't build shared library" FORCE)
set(LIBHPDF_INSTALL OFF CACHE BOOL "Don't install libharu" FORCE)
set(DEVPAK OFF CACHE BOOL "Don't create DevPackage" FORCE)
# Point libharu to existing PNG and ZLIB (Windows vendored paths)
if(WIN32)
    set(PNG_PNG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/png/include" CACHE PATH "PNG include dir" FORCE)
    set(PNG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/png/include" CACHE PATH "PNG include dir" FORCE)
    set(PNG_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/png/lib/libpng16.lib" CACHE FILEPATH "PNG library" FORCE)
    set(ZLIB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ocio/include" CACHE PATH "ZLIB include dir" FORCE)
    set(ZLIB_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/ocio/lib/zlib.lib" CACHE FILEPATH "ZLIB library" FORCE)
endif()
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
set(IMATH_INSTALL OFF CACHE BOOL "Don't install Imath" FORCE)
FetchContent_MakeAvailable(opentimelineio)
message(STATUS "OpenTimelineIO configured")

# ========================================
# INK-STROKE-MODELER - Real-time stroke smoothing
# (Also fetches Abseil automatically as a dependency)
# ========================================
FetchContent_Declare(
    ink_stroke_modeler
    GIT_REPOSITORY https://github.com/google/ink-stroke-modeler.git
    GIT_TAG main
    GIT_SHALLOW TRUE
)
# Let ink-stroke-modeler fetch its own dependencies (Abseil)
set(INK_STROKE_MODELER_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(INK_STROKE_MODELER_FIND_DEPENDENCIES OFF CACHE BOOL "" FORCE)
set(INK_STROKE_MODELER_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ink_stroke_modeler)
message(STATUS "ink-stroke-modeler configured (with Abseil dependency)")

# ========================================
# NANOVG - Vector graphics rendering
# ========================================
FetchContent_Declare(
    nanovg
    GIT_REPOSITORY https://github.com/memononen/nanovg.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nanovg)

# Create NanoVG library target
add_library(nanovg ${nanovg_SOURCE_DIR}/src/nanovg.c)
target_include_directories(nanovg PUBLIC ${nanovg_SOURCE_DIR}/src)
# NanoVG GL3 implementation header is included inline, define the macro
# STB_IMAGE_STATIC makes stb_image symbols static to avoid conflicts with main.cpp's stb_image
target_compile_definitions(nanovg
    PUBLIC NANOVG_GL3_IMPLEMENTATION
    PRIVATE STB_IMAGE_STATIC
)
target_link_libraries(nanovg glad)
message(STATUS "NanoVG configured")

# ========================================
# OPENCOLORIO - Manual Setup (Windows only, Linux uses pkg-config)
# ========================================
if(WIN32)
    set(OCIO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ocio")

    # Verify files exist
    if(NOT EXISTS "${OCIO_ROOT}/lib/OpenColorIO.lib")
        message(WARNING "OpenColorIO.lib not found at ${OCIO_ROOT}/lib/. Make sure you copied the vcpkg files.")
        message(STATUS "Expected path: ${OCIO_ROOT}/lib/OpenColorIO.lib")
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
endif()
