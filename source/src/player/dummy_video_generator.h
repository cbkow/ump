#pragma once

#include <string>
#include <map>
#include <filesystem>

// FFmpeg C API headers
extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/avutil.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/opt.h>
    #include <libswscale/swscale.h>
    #include <libavfilter/avfilter.h>
    #include <libavfilter/buffersink.h>
    #include <libavfilter/buffersrc.h>
}

namespace ump {

class DummyVideoGenerator {
public:
    DummyVideoGenerator();
    ~DummyVideoGenerator();

    // Get or create cached dummy video matching EXR specs (1 second duration)
    std::string GetDummyFor(int width, int height, double fps);

    // Get or create cached dummy video with specific duration (for EXR sequences)
    std::string GetDummyFor(int width, int height, double fps, double duration);

    // Cleanup temporary files
    void CleanupCache();

    // Clear all dummies from both default and custom cache locations
    // Returns total bytes deleted
    size_t ClearAllDummies();

    // Set custom temporary directory
    void SetTempDirectory(const std::string& temp_dir);

    // Check if a specific dummy exists in cache
    bool HasCachedDummy(int width, int height, double fps);

    // Set cache configuration (retention days, size limit, custom path, clear on exit)
    void SetCacheConfig(const std::string& custom_path, int retention_days, int max_gb, bool clear_on_exit = false);

private:
    // Create fast dummy video using FFmpeg (1 second duration)
    std::string CreateFastDummy(int width, int height, double fps);

    // Create fast dummy video with specific duration
    std::string CreateFastDummy(int width, int height, double fps, double duration);

    // Create dummy video using FFmpeg API directly (1 second duration)
    bool CreateDummyVideoWithAPI(const std::string& output_path, int width, int height, double fps);

    // Create dummy video using FFmpeg API directly with specific duration
    bool CreateDummyVideoWithAPI(const std::string& output_path, int width, int height, double fps, double duration);

    // Generate cache key for dimensions/fps combination
    std::string GenerateCacheKey(int width, int height, double fps);

    // Check if file exists and is valid
    bool FileExists(const std::string& path);

    // Setup %localappdata%\ump\dummies\ directory
    void SetupLocalAppDataDirectory();

    std::map<std::string, std::string> cache; // "1920x1080_24" -> full_path
    std::string temp_dir;
    bool initialized;

    // Cache configuration
    int cache_retention_days_ = 7;
    int cache_max_gb_ = 1;
    std::string custom_cache_path_ = "";
    bool clear_cache_on_exit_ = false;

    // Initialize paths and directories
    void Initialize();
};

} // namespace ump