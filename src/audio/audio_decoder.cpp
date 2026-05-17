#include "audio_decoder.h"

#include "project/media_item.h"   // AudioRoutingMode enum (values
                                   // mirrored by the int API on this
                                   // class; static_assert below catches
                                   // accidental drift).

#include <QtLogging>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <vector>

namespace qcv {

// Compile-time check that AudioDecoder's int routing API stays
// aligned with the canonical AudioRoutingMode enum on MediaItem. If
// someone reorders the enum, the resampler matrices below silently
// produce the wrong routing without this guard.
static_assert(static_cast<int>(AudioRoutingMode::Auto)       == 0,
              "AudioRoutingMode::Auto must be 0 — see audio_decoder.cpp");
static_assert(static_cast<int>(AudioRoutingMode::Downmix5_1) == 1,
              "AudioRoutingMode::Downmix5_1 must be 1");
static_assert(static_cast<int>(AudioRoutingMode::Stereo7_8)  == 2,
              "AudioRoutingMode::Stereo7_8 must be 2");

namespace {

// Build a 2×N output-from-input downmix matrix per routing mode +
// source channel count. Returns nullopt when no custom matrix is
// needed (FFmpeg's default swr behavior is correct: mono→stereo,
// stereo passthrough, 6ch 5.1→stereo BS.775 fold). When set, the
// matrix is row-major: [L_out_row..., R_out_row...].
std::optional<std::vector<double>>
computeRoutingMatrix(int mode, int srcChannels)
{
    if (srcChannels <= 0) return std::nullopt;

    auto pluck = [srcChannels](int leftCh, int rightCh)
        -> std::optional<std::vector<double>>
    {
        if (leftCh  >= srcChannels) leftCh  = std::min(0, srcChannels - 1);
        if (rightCh >= srcChannels) rightCh = std::min(1, srcChannels - 1);
        std::vector<double> m(2 * srcChannels, 0.0);
        m[leftCh]                  = 1.0;   // L_out = ch[leftCh]
        m[srcChannels + rightCh]   = 1.0;   // R_out = ch[rightCh]
        return m;
    };

    // ITU-R BS.775 5.1→stereo coefficients, applied to the first
    // 6 source channels (assumes broadcast layout: L R C LFE Ls Rs).
    // LFE deliberately dropped (not contributing to either output);
    // adding -10 dB LFE is a follow-up if reviewers ask.
    auto downmix5_1 = [srcChannels]()
        -> std::optional<std::vector<double>>
    {
        if (srcChannels < 6) return std::nullopt;
        std::vector<double> m(2 * srcChannels, 0.0);
        // L_out = L + 0.707·C + 0.707·Ls
        m[0]                = 1.0;     // L
        m[2]                = 0.707;   // C
        m[4]                = 0.707;   // Ls
        // R_out = R + 0.707·C + 0.707·Rs
        m[srcChannels + 1]  = 1.0;     // R
        m[srcChannels + 2]  = 0.707;   // C
        m[srcChannels + 5]  = 0.707;   // Rs
        return m;
    };

    const auto routing = static_cast<AudioRoutingMode>(mode);
    switch (routing) {
    case AudioRoutingMode::Downmix5_1:
        return downmix5_1();   // nullopt if source has <6 channels →
                                // falls through to default behavior.
    case AudioRoutingMode::Stereo7_8:
        if (srcChannels >= 8) return pluck(6, 7);
        return std::nullopt;   // UI gates this; defensive fallback.
    case AudioRoutingMode::Auto:
    default:
        // Auto: defer to FFmpeg's defaults for 1/2/6 ch (mono→stereo,
        // direct, BS.775 downmix). For 8ch, default to the broadcast
        // stereo bounce on channels 7-8. For other counts (3,4,5,7,
        // 9+), pluck the first two channels — safest fallback.
        if (srcChannels == 1 || srcChannels == 2 || srcChannels == 6) {
            return std::nullopt;
        }
        if (srcChannels >= 8) return pluck(6, 7);
        return pluck(0, 1);
    }
}

// Compute per-channel peak (|max sample|, normalized to [0..1])
// from a decoded AVFrame. Dispatches on the four sample formats
// commonly seen in broadcast deliverables (planar + interleaved
// float and int16); other formats leave peaks at their previous
// value — meter shows stale but audio still plays. Updates only
// indices [0..nb) where nb = min(srcChannels, 16).
template <typename SampleT>
inline float peakStrided(const SampleT *base, int stride, int n,
                          float invFullScale)
{
    SampleT peak = SampleT(0);
    for (int i = 0; i < n; ++i) {
        const SampleT s = base[i * stride];
        const SampleT a = s < SampleT(0) ? -s : s;
        if (a > peak) peak = a;
    }
    return static_cast<float>(peak) * invFullScale;
}

void computeFramePeaks(const AVFrame *frame,
                        int srcChannels,
                        std::array<std::atomic<float>, 16> &peaks)
{
    if (!frame || frame->nb_samples <= 0 || srcChannels <= 0) return;
    const int nbCh = std::min(srcChannels, 16);
    const int n    = frame->nb_samples;
    const auto fmt = static_cast<AVSampleFormat>(frame->format);

    auto store = [&](int ch, float v) {
        peaks[ch].store(v, std::memory_order_relaxed);
    };

    switch (fmt) {
    case AV_SAMPLE_FMT_FLT: {
        const float *base = reinterpret_cast<const float *>(frame->data[0]);
        for (int ch = 0; ch < nbCh; ++ch)
            store(ch, peakStrided<float>(base + ch, srcChannels, n, 1.0f));
        break;
    }
    case AV_SAMPLE_FMT_FLTP: {
        for (int ch = 0; ch < nbCh; ++ch) {
            const float *base = reinterpret_cast<const float *>(frame->data[ch]);
            store(ch, peakStrided<float>(base, 1, n, 1.0f));
        }
        break;
    }
    case AV_SAMPLE_FMT_S16: {
        const int16_t *base = reinterpret_cast<const int16_t *>(frame->data[0]);
        constexpr float kInv = 1.0f / 32768.0f;
        for (int ch = 0; ch < nbCh; ++ch)
            store(ch, peakStrided<int16_t>(base + ch, srcChannels, n, kInv));
        break;
    }
    case AV_SAMPLE_FMT_S16P: {
        constexpr float kInv = 1.0f / 32768.0f;
        for (int ch = 0; ch < nbCh; ++ch) {
            const int16_t *base = reinterpret_cast<const int16_t *>(frame->data[ch]);
            store(ch, peakStrided<int16_t>(base, 1, n, kInv));
        }
        break;
    }
    // FFmpeg unpacks pcm_s24le to S32 (24-bit value left-shifted into
    // the upper 24 bits of an int32). INT32_MAX as the divisor
    // normalizes both true 32-bit PCM and shifted 24-bit PCM to [0..1].
    // Broadcast deliverables almost universally hit this branch.
    case AV_SAMPLE_FMT_S32: {
        const int32_t *base = reinterpret_cast<const int32_t *>(frame->data[0]);
        constexpr float kInv = 1.0f / 2147483647.0f;
        for (int ch = 0; ch < nbCh; ++ch)
            store(ch, peakStrided<int32_t>(base + ch, srcChannels, n, kInv));
        break;
    }
    case AV_SAMPLE_FMT_S32P: {
        constexpr float kInv = 1.0f / 2147483647.0f;
        for (int ch = 0; ch < nbCh; ++ch) {
            const int32_t *base = reinterpret_cast<const int32_t *>(frame->data[ch]);
            store(ch, peakStrided<int32_t>(base, 1, n, kInv));
        }
        break;
    }
    default:
        // DBL / S64 / unknown — rare. Add cases here if a real source
        // turns up using them.
        break;
    }
}

} // namespace

AudioDecoder::AudioDecoder(QObject *parent)
    : QObject(parent)
{
    m_outputFormat.sampleRate     = 48000;
    m_outputFormat.channels       = 2;
    m_outputFormat.bytesPerSample = 4;
}

AudioDecoder::~AudioDecoder()
{
    close();
}

bool AudioDecoder::open(const QString &path)
{
    if (m_isOpen) close();
    m_path = path;
    if (!openAudioStream()) {
        qInfo("AudioDecoder: no audio stream in %s", qPrintable(path));
        m_hasAudio = false;
        return false;
    }
    m_hasAudio = true;
    m_ring = std::make_unique<AudioRingBuffer>();
    m_isOpen = true;
    qInfo("AudioDecoder: opened %s — %d Hz, %d ch, %.2fs",
          qPrintable(path), m_sourceSampleRate, m_sourceChannels, m_duration);
    return true;
}

void AudioDecoder::close()
{
    if (!m_isOpen) return;
    stop();
    closeAudioStream();
    m_ring.reset();
    m_isOpen = false;
    m_hasAudio = false;
    m_path.clear();
    m_duration = 0.0;
    m_sourceSampleRate = 0;
    m_sourceChannels = 0;
}

void AudioDecoder::start()
{
    if (!m_isOpen || m_running.load()) return;
    m_eofReached = false;
    m_running = true;
    m_decodeThread = std::thread(&AudioDecoder::decodeThreadFn, this);
}

void AudioDecoder::stop()
{
    if (!m_running.load()) return;
    m_running = false;
    m_seekCv.notify_all();
    if (m_decodeThread.joinable()) m_decodeThread.join();
}

void AudioDecoder::seek(double position)
{
    if (!m_isOpen) return;
    {
        std::lock_guard<std::mutex> lock(m_seekMutex);
        m_seekTarget = position;
        m_seekRequested = true;
    }
    m_seekCv.notify_one();
}

size_t AudioDecoder::read(float *output, size_t frameCount)
{
    if (!m_ring || !output) return 0;

    const size_t bytesRequested = frameCount * m_outputFormat.bytesPerFrame();
    const size_t bytesRead = m_ring->read(output, bytesRequested);
    const size_t framesRead = bytesRead / m_outputFormat.bytesPerFrame();

    // Pad underrun with silence so the audio device doesn't
    // produce noise in the unfilled tail.
    if (framesRead < frameCount) {
        const size_t silenceStart = framesRead * m_outputFormat.channels;
        const size_t silenceCount = (frameCount - framesRead)
                                    * m_outputFormat.channels;
        std::memset(output + silenceStart, 0, silenceCount * sizeof(float));
    }

    m_readPosition.store(
        m_readPosition.load()
        + static_cast<double>(framesRead) / m_outputFormat.sampleRate,
        std::memory_order_relaxed);

    return framesRead;
}

// ---- FFmpeg setup ----------------------------------------------------------

bool AudioDecoder::openAudioStream()
{
    m_formatCtx = avformat_alloc_context();
    if (avformat_open_input(&m_formatCtx, m_path.toUtf8().constData(),
                            nullptr, nullptr) < 0) {
        qWarning("AudioDecoder: avformat_open_input failed for %s",
                 qPrintable(m_path));
        return false;
    }
    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
        qWarning("AudioDecoder: avformat_find_stream_info failed");
        avformat_close_input(&m_formatCtx);
        return false;
    }

