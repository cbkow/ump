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
#if QCV_HAS_AUDIO_DEVICE
    // Servo scratch: sized for the device's real worst-case callback
    // (WASAPI's first fill can request the whole buffer) at the max
    // consumption ratio, plus interpolation margin. Never resized in
    // the render callback.
    if (m_device) {
        const std::size_t maxFrames =
            static_cast<std::size_t>(m_device->bufferFrameCount()) + 16;
        m_servoScratch.assign(maxFrames * 2 * 2, 0.0f);
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
    // No re-anchoring here: the playout-position anchor is owned by
    // seek() alone (consumption simply pauses with the device while
    // stopped, so the estimate stays valid across pause/play), and
    // WindowManager seeks before resuming playback anyway.
    if (m_device) m_device->start();
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

    // Apply the user's A/V-sync offset at the decoder boundary: fetch
    // samples for `seconds - offset` so the audio CONTENT lags the
    // video clock by offsetMs — the right direction to compensate for
    // video display + pipeline lag. The anchor lands in the same
    // shifted (source-seconds) domain; update() compares against
    // `videoPos - offset` so both sides of the drift subtraction stay
    // in one clock domain.
    //
    // Order matters: the decoder seek FIRST (it raises seekPending,
    // which makes the render callback stop consuming pre-flush
    // frames), THEN the anchor/counter reset. A callback already
    // in flight when this runs can attribute at most one buffer
    // (~10 ms) of pre-seek consumption to the new anchor — the servo
    // absorbs that.
    const double offsetSec =
        m_syncOffsetMs.load(std::memory_order_relaxed) / 1000.0;
    m_decoder->seek(seconds - offsetSec);

    m_anchorSrcSec = seconds - offsetSec;
    m_srcFramesConsumed.store(0, std::memory_order_relaxed);
    m_servo.reset();
    m_servoRatio.store(1.0f, std::memory_order_relaxed);
    m_lastServoUpdateValid = false;
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

void AudioPlayer::setPlaybackTempo(double tempo)
{
    if (m_decoder) m_decoder->setTempo(tempo);
}

double AudioPlayer::playbackTempo() const
{
    return m_decoder ? m_decoder->tempo() : 1.0;
}

int AudioPlayer::routingMode() const
{
    return m_decoder ? m_decoder->routingMode() : 0;
}

void AudioPlayer::beginShuttle(const QString &path, double srcSec,
                               double signedSpeed, int routingMode)
{
    if (!m_initialized) return;
    if (!m_shuttle) m_shuttle = std::make_unique<ShuttleAudioEngine>();
    m_shuttle->begin(path, srcSec, signedSpeed, routingMode);
#if QCV_HAS_AUDIO_DEVICE
    // Normal playback is paused during the gesture (device stopped);
    // the grain ring needs the callback running.
    if (m_device) m_device->start();
#endif
}

void AudioPlayer::shuttleTarget(const QString &path, double srcSec,
                                double signedSpeed)
{
    if (m_shuttle) m_shuttle->updateTarget(path, srcSec, signedSpeed);
}

void AudioPlayer::endShuttle()
{
    if (m_shuttle) m_shuttle->end();
#if QCV_HAS_AUDIO_DEVICE
    // Keep the device only if normal playback is running (the commit
    // seek + play() at gesture release restarts it otherwise).
    if (m_device && !m_isPlaying.load()) m_device->stop();
#endif
}

bool AudioPlayer::shuttleActive() const
{
    return m_shuttle && m_shuttle->active();
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
    // Continuous sync servo, called per video frame (video mode) or
    // from the ~30 Hz pump (other modes). Three tiers by |drift|:
    //
    //   <= 40 ms  servo band — trim the render callback's consumption
    //             ratio by up to ±0.2 % (inaudible) via the PI
    //             controller; drift converges to ~0 with no seeks.
    //   40 ms..1s soft re-seek with the 1 s cooldown (the old
    //             correction path — now rare; something external
    //             pushed audio well off the clock).
    //   > 1 s     discontinuity (loop wrap, external scrub): re-seek
    //             immediately, cooldown bypassed. Letting audio trail
    //             a full clip behind is the gap the user hears as
    //             "pause until the audio catches up."
    constexpr double kServoBandSeconds     = 0.040;
    constexpr double kSeekCooldownSeconds  = 1.0;
    constexpr double kDiscontinuitySeconds = 1.0;

    if (!m_decoder || !m_decoder->hasAudio()
        || !m_isPlaying.load()) return;

#if QCV_HAS_AUDIO_DEVICE
    if (!m_device) return;

    // A decoder seek is still in flight — the consumption counter is
    // frozen (callback outputs silence) and the estimate below would
    // be garbage. Wait for the flush to complete.
    if (m_decoder->seekPending()) {
        m_lastServoUpdateValid = false;
        return;
    }

    const double offsetSec =
        m_syncOffsetMs.load(std::memory_order_relaxed) / 1000.0;
    const double target = videoPositionSeconds - offsetSec;

    // EOF tail: past the end of the audio stream there is nothing to
    // consume — the position estimate freezes while the video clock
    // runs on, and correcting would just thrash seeks against EOF.
    // Freeze the servo instead; the next real seek (loop wrap, user
    // action) re-engages it.
    const double dur = m_decoder->duration();
    if (dur > 0.0 && target > dur - 0.050) {
        m_servoRatio.store(1.0f, std::memory_order_relaxed);
        m_lastServoUpdateValid = false;
        return;
    }

    // Playout position in source seconds: consumption at the ring
    // drain, minus what the device has buffered but not yet played.
    // Ring frames are output-domain; ×tempo maps them back to source
    // seconds (the tempo stage consumes `tempo` source seconds per
    // output second). Tempo changes re-anchor via seek, so the value
    // is constant within an anchor epoch.
    const int sampleRate = m_device->sampleRate();
    const double tempo = m_decoder->tempo();
    const double ratioNow =
        static_cast<double>(m_servoRatio.load(std::memory_order_relaxed));
    const double srcSecConsumed =
        static_cast<double>(m_srcFramesConsumed.load(std::memory_order_relaxed))
        / static_cast<double>(sampleRate) * tempo;
    const double latencySec =
        m_device->bufferLatencySeconds() * ratioNow * tempo;
    const double audioSrcPos = m_anchorSrcSec + srcSecConsumed - latencySec;

    const double drift    = target - audioSrcPos;
    const double absDrift = std::abs(drift);

    if (absDrift > kServoBandSeconds) {
        const bool discontinuity = absDrift > kDiscontinuitySeconds;
        if (discontinuity
            || m_decoder->secondsSinceLastSeek() > kSeekCooldownSeconds) {
            qInfo("AudioPlayer: drift %+0.0f ms — re-seeking audio to "
                  "%.2fs%s",
                  drift * 1000.0, videoPositionSeconds,
                  discontinuity ? " (discontinuity, cooldown bypassed)" : "");
            seek(videoPositionSeconds);
        }
        return;
    }

    // Servo band: dt-aware PI update, ratio published to the render
    // callback.
    const auto now = std::chrono::steady_clock::now();
    double dt = 0.0;
    if (m_lastServoUpdateValid) {
        dt = std::chrono::duration<double>(now - m_lastServoUpdate).count();
    }
    m_lastServoUpdate      = now;
    m_lastServoUpdateValid = true;

    const double ratio = m_servo.update(drift, dt);
    m_servoRatio.store(static_cast<float>(ratio), std::memory_order_relaxed);

    // Convergence trace, ~every 10 s at the 30 Hz pump cadence.
    // Expected steady state: |drift| < 5 ms, ratio within ±0.0005 of
    // 1. Cheap enough to keep in release builds for field diagnosis.
    if (++m_servoLogCounter >= 300) {
        m_servoLogCounter = 0;
        qInfo("AudioPlayer: servo drift %+.1f ms  ratio %.5f",
              drift * 1000.0, ratio);
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
    const size_t outBytes = static_cast<size_t>(frameCount) * 2
                            * sizeof(float);
    // Shuttle mode preempts normal playback (which the gesture
    // paused): drain the grain ring, then fall through to the shared
    // mute/volume/soft-limit tail below.
    if (m_shuttle && m_shuttle->active()) {
        m_shuttle->read(output, frameCount);
        if (m_muted.load()) {
            std::memset(output, 0, outBytes);
            return;
        }
        const float vol = m_volume.load();
        const size_t n = static_cast<size_t>(frameCount) * 2;
        for (size_t i = 0; i < n; ++i) output[i] *= vol;
        return;
    }
    if (!m_isPlaying.load() || !m_decoder || !m_decoder->hasAudio()) {
        std::memset(output, 0, outBytes);
        return;
    }
    // Decoder seek in flight: the ring holds pre-seek audio about to
    // be flushed. Output silence WITHOUT consuming so pre-flush
    // frames never count against the fresh seek anchor.
    if (m_decoder->seekPending()) {
        std::memset(output, 0, outBytes);
        return;
    }

    // Drain through the servo resampler at the ratio update()
    // published. Ratio 1.0 ± 0.2 % — srcNeeded ≈ frameCount.
    const double ratio =
        static_cast<double>(m_servoRatio.load(std::memory_order_relaxed));
    const size_t srcNeeded =
        m_servoResampler.sourceFramesNeeded(frameCount, ratio);
    if (srcNeeded * 2 > m_servoScratch.size()) {
        // Callback larger than the scratch sized at initialize() —
        // shouldn't happen; silence beats allocating on the RT thread.
        std::memset(output, 0, outBytes);
        return;
    }
    // read() pads underrun with silence; count only REAL frames —
    // the stream doesn't advance for the padded region, and the
    // position estimate must not either.
    const size_t framesRead =
        m_decoder->read(m_servoScratch.data(), srcNeeded);
    m_servoResampler.process(m_servoScratch.data(), srcNeeded,
                             output, frameCount, ratio);
    m_srcFramesConsumed.fetch_add(framesRead, std::memory_order_relaxed);

    // Muted AFTER consuming: the stream keeps advancing with the
    // clock, so unmute plays current audio, not a stale buffer.
    if (m_muted.load()) {
        std::memset(output, 0, outBytes);
        return;
    }

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
