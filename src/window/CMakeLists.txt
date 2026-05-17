# window subsystem — UI window + Player child window topology.
# Phase 1.2: shell. Future phases layer HDR + RhiItem on top.

add_library(qcv_window STATIC
    window_manager.cpp
    window_manager.h
    native_fullscreen_macos.h
    native_fullscreen_win.h
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
