#pragma once

#include <memory>
#include <string>
#include <map>
#include <mutex>
#include "image_loader_interface.h"

namespace ump {

class StreamingVideoDecoder;

//=============================================================================
// ScrubDecoder - Lightweight decoder for responsive scrubbing
//
// Key differences from ManagedVideoDecoder:
// - Small buffer (15 frames vs 168 frames)
// - No spawn-and-abandon (reuses single decoder, seeks instead)
// - Keyframe-only seeks (SeekQuality::PREVIEW)
// - No window awareness (completely detached from CacheWindowEngine)
// - Purpose: Fast scrub preview, not playback caching
//=============================================================================

class ScrubDecoder {
public:
    explicit ScrubDecoder(const std::string& video_path);
    ~ScrubDecoder();

    // Initialize decoder with lightweight scrub-optimized config
    bool Initialize();
    void Shutdown();

    // Primary scrub access - returns closest available frame (fast)
    // This is the main method used during scrubbing
    std::shared_ptr<PixelData> GetClosestFrame(int frame_number, int* actual_frame = nullptr);

    // Try to get exact frame (may return nullptr if not buffered)
    std::shared_ptr<PixelData> GetFrame(int frame_number);

    // Seek to target frame (keyframe-only for speed)
    void SeekTo(int frame_number);

    // Check if frame is in buffer
    bool HasFrame(int frame_number) const;

    // Metadata
    int GetWidth() const;
    int GetHeight() const;
    bool IsInitialized() const { return initialized_; }
    const std::string& GetPath() const { return video_path_; }

private:
    std::unique_ptr<StreamingVideoDecoder> decoder_;
    std::string video_path_;
    bool initialized_ = false;
};

//=============================================================================
// ScrubDecoderManager - Manages scrub decoders per source file
//
// Lazy creation: decoders created on first access per source
// Cleared when scrub mode ends to free memory
//=============================================================================

class ScrubDecoderManager {
public:
    ScrubDecoderManager() = default;
    ~ScrubDecoderManager();

    // Get or create scrub decoder for a source path
    // Returns nullptr if creation fails
    ScrubDecoder* GetDecoder(const std::string& source_path);

    // Clear all decoders (call when scrub mode ends)
    void ClearAll();

    // Clear decoder for a specific source
    void Clear(const std::string& source_path);

    // Check if any decoders are active
    bool HasDecoders() const;

private:
    std::map<std::string, std::unique_ptr<ScrubDecoder>> decoders_;
    mutable std::mutex mutex_;
};

} // namespace ump
