#include "ffmpeg_metadata_extractor.h"
#include "../utils/debug_utils.h"
#include <filesystem>
#include <algorithm>

// FFmpeg includes
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/dict.h>
}

namespace fs = std::filesystem;

namespace ump {

// ============================================================================
// PUBLIC API
// ============================================================================

VideoMetadata FFmpegMetadataExtractor::Extract(const std::string& file_path) {
    VideoMetadata metadata;

    if (file_path.empty()) {
        Debug::Log("FFmpegMetadataExtractor::Extract: Empty file path");
        return metadata;
    }

    AVFormatContext* format_ctx = nullptr;
    if (!OpenFile(file_path, &format_ctx)) {
        return metadata;
    }

    // Extract all metadata from streams
    ExtractFileInfo(file_path, format_ctx, metadata);
    ExtractVideoStream(format_ctx, metadata);
    ExtractAudioStream(format_ctx, metadata);

    // Cleanup
    avformat_close_input(&format_ctx);

    metadata.is_loaded = true;

    Debug::Log("FFmpegMetadataExtractor::Extract: Completed for " + file_path);
    Debug::Log("  Resolution: " + std::to_string(metadata.width) + "x" + std::to_string(metadata.height));
    Debug::Log("  Pixel Format: " + metadata.pixel_format);
    Debug::Log("  Colorspace: " + metadata.colorspace);
    Debug::Log("  Range: " + metadata.range_type);
    Debug::Log("  Codec: " + metadata.video_codec);
    Debug::Log("  Audio Codec: " + metadata.audio_codec);
    Debug::Log("  Audio Channels: " + std::to_string(metadata.audio_channels));
    if (metadata.has_embedded_timecode) {
        Debug::Log("  Timecode: " + metadata.timecode_format);
    }

    return metadata;
}

double FFmpegMetadataExtractor::ProbeDuration(const std::string& file_path) {
    if (file_path.empty()) {
        return 0.0;
    }

    AVFormatContext* format_ctx = nullptr;
    if (!OpenFile(file_path, &format_ctx)) {
        return 0.0;
    }

    double duration = format_ctx->duration / static_cast<double>(AV_TIME_BASE);

    avformat_close_input(&format_ctx);

    Debug::Log("FFmpegMetadataExtractor::ProbeDuration: " + file_path + " = " + std::to_string(duration) + "s");

    return duration;
}

// ============================================================================
// PRIVATE HELPERS
// ============================================================================

bool FFmpegMetadataExtractor::OpenFile(const std::string& file_path, AVFormatContext** format_ctx) {
    *format_ctx = avformat_alloc_context();
    if (!*format_ctx) {
        Debug::Log("FFmpegMetadataExtractor::OpenFile: Failed to allocate format context");
        return false;
    }

    if (avformat_open_input(format_ctx, file_path.c_str(), nullptr, nullptr) < 0) {
        Debug::Log("FFmpegMetadataExtractor::OpenFile: Failed to open file: " + file_path);
        return false;
    }

    if (avformat_find_stream_info(*format_ctx, nullptr) < 0) {
        Debug::Log("FFmpegMetadataExtractor::OpenFile: Failed to find stream info: " + file_path);
        avformat_close_input(format_ctx);
        return false;
    }

    return true;
}

void FFmpegMetadataExtractor::ExtractFileInfo(const std::string& file_path,
                                               AVFormatContext* format_ctx,
                                               VideoMetadata& metadata) {
    metadata.file_path = file_path;

    if (fs::exists(file_path)) {
        metadata.file_name = fs::path(file_path).filename().string();
        metadata.file_size = fs::file_size(file_path);
    }

    // Duration from container (works for audio-only files too)
    if (format_ctx->duration != AV_NOPTS_VALUE) {
        metadata.duration = format_ctx->duration / static_cast<double>(AV_TIME_BASE);
        metadata.total_frames = 0; // Will be calculated after we know frame rate
    }
}

void FFmpegMetadataExtractor::ExtractVideoStream(AVFormatContext* format_ctx, VideoMetadata& metadata) {
    // Find video stream
    int video_stream_index = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index < 0) {
        Debug::Log("FFmpegMetadataExtractor::ExtractVideoStream: No video stream found");
        return;
    }

