#include "playlist_single_decoder.h"

#ifdef _WIN32

#include "../player/d3d11_video_decoder.h"
#include "../utils/debug_utils.h"

namespace ump {

PlaylistSingleDecoder::PlaylistSingleDecoder() = default;

PlaylistSingleDecoder::~PlaylistSingleDecoder() {
    Shutdown();
}

//=============================================================================
// Lifecycle
//=============================================================================

bool PlaylistSingleDecoder::Initialize(ID3D11Device* device) {
    if (initialized_) {
        Debug::Log("[PlaylistSingleDecoder] Already initialized");
        return true;
    }

    if (!device) {
        Debug::Log("[PlaylistSingleDecoder] Error: device is null");
        return false;
    }

    device_ = device;
    initialized_ = true;

    Debug::Log("[PlaylistSingleDecoder] Initialized (no decoder loaded yet)");
    return true;
}

void PlaylistSingleDecoder::Shutdown() {
    if (!initialized_) {
        return;
    }

    Debug::Log("[PlaylistSingleDecoder] Shutting down");

    // Lock and shutdown decoder
    {
        std::lock_guard<std::mutex> lock(switch_mutex_);

        if (decoder_) {
            decoder_->Shutdown();
            decoder_.reset();
        }

        current_source_.clear();
    }

    device_ = nullptr;
    initialized_ = false;
}

//=============================================================================
// Source Management
//=============================================================================

bool PlaylistSingleDecoder::SwitchSource(const std::string& source_path) {
    if (!initialized_) {
        Debug::Log("[PlaylistSingleDecoder] Cannot switch source - not initialized");
        return false;
    }

    if (source_path.empty()) {
        Debug::Log("[PlaylistSingleDecoder] Cannot switch to empty source path");
        return false;
    }

    // Fast path: already on this source with valid decoder
    {
        std::lock_guard<std::mutex> lock(switch_mutex_);

        if (source_path == current_source_ && decoder_ && decoder_->IsInitialized()) {
            return true;  // Already on this source
        }
    }

    // Mark as switching
    switching_.store(true);

    Debug::Log("[PlaylistSingleDecoder] Switching source to: " + source_path);

    std::lock_guard<std::mutex> lock(switch_mutex_);

    // Shutdown existing decoder cleanly
    if (decoder_) {
        Debug::Log("[PlaylistSingleDecoder] Shutting down previous decoder for: " + current_source_);
        decoder_->Shutdown();
        decoder_.reset();
    }

    // Create fresh decoder for new source
    decoder_ = std::make_unique<D3D11VideoDecoder>();
    decoder_->SetVideoPath(source_path);

    // Configure for single-decoder mode (smaller buffer, no readahead)
    StreamingDecoderConfig config;
    config.readAheadFrames = 16;   // Minimal readahead - we're not pre-warming
    config.readBehindFrames = 4;   // Minimal back buffer
    decoder_->SetConfig(config);

    if (!decoder_->Initialize()) {
        Debug::Log("[PlaylistSingleDecoder] Failed to initialize decoder for: " + source_path);
        decoder_.reset();
        switching_.store(false);
        return false;
    }

    current_source_ = source_path;
    switching_.store(false);

    Debug::Log("[PlaylistSingleDecoder] Successfully switched to: " + source_path +
               " (" + std::to_string(decoder_->GetWidth()) + "x" + std::to_string(decoder_->GetHeight()) +
               " @ " + std::to_string(decoder_->GetFPS()) + "fps, " +
               std::to_string(decoder_->GetFrameCount()) + " frames)");

    return true;
}

//=============================================================================
// Frame Access
//=============================================================================

void PlaylistSingleDecoder::UpdatePlayhead(int source_frame) {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (!decoder_ || !decoder_->IsInitialized()) {
        return;
    }

    decoder_->UpdatePlayhead(source_frame);
}

bool PlaylistSingleDecoder::HasFrame(int source_frame) const {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (!decoder_ || !decoder_->IsInitialized()) {
        return false;
    }

    return decoder_->HasFrame(source_frame);
}

GLuint PlaylistSingleDecoder::GetFrameAsGLTexture(int source_frame) {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (!decoder_ || !decoder_->IsInitialized()) {
        return 0;
    }

    return decoder_->GetFrameAsGLTexture(source_frame);
}

ID3D11ShaderResourceView* PlaylistSingleDecoder::GetFrameAsD3D11SRV(int source_frame) {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (!decoder_ || !decoder_->IsInitialized()) {
        return nullptr;
    }

    return decoder_->GetFrameAsD3D11SRV(source_frame);
}

//=============================================================================
// Metadata
//=============================================================================

int PlaylistSingleDecoder::GetWidth() const {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (!decoder_ || !decoder_->IsInitialized()) {
        return 0;
    }

    return decoder_->GetWidth();
}

int PlaylistSingleDecoder::GetHeight() const {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (!decoder_ || !decoder_->IsInitialized()) {
        return 0;
    }

    return decoder_->GetHeight();
}

double PlaylistSingleDecoder::GetFPS() const {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (!decoder_ || !decoder_->IsInitialized()) {
        return 0.0;
    }

    return decoder_->GetFPS();
}

int PlaylistSingleDecoder::GetFrameCount() const {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (!decoder_ || !decoder_->IsInitialized()) {
        return 0;
    }

    return decoder_->GetFrameCount();
}

bool PlaylistSingleDecoder::IsHDR() const {
    std::lock_guard<std::mutex> lock(switch_mutex_);

    if (!decoder_ || !decoder_->IsInitialized()) {
        return false;
    }

    return decoder_->IsHDRContent();
}

} // namespace ump

#endif // _WIN32
