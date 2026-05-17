// PlayerWindowFactory — Phase 7.5 A.3 / Guide 20.
//
// Picks the right IPlayerRenderer for the current platform:
//   - macOS:        MetalPlayerRenderer (src/render/metal/...)
//   - Win / Linux:  VulkanPlayerRenderer (src/render/vulkan/...)
//
// Phase A.3 ships stub renderers; Phase B / C replace them with the
// real lifts.

#pragma once

#include <QSurface>

#include <memory>

namespace qcv {

class IPlayerRenderer;

// Construct the platform-appropriate IPlayerRenderer. Returns the
// stub in Phase A; the real one in Phase B/C.
std::unique_ptr<IPlayerRenderer> createPlayerRenderer();

// QSurface::SurfaceType for the player window. macOS = MetalSurface,
// non-Apple = VulkanSurface. Used by PlayerWindow's ctor before the
// renderer's surface is created.
QSurface::SurfaceType playerWindowSurfaceType();

} // namespace qcv
