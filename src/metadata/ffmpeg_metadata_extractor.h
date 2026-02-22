#pragma once

#include "video_metadata.h"
#include <string>

// Forward declarations for FFmpeg types
struct AVFormatContext;
struct AVCodecParameters;
struct AVStream;

namespace qcview {

/**
 * FFmpeg-based metadata extractor
 *
 * PURPOSE: Extract ALL metadata BEFORE playback begins
 * - One-pass extraction of all video/audio/color properties
 * - No frame decoding required (container + stream info only)
 * - Fast: ~10-50ms for most files
 *
 * USAGE:
 *   auto metadata = FFmpegMetadataExtractor::Extract(file_path);
 *   if (metadata.is_loaded) {
 *       // Use metadata to configure cache, conversion strategy, etc.
 *       auto strategy = ConversionStrategy::FromMetadata(metadata);
 *   }
 */
class FFmpegMetadataExtractor {
public:
    /**
     * Extract complete metadata from a media file using FFmpeg
     *
     * @param file_path Path to media file
     * @return VideoMetadata struct with all properties populated
     *
     * PERFORMANCE: Fast, no frame decoding required
     * - Opens container (avformat_open_input)
     * - Reads stream info (avformat_find_stream_info)
     * - Extracts codec parameters (AVCodecParameters)
     * - Reads color properties from stream metadata
     * - Closes cleanly
     *
     * WHAT IT EXTRACTS:
     * - File info: name, path, size, duration
     * - Video: codec, dimensions, pixel_format, frame_rate, total_frames
     * - Color: colorspace, primaries, transfer, range
     * - Audio: codec, sample_rate, channels
     * - Bit depth, alpha channel detection (from pixel_format)
     * - 420/422/4444 format detection
     */
    static VideoMetadata Extract(const std::string& file_path);

    /**
     * Fast duration probe (replaces VideoPlayer::ProbeFileDuration)
     *
     * @param file_path Path to media file
     * @return Duration in seconds, or 0.0 on error
     *
     * PERFORMANCE: Very fast (~5-20ms)
     * - Only opens container, doesn't initialize codec
     * - Very fast, only opens container without initializing codec
     */
    static double ProbeDuration(const std::string& file_path);

private:
    // Internal helpers
    static bool OpenFile(const std::string& file_path, AVFormatContext** format_ctx);
    static void ExtractVideoStream(AVFormatContext* format_ctx, VideoMetadata& metadata);
    static void ExtractAudioStream(AVFormatContext* format_ctx, VideoMetadata& metadata);
    static void ExtractColorProperties(const AVStream* stream, VideoMetadata& metadata);
    static void ExtractFileInfo(const std::string& file_path, AVFormatContext* format_ctx, VideoMetadata& metadata);

    // Pixel format parsing
    static std::string AVPixelFormatToString(int av_pix_fmt);
    static std::string AVColorSpaceToString(int av_color_space);
    static std::string AVColorPrimariesToString(int av_color_primaries);
    static std::string AVColorTransferToString(int av_color_trc);
    static std::string AVColorRangeToString(int av_color_range);
};

} // namespace qcview
