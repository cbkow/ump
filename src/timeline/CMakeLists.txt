# Timeline subsystem — Phase 7.2.
#
# OpenTimelineIO library is intentionally NOT linked. See the
# project_otio_library_dropped memory note + Guide 02's Phase 7.2
# banner for rationale. Local C++ structs only.

qt_add_library(qcv_timeline STATIC
    timeline_types.h
    playback_timer.h
    playback_timer.cpp
    timeline_flattener.h
    timeline_flattener.cpp
    timeline_controller.h
    timeline_controller.cpp
    # Phase 7.8 Stage C — QUndoCommand subclasses for dual edits.
    timeline_edits.h
    timeline_edits.cpp
    # Phase 3.H.5 — hover-thumbnail cache (port of old QCView's
    # TimelineThumbnailCache). Pulls in IImageLoader + per-format
    # loaders from qcv_decode and the Metal thumbnail texture pool
    # from qcv_render via QCV_NATIVE_PLAYER.
    timeline_thumbnail_cache.h
    timeline_thumbnail_cache.cpp
)

target_include_directories(qcv_timeline
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(qcv_timeline
    PUBLIC
        Qt6::Core
        Qt6::Gui       # Phase 7.8 — QUndoStack / QUndoCommand live in QtGui (Qt6)
        qcv_project    # MediaItem header included by timeline_controller
        qcv_decode     # IImageLoader + per-format loaders (thumbnail cache)
)

# Phase 3.H.5 — hover-thumbnail cache reaches into the renderer's
# Metal texture pool when the native player is on. Define + link
# match the qcv_window pattern.
if(QCV_NATIVE_PLAYER)
    target_compile_definitions(qcv_timeline PRIVATE QCV_NATIVE_PLAYER)
    target_link_libraries(qcv_timeline PRIVATE qcv_render)
endif()