    m_audioStreamIdx = av_find_best_stream(
        m_formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (m_audioStreamIdx < 0) {
        avformat_close_input(&m_formatCtx);
        return false;
    }

    AVStream *stream = m_formatCtx->streams[m_audioStreamIdx];
    m_sourceChannels   = stream->codecpar->ch_layout.nb_channels;
    m_sourceSampleRate = stream->codecpar->sample_rate;
    // Cache the layout name (e.g. "5.1(side)", "stereo", "7.1") for
    // the inspector + per-channel meter labels. computeRoutingMatrix
    // doesn't need this — it only keys off channel count + the user's
    // mode — but the UI needs it to label the meter bars correctly.
    {
        char buf[64] = {0};
        if (av_channel_layout_describe(&stream->codecpar->ch_layout,
                                         buf, sizeof(buf)) > 0) {
            m_sourceChannelLayoutName = QString::fromUtf8(buf);
        }
    }

    if (m_formatCtx->duration > 0) {
        m_duration = static_cast<double>(m_formatCtx->duration) / AV_TIME_BASE;
    } else if (stream->duration > 0) {
        m_duration = static_cast<double>(stream->duration)
                   * av_q2d(stream->time_base);
    }
    if (stream->start_time != AV_NOPTS_VALUE) {
        m_streamStartTime = stream->start_time;
    }

    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        qWarning("AudioDecoder: no decoder for codec %d", stream->codecpar->codec_id);
        avformat_close_input(&m_formatCtx);
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx
        || avcodec_parameters_to_context(m_codecCtx, stream->codecpar) < 0
        || avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        qWarning("AudioDecoder: codec context setup failed");
        if (m_codecCtx) avcodec_free_context(&m_codecCtx);
        avformat_close_input(&m_formatCtx);
        return false;
    }

