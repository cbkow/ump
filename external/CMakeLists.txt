# External dependencies — pinned per dependencies.md
#
# IMPORTANT: every FetchContent_Declare uses a fixed GIT_TAG. Never
# `master` / `main` / `HEAD`. See dependencies.md §0 for pin policy.
#
# Heavy deps (FFmpeg, OCIO, OpenEXR) prefer system packages by default
# via QCVIEW_USE_SYSTEM_* options to keep dev builds fast. CI and
# packaging builds toggle to FetchContent for reproducibility.

include(FetchContent)

# Avoid noisy install steps from third-party deps
set(FETCHCONTENT_QUIET FALSE)
set(FETCHCONTENT_UPDATES_DISCONNECTED TRUE)

# ---------------------------------------------------------------------
# Phase 1.1 — bootstrap: Qt only.
#
# All deps below are commented out and added incrementally as their
# subsystems are implemented. Order roughly matches Guide 12 §6
# implementation phases.
# ---------------------------------------------------------------------

# ---- nlohmann/json (Phase 1, used by settings + project files) ----
# FetchContent_Declare(
#     nlohmann_json
#     GIT_REPOSITORY https://github.com/nlohmann/json.git
#     GIT_TAG v3.11.3
#     GIT_SHALLOW TRUE
# )
# set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
# set(JSON_Install    OFF CACHE BOOL "" FORCE)
# FetchContent_MakeAvailable(nlohmann_json)

# ---- FFmpeg (Phase 1.8, VideoDecoder + tools/probe-video) ----
# Dev: Homebrew (`brew install ffmpeg`) — fast iteration.
# CI/packaging: switch QCVIEW_USE_SYSTEM_FFMPEG=OFF and bring in via
# ExternalProject_Add (FFmpeg uses ./configure, not CMake).
#
# Phase F.2.4 Windows override: vcpkg's FFmpeg 8.1 is missing
# libplacebo + libshaderc, so the prores Vulkan compute decoder isn't
# available. BtbN's `FFmpeg.GPL.Shared.8.1` winget package ships
# FFmpeg 8.1 (same major API: libavcodec 62 / libavutil 60 /
# libavformat 62) with libplacebo + libshaderc baked into the DLLs.
# Prepending its pkgconfig dir to PKG_CONFIG_PATH makes pkg-config
# prefer it over vcpkg's. Configure with -DQCV_BTBN_FFMPEG_DIR=<path>
# to override the default winget location.
if(QCVIEW_USE_SYSTEM_FFMPEG)
    if(WIN32)
        if(NOT QCV_BTBN_FFMPEG_DIR)
            set(QCV_BTBN_FFMPEG_DIR
                "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Packages/BtbN.FFmpeg.GPL.Shared.8.1_Microsoft.Winget.Source_8wekyb3d8bbwe/ffmpeg-n8.1-11-g75d37c499d-win64-gpl-shared-8.1"
                CACHE PATH "BtbN FFmpeg GPL Shared install root (must contain bin/ lib/ include/ lib/pkgconfig/)")
        endif()
        if(EXISTS "${QCV_BTBN_FFMPEG_DIR}/lib/pkgconfig/libavcodec.pc")
            set(ENV{PKG_CONFIG_PATH}
                "${QCV_BTBN_FFMPEG_DIR}/lib/pkgconfig;$ENV{PKG_CONFIG_PATH}")
            message(STATUS "FFmpeg: prefer BtbN GPL Shared at ${QCV_BTBN_FFMPEG_DIR}")
            # POST_BUILD copy of BtbN DLLs alongside qcview.exe lives in
            # src/app/CMakeLists.txt — gate on QCV_BTBN_FFMPEG_DLL_DIR.
            set(QCV_BTBN_FFMPEG_DLL_DIR "${QCV_BTBN_FFMPEG_DIR}/bin"
                CACHE PATH "Source dir for FFmpeg DLLs to copy at build time" FORCE)
        else()
            message(WARNING
                "QCV_BTBN_FFMPEG_DIR='${QCV_BTBN_FFMPEG_DIR}' is missing "
                "lib/pkgconfig/libavcodec.pc — falling back to vcpkg FFmpeg "
                "(prores Vulkan decode will NOT be available). Install via: "
                "winget install BtbN.FFmpeg.GPL.Shared.8.1")
        endif()
    endif()
    find_package(PkgConfig REQUIRED)
    # GLOBAL promotes PkgConfig::FFMPEG out of this subdir's scope so
    # tools/ + src/ can both link against it.
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
        libavcodec libavformat libavutil libswscale libswresample libavfilter
    )
    message(STATUS "FFmpeg via pkg-config: ${FFMPEG_VERSION}")
