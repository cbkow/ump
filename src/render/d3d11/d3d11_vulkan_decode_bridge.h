// D3D11VulkanDecodeBridge — Phase F.2.4.3 / split at F.2.13.
//
// Per-stream input/output cache for a single FFmpeg-decoded Vulkan
// video feed. The shared compute pipeline lives in
// D3D11VulkanYuvCompositor (one renderer-owned instance), which this
// bridge points at via a non-owning pointer. This bridge holds only
// the stream-specific state: the imported D3D11/Vulkan output texture,
// the per-VkImage view cache (FFmpeg's pool of ~26 distinct images),
// the format/dim cache, and the ImportedFrame returned to callers.
//
// Two bridges (e.g. dual-mode A and B) safely share one yuv compositor.
// macOS uses the same pattern: CvPixbufMetalBridge[2] + one shared
// MetalYuvRenderer (see dual_pixbuf_converter_impl.mm).
//
// Lifetime: the bridge must be destroyed before the compositor it
// points at. D3D11PlayerRenderer enforces this via Impl member
// declaration order (compositor declared first → destroyed last).

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

// Forward declarations to keep this header light. Real Vulkan + D3D11
// types only show up in the .cpp via void* + opaque struct pointers.
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct AVFrame;

namespace qcv {

class FrameHandle;
class D3D11VulkanYuvCompositor;

class D3D11VulkanDecodeBridge {
public:
    D3D11VulkanDecodeBridge();
    ~D3D11VulkanDecodeBridge();

    D3D11VulkanDecodeBridge(const D3D11VulkanDecodeBridge &)            = delete;
    D3D11VulkanDecodeBridge &operator=(const D3D11VulkanDecodeBridge &) = delete;

    // Reachable once both qcv::VulkanDeviceManager + qcv::D3D11DeviceManager
    // are initialized AND the shared compositor is alive (typically
    // called from D3D11PlayerRenderer::init after yuvCompositor.init).
    // The compositor pointer is non-owning and must outlive this
    // bridge. Returns false if either device isn't ready or the
    // compositor is null/uninitialized.
    bool initialize(D3D11VulkanYuvCompositor *yuvCompositor);
    void shutdown();
    bool isInitialized() const;

    // Per-plane handles produced by consume(). The renderer samples
    // these from HLSL; the bridge retains ownership of the underlying
    // resources for re-use across frames (LRU lands in Stage C).
    struct ImportedPlane {
        ID3D11Texture2D          *texture;   // ref'd; lifetime = bridge
        ID3D11ShaderResourceView *srv;       // ref'd; lifetime = bridge
        int                       width;     // plane pixel width
        int                       height;    // plane pixel height
        int                       dxgiFormat;// DXGI_FORMAT cast to int
    };

    struct ImportedFrame {
        std::vector<ImportedPlane> planes;       // 1 for NV12/P010 multi-plane,
                                                  //  N for per-plane YUV formats
        int                        pictureWidth;  // top-level picture dims (may
        int                        pictureHeight; //  differ from plane dims)
        int                        avSwFormat;    // AVPixelFormat of the underlying
                                                  //  YUV (NV12/P010/YUV422P10LE/...)
        uint64_t                   frameSequence; // bumped on each consume() —
                                                  //  renderer compares to skip
                                                  //  re-uploads when unchanged
    };

    // Consume a FrameHandle::Vulkan: wait on FFmpeg's semaphore, copy
    // VkImage planes into the bridge's exportable images, signal the
    // cross-API timeline, return the D3D11 SRVs for the renderer to
    // sample. Returns nullptr if the FrameHandle isn't a Vulkan kind
    // or if the bridge isn't initialized.
    //
    // `rangeOverride` mirrors the per-clip videoRangeOverride pill on
    // MediaItem (0 = Auto / detect from avFrame->color_range,
    // 1 = Full / PC-range, 2 = Limited / TV-range). Caller passes
    // VideoDecoder::rangeOverride() each frame; macOS does the same in
    // metal_player_renderer.mm:1198-1204.
    const ImportedFrame *consume(const FrameHandle &fh,
                                  int rangeOverride = 0);

    // F.2.12.e — same work as `consume(FrameHandle)` but takes the
    // raw owning AVFrame* directly. The dual compositor pulls
    // AVFrame* opaquely from its IDualFrameSource payload (qcv_render
    // can't link qcv_dual, so the dual side doesn't go through
    // FrameHandle). Caller retains AVFrame ownership; the bridge
    // reads the AVVkFrame for the duration of the call.
    const ImportedFrame *consumeAVFrame(AVFrame *avFrame,
                                         int rangeOverride = 0);

    // Drop all imported resources (called on renderer shutdown OR on
    // source switch so stale VkImages don't outlive their VkDevice).
    void reset();

public:
    // Impl is forward-declared here because the .cpp's helper
    // functions need to construct Impl::BridgeImage values. C++
    // access control on nested types inside a unique_ptr-managed
    // PIMPL is awkward to thread through free helpers; making Impl
    // public-as-an-opaque-name is the lightest workaround. The
    // type's MEMBERS stay defined only in the .cpp, so callers
    // can't poke at internals.
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace qcv
