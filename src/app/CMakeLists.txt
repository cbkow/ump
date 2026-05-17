# Application entry — qcview executable
#
# Phase 1.1 bootstrap: bare Qt Quick app that opens a window and
# exits cleanly. Verifies the toolchain end-to-end before subsystems
# get layered in.

qt_add_executable(qcview
    main.cpp
)

# Phase 2.4: full UI skeleton + slot-machine Color panel.
# Each component lives in its own QML file under src/ui/qml. They're
# all in the Qcv module via qt_add_qml_module so they cross-reference
# each other by short name (e.g., `LeftRail { }` from Main.qml).
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
)

foreach(qml_file IN LISTS QCVIEW_QML_FILES)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/src/ui/qml/${qml_file}
        PROPERTIES QT_RESOURCE_ALIAS ${qml_file}
    )
endforeach()

# Theme.qml is registered as a singleton — call sites use
# `import Qcv` then `Theme.accent` etc.
set_source_files_properties(${CMAKE_SOURCE_DIR}/src/ui/qml/Theme.qml
    PROPERTIES
        QT_QML_SINGLETON_TYPE TRUE
        QT_RESOURCE_ALIAS Theme.qml
)
# PhosphorIcons.js is bundled as a non-QML resource so Icon.qml's
# JS import can resolve it. Alias makes the absolute path land at
# the module's relative resource root.
set_source_files_properties(${CMAKE_SOURCE_DIR}/src/ui/qml/PhosphorIcons.js
    PROPERTIES
        QT_RESOURCE_ALIAS PhosphorIcons.js
)

qt_add_qml_module(qcview
    URI Qcv
    VERSION 1.0
    QML_FILES
        ${CMAKE_SOURCE_DIR}/src/ui/qml/Main.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/PlayerWindow.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/TransportBar.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/TimelinePanel.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/TimelineStatus.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/ViewportOverlay.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/ColorPanel.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/StatusStrip.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/LeftRail.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/RightRail.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/CollapsibleSection.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/ImageSequenceInspector.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/InspectorPanel.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/NotesPanel.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/ColorPickerInline.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/Theme.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/Icon.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/FlatButton.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/FlatComboBox.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/FlatSlider.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/FlatSpinBox.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/FlatSwitch.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/FlatTextField.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/FlatToolTip.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/ToolStrip.qml
        ${CMAKE_SOURCE_DIR}/src/ui/qml/ThemedMenu.qml
    RESOURCES
        ${CMAKE_SOURCE_DIR}/src/ui/qml/PhosphorIcons.js
)

target_link_libraries(qcview
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Network        # QLocalServer/QLocalSocket — single-instance handoff
        Qt6::Quick
        Qt6::QuickControls2
        qcv_window
        qcv_renderplugin   # static QML module init for Qcv.Render
)

target_compile_definitions(qcview
    PRIVATE
        QCV_VERSION_STR="${PROJECT_VERSION}"
)

# Bundle ID / metadata
#
# We use packaging/macos/Info.plist.in as the source of truth for
# CFBundleIdentifier, document types, URL scheme, and UTI exports —
# CMake's auto-generated Info.plist can't express those. Only the
# template-substituted vars (executable name, version) need to be
# set as target properties; CMake does the @VAR@ replacement on
# load.
set_target_properties(qcview PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST
        "${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in"
    MACOSX_BUNDLE_EXECUTABLE_NAME "qcview"
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    # Tell dyld to look for vendored dylibs in the bundle's
    # Frameworks/ dir. scripts/bundle_dylibs.sh copies the
    # Homebrew-linked deps there + rewrites their install names
    # to @rpath/<basename>. Without this rpath the bundled DMG
    # fails to launch on machines that don't have Homebrew.
    INSTALL_RPATH "@executable_path/../Frameworks"
    BUILD_RPATH   "@executable_path/../Frameworks"
    WIN32_EXECUTABLE TRUE
)

