cmake_minimum_required(VERSION 3.24)   # GLOBAL keyword on find_package (Phase F)

# ---------------------------------------------------------------------
# macOS deployment target — must be set BEFORE project() so the
# compiler picks it up via -mmacosx-version-min. LSMinimumSystemVersion
# in packaging/macos/Info.plist.in is hardcoded to the same 13.0; if
# you bump one, bump the other.
# ---------------------------------------------------------------------
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0"
        CACHE STRING "Minimum macOS version" FORCE)
endif()

# ---------------------------------------------------------------------
# Project
# ---------------------------------------------------------------------
project(qcview
    VERSION 2.0.6
    LANGUAGES CXX
    DESCRIPTION "QCView Player — HDR-capable review tool (Qt Quick + QRhi rebuild)"
)

# Apple platforms also need Objective-C++ for native interop (CAMetalLayer etc.)
if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

# ---------------------------------------------------------------------
# Standards & global compile options
# ---------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Lock against pre-Qt-6.11 deprecations (per dependencies.md §1)
add_compile_definitions(QT_NO_DEPRECATED_BEFORE=0x060B00)

# ---------------------------------------------------------------------
# Platform detection
# ---------------------------------------------------------------------
if(APPLE)
    set(QCVIEW_PLATFORM "macos")
elseif(WIN32)
    set(QCVIEW_PLATFORM "windows")
elseif(UNIX)
    set(QCVIEW_PLATFORM "linux")
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

message(STATUS "QCView platform: ${QCVIEW_PLATFORM}")

# ---------------------------------------------------------------------
# Vendored deps prefix — when scripts/build_macos_deps.sh has been
# run, external/install/ contains FFmpeg / OCIO / OpenEXR / etc. all
# built for our deployment target. Prepend to CMAKE_PREFIX_PATH and
# PKG_CONFIG_PATH so find_package + pkg_check_modules pick them up
# BEFORE Homebrew. Falls through to Homebrew when the prefix doesn't
# exist (dev iteration without rebuilding deps).
# ---------------------------------------------------------------------
set(QCV_VENDOR_PREFIX "${CMAKE_SOURCE_DIR}/external/install")
if(APPLE AND EXISTS "${QCV_VENDOR_PREFIX}/lib")
    list(PREPEND CMAKE_PREFIX_PATH "${QCV_VENDOR_PREFIX}")
    if(DEFINED ENV{PKG_CONFIG_PATH})
        set(ENV{PKG_CONFIG_PATH}
            "${QCV_VENDOR_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    else()
        set(ENV{PKG_CONFIG_PATH} "${QCV_VENDOR_PREFIX}/lib/pkgconfig")
    endif()
    message(STATUS "Using vendored deps from ${QCV_VENDOR_PREFIX}")
endif()

# ---------------------------------------------------------------------
# Build options
# ---------------------------------------------------------------------
option(QCVIEW_USE_SYSTEM_FFMPEG  "Use system FFmpeg instead of FetchContent" ON)
option(QCVIEW_USE_SYSTEM_OCIO    "Use system OCIO instead of FetchContent"   ON)
option(QCVIEW_USE_SYSTEM_OPENEXR "Use system OpenEXR instead of FetchContent" ON)
option(QCVIEW_BUILD_TESTS        "Build test harnesses" OFF)

# Phase 7.5 — native QWindow + Metal/Vulkan player surface, instead
# of the QQuickRhiItem-backed QML PlayerWindow.
#
# Phase B.8 (macOS cutover, 2026-04-27): default ON for macOS.
# 4K EXR freeze fix landed at B.6.2; video / OCIO / annotations /
# safety guides / drag-drop all wired through Phase B.6.x and B.7.
#
# Phase F.2.0 (2026-05-11): default ON for Windows too — the D3D11
# native renderer lands in F.2.1 alongside this flip. macOS keeps
# Metal; Windows uses D3D11 + DComp child-HWND (NOT Vulkan render).
if(APPLE OR WIN32)
    option(QCV_NATIVE_PLAYER "Use native QWindow + platform renderer (Metal/D3D11)" ON)
else()
    option(QCV_NATIVE_PLAYER "Use native QWindow + platform renderer (Metal/D3D11)" OFF)
endif()

# ---------------------------------------------------------------------
# Qt 6.11 (per dependencies.md §1)
# ---------------------------------------------------------------------
find_package(Qt6 6.11 REQUIRED COMPONENTS
    Core
    Gui
    GuiPrivate              # rhi/qrhi.h, rhi/qshader.h — renderer + Phase 2 shader baking
    Quick
    QuickControls2
    ShaderTools
    ShaderToolsPrivate      # rhi/qshaderbaker.h — Phase 2 OCIO → QShader pipeline
    Network
    Concurrent
    Multimedia
)

qt_standard_project_setup(REQUIRES 6.11)

# ---------------------------------------------------------------------
# CMake helper modules
# ---------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# ---------------------------------------------------------------------
# External dependencies (FetchContent + system finds)
# ---------------------------------------------------------------------
add_subdirectory(external)

# ---------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------
add_subdirectory(src)

# ---------------------------------------------------------------------
# Tools — small CLI utilities used during development (probe-video,
# eventually transcode CLI, etc.). Always built; cheap.
# ---------------------------------------------------------------------
add_subdirectory(tools)

# ---------------------------------------------------------------------
# Packaging (Windows MSIX)
#
# Adds a qcview_msix target that packages the staged build-release/ into
# installer/msix/dist/QCView-${PROJECT_VERSION}-win64.msix. Runs only on
# Windows; the subdir's CMakeLists early-returns elsewhere. Both Store
# submission (unsigned MSIX) and signed-sideload flows are driven by the
# same target — see installer/README.md for the env-var-based signing
# switch.
# ---------------------------------------------------------------------
add_subdirectory(installer)