    if (!setupResampler()) {
        qWarning("AudioDecoder: resampler setup failed");
        avcodec_free_context(&m_codecCtx);
        avformat_close_input(&m_formatCtx);
        return false;
    }

    m_decodeFrame = av_frame_alloc();
    m_packet      = av_packet_alloc();
    if (!m_decodeFrame || !m_packet) {
        closeAudioStream();
        return false;
    }
    return true;
}

bool AudioDecoder::setupResampler()
{
    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
    // Deep copy the codec context's input layout rather than struct-
    // aliasing it. AVChannelLayout carries a u.map pointer for
    // AV_CHANNEL_ORDER_CUSTOM / AMBISONIC layouts; shallow copy
    // aliases that pointer, which is undefined if anything later
    // uninits one of the copies. swr_alloc_set_opts2 happens to deep-
    // copy internally so this was safe-by-accident, but the explicit
    // copy + uninit is what the FFmpeg API contract requires.
    AVChannelLayout inLayout{};
    if (av_channel_layout_copy(&inLayout, &m_codecCtx->ch_layout) < 0) {
        return false;
    }
    const int srcChannels = inLayout.nb_channels;

    if (swr_alloc_set_opts2(&m_swrCtx,
                            &outLayout, AV_SAMPLE_FMT_FLT,
                            m_outputFormat.sampleRate,
                            &inLayout, m_codecCtx->sample_fmt,
                            m_codecCtx->sample_rate,
                            0, nullptr) < 0) {
        av_channel_layout_uninit(&inLayout);
        return false;
    }

    // Per-routing-mode downmix matrix. nullopt means FFmpeg's default
    // behavior is correct (mono→stereo, stereo passthrough, BS.775
    // for 5.1 native source). When set, swr_set_matrix overrides the
    // implicit matrix; row-major 2 (output) × srcChannels (input).
    const int routingMode = m_routingMode.load();
    if (auto mtx = computeRoutingMatrix(routingMode, srcChannels)) {
        if (swr_set_matrix(m_swrCtx, mtx->data(), srcChannels) < 0) {
            qWarning("AudioDecoder: swr_set_matrix failed (mode=%d, ch=%d)",
                     routingMode, srcChannels);
            // Continue — falling back to default downmix is better than
            // refusing to play audio.
        }
    }

    // Modest headroom (~ -1 dB) so multi-track sums (Phase 5.x) and
    // soft-limiting later don't visibly clip on hot peaks. Same
    // value the old app used.
    av_opt_set_double(m_swrCtx, "rematrix_volume", 0.9, 0);

    if (swr_init(m_swrCtx) < 0) {
        swr_free(&m_swrCtx);
        m_swrCtx = nullptr;
        av_channel_layout_uninit(&inLayout);
        return false;
    }
    av_channel_layout_uninit(&inLayout);
    return true;
}

