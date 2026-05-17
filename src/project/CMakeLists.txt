# project subsystem — Phase 7.1 bins data model.
#
# Owns MediaItem + ProjectBin + ProjectManager. UI lives in QML
# (LeftRail's Bins section) and reads via Q_PROPERTY exposed
# through WindowManager. No persistence yet; .qcvproj save/load
# lands in 7.6.

qt_add_library(qcv_project STATIC
    media_item.h
    project_manager.h
    project_manager.cpp
    # Phase 7.6 — `.qcvproj` v2 save/load.
    project_io.h
    project_io.cpp
    # Phase 3.B — async metadata extraction.
    metadata_service.h
    metadata_service.cpp
    ffmpeg_metadata_extractor.h
    ffmpeg_metadata_extractor.cpp
    # Phase 3.D — Adobe project links + extended timecodes via the
    # bundled exiftool. Spawned via QProcess.
    adobe_metadata_extractor.h
    adobe_metadata_extractor.cpp
)

target_include_directories(qcv_project
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(qcv_project
    PUBLIC
        Qt6::Core
    PRIVATE
        # Phase 3.B — FFmpegMetadataExtractor links libavformat /
        # libavcodec / libavutil. Promoted via the existing
        # PkgConfig::FFMPEG IMPORTED target.
        PkgConfig::FFMPEG
)
