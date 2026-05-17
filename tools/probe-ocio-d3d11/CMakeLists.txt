# probe-ocio-d3d11 — Phase F.1.a spike (standalone build).
#
# Self-contained — doesn't link into the main qcview build. Configure
# from this directory:
#
#   cd tools/probe-ocio-d3d11
#   cmake -B build -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=C:/z_TempDev/vcpkg/scripts/buildsystems/vcpkg.cmake \
#       -DVCPKG_TARGET_TRIPLET=x64-windows
#   cmake --build build
#   ./build/probe-ocio-d3d11.exe ../../assets/OCIO/Blender5.1/config.ocio

cmake_minimum_required(VERSION 3.20)
project(probe-ocio-d3d11 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "probe-ocio-d3d11 is Windows-only (uses D3D11).")
endif()

find_package(OpenColorIO CONFIG REQUIRED)

add_executable(probe-ocio-d3d11 main.cpp)

target_link_libraries(probe-ocio-d3d11 PRIVATE
    OpenColorIO::OpenColorIO
    d3d11.lib
    dxgi.lib
    d3dcompiler.lib
)

target_compile_definitions(probe-ocio-d3d11 PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
