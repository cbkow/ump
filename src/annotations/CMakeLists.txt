# annotations subsystem — Phase 7.5 / Guide 04 / Guide 19 §2.3.
#
# Per-phase rollout:
#   A.2.2 (this commit): active_stroke + stroke_tessellator
#   A.2.3: ink_stroke_modeler_wrapper
#   A.2.4: serializer + note + io + manager
#   A.2.5: viewport_annotator (decoupled from ImGui)
#
# Pure CPU; consumed by the player surface's stroke render pass
# (Phase B.6.5 Metal / Phase C Vulkan).

qt_add_library(qcv_annotations STATIC
    active_stroke.h
    stroke_tessellator.h
    stroke_tessellator.cpp
    ink_stroke_modeler_wrapper.h
    ink_stroke_modeler_wrapper.cpp
    annotation_note.h
    annotation_note.cpp
    annotation_serializer.h
    annotation_serializer.cpp
    annotation_io.h
    annotation_io.cpp
    annotation_manager.h
    annotation_manager.cpp
    annotation_exporter.h
    annotation_exporter.cpp
    annotation_thumbnail.h
    annotation_thumbnail.cpp
    viewport_annotator.h
    viewport_annotator.cpp
)

target_include_directories(qcv_annotations
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE
        # ink-stroke-modeler doesn't propagate its include dir as a
        # usage requirement — match the old app's manual add (see
        # /Users/chris/Documents/GitHub/QCView-Player/CMakeLists.txt:74).
        ${CMAKE_BINARY_DIR}/_deps/ink_stroke_modeler-src
)

target_link_libraries(qcv_annotations
    PUBLIC
        Qt6::Core
        Qt6::Gui
    PRIVATE
        # Phase 3.H.6 Stage D — DOCX export uses QZipWriter from
        # QtCore's private header set. Marked private but stable
        # since Qt 5; surface area we touch (addFile, close) has
        # not changed.
        Qt6::CorePrivate
        # Google's ink-stroke-modeler — pulled via FetchContent in
        # external/CMakeLists.txt. PRIVATE: callers don't need to
        # know about ink::stroke_model types.
        InkStrokeModeler::stroke_modeler
)
