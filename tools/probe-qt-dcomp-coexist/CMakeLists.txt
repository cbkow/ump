# probe-qt-dcomp-coexist — Phase F.1.b spike.
#
# Verifies that a custom DirectComposition visual can coexist with
# Qt RHI's UI swapchain on the same HWND.
#
# What we want to find out:
#   1. Does Qt's QML rendering stay visible after we attach a DComp
#      target to the same HWND?
#   2. Does our DComp visual show up over Qt's QML?
#   3. Does our visual resize cleanly when the window resizes?
#   4. Do native popups (file dialogs, MenuBars) render correctly?
#
# Outcome determines the present-path architecture for Phase F.2+
# (the D3D11 renderer rewrite).
#
# Standalone — not part of the main qcview build. Configure with:
#   cd tools/probe-qt-dcomp-coexist
#   cmake -B build -G Ninja \
#       -DCMAKE_PREFIX_PATH=C:/Qt/6.11.0/msvc2022_64 \
#       -DCMAKE_TOOLCHAIN_FILE=C:/z_TempDev/vcpkg/scripts/buildsystems/vcpkg.cmake \
#       -DVCPKG_TARGET_TRIPLET=x64-windows
#   cmake --build build
#   ./build/probe-qt-dcomp-coexist.exe

cmake_minimum_required(VERSION 3.20)
project(probe-qt-dcomp-coexist LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT WIN32)
    message(FATAL_ERROR "probe-qt-dcomp-coexist is Windows-only.")
endif()

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Qml Quick)

qt_standard_project_setup()

qt_add_executable(probe-qt-dcomp-coexist
    main.cpp
)

qt_add_qml_module(probe-qt-dcomp-coexist
    URI ProbeDComp
    VERSION 1.0
    QML_FILES
        Main.qml
)

target_link_libraries(probe-qt-dcomp-coexist PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    d3d11.lib
    dxgi.lib
    dcomp.lib
    d3dcompiler.lib
)

target_compile_definitions(probe-qt-dcomp-coexist PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

# Console subsystem so we see stdout/stderr while debugging
set_target_properties(probe-qt-dcomp-coexist PROPERTIES
    WIN32_EXECUTABLE FALSE
)
