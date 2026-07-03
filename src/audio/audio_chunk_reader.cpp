#include "audio_chunk_reader.h"

#include "audio_routing_matrix.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include <QtLogging>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace qcv {

namespace {
// Forward-continuation tolerance. Grain cursors advance by exactly
// the frames delivered, so back-to-back reads match to double
// rounding; 2 ms catches that while never mistaking a real jump
// (smallest grain step is ~40 ms of source at 0.5x) for continuity.
constexpr double kContinuitySeconds = 0.002;
constexpr int    kOutRate = 48000;
}

AudioChunkReader::~AudioChunkReader() { close(); }

bool AudioChunkReader::open(const QString &path, int routingMode)
{
    close();
    m_path        = path;
    m_routingMode = routingMode;

    const QByteArray utf8 = path.toUtf8();
    if (avformat_open_input(&m_fmt, utf8.constData(), nullptr, nullptr) < 0) {
        qWarning("AudioChunkReader: open failed for %s", qPrintable(path));
        return false;
    }
    if (avformat_find_stream_info(m_fmt, nullptr) < 0) {
        close();
        return false;
    }
    const AVCodec *dec = nullptr;
    m_streamIdx = av_find_best_stream(m_fmt, AVMEDIA_TYPE_AUDIO, -1, -1,
                                       &dec, 0);
    if (m_streamIdx < 0 || !dec) {
        close();
        return false;
    }
    AVStream *stream = m_fmt->streams[m_streamIdx];
    m_codec = avcodec_alloc_context3(dec);
    if (!m_codec
        || avcodec_parameters_to_context(m_codec, stream->codecpar) < 0
        || avcodec_open2(m_codec, dec, nullptr) < 0) {
        close();
        return false;
    }
    m_streamStartTime =
        (stream->start_time != AV_NOPTS_VALUE) ? stream->start_time : 0;
    m_srcChannels = m_codec->ch_layout.nb_channels;
    m_duration = (m_fmt->duration != AV_NOPTS_VALUE)
        ? static_cast<double>(m_fmt->duration) / AV_TIME_BASE : 0.0;

    if (!setupSwr()) {
        close();
        return false;
    }
    m_pkt   = av_packet_alloc();
    m_frame = av_frame_alloc();
    if (!m_pkt || !m_frame) {
        close();
        return false;
    }
    m_open = true;
    m_posValid = false;
    m_eof = false;
    fifoClear();
    return true;
}

void AudioChunkReader::close()
{
    teardownSwr();
    if (m_frame) av_frame_free(&m_frame);
    if (m_pkt)   av_packet_free(&m_pkt);
    if (m_codec) avcodec_free_context(&m_codec);
    if (m_fmt)   avformat_close_input(&m_fmt);
    m_streamIdx = -1;
    m_open = false;
    m_posValid = false;
    m_eof = false;
    fifoClear();
}

bool AudioChunkReader::setupSwr()
{
    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&m_swr,
                            &outLayout, AV_SAMPLE_FMT_FLT, kOutRate,
                            &m_codec->ch_layout, m_codec->sample_fmt,
                            m_codec->sample_rate,
                            0, nullptr) < 0) {
        return false;
    }
    // Same routing fold as AudioDecoder::setupResampler, so a 5.1
    // master shuttles with the mix the user hears at 1x.
    if (const auto matrix = computeRoutingMatrix(m_routingMode,
                                                 m_srcChannels)) {
        swr_set_matrix(m_swr, matrix->data(), m_srcChannels);
    } else {
        av_opt_set_double(m_swr, "rematrix_volume", 0.9, 0);
    }
    return swr_init(m_swr) >= 0;
}

void AudioChunkReader::teardownSwr()
{
    if (m_swr) swr_free(&m_swr);
}

void AudioChunkReader::fifoClear()
{
    m_fifo.clear();
    m_fifoOffsetFrames = 0;
    m_fifoTimeValid = false;
    m_fifoStartSec = 0.0;
}

bool AudioChunkReader::seekAndPrime(double targetSec)
{
    AVStream *stream = m_fmt->streams[m_streamIdx];
    int64_t ts = static_cast<int64_t>(
        targetSec / av_q2d(stream->time_base));
    if (m_streamStartTime > 0) ts += m_streamStartTime;
    if (av_seek_frame(m_fmt, m_streamIdx, ts, AVSEEK_FLAG_BACKWARD) < 0) {
        av_seek_frame(m_fmt, m_streamIdx, 0, AVSEEK_FLAG_BACKWARD);
    }
    avcodec_flush_buffers(m_codec);
    // Rebuild swr rather than trying to drain its internal delay —
    // microseconds of work at the shuttle's worst-case ~12 seeks/s.
    teardownSwr();
    if (!setupSwr()) return false;
    fifoClear();
    m_eof = false;

    // Decode-and-discard up to targetSec. Each decodeMore() appends
    // to the FIFO with a valid start time; trim leading frames that
    // precede the target. Bounded by the decode landing at or before
    // the keyframe/packet that contains targetSec.
    while (true) {
        const std::size_t fifoFrames =
            m_fifo.size() / 2 - m_fifoOffsetFrames;
        if (m_fifoTimeValid && fifoFrames > 0) {
            const double fifoEndSec = m_fifoStartSec
                + static_cast<double>(fifoFrames) / kOutRate;
            if (fifoEndSec > targetSec) {
                // Trim frames before the target, keep the rest.
                const double aheadSec =
                    std::max(0.0, targetSec - m_fifoStartSec);
                const auto drop = static_cast<std::size_t>(
                    std::llround(aheadSec * kOutRate));
                const std::size_t dropClamped =
                    std::min(drop, fifoFrames);
                m_fifoOffsetFrames += dropClamped;
                m_fifoStartSec     += static_cast<double>(dropClamped)
                                      / kOutRate;
                m_posSec  = m_fifoStartSec;
                m_posValid = true;
                return true;
            }
            // Whole FIFO precedes the target — drop it wholesale.
            fifoClear();
        }
        if (m_eof) {
            // Target past stream end; position at end, deliver 0.
            m_posSec = targetSec;
            m_posValid = true;
            return true;
        }
        if (!decodeMore() && !m_eof) return false;   // fatal decode error
    }
}

