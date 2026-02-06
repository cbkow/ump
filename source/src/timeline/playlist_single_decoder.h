#pragma once

#ifdef _WIN32

#include <string>
#include <memory>
#include <mutex>
#include <d3d11_1.h>
#include <glad/gl.h>

namespace ump {

// Forward declarations
class D3D11VideoDecoder;

//=============================================================================
// PlaylistSingleDecoder
//
// mpv EDL-style single decoder for playlist playback.
//
// Key design principles:
// - **Single decoder instance** for the entire playlist
// - **No prefetch/pre-warm** of upcoming segments
// - **Clean flush + seek** when switching clips
// - **Brief pause at transitions** (acceptable tradeoff for stability)
//
// This approach eliminates D3D11 contention issues that cause crashes
// when multiple decoders are initialized/used simultaneously.
//
// Flow on clip transition:
//   1. Pause playback
//   2. decoder_.Shutdown()
//   3. decoder_.SetVideoPath(new_clip)
//   4. decoder_.Initialize()
//   5. decoder_.Seek(clip_start_frame)
//   6. Resume playback
//=============================================================================

class PlaylistSingleDecoder {
public:
    PlaylistSingleDecoder();
    ~PlaylistSingleDecoder();

    // Non-copyable
    PlaylistSingleDecoder(const PlaylistSingleDecoder&) = delete;
    PlaylistSingleDecoder& operator=(const PlaylistSingleDecoder&) = delete;

    //=========================================================================
    // Lifecycle
    //=========================================================================

    // Initialize with D3D11 device
    // Must be called before any other methods
    bool Initialize(ID3D11Device* device);

    // Shutdown and release all resources
    void Shutdown();

    // Check if initialized
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Source Management
    //=========================================================================

    // Switch to a different source file (flush + reinit)
    // Returns false if switch failed (caller should show last good frame or black)
    // If source_path matches current source and decoder is valid, returns immediately
    bool SwitchSource(const std::string& source_path);

    // Get current source path
    const std::string& GetCurrentSource() const { return current_source_; }

    // Check if a source switch is currently in progress
    bool IsSwitching() const { return switching_.load(); }

    //=========================================================================
    // Frame Access - Delegates to internal decoder
    //=========================================================================

    // Update playhead position (triggers decode)
    void UpdatePlayhead(int source_frame);

    // Check if frame is available
    bool HasFrame(int source_frame) const;

    // Get frame as OpenGL texture
    // Returns 0 if frame not available
    GLuint GetFrameAsGLTexture(int source_frame);

    // Get frame as D3D11 SRV
    // Returns nullptr if frame not available
    ID3D11ShaderResourceView* GetFrameAsD3D11SRV(int source_frame);

    //=========================================================================
    // Metadata - Delegates to internal decoder
    //=========================================================================

    int GetWidth() const;
    int GetHeight() const;
    double GetFPS() const;
    int GetFrameCount() const;
    bool IsHDR() const;

    //=========================================================================
    // Internal Decoder Access (for advanced use)
    //=========================================================================

    D3D11VideoDecoder* GetDecoder() const { return decoder_.get(); }

private:
    //=========================================================================
    // State
    //=========================================================================

    bool initialized_ = false;
    std::string current_source_;
    std::unique_ptr<D3D11VideoDecoder> decoder_;

    // D3D11 device reference (not owned)
    ID3D11Device* device_ = nullptr;

    // Thread safety for source switching
    mutable std::mutex switch_mutex_;
    std::atomic<bool> switching_{false};
};

} // namespace ump

#endif // _WIN32
