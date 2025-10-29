#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <functional>

// Forward declaration (in global namespace)
struct VideoMetadata;

namespace ump {

// Forward declarations
class DifferenceSequenceTranscoder;

/**
 * TranscodeManager
 *
 * Manages difference sequence transcoding for frame-accurate comparison mode.
 * Uses DifferenceSequenceTranscoder for direct 2-producer, 1-consumer flow.
 *
 * Usage:
 *   TranscodeManager mgr;
 *   mgr.SetCompletionCallback([](){ Debug::Log("Transcoding complete!"); });
 *   mgr.StartTranscode(primary_path, primary_metadata, comparison_path, comparison_metadata);
 *   float progress = mgr.GetProgress();
 */
class TranscodeManager {
public:
    enum class TranscodeState {
        IDLE,                   // Not started
        TRANSCODING,            // Transcoding difference sequence
        COMPLETE,               // Transcode finished
        FAILED                  // Transcode failed
    };

    TranscodeManager();
    ~TranscodeManager();

    // Start transcoding difference sequence
    bool StartTranscode(
        const std::string& primary_video_path,
        const VideoMetadata& primary_metadata,
        const std::string& comparison_video_path,
        const VideoMetadata& comparison_metadata,
        float amplification = 5.0f,
        const std::string& custom_cache_path = ""
    );

    // Cancel in-progress transcode
    void CancelTranscode();

    // Status
    TranscodeState GetState() const { return state_; }
    bool IsTranscoding() const { return state_ == TranscodeState::TRANSCODING; }
    bool IsComplete() const { return state_ == TranscodeState::COMPLETE; }
    float GetProgress() const;  // 0.0-1.0

    // Cache directory (combined difference sequence)
    std::string GetCombinedCacheDirectory() const;

    // Completion callback
    void SetCompletionCallback(std::function<void()> callback) { completion_callback_ = callback; }

private:
    // Single difference transcoder
    std::unique_ptr<DifferenceSequenceTranscoder> transcoder_;

    // State
    std::atomic<TranscodeState> state_{TranscodeState::IDLE};

    // Completion callback
    std::function<void()> completion_callback_;
};

} // namespace ump
