# probe-ocio — Phase 2.0 OpenColorIO smoke test.
#
# Loads a config from disk and prints its name, roles, colorspaces,
# looks, displays, and views. Proves the OCIO link is working before
# the renderer-side integration lands in Phase 2.2+.

add_executable(probe-ocio
    main.cpp
)

target_link_libraries(probe-ocio
    PRIVATE
        PkgConfig::OPENCOLORIO
)

target_compile_features(probe-ocio PRIVATE cxx_std_20)