void AudioDecoder::setRoutingMode(int mode)
{
    if (mode < 0 || mode > 2) mode = 0;
    if (m_routingMode.exchange(mode) == mode) return;
    // Flag for the decode thread to teardown + re-init swr at its
    // next iteration boundary. No mutex needed — the swr handle is
    // touched only on the decode thread; we just flip a flag.
    m_routingDirty.store(true);
}

std::array<float, 16> AudioDecoder::peakLevels() const
{
    std::array<float, 16> out{};
    for (int i = 0; i < 16; ++i) {
        out[i] = m_peakPerChannel[i].load(std::memory_order_relaxed);
    }
    return out;
}

QStringList AudioDecoder::sourceChannelNames() const
{
    QStringList names;
    if (m_sourceChannels <= 0) return names;
    names.reserve(m_sourceChannels);
    // Use FFmpeg's per-channel naming when the source has a real
    // layout (returns "FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"
    // etc.). Fall back to numeric "1", "2", ... otherwise. Re-derive
    // from the codec ctx because m_codecCtx is alive for the
    // lifetime of an open decoder.
    //
    // Two fallback paths matter:
    //   1. ch_layout.order == AV_CHANNEL_ORDER_UNSPEC — the source
    //      knows channel COUNT but not channel identity. Common for
    //      bare pcm_s16le in MOV containers without a channel-layout
    //      atom. av_channel_layout_channel_from_index returns
    //      AV_CHAN_NONE; av_channel_name then writes the literal
    //      string "NONE" (rc > 0, buf[0] != 0). We have to check
    //      for AV_CHAN_NONE explicitly to avoid putting "NONE" labels
    //      under every meter bar.
    //   2. m_codecCtx not yet built — pre-open polling. Pure numeric.
    char buf[16] = {0};
    if (m_codecCtx) {
        for (int i = 0; i < m_sourceChannels; ++i) {
            const enum AVChannel ch = av_channel_layout_channel_from_index(
                &m_codecCtx->ch_layout, i);
            if (ch != AV_CHAN_NONE) {
                buf[0] = 0;
                const int rc = av_channel_name(buf, sizeof(buf), ch);
                if (rc > 0 && buf[0] != 0) {
                    names << QString::fromUtf8(buf);
                    continue;
                }
            }
            names << QString::number(i + 1);
        }
    } else {
        for (int i = 0; i < m_sourceChannels; ++i) {
            names << QString::number(i + 1);
        }
    }
    return names;
}