# Generate macOS .icns from the master PNG so the app gets a proper
# mortised icon in Finder / dock / cmd-tab. iconutil + sips are
# part of the macOS toolchain — no extra dependencies. Source is
# 512×512; we resize down for each iconset slot. (1024×1024 retina
# slot is skipped because the master is 512; bump the source to
# 1024 if Retina dock previews look soft.)
#
# Source artwork is qcviewMac.png — yellow Q with a transparent
# cleaver cutout, NO background. macOS Tahoe's "Dark" icon
# appearance auto-applies the system mortise (the standard dark
# squircle every dock icon sits in) to icons that ship as foreground-
# only artwork on transparent background. Baking our own squircle
# into the .icns prevents the system mortise from rendering correctly,
# so we feed qcviewMac.png straight into the iconset and let macOS
# composite the mortise behind it. Windows still uses qcview.png with
# the squircle baked in (no Windows equivalent of the system mortise).
if(APPLE)
    set(QCV_ICON_PNG     "${CMAKE_SOURCE_DIR}/assets/icons/qcviewMac.png")
    set(QCV_ICONSET_DIR  "${CMAKE_BINARY_DIR}/qcview.iconset")
    set(QCV_ICNS_FILE    "${CMAKE_BINARY_DIR}/qcview.icns")

    add_custom_command(
        OUTPUT  "${QCV_ICNS_FILE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${QCV_ICONSET_DIR}"
        COMMAND sips -z 16   16   "${QCV_ICON_PNG}" --out "${QCV_ICONSET_DIR}/icon_16x16.png"
        COMMAND sips -z 32   32   "${QCV_ICON_PNG}" --out "${QCV_ICONSET_DIR}/icon_16x16@2x.png"
        COMMAND sips -z 32   32   "${QCV_ICON_PNG}" --out "${QCV_ICONSET_DIR}/icon_32x32.png"
        COMMAND sips -z 64   64   "${QCV_ICON_PNG}" --out "${QCV_ICONSET_DIR}/icon_32x32@2x.png"
        COMMAND sips -z 128  128  "${QCV_ICON_PNG}" --out "${QCV_ICONSET_DIR}/icon_128x128.png"
        COMMAND sips -z 256  256  "${QCV_ICON_PNG}" --out "${QCV_ICONSET_DIR}/icon_128x128@2x.png"
        COMMAND sips -z 256  256  "${QCV_ICON_PNG}" --out "${QCV_ICONSET_DIR}/icon_256x256.png"
        COMMAND sips -z 512  512  "${QCV_ICON_PNG}" --out "${QCV_ICONSET_DIR}/icon_256x256@2x.png"
        COMMAND sips -z 512  512  "${QCV_ICON_PNG}" --out "${QCV_ICONSET_DIR}/icon_512x512.png"
        COMMAND iconutil -c icns "${QCV_ICONSET_DIR}" -o "${QCV_ICNS_FILE}"
        DEPENDS "${QCV_ICON_PNG}"
        COMMENT "Generating qcview.icns from ${QCV_ICON_PNG}"
        VERBATIM
    )

    set_source_files_properties("${QCV_ICNS_FILE}"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources"
    )
    target_sources(qcview PRIVATE "${QCV_ICNS_FILE}")
endif()

# Phase 2.5: bundle the shipped OCIO configs into the .app's Resources
# so they ship with the executable. AssetPaths::ocioRootDir() resolves
# them at runtime from QCoreApplication::applicationDirPath().
#
# On macOS:  qcview.app/Contents/Resources/assets/OCIO/Blender5.1/config.ocio
# Build dev: <build>/src/app/qcview.app/Contents/Resources/assets/OCIO/...
#            (same as bundled — CMake copies on build)
#
# Files are added as target sources with MACOSX_PACKAGE_LOCATION set;
# CMake copies them into the bundle layout each build.
file(GLOB_RECURSE QCV_OCIO_ASSETS
    LIST_DIRECTORIES FALSE
    "${CMAKE_SOURCE_DIR}/assets/OCIO/*"
)
# Drop .DS_Store / hidden Finder metadata from the glob — those would
# otherwise land in Contents/Resources/assets/OCIO/.DS_Store and
# block codesign with "resource fork, Finder information, or similar
# detritus not allowed".
list(FILTER QCV_OCIO_ASSETS EXCLUDE REGEX "/\\.DS_Store$")
list(FILTER QCV_OCIO_ASSETS EXCLUDE REGEX "/\\._")
foreach(asset_file IN LISTS QCV_OCIO_ASSETS)
    file(RELATIVE_PATH rel "${CMAKE_SOURCE_DIR}/assets" "${asset_file}")
    get_filename_component(rel_dir "${rel}" DIRECTORY)
    set_source_files_properties("${asset_file}"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/assets/${rel_dir}"
    )
endforeach()
target_sources(qcview PRIVATE ${QCV_OCIO_ASSETS})

# Polish — bundle Inter / JetBrainsMono / Phosphor fonts the same
# way as the OCIO + safety assets. main.cpp loads them via
# QFontDatabase::addApplicationFont at startup.
file(GLOB QCV_FONT_ASSETS LIST_DIRECTORIES FALSE
    "${CMAKE_SOURCE_DIR}/assets/fonts/*.ttf"
    "${CMAKE_SOURCE_DIR}/assets/fonts/*.txt"
)
foreach(asset_file IN LISTS QCV_FONT_ASSETS)
    set_source_files_properties("${asset_file}"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/assets/fonts"
    )
endforeach()
target_sources(qcview PRIVATE ${QCV_FONT_ASSETS})

# Phase 7.5 B.6.6: bundle the safety-overlay SVGs the same way.
file(GLOB QCV_SAFETY_ASSETS LIST_DIRECTORIES FALSE
    "${CMAKE_SOURCE_DIR}/assets/safety/*.svg"
)
foreach(asset_file IN LISTS QCV_SAFETY_ASSETS)
    set_source_files_properties("${asset_file}"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/assets/safety"
    )