    AVStream* video_stream = format_ctx->streams[video_stream_index];
    AVCodecParameters* codecpar = video_stream->codecpar;

    // Extract timecode from stream metadata (MXF, MOV, etc.)
    // FFmpeg stores timecode in stream metadata as "timecode" key
    Debug::Log("FFmpegMetadataExtractor: Checking video stream " + std::to_string(video_stream_index) + " for timecode");
    Debug::Log("FFmpegMetadataExtractor: Video stream metadata ptr: " + std::to_string((uintptr_t)video_stream->metadata));

    // Debug: dump all metadata keys in video stream
    if (video_stream->metadata) {
        AVDictionaryEntry* tag = nullptr;
        while ((tag = av_dict_get(video_stream->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
            Debug::Log("FFmpegMetadataExtractor: Stream metadata key='" + std::string(tag->key) +
                       "' value='" + std::string(tag->value) + "'");
        }
    }

    AVDictionaryEntry* tc_entry = av_dict_get(video_stream->metadata, "timecode", nullptr, 0);
    if (tc_entry && tc_entry->value) {
        metadata.timecode_format = tc_entry->value;
        metadata.has_embedded_timecode = true;
        Debug::Log("FFmpegMetadataExtractor: Found stream timecode: " + metadata.timecode_format);
    } else {
        Debug::Log("FFmpegMetadataExtractor: No timecode in video stream metadata");
    }

    // Also check format-level metadata (some containers store it there)
    if (!metadata.has_embedded_timecode) {
        Debug::Log("FFmpegMetadataExtractor: Checking format-level metadata");
        tc_entry = av_dict_get(format_ctx->metadata, "timecode", nullptr, 0);
        if (tc_entry && tc_entry->value) {
            metadata.timecode_format = tc_entry->value;
            metadata.has_embedded_timecode = true;
            Debug::Log("FFmpegMetadataExtractor: Found format timecode: " + metadata.timecode_format);
        }
    }

    // Check for timecode in all streams (some files have dedicated timecode tracks)
    if (!metadata.has_embedded_timecode) {
        Debug::Log("FFmpegMetadataExtractor: Checking all " + std::to_string(format_ctx->nb_streams) + " streams for timecode");
        for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
            AVStream* stream = format_ctx->streams[i];

            // Debug: show stream type
            const char* stream_type = "unknown";
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) stream_type = "video";
            else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) stream_type = "audio";
            else if (stream->codecpar->codec_type == AVMEDIA_TYPE_DATA) stream_type = "data";
            else if (stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) stream_type = "subtitle";

            Debug::Log("FFmpegMetadataExtractor: Stream " + std::to_string(i) + " type=" + stream_type);

            // Dump all metadata for this stream
            if (stream->metadata) {
                AVDictionaryEntry* tag = nullptr;
                while ((tag = av_dict_get(stream->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
                    Debug::Log("FFmpegMetadataExtractor:   Stream " + std::to_string(i) +
                               " key='" + std::string(tag->key) + "' value='" + std::string(tag->value) + "'");
                }
            }

            tc_entry = av_dict_get(stream->metadata, "timecode", nullptr, 0);
            if (tc_entry && tc_entry->value) {
                metadata.timecode_format = tc_entry->value;
                metadata.has_embedded_timecode = true;
                Debug::Log("FFmpegMetadataExtractor: Found timecode in stream " +
                           std::to_string(i) + ": " + metadata.timecode_format);
                break;
            }
        }
    }

    // Dimensions
    metadata.width = codecpar->width;
    metadata.height = codecpar->height;

    // Frame rate
    AVRational frame_rate = video_stream->r_frame_rate;
    if (frame_rate.num == 0 || frame_rate.den == 0) {
        frame_rate = video_stream->avg_frame_rate;
    }
    if (frame_rate.num > 0 && frame_rate.den > 0) {
        metadata.frame_rate = av_q2d(frame_rate);
    } else {
        metadata.frame_rate = 24.0; // Fallback
    }

    // Calculate total frames
    if (format_ctx->duration != AV_NOPTS_VALUE) {
        double duration = format_ctx->duration / static_cast<double>(AV_TIME_BASE);
        metadata.total_frames = static_cast<int>(duration * metadata.frame_rate);
    }

    // Stream start time (for H.264/H.265 PTS offset sync)
    // This is non-zero for containers with B-frames or edit lists
    if (video_stream->start_time != AV_NOPTS_VALUE) {
        metadata.stream_start_time = av_rescale_q(video_stream->start_time,
                                                   video_stream->time_base,
                                                   AV_TIME_BASE_Q);
    }

    // Video codec
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (codec) {
        metadata.video_codec = codec->name;
    } else {
        metadata.video_codec = "unknown";
    }

    // Pixel format (CRITICAL for 420/422/4444 detection)
    metadata.pixel_format = AVPixelFormatToString(codecpar->format);

    // Cache format detection flags immediately
    metadata.is_411_format = metadata.Is411Format();
    metadata.is_421_format = metadata.Is421Format();

    // Extract color properties
    ExtractColorProperties(video_stream, metadata);
}

void FFmpegMetadataExtractor::ExtractAudioStream(AVFormatContext* format_ctx, VideoMetadata& metadata) {
    // Find audio stream
    int audio_stream_index = av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_stream_index < 0) {
        // No audio stream (not an error - video might be video-only)
        return;
    }