void AudioDecoder::cleanupResampler()
{
    if (m_swrCtx) {
        swr_free(&m_swrCtx);
        m_swrCtx = nullptr;
    }
}

void AudioDecoder::closeAudioStream()
{
    if (m_decodeFrame) av_frame_free(&m_decodeFrame);
    if (m_packet)      av_packet_free(&m_packet);
    cleanupResampler();
    if (m_codecCtx)    avcodec_free_context(&m_codecCtx);
    if (m_formatCtx)   avformat_close_input(&m_formatCtx);
    m_audioStreamIdx = -1;
}

// ---- Decode thread ---------------------------------------------------------

double AudioDecoder::secondsSinceLastSeek() const
{
    const double last = m_lastSeekTime.load();
    if (last <= 0.0) return 1e9;   // no seek yet — effectively "long ago"
    const double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return now - last;
}

void AudioDecoder::flushAndSeek(double position)
{
    if (!m_formatCtx || m_audioStreamIdx < 0) return;
    AVStream *stream = m_formatCtx->streams[m_audioStreamIdx];

    m_ring->clear();
    avcodec_flush_buffers(m_codecCtx);

    int64_t targetTs = static_cast<int64_t>(position / av_q2d(stream->time_base));
    if (m_streamStartTime != AV_NOPTS_VALUE && m_streamStartTime > 0) {
        targetTs += m_streamStartTime;
    }
    int rc = av_seek_frame(m_formatCtx, m_audioStreamIdx, targetTs,
                           AVSEEK_FLAG_BACKWARD);
    if (rc < 0) {
        // Fallback: seek from start. Rare on broken indexes.
        av_seek_frame(m_formatCtx, m_audioStreamIdx, 0, AVSEEK_FLAG_BACKWARD);
    }
    m_eofReached = false;
    m_decodePosition = position;
    m_readPosition   = position;
    m_lastSeekTime.store(std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count(),
        std::memory_order_relaxed);
}

