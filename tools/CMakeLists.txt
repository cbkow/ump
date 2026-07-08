# Development tools — small CLI utilities.
#
# Each tool gets its own subdirectory + executable target. Keep them
# self-contained so they can be invoked from the build dir without
# loading the rest of the app.

add_subdirectory(probe-video)
add_subdirectory(probe-ocio)
add_subdirectory(probe-ocio-shader)
add_subdirectory(probe-exr-decode)

# probe-ocio-d3d11 (Phase F.1.a spike) builds standalone — see its own
# CMakeLists.txt for the configure command. Not part of the main build.
