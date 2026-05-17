# color subsystem — OCIOConfigManager + (later) the Pass 2 OCIO
# display pipeline.
#
# Phase 2.1: OCIOConfigManager only. No rendering integration yet.
# Phase 2.2+: OCIO → QShaderBaker → QRhiGraphicsPipeline (Guide 14 §II D7).

qt_add_library(qcv_color STATIC
    ocio_config_manager.cpp
    ocio_config_manager.h
    ocio_chain_builder.cpp
    ocio_chain_builder.h
    ocio_lut_baker.cpp
    ocio_lut_baker.h
    preset_manager.cpp
    preset_manager.h
)

target_include_directories(qcv_color
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(qcv_color
    PUBLIC
        Qt6::Core
        # Phase 7.5 A.2.7: ocio_chain_builder.h exposes
        # OCIO::ConstGpuShaderDescRcPtr in its return type. Anything
        # linking qcv_color needs OCIO headers reachable.
        PkgConfig::OPENCOLORIO
)