void AudioDecoder::decodeThreadFn()
{
    while (m_running.load()) {
        // Pending routing-mode change. Cheap atomic check; rebuild
        // m_swrCtx only when the user actually clicked a different
        // pill. Done at the top of the iteration so the new matrix
        // is in place for whatever decodeNextPacket pulls next.
        // Audible artifact at most one decoded-frame's worth of
        // stale routing (~21 ms at 48 kHz / typical aac frame size).
        if (m_routingDirty.exchange(false)) {
            cleanupResampler();
            if (!setupResampler()) {
                qWarning("AudioDecoder: resampler re-init failed after "
                         "routing change; audio output will be silent until "
                         "the next file open");
            }
        }
        // Service pending seek.
        {
            std::unique_lock<std::mutex> lock(m_seekMutex);
            if (m_seekRequested.load()) {
                flushAndSeek(m_seekTarget);
                m_seekRequested = false;
            }
        }
        // Back-pressure: ring near-full.
        if (m_ring->availableWrite() < 8192) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        // EOF: idle until a seek wakes us up.
        if (m_eofReached.load()) {
            std::unique_lock<std::mutex> lock(m_seekMutex);
            m_seekCv.wait_for(lock, std::chrono::milliseconds(50),
                              [this] { return !m_running.load()
                                            || m_seekRequested.load(); });
            continue;
        }
        if (!decodeNextPacket()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

bool AudioDecoder::decodeNextPacket()
{
    if (!m_formatCtx || !m_codecCtx) return false;
    // TRACE_AUDIO_DECODE_FIRST — first packet decoded on the audio
    // decode thread. Brackets the swr+computeFramePeaks first run
    // so we can correlate against the playback-decoder timing for
    // the corruption window investigation.
    static thread_local bool s_loggedFirst = false;
    if (!s_loggedFirst) {
        qInfo("AudioDecoder: decodeThread first iteration");
        s_loggedFirst = true;
    }
    // A failed setupResampler() on a routing-mode change leaves
    // m_swrCtx null; swr_get_delay / swr_convert below do not
    // null-check the handle and would segfault on the decode thread.
    // Bail out of the iteration; the decoder stays silent until the
    // user picks another routing mode (the next setRoutingMode flips
    // m_routingDirty, decodeThreadFn retries setupResampler) or the
    // file is reopened.
    if (!m_swrCtx) return false;

    int rc = av_read_frame(m_formatCtx, m_packet);
    if (rc < 0) {
        if (rc == AVERROR_EOF) m_eofReached = true;
        return false;
    }
    if (m_packet->stream_index != m_audioStreamIdx) {
        av_packet_unref(m_packet);
        return true;
    }

    rc = avcodec_send_packet(m_codecCtx, m_packet);
    av_packet_unref(m_packet);
    if (rc < 0) return false;

    while (rc >= 0) {
        rc = avcodec_receive_frame(m_codecCtx, m_decodeFrame);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) return false;

        const int64_t delay = swr_get_delay(m_swrCtx, m_codecCtx->sample_rate);
        const int dstNbSamples = static_cast<int>(av_rescale_rnd(
            delay + m_decodeFrame->nb_samples,
            m_outputFormat.sampleRate, m_codecCtx->sample_rate, AV_ROUND_UP));

        const size_t outSize = dstNbSamples
                             * m_outputFormat.channels * sizeof(float);
        if (m_resampleBuffer.size() < outSize) m_resampleBuffer.resize(outSize);
        uint8_t *dst = m_resampleBuffer.data();

        // Per-channel peak tap for the inspector level meters.
        // Done BEFORE the SwrContext fold so we capture the real
        // source-channel signal (a 5.1 master's center channel,
        // an 8-ch deliverable's stereo bounce, etc.) — what gets
        // routed to L/R for playback is downstream of the user's
        // mode pick. Cheap (one max-abs sweep per ~21ms frame).
        computeFramePeaks(m_decodeFrame, m_sourceChannels,
                            m_peakPerChannel);

        const int converted = swr_convert(
            m_swrCtx, &dst, dstNbSamples,
            const_cast<const uint8_t **>(m_decodeFrame->data),
            m_decodeFrame->nb_samples);

        if (converted > 0) {
            const size_t bytesToWrite = static_cast<size_t>(converted)
                                      * m_outputFormat.channels * sizeof(float);
            // Block until the ring has space — codecs that decode
            // faster than realtime (e.g. raw WAV) overrun otherwise.
            size_t total = 0;
            const uint8_t *p = m_resampleBuffer.data();
            while (total < bytesToWrite
                   && m_running.load() && !m_seekRequested.load()) {
                const size_t n = m_ring->write(p + total, bytesToWrite - total);
                total += n;
                if (total < bytesToWrite) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }

            if (m_decodeFrame->pts != AV_NOPTS_VALUE) {
                AVStream *stream = m_formatCtx->streams[m_audioStreamIdx];
                const double pts = static_cast<double>(
                    m_decodeFrame->pts - m_streamStartTime)
                    * av_q2d(stream->time_base);
                m_decodePosition = pts;
            } else {
                m_decodePosition = m_decodePosition.load()
                    + static_cast<double>(converted) / m_outputFormat.sampleRate;
            }
        }
        av_frame_unref(m_decodeFrame);
    }
    return true;
}

} // namespace qcv
