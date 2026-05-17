// AudioDecoder — Phase 5.0.a.
//
// FFmpeg-based audio decoder mirroring the old app's
// src/audio/audio_decoder.cpp (564 LOC; this port is functionally
// equivalent for the single-clip case).
//
// Output format is fixed: float32 stereo 48 kHz. SwrContext does the
// resample at decode time so the output device + ring buffer never
// see anything else. Decode thread fills the ring buffer; the audio
// callback drains via read().
//
// Phase 5.0.a is the smoke-test substrate — open / start / read.
// Phase 5.0.b adds the master-clock sync (seek-to-correct-drift)
// loop that the old app's AudioPlayer / AudioMixer use.

#pragma once

#include "audio_ring_buffer.h"
#include "i_audio_source.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace qcv {

class AudioDecoder : public QObject, public IAudioSource
{
    Q_OBJECT

public:
    explicit AudioDecoder(QObject *parent = nullptr);
    ~AudioDecoder() override;

    AudioDecoder(const AudioDecoder &) = delete;
    AudioDecoder &operator=(const AudioDecoder &) = delete;

    // Open the file's audio stream. Returns false if no audio stream
    // is present or any FFmpeg call fails. The video pipeline opens
    // the same file separately; we re-open here because audio decode
    // wants an independent AVFormatContext on a private thread.
    bool open(const QString &path) override;
    void close() override;

    // Start / stop the decode thread.
    void start() override;
    void stop() override;

    bool hasAudio()  const override { return m_hasAudio; }
    bool isOpen()    const override { return m_isOpen; }
    bool isRunning() const override { return m_running.load(); }

    double duration() const override { return m_duration; }

    // Seek to a position in seconds. Lazy — actually performed by
    // the decode thread on the next iteration.
    void seek(double position) override;

    // Drain the ring buffer into `output` (float32 interleaved
    // stereo). Returns frames actually delivered. Underrun pads with
    // silence. Called from the audio device's render callback.
    std::size_t read(float *output, std::size_t frameCount) override;

    const AudioFormat &format() const override { return m_outputFormat; }

    // Wall-clock seconds since the last completed seek. Used by
    // AudioPlayer's drift-correction loop as a cooldown — codec
    // re-seeks are expensive (flush ring + flush codec state) and
    // a too-frequent loop can thrash. Returns a large value if no
    // seek has happened yet.
    double secondsSinceLastSeek() const override;

    // Per-clip channel routing mode. `mode` matches the
    // qcv::AudioRoutingMode enum in media_item.h (0 = Auto, 1 =
    // Downmix5_1, 2 = Stereo7_8). Setter is thread-safe — it just
    // stores the new mode + sets a dirty flag; the decode thread
    // tears down + re-inits SwrContext between decode iterations.
    // Mid-stream reroute is microsecond-cost; user hears at most
    // one decode-frame's worth of stale audio (~21 ms at 48 kHz).
    void setRoutingMode(int mode) override;
    int  routingMode() const override { return m_routingMode.load(); }

    // Detected source channel layout for per-channel meters + the
    // inspector's "layout name" readout. nb=0 / empty until open().
    int     sourceChannels()          const override { return m_sourceChannels; }
    QString sourceChannelLayoutName() const override { return m_sourceChannelLayoutName; }

    // Per-channel peak levels of the most-recently-decoded frame,
    // BEFORE the SwrContext fold. Range [0..1]; index i is source
    // channel i. Indices [sourceChannels()..16) are zero. Updated
    // by the decode thread; safe to call from any thread (atomic
    // loads). Used by WindowManager's 30 Hz audio meter timer to
    // drive the inspector's per-channel level bars.
    std::array<float, 16> peakLevels() const override;

    // Per-source-channel name strings for the meter labels. "L",
    // "R", "C", "LFE", "Ls", "Rs", etc. when the source has a
    // recognized channel layout; falls back to "1", "2", ... when
    // FFmpeg reports the layout as unspecified (just channel count,
    // no positions).
    QStringList sourceChannelNames() const override;

private:
    void decodeThreadFn();
    bool decodeNextPacket();
    void flushAndSeek(double position);

    bool openAudioStream();
    void closeAudioStream();
    bool setupResampler();
    void cleanupResampler();

    bool          m_isOpen   = false;
    bool          m_hasAudio = false;
    AudioFormat   m_outputFormat;
    QString       m_path;

    int     m_sourceSampleRate = 0;
    int     m_sourceChannels   = 0;
    QString m_sourceChannelLayoutName;
    double  m_duration         = 0.0;
    int64_t m_streamStartTime  = 0;

    // Per-clip routing mode + dirty flag. Setter (GUI thread) bumps
    // both atomically; decode thread observes m_routingDirty between
    // iterations and rebuilds m_swrCtx if set. Default 0 = Auto.
    std::atomic<int>  m_routingMode{0};
    std::atomic<bool> m_routingDirty{false};

    // Per-channel peak levels of the most-recent decoded frame,
    // pre-SwrContext-fold. Decode thread writes after each successful
    // av_frame is pulled (before the resample); GUI-thread meter
    // poll reads via atomic loads. Capacity-16 covers Atmos-bed-style
    // sources comfortably; broadcast deliverables top out at 16ch.
    mutable std::array<std::atomic<float>, 16> m_peakPerChannel{};

    AVFormatContext *m_formatCtx   = nullptr;
    AVCodecContext  *m_codecCtx    = nullptr;
    SwrContext      *m_swrCtx      = nullptr;
    AVFrame         *m_decodeFrame = nullptr;
    AVPacket        *m_packet      = nullptr;
    int              m_audioStreamIdx = -1;

    std::unique_ptr<AudioRingBuffer> m_ring;

    std::thread          m_decodeThread;
    std::atomic<bool>    m_running{false};
    std::atomic<double>  m_decodePosition{0.0};
    std::atomic<double>  m_readPosition{0.0};
    std::atomic<bool>    m_eofReached{false};

    std::mutex              m_seekMutex;
    std::condition_variable m_seekCv;
    std::atomic<bool>       m_seekRequested{false};
    double                  m_seekTarget = 0.0;
    std::atomic<double>     m_lastSeekTime{0.0};   // steady_clock seconds-since-epoch

    // Reusable per-frame resample destination — avoids per-frame
    // heap allocation in the hot path.
    std::vector<uint8_t> m_resampleBuffer;
};

} // namespace qcv
