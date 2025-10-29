#include "transcode_manager.h"
#include "difference_sequence_transcoder.h"
#include "../metadata/video_metadata.h"
#include "../utils/debug_utils.h"

namespace ump {

// ============================================================================
// Constructor / Destructor
// ============================================================================

TranscodeManager::TranscodeManager() {
    Debug::Log("TranscodeManager: Constructor");
}

TranscodeManager::~TranscodeManager() {
    Debug::Log("TranscodeManager: Destructor");
    CancelTranscode();
}

// ============================================================================
// Transcode Control
// ============================================================================

bool TranscodeManager::StartTranscode(
    const std::string& primary_video_path,
    const VideoMetadata& primary_metadata,
    const std::string& comparison_video_path,
    const VideoMetadata& comparison_metadata,
    float amplification,
    const std::string& custom_cache_path)
{
    Debug::Log("TranscodeManager: Starting transcode");
    Debug::Log("  Primary: " + primary_video_path);
    Debug::Log("  Comparison: " + comparison_video_path);
    Debug::Log("  Amplification: " + std::to_string(amplification));
    if (!custom_cache_path.empty()) {
        Debug::Log("  Custom cache path: " + custom_cache_path);
    }

    // Check if already transcoding
    if (state_ == TranscodeState::TRANSCODING) {
        Debug::Log("ERROR: TranscodeManager: Already transcoding");
        return false;
    }

    // Create transcoder
    transcoder_ = std::make_unique<DifferenceSequenceTranscoder>();

    // Setup config
    DifferenceSequenceTranscoder::TranscodeConfig config;
    config.amplification = amplification;
    config.custom_cache_path = custom_cache_path;

    // Initialize
    if (!transcoder_->Initialize(primary_video_path, primary_metadata, comparison_video_path, comparison_metadata, config)) {
        Debug::Log("ERROR: TranscodeManager: Failed to initialize transcoder");
        state_ = TranscodeState::FAILED;
        return false;
    }

    // Check if already cached
    if (transcoder_->IsCached()) {
        Debug::Log("TranscodeManager: Already cached, skipping transcode");
        state_ = TranscodeState::COMPLETE;
        if (completion_callback_) {
            completion_callback_();
        }
        return true;
    }

    // Set callbacks
    transcoder_->SetCompletionCallback([this]() {
        Debug::Log("TranscodeManager: Transcode complete!");
        state_ = TranscodeState::COMPLETE;
        if (completion_callback_) {
            completion_callback_();
        }
    });

    // Start transcode
    if (!transcoder_->StartTranscode()) {
        Debug::Log("ERROR: TranscodeManager: Failed to start transcode");
        state_ = TranscodeState::FAILED;
        return false;
    }

    // Update state
    state_ = TranscodeState::TRANSCODING;

    Debug::Log("TranscodeManager: Transcode started successfully");
    return true;
}

void TranscodeManager::CancelTranscode() {
    Debug::Log("TranscodeManager: Cancel requested");

    if (transcoder_) {
        transcoder_->CancelTranscode();
    }

    state_ = TranscodeState::IDLE;
}

// ============================================================================
// Progress / Status
// ============================================================================

float TranscodeManager::GetProgress() const {
    if (!transcoder_) return 0.0f;
    return transcoder_->GetProgress();
}

std::string TranscodeManager::GetCombinedCacheDirectory() const {
    if (!transcoder_) return "";
    return transcoder_->GetCombinedCacheDirectory();
}

} // namespace ump
