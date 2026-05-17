// MetalHdrSwapchain — see header for adaptation notes.

#include "metal_hdr_swapchain.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <QWindow>
#include <QtLogging>

namespace qcv {

namespace {

// Resolve the NSWindow that backs `qwindow`, walking up to the parent
// chain if needed. `qwindow->winId()` returns the NSView; the NSView
// belongs to an NSWindow. Returns nil if not yet realized.
NSWindow *nsWindowFor(QWindow *qwindow)
{
    if (!qwindow) return nil;
    NSView *view = (__bridge NSView *)reinterpret_cast<void *>(qwindow->winId());
    return view ? view.window : nil;
}

} // namespace

MetalHdrSwapchain::MetalHdrSwapchain() = default;

MetalHdrSwapchain::~MetalHdrSwapchain()
{
    shutdown();
}

void MetalHdrSwapchain::initialize(void *layer, QWindow *window)
{
    m_layer  = layer;
    m_window = window;
    // Initial state mirrors what MetalDeviceManager set — BGRA8Unorm,
    // no EDR. Mark applied as SdrSRgb so the first apply() can detect
    // a change.
    m_appliedMode = HdrMode::SdrSRgb;
    m_pendingMode = HdrMode::SdrSRgb;
}

void MetalHdrSwapchain::shutdown()
{
    m_layer  = nullptr;
    m_window = nullptr;
}

void MetalHdrSwapchain::setMode(HdrMode mode)
{
    m_pendingMode = mode;
}

void MetalHdrSwapchain::applyPendingOnRenderThread()
{
    if (m_pendingMode == m_appliedMode) return;
    applyImpl(m_pendingMode);
}

void MetalHdrSwapchain::applyImpl(HdrMode mode)
{
    if (!m_layer) {
        m_appliedMode = mode;
        return;
    }

    CAMetalLayer *layer = (__bridge CAMetalLayer *)m_layer;

    // macOS doesn't expose a PQ surface format. Hdr10 falls back to
    // the ExtendedLinearSRgb path — visually distinct from SDR but
    // not the wider Rec.2020 PQ that Vulkan offers. Warn once.
    HdrMode effective = mode;
    if (effective == HdrMode::Hdr10) {
        static bool warned = false;
        if (!warned) {
            qWarning("MetalHdrSwapchain: Hdr10 not supported on macOS; "
                     "falling back to ExtendedLinearSRgb (EDR)");
            warned = true;
        }
        effective = HdrMode::ExtendedLinearSRgb;
    }

    CFStringRef csName = nullptr;
    MTLPixelFormat pixelFmt = MTLPixelFormatBGRA8Unorm;
    BOOL wantsEdr = NO;

    switch (effective) {
        case HdrMode::SdrSRgb:
            pixelFmt = MTLPixelFormatBGRA8Unorm;
            csName   = kCGColorSpaceSRGB;
            wantsEdr = NO;
            break;
        case HdrMode::SdrDisplayP3:
            pixelFmt = MTLPixelFormatBGRA8Unorm;
            csName   = kCGColorSpaceDisplayP3;
            wantsEdr = NO;
            break;
        case HdrMode::ExtendedLinearSRgb:
            pixelFmt = MTLPixelFormatRGBA16Float;
            csName   = kCGColorSpaceExtendedLinearSRGB;
            wantsEdr = YES;
            break;
        case HdrMode::ExtendedLinearDisplayP3:
            pixelFmt = MTLPixelFormatRGBA16Float;
            csName   = kCGColorSpaceExtendedLinearDisplayP3;
            wantsEdr = YES;
            break;
        case HdrMode::Hdr10:
        default:
            // Already mapped to ExtendedLinearSRgb above.
            pixelFmt = MTLPixelFormatRGBA16Float;
            csName   = kCGColorSpaceExtendedLinearSRGB;
            wantsEdr = YES;
            break;
    }

    layer.pixelFormat = pixelFmt;
    layer.wantsExtendedDynamicRangeContent = wantsEdr;
    if (csName) {
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(csName);
        if (cs) {
            layer.colorspace = cs;
            CGColorSpaceRelease(cs);
        }
    }

    m_appliedMode = mode;   // store the user's choice, even if we mapped Hdr10

    qInfo("MetalHdrSwapchain: applied mode=%d (pixelFmt=%lu, EDR=%s, headroom=%.2f)",
          static_cast<int>(mode),
          static_cast<unsigned long>(pixelFmt),
          wantsEdr ? "on" : "off",
          static_cast<double>(maxEdrHeadroom()));
}

bool MetalHdrSwapchain::isHdrSupported() const
{
    return maxEdrHeadroom() > 1.0f;
}

float MetalHdrSwapchain::maxEdrHeadroom() const
{
    NSWindow *win = nsWindowFor(m_window);
    NSScreen *screen = win ? win.screen : nil;
    if (!screen) return 1.0f;
    return static_cast<float>(
        screen.maximumPotentialExtendedDynamicRangeColorComponentValue);
}

} // namespace qcv
