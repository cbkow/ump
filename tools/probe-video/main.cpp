// probe-video — Phase 1.8.0 smoke test for FFmpeg integration.
//
// Opens a video file, prints metadata about the first video stream,
// then decodes the first frame and prints its dimensions / pixel
// format. No hardware acceleration yet — that's Phase 1.8.2.
//
// Usage:  probe-video <path-to-video>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::string formatDuration(int64_t durationUs) {
    if (durationUs <= 0) return "?";
    int64_t totalSec = durationUs / AV_TIME_BASE;
    int hours   = static_cast<int>(totalSec / 3600);
    int minutes = static_cast<int>((totalSec % 3600) / 60);
    int seconds = static_cast<int>(totalSec % 60);
    int millis  = static_cast<int>((durationUs % AV_TIME_BASE) / 1000);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                  hours, minutes, seconds, millis);
    return buf;
}

double rationalToDouble(AVRational r) {
    return r.den ? static_cast<double>(r.num) / r.den : 0.0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: probe-video <path>\n");
        return 2;
    }
    const char *path = argv[1];

    AVFormatContext *fmt = nullptr;
    if (int err = avformat_open_input(&fmt, path, nullptr, nullptr); err < 0) {
        char msg[256];
        av_strerror(err, msg, sizeof(msg));
        std::fprintf(stderr, "avformat_open_input failed: %s\n", msg);
        return 1;
    }

    if (int err = avformat_find_stream_info(fmt, nullptr); err < 0) {
        char msg[256];
        av_strerror(err, msg, sizeof(msg));
        std::fprintf(stderr, "avformat_find_stream_info failed: %s\n", msg);
        avformat_close_input(&fmt);
        return 1;
    }

    int videoStreamIdx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIdx = static_cast<int>(i);
            break;
        }
    }
    if (videoStreamIdx < 0) {
        std::fprintf(stderr, "no video stream found in %s\n", path);
        avformat_close_input(&fmt);
        return 1;
    }

    AVStream *st = fmt->streams[videoStreamIdx];
    AVCodecParameters *codecpar = st->codecpar;

    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::fprintf(stderr, "no decoder for codec_id %d\n", codecpar->codec_id);
        avformat_close_input(&fmt);
        return 1;
    }

    AVCodecContext *cctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(cctx, codecpar);

    if (int err = avcodec_open2(cctx, codec, nullptr); err < 0) {
        char msg[256];
        av_strerror(err, msg, sizeof(msg));
        std::fprintf(stderr, "avcodec_open2 failed: %s\n", msg);
        avcodec_free_context(&cctx);
        avformat_close_input(&fmt);
        return 1;
    }

    // ---- Metadata dump
    const AVCodecDescriptor *desc = avcodec_descriptor_get(codecpar->codec_id);
    const char *intraStr = (desc && (desc->props & AV_CODEC_PROP_INTRA_ONLY))
                               ? "intra-only" : "inter-frame";

    const AVPixelFormat pixfmt = static_cast<AVPixelFormat>(codecpar->format);
    const char *pixfmtName = av_get_pix_fmt_name(pixfmt);
    if (!pixfmtName) pixfmtName = "?";

    std::printf("=== probe-video — Phase 1.8.0 smoke test ===\n");
    std::printf("file:       %s\n", path);
    std::printf("container:  %s\n", fmt->iformat->name ? fmt->iformat->name : "?");
    std::printf("duration:   %s (%lld us)\n",
                formatDuration(fmt->duration).c_str(),
                static_cast<long long>(fmt->duration));
    std::printf("\n");
    std::printf("video stream #%d:\n", videoStreamIdx);
    std::printf("  codec:        %s (id=%d, %s)\n",
                codec->long_name ? codec->long_name : codec->name,
                codecpar->codec_id, intraStr);
    std::printf("  dimensions:   %dx%d\n", codecpar->width, codecpar->height);
    std::printf("  pixel fmt:    %s\n", pixfmtName);
    std::printf("  bit depth:    %d bits/component (chroma %s)\n",
                av_pix_fmt_desc_get(pixfmt) ?
                    av_pix_fmt_desc_get(pixfmt)->comp[0].depth : 0,
                av_pix_fmt_desc_get(pixfmt) ?
                    (av_pix_fmt_desc_get(pixfmt)->log2_chroma_w == 0 &&
                     av_pix_fmt_desc_get(pixfmt)->log2_chroma_h == 0 ? "4:4:4" :
                     av_pix_fmt_desc_get(pixfmt)->log2_chroma_w == 1 &&
                     av_pix_fmt_desc_get(pixfmt)->log2_chroma_h == 0 ? "4:2:2" :
                     av_pix_fmt_desc_get(pixfmt)->log2_chroma_w == 1 &&
                     av_pix_fmt_desc_get(pixfmt)->log2_chroma_h == 1 ? "4:2:0" : "?")
                    : "?");
    std::printf("  fps (avg):    %.6f\n", rationalToDouble(st->avg_frame_rate));
    std::printf("  fps (real):   %.6f\n", rationalToDouble(st->r_frame_rate));
    std::printf("  time_base:    %d/%d\n", st->time_base.num, st->time_base.den);
    std::printf("  nb_frames:    %lld\n", static_cast<long long>(st->nb_frames));
    std::printf("  start_time:   %lld\n", static_cast<long long>(st->start_time));
    std::printf("  color_range:  %d  primaries: %d  transfer: %d  space: %d\n",
                codecpar->color_range, codecpar->color_primaries,
                codecpar->color_trc, codecpar->color_space);
    std::printf("\n");

    // ---- Decode the first frame
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    bool gotFrame = false;
    int packetsRead = 0;

    while (!gotFrame && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != videoStreamIdx) {
            av_packet_unref(pkt);
            continue;
        }
        ++packetsRead;
        if (avcodec_send_packet(cctx, pkt) >= 0) {
            int rc = avcodec_receive_frame(cctx, frame);
            if (rc >= 0) {
                gotFrame = true;
            } else if (rc != AVERROR(EAGAIN)) {
                char msg[256];
                av_strerror(rc, msg, sizeof(msg));
                std::fprintf(stderr, "receive_frame error: %s\n", msg);
            }
        }
        av_packet_unref(pkt);
    }

    if (gotFrame) {
        std::printf("first frame:\n");
        std::printf("  decoded:      %dx%d\n", frame->width, frame->height);
        std::printf("  pix_fmt:      %s\n",
                    av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)));
        std::printf("  pts:          %lld (best_effort: %lld)\n",
                    static_cast<long long>(frame->pts),
                    static_cast<long long>(frame->best_effort_timestamp));
        std::printf("  pkt_dts:      %lld\n",
                    static_cast<long long>(frame->pkt_dts));
        std::printf("  key_frame:    %s\n",
                    (frame->flags & AV_FRAME_FLAG_KEY) ? "yes" : "no");
        std::printf("  packets read: %d\n", packetsRead);
        std::printf("\nresult: PASS — first frame decoded successfully.\n");
    } else {
        std::printf("\nresult: FAIL — no frame decoded after %d packets.\n",
                    packetsRead);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&cctx);
    avformat_close_input(&fmt);

    return gotFrame ? 0 : 1;
}
