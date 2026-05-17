# probe-video — CLI smoke test for the FFmpeg integration.
#
# Phase 1.8.0 — opens a video file, prints stream/codec metadata,
# and decodes the first frame. Proves that FFmpeg is linked and
# working before the full VideoDecoder lands.

add_executable(probe-video
    main.cpp
)

target_link_libraries(probe-video
    PRIVATE
        PkgConfig::FFMPEG
)

target_compile_features(probe-video PRIVATE cxx_std_20)
