#pragma once

#ifdef _WIN32

#include <string>
#include <memory>
#include <glad/gl.h>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ump {

// Forward declarations
class StreamingVideoDecoder;
class D3D11HWFrameExtractor;
class D3D11YUVRenderer;
class D3D11VideoInterop;

//=============================================================================
// HDRDirectVideoDecoder
//
// Zero-copy HDR video decoder that combines:
// - FFmpeg D3D11VA hardware decode (NV12/P010 output)
// - D3D11 YUV->RGB shader with PQ decode
// - D3D11->GL interop for OpenGL pipeline integration
//
// This provides a decoded, color-converted video frame as an OpenGL texture
// without any CPU-GPU data transfers.
//
// Usage:
//   decoder.Initialize(device, "video.mp4");
//   GLuint tex = decoder.GetFrameAsGLTexture(frame_number);
//   // Use tex in OpenGL pipeline (OCIO, ImGui, etc.)
//=============================================================================

class HDRDirectVideoDecoder {
public:
    HDRDirectVideoDecoder();
    ~HDRDirectVideoDecoder();

    // Delete copy/move
    HDRDirectVideoDecoder(const HDRDirectVideoDecoder&) = delete;
    HDRDirectVideoDecoder& operator=(const HDRDirectVideoDecoder&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    // Initialize with D3D11 device and video file
    // The device should be the same one used for FFmpeg D3D11VA decode
    bool Initialize(ID3D11Device* device, const std::string& video_path);

    // Shutdown and release all resources
    void Shutdown();

    // Check if initialized
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Frame Access (Zero-Copy)
    //=========================================================================

    // Get frame as OpenGL texture ID
    // This performs:
    // 1. D3D11VA hardware decode (if available)
    // 2. YUV->RGB conversion with PQ decode via D3D11 shader
    // 3. D3D11->GL interop to expose as OpenGL texture
    // Returns 0 on failure
    GLuint GetFrameAsGLTexture(int frame_number);

    // Update playhead position (for streaming decoder)
    void UpdatePlayhead(int frame_number);

    //=========================================================================
    // Decode Mode Queries
    //=========================================================================

    // Check if hardware decode is active
    bool IsHardwareDecodeActive() const;

    // Check if zero-copy interop is active
    bool IsZeroCopyActive() const;

    // Check if content is HDR (P010 format detected)
    bool IsHDRContent() const { return is_hdr_content_; }

    // Get decode mode description for UI
    std::string GetDecodeModeDescription() const;

    //=========================================================================
    // Video Metadata
    //=========================================================================

    int GetWidth() const;
    int GetHeight() const;
    double GetFPS() const;
    double GetDuration() const;
    int GetFrameCount() const;

    //=========================================================================
    // Internal Decoder Access (for advanced use)
    //=========================================================================

    StreamingVideoDecoder* GetStreamingDecoder() const { return decoder_.get(); }

private:
    //=========================================================================
    // Frame Processing
    //=========================================================================

    // Process frame through the GPU pipeline
    // Returns true if frame was successfully processed
    bool ProcessFrame(int frame_number);

    // Ensure interop texture is created with correct dimensions
    bool EnsureInteropTexture(int width, int height);

    //=========================================================================
    // State
    //=========================================================================

    std::unique_ptr<StreamingVideoDecoder> decoder_;
    std::unique_ptr<D3D11HWFrameExtractor> extractor_;
    std::unique_ptr<D3D11YUVRenderer> yuv_renderer_;
    std::unique_ptr<D3D11VideoInterop> interop_;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

    std::string video_path_;
    int current_frame_ = -1;
    bool is_hdr_content_ = false;
    bool initialized_ = false;
};

} // namespace ump

#endif // _WIN32
