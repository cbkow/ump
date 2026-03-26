#pragma once

#ifdef __APPLE__

#include <string>

namespace qcview {

//=============================================================================
// EDR Colorspace options for macOS
//=============================================================================

enum class EDRColorspace {
    LinearSRGB = 0,      // kCGColorSpaceExtendedLinearSRGB — linear, BT.709/sRGB primaries
    LinearDisplayP3,     // kCGColorSpaceExtendedLinearDisplayP3 — linear, P3 primaries
    GammaSRGB,           // kCGColorSpaceExtendedSRGB — sRGB gamma, BT.709/sRGB primaries
    GammaDisplayP3,      // kCGColorSpaceExtendedDisplayP3 — sRGB gamma, P3 primaries
    Count
};

inline const char* EDRColorspaceName(EDRColorspace cs) {
    switch (cs) {
        case EDRColorspace::LinearSRGB:      return "Linear sRGB";
        case EDRColorspace::LinearDisplayP3: return "Linear Display P3";
        case EDRColorspace::GammaSRGB:       return "sRGB (Gamma)";
        case EDRColorspace::GammaDisplayP3:  return "Display P3 (Gamma)";
        default:                             return "Unknown";
    }
}

inline bool EDRColorspaceIsLinear(EDRColorspace cs) {
    return cs == EDRColorspace::LinearSRGB || cs == EDRColorspace::LinearDisplayP3;
}

//=============================================================================
// MetalHDRSwapchain
//
// Configures CAMetalLayer for EDR (Extended Dynamic Range) on macOS.
// macOS uses linear-light EDR instead of PQ/ST.2084.
//=============================================================================

class MetalHDRSwapchain {
public:
    MetalHDRSwapchain();
    ~MetalHDRSwapchain();

    bool Initialize(void* ns_window);
    void Shutdown();

    // HDR control
    bool IsHDRSupported() const;
    bool IsHDRActive() const { return hdr_active_; }
    bool SetHDREnabled(bool enabled);

    // EDR headroom
    float GetMaxEDRHeadroom() const;

    // UI brightness (EDR scale: 1.0 = SDR white, >1.0 = brighter)
    void SetUIBrightness(float edr_scale);
    float GetUIBrightness() const { return edr_ui_scale_; }

    // EDR colorspace selection
    void SetEDRColorspace(EDRColorspace cs);
    EDRColorspace GetEDRColorspace() const { return edr_colorspace_; }

    // Resize handling
    void RecreateIfNeeded(void* ns_window);

private:
    void ApplyColorspace();  // Apply current colorspace to layer

    void* metal_layer_ = nullptr;  // CAMetalLayer*
    void* window_ = nullptr;       // NSWindow*
    bool hdr_active_ = false;
    // EDR scale: 1.0 = SDR white (macOS maps to display nits automatically)
    // Unlike PQ/Vulkan where we specify absolute nits, EDR is relative.
    float edr_ui_scale_ = 1.0f;
    EDRColorspace edr_colorspace_ = EDRColorspace::LinearSRGB;
};

} // namespace qcview

#endif // __APPLE__
