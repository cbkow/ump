#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <future>
#include <half.h>

#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfCompression.h>

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

namespace ump {

//=============================================================================
// EXR Transcode Configuration
//=============================================================================

struct EXRTranscodeConfig {
    std::string cache_path = "";  // Empty = use default %LOCALAPPDATA%
    int max_width = 0;             // 0 = native resolution
    Imf::Compression compression = Imf::B44A_COMPRESSION;  // B44A default (lossy, 32:1 ratio, fast)

    // Parallel transcoding (similar to DirectEXRCache threadCount)
    size_t threadCount = 8;        // Conservative default (write-heavy operation)

    // Future settings
    bool auto_transcode = false;   // Auto-suggest for multilayer
    int default_max_width = 0;
    Imf::Compression default_compression = Imf::B44A_COMPRESSION;

    bool IsValid() const {
        return max_width >= 0 &&
               threadCount >= 1 && threadCount <= 16;
    }
};

//=============================================================================
// EXRTranscoder - Single-layer EXR cache generator
//=============================================================================
// Converts multilayer EXRs to single-layer + optional resize for playback
// Similar pattern to DummyVideoGenerator

class EXRTranscoder {
public:
    EXRTranscoder();
    ~EXRTranscoder();

    // Initialize transcoder (setup cache directory)
    void Initialize();

    // Check if transcode already exists
    bool HasTranscodedSequence(const std::vector<std::string>& source_files,
                               const std::string& layer,
                               int max_width,
                               Imf::Compression compression) const;

    // Get transcode directory path
    std::string GetTranscodePath(const std::string& source_first_file,
                                 const std::string& layer,
                                 int max_width,
                                 Imf::Compression compression) const;

    // Get transcoded file list (if exists)
    std::vector<std::string> GetTranscodedFiles(const std::vector<std::string>& source_files,
                                                 const std::string& layer,
                                                 int max_width,
                                                 Imf::Compression compression) const;

    // Async transcode with progress
    void TranscodeSequenceAsync(
        const std::vector<std::string>& source_files,
        const std::string& layer,
        const EXRTranscodeConfig& config,
        std::function<void(int current, int total, const std::string& message)> progress_callback,
        std::function<void(bool success, const std::string& error_message)> completion_callback
    );

    // Cancel ongoing transcode
    void CancelTranscode();

    // Check if transcode is running
    bool IsTranscoding() const { return is_transcoding_.load(); }

    // Get/set cache directory
    void SetCacheDirectory(const std::string& path);
    std::string GetCacheDirectory() const { return cache_dir_; }

    // Set cache configuration (retention days, size limit, custom path, clear on exit)
    void SetCacheConfig(const std::string& custom_path, int retention_days, int max_gb, bool clear_on_exit = false);

    // Clear all transcodes from both default and custom cache locations
    // Returns total bytes deleted
    size_t ClearAllTranscodes();

private:
    // Setup default cache directory (%LOCALAPPDATA%/ump/EXRtranscodes/)
    void SetupDefaultCacheDirectory();

    // Transcode worker thread
    void TranscodeWorker(
        std::vector<std::string> source_files,
        std::string layer,
        EXRTranscodeConfig config,
        std::function<void(int, int, const std::string&)> progress_callback,
        std::function<void(bool, const std::string&)> completion_callback
    );

    // Transcode single frame (EXR → EXR)
    bool TranscodeFrame(const std::string& source_path,
                       const std::string& dest_path,
                       const std::string& layer,
                       int target_width,
                       int target_height,
                       Imf::Compression compression,
                       std::string& error_message);

    // Transcode single frame (TIFF/PNG → EXR)
    bool TranscodeImageToEXR(const std::string& source_path,
                            const std::string& dest_path,
                            int target_width,
                            int target_height,
                            Imf::Compression compression,
                            std::string& error_message);

    // Resize pixels using swscale (Lanczos high-quality)
    bool ResizePixels(const std::vector<half>& src_pixels,
                     int src_width, int src_height,
                     std::vector<half>& dst_pixels,
                     int dst_width, int dst_height);

    // Generate cache key/directory name
    std::string GenerateCacheKey(const std::string& base_name,
                                 const std::string& layer,
                                 int max_width,
                                 Imf::Compression compression) const;

    // Compression type to string
    const char* CompressionToString(Imf::Compression comp) const;

    // State
    std::string cache_dir_;
    bool initialized_ = false;

    // Cache configuration
    int cache_retention_days_ = 7;
    int cache_max_gb_ = 10;
    std::string custom_cache_path_ = "";
    bool clear_cache_on_exit_ = false;

    // Transcode state
    std::atomic<bool> is_transcoding_{false};
    std::atomic<bool> cancel_requested_{false};
    std::thread transcode_thread_;
    std::mutex transcode_mutex_;

    // Progress tracking for parallel transcoding
    std::atomic<int> completed_count_{0};
    std::atomic<int> failed_count_{0};
};

} // namespace ump
