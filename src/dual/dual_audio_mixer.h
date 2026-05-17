// DualAudioMixer — Phase 7.7 Stage 6.
//
// Owns two AudioDecoder instances (one per dual side) feeding a
// single CoreAudioDevice via a per-frame mix in the render callback.
// Mirrors the qcv_audio/AudioPlayer pattern but with two decoders +
// independent mute / volume controls per side.
//
// Sample-rate matching is automatic: AudioDecoder always resamples
// to 48kHz / stereo / float32 (its SwrContext target), so a 44.1kHz
// reference track and a 48kHz video both land at the mixer's common
// sink rate without extra work here.
//
// Master-clock sync mirrors AudioPlayer::update — caller passes the
// dual master playhead in seconds each tick; per-side drift triggers
// a re-seek on each decoder independently when the threshold +
// cooldown allow.
//
// Lifetime: owned by DualPlaybackController. Open both paths up
// front (empty path = silent side); start on play(), stop on
// pause(), seek both decoders on seekToFrame.

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <atomic>
#include <memory>

namespace qcv {
class IAudioSource;
#if defined(Q_OS_MACOS) || defined(__APPLE__)
class CoreAudioDevice;
#elif defined(Q_OS_WIN)
class WasapiAudioDevice;
#endif
} // namespace qcv

namespace qcv::dual {

class DualAudioMixer : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasAudioA READ hasAudioA NOTIFY hasAudioChanged)
    Q_PROPERTY(bool hasAudioB READ hasAudioB NOTIFY hasAudioChanged)
    Q_PROPERTY(bool mutedA READ mutedA WRITE setMutedA NOTIFY mutedAChanged)
    Q_PROPERTY(bool mutedB READ mutedB WRITE setMutedB NOTIFY mutedBChanged)

public:
    explicit DualAudioMixer(QObject *parent = nullptr);
    ~DualAudioMixer() override;

    DualAudioMixer(const DualAudioMixer &)            = delete;
    DualAudioMixer &operator=(const DualAudioMixer &) = delete;

    // Initialize the audio device; idempotent. Returns true on
    // success. Must be called before open(). Pairs with shutdown().
    bool initialize();
    void shutdown();

    // Open A and B audio streams from the given paths. Empty path
    // leaves that side silent (no decoder spawned). Returns true if
    // at least one side opened or both paths were empty (degenerate
    // success — no audio but the mixer is "ready").
    //
    // The optional ...CountHint args, when >= 0, skip the per-side
    // dispatch probe (avformat_open_input + find_stream_info just
    // to count streams) and use the supplied value to pick
    // AudioDecoder (≤1) vs MultiStreamAudioDecoder (≥2). Caller
    // (WindowManager / DualPlaybackController) passes the cached
    // MediaItem.video.audioStreamCount once metadata has loaded;
    // -1 falls back to the in-process probe per side.
    bool open(const QString &pathA, const QString &pathB,
              int audioStreamCountHintA = -1,
              int audioStreamCountHintB = -1);
    void close();

    // Transport. play() spins up the decode threads; pause() halts
    // the device callback (ring buffers stay primed). seek(seconds)
    // re-seeks both decoders to the same position — used for the
    // simple no-edit case and as a fallback. seekPerSide is the
    // edit-aware path: caller passes already-translated source
    // seconds for each side, with -1 meaning "this side is in a
    // timeline gap" (mutes that side until master enters a clip).
    void play();
    void pause();
    void seek(double seconds);
    void seekPerSide(double sourceSecondsA, double sourceSecondsB);

    // Master-clock + edit-aware sync tick. Caller passes the per-
    // side translated source positions each pump iteration. We use
    // them to:
    //   - flip m_inGap{A,B} so processAudio mutes that side when
    //     the master is in a gap on it,
    //   - re-seek the decoder when transitioning gap → clip (so
    //     audio resumes at the correct source offset, not where
    //     the gap left off),
    //   - drift-correct on long-running playback (threshold +
    //     cooldown to avoid thrashing the codec).
    void updatePerSide(double sourceSecondsA, double sourceSecondsB);

    bool hasAudioA() const;
    bool hasAudioB() const;
    bool mutedA() const { return m_mutedA.load(); }
    bool mutedB() const { return m_mutedB.load(); }
    bool isPlaying() const { return m_playing.load(); }

    void setMutedA(bool m);
    void setMutedB(bool m);

    // A/V sync compensation. Same semantic as AudioPlayer::
    // setSyncOffsetMs — positive = audio plays LATER, applied at
    // each side's seek boundary as `seek(time - offset)`. One global
    // value for both sides; WindowManager owns the slider + per-OS
    // default + persistence.
    void setSyncOffsetMs(int ms);
    int  syncOffsetMs() const { return m_syncOffsetMs.load(); }

    // Per-side channel routing. Pass-through to the corresponding
    // AudioDecoder; safe mid-playback. Each side's MediaItem carries
    // its own AudioRoutingMode (per the videoRangeOverride pattern),
    // so A and B can be on different routing modes simultaneously.
    void setRoutingModeA(int mode);
    void setRoutingModeB(int mode);

    // Per-side per-source-channel peaks + names. Empty when that
    // side has no audio. WindowManager's 30 Hz pump drains these
    // into Q_PROPERTYs the inspector binds to.
    QVariantList audioChannelPeaksA() const;
    QVariantList audioChannelPeaksB() const;
    QStringList  audioChannelNamesA() const;
    QStringList  audioChannelNamesB() const;

signals:
    void hasAudioChanged();
    void mutedAChanged();
    void mutedBChanged();

private:
    static void dataCallback(void *device, float *output,
                              uint32_t frameCount, void *userData);
    void processAudio(float *output, uint32_t frameCount);

    bool m_initialized = false;
    // IAudioSource so each side independently dispatches to single-
    // (AudioDecoder) or multi-stream (MultiStreamAudioDecoder) at
    // open time based on the file's audio-stream count.
    std::unique_ptr<IAudioSource> m_decoderA;
    std::unique_ptr<IAudioSource> m_decoderB;

#if defined(Q_OS_MACOS) || defined(__APPLE__)
    std::unique_ptr<CoreAudioDevice> m_device;
#elif defined(Q_OS_WIN)
    std::unique_ptr<WasapiAudioDevice> m_device;
#endif

    std::atomic<bool>  m_playing{false};
    std::atomic<bool>  m_mutedA{false};
    // B starts muted by default — in a dual SBS comparison, the
    // common case is "audio reference is A, B is the variant being
    // judged visually" and stacking both audio streams is rarely
    // useful. User unmutes B explicitly when they want to hear it.
    std::atomic<bool>  m_mutedB{true};
    // Gap state — set true by updatePerSide when the master is
    // outside any clip on that track. processAudio skips mixing
    // contributions for sides flagged in a gap.
    std::atomic<bool>  m_inGapA{false};
    std::atomic<bool>  m_inGapB{false};
    // Last per-side seek time + position, used by updatePerSide
    // for drift detection and gap→clip re-seek.
    std::atomic<double> m_lastSeekPosA{-1.0};
    std::atomic<double> m_lastSeekPosB{-1.0};

    // Single A/V-sync offset shared by both sides (matches the
    // global slider). Applied internally at every seek call site.
    std::atomic<int>    m_syncOffsetMs{0};
};

} // namespace qcv::dual
