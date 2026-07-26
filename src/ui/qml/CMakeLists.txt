# QML module setup for the qcview target.
#
# Why this lives here instead of src/app/CMakeLists.txt:
# Qt6QmlMacros.cmake computes the qmlcachegen output directory as
#     ${CMAKE_CURRENT_BINARY_DIR}/.rcc/qmlcache/${target}_${file_relative}
# where file_relative is the path of each QML source relative to
# CMAKE_CURRENT_SOURCE_DIR. When qt_add_qml_module was called from
# src/app/, file_relative was "../ui/qml/Foo.qml" — which baked a
# literal ".." into the output dir name (qcview_../ui/qml/). Windows
# strips trailing dots from directory names, so Ninja's mkdir on
# "qcview_.." failed with ENOENT. Hosting the call here makes the
# relative path a bare filename and the output dir becomes
# qcview_Foo.qml (no `..` segment).

set(QCVIEW_QML_FILES
    Main.qml
    PlayerWindow.qml
    TransportBar.qml
    TimelinePanel.qml
    TimelineStatus.qml
    ViewportOverlay.qml
    ColorPanel.qml
    StatusStrip.qml
    LeftRail.qml
    RightRail.qml
    CollapsibleSection.qml
    ImageSequenceInspector.qml
    InspectorPanel.qml
    InspectorCard.qml
    KvRow.qml
    KvChipRow.qml
    FlatChip.qml
    MiniSpinner.qml
    NotesPanel.qml
    ColorPickerInline.qml
    Theme.qml
    Icon.qml
    FlatButton.qml
    FlatComboBox.qml
    FlatSlider.qml
    FlatSpinBox.qml
    FlatSwitch.qml
    FlatTextField.qml
    FlatToolTip.qml
    ToolStrip.qml
    ThemedMenu.qml
    ThemedMenuSeparator.qml
    ModalHost.qml
    Toast.qml
    LiveStrip.qml
)

foreach(qml_file IN LISTS QCVIEW_QML_FILES)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/${qml_file}
        PROPERTIES QT_RESOURCE_ALIAS ${qml_file}
    )
endforeach()

# Theme.qml is registered as a singleton — call sites use
# `import Qcv` then `Theme.accent` etc.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/Theme.qml
    PROPERTIES
        QT_QML_SINGLETON_TYPE TRUE
        QT_RESOURCE_ALIAS Theme.qml
)
# PhosphorIcons.js is bundled as a non-QML resource so Icon.qml's
# JS import can resolve it. Alias makes the absolute path land at
# the module's relative resource root.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/PhosphorIcons.js
    PROPERTIES
        QT_RESOURCE_ALIAS PhosphorIcons.js
)

qt_add_qml_module(qcview
    URI Qcv
    VERSION 1.0
    # main.cpp has no Q_OBJECT / QML_ELEMENT classes, so qmltyperegistrar
    # would error on the 0-byte qt6qcview_metatypes.json placeholder.
    # Skip qmltypes generation — the QML files themselves are still
    # bundled via the generated .qrc, qmldir, and qmlcache loader.
    NO_GENERATE_QMLTYPES
    QML_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/Main.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/PlayerWindow.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/TransportBar.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/TimelinePanel.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/TimelineStatus.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/ViewportOverlay.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/ColorPanel.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/StatusStrip.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/LeftRail.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/RightRail.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/CollapsibleSection.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/ImageSequenceInspector.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/InspectorPanel.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/InspectorCard.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/KvRow.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/KvChipRow.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/FlatChip.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/MiniSpinner.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/NotesPanel.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/ColorPickerInline.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/Theme.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/Icon.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/FlatButton.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/FlatComboBox.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/FlatSlider.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/FlatSpinBox.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/FlatSwitch.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/FlatTextField.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/FlatToolTip.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/ToolStrip.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/ThemedMenu.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/ThemedMenuSeparator.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/ModalHost.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/Toast.qml
        ${CMAKE_CURRENT_SOURCE_DIR}/LiveStrip.qml
    RESOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/PhosphorIcons.js
)
