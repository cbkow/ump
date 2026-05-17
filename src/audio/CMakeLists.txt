# Audio subsystem — Phase 5.0.a substrate + F.2.10 Windows.
#
# AudioRingBuffer (header-only) + AudioDecoder (ffmpeg + SwrContext +
# decode thread) + per-platform audio device + AudioPlayer wrapper.
# F.2.10 ships the Windows WASAPI device; Linux PipeWire backend is
# still TODO.

set(QCV_AUDIO_SOURCES
    audio_ring_buffer.h
    i_audio_source.h
    audio_decoder.h
    audio_decoder.cpp
    multi_stream_audio_decoder.h
    multi_stream_audio_decoder.cpp
    audio_player.h
    audio_player.cpp
)

if(APPLE)
    list(APPEND QCV_AUDIO_SOURCES
        coreaudio_device.h
        coreaudio_device.mm
    )
elseif(WIN32)
    list(APPEND QCV_AUDIO_SOURCES
        wasapi_audio_device.h
        wasapi_audio_device.cpp
    )
endif()

qt_add_library(qcv_audio STATIC ${QCV_AUDIO_SOURCES})

target_include_directories(qcv_audio
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(qcv_audio
    PUBLIC
        Qt6::Core
    PRIVATE
        PkgConfig::FFMPEG
)

if(APPLE)
    target_link_libraries(qcv_audio
        PRIVATE
            "-framework AudioToolbox"
            "-framework CoreAudio"
            "-framework AudioUnit"
    )
elseif(WIN32)
    target_link_libraries(qcv_audio
        PRIVATE
            # WASAPI / MMDevice / COM
            ole32
            # MMCSS (AvSetMmThreadCharacteristics)
            avrt
    )
endif()
