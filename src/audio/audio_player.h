// AudioPlayer — single-clip audio playback.
//
// Owns one AudioDecoder (or MultiStreamAudioDecoder) + one platform
// audio device. The render callback drains the decoder's ring buffer
// into the device output buffer through a fractional-rate resampler.
//
// Master-clock sync is a continuous servo: update() measures drift
// between the master clock and the audio playout position (recon-
// structed from source frames actually consumed, minus device
// latency) and trims the consumption ratio by up to ±0.2 % — an
// inaudible pitch change — so drift converges to ~0 with no seeks.
// Hard decoder re-seeks remain only as escape tiers: a soft re-seek
// when drift leaves the servo band (rare) and an immediate one for
// discontinuities > 1 s (loop wraps, external scrubs).

#pragma once

#include "audio_sync_servo.h"
#include "fractional_resampler.h"
#include "i_audio_source.h"
#include "shuttle_audio_engine.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

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
    // persistence; this is just the live knob. The sync servo tracks
    // the shifted target continuously; for slider jumps beyond the
    // servo band the soft re-seek tier converges it, or call
    // seek(currentVideoPos) yourself for immediate effect.
    void setSyncOffsetMs(int ms);
    int  syncOffsetMs() const { return m_syncOffsetMs.load(); }

    // Per-clip channel routing. Pass-through to the decoder; safe to
    // call mid-playback (decoder does the SwrContext rebuild on its
    // own thread). `mode` matches qcv::AudioRoutingMode (0 = Auto,
    // 1 = Downmix5_1, 2 = Stereo7_8). WindowManager calls this both
    // on initial open (with the MediaItem's stored mode) and when
    // the inspector pill emits audioRoutingModeChanged.
    void setRoutingMode(int mode);

    // Constant-pitch playback tempo (review speeds, WSOLA in the
    // decoder's TempoStage). 1.0 = bypass. Caller (WindowManager)
    // re-anchors with seek(currentMasterPos) right after a change so
    // old-tempo ring residue is dropped and the servo restarts clean.
    void   setPlaybackTempo(double tempo);
    double playbackTempo() const;

    // Current decoder routing mode (for handing to the shuttle
    // engine's grain reader so shuttle honors the clip's fold).
    int routingMode() const;

    // ---- Shuttle (FF/RW hold gesture) ----
    // Grain-based varispeed audio following the fast-seek position;
    // see ShuttleAudioEngine. begin starts the grain thread (and the
    // device, which normal pause() may have stopped); shuttleTarget
    // is the per-gesture-tick update (signedSpeed < 0 = rewind);
    // endShuttle joins the thread and stops the device again unless
    // normal playback is running. While active, processAudio drains
    // the grain ring INSTEAD of the main decoder (whose decode thread
    // idles on back-pressure, ready for the commit seek at release).
    void beginShuttle(const QString &path, double srcSec,
                      double signedSpeed, int routingMode);
    void shuttleTarget(const QString &path, double srcSec,
                       double signedSpeed);
    void endShuttle();
    bool shuttleActive() const;

    // Per-source-channel peak levels of the most-recently-decoded
    // frame, pre-fold. Empty when no audio is open. Driven by the
    // 30 Hz audio meter pump in WindowManager.
    QVariantList audioChannelPeaks() const;
    QStringList  audioChannelNames() const;

    // Sync tick. Caller passes the master clock's current position
    // in seconds. Runs the drift servo (see header comment): inside
    // the servo band the consumption ratio is trimmed; outside it,
    // the decoder is re-seeked (soft tier with cooldown, immediate
    // for > 1 s discontinuities). Call cadence is irregular (per
    // video frame in video mode, ~30 Hz pump otherwise) — the servo
    // is dt-aware.
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
    // Grain-based shuttle audio (constructed lazily on first
    // gesture). Read by the render callback via active() + read().
    std::unique_ptr<ShuttleAudioEngine> m_shuttle;
#if defined(Q_OS_MACOS) || defined(__APPLE__)
    std::unique_ptr<CoreAudioDevice>  m_device;
#elif defined(Q_OS_WIN)
    std::unique_ptr<WasapiAudioDevice> m_device;
#endif
    std::atomic<bool>                 m_isPlaying{false};
    std::atomic<float>                m_volume{1.0f};
    std::atomic<bool>                 m_muted{false};

    // ---- Servo state ----
    // Audio playout position is reconstructed in SOURCE seconds:
    //   audioSrcPos = m_anchorSrcSec
    //               + srcFramesConsumed / sampleRate
    //               - device bufferLatencySeconds() × ratio
    // The anchor is owned solely by seek() (play() does not re-anchor;
    // WindowManager always seeks before resuming). Consumption counts
    // REAL ring frames drained by the render callback — silence
    // padding on underrun does not advance it, and while a decoder
    // seek is pending the callback outputs silence without consuming,
    // so pre-flush frames never count against a fresh anchor.
    double                            m_anchorSrcSec = 0.0;   // UI thread
    std::atomic<uint64_t>             m_srcFramesConsumed{0};

    // Controller runs on the UI thread in update(); the resulting
    // ratio crosses to the render callback through this atomic.
    AudioSyncServo                    m_servo;                // UI thread
    std::atomic<float>                m_servoRatio{1.0f};

    // Render-callback-only: fractional resampler + its source scratch
    // (sized once in initialize() from the device's real buffer frame
    // count — WASAPI can request the full buffer in one callback).
    FractionalResampler               m_servoResampler;
    std::vector<float>                m_servoScratch;

    // dt source for the servo's PI terms (update cadence is
    // irregular). UI thread only.
    std::chrono::steady_clock::time_point m_lastServoUpdate{};
    bool                              m_lastServoUpdateValid = false;
    int                               m_servoLogCounter = 0;

    // A/V sync offset in milliseconds. Applied at seek() — the
    // decoder is told to fetch samples for `videoTime - offsetMs/1000`
    // and the anchor lands in the same shifted domain; update()
    // compares against `videoTime - offset` so the servo holds the
    // offset continuously (a slider change shifts the target without
    // waiting for the next seek). End result: audio plays `offsetMs`
    // later than video (positive offset = compensates for video
    // display + pipeline lag).
    std::atomic<int>                  m_syncOffsetMs{0};
};

} // namespace qcv