endforeach()
target_sources(qcview PRIVATE ${QCV_SAFETY_ASSETS})

# Phase 3.H.6 Stage D — branding icon used at the top of every
# annotation-export document (Markdown / HTML / PDF). Same
# resource-dir pattern as the other asset folders.
file(GLOB QCV_ICON_ASSETS LIST_DIRECTORIES FALSE
    "${CMAKE_SOURCE_DIR}/assets/icons/*.png"
)
foreach(asset_file IN LISTS QCV_ICON_ASSETS)
    set_source_files_properties("${asset_file}"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/assets/icons"
    )
endforeach()
target_sources(qcview PRIVATE ${QCV_ICON_ASSETS})

# Phase 3.D — bundle the Perl exiftool distribution. Required by
# AdobeMetadataExtractor (project links + XMP / QT / MXF timecodes).
# The bundle is ~43 MB; ships only on macOS for now (Win/Linux
# packaging adds it via the same MACOSX_PACKAGE_LOCATION pattern
# replaced with the platform's resource dir at packaging time).
file(GLOB_RECURSE QCV_EXIFTOOL_ASSETS LIST_DIRECTORIES FALSE
    "${CMAKE_SOURCE_DIR}/assets/exiftool/*"
)
foreach(asset_file IN LISTS QCV_EXIFTOOL_ASSETS)
    file(RELATIVE_PATH rel "${CMAKE_SOURCE_DIR}/assets" "${asset_file}")
    get_filename_component(rel_dir "${rel}" DIRECTORY)
    set_source_files_properties("${asset_file}"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/assets/${rel_dir}"
    )
endforeach()
target_sources(qcview PRIVATE ${QCV_EXIFTOOL_ASSETS})

# Phase F.2.1 (Windows asset staging):
# On macOS the MACOSX_PACKAGE_LOCATION property routes each asset file
# into qcview.app/Contents/Resources/... at build time. Windows has no
# equivalent bundle mechanism — main.cpp's font / OCIO / safety / icon
# loaders all probe <exe-dir>/assets/<subdir>/, so we copy the source
# assets tree alongside qcview.exe via a POST_BUILD step. Reads fonts,
# OCIO configs, safety SVGs, icons, exiftool. Idempotent per-build:
# cmake -E copy_directory preserves timestamps for unchanged files.
if(WIN32)
    # Long-path-aware app manifest. Adding the manifest as a source
    # for an MSVC executable target makes CMake embed it into the EXE
    # at link time (no .rc wrapper needed). Lifts the 260-char Win32
    # MAX_PATH limit for our file APIs so deep EXR-sequence paths and
    # ${PROJECT_DIR}-relative .qcvproj reads don't truncate.
    target_sources(qcview PRIVATE
        "${CMAKE_SOURCE_DIR}/packaging/windows/qcview.manifest"
    )

    # Embed the multi-resolution app icon into qcview.exe. The .rc
    # references assets/icons/qcview.ico (generated from qcview.png
    # via ImageMagick — see comment in the .rc). Windows Explorer,
    # the taskbar, and the Alt+Tab switcher all read the first ICON
    # entry from the binary's resource section. The in-window title-
    # bar icon is set separately at runtime via setWindowIcon() in
    # main.cpp so the desktop / file-explorer icon and the in-app
    # title-bar icon can use different variants.
    target_sources(qcview PRIVATE
        "${CMAKE_SOURCE_DIR}/packaging/windows/qcview.rc"
    )

    add_custom_command(TARGET qcview POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:qcview>/assets"
        COMMENT "Staging assets/ alongside qcview.exe (Windows)"
        VERBATIM
    )

    # Phase F.2.4.X — drop BtbN's avcodec-XX.dll / avformat-XX.dll / ...
    # alongside qcview.exe. They ABI-match vcpkg's FFmpeg 8.1 majors but
    # ship libplacebo + libshaderc statically linked, enabling prores
    # Vulkan decode. Runs AFTER vcpkg's applocal.ps1 so we overwrite the
    # vcpkg-supplied DLLs (Windows binds to the file in the exe's
    # directory before walking PATH). Skipped when QCV_BTBN_FFMPEG_DLL_DIR
    # isn't set (vcpkg-fallback path in external/CMakeLists.txt).
    if(QCV_BTBN_FFMPEG_DLL_DIR)
        foreach(_dll IN ITEMS
            avcodec-62.dll avformat-62.dll avutil-60.dll
            avfilter-11.dll avdevice-62.dll
            swresample-6.dll swscale-9.dll)
            add_custom_command(TARGET qcview POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${QCV_BTBN_FFMPEG_DLL_DIR}/${_dll}"
                    "$<TARGET_FILE_DIR:qcview>/${_dll}"
                COMMENT "Staging BtbN ${_dll} (overrides vcpkg copy)"
                VERBATIM
            )
        endforeach()
    endif()
endif()