bool AudioChunkReader::decodeMore()
{
    // Pull packets until the decoder yields at least one frame (or
    // EOF). Appends resampled output to the FIFO and stamps
    // m_fifoStartSec from the first decoded frame's PTS when the
    // FIFO was empty/timeless.
    while (true) {
        const int rrc = av_read_frame(m_fmt, m_pkt);
        if (rrc < 0) {
            m_eof = true;
            return false;
        }
        if (m_pkt->stream_index != m_streamIdx) {
            av_packet_unref(m_pkt);
            continue;
        }
        int src = avcodec_send_packet(m_codec, m_pkt);
        av_packet_unref(m_pkt);
        if (src < 0) return false;

        bool produced = false;
        while (avcodec_receive_frame(m_codec, m_frame) == 0) {
            const int64_t delay = swr_get_delay(m_swr,
                                                m_codec->sample_rate);
            const int dstCap = static_cast<int>(av_rescale_rnd(
                delay + m_frame->nb_samples,
                kOutRate, m_codec->sample_rate, AV_ROUND_UP));
            const std::size_t needBytes =
                static_cast<std::size_t>(dstCap) * 2 * sizeof(float);
            if (m_swrBuffer.size() < needBytes) m_swrBuffer.resize(needBytes);
            uint8_t *dst = m_swrBuffer.data();
            const int converted = swr_convert(
                m_swr, &dst, dstCap,
                const_cast<const uint8_t **>(m_frame->data),
                m_frame->nb_samples);
            if (converted > 0) {
                if (!m_fifoTimeValid
                    && m_frame->pts != AV_NOPTS_VALUE) {
                    AVStream *stream = m_fmt->streams[m_streamIdx];
                    m_fifoStartSec = static_cast<double>(
                        m_frame->pts - m_streamStartTime)
                        * av_q2d(stream->time_base);
                    m_fifoTimeValid = true;
                } else if (!m_fifoTimeValid) {
                    // No PTS (rare) — anchor at the last known
                    // position; shuttle tolerates sub-frame slop.
                    m_fifoStartSec = m_posValid ? m_posSec : 0.0;
                    m_fifoTimeValid = true;
                }
                const float *pcm =
                    reinterpret_cast<const float *>(m_swrBuffer.data());
                m_fifo.insert(m_fifo.end(), pcm,
                              pcm + static_cast<std::size_t>(converted) * 2);
                produced = true;
            }
            av_frame_unref(m_frame);
        }
        if (produced) return true;
        // Packet consumed but no frame yet (codec priming) — loop.
    }
}

std::size_t AudioChunkReader::readAt(double srcStartSec, float *dst,
                                      std::size_t frames)
{
    if (!m_open || !dst || frames == 0) return 0;

    if (!m_posValid
        || std::abs(srcStartSec - m_posSec) > kContinuitySeconds) {
        if (!seekAndPrime(srcStartSec)) return 0;
    }

    std::size_t delivered = 0;
    while (delivered < frames) {
        std::size_t fifoFrames = m_fifo.size() / 2 - m_fifoOffsetFrames;
        if (fifoFrames == 0) {
            if (m_eof) break;
            if (!decodeMore()) break;   // EOF or fatal — short read
            fifoFrames = m_fifo.size() / 2 - m_fifoOffsetFrames;
            if (fifoFrames == 0) continue;
        }
        const std::size_t n = std::min(frames - delivered, fifoFrames);
        std::memcpy(dst + delivered * 2,
                    m_fifo.data() + m_fifoOffsetFrames * 2,
                    n * 2 * sizeof(float));
        delivered          += n;
        m_fifoOffsetFrames += n;
        m_fifoStartSec     += static_cast<double>(n) / kOutRate;
    }

    // Compact the FIFO so it doesn't grow across a long forward run.
    if (m_fifoOffsetFrames > 0) {
        m_fifo.erase(m_fifo.begin(),
                     m_fifo.begin()
                     + static_cast<std::ptrdiff_t>(m_fifoOffsetFrames * 2));
        m_fifoOffsetFrames = 0;
    }

    m_posSec   = srcStartSec + static_cast<double>(delivered) / kOutRate;
    m_posValid = true;
    return delivered;
}

} // namespace qcv