    AVStream* audio_stream = format_ctx->streams[audio_stream_index];
    AVCodecParameters* codecpar = audio_stream->codecpar;

    // Audio codec
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (codec) {
        metadata.audio_codec = codec->name;
    } else {
        metadata.audio_codec = "unknown";
    }

    // Audio properties
    metadata.audio_sample_rate = codecpar->sample_rate;

    // Handle channel count (newer FFmpeg uses ch_layout)
#if LIBAVUTIL_VERSION_MAJOR >= 58
    metadata.audio_channels = codecpar->ch_layout.nb_channels;
#else
    metadata.audio_channels = codecpar->channels;
#endif
}

void FFmpegMetadataExtractor::ExtractColorProperties(const AVStream* stream, VideoMetadata& metadata) {
    AVCodecParameters* codecpar = stream->codecpar;

    // Colorspace (matrix coefficients)
    metadata.colorspace = AVColorSpaceToString(codecpar->color_space);

    // Color primaries
    metadata.color_primaries = AVColorPrimariesToString(codecpar->color_primaries);

    // Color transfer characteristics
    metadata.color_transfer = AVColorTransferToString(codecpar->color_trc);

    // Color range
    metadata.range_type = AVColorRangeToString(codecpar->color_range);

    // Detect and cache NCLC (QuickTime color tag) for later use
    metadata.DetectAndCacheNCLC();
}

// ============================================================================
// PIXEL FORMAT AND COLOR MAPPING
// ============================================================================

std::string FFmpegMetadataExtractor::AVPixelFormatToString(int av_pix_fmt) {
    if (av_pix_fmt == AV_PIX_FMT_NONE) {
        return "unknown";
    }

    const char* pix_fmt_name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(av_pix_fmt));
    if (pix_fmt_name) {
        return std::string(pix_fmt_name);
    }

    return "unknown";
}

