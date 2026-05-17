// D3D11VulkanYuvCompositor — Phase F.2.13.
//
// Shared Vulkan compute pipeline that converts planar/biplanar YUV(A)
// or planar GBR(A) from FFmpeg's hwaccel-decoded VkImages into RGBA16F
// in a caller-supplied D3D11/Vulkan-shared VkImage.
//
// This is the macOS-faithful split of the old D3D11VulkanDecodeBridge:
// macOS keeps per-slot input bridges (CvPixbufMetalBridge[2]) but uses
// a SINGLE shared MetalYuvRenderer — see
// `src/render/metal/dual_pixbuf_converter_impl.mm` for the model.
// Replicating the compute pipeline per stream (the old shape) caused
// dual-mode stutter on Intel Arc; one shared pipeline + per-stream
// caches matches the macOS pattern and fixes it.
//
// Lifetime model:
//  - One instance owned by D3D11PlayerRenderer, initialized once at
//    renderer init() time. Outlives every bridge that points at it.
//  - Bridges hold a non-owning pointer injected at their initialize().
//
// Threading: dispatch() is called only from the render thread (the
// only thread that owns the D3D11 immediate context). The shared
// descriptor set + cmd buf are serialized GPU-side by a timeline
// semaphore wait at the top of each dispatch — multiple bridges
// dispatching back-to-back on the same thread serialize cleanly.

#pragma once

#include <cstdint>
#include <memory>

// Vulkan / D3D11 types stay opaque in this header; the .cpp owns all
// the API plumbing.
struct AVFrame;
typedef struct VkImage_T              *VkImage;
typedef struct VkImageView_T          *VkImageView;

namespace qcv {

class D3D11VulkanYuvCompositor {
public:
    D3D11VulkanYuvCompositor();
    ~D3D11VulkanYuvCompositor();

    D3D11VulkanYuvCompositor(const D3D11VulkanYuvCompositor &)            = delete;
    D3D11VulkanYuvCompositor &operator=(const D3D11VulkanYuvCompositor &) = delete;

    // Stand up the compute pipeline + cross-API timeline fence. Needs
    // VulkanDeviceManager and D3D11DeviceManager already initialized.
    // Returns true on success; logs and returns false on driver gaps
    // (caller falls back to dropping Vulkan frames).
    bool initialize();
    void shutdown();
    bool isInitialized() const;

    // Parameters for a single compute dispatch. Caller (the bridge)
    // resolves per-stream state — plane views + their formats are
    // looked up via the bridge's own viewCache; output points at the
    // bridge's shared RGBA16F VkImage.
    struct DispatchParams {
        // Source planes — 4 entries. For 3-plane YUV/GBRP, [3] should
        // duplicate [2] (or any valid view) to keep the descriptor set
        // well-formed; the shader's `hasAlpha` push constant gates
        // sampling of [3]. For biplanar (NV12/P010 etc), [0] is the
        // luma PLANE_0 view, [1] is the interleaved UV PLANE_1 view,
        // [2]/[3] are duplicates of [0].
        VkImageView samplerViews[4];

        // Destination: the bridge's imported VkImage and its color view.
        // The compositor transitions UNDEFINED→GENERAL before the
        // compute write and GENERAL→SHADER_READ_ONLY_OPTIMAL after, so
        // the D3D11-side sample sees a valid layout.
        VkImage     outputImage;
        VkImageView outputView;

        // Picture dimensions (push-constant width/height for the
        // dispatch and the layout barriers).
        int width;
        int height;

        // Shader format selectors (driven by AVFrame metadata on the
        // caller side — keeps format-detection logic out of the shared
        // compositor).
        float bitScale;     // 1.0 for biplanar / 16-bit / 8-bit; 65535/1023
                            //  for 10-bit planar; 65535/4095 for 12-bit
        int   colorSpace;   // 0=BT.601, 1=BT.709, 2=BT.2020
        int   range;        // 0=limited (MPEG), 1=full (JPEG)
        int   hasAlpha;     // 1 if samplerViews[3] is real alpha
        int   isRgb;        // 1 if planes are G/B/R (GBRP family)
        int   isBiplanar;   // 1 if samplerViews[1] is interleaved UV
    };

    // Run the YUV→RGB compute pass. Serializes GPU-side on the shared
    // timeline semaphore so back-to-back dispatches from different
    // bridges don't stomp on the shared descriptor set / cmd buf.
    // Returns false on submit failure; on the timeline-fence path,
    // queues a D3D11 GPU-side Wait so the next D3D11 sample of
    // `outputImage`'s sibling SRV stalls correctly.
    bool dispatch(const DispatchParams &params);

    // Diagnostic — log which (format, handle-type) export combinations
    // the physical Vulkan device supports. Useful for triaging the
    // Intel Arc R16_UNORM rejection vs broader driver limits. Called
    // from initialize() once; safe to call again.
    void probeExternalMemoryCapabilities();

public:
    // PIMPL kept publicly forward-declared so the bridge's helpers can
    // friend it if ever needed (kept opaque in practice — same pattern
    // the old bridge used).
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace qcv
