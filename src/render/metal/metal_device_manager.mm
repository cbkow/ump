// MetalDeviceManager — see header for adaptation notes.

#include "metal_device_manager.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <QWindow>
#include <QtLogging>

namespace qcv {

struct MetalDeviceManager::Impl {
    id<MTLDevice>       device       = nil;
    id<MTLCommandQueue> commandQueue = nil;
    CAMetalLayer       *layer        = nil;
};

MetalDeviceManager &MetalDeviceManager::instance()
{
    static MetalDeviceManager s_instance;
    return s_instance;
}

MetalDeviceManager::MetalDeviceManager()
    : m_impl(std::make_unique<Impl>())
{
}

MetalDeviceManager::~MetalDeviceManager()
{
    shutdown();
}

bool MetalDeviceManager::initialize(QWindow *window)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_initialized) {
        return true;  // idempotent
    }

    if (!window) {
        qWarning("MetalDeviceManager: null window");
        return false;
    }

    // QWindow::winId() returns NSView* on macOS once the platform
    // window is created. Force creation if the window hasn't been
    // exposed yet (winId is also a creation trigger).
    // QWindow::winId() returns WId (unsigned long long). On macOS this
    // is an NSView*. Cast through void* + __bridge to satisfy ARC.
    NSView *nsView = (__bridge NSView *)reinterpret_cast<void *>(window->winId());
    if (!nsView) {
        qWarning("MetalDeviceManager: QWindow has no native NSView yet");
        return false;
    }

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        qWarning("MetalDeviceManager: MTLCreateSystemDefaultDevice failed");
        return false;
    }
    m_impl->device = device;

    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!queue) {
        qWarning("MetalDeviceManager: newCommandQueue failed");
        m_impl->device = nil;
        return false;
    }
    m_impl->commandQueue = queue;

    // Layer attachment strategy: Qt's QNSView keeps its own
    // QContainerLayer (or whatever it set up) as the root layer of
    // the view, and OWNS that layer's lifecycle for drawing
    // callbacks like `displayLayer:`. Replacing the root layer
    // crashes Qt mid-paint (objc_msgSend on a deleted Qt object).
    //
    // Instead, we ADD a CAMetalLayer as a sublayer of Qt's root.
    // Qt's drawing path is undisturbed; we own a transparent sublayer
    // we render into. resize() keeps the sublayer's frame matching
    // the parent's bounds.
    [nsView setWantsLayer:YES];
    if (!nsView.layer) {
        qWarning("MetalDeviceManager: NSView has no layer after setWantsLayer:YES");
        m_impl->device = nil;
        return false;
    }

    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.device              = device;
    layer.pixelFormat         = MTLPixelFormatBGRA8Unorm;     // Phase B.3 swaps to FP16 for HDR
    layer.framebufferOnly     = YES;                          // Phase B.4 may flip to NO if readback needed
    layer.displaySyncEnabled  = YES;                          // vsync ON (matches old app)
    layer.maximumDrawableCount = 3;                           // matches old app
    layer.frame               = nsView.bounds;                // tracks parent on resize

    // Match the NSView's backing-store size in pixels. QWindow::size()
    // is in points; multiply by devicePixelRatio for retina.
    const CGFloat dpr = nsView.window ? nsView.window.backingScaleFactor : 1.0;
    const CGSize  px  = CGSizeMake(window->width()  * dpr,
                                   window->height() * dpr);
    layer.drawableSize = px;
    layer.contentsScale = dpr;

    [nsView.layer addSublayer:layer];

    m_impl->layer  = layer;
    m_initialized  = true;

    qInfo("MetalDeviceManager: initialized — device=%s drawable=%dx%d dpr=%.1f",
          [device.name UTF8String],
          static_cast<int>(px.width), static_cast<int>(px.height),
          static_cast<double>(dpr));
    return true;
}

void MetalDeviceManager::shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) return;

    // ARC releases on assignment to nil. The CAMetalLayer was set on
    // the NSView; the view holds its own reference and will release
    // when the view goes away. We do not detach here — that's the
    // owning window's job.
    m_impl->commandQueue = nil;
    m_impl->layer        = nil;
    m_impl->device       = nil;
    m_initialized = false;
}

void *MetalDeviceManager::device() const
{
    return (__bridge void *)m_impl->device;
}

void *MetalDeviceManager::commandQueue() const
{
    return (__bridge void *)m_impl->commandQueue;
}

void *MetalDeviceManager::metalLayer() const
{
    return (__bridge void *)m_impl->layer;
}

void *MetalDeviceManager::createCommandBuffer()
{
    if (!m_impl->commandQueue) return nullptr;
    id<MTLCommandBuffer> cb = [m_impl->commandQueue commandBuffer];
    // __bridge_retained: hand a +1 reference out to the caller; they
    // must CFRelease (or __bridge_transfer back to ARC) to balance.
    return (__bridge_retained void *)cb;
}

void MetalDeviceManager::waitForGpu()
{
    if (!m_impl->commandQueue) return;
    id<MTLCommandBuffer> sync = [m_impl->commandQueue commandBuffer];
    [sync commit];
    [sync waitUntilCompleted];
}

const char *MetalDeviceManager::deviceName() const
{
    if (!m_impl->device) return "Unknown";
    return [m_impl->device.name UTF8String];
}

std::size_t MetalDeviceManager::recommendedMaxWorkingSetBytes() const
{
    if (!m_impl->device) return 0;
    return static_cast<std::size_t>(m_impl->device.recommendedMaxWorkingSetSize);
}

} // namespace qcv
