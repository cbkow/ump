// VideoImageLoader implementation. See header for architecture +
// adaptation notes vs old QCView's image_loaders.cpp.

#include "video_image_loader.h"

#include <QtLogging>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <climits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace qcv {

VideoImageLoader::VideoImageLoader(const std::string &videoPath,
                                   double fpsHint, double durationHint)
    : m_videoPath(videoPath)
    , m_fps(fpsHint > 0.0 ? fpsHint : 24.0)
    , m_duration(durationHint)
{
    m_initialized = initializeFfmpeg();
    if (!m_initialized) {
        qWarning("VideoImageLoader: init failed for %s",
                 m_videoPath.c_str());
    }
}

VideoImageLoader::~VideoImageLoader()
{
    cleanupFfmpeg();
}

int VideoImageLoader::frameCount() const
{
    if (m_fps <= 0.0 || m_duration <= 0.0) return 0;
    return static_cast<int>(m_duration * m_fps);
}

bool VideoImageLoader::initializeFfmpeg()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_formatCtx = avformat_alloc_context();
    if (avformat_open_input(&m_formatCtx, m_videoPath.c_str(),
                            nullptr, nullptr) < 0) {
        return false;
    }

    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
        avformat_close_input(&m_formatCtx);
        return false;
    }

    m_videoStreamIdx = av_find_best_stream(
        m_formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_videoStreamIdx < 0) {
        avformat_close_input(&m_formatCtx);
        return false;
    }

    AVStream *stream = m_formatCtx->streams[m_videoStreamIdx];
    m_width  = stream->codecpar->width;
    m_height = stream->codecpar->height;

    // Detect actual fps from container metadata — the hint passed
    // to the ctor may be the timeline fps, which is wrong for
    // single-frame seek math.
    double detectedFps = 0.0;
    if (stream->avg_frame_rate.den > 0
        && stream->avg_frame_rate.num > 0) {
        detectedFps = av_q2d(stream->avg_frame_rate);
    } else if (stream->r_frame_rate.den > 0
               && stream->r_frame_rate.num > 0) {
        detectedFps = av_q2d(stream->r_frame_rate);
    }
    if (detectedFps > 0.0 && std::abs(detectedFps - m_fps) > 0.1) {
        m_fps = detectedFps;
    }

    double detectedDur = 0.0;
    if (m_formatCtx->duration > 0) {
        detectedDur = static_cast<double>(m_formatCtx->duration)
                      / AV_TIME_BASE;
    } else if (stream->duration > 0) {
        detectedDur = static_cast<double>(stream->duration)
                      * av_q2d(stream->time_base);
    }
    if (detectedDur > 0.0
        && (m_duration <= 0.0
            || std::abs(detectedDur - m_duration) > 0.1)) {
        m_duration = detectedDur;
    }

    // Container start_time offset (HEVC, some MOVs).
    if (stream->start_time != AV_NOPTS_VALUE) {
        m_startTime = av_rescale_q(stream->start_time,
                                    stream->time_base,
                                    AV_TIME_BASE_Q);
    }

    const AVCodec *decoder =
        avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        avformat_close_input(&m_formatCtx);
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(decoder);
    if (avcodec_parameters_to_context(m_codecCtx,
                                       stream->codecpar) < 0) {
        avcodec_free_context(&m_codecCtx);
        avformat_close_input(&m_formatCtx);
        return false;
    }

    // Single-threaded software decode for thread safety; AV_CODEC_FLAG2_FAST
    // for ~1.3x faster decode on quality-tolerant thumbnails.
    m_codecCtx->thread_count = 1;
    m_codecCtx->thread_type  = FF_THREAD_SLICE;
    m_codecCtx->flags2      |= AV_CODEC_FLAG2_FAST;

    if (avcodec_open2(m_codecCtx, decoder, nullptr) < 0) {
        avcodec_free_context(&m_codecCtx);
        avformat_close_input(&m_formatCtx);
        return false;
    }

    return true;
}

void VideoImageLoader::cleanupFfmpeg()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
    }
    if (m_formatCtx) {
        avformat_close_input(&m_formatCtx);
    }
    m_initialized = false;
}

std::shared_ptr<PixelData> VideoImageLoader::loadFrame(
    const std::string &path, const std::string & /*layer*/,
    PipelineMode pipeline_mode)
{
    int frameNo = 0;
    try {
        frameNo = std::stoi(path);
    } catch (...) {
        return nullptr;
    }
    return extractFrame(frameNo, pipeline_mode, /*max_size=*/0);
}

std::shared_ptr<PixelData> VideoImageLoader::loadThumbnail(
    const std::string &path, int max_size)
{
    int frameNo = 0;
    try {
        frameNo = std::stoi(path);
    } catch (...) {
        return nullptr;
    }
    return extractFrame(frameNo, PipelineMode::NORMAL, max_size);
}

bool VideoImageLoader::getDimensions(const std::string & /*path*/,
                                      int &width, int &height)
{
    width  = m_width;
    height = m_height;
    return m_initialized;
}

