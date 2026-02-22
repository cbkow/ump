#pragma once

#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/parseutils.h>
#include <libavutil/audio_fifo.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
}

namespace qcview {

/**
 * FFMPEGVideoEncoder
 *
 * Encodes RGBA frames to video using FFMPEG.
 * Inverse of VideoImageLoader (which decodes).
 *
 * Supports:
 * - H.264 (libx264) with CRF quality
 * - H.265 (libx265) with CRF quality
 * - ProRes (prores_ks)
 * - DNxHD (dnxhd)
 */
class FFMPEGVideoEncoder {
public:
    struct EncoderSettings {
        std::string output_path;
        int width = 0;
        int height = 0;
        double fps = 24.0;

        // Codec settings
        std::string codec = "libx264";  // or "libx265", "prores_ks", "dnxhd"
        int crf = 18;  // Quality (0-51 for x264/x265, lower=better, 18=visually lossless)
        std::string preset = "slow";  // x264/x265 preset: ultrafast/fast/medium/slow/veryslow
        std::string pixel_format = "yuv420p";  // or yuv444p for better color
        int bitrate_kbps = 0;  // 0 = CRF mode, >0 = constant bitrate mode

        // Advanced
        std::string profile = "";  // e.g., "high" for H.264, "main" for H.265
        int prores_profile = 3;  // ProRes profile: 0=Proxy, 1=LT, 2=Standard, 3=HQ, 4=4444, 5=4444XQ
        int gop_size = 12;  // Keyframe interval (frames)
        int max_b_frames = 2;  // B-frame count

        // Performance
        int thread_count = 0;  // 0 = auto (use all CPU cores), 1-32 = manual thread count

        // Audio settings
        bool copy_audio = true;  // Copy audio streams from source (stream copy mode)

        // LUT-based color transform (for video file encoding)
        std::string lut_file_path;   // Path to .cube LUT file (empty = no LUT)
        bool use_lut_filter = false; // Enable FFmpeg lut3d filter
    };

    struct AudioStreamInfo {
        int source_stream_index = -1;  // Index in source file
        int output_stream_index = -1;  // Index in output file
        AVStream* output_stream = nullptr;
        AVRational source_time_base;

        // Re-encoding support (NEW)
        bool needs_reencoding = false;
        AVCodecContext* decoder_ctx = nullptr;  // For decoding source audio
        AVCodecContext* encoder_ctx = nullptr;  // For encoding to AAC
        struct SwrContext* swr_ctx = nullptr;   // For resampling
        AVAudioFifo* fifo = nullptr;            // Sample buffer for fixed-size encoding
        int64_t next_pts = 0;                   // Next output PTS for encoder
    };

    FFMPEGVideoEncoder();
    ~FFMPEGVideoEncoder();

    /**
     * Open encoder (prepares encoder, does NOT write header yet)
     *
     * After Open(), call AddAudioStream() for any audio streams,
     * then call WriteHeader() to finalize and start encoding.
     *
     * @param settings Encoder configuration
     * @return true if successful
     */
    bool Open(const EncoderSettings& settings);

    /**
     * Write file header (call after Open() and AddAudioStream())
     *
     * This finalizes all stream configurations and writes the file header.
     * Must be called before EncodeFrame() or WriteAudioPacket().
     *
     * @return true if successful
     */
    bool WriteHeader();

    /**
     * Encode one frame
     *
     * @param rgba_pixels RGBA8 pixel data (4 bytes per pixel)
     * @param stride Row stride in bytes (usually width * 4)
     * @return true if successful
     */
    bool EncodeFrame(const uint8_t* rgba_pixels, int stride);

    /**
     * Close encoder and finalize file
     *
     * Flushes any remaining frames and writes file trailer.
     *
     * @return true if successful
     */
    bool Close();

    /**
     * Get current frame count (number of frames encoded)
     */
    int GetFrameCount() const { return frame_count_; }

    /**
     * Get output file path
     */
    const std::string& GetOutputPath() const { return settings_.output_path; }

    /**
     * Detect best available hardware encoder for H.264/H.265
     *
     * @param base_codec "h264" or "h265"
     * @param prefer_hardware Try hardware encoders first
     * @param preferred_hw "nvenc", "qsv", "videotoolbox", or "auto"
     * @return Codec name (e.g., "h264_nvenc", "libx264")
     */
    static std::string SelectBestEncoder(const std::string& base_codec,
                                         bool prefer_hardware = true,
                                         const std::string& preferred_hw = "auto");

    /**
     * Add audio stream from source file (stream copy mode)
     *
     * @param source_format_ctx Source file format context
     * @param source_stream_index Audio stream index in source file
     * @return true if successful
     */
    bool AddAudioStream(AVFormatContext* source_format_ctx, int source_stream_index);

    /**
     * Add audio stream with re-encoding (for incompatible codecs)
     *
     * @param source_format_ctx Source file format context
     * @param source_stream_index Audio stream index in source file
     * @param target_codec Target codec name (e.g., "aac")
     * @param target_bitrate_kbps Target bitrate in kbps (e.g., 192)
     * @return true if successful
     */
    bool AddAudioStreamWithEncoding(AVFormatContext* source_format_ctx, int source_stream_index,
                                     const std::string& target_codec = "aac", int target_bitrate_kbps = 192);

    /**
     * Set audio trim offset (for In/Out point trimming)
     *
     * When trimming video with In/Out points, audio timestamps need to be
     * offset to start from 0 to match the trimmed video.
     *
     * @param start_time_sec Start time in seconds (e.g., 10.0 for In point at 10s)
     */
    void SetAudioTrimOffset(double start_time_sec) { audio_trim_offset_sec_ = start_time_sec; }

    /**
     * Write audio packet (stream copy mode)
     *
     * @param packet Audio packet from source file
     * @param source_stream_index Stream index in source file
     * @return true if successful
     */
    bool WriteAudioPacket(AVPacket* packet, int source_stream_index);

private:
    // Initialize codec context
    bool InitializeCodec();

    // Create output format context
    bool CreateOutputContext();

    // Initialize filter graph for LUT-based color transform
    bool InitializeFilterGraph(const std::string& lut_path);

    // Escape path for FFmpeg filter string (Windows paths need escaping)
    static std::string EscapePathForFFmpeg(const std::string& path);

    // Encode and write packet
    bool EncodeAndWrite(AVFrame* frame);

    // Flush encoder
    bool FlushEncoder();

    EncoderSettings settings_;

    AVFormatContext* format_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    AVStream* video_stream_ = nullptr;
    const AVCodec* codec_ = nullptr;

    SwsContext* sws_ctx_ = nullptr;  // RGB → YUV conversion

    // Audio streams (NEW - for stream copy mode)
    std::vector<AudioStreamInfo> audio_streams_;

    // Audio trim offset for In/Out point support
    double audio_trim_offset_sec_ = 0.0;

    int frame_count_ = 0;
    bool is_open_ = false;
    bool header_written_ = false;

    // Filter graph for LUT-based color transform
    AVFilterGraph* filter_graph_ = nullptr;
    AVFilterContext* buffersrc_ctx_ = nullptr;
    AVFilterContext* buffersink_ctx_ = nullptr;
    AVFrame* filtered_frame_ = nullptr;  // Frame after filter processing
    bool use_filter_graph_ = false;
};

} // namespace qcview