else()
    message(FATAL_ERROR "FetchContent path for FFmpeg not yet implemented; set QCVIEW_USE_SYSTEM_FFMPEG=ON")
endif()

# ---- OpenColorIO (Phase 2.0+, color management) ----
# 2.5.x is the target (per dependencies.md). Homebrew currently
# ships 2.5.1; verified end-to-end via tools/probe-ocio.
if(QCVIEW_USE_SYSTEM_OCIO)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OPENCOLORIO REQUIRED IMPORTED_TARGET GLOBAL OpenColorIO)
    message(STATUS "OpenColorIO via pkg-config: ${OPENCOLORIO_VERSION}")
else()
    message(FATAL_ERROR "FetchContent path for OpenColorIO not yet implemented; set QCVIEW_USE_SYSTEM_OCIO=ON")
endif()

# ---- OpenEXR + Imath (Phase 7.4.b, EXR image sequences) ----
# Homebrew on macOS ships OpenEXR 3.4 + Imath 3.x as separate
# packages. find_package(OpenEXR) implicitly pulls in Imath, but
# we also find_package(Imath) explicitly so the half-float typedef
# (Imath::half) is reachable from the loader.
if(QCVIEW_USE_SYSTEM_OPENEXR)
    find_package(OpenEXR 3.3 REQUIRED)
    find_package(Imath REQUIRED)
    if(NOT TARGET OpenEXR::OpenEXR)
        message(FATAL_ERROR "OpenEXR::OpenEXR target missing")
    endif()
    if(NOT TARGET Imath::Imath)
        message(FATAL_ERROR "Imath::Imath target missing")
    endif()
    set_target_properties(OpenEXR::OpenEXR PROPERTIES IMPORTED_GLOBAL TRUE)
    set_target_properties(Imath::Imath     PROPERTIES IMPORTED_GLOBAL TRUE)
    message(STATUS "OpenEXR: ${OpenEXR_VERSION}, Imath: ${Imath_VERSION}")
else()
    message(FATAL_ERROR "FetchContent path for OpenEXR not yet implemented; set QCVIEW_USE_SYSTEM_OPENEXR=ON")
endif()

# ---- libpng (Phase 7.4.a, PNG image sequences) ----
# Homebrew on macOS, libpng-dev on Debian/Ubuntu, vcpkg/PNG on Windows.
# Pulls zlib in transitively. Used by qcv_decode's image-sequence
# loader. Promotes PNG::PNG out of this subdir's scope.
find_package(PNG REQUIRED)
if(NOT TARGET PNG::PNG)
    message(FATAL_ERROR "PNG::PNG target not provided by find_package(PNG)")
endif()
set_target_properties(PNG::PNG PROPERTIES IMPORTED_GLOBAL TRUE)
message(STATUS "libpng: ${PNG_LIBRARY} (${PNG_VERSION_STRING})")

# ---- libtiff (Phase 7.4.c, TIFF image sequences) ----
find_package(TIFF REQUIRED)
if(NOT TARGET TIFF::TIFF)
    message(FATAL_ERROR "TIFF::TIFF target not provided by find_package(TIFF)")
endif()
set_target_properties(TIFF::TIFF PROPERTIES IMPORTED_GLOBAL TRUE)
message(STATUS "libtiff: ${TIFF_LIBRARIES} (${TIFF_VERSION_STRING})")

# ---- libjpeg-turbo (Phase 7.4.c, JPEG image sequences) ----
# Homebrew ships libjpeg-turbo as the `jpeg` package; CMake's
# FindJPEG resolves it. JPEG::JPEG is the imported target.
find_package(JPEG REQUIRED)
if(NOT TARGET JPEG::JPEG)
    message(FATAL_ERROR "JPEG::JPEG target not provided by find_package(JPEG)")
