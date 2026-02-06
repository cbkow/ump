#pragma once

#ifdef _WIN32

#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <mutex>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace ump {

//=============================================================================
// FrameReadback
//
// Reads back decoded video frames from D3D11 textures to CPU AVFrames.
// Used for:
// - Dual view: readback secondary stream to CPU, upload to GL separately
// - Frame export: save frames to disk
// - CPU-side processing
//
// Design:
// - Pooled staging textures for async readback
// - Supports NV12/P010 (HW decode) and planar YUV (SW decode)
// - Thread-safe slot management
//=============================================================================

class FrameReadback {
public:
    FrameReadback();
    ~FrameReadback();

    // Non-copyable
    FrameReadback(const FrameReadback&) = delete;
    FrameReadback& operator=(const FrameReadback&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    // Initialize with D3D11 device
    bool Initialize(ID3D11Device* device);
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Synchronous Readback (blocking)
    //=========================================================================

    // Readback D3D11 texture to a new AVFrame (allocates frame)
    // Returns nullptr on failure
    // src_texture: Source D3D11 texture (NV12/P010 or RGBA)
    // dest_format: Desired output pixel format (AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P, etc.)
    AVFrame* ReadbackToNewFrame(ID3D11Texture2D* src_texture,
                                 AVPixelFormat dest_format = AV_PIX_FMT_NV12);

    // Readback D3D11 texture to existing AVFrame
    // dest_frame must be allocated with correct dimensions and format
    bool ReadbackToFrame(ID3D11Texture2D* src_texture, AVFrame* dest_frame);

    //=========================================================================
    // Async Readback (non-blocking queue + poll)
    //=========================================================================

    // Queue a texture for async readback
    // Returns slot ID (0-3) or -1 on failure
    // The GPU copy happens immediately, but Map is deferred
    int QueueReadback(ID3D11Texture2D* src_texture);

    // Check if async readback is complete
    bool IsReadbackComplete(int slot_id);

    // Get result of async readback (blocks until complete if not ready)
    // Returns nullptr on failure, caller owns the returned frame
    AVFrame* GetReadbackResult(int slot_id);

    // Cancel pending readback and free slot
    void CancelReadback(int slot_id);

    //=========================================================================
    // Texture Format Helpers
    //=========================================================================

    // Get AVPixelFormat from DXGI_FORMAT
    static AVPixelFormat DXGIToAVPixelFormat(DXGI_FORMAT format);

    // Get DXGI_FORMAT from AVPixelFormat
    static DXGI_FORMAT AVPixelFormatToDXGI(AVPixelFormat format);

private:
    //=========================================================================
    // Staging Slot
    //=========================================================================

    static constexpr int kStagingSlotCount = 4;

    struct StagingSlot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_texture;
        Microsoft::WRL::ComPtr<ID3D11Query> query;  // For async completion check
        int width = 0;
        int height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        bool pending = false;      // Copy queued, waiting for GPU
        bool ready = false;        // GPU done, ready for Map
        bool in_use = false;       // Slot allocated
    };

    std::array<StagingSlot, kStagingSlotCount> slots_;
    std::mutex slots_mutex_;

    //=========================================================================
    // Internal Helpers
    //=========================================================================

    // Find or create a staging texture matching dimensions
    int AllocateSlot(int width, int height, DXGI_FORMAT format);
    void FreeSlot(int slot_id);

    // Ensure staging texture is correct size
    bool EnsureStagingTexture(StagingSlot& slot, int width, int height, DXGI_FORMAT format);

    // Map staging texture and copy to AVFrame
    bool MapAndCopyToFrame(StagingSlot& slot, AVFrame* dest_frame);

    //=========================================================================
    // State
    //=========================================================================

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    bool initialized_ = false;
};

} // namespace ump

#endif // _WIN32
