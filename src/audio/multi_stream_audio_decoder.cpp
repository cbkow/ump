#include "multi_stream_audio_decoder.h"

#include "project/media_item.h"   // qcv::AudioRoutingMode (values
                                   // mirrored by the int routing API).

#include <QtLogging>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <set>
#include <utility>

namespace qcv {

namespace {

// Common output of all filter graphs: 48 kHz float32 stereo. Mirrors
// what AudioDecoder's SwrContext targets so the platform device side
// (CoreAudio / WASAPI) doesn't care which decoder shape is in use.
constexpr int kOutSampleRate = 48000;

// Ring is sized for headroom; the actual buffered depth is governed
// by kBackPressureBufferedBytes below. Larger total capacity gives the
// device-side reader breathing room across scheduler hiccups without
// physically capping how much we keep ahead of playback.
//
//   kRingBytes = 192000 bytes ≈ 500 ms total at 48 kHz stereo float32.
//
// Bytes are kSampleRate × stereoChannels × float32 = 48000·2·4 = 384000
// bytes per second of output. So 38400 bytes = 100 ms.
constexpr std::size_t kRingBytes                = 192000;

// Steady-state buffered depth target. Decode pump back-pressures
// (sleeps) once the ring has more than this many bytes queued for the
// device. Lower = tighter video-to-audio sync because samples reach
// the device sooner after they're decoded. Higher = more cushion vs.
// scheduler/decoder hiccups before the device hears a glitch.
//
// 50 ms at 48 kHz stereo float32 = 9600 bytes. PCM decode is a memcpy
// and the libavfilter graph's per-frame overhead is well under a ms,
// so the only real risk at this depth is OS scheduler jitter. WASAPI's
// own 10 ms callback buffer absorbs single-tick stalls; anything past
// ~30 ms back-to-back will glitch regardless of ring size, so going
// deeper than 50 ms doesn't actually buy resilience — it just trades
// sync for nothing.
constexpr std::size_t kBackPressureBufferedBytes = 9600;   // ~50 ms

} // namespace

MultiStreamAudioDecoder::Pipeline::~Pipeline()
{
    if (graph) {
        avfilter_graph_free(&graph);
        graph = nullptr;
    }
    // buffersrcs / buffersink are owned by graph; nullptrs after free.
}

MultiStreamAudioDecoder::MultiStreamAudioDecoder(QObject *parent)
    : QObject(parent)
{
    m_outputFormat.sampleRate     = kOutSampleRate;
    m_outputFormat.channels       = 2;
    m_outputFormat.bytesPerSample = 4;
}

MultiStreamAudioDecoder::~MultiStreamAudioDecoder()
{
    close();
}

bool MultiStreamAudioDecoder::open(const QString &path)
{
    if (m_isOpen) close();
    m_path = path;

    if (!openStreams(path)) {
        closeStreams();
        return false;
    }

    m_ring = std::make_unique<AudioRingBuffer>(kRingBytes);

    // Build the initial pipeline. setRoutingMode() returns early if
    // mode hasn't changed, so we exchange to a sentinel first to
    // force a build at the current routing mode.
    auto initial = buildPipeline(m_routingMode.load());
    if (!initial) {
        qWarning("MultiStreamAudioDecoder: initial pipeline build failed");
        closeStreams();
        return false;
    }
    m_pipeline = std::move(initial);

    m_isOpen   = true;
    m_hasAudio = (m_streamCount > 0);
    qInfo("MultiStreamAudioDecoder: opened %s — %d audio streams, "
          "duration=%.2fs",
          qPrintable(path), m_streamCount, m_duration);
    return true;
}

void MultiStreamAudioDecoder::close()
{
    stop();
    {
        std::lock_guard lock(m_pipelineMutex);
        m_pipeline.reset();
    }
    closeStreams();
    m_ring.reset();
    m_isOpen   = false;
    m_hasAudio = false;
    m_duration = 0.0;
    m_streamCount   = 0;
    m_totalChannels = 0;
    m_channelStartOffset.clear();
    m_layoutName.clear();
    for (auto &p : m_peakPerChannel) p.store(0.0f, std::memory_order_relaxed);
}

void MultiStreamAudioDecoder::start()
{
    if (!m_isOpen || m_running.load()) return;
    m_eofReached.store(false);
    m_stopRequested.store(false);
    m_running.store(true);
    m_decodeThread = std::thread(&MultiStreamAudioDecoder::decodeThreadFn, this);
}

void MultiStreamAudioDecoder::stop()
{
    if (!m_running.exchange(false)) return;
    m_stopRequested.store(true);
    {
        std::lock_guard lock(m_seekMutex);
    }
    m_seekCv.notify_one();
    if (m_decodeThread.joinable()) m_decodeThread.join();
}

void MultiStreamAudioDecoder::seek(double position)
{
    {
        std::lock_guard lock(m_seekMutex);
        m_seekTarget    = position;
        m_seekRequested.store(true);
    }
    m_seekCv.notify_one();
}

std::size_t MultiStreamAudioDecoder::read(float *output, std::size_t frameCount)
{
    if (!m_ring) {
        std::memset(output, 0, frameCount * 2 * sizeof(float));
        return 0;
    }
    const std::size_t bytesNeeded = frameCount * 2 * sizeof(float);
    const std::size_t bytesRead = m_ring->read(
        reinterpret_cast<uint8_t *>(output), bytesNeeded);
    const std::size_t framesRead = bytesRead / (2 * sizeof(float));
    if (framesRead < frameCount) {
        std::memset(output + framesRead * 2, 0,
                    (frameCount - framesRead) * 2 * sizeof(float));
    }
    m_readPosition.store(
        m_readPosition.load()
        + static_cast<double>(framesRead) / m_outputFormat.sampleRate,
        std::memory_order_relaxed);
    return framesRead;
}

double MultiStreamAudioDecoder::secondsSinceLastSeek() const
{
    const double last = m_lastSeekTime.load(std::memory_order_relaxed);
    if (last <= 0.0) return 1e9;
    const double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return now - last;
}

void MultiStreamAudioDecoder::setRoutingMode(int mode)
{
    if (mode < 0 || mode > 2) mode = 0;
    if (m_routingMode.exchange(mode) == mode) return;
    if (!m_isOpen) return;   // pipeline rebuilt at next open()

    auto next = buildPipeline(mode);
    if (!next) {
        qWarning("MultiStreamAudioDecoder: pipeline rebuild for mode=%d "
                 "failed; keeping previous pipeline", mode);
        return;
    }
    {
        std::lock_guard lock(m_pipelineMutex);
        m_pipeline = std::move(next);
    }
    // Old pipeline released at scope exit. Decoders unaffected.
}

std::array<float, 16> MultiStreamAudioDecoder::peakLevels() const
{
    std::array<float, 16> out{};
    for (int i = 0; i < 16; ++i) {
        out[i] = m_peakPerChannel[i].load(std::memory_order_relaxed);
    }
    return out;
}

QStringList MultiStreamAudioDecoder::sourceChannelNames() const
{
    QStringList names;
    const int n = std::min(m_totalChannels, 16);
    names.reserve(n);
    // Sequential channel numbers across the whole file. A stereo
    // bounce stream contributes two channels (e.g., "7" and "8" in a
    // 6-mono+1-stereo broadcast master), so the meter row reads the
    // same as the user's mental "tracks 1..8" model regardless of
    // how the file packages its streams.
    for (int i = 0; i < n; ++i) {
        names << QString::number(i + 1);
    }
    return names;
}

// -- private --------------------------------------------------------

bool MultiStreamAudioDecoder::openStreams(const QString &path)
{
    m_formatCtx = avformat_alloc_context();
    const QByteArray pathUtf8 = path.toUtf8();
    if (avformat_open_input(&m_formatCtx, pathUtf8.constData(),
                              nullptr, nullptr) < 0) {
        qWarning("MultiStreamAudioDecoder: avformat_open_input failed for %s",
                 qPrintable(path));
        return false;
    }
    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
        qWarning("MultiStreamAudioDecoder: avformat_find_stream_info failed");
        return false;
    }