endif()
set_target_properties(JPEG::JPEG PROPERTIES IMPORTED_GLOBAL TRUE)
message(STATUS "libjpeg-turbo: ${JPEG_LIBRARIES}")

# ---- ink-stroke-modeler + Abseil (Phase 4, annotations) ----
# Pin commit hash on first integration — Google project doesn't tag
# regularly. Update dependencies.md §2 when committed.
# FetchContent_Declare(
#     ink_stroke_modeler
#     GIT_REPOSITORY https://github.com/google/ink-stroke-modeler.git
#     GIT_TAG <commit-hash>
#     GIT_SHALLOW FALSE
# )

# ---- QtKeychain (Phase 4, Frame.io tokens) ----
# FetchContent_Declare(
#     qtkeychain
#     GIT_REPOSITORY https://github.com/frankosterfeld/qtkeychain.git
#     GIT_TAG v0.14.3
#     GIT_SHALLOW TRUE
# )

# ---- OpenTimelineIO (Phase 6, playlist + timeline) ----
# FetchContent_Declare(
#     opentimelineio
#     GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/OpenTimelineIO.git
#     GIT_TAG v0.18.0
#     GIT_SHALLOW TRUE
# )

# ---- Vulkan (Phase F decode-only on Windows; future Linux) ----
# macOS uses Metal directly. On Windows the renderer is D3D11
# (Phase F); Vulkan stays in the codebase only as the FFmpeg
# hwaccel decode path. Linux Vulkan stays gated behind
# QCV_BUILD_LINUX_VULKAN (default OFF, reserved).
#
# `GLOBAL` keyword on find_package (CMake 3.24+) makes the target
# visible to src/render at top scope without the (invalid) manual
# `set_target_properties(... IMPORTED_GLOBAL TRUE)` promotion that
# was here pre-Phase-F.
if(NOT APPLE)
    find_package(Vulkan REQUIRED GLOBAL)
    if(NOT TARGET Vulkan::Vulkan)
        message(FATAL_ERROR "Vulkan::Vulkan target not provided by find_package(Vulkan)")
    endif()
    message(STATUS "Vulkan: ${Vulkan_LIBRARY} (${Vulkan_VERSION})")
endif()

# ---- shaderc / glslang (Phase C.4, runtime GLSL → SPIR-V for OCIO shaders) ----
# Deferred — activated when VulkanShaderCompiler is lifted in Phase C.4.
# Old app uses shaderc; new port can either match or use glslang directly.
# if(NOT APPLE)
#     FetchContent_Declare(
#         shaderc
#         GIT_REPOSITORY https://github.com/google/shaderc.git
#         GIT_TAG v2024.4
#         GIT_SHALLOW TRUE
#     )
# endif()

# ---- ink-stroke-modeler (Phase 7.5 A.2.3, annotation drawing smoothing) ----
# Google's real-time stroke smoothing library. Pin to a specific commit
# (the project doesn't tag releases regularly; old app uses GIT_TAG=main
# which is supply-chain-risky — we pin per the dependencies.md §0 policy).
# Resolved commit: tip of main on 2026-03-10 (matches old app's then-active
# build). Update commit pin + corresponding date here when refreshing.
#
# Pulls Abseil transitively (the wrapper's include path needs both).
FetchContent_Declare(
    ink_stroke_modeler
    GIT_REPOSITORY https://github.com/google/ink-stroke-modeler.git
    GIT_TAG da42d439389c90ec7574f0381ec53e7f5be0c2eb
    GIT_SHALLOW FALSE
)
set(INK_STROKE_MODELER_BUILD_TESTING   OFF CACHE BOOL "" FORCE)
set(INK_STROKE_MODELER_FIND_DEPENDENCIES OFF CACHE BOOL "" FORCE)
set(INK_STROKE_MODELER_ENABLE_INSTALL  OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL                OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST                      OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ink_stroke_modeler)
message(STATUS "ink-stroke-modeler: pinned commit da42d439")

# Phase 2.0+: Qt + FFmpeg + OCIO (system pkg-config) + Vulkan (non-Apple).
message(STATUS "External deps configured")
