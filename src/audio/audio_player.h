// AudioPlayer — Phase 5.0.a, single-clip audio playback.
//
// Owns one AudioDecoder + one platform audio device. The render
// callback drains the decoder's ring buffer into the device output
// buffer. Phase 5.0.a is the simplest path — no master-clock sync,
// no gap detection, no volume soft-limit. The decode thread runs
// free; the device runs free; the audio plays at its own rate as
// fast as the codec produces it.
//
// Phase 5.0.b adds the master-clock sync: the video clock owns
// time, AudioPlayer::update() polls drift and seeks the decoder
// when drift > threshold. Volume + mute land there too.

#pragma once

#include "i_audio_source.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <atomic>
#include <memory>

namespace qcv {

#if defined(Q_OS_MACOS) || defined(__APPLE__)
class CoreAudioDevice;
#elif defined(Q_OS_WIN)
class WasapiAudioDevice;
#endif

class AudioPlayer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasAudio READ hasAudio NOTIFY hasAudioChanged)
    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY isPlayingChanged)
    // Linear gain in [0,1]. Soft-limited in the render callback so
    // a sum of multi-track sources (Phase 5.x) doesn't clip.
    Q_PROPERTY(float volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool  muted  READ muted  WRITE setMuted  NOTIFY mutedChanged)

public:
    explicit AudioPlayer(QObject *parent = nullptr);
    ~AudioPlayer() override;

    AudioPlayer(const AudioPlayer &) = delete;
    AudioPlayer &operator=(const AudioPlayer &) = delete;

    bool initialize();
    void shutdown();

    // Opens the file's audio. `audioStreamCountHint`, when >= 0,
    // skips the dispatch probe (avformat_open_input +
    // find_stream_info just to count streams) and uses the supplied
    // value to pick AudioDecoder (≤1) vs MultiStreamAudioDecoder
    // (≥2). WindowManager passes the cached
    // MediaItem.video.audioStreamCount once metadata has loaded;
    // -1 falls back to the in-process probe (cold-load case).
    bool open(const QString &path, int audioStreamCountHint = -1);
    void close();

    void play();
    void pause();
    void stop();

    // Seek the audio decoder to `seconds`. Phase 5.0.a wires this
    // to VideoDecoder so scrub + play-after-scrub align audio with
    // video. Master-clock drift correction (continuous re-seek
    // during playback) is Phase 5.0.b.
    void seek(double seconds);

    bool   hasAudio()  const;
    bool   isPlaying() const { return m_isPlaying.load(); }
    float  volume()    const { return m_volume.load(); }
    bool   muted()     const { return m_muted.load(); }
    // Audio file duration in seconds. 0.0 when no file is open. Read
    // from AudioDecoder, which pulls from the FFmpeg container's
    // duration field at open() time. Authoritative for audio-only
    // files where VideoDecoder reports 0 (no video stream → no
    // fps × frameCount duration).
    double duration() const;

    void setVolume(float v);
    void setMuted(bool m);

    // A/V sync compensation. Positive offset DELAYS audio relative
    // to video (read sample for time T-offset when video is at T) —
    // the right direction to compensate for video pipeline + display
    // lag, which is why audio normally sounds "ahead" of the visible
    // image. Negative offset advances audio (uncommon — use case is
    // an audio-side delay we want to compensate for). WindowManager
    // owns the user-facing slider + per-OS default + QSettings
    // persistence; this is just the live knob. Re-anchors on the
    // next seek; call seek(currentVideoPos) yourself if you want
    // immediate effect.
    void setSyncOffsetMs(int ms);
    int  syncOffsetMs() const { return m_syncOffsetMs.load(); }

    // Per-clip channel routing. Pass-through to the decoder; safe to
    // call mid-playback (decoder does the SwrContext rebuild on its
    // own thread). `mode` matches qcv::AudioRoutingMode (0 = Auto,
    // 1 = Downmix5_1, 2 = Stereo7_8). WindowManager calls this both
    // on initial open (with the MediaItem's stored mode) and when
    // the inspector pill emits audioRoutingModeChanged.
    void setRoutingMode(int mode);

    // Per-source-channel peak levels of the most-recently-decoded
    // frame, pre-fold. Empty when no audio is open. Driven by the
    // 30 Hz audio meter pump in WindowManager.
    QVariantList audioChannelPeaks() const;
    QStringList  audioChannelNames() const;

    // Drift-correction tick. Caller passes the master clock's
    // current position in seconds (the wall-clock-driven video
    // clock). If audio's playout position diverges by more than
    // the drift threshold AND a seek-cooldown has elapsed,
    // re-seeks the audio decoder to videoPos. No-op otherwise so
    // the codec stays pristine between corrections.
    void update(double videoPositionSeconds);

signals:
    void hasAudioChanged();
    void isPlayingChanged();
    void volumeChanged();
    void mutedChanged();

private:
    static void dataCallback(void *device, float *output,
                             uint32_t frameCount, void *userData);
    void processAudio(float *output, uint32_t frameCount);

    bool                              m_initialized = false;
    // IAudioSource so the same player drives single-stream
    // (AudioDecoder) and multi-stream (MultiStreamAudioDecoder)
    // sources interchangeably. Concrete instance picked at open()
    // time based on the file's audio-stream count.
    std::unique_ptr<IAudioSource>     m_decoder;
#if defined(Q_OS_MACOS) || defined(__APPLE__)
    std::unique_ptr<CoreAudioDevice>  m_device;
#elif defined(Q_OS_WIN)
    std::unique_ptr<WasapiAudioDevice> m_device;
#endif
    std::atomic<bool>                 m_isPlaying{false};
    std::atomic<float>                m_volume{1.0f};
    std::atomic<bool>                 m_muted{false};

    // Audio's playout position is reconstructed from
    //   playStartPos + (samplesAtPlayStart → samplesNow) / sampleRate
    // playStartPos snapshots the seek target at the moment play()
    // started, so the math survives subsequent seeks (each seek
    // resets both anchors).
    std::atomic<double>               m_playStartPos{0.0};
    std::atomic<uint64_t>             m_playStartSamples{0};

    // A/V sync offset in milliseconds. Applied at seek() — the
    // decoder is told to fetch samples for `videoTime - offsetMs/1000`
    // while m_playStartPos stays anchored at videoTime so the drift
    // loop in update() compares against the user-visible video clock
    // unmodified. End result: audio plays `offsetMs` later than video
    // (positive offset = compensates for video display + pipeline lag).
    std::atomic<int>                  m_syncOffsetMs{0};
};

} // namespace qcv
