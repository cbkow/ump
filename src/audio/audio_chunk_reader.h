// AudioChunkReader — synchronous, thread-confined grain decoder for
// the shuttle audio engine.
//
// Unlike AudioDecoder (decode thread + ring + lazy seek), this is a
// plain pull API: the grain thread asks for N output frames starting
// at an arbitrary source time and blocks until they're decoded. Two
// regimes fall out of one code path:
//
//   - Forward continuation (FF shuttle at any speed): a readAt whose
//     start equals the previous read's end continues the stream with
//     NO container seek — one continuous forward decode.
//   - Backward / random grabs (RW shuttle, snap re-syncs): container
//     seek (AVSEEK_FLAG_BACKWARD) + codec flush + decode-and-discard
//     up to the target. The discarded lead-in also absorbs AAC
//     priming/preroll; PCM lands nearly sample-exact.
//
// Output is the pipeline standard: interleaved stereo f32 @ 48 kHz,
// folded per the clip's AudioRoutingMode via the same
// computeRoutingMatrix the main decoder uses. Multi-stream broadcast
// masters pluck av_find_best_stream's pick (typically the stereo
// bounce) — accepted v1 limitation; full amerge routing is a
// follow-up.
//
// Owned and driven by exactly one thread (ShuttleAudioEngine's grain
// thread). No locks, no atomics — do not share instances.

#pragma once

#include <QString>

#include <cstddef>
#include <cstdint>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SwrContext;

namespace qcv {

class AudioChunkReader
{
public:
    AudioChunkReader() = default;
    ~AudioChunkReader();

    AudioChunkReader(const AudioChunkReader &) = delete;
    AudioChunkReader &operator=(const AudioChunkReader &) = delete;

    // Opens the file's best audio stream. Returns false when the
    // file has no decodable audio.
    bool open(const QString &path, int routingMode);
    void close();

    bool    isOpen() const { return m_open; }
    QString path()   const { return m_path; }
    double  duration() const { return m_duration; }

    // Deliver up to `frames` interleaved-stereo output frames of
    // source audio starting at srcStartSec. Returns frames actually
    // delivered — short only at stream end / decode failure (caller
    // treats the remainder as silence). Blocks for the decode.
    std::size_t readAt(double srcStartSec, float *dst, std::size_t frames);

private:
    bool setupSwr();
    void teardownSwr();
    bool seekAndPrime(double targetSec);
    // Decode one packet's worth of frames into the FIFO. Returns
    // false at EOF / fatal error.
    bool decodeMore();
    void fifoClear();

    QString          m_path;
    bool             m_open = false;
    double           m_duration = 0.0;
    int              m_routingMode = 0;

    AVFormatContext *m_fmt   = nullptr;
    AVCodecContext  *m_codec = nullptr;
    SwrContext      *m_swr   = nullptr;
    AVPacket        *m_pkt   = nullptr;
    AVFrame         *m_frame = nullptr;
    int              m_streamIdx = -1;
    int64_t          m_streamStartTime = 0;
    int              m_srcChannels = 0;

    // Decoded+resampled output FIFO. m_fifoStartSec is the source
    // time of m_fifo[m_fifoOffset] (interleaved stereo — offset in
    // FRAMES). Compacted on each readAt.
    std::vector<float> m_fifo;
    std::size_t        m_fifoOffsetFrames = 0;
    double             m_fifoStartSec = 0.0;
    bool               m_fifoTimeValid = false;
    bool               m_eof = false;

    // End position of the previous readAt — the forward-continuation
    // fast path triggers when the next request starts here.
    double m_posSec  = 0.0;
    bool   m_posValid = false;

    std::vector<uint8_t> m_swrBuffer;
};

} // namespace qcv
