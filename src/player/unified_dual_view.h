#pragma once

#ifdef _WIN32

#include <memory>
#include <atomic>
#include <mutex>
#include <string>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <glad/gl.h>

extern "C" {
#include <libavutil/frame.h>
}

namespace ump {

// Forward declarations
class D3D11VideoDecoder;
class FrameReadback;
class CPUFramePool;

//=============================================================================
// UnifiedDualView
//
// Single-decoder architecture for dual view that avoids D3D11 multi-decoder
// threading issues. Uses one HW decoder, frames read back
// to CPU for secondary view, then uploaded to GL.
//
// Architecture:
//   Single D3D11VideoDecoder (time-multiplexed between sources A and B)
//           |
//           v
//   +------------------+
//   | Source A frame   |---> D3D11->GL interop (primary, zero-copy)
//   +------------------+
//   | Source B frame   |---> Readback to CPU ---> GL upload (secondary)
//   +------------------+
//
// Benefits:
// - Single D3D11 device/context - no threading issues
// - HW acceleration for both streams
// - Predictable decode scheduling
//
// Trade-offs:
// - Sequential decode (can't parallel decode A and B)
// - Readback latency for secondary view
// - May struggle with 2x 4K60 real-time (but fine for scrubbing/comparison)
//=============================================================================

class UnifiedDualView {
public:
    UnifiedDualView();
    ~UnifiedDualView();

    // Non-copyable
    UnifiedDualView(const UnifiedDualView&) = delete;
    UnifiedDualView& operator=(const UnifiedDualView&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    // Initialize with D3D11 device
    bool Initialize(ID3D11Device* device);
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Source Management
    //=========================================================================

    // Set source paths for A and B views
    // Returns true if both sources opened successfully
    bool SetSources(const std::string& path_a, const std::string& path_b);

    // Close sources and release decoder
    void CloseSources();

    // Check if sources are loaded
    bool HasSources() const { return sources_loaded_; }

    //=========================================================================
    // Frame Access
    //=========================================================================

    // Update to target frame for both views
    // Call this each frame before GetTextures
    // Returns true if both frames are ready
    bool UpdateFrame(int64_t frame_a, int64_t frame_b);

    // Get GL textures for rendering
    // Returns 0 if frame not ready
    GLuint GetTextureA() const { return gl_texture_a_; }
    GLuint GetTextureB() const { return gl_texture_b_; }

    // Get dimensions
    void GetDimensionsA(int& width, int& height) const;
    void GetDimensionsB(int& width, int& height) const;

    //=========================================================================
    // Seek / Scrub
    //=========================================================================

    // Notify of seek (invalidates cached frames)
    void OnSeek();

    // Set which view is "primary" (gets priority decode and zero-copy path)
    // 0 = A is primary, 1 = B is primary
    void SetPrimaryView(int view);
    int GetPrimaryView() const { return primary_view_; }

    //=========================================================================
    // Playback Control
    //=========================================================================

    // Set playback state (affects decode strategy)
    void SetPlaying(bool playing);
    bool IsPlaying() const { return playing_; }

    //=========================================================================
    // Statistics
    //=========================================================================

    struct Stats {
        int frames_decoded_a;
        int frames_decoded_b;
        int readback_count;
        double last_decode_time_ms;
    };

    Stats GetStats() const;

private:
    //=========================================================================
    // Internal Types
    //=========================================================================

    enum class SourceState {
        IDLE,
        LOADING,
        READY,
        FAILED
    };

    struct SourceInfo {
        std::string path;
        int width = 0;
        int height = 0;
        double fps = 0.0;
        int frame_count = 0;
        SourceState state = SourceState::IDLE;
    };

    //=========================================================================
    // Internal Methods
    //=========================================================================

    // Decode a frame from the specified source
    bool DecodeFrame(int source_id, int64_t frame_number);

    // Switch decoder to different source file
    bool SwitchSource(int source_id);

    // Read back current frame from GPU to CPU pool
    bool ReadbackCurrentFrame(int source_id, int frame_number);

    // Upload CPU frame to GL texture
    bool UploadFrameToGL(AVFrame* frame, GLuint& texture, int width, int height);

    // Ensure GL textures are created
    bool EnsureGLTextures();

    //=========================================================================
    // State
    //=========================================================================

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    bool initialized_ = false;
    bool sources_loaded_ = false;

    // Single decoder (switches between sources)
    std::unique_ptr<D3D11VideoDecoder> decoder_;
    int current_decoder_source_ = -1;  // Which source the decoder is currently configured for

    // Readback and CPU frame pool
    std::unique_ptr<FrameReadback> readback_;
    std::unique_ptr<CPUFramePool> frame_pool_;

    // Source info
    SourceInfo source_a_;
    SourceInfo source_b_;

    // Output GL textures
    GLuint gl_texture_a_ = 0;
    GLuint gl_texture_b_ = 0;
    int gl_texture_a_width_ = 0;
    int gl_texture_a_height_ = 0;
    int gl_texture_b_width_ = 0;
    int gl_texture_b_height_ = 0;

    // Current frame tracking
    int64_t current_frame_a_ = -1;
    int64_t current_frame_b_ = -1;

    // Control
    int primary_view_ = 0;  // 0 = A, 1 = B
    std::atomic<bool> playing_{false};

    // Thread safety
    mutable std::mutex mutex_;

    // Statistics
    mutable std::atomic<int> frames_decoded_a_{0};
    mutable std::atomic<int> frames_decoded_b_{0};
    mutable std::atomic<int> readback_count_{0};
    mutable std::atomic<double> last_decode_time_ms_{0.0};
};

} // namespace ump

#endif // _WIN32
