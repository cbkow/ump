# dual subsystem — Phase 7.7 dual playback island.
#
# Isolated subsystem for dual-source (A/B comparison) playback. Lives
# parallel to the single flow; never touched in single mode. Code
# duplication vs qcv_decode / qcv_render is acceptable per Phase 7.7
# design — isolation is the goal.
#
# Stage 1: IDualSource + DualVideoDecoder with ring buffer + NeedsMoreFrames.
# Stage 2: DualImageSeqSource — cache thread + I/O worker pool.
# Stage 3: DualPlaybackTimer + DualPlaybackController + DualCompositor.

qt_add_library(qcv_dual STATIC
    i_dual_source.h
    i_dual_pixbuf_converter.h
    dual_video_decoder.cpp
    dual_video_decoder.h
    dual_image_seq_source.cpp
    dual_image_seq_source.h
    dual_playback_timer.cpp
    dual_playback_timer.h
    dual_playback_controller.cpp
    dual_playback_controller.h
    dual_audio_mixer.cpp
    dual_audio_mixer.h
    i_dual_scrub_decoder.h
    dual_scrub_cache.h
)

if(APPLE)
    target_sources(qcv_dual PRIVATE
        metal/dual_compositor.h
        metal/dual_compositor.mm
        dual_cv_helpers.mm
        # macOS scrub decoder — zero-copy VideoToolbox publish. Plain C++:
        # the CVPixelBuffer is handled via DualVideoDecoder::makeMetalScrubFrame
        # + dual_cv_helpers, so it needs no -fobjc-arc.
        dual_scrub_decoder_macos.cpp
    )
    set_source_files_properties(
        metal/dual_compositor.mm
        dual_cv_helpers.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc"
    )
    target_link_libraries(qcv_dual PRIVATE
        "-framework Metal"
        "-framework QuartzCore"
        "-framework CoreVideo"
    )
else()
    # Windows/Linux scrub decoder — CPU readback (sws_scale). Gated out of
    # the macOS build entirely (it's a QObject; keeping it out of the macOS
    # source list stops AUTOMOC emitting a meta-object for a class that is
    # never compiled there).
    target_sources(qcv_dual PRIVATE
        dual_scrub_decoder.cpp
        dual_scrub_decoder.h
    )
endif()

target_include_directories(qcv_dual
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(qcv_dual
    PUBLIC
        Qt6::Core
        Qt6::Gui
        PkgConfig::FFMPEG
        # Phase 7.8 — DualPlaybackController consults TimelineFlattener
        # to translate master playhead → per-side source frame using
        # the dual-mode timeline's clips on tracks A / B.
        qcv_timeline
        # Task #110 — DualImageSeqSource needs qcv_decode's
        # EXRImageLoader so EXR sequences load on either dual side.
        # Consume-only; single-flow EXR pipeline is unaffected.
        qcv_decode
        # Phase 7.7 Stage 6 — DualAudioMixer reuses qcv_audio's
        # AudioDecoder + CoreAudioDevice. Two decoders feeding one
        # device, mixed in the render callback.
        qcv_audio
)

# Stage 1 verification harness — DualVideoDecoder ring-buffer mechanics.
# Build:  cmake --build build --target test_dual_decoder
# Run:    build/src/dual/test_dual_decoder /path/to/video.mov
add_executable(test_dual_decoder EXCLUDE_FROM_ALL
    test_dual_decoder.cpp
)
target_link_libraries(test_dual_decoder PRIVATE qcv_dual)

# Stage 2 verification harness — image-sequence cache + worker pool.
# Build:  cmake --build build --target test_dual_image_seq
# Run:    build/src/dual/test_dual_image_seq /path/to/sequence/dir
#   Or:   build/src/dual/test_dual_image_seq /path/to/frame.0001.png
#   Optional: --threads N to verify adaptive thread count knob.
add_executable(test_dual_image_seq EXCLUDE_FROM_ALL
    test_dual_image_seq.cpp
)
target_link_libraries(test_dual_image_seq PRIVATE qcv_dual)
