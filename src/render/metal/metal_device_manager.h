// MetalDeviceManager — Phase 7.5 B.1 / Guide 19 §2.1.
//
// Direct lift from old QCView's src/gpu/metal_device_manager.{h,mm}
// (~164 LoC). Centralized id<MTLDevice> + id<MTLCommandQueue> +
// CAMetalLayer for the native player surface. Singleton — one Metal
// device per process is the right shape (the OS exposes one default
// device; multiple windows would share it).
//
// Adaptation notes vs old app:
//   - Namespace qcview → qcv
//   - Initialize(void* mtkview) → initialize(QWindow*)
//     The old app handed in an MTKView that already had a device +
//     CAMetalLayer; we attach a fresh CAMetalLayer to the QWindow's
//     backing NSView (winId() returns NSView* on macOS when
//     surfaceType == MetalSurface).
//   - PIMPL via std::unique_ptr<Impl>: matches the existing
//     ZeroCopyBridgeMetal pattern — ARC-managed ObjC types live in
//     the .mm-private struct.
//   - Public API still returns void* for the device/queue/layer so
//     this header can be #included from plain .cpp without dragging
//     in <Metal/Metal.h>. The .mm-side typed accessors live alongside
//     the Impl struct.
//   - Debug::Log → qInfo / qWarning

// macOS-only file. The Apple-isolated CMake block (if(APPLE) ... endif()
// in src/render/CMakeLists.txt) is the single platform gate; we don't
// double-guard with #ifdef Q_OS_MACOS in the header — same pattern as
// zero_copy_bridge_metal.h.

#pragma once

#include <cstddef>
#include <memory>
#include <mutex>

class QWindow;

namespace qcv {

class MetalDeviceManager {
public:
    static MetalDeviceManager &instance();

    MetalDeviceManager(const MetalDeviceManager &)            = delete;
    MetalDeviceManager &operator=(const MetalDeviceManager &) = delete;

    // Attaches a CAMetalLayer to `window`'s backing NSView, picks the
    // system default MTLDevice, and creates a command queue. `window`
    // must have surfaceType() == QSurface::MetalSurface and must have
    // been created (winId() non-zero). Returns false on any failure.
    bool initialize(QWindow *window);
    void shutdown();
    bool isInitialized() const { return m_initialized; }

    // Raw ObjC types are returned as void* so this header is usable
    // from plain .cpp. Cast on the .mm side:
    //   id<MTLDevice>       d = (__bridge id<MTLDevice>)mgr.device();
    //   id<MTLCommandQueue> q = (__bridge id<MTLCommandQueue>)mgr.commandQueue();
    //   CAMetalLayer       *l = (__bridge CAMetalLayer *)mgr.metalLayer();
    void *device()       const;
    void *commandQueue() const;
    void *metalLayer()   const;

    // Allocate a one-shot command buffer. Caller takes ownership;
    // release with CFRelease (or let ARC handle via __bridge_transfer
    // if the caller is in .mm).
    void *createCommandBuffer();

    // Submit an empty buffer and wait for all prior GPU work. Use
    // before CPU readback of GPU-written textures.
    void waitForGpu();

    // Diagnostics.
    const char *deviceName() const;        // UTF-8, owned by the device; do not free
    std::size_t recommendedMaxWorkingSetBytes() const;

private:
    MetalDeviceManager();
    ~MetalDeviceManager();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool                  m_initialized = false;
    mutable std::mutex    m_mutex;
};

} // namespace qcv