std::shared_ptr<PixelData> VideoImageLoader::extractFrame(
    int frameNumber, PipelineMode mode, int max_size)
{
    if (!m_initialized) return nullptr;

    std::lock_guard<std::mutex> lock(m_mutex);

    int maxFrame = frameCount() - 1;
    if (maxFrame < 0) maxFrame = 0;
    if (frameNumber < 0) frameNumber = 0;
    if (frameNumber > maxFrame) frameNumber = maxFrame;

    // +0.5 → seek to mid-frame so we land robustly inside the
    // frame's display period at boundaries.
    double timestamp = (static_cast<double>(frameNumber) + 0.5) / m_fps;
    if (m_startTime > 0) {
        timestamp += static_cast<double>(m_startTime) / AV_TIME_BASE;
    }

    AVFrame *frame = av_frame_alloc();
    if (!frame) return nullptr;

    if (!seekAndDecodeFrame(timestamp, frame)) {
        av_frame_free(&frame);
        return nullptr;
    }

    auto result = std::make_shared<PixelData>();
    result->setFormat(PixelFormat::RGBA8);
    result->pipeline_mode = PipelineMode::NORMAL;

    if (!convertFrameToPixels(frame, result->pixels,
                                result->width, result->height,
                                mode, max_size)) {
        av_frame_free(&frame);
        return nullptr;
    }

    av_frame_free(&frame);
    return result;
}

bool VideoImageLoader::seekAndDecodeFrame(double timestampSec,
                                           AVFrame *output)
{
    AVStream *stream = m_formatCtx->streams[m_videoStreamIdx];
    int64_t targetPts = av_rescale_q(
        static_cast<int64_t>(timestampSec * AV_TIME_BASE),
        AV_TIME_BASE_Q, stream->time_base);

    if (av_seek_frame(m_formatCtx, m_videoStreamIdx, targetPts,
                      AVSEEK_FLAG_BACKWARD) < 0) {
        return false;
    }
    avcodec_flush_buffers(m_codecCtx);

    AVPacket *packet = av_packet_alloc();
    if (!packet) return false;

    bool found        = false;
    bool passedTarget = false;
    int64_t bestDiff  = INT64_MAX;
    AVFrame *best = av_frame_alloc();

    // Decode forward from the keyframe seek-target until we've
    // walked past the requested PTS, keeping the closest decoded
    // frame as we go.
    while (av_read_frame(m_formatCtx, packet) >= 0) {
        if (packet->stream_index == m_videoStreamIdx) {
            if (avcodec_send_packet(m_codecCtx, packet) >= 0) {
                while (avcodec_receive_frame(m_codecCtx, output)
                       >= 0) {
                    int64_t framePts = output->pts;
                    int64_t diff =
                        std::abs(framePts - targetPts);
                    if (diff < bestDiff) {
                        bestDiff = diff;
                        av_frame_unref(best);
                        av_frame_ref(best, output);
                        found = true;
                    }
                    if (framePts > targetPts) {
                        passedTarget = true;
                    }
                    if (passedTarget && found) {
                        av_frame_unref(output);
                        break;
                    }
                    av_frame_unref(output);
                }
            }
            if (passedTarget && found) {
                av_packet_unref(packet);
                break;
            }
        }
        av_packet_unref(packet);
    }

    if (found) {
        av_frame_ref(output, best);
    }
    av_frame_free(&best);
    av_packet_free(&packet);
    return found;
}

bool VideoImageLoader::convertFrameToPixels(
    AVFrame *frame, std::vector<std::uint8_t> &outPixels,
    int &outW, int &outH,
    PipelineMode /*mode*/, int max_size)
{
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        return false;
    }

    if (max_size > 0) {
        int srcMax = std::max(frame->width, frame->height);
        if (srcMax > max_size) {
            float scale =
                static_cast<float>(max_size) / srcMax;
            outW = static_cast<int>(frame->width * scale);
            outH = static_cast<int>(frame->height * scale);
        } else {
            outW = frame->width;
            outH = frame->height;
        }
    } else {
        outW = frame->width;
        outH = frame->height;
    }

    AVPixelFormat targetFmt = AV_PIX_FMT_RGBA;
    constexpr int kBytesPerPixel = 4;

    AVFrame *target = av_frame_alloc();
    target->format = targetFmt;
    target->width  = outW;
    target->height = outH;
    if (av_frame_get_buffer(target, 32) < 0) {
        av_frame_free(&target);
        return false;
    }

    bool needNewSws = (m_swsCtx == nullptr
                        || m_swsSrcW   != frame->width
                        || m_swsSrcH   != frame->height
                        || m_swsSrcFmt != frame->format
                        || m_swsDstW   != outW
                        || m_swsDstH   != outH);
    if (needNewSws) {
        if (m_swsCtx) sws_freeContext(m_swsCtx);
        m_swsCtx = sws_getContext(
            frame->width, frame->height,
            static_cast<AVPixelFormat>(frame->format),
            outW, outH, targetFmt,
            SWS_POINT,        // nearest-neighbor — fast for thumbs
            nullptr, nullptr, nullptr);
        if (!m_swsCtx) {
            av_frame_free(&target);
            return false;
        }
        m_swsSrcW   = frame->width;
        m_swsSrcH   = frame->height;
        m_swsSrcFmt = frame->format;
        m_swsDstW   = outW;
        m_swsDstH   = outH;
    }

    sws_scale(m_swsCtx, frame->data, frame->linesize, 0,
              frame->height, target->data, target->linesize);

    const std::size_t dataSize =
        static_cast<std::size_t>(outW) * outH * kBytesPerPixel;
    outPixels.resize(dataSize);

    const int rowSize = outW * kBytesPerPixel;
    if (target->linesize[0] == rowSize) {
        std::memcpy(outPixels.data(), target->data[0], dataSize);
    } else {
        // Stride > row_size — copy line by line.
        std::uint8_t *dst = outPixels.data();
        const std::uint8_t *src = target->data[0];
        for (int y = 0; y < outH; ++y) {
            std::memcpy(dst, src, rowSize);
            dst += rowSize;
            src += target->linesize[0];
        }
    }

    av_frame_free(&target);
    return true;
}

} // namespace qcv