    if (m_formatCtx->duration > 0) {
        m_duration =
            static_cast<double>(m_formatCtx->duration) / AV_TIME_BASE;
    }

    // Enumerate audio streams + open one decoder per stream.
    m_audioStreamIndices.clear();
    m_decoders.clear();
    m_globalToAudioIdx.assign(m_formatCtx->nb_streams, -1);

    for (unsigned i = 0; i < m_formatCtx->nb_streams; ++i) {
        AVStream *s = m_formatCtx->streams[i];
        if (!s || !s->codecpar) continue;
        if (s->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;

        const AVCodec *codec = avcodec_find_decoder(s->codecpar->codec_id);
        if (!codec) {
            qWarning("MultiStreamAudioDecoder: no decoder for codec %d "
                     "on stream %u — skipping", s->codecpar->codec_id, i);
            continue;
        }
        AVCodecContext *cctx = avcodec_alloc_context3(codec);
        if (!cctx
            || avcodec_parameters_to_context(cctx, s->codecpar) < 0
            || avcodec_open2(cctx, codec, nullptr) < 0) {
            qWarning("MultiStreamAudioDecoder: codec open failed on stream %u",
                     i);
            if (cctx) avcodec_free_context(&cctx);
            continue;
        }

        m_globalToAudioIdx[i] = static_cast<int>(m_decoders.size());
        m_audioStreamIndices.push_back(static_cast<int>(i));
        m_decoders.push_back(cctx);
    }

    m_streamCount = static_cast<int>(m_decoders.size());
    if (m_streamCount == 0) {
        qWarning("MultiStreamAudioDecoder: no usable audio streams in %s",
                 qPrintable(path));
        return false;
    }

    // Layout-name surface. For multi-stream broadcast files the
    // common case is "N mono tracks"; fall back to "N channels" if
    // any stream isn't mono. Also build the per-stream cumulative
    // channel offset so the per-channel meter tap can map
    // (streamIdx, localCh) → global channel in O(1).
    bool allMono = true;
    int  totalCh = 0;
    m_channelStartOffset.assign(m_streamCount, 0);
    for (int i = 0; i < m_streamCount; ++i) {
        m_channelStartOffset[i] = totalCh;
        const int n = m_decoders[i]->ch_layout.nb_channels;
        if (n != 1) allMono = false;
        totalCh += n;
    }
    m_totalChannels = totalCh;
    m_layoutName = allMono
        ? QString::number(m_streamCount) + QStringLiteral(" mono tracks")
        : QString::number(totalCh)       + QStringLiteral(" channels");

    return true;
}

void MultiStreamAudioDecoder::closeStreams()
{
    for (auto *cctx : m_decoders) {
        if (cctx) avcodec_free_context(&cctx);
    }
    m_decoders.clear();
    m_audioStreamIndices.clear();
    m_globalToAudioIdx.clear();
    m_streamCount = 0;
    if (m_formatCtx) avformat_close_input(&m_formatCtx);
}

// Resolve which input-stream indices the routing mode needs and what
// pan expression (if any) the merged amerge output gets folded with.
// Returns a list of source-stream indices in [0..streamChannels.size())
// and an optional pan expression. Empty pan = pass amerge output
// through (relies on aformat to negotiate channel-count / layout to
// stereo).
struct RoutingPlan {
    std::vector<int> inputStreams;   // indices into m_decoders
    QByteArray       panExpr;        // empty = no explicit pan
};

// Reasons for taking per-stream channel counts instead of just stream
// count: broadcast 8-channel masters ship in two common shapes —
//   (a) 8 mono streams (older / international / some post houses)
//   (b) 6 mono + 1 stereo bounce = 7 streams, 8 channels (CBS/NBC/ABC)
// (a) used to work because "Stereo7_8 = pluck(6,7)" lined up with the
// stream indices. (b) silently broke the same mode: streamCount=7 fell
// past the >=8 branch and into pluck(0,1) — the L+R discrete pair,
// missing the dialog channel C entirely. The fix is to think in
// global channels (1..N across the whole file), find which stream(s)
// contain the desired global channels, and build the graph from
// those. Same logic naturally handles the 5.1-in-1-stream-plus-
// stereo-in-1-stream case and any future hybrid layouts.
static RoutingPlan resolveRouting(int mode,
                                    const std::vector<int> &streamChannels)
{
    RoutingPlan plan;
    const auto m = static_cast<AudioRoutingMode>(mode);
    const int streamCount = static_cast<int>(streamChannels.size());
    int totalCh = 0;
    for (int c : streamChannels) totalCh += c;

    // Locate global channel `gc` (zero-indexed) in the per-stream
    // layout. Returns {streamIdx, localCh} or {-1, -1} if out of range.
    auto locate = [&](int gc) -> std::pair<int, int> {
        int cumul = 0;
        for (int i = 0; i < streamCount; ++i) {
            const int n = streamChannels[i];
            if (gc >= cumul && gc < cumul + n) {
                return { i, gc - cumul };
            }
            cumul += n;
        }
        return { -1, -1 };
    };

    // After amerge concatenates the chosen inputStreams in order, what
    // is the post-amerge channel index for the given (stream, localCh)?
    auto amergeChannelOf = [&](int streamIdx, int localCh) -> int {
        int amergeCh = 0;
        for (int s : plan.inputStreams) {
            if (s == streamIdx) return amergeCh + localCh;
            amergeCh += streamChannels[s];
        }
        return -1;
    };

    // Pluck a pair of global channels into stereo. Resolves which
    // source streams to pull, then builds a pan iff the amerge output
    // doesn't already match (e.g., picking 2 mono streams produces a
    // 2-ch amerge output that's stereo natively — no pan needed).
    auto pluckGlobalPair = [&](int gcL, int gcR) -> bool {
        const auto [sL, lcL] = locate(gcL);
        const auto [sR, lcR] = locate(gcR);
        if (sL < 0 || sR < 0) return false;

        // Unique, sorted source stream list.
        if (sL == sR) {
            plan.inputStreams = { sL };
        } else if (sL < sR) {
            plan.inputStreams = { sL, sR };
        } else {
            plan.inputStreams = { sR, sL };
        }

        const int amergeChL = amergeChannelOf(sL, lcL);
        const int amergeChR = amergeChannelOf(sR, lcR);

        // Skip pan when the amerge output is already exactly our pair
        // in the right order — the common case for picking a single
        // stereo stream (output ch0,ch1 = our L,R) or two mono streams
        // pulled in L-then-R order.
        int amergeTotalCh = 0;
        for (int s : plan.inputStreams) amergeTotalCh += streamChannels[s];
        const bool passthrough = (amergeTotalCh == 2)
                                && amergeChL == 0 && amergeChR == 1;
        if (passthrough) {
            plan.panExpr.clear();
        } else {
            QByteArray pe = "stereo|c0=c";
            pe += QByteArray::number(amergeChL);
            pe += "|c1=c";
            pe += QByteArray::number(amergeChR);
            plan.panExpr = pe;
        }
        return true;
    };

    // BS.775 5.1→stereo fold. Sources global channels 0..5 (assumed
    // L R C LFE Ls Rs in that order); resolves them to whichever
    // streams hold them, then maps the BS.775 coefficients onto the
    // resulting amerge channel indices. LFE deliberately dropped.
    auto downmix5_1 = [&]() -> bool {
        if (totalCh < 6) return false;
        std::array<std::pair<int, int>, 6> locs;
        std::set<int> uniqueStreams;
        for (int gc = 0; gc < 6; ++gc) {
            locs[gc] = locate(gc);
            if (locs[gc].first < 0) return false;
            uniqueStreams.insert(locs[gc].first);
        }
        plan.inputStreams.assign(uniqueStreams.begin(), uniqueStreams.end());
        std::sort(plan.inputStreams.begin(), plan.inputStreams.end());

        std::array<int, 6> amergeCh;
        for (int gc = 0; gc < 6; ++gc) {
            amergeCh[gc] = amergeChannelOf(locs[gc].first, locs[gc].second);
            if (amergeCh[gc] < 0) return false;
        }
        // L_out = L + 0.707·C + 0.707·Ls
        // R_out = R + 0.707·C + 0.707·Rs
        QByteArray pe = "stereo";
        pe += "|c0=c" + QByteArray::number(amergeCh[0])
            + "+0.707*c" + QByteArray::number(amergeCh[2])
            + "+0.707*c" + QByteArray::number(amergeCh[4]);
        pe += "|c1=c" + QByteArray::number(amergeCh[1])
            + "+0.707*c" + QByteArray::number(amergeCh[2])
            + "+0.707*c" + QByteArray::number(amergeCh[5]);
        plan.panExpr = pe;
        return true;
    };

    auto fallback = [&]() {
        if (totalCh >= 2)      pluckGlobalPair(0, 1);
        else if (streamCount > 0) plan.inputStreams = { 0 };
    };

    switch (m) {
    case AudioRoutingMode::Downmix5_1:
        if (!downmix5_1()) fallback();
        break;
    case AudioRoutingMode::Stereo7_8:
        if (totalCh < 8 || !pluckGlobalPair(6, 7)) fallback();
        break;
    case AudioRoutingMode::Auto:
    default:
        // Auto picks the broadcast convention by total-channel count.
        // 8 ch → producer's stereo bounce (channels 7-8) is the
        // intended deliverable. 6 ch → fold the 5.1. Smaller → just
        // play whatever stereo or mono the file has.
        if (totalCh >= 8 && pluckGlobalPair(6, 7)) break;
        if (totalCh >= 6 && downmix5_1())          break;
        fallback();
        break;
    }
    return plan;
}

std::shared_ptr<MultiStreamAudioDecoder::Pipeline>
MultiStreamAudioDecoder::buildPipeline(int mode) const
{
    if (m_streamCount == 0 || m_decoders.empty()) return {};

    // Snapshot per-stream channel counts so resolveRouting can think in
    // global channel positions across mixed mono/stereo layouts (the
    // 6-mono+1-stereo broadcast convention vs. 8-mono).
    std::vector<int> streamChannels(m_streamCount, 0);
    for (int i = 0; i < m_streamCount; ++i) {
        if (m_decoders[i]) {
            streamChannels[i] = m_decoders[i]->ch_layout.nb_channels;
        }
    }

    const RoutingPlan plan = resolveRouting(mode, streamChannels);
    if (plan.inputStreams.empty()) return {};

    auto p = std::make_shared<Pipeline>();
    p->graph = avfilter_graph_alloc();
    if (!p->graph) return {};

    p->buffersrcs.resize(m_streamCount, nullptr);

    const AVFilter *abuffer  = avfilter_get_by_name("abuffer");
    const AVFilter *abufsink = avfilter_get_by_name("abuffersink");
    const AVFilter *amerge   = avfilter_get_by_name("amerge");
    const AVFilter *pan      = avfilter_get_by_name("pan");
    const AVFilter *aformat  = avfilter_get_by_name("aformat");
    if (!abuffer || !abufsink || !amerge || !pan || !aformat) {
        qWarning("MultiStreamAudioDecoder: required filter missing "
                 "(abuffer/abuffersink/amerge/pan/aformat)");
        return {};
    }

    // Create an abuffer source per stream the routing plan uses. We
    // skip streams the plan ignores so the decode thread doesn't
    // waste a graph push on samples nothing reads — those packets
    // are decoded for the per-stream meter tap and then discarded.
    for (int srcIdx : plan.inputStreams) {
        if (srcIdx < 0 || srcIdx >= m_streamCount) {
            qWarning("MultiStreamAudioDecoder: routing plan referenced "
                     "out-of-range stream %d (count=%d)",
                     srcIdx, m_streamCount);
            return {};
        }
        AVCodecContext *cctx = m_decoders[srcIdx];
        char layoutBuf[64] = {0};
        av_channel_layout_describe(&cctx->ch_layout, layoutBuf,
                                     sizeof(layoutBuf));
        const QByteArray layoutStr = (layoutBuf[0] != 0)
            ? QByteArray(layoutBuf)
            : (cctx->ch_layout.nb_channels == 1
                 ? QByteArray("mono")
                 : QByteArray::number(cctx->ch_layout.nb_channels) + "c");

        const QByteArray args =
            "time_base=1/" + QByteArray::number(cctx->sample_rate)
            + ":sample_rate=" + QByteArray::number(cctx->sample_rate)
            + ":sample_fmt=" + av_get_sample_fmt_name(cctx->sample_fmt)
            + ":channel_layout=" + layoutStr;

        const QByteArray name = "src_" + QByteArray::number(srcIdx);
        AVFilterContext *src = nullptr;
        if (avfilter_graph_create_filter(&src, abuffer, name.constData(),
                                            args.constData(), nullptr,
                                            p->graph) < 0) {
            qWarning("MultiStreamAudioDecoder: abuffer create failed for "
                     "stream %d (args=%s)", srcIdx, args.constData());
            return {};
        }
        p->buffersrcs[srcIdx] = src;
    }

    // Sink + final-format opts.
    if (avfilter_graph_create_filter(&p->buffersink, abufsink, "out",
                                        nullptr, nullptr, p->graph) < 0) {
        qWarning("MultiStreamAudioDecoder: abuffersink create failed");
        return {};
    }
    static const enum AVSampleFormat kOutFmts[] = { AV_SAMPLE_FMT_FLT,
                                                     AV_SAMPLE_FMT_NONE };
    static const int kOutRates[] = { kOutSampleRate, -1 };
    av_opt_set_int_list(p->buffersink, "sample_fmts", kOutFmts,
                          AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int_list(p->buffersink, "sample_rates", kOutRates,
                          -1, AV_OPT_SEARCH_CHILDREN);
    av_opt_set(p->buffersink, "ch_layouts", "stereo",
                 AV_OPT_SEARCH_CHILDREN);

    const int mergeInputs = static_cast<int>(plan.inputStreams.size());
    AVFilterContext *prev = nullptr;

    if (mergeInputs == 1) {
        // Single-stream path through the multi-stream class — no
        // amerge needed; aformat handles up-mix from mono to stereo.
        prev = p->buffersrcs[plan.inputStreams[0]];
    } else {
        AVFilterContext *amergeCtx = nullptr;
        const QByteArray amergeArgs =
            "inputs=" + QByteArray::number(mergeInputs);
        if (avfilter_graph_create_filter(&amergeCtx, amerge, "amerge",
                                            amergeArgs.constData(), nullptr,
                                            p->graph) < 0) {
            qWarning("MultiStreamAudioDecoder: amerge create failed");
            return {};
        }
        for (int i = 0; i < mergeInputs; ++i) {
            const int srcIdx = plan.inputStreams[i];
            if (avfilter_link(p->buffersrcs[srcIdx], 0,
                                amergeCtx, i) < 0) {
                qWarning("MultiStreamAudioDecoder: link src[%d]→amerge[%d] "
                         "failed", srcIdx, i);
                return {};
            }
        }
        prev = amergeCtx;
    }

    // Optional pan stage for explicit downmix matrices.
    if (!plan.panExpr.isEmpty()) {
        AVFilterContext *panCtx = nullptr;
        if (avfilter_graph_create_filter(&panCtx, pan, "pan",
                                            plan.panExpr.constData(), nullptr,
                                            p->graph) < 0) {
            qWarning("MultiStreamAudioDecoder: pan create failed (expr=%s)",
                     plan.panExpr.constData());
            return {};
        }
        if (avfilter_link(prev, 0, panCtx, 0) < 0) {
            qWarning("MultiStreamAudioDecoder: link merge→pan failed");
            return {};
        }
        prev = panCtx;
    }

    // Final aformat → buffersink.
    AVFilterContext *aformatCtx = nullptr;
    const QByteArray aformatArgs =
        "sample_fmts=flt:sample_rates="
        + QByteArray::number(kOutSampleRate)
        + ":channel_layouts=stereo";
    if (avfilter_graph_create_filter(&aformatCtx, aformat, "aformat",
                                        aformatArgs.constData(), nullptr,
                                        p->graph) < 0) {
        qWarning("MultiStreamAudioDecoder: aformat create failed");
        return {};
    }
    if (avfilter_link(prev, 0, aformatCtx, 0) < 0
        || avfilter_link(aformatCtx, 0, p->buffersink, 0) < 0) {
        qWarning("MultiStreamAudioDecoder: link prev→aformat→sink failed");
        return {};
    }

    if (avfilter_graph_config(p->graph, nullptr) < 0) {
        qWarning("MultiStreamAudioDecoder: graph config failed (mode=%d)",
                 mode);
        return {};
    }

    qInfo("MultiStreamAudioDecoder: pipeline built — mode=%d, "
          "merge=%d streams, pan=%s",
          mode, mergeInputs,
          plan.panExpr.isEmpty() ? "(none)" : plan.panExpr.constData());
    return p;
}

void MultiStreamAudioDecoder::updatePeakForFrame(int streamIdx,
                                                   const AVFrame *frame)
{
    if (!frame || streamIdx < 0 || streamIdx >= m_streamCount) return;
    if (frame->nb_samples <= 0) return;

    const auto fmt = static_cast<AVSampleFormat>(frame->format);
    const int  n   = frame->nb_samples;
    const int  srcCh = frame->ch_layout.nb_channels;
    if (srcCh <= 0) return;

    const int baseGlobal = m_channelStartOffset[streamIdx];

    auto sweepFloat = [&](const float *p, int stride) -> float {
        float pk = 0.0f;
        for (int i = 0; i < n; ++i) {
            const float a = std::fabs(p[i * stride]);
            if (a > pk) pk = a;
        }
        return pk;
    };
    auto sweepDouble = [&](const double *p, int stride) -> float {
        double pk = 0.0;
        for (int i = 0; i < n; ++i) {
            const double a = std::fabs(p[i * stride]);
            if (a > pk) pk = a;
        }
        return static_cast<float>(pk);
    };
    auto sweepS16 = [&](const int16_t *p, int stride) -> float {
        constexpr float kInv = 1.0f / 32768.0f;
        int16_t pk = 0;
        for (int i = 0; i < n; ++i) {
            const int16_t v = p[i * stride];
            const int16_t a = v < 0 ? -v : v;
            if (a > pk) pk = a;
        }
        return static_cast<float>(pk) * kInv;
    };
    // FFmpeg unpacks pcm_s24le to AV_SAMPLE_FMT_S32 (24-bit value
    // shifted into the upper 24 bits of int32). INT32_MAX as divisor
    // normalizes both true 32-bit and shifted 24-bit PCM to [0..1].
    auto sweepS32 = [&](const int32_t *p, int stride) -> float {
        constexpr float kInv = 1.0f / 2147483647.0f;
        int32_t pk = 0;
        for (int i = 0; i < n; ++i) {
            const int32_t v = p[i * stride];
            const int32_t a = v < 0 ? -v : v;
            if (a > pk) pk = a;
        }
        return static_cast<float>(pk) * kInv;
    };

    // Sweep each source channel. For interleaved formats data[0]
    // holds all channels with stride = srcCh; for planar formats
    // each channel sits in its own data[ch] buffer with stride 1.
    for (int ch = 0; ch < srcCh; ++ch) {
        const int globalCh = baseGlobal + ch;
        if (globalCh >= 16) break;   // 16-channel cap

        float peak = 0.0f;
        bool  handled = true;

        switch (fmt) {
        case AV_SAMPLE_FMT_FLT: {
            const auto *p = reinterpret_cast<const float *>(frame->data[0]);
            peak = sweepFloat(p + ch, srcCh);
            break;
        }
        case AV_SAMPLE_FMT_FLTP: {
            const auto *p = reinterpret_cast<const float *>(frame->data[ch]);
            peak = sweepFloat(p, 1);
            break;
        }
        case AV_SAMPLE_FMT_DBL: {
            const auto *p = reinterpret_cast<const double *>(frame->data[0]);
            peak = sweepDouble(p + ch, srcCh);
            break;
        }
        case AV_SAMPLE_FMT_DBLP: {
            const auto *p = reinterpret_cast<const double *>(frame->data[ch]);
            peak = sweepDouble(p, 1);
            break;
        }
        case AV_SAMPLE_FMT_S16: {
            const auto *p = reinterpret_cast<const int16_t *>(frame->data[0]);
            peak = sweepS16(p + ch, srcCh);
            break;
        }
        case AV_SAMPLE_FMT_S16P: {
            const auto *p = reinterpret_cast<const int16_t *>(frame->data[ch]);
            peak = sweepS16(p, 1);
            break;
        }
        case AV_SAMPLE_FMT_S32: {
            const auto *p = reinterpret_cast<const int32_t *>(frame->data[0]);
            peak = sweepS32(p + ch, srcCh);
            break;
        }
        case AV_SAMPLE_FMT_S32P: {
            const auto *p = reinterpret_cast<const int32_t *>(frame->data[ch]);
            peak = sweepS32(p, 1);
            break;
        }
        default:
            handled = false;
            break;
        }

        // Unhandled sample format: leave whatever the previous frame
        // wrote in place (avoids zero-flicker on every other frame).
        if (handled) {
            m_peakPerChannel[globalCh].store(peak, std::memory_order_relaxed);
        }
    }
}

void MultiStreamAudioDecoder::flushAndSeek(double position)
{
    if (!m_formatCtx) return;
    const int64_t target = static_cast<int64_t>(position * AV_TIME_BASE);
    avformat_seek_file(m_formatCtx, -1, INT64_MIN, target, INT64_MAX,
                        AVSEEK_FLAG_BACKWARD);
    for (auto *cctx : m_decoders) {
        if (cctx) avcodec_flush_buffers(cctx);
    }
    if (m_ring) m_ring->clear();

    // Rebuild the libavfilter graph atomically. Decoder flushes only
    // clear FFmpeg's per-codec internal buffers; amerge's per-input
    // frame queues keep whatever pre-seek samples are sitting in them
    // and on resume amerge pairs misaligned post-seek + pre-seek
    // frames → multi-second sync slip + audible echo. The cleanest
    // fix is the same hot-swap setRoutingMode uses: build a fresh
    // pipeline configured for the current routing mode and atomic-
    // swap the shared_ptr. Decoders stay alive (no codec re-init).
    // Cost: ~1-5 ms graph build, plus ~21 ms of stale samples
    // sitting in the old sink that get dropped — invisible inside
    // a seek event.
    auto fresh = buildPipeline(m_routingMode.load());
    if (fresh) {
        std::lock_guard lock(m_pipelineMutex);
        m_pipeline = std::move(fresh);
    }
    // else: build failed (rare; would be logged by buildPipeline).
    // Keep the existing pipeline — better stale audio than silent.

    m_decodePosition.store(position, std::memory_order_relaxed);
    m_readPosition.store(position, std::memory_order_relaxed);
    m_eofReached.store(false);
    m_lastSeekTime.store(
        std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count(),
        std::memory_order_relaxed);
}

void MultiStreamAudioDecoder::decodeThreadFn()
{
    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frame = av_frame_alloc();
    AVFrame  *outFrame = av_frame_alloc();
    if (!pkt || !frame || !outFrame) {
        if (pkt)      av_packet_free(&pkt);
        if (frame)    av_frame_free(&frame);
        if (outFrame) av_frame_free(&outFrame);
        return;
    }

    while (m_running.load()) {
        // Service pending seek.
        {
            std::unique_lock lock(m_seekMutex);
            if (m_seekRequested.load()) {
                flushAndSeek(m_seekTarget);
                m_seekRequested.store(false);
            }
        }

        // Back-pressure: enforce the buffered-depth target so we don't
        // run hundreds of ms ahead of the device. availableRead() is
        // bytes currently queued for the device; if that exceeds the
        // target, sleep until the device drains some.
        if (m_ring
            && m_ring->availableRead() > kBackPressureBufferedBytes) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        // EOF: idle until a seek wakes us.
        if (m_eofReached.load()) {
            std::unique_lock lock(m_seekMutex);
            m_seekCv.wait_for(lock, std::chrono::milliseconds(50),
                              [this] { return !m_running.load()
                                              || m_seekRequested.load(); });
            continue;
        }

        // Read one packet.
        const int rc = av_read_frame(m_formatCtx, pkt);
        if (rc < 0) {
            if (rc == AVERROR_EOF) m_eofReached.store(true);
            else std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // Dispatch by stream — only audio packets matter.
        const int gIdx = pkt->stream_index;
        const int aIdx = (gIdx >= 0 && gIdx < static_cast<int>(m_globalToAudioIdx.size()))
                       ? m_globalToAudioIdx[gIdx]
                       : -1;
        if (aIdx < 0) {
            av_packet_unref(pkt);
            continue;
        }

        AVCodecContext *cctx = m_decoders[aIdx];
        if (avcodec_send_packet(cctx, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);

        // Drain decoded frames + push into the active pipeline.
        while (avcodec_receive_frame(cctx, frame) == 0) {
            updatePeakForFrame(aIdx, frame);

            std::shared_ptr<Pipeline> p;
            {
                std::lock_guard lock(m_pipelineMutex);
                p = m_pipeline;
            }
            if (!p || aIdx >= static_cast<int>(p->buffersrcs.size())
                   || !p->buffersrcs[aIdx]) {
                av_frame_unref(frame);
                continue;
            }
            // Keep ref so the frame can stay live in the graph's
            // internal buffer past this scope. unref clears our copy.
            if (av_buffersrc_add_frame_flags(
                    p->buffersrcs[aIdx], frame,
                    AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                av_frame_unref(frame);
                continue;
            }
            av_frame_unref(frame);

            // Drain the sink. amerge needs samples on all its inputs
            // before it produces output, so for the first few packets
            // on each stream this returns AVERROR(EAGAIN); fine.
            while (av_buffersink_get_frame(p->buffersink, outFrame) >= 0) {
                if (m_ring) {
                    const int bytes = outFrame->nb_samples * 2 * sizeof(float);
                    const auto *data = outFrame->extended_data
                        ? outFrame->extended_data[0]
                        : outFrame->data[0];
                    if (data) {
                        m_ring->write(reinterpret_cast<const uint8_t *>(data),
                                      static_cast<std::size_t>(bytes));
                    }
                }
                av_frame_unref(outFrame);
            }
        }
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_frame_free(&outFrame);
}

} // namespace qcv
