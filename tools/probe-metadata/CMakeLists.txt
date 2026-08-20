# probe-metadata — CLI dump of FFmpegMetadataExtractor output.
#
# Runs the same header-only extraction the Inspector relies on
# (VideoMetadata) against one or more files and prints the fields —
# container family / MXF operational pattern / encoder tool /
# bitrates / codec profile (incl. the DNxHD/DNxHR CID mapping) plus
# the stream basics. Use it to verify the Inspector's metadata against
# a sample set without launching the app:
#
#   build/tools/probe-metadata/probe-metadata a.mxf b.mov …
#   build/tools/probe-metadata/probe-metadata --probe a.mxf   # probeContainer() path

add_executable(probe-metadata
    main.cpp
)

target_link_libraries(probe-metadata
    PRIVATE
        qcv_project
        Qt6::Core
)

target_compile_features(probe-metadata PRIVATE cxx_std_20)