std::string FFmpegMetadataExtractor::AVColorSpaceToString(int av_color_space) {
    switch (av_color_space) {
        case AVCOL_SPC_RGB:           return "rgb";
        case AVCOL_SPC_BT709:         return "bt709";
        case AVCOL_SPC_UNSPECIFIED:   return "unknown";
        case AVCOL_SPC_RESERVED:      return "reserved";
        case AVCOL_SPC_FCC:           return "fcc";
        case AVCOL_SPC_BT470BG:       return "bt470bg";
        case AVCOL_SPC_SMPTE170M:     return "smpte170m";
        case AVCOL_SPC_SMPTE240M:     return "smpte240m";
        case AVCOL_SPC_YCGCO:         return "ycgco";
        case AVCOL_SPC_BT2020_NCL:    return "bt2020nc";
        case AVCOL_SPC_BT2020_CL:     return "bt2020c";
        case AVCOL_SPC_SMPTE2085:     return "smpte2085";
        case AVCOL_SPC_CHROMA_DERIVED_NCL: return "chroma-derived-nc";
        case AVCOL_SPC_CHROMA_DERIVED_CL:  return "chroma-derived-c";
        case AVCOL_SPC_ICTCP:         return "ictcp";
        default:                      return "unknown";
    }
}

std::string FFmpegMetadataExtractor::AVColorPrimariesToString(int av_color_primaries) {
    switch (av_color_primaries) {
        case AVCOL_PRI_RESERVED0:     return "reserved";
        case AVCOL_PRI_BT709:         return "bt709";
        case AVCOL_PRI_UNSPECIFIED:   return "unknown";
        case AVCOL_PRI_RESERVED:      return "reserved";
        case AVCOL_PRI_BT470M:        return "bt470m";
        case AVCOL_PRI_BT470BG:       return "bt470bg";
        case AVCOL_PRI_SMPTE170M:     return "smpte170m";
        case AVCOL_PRI_SMPTE240M:     return "smpte240m";
        case AVCOL_PRI_FILM:          return "film";
        case AVCOL_PRI_BT2020:        return "bt2020";
        case AVCOL_PRI_SMPTE428:      return "smpte428";
        case AVCOL_PRI_SMPTE431:      return "smpte431";
        case AVCOL_PRI_SMPTE432:      return "smpte432";
        case AVCOL_PRI_EBU3213:       return "ebu3213";
        default:                      return "unknown";
    }
}

std::string FFmpegMetadataExtractor::AVColorTransferToString(int av_color_trc) {
    switch (av_color_trc) {
        case AVCOL_TRC_RESERVED0:     return "reserved";
        case AVCOL_TRC_BT709:         return "bt709";
        case AVCOL_TRC_UNSPECIFIED:   return "unknown";
        case AVCOL_TRC_RESERVED:      return "reserved";
        case AVCOL_TRC_GAMMA22:       return "gamma22";
        case AVCOL_TRC_GAMMA28:       return "gamma28";
        case AVCOL_TRC_SMPTE170M:     return "smpte170m";
        case AVCOL_TRC_SMPTE240M:     return "smpte240m";
        case AVCOL_TRC_LINEAR:        return "linear";
        case AVCOL_TRC_LOG:           return "log100";
        case AVCOL_TRC_LOG_SQRT:      return "log316";
        case AVCOL_TRC_IEC61966_2_4:  return "iec61966-2-4";
        case AVCOL_TRC_BT1361_ECG:    return "bt1361e";
        case AVCOL_TRC_IEC61966_2_1:  return "iec61966-2-1";
        case AVCOL_TRC_BT2020_10:     return "bt2020-10";
        case AVCOL_TRC_BT2020_12:     return "bt2020-12";
        case AVCOL_TRC_SMPTE2084:     return "smpte2084";
        case AVCOL_TRC_SMPTE428:      return "smpte428";
        case AVCOL_TRC_ARIB_STD_B67:  return "arib-std-b67";
        default:                      return "unknown";
    }
}

std::string FFmpegMetadataExtractor::AVColorRangeToString(int av_color_range) {
    switch (av_color_range) {
        case AVCOL_RANGE_UNSPECIFIED: return "unknown";
        case AVCOL_RANGE_MPEG:        return "limited";
        case AVCOL_RANGE_JPEG:        return "full";
        default:                      return "unknown";
    }
}

} // namespace ump
