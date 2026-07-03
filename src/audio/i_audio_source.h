// IAudioSource — abstract interface implemented by the two audio
// decoder shapes the app supports:
//
//   - AudioDecoder           : single-stream (one AVCodecContext +
//                              SwrContext). Today's stable path.
//   - MultiStreamAudioDecoder : multi-mono-stream broadcast deliverables
//                              (libavfilter graph: amerge + pan +
//                              aformat). Sibling class added for v2.
//
// AudioPlayer / DualAudioMixer hold an IAudioSource * and dispatch on
// the file's audio-stream count at open time. Single-stream files keep
// the well-tested AudioDecoder path; multi-stream files take the
// MultiStreamAudioDecoder path. The interface is intentionally narrow
// — every public method here is something AudioPlayer or
// DualAudioMixer actually calls.
//
// Not a QObject. Concrete implementations may be QObjects (AudioDecoder
// is) but the interface stays Qt-free so the type isn't constrained
// to a single threading model.

#pragma once

#include <QString>
#include <QStringList>

#include <array>
#include <cstddef>

namespace qcv {

// Output format of every IAudioSource. The audio device side
// (CoreAudioDevice / WasapiAudioDevice) consumes float32 stereo at
// 48 kHz; both decoder shapes resample to this on the way out. Lives
// here because both implementations need to expose it.
struct AudioFormat {
    int sampleRate     = 48000;
    int channels       = 2;
    int bytesPerSample = 4;     // float32

    std::size_t bytesPerFrame() const { return channels * bytesPerSample; }
    double bytesToSeconds(std::size_t bytes) const {
        return static_cast<double>(bytes)
             / (sampleRate * bytesPerFrame());
    }
};

class IAudioSource
{
public:
    virtual ~IAudioSource() = default;

    // ---- Lifecycle ----
    virtual bool open(const QString &path)  = 0;
    virtual void close()                    = 0;
    virtual void start()                    = 0;
    virtual void stop()                     = 0;

    // ---- State ----
    virtual bool   hasAudio()  const = 0;
    virtual bool   isOpen()    const = 0;
    virtual bool   isRunning() const = 0;
    virtual double duration()  const = 0;

    // ---- Transport ----
    // Lazy seek — actually performed on the decode thread at its next
    // iteration boundary. Non-blocking.
    virtual void seek(double position) = 0;

    // Drain the output ring into `output` (float32 interleaved stereo).
    // Returns frames actually delivered; underrun pads with silence.
    // Called from the audio device's render callback at ~10 ms cadence.
    virtual std::size_t read(float *output, std::size_t frameCount) = 0;

    virtual const AudioFormat &format() const = 0;

    // ---- Sync ----
    // Wall-clock seconds since the last completed seek. Used by
    // AudioPlayer's drift-correction cooldown.
    virtual double secondsSinceLastSeek() const = 0;

    // True between seek() being requested and the decode thread
    // completing the flush. While pending, ring content is pre-seek
    // audio about to be discarded — the render callback outputs
    // silence instead of consuming it, so the caller's consumption-
    // based position accounting only ever counts post-flush frames
    // against the new seek anchor.
    virtual bool seekPending() const = 0;

    // ---- Routing ----
    // Per-clip channel routing mode (qcv::AudioRoutingMode cast to
    // int: 0 = Auto, 1 = Downmix5_1, 2 = Stereo7_8). Setter is
    // thread-safe; the implementation handles teardown / hot swap of
    // whatever DSP graph is in use without disturbing decoders.
    virtual void setRoutingMode(int mode) = 0;
    virtual int  routingMode() const = 0;

    // ---- Tempo (review speeds) ----
    // Constant-pitch time-stretch on the decode thread (TempoStage /
    // SoundTouch WSOLA). tempo semantics: output duration = input /
    // tempo, so ring frames map to source seconds ×tempo — the
    // players scale their consumption-based position estimates
    // accordingly. Setter is thread-safe; 1.0 bypasses the stage
    // (byte-identical to the pre-tempo path). Callers re-anchor with
    // a seek after changing it.
    virtual void   setTempo(double tempo) = 0;
    virtual double tempo() const = 0;

    // ---- Source description (for inspector + meters) ----
    // For AudioDecoder: number of channels in the one open stream.
    // For MultiStreamAudioDecoder: number of source streams (each
    // typically mono in broadcast deliverables). The inspector uses
    // this to decide which routing pills to enable.
    virtual int     sourceChannels()          const = 0;
    virtual QString sourceChannelLayoutName() const = 0;

    // Per-source-channel (or per-stream) peak levels of the most
    // recently decoded frame, range [0..1]. Indices [sourceChannels()
    // .. 16) are zero. Sampled at ~30 Hz by WindowManager's audio
    // meter pump.
    virtual std::array<float, 16> peakLevels() const = 0;

    // Labels for each peak index, length matches sourceChannels().
    // AudioDecoder returns positional names ("L", "R", "C", ...) when
    // the layout is recognized, "1".."N" otherwise.
    // MultiStreamAudioDecoder returns "1".."N" (track numbers).
    virtual QStringList sourceChannelNames() const = 0;
};

} // namespace qcv
