#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include "pipeline_mode.h"
#include "image_loader_interface.h"

// Forward declarations for FFmpeg types (in global namespace)
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;

namespace ump {

// Image format detection
enum class ImageFormat {
    UNKNOWN,
    TIFF,
    PNG,
    JPEG
};

struct ImageInfo {
    int width = 0;
    int height = 0;
    int channels = 0;
    int bit_depth = 0;  // 8, 16, 32
    bool is_float = false;
    PipelineMode recommended_pipeline = PipelineMode::NORMAL;
};

// Format detection utilities
ImageFormat DetectImageFormat(const std::string& path);
bool GetImageInfo(const std::string& path, ImageInfo& info);

// TIFF Loader (using libtiff)
namespace TIFFLoader {
    bool Load(const std::string& path, std::vector<uint8_t>& pixel_data,
              int& width, int& height, PipelineMode& mode);
    bool GetInfo(const std::string& path, ImageInfo& info);
}

// PNG Loader (using libpng)
namespace PNGLoader {
    bool Load(const std::string& path, std::vector<uint8_t>& pixel_data,
              int& width, int& height, PipelineMode& mode);
    bool GetInfo(const std::string& path, ImageInfo& info);
}

// JPEG Loader (using libjpeg-turbo)
namespace JPEGLoader {
    bool Load(const std::string& path, std::vector<uint8_t>& pixel_data,
              int& width, int& height, PipelineMode& mode);
    bool GetInfo(const std::string& path, ImageInfo& info);
}

//=============================================================================
// IImageLoader Implementations (for Universal Cache)
//=============================================================================

// EXR Image Loader (wraps existing DirectEXRCache EXR loading logic)
class EXRImageLoader : public IImageLoader {
public:
    // Set layer name for multi-layer EXR support
    // Must be called before LoadFrame/LoadThumbnail for non-default layers
    void SetLayer(const std::string& layer) { layer_name_ = layer; }
    const std::string& GetLayer() const { return layer_name_; }

    std::shared_ptr<PixelData> LoadFrame(
        const std::string& path,
        const std::string& layer,
        PipelineMode pipeline_mode
    ) override;

    // EXR thumbnail loading - optimized scanline-based loading, keeps HDR data
    std::shared_ptr<PixelData> LoadThumbnail(
        const std::string& path,
        int max_size = 320
    ) override;

    bool GetDimensions(const std::string& path, int& width, int& height) override;
    std::string GetLoaderName() const override { return "EXR"; }

private:
    std::string layer_name_;  // Layer name for multi-layer EXR (empty = default layer)
};

// TIFF Image Loader (wraps TIFFLoader namespace logic)
class TIFFImageLoader : public IImageLoader {
public:
    std::shared_ptr<PixelData> LoadFrame(
        const std::string& path,
        const std::string& layer,        // Ignored for TIFF
        PipelineMode pipeline_mode
    ) override;

    // Fast thumbnail loading - reads every Nth scanline for speed
    std::shared_ptr<PixelData> LoadThumbnail(
        const std::string& path,
        int max_size = 320
    ) override;

    bool GetDimensions(const std::string& path, int& width, int& height) override;
    std::string GetLoaderName() const override { return "TIFF"; }
};

// PNG Image Loader (wraps PNGLoader namespace logic)
class PNGImageLoader : public IImageLoader {
public:
    std::shared_ptr<PixelData> LoadFrame(
        const std::string& path,
        const std::string& layer,        // Ignored for PNG
        PipelineMode pipeline_mode
    ) override;

    // Fast thumbnail loading - minimal transformations
    std::shared_ptr<PixelData> LoadThumbnail(
        const std::string& path,
        int max_size = 320
    ) override;

    bool GetDimensions(const std::string& path, int& width, int& height) override;
    std::string GetLoaderName() const override { return "PNG"; }
};

// JPEG Image Loader (wraps JPEGLoader namespace logic)
class JPEGImageLoader : public IImageLoader {
public:
    std::shared_ptr<PixelData> LoadFrame(
        const std::string& path,
        const std::string& layer,        // Ignored for JPEG
        PipelineMode pipeline_mode
    ) override;

    // Optimized thumbnail loading using libjpeg DCT scaling (1/2, 1/4, 1/8)
    std::shared_ptr<PixelData> LoadThumbnail(
        const std::string& path,
        int max_size = 320
    ) override;

    bool GetDimensions(const std::string& path, int& width, int& height) override;
    std::string GetLoaderName() const override { return "JPEG"; }
};

// Video Image Loader (extracts frames from video files using FFmpeg)
// Usage: Create with video path, then pass frame numbers as "0", "1", "2", etc. to LoadFrame
class VideoImageLoader : public IImageLoader {
public:
    // Initialize with video file path and metadata
    VideoImageLoader(const std::string& video_path, double fps, double duration);
    ~VideoImageLoader();

    // Prevent copying (FFmpeg contexts are not copyable)
    VideoImageLoader(const VideoImageLoader&) = delete;
    VideoImageLoader& operator=(const VideoImageLoader&) = delete;

    std::shared_ptr<PixelData> LoadFrame(
        const std::string& path,         // Frame number as string ("0", "1", "2", etc.)
        const std::string& layer,        // Ignored for video
        PipelineMode pipeline_mode
    ) override;

    // Optimized thumbnail loading - same as LoadFrame but at reduced resolution
    std::shared_ptr<PixelData> LoadThumbnail(
        const std::string& path,         // Frame number as string
        int max_size = 320
    ) override;

    bool GetDimensions(const std::string& path, int& width, int& height) override;
    std::string GetLoaderName() const override { return "Video"; }

    // Get video metadata
    double GetFrameRate() const { return fps_; }
    double GetDuration() const { return duration_; }
    int GetFrameCount() const { return static_cast<int>(duration_ * fps_); }

private:
    // Internal frame extraction with optional scaling
    std::shared_ptr<PixelData> ExtractFrame(int frame_number, PipelineMode pipeline_mode, int max_size = 0);

    // Initialize FFmpeg context (called in constructor)
    bool InitializeFFmpeg();

    // Clean up FFmpeg resources
    void CleanupFFmpeg();

    // Seek to and decode specific frame
    bool SeekAndDecodeFrame(double timestamp, ::AVFrame* output_frame);

    // Convert AVFrame to pixel buffer
    bool ConvertFrameToPixels(::AVFrame* frame, std::vector<uint8_t>& pixels,
                              int& width, int& height, PipelineMode pipeline_mode, int max_size);

    // Video metadata
    std::string video_path_;
    double fps_;
    double duration_;
    int width_;
    int height_;

    // FFmpeg context (using global namespace FFmpeg types)
    ::AVFormatContext* format_context_ = nullptr;
    ::AVCodecContext* codec_context_ = nullptr;
    int video_stream_index_ = -1;

    // Thread safety
    std::mutex ffmpeg_mutex_;
    bool initialized_ = false;
};

} // namespace ump
