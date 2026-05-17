// FrameHandle — Phase 1.8.2b: discriminated handle for a decoded
// video frame, carrying either a CPU QImage (software path / fallback)
// or an opaque platform-native GPU handle (zero-copy hwaccel path).
//
// The handle owns the underlying resource and releases it on destroy:
//   - QImage: implicit Qt refcount
//   - CVPixelBufferRef (macOS): CFRelease on destroy
//   - AVFrame* (Phase F.2.4 Windows): av_frame_free on destroy. The
//     AVFrame's data[0] is an AVVkFrame containing a VkImage on our
//     shared decode-only VkDevice (qcv::VulkanDeviceManager).
//
// FrameHandle is non-copyable, move-only. Construct via the static
// factories (cpu(), metal(), vulkan()) or the default no-frame
// constructor.

#pragma once

#include <QImage>
#include <cstdint>
#include <memory>

// Forward decl so this header doesn't drag <libavutil/frame.h> through
// every consumer. Real type lives in libavutil; the .cpp on non-Apple
// (frame_handle_nonapple.cpp) includes the actual FFmpeg headers.
struct AVFrame;

namespace qcv {

class FrameHandle
{
public:
    enum class Kind {
        Empty   = 0,
        Cpu     = 1,    // software path: cpuImage holds RGBA8 pixels
        Metal   = 2,    // macOS: metalPixelBuffer holds CVPixelBufferRef
        Vulkan  = 3,    // Win/Linux: avFrame owns an AVVkFrame with a
                        // VkImage on our shared VkDevice. Bridge in
                        // D3D11VulkanDecodeBridge (F.2.4.3) imports it
                        // into an ID3D11Texture2D via NT shared handle.
    };

    FrameHandle() = default;
    ~FrameHandle() { reset(); }

    FrameHandle(const FrameHandle &) = delete;
    FrameHandle &operator=(const FrameHandle &) = delete;

    FrameHandle(FrameHandle &&other) noexcept { *this = std::move(other); }
    FrameHandle &operator=(FrameHandle &&other) noexcept;

    static FrameHandle cpu(QImage image, int64_t pts);

    // Phase 7.4.b: zero-copy publish for image-sequence frames.
    // The QImage is a NON-owning view into a buffer kept alive by
    // `keepAlive`, which is type-erased so this header doesn't need
    // to know about decode/PixelData.h. Saves ~70 MB / frame on 4K
    // EXR FP16 vs the cpu()-with-copy path — at 24 fps that's
    // 1.7 GB/sec of pure memcpy avoided.
    static FrameHandle cpuShared(QImage view,
                                 std::shared_ptr<void> keepAlive,
                                 int64_t pts);

    // metalPixelBuffer is a CVPixelBufferRef passed as void* to keep
    // this header free of CoreVideo. The factory takes ownership and
    // assumes the caller has already retained it.
    static FrameHandle metal(void *cvPixelBuffer, int width, int height,
                             int64_t pts);

    // Phase F.2.4 Windows hybrid: Vulkan-decoded frame on our shared
    // VkDevice. `avFrame` MUST be a freshly cloned ref (av_frame_clone);
    // FrameHandle takes ownership and av_frame_free's it on destroy.
    // `width`/`height` are the picture dimensions (NOT the VkImage's
    // padded / aligned extent — video-decode VkImages are often larger
    // than the displayable picture).
    static FrameHandle vulkan(AVFrame *avFrame, int width, int height,
                              int64_t pts);

    Kind     kind() const { return m_kind; }
    bool     isValid() const { return m_kind != Kind::Empty; }
    int64_t  pts() const { return m_pts; }

    // Cpu accessors — undefined if kind() != Cpu.
    const QImage &cpuImage() const { return m_cpuImage; }

    // Metal accessors — undefined if kind() != Metal.
    void *metalPixelBuffer() const { return m_metalPixbuf; }
    int   width() const { return m_width; }
    int   height() const { return m_height; }

    // Vulkan accessors — undefined if kind() != Vulkan.
    // Returns the owning AVFrame (DO NOT free; FrameHandle owns it).
    // The bridge extracts AVVkFrame from avFrame->data[0] and reads
    // format / sw_format from avFrame->hw_frames_ctx.
    AVFrame *vulkanAvFrame() const { return m_avFrame; }

    void reset();

private:
    Kind    m_kind = Kind::Empty;
    int64_t m_pts = 0;
    int     m_width = 0;
    int     m_height = 0;

    QImage  m_cpuImage;     // Cpu kind
    void   *m_metalPixbuf = nullptr;  // CVPixelBufferRef, retained
    AVFrame *m_avFrame    = nullptr;  // Vulkan kind, av_frame_free'd
    // Type-erased keepalive for the cpuShared path. Non-null for
    // FrameHandles built via cpuShared(); the QImage's bits point
    // into this object's buffer and stay valid until the FrameHandle
    // is destroyed (or moved out, which transfers ownership).
    std::shared_ptr<void> m_keepAlive;
};

} // namespace qcv
