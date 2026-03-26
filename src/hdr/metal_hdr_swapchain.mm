#ifdef __APPLE__

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Cocoa/Cocoa.h>

#include "metal_hdr_swapchain.h"
#include "../gpu/metal_device_manager.h"
#include "../utils/debug_utils.h"
#include <imgui_impl_metal.h>

// Defined in metal_imgui_bridge.mm
extern void SetMetalEDRState(bool active, float edr_scale);
extern void SetMetalEDRLinear(bool is_linear);

#ifdef QCVIEW_USE_METAL
extern void* MacOS_GetNSView();
#endif

namespace qcview {

MetalHDRSwapchain::MetalHDRSwapchain() = default;

MetalHDRSwapchain::~MetalHDRSwapchain() {
    Shutdown();
}

bool MetalHDRSwapchain::Initialize(void* ns_window_ptr) {
    if (!ns_window_ptr) return false;

    window_ = ns_window_ptr;  // Already NSWindow* (native macOS) or from glfwGetCocoaWindow
    metal_layer_ = MetalDeviceManager::Instance().GetMetalLayer();

    Debug::Log("MetalHDRSwapchain: Initialized (EDR headroom: " +
               std::to_string(GetMaxEDRHeadroom()) + ")");
    return true;
}

void MetalHDRSwapchain::Shutdown() {
    if (hdr_active_) {
        SetHDREnabled(false);
    }
    metal_layer_ = nullptr;
    window_ = nullptr;
}

bool MetalHDRSwapchain::IsHDRSupported() const {
    if (!window_) return false;
    NSWindow* ns_window = (__bridge NSWindow*)window_;
    NSScreen* screen = [ns_window screen];
    if (!screen) return false;

    CGFloat headroom = [screen maximumPotentialExtendedDynamicRangeColorComponentValue];
    return headroom > 1.0;
}

float MetalHDRSwapchain::GetMaxEDRHeadroom() const {
    if (!window_) return 1.0f;
    NSWindow* ns_window = (__bridge NSWindow*)window_;
    NSScreen* screen = [ns_window screen];
    if (!screen) return 1.0f;

    return static_cast<float>([screen maximumPotentialExtendedDynamicRangeColorComponentValue]);
}

bool MetalHDRSwapchain::SetHDREnabled(bool enabled) {
    if (!metal_layer_) return false;

    if (!ImGui::GetCurrentContext() || !ImGui::GetIO().BackendRendererUserData) {
        hdr_active_ = false;
        return false;
    }

    CAMetalLayer* layer = (__bridge CAMetalLayer*)metal_layer_;

    if (enabled) {
        layer.pixelFormat = MTLPixelFormatRGBA16Float;
        layer.wantsExtendedDynamicRangeContent = YES;

#ifdef QCVIEW_USE_METAL
        // Update MTKView's colorPixelFormat to match (MTKView owns the layer)
        if (MacOS_GetNSView()) {
            ((__bridge MTKView*)MacOS_GetNSView()).colorPixelFormat = MTLPixelFormatRGBA16Float;
        }
#endif

        ApplyColorspace();

        bool is_linear = EDRColorspaceIsLinear(edr_colorspace_);
        if (is_linear) {
            ImGui_ImplMetal_SetHDRMode(true, edr_ui_scale_);
        } else {
            ImGui_ImplMetal_SetHDRMode(false, 1.0f);
        }
        SetMetalEDRState(true, edr_ui_scale_);
        SetMetalEDRLinear(is_linear);

        hdr_active_ = true;
        Debug::Log("MetalHDRSwapchain: EDR enabled (" +
                   std::string(EDRColorspaceName(edr_colorspace_)) +
                   ", headroom: " + std::to_string(GetMaxEDRHeadroom()) +
                   "x, UI scale: " + std::to_string(edr_ui_scale_) + ")");
    } else {
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.colorspace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        layer.wantsExtendedDynamicRangeContent = NO;

#ifdef QCVIEW_USE_METAL
        if (MacOS_GetNSView()) {
            ((__bridge MTKView*)MacOS_GetNSView()).colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        }
#endif

        ImGui_ImplMetal_SetHDRMode(false, 1.0f);
        SetMetalEDRState(false, 1.0f);
        SetMetalEDRLinear(false);

        hdr_active_ = false;
        Debug::Log("MetalHDRSwapchain: EDR disabled (SDR mode)");
    }

    return true;
}

void MetalHDRSwapchain::ApplyColorspace() {
    if (!metal_layer_) return;
    CAMetalLayer* layer = (__bridge CAMetalLayer*)metal_layer_;

    CFStringRef cs_name;
    switch (edr_colorspace_) {
        case EDRColorspace::LinearSRGB:
            cs_name = kCGColorSpaceExtendedLinearSRGB;
            break;
        case EDRColorspace::LinearDisplayP3:
            cs_name = kCGColorSpaceExtendedLinearDisplayP3;
            break;
        case EDRColorspace::GammaSRGB:
            cs_name = kCGColorSpaceExtendedSRGB;
            break;
        case EDRColorspace::GammaDisplayP3:
            cs_name = kCGColorSpaceExtendedDisplayP3;
            break;
        default:
            cs_name = kCGColorSpaceExtendedLinearSRGB;
            break;
    }

    CGColorSpaceRef colorspace = CGColorSpaceCreateWithName(cs_name);
    if (colorspace) {
        layer.colorspace = colorspace;
        CGColorSpaceRelease(colorspace);
    }
}

void MetalHDRSwapchain::SetEDRColorspace(EDRColorspace cs) {
    if (cs == edr_colorspace_) return;
    edr_colorspace_ = cs;

    if (hdr_active_) {
        SetHDREnabled(true);
    }
}

void MetalHDRSwapchain::SetUIBrightness(float edr_scale) {
    edr_ui_scale_ = edr_scale;
    if (hdr_active_) {
        ImGui_ImplMetal_SetHDRMode(true, edr_ui_scale_);
        SetMetalEDRState(true, edr_ui_scale_);
    }
}

void MetalHDRSwapchain::RecreateIfNeeded(void* ns_window_ptr) {
    // With native MTKView, drawable size is managed automatically by the view.
    // This method is kept for API compatibility but is a no-op on native macOS.
    // On GLFW builds, it would update layer.drawableSize on resize.
    (void)ns_window_ptr;
}

} // namespace qcview

#endif // __APPLE__
