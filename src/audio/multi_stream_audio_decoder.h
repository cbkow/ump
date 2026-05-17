// MultiStreamAudioDecoder — sibling to AudioDecoder, dispatched at
// AudioPlayer::open() time when the source file has more than one
// audio stream (the broadcast-deliverable common case: one mono PCM
// stream per "track", 8 of them for a 5.1 + stereo-bounce master).
//
// Architecture:
//
//   AVFormatContext  →  packet pump thread  →  routes by stream index
//                                                       ↓
//                       N AVCodecContexts (one per audio stream;
//                       CPU-only PCM/AAC/etc.)
//                                                       ↓
//                       libavfilter graph
//                          ├─ buffersrc[0..N-1]  (one per stream)
//                          ├─ amerge=inputs=K     (K = streams used by mode)
//                          ├─ pan=stereo|...      (per-mode matrix)
//                          ├─ aformat=flt|48k|stereo
//                          └─ buffersink           → AudioRingBuffer
//
// The Pipeline (filter graph + buffersrc/sink handles) is held in a
// std::shared_ptr so setRoutingMode can swap it atomically. Decoders
// are NOT part of the Pipeline — they live for the lifetime of the
// open file and don't get touched by mode changes. avfilter_graph_free
// only releases graph nodes + buffered frames; no codec state, no
// device handles. Worst-case audible artifact on swap: ~21 ms of
// samples sitting in the old sink get dropped.

#pragma once

#include "audio_ring_buffer.h"
#include "i_audio_source.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVFilterGraph;
struct AVFilterContext;
struct AVFrame;
struct AVPacket;

namespace qcv {

class MultiStreamAudioDecoder : public QObject, public IAudioSource
{
    Q_OBJECT

public:
    explicit MultiStreamAudioDecoder(QObject *parent = nullptr);
    ~MultiStreamAudioDecoder() override;

    MultiStreamAudioDecoder(const MultiStreamAudioDecoder &)            = delete;
    MultiStreamAudioDecoder &operator=(const MultiStreamAudioDecoder &) = delete;

    // ---- IAudioSource ----
    bool open(const QString &path) override;
    void close() override;
    void start() override;
    void stop() override;

    bool   hasAudio()  const override { return m_hasAudio; }
    bool   isOpen()    const override { return m_isOpen; }
    bool   isRunning() const override { return m_running.load(); }
    double duration()  const override { return m_duration; }

    void seek(double position) override;
    std::size_t read(float *output, std::size_t frameCount) override;
    const AudioFormat &format() const override { return m_outputFormat; }

    double secondsSinceLastSeek() const override;

    void setRoutingMode(int mode) override;
    int  routingMode() const override { return m_routingMode.load(); }

    int     sourceChannels()          const override { return m_totalChannels; }
    QString sourceChannelLayoutName() const override { return m_layoutName; }

    std::array<float, 16> peakLevels() const override;
    QStringList sourceChannelNames() const override;

private:
    // Pipeline = the filter graph + handles into / out of it. Held by
    // shared_ptr so the decode thread can keep using a snapshot even
    // while setRoutingMode swaps in a replacement on the GUI thread.
    struct Pipeline {
        AVFilterGraph                  *graph      = nullptr;
        std::vector<AVFilterContext *>  buffersrcs;   // one per audio stream
        AVFilterContext                *buffersink = nullptr;

        ~Pipeline();
        Pipeline()                            = default;
        Pipeline(const Pipeline &)            = delete;
        Pipeline &operator=(const Pipeline &) = delete;
    };

    void decodeThreadFn();
    bool openStreams(const QString &path);
    void closeStreams();
    void flushAndSeek(double position);

    // Build a fresh Pipeline configured per `mode`. Returns nullptr
    // on failure (caller keeps the existing pipeline).
    std::shared_ptr<Pipeline> buildPipeline(int mode) const;

    // Per-channel peak tap. For each channel in the frame, writes
    // peak |sample| to m_peakPerChannel[globalChannelOffset + ch].
    // globalChannelOffset = m_channelStartOffset[streamIdx], the
    // cumulative channel count of all earlier streams. This means
    // an 8-mono file and a 6-mono+1-stereo file both render to 8
    // channel meters — one bar per actual audio channel rather than
    // one per stream — so the user doesn't have to know how the file
    // packages its channels to read the meter.
    void updatePeakForFrame(int streamIdx, const AVFrame *frame);

    bool          m_isOpen   = false;
    bool          m_hasAudio = false;
    AudioFormat   m_outputFormat;
    QString       m_path;

    double  m_duration         = 0.0;
    int     m_streamCount      = 0;
    int     m_totalChannels    = 0;   // sum of per-stream channel counts
    QString m_layoutName;        // "N mono tracks" / "N channels"
    // Cumulative channel offset per stream — m_channelStartOffset[i]
    // is the global channel index where stream i's first channel
    // lives. Computed once in openStreams() so updatePeakForFrame can
    // map (streamIdx, localCh) → global channel in O(1).
    std::vector<int> m_channelStartOffset;

    AVFormatContext *m_formatCtx = nullptr;
    // Indices of audio streams in m_formatCtx->streams. Length =
    // m_streamCount. Used by the packet pump to dispatch packets to
    // the right decoder.
    std::vector<int>              m_audioStreamIndices;
    // Decoders parallel to m_audioStreamIndices. Indexed by
    // audio-stream ordinal (NOT by global stream index).
    std::vector<AVCodecContext *> m_decoders;
    // Reverse map: global stream index → audio ordinal in m_decoders.
    // Sized to m_formatCtx->nb_streams, -1 for non-audio entries.
    std::vector<int>              m_globalToAudioIdx;

    // Hot-swappable filter graph.
    std::shared_ptr<Pipeline> m_pipeline;
    mutable std::mutex        m_pipelineMutex;

    // Routing mode (qcv::AudioRoutingMode cast to int).
    std::atomic<int> m_routingMode{0};

    // Output ring (float32 stereo @ m_outputFormat.sampleRate).
    std::unique_ptr<AudioRingBuffer> m_ring;

    // Decode + pump.
    std::thread        m_decodeThread;
    std::atomic<bool>  m_running{false};
    std::atomic<bool>  m_stopRequested{false};
    std::atomic<bool>  m_eofReached{false};

    // Seek state. Same shape as AudioDecoder's lazy seek.
    std::mutex              m_seekMutex;
    std::condition_variable m_seekCv;
    std::atomic<bool>       m_seekRequested{false};
    double                  m_seekTarget   = 0.0;
    std::atomic<double>     m_lastSeekTime{0.0};
    std::atomic<double>     m_decodePosition{0.0};
    std::atomic<double>     m_readPosition{0.0};

    // Per-channel peaks. Indexed by global channel position (0-based)
    // — channel i means "the i-th audio channel across the whole file
    // when streams are concatenated in their AVFormat order". 16 is
    // the cap; broadcast deliverables almost never exceed 8.
    mutable std::array<std::atomic<float>, 16> m_peakPerChannel{};
};

} // namespace qcv
