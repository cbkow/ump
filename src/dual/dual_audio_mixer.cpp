#include "dual_audio_mixer.h"

#include "audio/audio_decoder.h"
#include "audio/i_audio_source.h"
#include "audio/multi_stream_audio_decoder.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <QtGlobal>

#if defined(Q_OS_MACOS) || defined(__APPLE__)
#include "audio/coreaudio_device.h"
#define QCV_DUAL_HAS_AUDIO_DEVICE 1
#elif defined(Q_OS_WIN)
#include "audio/wasapi_audio_device.h"
#define QCV_DUAL_HAS_AUDIO_DEVICE 1
#else
#define QCV_DUAL_HAS_AUDIO_DEVICE 0
#endif

#include <QtLogging>

#include <cmath>
#include <cstring>
#include <vector>

namespace qcv::dual {

namespace {
// Same probe as AudioPlayer's. Tiny duplication; factoring out into
// a shared helper would mean dragging libavformat into a header that
// many TUs include. Cheaper to keep two copies.
int probeAudioStreamCount(const QString &path)
{
    AVFormatContext *ctx = nullptr;
    const QByteArray pathUtf8 = path.toUtf8();
    if (avformat_open_input(&ctx, pathUtf8.constData(),
                              nullptr, nullptr) < 0) {
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
    return count;
}

// Caller-supplied hint (>=1) bypasses the file-open probe; anything
// less falls back to probing. Same intent as AudioPlayer::open's
// hint param — reuse the metadata extractor's already-completed
// find_stream_info pass so we don't pay a second one per playlist
// boundary cross.
//
// `>= 1` not `>= 0` — a hint of 0 means "I don't know" (the
// default-initialized value on MediaItem.video.audioStreamCount
// before the extractor populates it). Treating 0 as a confident
// "no audio" dispatches every multi-stream broadcast master into
// single-stream AudioDecoder, which collapses the meter row to
// 2 unlabeled bars. See AudioPlayer::open for the same fix.
std::unique_ptr<IAudioSource> makeDecoderForPath(const QString &path,
                                                   QObject *parent,
                                                   int streamCountHint)
{
    if (path.isEmpty()) return nullptr;
    const int streamCount = (streamCountHint >= 1)
                            ? streamCountHint
                            : probeAudioStreamCount(path);
    if (streamCount >= 2) {
        return std::make_unique<MultiStreamAudioDecoder>(parent);
    }
    return std::make_unique<AudioDecoder>(parent);
}
} // namespace

DualAudioMixer::DualAudioMixer(QObject *parent)
    : QObject(parent)
{
    // Decoders constructed lazily in open() once we know each side's
    // stream count (single-stream vs multi-stream pick is per-side).
#if defined(Q_OS_MACOS) || defined(__APPLE__)
    m_device = std::make_unique<CoreAudioDevice>();
#elif defined(Q_OS_WIN)
    m_device = std::make_unique<WasapiAudioDevice>();
#endif
}

DualAudioMixer::~DualAudioMixer() { shutdown(); }

bool DualAudioMixer::initialize()
{
    if (m_initialized) return true;
#if defined(Q_OS_MACOS) || defined(__APPLE__)
    CoreAudioDeviceConfig cfg;
    cfg.dataCallback = &DualAudioMixer::dataCallback;
    cfg.userData     = this;
    cfg.sampleRate   = 48000;
    cfg.channels     = 2;
    cfg.bufferSizeMs = 10;
    if (!m_device || !m_device->initialize(cfg)) {
        qWarning("DualAudioMixer: device init failed");
        return false;
    }
#elif defined(Q_OS_WIN)
    WasapiAudioDeviceConfig cfg;
    cfg.dataCallback = &DualAudioMixer::dataCallback;
    cfg.userData     = this;
    cfg.sampleRate   = 48000;
    cfg.channels     = 2;
    cfg.bufferSizeMs = 10;
    if (!m_device || !m_device->initialize(cfg)) {
        qWarning("DualAudioMixer: device init failed");
        return false;
    }
#endif
    m_initialized = true;
    return true;
}

void DualAudioMixer::shutdown()
{
    if (!m_initialized) return;
    pause();
    close();
#if QCV_DUAL_HAS_AUDIO_DEVICE
    if (m_device) m_device->shutdown();
#endif
    m_initialized = false;
}

bool DualAudioMixer::open(const QString &pathA, const QString &pathB,
                            int audioStreamCountHintA,
                            int audioStreamCountHintB)
{
    if (!m_initialized) {
        qWarning("DualAudioMixer::open called before initialize");
        return false;
    }
    close();

    // Pick the right decoder shape per-side. A could be a single-
    // stream stereo file while B is an 8-stream broadcast master
    // (or vice versa) — they're independent.
    m_decoderA = makeDecoderForPath(pathA, this, audioStreamCountHintA);
    m_decoderB = makeDecoderForPath(pathB, this, audioStreamCountHintB);

    bool any = false;
    if (m_decoderA && !pathA.isEmpty() && m_decoderA->open(pathA)) {
        m_decoderA->start();
        any = true;
    }
    if (m_decoderB && !pathB.isEmpty() && m_decoderB->open(pathB)) {
        m_decoderB->start();
        any = true;
    }
    emit hasAudioChanged();

    qInfo("DualAudioMixer: opened — A=%s, B=%s",
          (m_decoderA && m_decoderA->hasAudio() ? "yes" : "no"),
          (m_decoderB && m_decoderB->hasAudio() ? "yes" : "no"));
    // Degenerate success: both paths empty / both lacking audio
    // streams is fine — the mixer renders silence.
    return any || (pathA.isEmpty() && pathB.isEmpty());
}

void DualAudioMixer::close()
{
    if (m_playing.load()) pause();
    if (m_decoderA) m_decoderA->close();
    if (m_decoderB) m_decoderB->close();
    emit hasAudioChanged();
}

void DualAudioMixer::play()
{
    if (!hasAudioA() && !hasAudioB()) return;
    if (m_playing.exchange(true)) return;
#if QCV_DUAL_HAS_AUDIO_DEVICE
    if (m_device) m_device->start();
#endif
}

void DualAudioMixer::pause()
{
    if (!m_playing.exchange(false)) return;
#if QCV_DUAL_HAS_AUDIO_DEVICE
    if (m_device) m_device->stop();
#endif
}

void DualAudioMixer::seek(double seconds)
{
    seekPerSide(seconds, seconds);
}

void DualAudioMixer::seekPerSide(double sourceSecondsA,
                                   double sourceSecondsB)
{
    // Negative = "in a timeline gap"; mute that side and skip the
    // codec seek (the audio decoder has nowhere meaningful to go).
    // Sync offset shifts the audio decoder BEHIND the video clock by
    // offsetMs — positive offset → audio plays later, the direction
    // that compensates for video display + pipeline lag. The
    // m_lastSeekPos{A,B} atomics keep the un-offset source time so
    // the gap-edge re-seek logic in updatePerSide compares apples to
    // apples with the master clock the controller passes in.
    const double offsetSec =
        m_syncOffsetMs.load(std::memory_order_relaxed) / 1000.0;

    if (m_decoderA && m_decoderA->hasAudio()) {
        if (sourceSecondsA >= 0.0) {
            m_decoderA->seek(sourceSecondsA - offsetSec);
            m_lastSeekPosA.store(sourceSecondsA);
        }
        m_inGapA.store(sourceSecondsA < 0.0);
    } else {
        m_inGapA.store(true);
    }
    if (m_decoderB && m_decoderB->hasAudio()) {
        if (sourceSecondsB >= 0.0) {
            m_decoderB->seek(sourceSecondsB - offsetSec);
            m_lastSeekPosB.store(sourceSecondsB);
        }
        m_inGapB.store(sourceSecondsB < 0.0);
    } else {
        m_inGapB.store(true);
    }
}

void DualAudioMixer::setSyncOffsetMs(int ms)
{
    if (ms < -100) ms = -100;
    if (ms >  100) ms =  100;
    m_syncOffsetMs.store(ms, std::memory_order_relaxed);
}

void DualAudioMixer::updatePerSide(double sourceSecondsA,
                                     double sourceSecondsB)
{
    // Edit-aware sync. Called per pump tick by the controller.
    // Two responsibilities:
    //   1. Update gap flags so processAudio skips the right side
    //      when master is outside that track's clip range.
    //   2. Re-seek a side when it transitions gap → clip, so audio
    //      resumes at the new clip's source-in offset rather than
    //      where the gap left it.
    // Drift correction (long-running re-seek for clock skew) is
    // a follow-up — the decoders + device share a wall clock and
    // nominal drift in steady state is zero.
    constexpr double kSeekCooldownSeconds = 1.0;

    const double offsetSec =
        m_syncOffsetMs.load(std::memory_order_relaxed) / 1000.0;
    auto handle = [offsetSec](IAudioSource *dec, double newPos,
                                std::atomic<bool> &inGap,
                                std::atomic<double> &lastSeekPos) {
        if (!dec || !dec->hasAudio()) {
            inGap.store(true);
            return;
        }
        const bool wasInGap = inGap.load();
        const bool nowInGap = (newPos < 0.0);
        if (nowInGap != wasInGap) inGap.store(nowInGap);
        if (!nowInGap && wasInGap) {
            // Gap → clip transition. Seek the decoder to the new
            // clip's source position so playback resumes at the
            // right offset. Cooldown is short (we don't expect
            // back-to-back gap crossings).
            if (dec->secondsSinceLastSeek() > kSeekCooldownSeconds) {
                dec->seek(newPos - offsetSec);
                lastSeekPos.store(newPos);
            }
        }
    };
    handle(m_decoderA.get(), sourceSecondsA, m_inGapA, m_lastSeekPosA);
    handle(m_decoderB.get(), sourceSecondsB, m_inGapB, m_lastSeekPosB);
}

bool DualAudioMixer::hasAudioA() const
{
    return m_decoderA && m_decoderA->hasAudio();
}

bool DualAudioMixer::hasAudioB() const
{
    return m_decoderB && m_decoderB->hasAudio();
}

void DualAudioMixer::setMutedA(bool m)
{
    if (m_mutedA.exchange(m) == m) return;
    emit mutedAChanged();
}

void DualAudioMixer::setMutedB(bool m)
{
    if (m_mutedB.exchange(m) == m) return;
    emit mutedBChanged();
}

void DualAudioMixer::setRoutingModeA(int mode)
{
    if (m_decoderA) m_decoderA->setRoutingMode(mode);
}

void DualAudioMixer::setRoutingModeB(int mode)
{
    if (m_decoderB) m_decoderB->setRoutingMode(mode);
}

namespace {
QVariantList peaksFromDecoder(const IAudioSource *dec)
{
    QVariantList out;
    if (!dec) return out;
    const int nb = dec->sourceChannels();
    if (nb <= 0) return out;
    const auto peaks = dec->peakLevels();
    out.reserve(nb);
    for (int i = 0; i < nb && i < 16; ++i) out.append(peaks[i]);
    return out;
}
} // namespace

QVariantList DualAudioMixer::audioChannelPeaksA() const
{ return peaksFromDecoder(m_decoderA.get()); }
QVariantList DualAudioMixer::audioChannelPeaksB() const
{ return peaksFromDecoder(m_decoderB.get()); }
QStringList DualAudioMixer::audioChannelNamesA() const
{ return m_decoderA ? m_decoderA->sourceChannelNames() : QStringList(); }
QStringList DualAudioMixer::audioChannelNamesB() const
{ return m_decoderB ? m_decoderB->sourceChannelNames() : QStringList(); }

void DualAudioMixer::dataCallback(void * /*device*/, float *output,
                                    uint32_t frameCount, void *userData)
{
    auto *self = static_cast<DualAudioMixer *>(userData);
    self->processAudio(output, frameCount);
}

void DualAudioMixer::processAudio(float *output, uint32_t frameCount)
{
    const size_t samples = static_cast<size_t>(frameCount) * 2; // stereo
    std::memset(output, 0, samples * sizeof(float));

    if (!m_playing.load()) return;

    // Pull each side into a temp buffer, sum into output. Both
    // sides land at 48k stereo float32 (AudioDecoder's SwrContext
    // resamples on the decode thread), so direct accumulation works.
    static thread_local std::vector<float> sideBuf;
    if (sideBuf.size() < samples) sideBuf.resize(samples);

    auto pullAndMix = [&](IAudioSource *dec, bool muted, bool inGap) {
        if (!dec || !dec->hasAudio() || muted || inGap) return;
        std::memset(sideBuf.data(), 0, samples * sizeof(float));
        dec->read(sideBuf.data(), frameCount);
        for (size_t i = 0; i < samples; ++i) {
            output[i] += sideBuf[i];
        }
    };
    pullAndMix(m_decoderA.get(), m_mutedA.load(), m_inGapA.load());
    pullAndMix(m_decoderB.get(), m_mutedB.load(), m_inGapB.load());

    // Soft limit on the sum so two unity-gain sources mixing to
    // ±2.0 don't hard-clip the device. Same shape as
    // AudioPlayer::processAudio.
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
    for (size_t i = 0; i < samples; ++i) output[i] = softLimit(output[i]);
}

} // namespace qcv::dual
