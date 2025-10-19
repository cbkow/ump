#pragma once

#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/parseutils.h>
}

namespace ump {

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
    };

    FFMPEGVideoEncoder();
    ~FFMPEGVideoEncoder();

    /**
     * Open encoder
     *
     * @param settings Encoder configuration
     * @return true if successful
     */
    bool Open(const EncoderSettings& settings);

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

private:
    // Initialize codec context
    bool InitializeCodec();

    // Create output format context
    bool CreateOutputContext();

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

    int frame_count_ = 0;
    bool is_open_ = false;
};

} // namespace ump
