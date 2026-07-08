# probe-exr-decode — EXR decode performance audit CLI.
#
# 2026-07-08 — measures the real cost of the app's EXR decode path
# (EXRImageLoader over MemoryMappedIStream) on actual sequences:
# header anatomy (parts/channels/compression/data-window), per-frame
# loadFrame latency, worker-parallel throughput (the ImageSequenceCache
# shape), OpenEXR-internal-thread sensitivity, and the thumbnail paths.
# Run it whenever "EXR feels slower" — it answers whether the file
# shape changed or the code did.

add_executable(probe-exr-decode
    main.cpp
)

target_link_libraries(probe-exr-decode
    PRIVATE
        qcv_decode
        OpenEXR::OpenEXR
        Imath::Imath
)

target_compile_features(probe-exr-decode PRIVATE cxx_std_20)
