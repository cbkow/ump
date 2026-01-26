#include "scrub_decoder.h"
#include "streaming_video_decoder.h"
#include "../utils/debug_utils.h"

namespace ump {

//=============================================================================
// ScrubDecoder Implementation
//=============================================================================

ScrubDecoder::ScrubDecoder(const std::string& video_path)
    : video_path_(video_path) {}

ScrubDecoder::~ScrubDecoder() {
    Shutdown();
}

bool ScrubDecoder::Initialize() {
    if (initialized_) return true;

    decoder_ = std::make_unique<StreamingVideoDecoder>(video_path_);

    // Configure for scrubbing: small buffer, fast seeks
    // Unlike ManagedVideoDecoder's 120+48 frame buffer, we use minimal buffering
    StreamingDecoderConfig config;
    config.readAheadFrames = 10;     // Small buffer (~0.4 sec @ 24fps)
    config.readBehindFrames = 5;     // Minimal behind buffer
    config.useHardwareAccel = true;  // Use HW accel for faster decodes
    config.decodeThreads = 2;        // Fewer threads for lightweight operation
    config.minBufferBeforePlay = 1;  // Don't wait for buffer fill
    decoder_->SetConfig(config);

    if (!decoder_->Initialize()) {
        Debug::Log("ScrubDecoder: Failed to initialize for " + video_path_);
        decoder_.reset();
        return false;
    }

    initialized_ = true;
    Debug::Log("ScrubDecoder: Initialized for " + video_path_ +
               " (buffer: " + std::to_string(config.readAheadFrames) + " ahead, " +
               std::to_string(config.readBehindFrames) + " behind)");
    return true;
}

void ScrubDecoder::Shutdown() {
    if (decoder_) {
        decoder_->Shutdown();
        decoder_.reset();
    }
    initialized_ = false;
}

std::shared_ptr<PixelData> ScrubDecoder::GetClosestFrame(int frame_number, int* actual_frame) {
    if (!initialized_ || !decoder_) return nullptr;

    // Update playhead with PREVIEW quality (keyframe-only seek for speed)
    // force_seek=false lets the decoder decide when to seek based on buffer position
    decoder_->UpdatePlayhead(frame_number, SeekQuality::PREVIEW, false);

    // Return closest available frame from buffer
    return decoder_->GetClosestFrame(frame_number, actual_frame);
}

std::shared_ptr<PixelData> ScrubDecoder::GetFrame(int frame_number) {
    if (!initialized_ || !decoder_) return nullptr;

    // Update playhead with PREVIEW quality
    decoder_->UpdatePlayhead(frame_number, SeekQuality::PREVIEW, false);

    // Try to get exact frame
    return decoder_->GetFrame(frame_number);
}

void ScrubDecoder::SeekTo(int frame_number) {
    if (!initialized_ || !decoder_) return;

    // Force seek with PREVIEW quality for fast response
    decoder_->UpdatePlayhead(frame_number, SeekQuality::PREVIEW, true);
}

bool ScrubDecoder::HasFrame(int frame_number) const {
    if (!initialized_ || !decoder_) return false;
    return decoder_->HasFrame(frame_number);
}

int ScrubDecoder::GetWidth() const {
    return decoder_ ? decoder_->GetWidth() : 0;
}

int ScrubDecoder::GetHeight() const {
    return decoder_ ? decoder_->GetHeight() : 0;
}

//=============================================================================
// ScrubDecoderManager Implementation
//=============================================================================

ScrubDecoderManager::~ScrubDecoderManager() {
    ClearAll();
}

ScrubDecoder* ScrubDecoderManager::GetDecoder(const std::string& source_path) {
    if (source_path.empty()) return nullptr;

    std::lock_guard<std::mutex> lock(mutex_);

    // Check if decoder already exists
    auto it = decoders_.find(source_path);
    if (it != decoders_.end()) {
        return it->second.get();
    }

    // Create new decoder
    auto decoder = std::make_unique<ScrubDecoder>(source_path);
    if (!decoder->Initialize()) {
        Debug::Log("ScrubDecoderManager: Failed to create decoder for " + source_path);
        return nullptr;
    }

    ScrubDecoder* raw_ptr = decoder.get();
    decoders_[source_path] = std::move(decoder);

    Debug::Log("ScrubDecoderManager: Created decoder for " + source_path +
               " (total: " + std::to_string(decoders_.size()) + ")");
    return raw_ptr;
}

void ScrubDecoderManager::ClearAll() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!decoders_.empty()) {
        Debug::Log("ScrubDecoderManager: Clearing " + std::to_string(decoders_.size()) + " decoders");
        decoders_.clear();
    }
}

void ScrubDecoderManager::Clear(const std::string& source_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = decoders_.find(source_path);
    if (it != decoders_.end()) {
        Debug::Log("ScrubDecoderManager: Clearing decoder for " + source_path);
        decoders_.erase(it);
    }
}

bool ScrubDecoderManager::HasDecoders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !decoders_.empty();
}

} // namespace ump
