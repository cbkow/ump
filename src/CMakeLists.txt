# Source tree
#
# Subsystems get added as they're implemented. Each subdir is its
# own CMake target (typically a static library) that the app links
# against. Phase 1.1 only has the app entry.

# Phase 1.3 — bootstrap + window topology + render pipeline skeleton
# Phase 1.8.1 — decode subsystem (VideoDecoder skeleton)
# Phase 2.1 — color subsystem (OCIOConfigManager)
# Phase 5.0.a — audio subsystem (AudioDecoder + CoreAudio device)
# Phase 7.2.a — timeline subsystem (data structs only)
add_subdirectory(decode)
add_subdirectory(color)
add_subdirectory(audio)
add_subdirectory(project)
add_subdirectory(timeline)
add_subdirectory(annotations)
# Phase 7.7 — dual playback island. Isolated from single flow.
# Must be added BEFORE render (render depends on dual for the
# DualCompositor delegate in MetalPlayerRenderer's dual branch).
add_subdirectory(dual)
add_subdirectory(window)
add_subdirectory(render)
add_subdirectory(app)

# Future subsystems (uncomment as implemented):
# add_subdirectory(hdr)
# add_subdirectory(bins)
# add_subdirectory(playlist)
# add_subdirectory(transcode)
# add_subdirectory(settings)
# add_subdirectory(ui)
