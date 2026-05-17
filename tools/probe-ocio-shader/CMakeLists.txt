# probe-ocio-shader — Phase 2.2 OCIO + QShaderBaker smoke test.
#
# Loads an OCIO config, builds a Display/View processor, asks OCIO
# for GLSL, and pipes it through Qt's QShaderBaker to verify the
# pipeline is viable at runtime before we touch the renderer.

add_executable(probe-ocio-shader
    main.cpp
)

target_link_libraries(probe-ocio-shader
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::GuiPrivate           # rhi/qshader.h
        Qt6::ShaderTools
        Qt6::ShaderToolsPrivate   # rhi/qshaderbaker.h
        PkgConfig::OPENCOLORIO
)

target_compile_features(probe-ocio-shader PRIVATE cxx_std_20)
