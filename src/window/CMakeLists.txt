# window subsystem — UI window + Player child window topology.
# Phase 1.2: shell. Future phases layer HDR + RhiItem on top.

add_library(qcv_window STATIC
    window_manager.cpp
    window_manager.h
    native_fullscreen_macos.h
    native_fullscreen_win.h
    sparkle_updater_macos.h
    # Phase F.2.11.b — adapter wiring qcv_render's pure-virtual
    # IDualFrameSource to qcv_dual's DualPlaybackController. Lives
    # here because qcv_window already links both libraries; qcv_render
    # cannot link qcv_dual on non-Apple (qmltypes cycle).
    window_manager_dual_source_adapter.h
    window_manager_dual_source_adapter.cpp
)

# macOS borderless-fullscreen helper — manipulates NSWindow level /
# styleMask / presentationOptions to bypass the OS's animated
# setVisibility(FullScreen). Compiled only on Apple platforms; the
# header guards everything behind Q_OS_MACOS so other platforms see
# an empty namespace.
if(APPLE)
    target_sources(qcv_window PRIVATE native_fullscreen_macos.mm)
    target_link_libraries(qcv_window PUBLIC "-framework AppKit")

    # Sparkle auto-updater shim (sparkle_updater_macos.mm wraps
    # SPUStandardUpdaterController). Sparkle.framework is vendored under
    # external/Sparkle by scripts/fetch_sparkle.sh — fail early with a
    # clear message if it's missing rather than emit a cryptic link error.
    # The app target embeds the framework into Contents/Frameworks and the
    # @executable_path/../Frameworks rpath (set there) resolves it.
    set(QCV_SPARKLE_DIR "${CMAKE_SOURCE_DIR}/external/Sparkle")
    if(NOT EXISTS "${QCV_SPARKLE_DIR}/Sparkle.framework")
        message(FATAL_ERROR
            "Sparkle.framework not found at ${QCV_SPARKLE_DIR}/Sparkle.framework.\n"
            "Run scripts/fetch_sparkle.sh once to vendor it before building.")
    endif()
    target_sources(qcv_window PRIVATE sparkle_updater_macos.mm)
    # -F at compile time so `#import <Sparkle/Sparkle.h>` resolves, and at
    # link time so `-framework Sparkle` finds it.
    target_compile_options(qcv_window PRIVATE "-F${QCV_SPARKLE_DIR}")
    target_link_libraries(qcv_window PUBLIC
        "-F${QCV_SPARKLE_DIR}" "-framework Sparkle")
endif()

# Windows borderless-fullscreen helper — sidesteps Qt's
# ApplicationWindow.FullScreen visibility transition (which disrupts
# the parent HWND our D3D11 child HWND tracks). Sets WS_EX_TOPMOST +
# resizes to screen.geometry() instead.
if(WIN32)
    target_sources(qcv_window PRIVATE native_fullscreen_win.cpp)
endif()

target_include_directories(qcv_window
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(qcv_window
    PUBLIC
        Qt6::Core
        Qt6::Concurrent
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        qcv_decode      # exposes VideoDecoder via WindowManager
        qcv_color       # exposes OCIOConfigManager via WindowManager
        qcv_audio       # exposes AudioPlayer via WindowManager
        qcv_project     # exposes ProjectManager via WindowManager
        qcv_timeline    # exposes TimelineController via WindowManager
        qcv_annotations # ViewportAnnotator owned + exposed by WindowManager (B.6.5)
        qcv_dual        # Phase 7.7 — DualPlaybackController owned by WindowManager
)

# Phase 7.5: when QCV_NATIVE_PLAYER is on, WindowManager instantiates
# qcv::PlayerWindow from qcv_render and bypasses PlayerWindow.qml.
# Compile-define propagates so window_manager.cpp's #ifdef block
# fires.
if(QCV_NATIVE_PLAYER)
    target_compile_definitions(qcv_window PRIVATE QCV_NATIVE_PLAYER)
    target_link_libraries(qcv_window PRIVATE qcv_render)
endif()

set_target_properties(qcv_window PROPERTIES
    AUTOMOC ON
)
