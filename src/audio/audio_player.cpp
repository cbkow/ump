#include "audio_player.h"
#include "audio_decoder.h"
#include "multi_stream_audio_decoder.h"

#if defined(Q_OS_MACOS) || defined(__APPLE__)
#include "coreaudio_device.h"
#define QCV_HAS_AUDIO_DEVICE 1
#elif defined(Q_OS_WIN)
#include "wasapi_audio_device.h"
#define QCV_HAS_AUDIO_DEVICE 1
#else
#define QCV_HAS_AUDIO_DEVICE 0
#endif

extern "C" {
#include <libavformat/avformat.h>
}

#include <QFileInfo>
#include <QtGlobal>
#include <QtLogging>
#include <cmath>
#include <cstring>

namespace qcv {

namespace {

// Quick probe: how many audio streams does this file have? Returns
// 0 if the file can't be opened. AudioPlayer dispatches on the
// result — single-stream sources keep the well-tested AudioDecoder
// path; multi-stream sources go to MultiStreamAudioDecoder. Open
// + find_stream_info is the same expensive call AudioDecoder /
// MultiStreamAudioDecoder will make again on the chosen instance,
// but that's the cost of dispatching at the right layer (and the
// alternative — peeking inside the constructed decoder — would
// require both classes to expose half-open intermediate state).
int probeAudioStreamCount(const QString &path)
{
    // TRACE_AUDIO_PROBE — a third concurrent avformat_open_input on
    // the same file when MediaItem has no hint yet (cold File >
    // Open). Visible alongside FFmpegMetadataExtractor + the actual
    // audio/video decoders to confirm interleaving.
    const QString trimmedName = QFileInfo(path).fileName();
    qInfo("probeAudioStreamCount: begin '%s'", qPrintable(trimmedName));
    AVFormatContext *ctx = nullptr;
    const QByteArray pathUtf8 = path.toUtf8();
    if (avformat_open_input(&ctx, pathUtf8.constData(),
                              nullptr, nullptr) < 0) {
        qInfo("probeAudioStreamCount: end (open failed) '%s'",
              qPrintable(trimmedName));
        return 0;
    }
    int count = 0;
    if (avformat_find_stream_info(ctx, nullptr) >= 0) {
        for (unsigned i = 0; i < ctx->nb_streams; ++i) {
            const AVStream *s = ctx->streams[i];
            if (s && s->codecpar
                && s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                ++count;
            }
        }
    }
    avformat_close_input(&ctx);
    qInfo("probeAudioStreamCount: end '%s' (count=%d)",
          qPrintable(trimmedName), count);
    return count;
}

} // namespace

AudioPlayer::AudioPlayer(QObject *parent)
    : QObject(parent)
{
    // m_decoder is constructed lazily in open() once we know whether
    // the file needs the single- or multi-stream class.
#if defined(Q_OS_MACOS) || defined(__APPLE__)
    m_device = std::make_unique<CoreAudioDevice>();
#elif defined(Q_OS_WIN)
    m_device = std::make_unique<WasapiAudioDevice>();
#endif
}

AudioPlayer::~AudioPlayer() { shutdown(); }

bool AudioPlayer::initialize()
{
    if (m_initialized) return true;
#if defined(Q_OS_MACOS) || defined(__APPLE__)
    CoreAudioDeviceConfig cfg;
    cfg.dataCallback = &AudioPlayer::dataCallback;
    cfg.userData     = this;
    cfg.sampleRate   = 48000;
    cfg.channels     = 2;
    cfg.bufferSizeMs = 10;
    if (!m_device || !m_device->initialize(cfg)) {
        qWarning("AudioPlayer: device init failed");
        return false;
    }
#elif defined(Q_OS_WIN)
    WasapiAudioDeviceConfig cfg;
    cfg.dataCallback = &AudioPlayer::dataCallback;
    cfg.userData     = this;
    cfg.sampleRate   = 48000;
    cfg.channels     = 2;
    cfg.bufferSizeMs = 10;
    if (!m_device || !m_device->initialize(cfg)) {
        qWarning("AudioPlayer: device init failed");
        return false;
    }
#endif
    m_initialized = true;
    return true;
}

void AudioPlayer::shutdown()
{
    if (!m_initialized) return;
    stop();
    close();
#if QCV_HAS_AUDIO_DEVICE
    if (m_device) m_device->shutdown();
#endif
    m_initialized = false;
}

bool AudioPlayer::open(const QString &path, int audioStreamCountHint)
{
    if (!m_initialized) {
        qWarning("AudioPlayer::open called before initialize");
        return false;
    }
    close();
    // Pick the right decoder shape for this file's audio layout:
    //   - 0 or 1 audio streams → AudioDecoder (today's well-tested
    //     single-stream path; covers stereo MOVs, AAC files, etc.)
    //   - 2+ audio streams → MultiStreamAudioDecoder (libavfilter
    //     graph; covers broadcast multi-mono-track deliverables).
    // Dispatching here means AudioPlayer's transport / drift code
    // doesn't care which shape is in use.
    //
    // Prefer the caller-supplied hint over an in-process probe.
    // The metadata extractor already ran avformat_find_stream_info
    // on every bin item; reusing its result avoids a second
    // expensive index scan per playlist boundary cross on broadcast
    // multi-stream files. Probe is the cold-load fallback.
    //
    // `>= 1` not `>= 0` — a hint of 0 means "I don't know" (the
    // default-initialized value on MediaItem.video.audioStreamCount
    // before the extractor populates it, or on projects saved before
    // the field existed). Treating 0 as a confident "no audio"
    // dispatches every multi-stream broadcast master into single-
    // stream AudioDecoder, which picks ONE 2-channel bounce stream
    // via av_find_best_stream — losing the 5.1 / 7.1 layout and
    // collapsing the meter row to 2 unlabeled bars.
    const int streamCount = (audioStreamCountHint >= 1)
                            ? audioStreamCountHint
                            : probeAudioStreamCount(path);
    if (streamCount >= 2) {
        m_decoder = std::make_unique<MultiStreamAudioDecoder>(this);
    } else {
        m_decoder = std::make_unique<AudioDecoder>(this);
    }
    if (!m_decoder->open(path)) {
        emit hasAudioChanged();
        return false;
    }
    m_decoder->start();
    emit hasAudioChanged();
    return true;
}

void AudioPlayer::close()
{
    if (m_isPlaying.load()) stop();
    if (m_decoder) m_decoder->close();
    emit hasAudioChanged();
}

void AudioPlayer::play()
{
    if (!m_decoder || !m_decoder->hasAudio()) return;
    if (m_isPlaying.exchange(true)) return;
#if QCV_HAS_AUDIO_DEVICE
    if (m_device) {
        // Anchor for the playout-position calculation in update().
        m_playStartSamples.store(m_device->samplesWritten(),
                                 std::memory_order_relaxed);
        m_device->start();
    }
#endif
    emit isPlayingChanged();
}

void AudioPlayer::pause()
{
    if (!m_isPlaying.exchange(false)) return;
#if QCV_HAS_AUDIO_DEVICE
    if (m_device) m_device->stop();
#endif
    emit isPlayingChanged();
}

void AudioPlayer::stop() { pause(); }

void AudioPlayer::seek(double seconds)
{
    if (!m_decoder || !m_decoder->hasAudio()) return;

    // Apply the user's A/V-sync offset only at the decoder boundary:
    // tell the decoder to fetch samples for `seconds - offset` so the
    // audio CONTENT lags the video clock by offsetMs — the right
    // direction to compensate for video display + pipeline lag (the
    // user normally hears audio "ahead" of the visible image and
    // wants to delay it). Anchor m_playStartPos at `seconds` (the
    // video time) so update()'s drift math keeps comparing two
    // values in the same clock domain.
    const double offsetSec =
        m_syncOffsetMs.load(std::memory_order_relaxed) / 1000.0;
    m_decoder->seek(seconds - offsetSec);

    m_playStartPos.store(seconds, std::memory_order_relaxed);
#if QCV_HAS_AUDIO_DEVICE
    if (m_device) {
        m_playStartSamples.store(m_device->samplesWritten(),
                                 std::memory_order_relaxed);
    }
#endif
}

void AudioPlayer::setSyncOffsetMs(int ms)
{
    if (ms < -100) ms = -100;
    if (ms >  100) ms =  100;
    m_syncOffsetMs.store(ms, std::memory_order_relaxed);
    // Caller (WindowManager) decides whether to immediately re-seek;
    // we don't pull a position out of thin air here.
}

void AudioPlayer::setVolume(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    if (qFuzzyCompare(m_volume.load(), v)) return;
    m_volume.store(v);
    emit volumeChanged();
}

void AudioPlayer::setMuted(bool m)
{
    if (m_muted.exchange(m) == m) return;
    emit mutedChanged();
}

void AudioPlayer::setRoutingMode(int mode)
{
    if (m_decoder) m_decoder->setRoutingMode(mode);
}

QVariantList AudioPlayer::audioChannelPeaks() const
{
    QVariantList out;
    if (!m_decoder) return out;
    const int nb = m_decoder->sourceChannels();
    if (nb <= 0) return out;
    const auto peaks = m_decoder->peakLevels();
    out.reserve(nb);
    for (int i = 0; i < nb && i < 16; ++i) out.append(peaks[i]);
    return out;
}

QStringList AudioPlayer::audioChannelNames() const
{
    return m_decoder ? m_decoder->sourceChannelNames() : QStringList();
}

void AudioPlayer::update(double videoPositionSeconds)
{
    // Drift-correction loop, called per video frame from the
    // owning playback controller. Wall-clock master, audio chases
    // via codec seek when drift exceeds threshold AND the cooldown
    // has elapsed (codec seeks are destructive — flush ring + flush
    // codec state — and a too-frequent loop thrashes).
    //
    // Threshold of 150 ms is below human A/V-desync perception
    // (~80 ms) margin and above normal jitter (~30–60 ms decode
    // jitter + buffer latency). Same value the old app's AudioMixer
    // used. Cooldown of 1 s prevents back-to-back seeks while the
    // ring buffer refills.
    //
    // Discontinuity escape: loop wraps and external scrubs push
    // |drift| past 1 s instantly. In that regime the cooldown is
    // counter-productive — letting audio fall a full clip behind
    // before we correct is the gap the user hears as "pause until
    // the audio catches up." Above this threshold we re-seek
    // immediately regardless of cooldown.
    constexpr double kDriftThresholdSeconds = 0.150;
    constexpr double kSeekCooldownSeconds   = 1.0;
    constexpr double kDiscontinuitySeconds  = 1.0;

    if (!m_decoder || !m_decoder->hasAudio()
        || !m_isPlaying.load()) return;

#if QCV_HAS_AUDIO_DEVICE
    if (!m_device) return;
    const uint64_t samplesNow = m_device->samplesWritten();
    const uint64_t samplesAtStart = m_playStartSamples.load();
    const double elapsedAudio =
        (samplesNow >= samplesAtStart)
        ? static_cast<double>(samplesNow - samplesAtStart)
          / m_device->sampleRate()
        : 0.0;
    const double audioPos = m_playStartPos.load() + elapsedAudio;

    const double drift = videoPositionSeconds - audioPos;
    const double absDrift = std::abs(drift);
    const bool   discontinuity = absDrift > kDiscontinuitySeconds;
    if (absDrift > kDriftThresholdSeconds
        && (discontinuity
            || m_decoder->secondsSinceLastSeek() > kSeekCooldownSeconds)) {
        qInfo("AudioPlayer: drift %+0.0f ms — re-seeking audio to %.2fs%s",
              drift * 1000.0, videoPositionSeconds,
              discontinuity ? " (discontinuity, cooldown bypassed)" : "");
        seek(videoPositionSeconds);
    }
#else
    Q_UNUSED(videoPositionSeconds);
#endif
}

bool AudioPlayer::hasAudio() const
{
    return m_decoder && m_decoder->hasAudio();
}

double AudioPlayer::duration() const
{
    return m_decoder ? m_decoder->duration() : 0.0;
}

void AudioPlayer::dataCallback(void * /*device*/, float *output,
                                uint32_t frameCount, void *userData)
{
    auto *self = static_cast<AudioPlayer *>(userData);
    self->processAudio(output, frameCount);
}

void AudioPlayer::processAudio(float *output, uint32_t frameCount)
{
    if (!m_isPlaying.load()
        || m_muted.load()
        || !m_decoder
        || !m_decoder->hasAudio()) {
        std::memset(output, 0, frameCount * 2 * sizeof(float));
        return;
    }
    m_decoder->read(output, frameCount);

    // Soft limit: keep small signals linear, smoothly compress
    // anything above ±0.8 toward ±1.0 asymptote so multi-track
    // sums (Phase 5.x) and >1.0 user volumes don't hard-clip.
    // Same shape as the old app's AudioMixer / AudioPlayer.
    const float vol = m_volume.load();
    auto softLimit = [](float x) noexcept -> float {
        constexpr float threshold = 0.8f;
        if (x >  threshold) {
            const float excess = x - threshold;
            return  threshold + (1.0f - threshold) * (excess / (1.0f + excess));
        }
        if (x < -threshold) {
            const float excess = -x - threshold;
            return -threshold - (1.0f - threshold) * (excess / (1.0f + excess));
        }
        return x;
    };
    const size_t samples = static_cast<size_t>(frameCount) * 2; // stereo
    for (size_t i = 0; i < samples; ++i) {
        output[i] = softLimit(output[i] * vol);
    }
}

} // namespace qcv
