#pragma once

#ifdef _WIN32

#include <memory>
#include <atomic>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <glad/gl.h>

#include "../gpu/dual_view_layout.h"

namespace ump {

// Forward declarations
class D3D11DualCompositor;
class D3D11VideoInterop;
class D3D11VideoDecoder;

//=============================================================================
// DualViewPipeline
//
// Unified pipeline for dual view video playback that solves the buffer
// exhaustion problem caused by two independent D3D11->GL interops.
//
// Architecture:
// - Two D3D11VideoDecoders produce YUV frames to D3D11 textures
// - D3D11DualCompositor combines both into a single composite texture
// - Single D3D11VideoInterop shares composite to OpenGL
// - OpenGL uses UV coordinates to sample LEFT/RIGHT halves
//
// This design ensures:
// - One state machine (single interop lock/unlock cycle)
// - GPU-efficient compositing before interop
// - No competing triple-buffer systems
//=============================================================================

class DualViewPipeline {
public:
    DualViewPipeline();
    ~DualViewPipeline();

    // Non-copyable
    DualViewPipeline(const DualViewPipeline&) = delete;
    DualViewPipeline& operator=(const DualViewPipeline&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    // Initialize with D3D11 device and source dimensions
    bool Initialize(ID3D11Device* device,
                    int left_width, int left_height,
                    int right_width, int right_height);

    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Decoder Registration
    //=========================================================================

    // Set references to the decoders (owned externally, e.g., by TimelineCache)
    void SetDecoders(D3D11VideoDecoder* left, D3D11VideoDecoder* right);

    //=========================================================================
    // Frame Update
    //=========================================================================

    // Update composite frame from both sources
    // Returns GL texture ID of the composite, or 0 if not ready
    // frame_left, frame_right: Source frame numbers (may differ for FPS mismatch)
    GLuint UpdateFrame(int64_t frame_left, int64_t frame_right);

    // Get dimensions of the output GL texture
    void GetOutputDimensions(int& width, int& height) const;

    //=========================================================================
    // Layout Access
    //=========================================================================

    // Get current layout for UV calculations
    const DualViewLayout& GetLayout() const { return layout_; }

    // Force layout recalculation (call after source dimension change)
    void InvalidateLayout();

    //=========================================================================
    // Playback State
    //=========================================================================

    // Notify pipeline of playback mode change (for watchdog)
    void SetPlaybackMode(bool playing);

    // Seek notification - invalidates pending frames
    void OnSeek(int64_t target_frame);

    //=========================================================================
    // Frame Synchronization
    //=========================================================================

    // Check if both sources have frames ready
    bool AreBothFramesReady(int64_t frame_left, int64_t frame_right) const;

    // Get wait status (for audio sync)
    bool IsWaitingForBothFrames() const { return waiting_for_both_frames_.load(); }

private:
    //=========================================================================
    // Internal Methods
    //=========================================================================

    bool EnsureCompositorAndInterop();
    bool UpdateLayout(int left_w, int left_h, int right_w, int right_h);

    //=========================================================================
    // Components
    //=========================================================================

    std::unique_ptr<D3D11DualCompositor> compositor_;
    std::unique_ptr<D3D11VideoInterop> interop_;

    //=========================================================================
    // Decoder References (not owned)
    //=========================================================================

    D3D11VideoDecoder* left_decoder_ = nullptr;
    D3D11VideoDecoder* right_decoder_ = nullptr;

    //=========================================================================
    // State
    //=========================================================================

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    DualViewLayout layout_;

    std::atomic<bool> waiting_for_both_frames_{false};
    std::atomic<bool> playing_{false};
    bool initialized_ = false;
    bool layout_valid_ = false;
};

} // namespace ump

#endif // _WIN32
