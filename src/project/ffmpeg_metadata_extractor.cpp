// FFmpegMetadataExtractor — see header.

#include "ffmpeg_metadata_extractor.h"

#include <QFileInfo>
#include <QtLogging>

#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

namespace qcv {

namespace {

bool openFile(const QString &filePath, AVFormatContext **ctx)
{
    *ctx = avformat_alloc_context();
    if (!*ctx) return false;
    const QByteArray pathUtf8 = filePath.toUtf8();
    if (avformat_open_input(ctx, pathUtf8.constData(), nullptr, nullptr) < 0) {
        return false;
    }
    if (avformat_find_stream_info(*ctx, nullptr) < 0) {
        avformat_close_input(ctx);
        return false;
    }
    return true;
}

QString colorspaceName(AVColorSpace cs)
{
    switch (cs) {
        case AVCOL_SPC_RGB:                return QStringLiteral("rgb");
        case AVCOL_SPC_BT709:              return QStringLiteral("bt709");
        case AVCOL_SPC_FCC:                return QStringLiteral("fcc");
        case AVCOL_SPC_BT470BG:            return QStringLiteral("bt470bg");
        case AVCOL_SPC_SMPTE170M:          return QStringLiteral("smpte170m");
        case AVCOL_SPC_SMPTE240M:          return QStringLiteral("smpte240m");
        case AVCOL_SPC_YCGCO:              return QStringLiteral("ycgco");
        case AVCOL_SPC_BT2020_NCL:         return QStringLiteral("bt2020nc");
        case AVCOL_SPC_BT2020_CL:          return QStringLiteral("bt2020c");
        case AVCOL_SPC_SMPTE2085:          return QStringLiteral("smpte2085");
        case AVCOL_SPC_CHROMA_DERIVED_NCL: return QStringLiteral("chroma-derived-nc");
        case AVCOL_SPC_CHROMA_DERIVED_CL:  return QStringLiteral("chroma-derived-c");
        case AVCOL_SPC_ICTCP:              return QStringLiteral("ictcp");
        default:                           return {};
    }
}

QString primariesName(AVColorPrimaries p)
{
    switch (p) {
        case AVCOL_PRI_BT709:      return QStringLiteral("bt709");
        case AVCOL_PRI_BT470M:     return QStringLiteral("bt470m");
        case AVCOL_PRI_BT470BG:    return QStringLiteral("bt470bg");
        case AVCOL_PRI_SMPTE170M:  return QStringLiteral("smpte170m");
        case AVCOL_PRI_SMPTE240M:  return QStringLiteral("smpte240m");
        case AVCOL_PRI_FILM:       return QStringLiteral("film");
        case AVCOL_PRI_BT2020:     return QStringLiteral("bt2020");
        case AVCOL_PRI_SMPTE428:   return QStringLiteral("smpte428");
        case AVCOL_PRI_SMPTE431:   return QStringLiteral("smpte431");
        case AVCOL_PRI_SMPTE432:   return QStringLiteral("smpte432");
        case AVCOL_PRI_EBU3213:    return QStringLiteral("ebu3213");
        default:                   return {};
    }
}

QString transferName(AVColorTransferCharacteristic t)
{
    switch (t) {
        case AVCOL_TRC_BT709:        return QStringLiteral("bt709");
        case AVCOL_TRC_GAMMA22:      return QStringLiteral("gamma22");
        case AVCOL_TRC_GAMMA28:      return QStringLiteral("gamma28");
        case AVCOL_TRC_SMPTE170M:    return QStringLiteral("smpte170m");
        case AVCOL_TRC_SMPTE240M:    return QStringLiteral("smpte240m");
        case AVCOL_TRC_LINEAR:       return QStringLiteral("linear");
        case AVCOL_TRC_LOG:          return QStringLiteral("log100");
        case AVCOL_TRC_LOG_SQRT:     return QStringLiteral("log316");
        case AVCOL_TRC_IEC61966_2_4: return QStringLiteral("iec61966-2-4");
        case AVCOL_TRC_BT1361_ECG:   return QStringLiteral("bt1361e");
        case AVCOL_TRC_IEC61966_2_1: return QStringLiteral("iec61966-2-1");
        case AVCOL_TRC_BT2020_10:    return QStringLiteral("bt2020-10");
        case AVCOL_TRC_BT2020_12:    return QStringLiteral("bt2020-12");
        case AVCOL_TRC_SMPTE2084:    return QStringLiteral("smpte2084");
        case AVCOL_TRC_SMPTE428:     return QStringLiteral("smpte428");
        case AVCOL_TRC_ARIB_STD_B67: return QStringLiteral("arib-std-b67");
        default:                     return {};
    }
}

QString rangeName(AVColorRange r)
{
    switch (r) {
        case AVCOL_RANGE_MPEG: return QStringLiteral("limited");
        case AVCOL_RANGE_JPEG: return QStringLiteral("full");
        default:               return {};
    }
}

// Detect bit depth from pixel-format name (matches old app's logic).
int bitDepthFromPixFmtName(const QString &name)
{
    if (name.contains(QStringLiteral("16le")) ||
        name.contains(QStringLiteral("16be")) ||
        name.contains(QStringLiteral("p16"))) return 16;
    if (name.contains(QStringLiteral("12le")) ||
        name.contains(QStringLiteral("12be")) ||
        name.contains(QStringLiteral("p12"))) return 12;
    if (name.contains(QStringLiteral("10le")) ||
        name.contains(QStringLiteral("10be")) ||
        name.contains(QStringLiteral("p10"))) return 10;
    return 8;
}

bool hasAlphaFromPixFmtName(const QString &name)
{
    // YUVA / GBRA / RGBA / BGRA / ARGB / ABGR / AYUV / xxxxa.
    return name.startsWith(QLatin1String("yuva")) ||
           name.startsWith(QLatin1String("gbra")) ||
           name.contains(QLatin1String("rgba"))    ||
           name.contains(QLatin1String("bgra"))    ||
           name.contains(QLatin1String("argb"))    ||
           name.contains(QLatin1String("abgr"))    ||
           name.contains(QLatin1String("ayuv"));
}

bool isHdrFromTransfer(const QString &transfer, int bitDepth)
{
    // PQ (smpte2084) or HLG (arib-std-b67) transfer + ≥10-bit = HDR.
    if (bitDepth < 10) return false;
    return transfer == QStringLiteral("smpte2084") ||
           transfer == QStringLiteral("arib-std-b67");
}

QString nclcTagFromIndices(int p, int t, int m)
{
    if (p == 0 && t == 0 && m == 0) return {};
    return QStringLiteral("%1-%2-%3").arg(p).arg(t).arg(m);
}

QString readEmbeddedTimecode(AVFormatContext *ctx, AVStream *videoStream)
{
    if (!ctx) return {};
    auto readKey = [](AVDictionary *dict) -> QString {
        if (!dict) return {};
        AVDictionaryEntry *e = av_dict_get(dict, "timecode", nullptr, 0);
        return (e && e->value) ? QString::fromUtf8(e->value) : QString{};
    };

    if (videoStream) {
        QString tc = readKey(videoStream->metadata);
        if (!tc.isEmpty()) return tc;
    }
    QString tc = readKey(ctx->metadata);
    if (!tc.isEmpty()) return tc;

    // Walk all streams (some containers put TC on a dedicated data
    // stream).
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
        tc = readKey(ctx->streams[i]->metadata);
        if (!tc.isEmpty()) return tc;
    }
    return {};
}

void extractVideoStream(AVFormatContext *ctx, VideoMetadata &m)
{
    int idx = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (idx < 0) return;

    AVStream *stream = ctx->streams[idx];
    AVCodecParameters *cp = stream->codecpar;

    m.width  = cp->width;
    m.height = cp->height;

    AVRational fr = stream->r_frame_rate;
    if (fr.num == 0 || fr.den == 0) fr = stream->avg_frame_rate;
    m.frameRate = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 24.0;

    if (ctx->duration != AV_NOPTS_VALUE) {
        m.duration = ctx->duration / static_cast<double>(AV_TIME_BASE);
        // Round, don't truncate — `static_cast<int>` was producing 720
        // for 30.0717s × 23.976024fps which is mathematically 721 but
        // lands at ~720.99999 in double precision. The other count
        // sites (VideoDecoder.frameCount via st->nb_frames,
        // PlaybackTimer.totalFrames via lround) report 721; aligning
        // here keeps the inspector consistent with the timeline.
        m.totalFrames = static_cast<int>(std::lround(m.duration * m.frameRate));
    }

    const AVCodec *codec = avcodec_find_decoder(cp->codec_id);
    m.videoCodec = codec ? QString::fromUtf8(codec->name)
                         : QStringLiteral("unknown");

    const char *pixName = av_get_pix_fmt_name(static_cast<AVPixelFormat>(cp->format));
    m.pixelFormat = pixName ? QString::fromUtf8(pixName) : QString{};
    m.bitDepth    = bitDepthFromPixFmtName(m.pixelFormat);
    m.hasAlpha    = hasAlphaFromPixFmtName(m.pixelFormat);

    m.colorspace      = colorspaceName(cp->color_space);
    m.colorPrimaries  = primariesName(cp->color_primaries);
    m.colorTransfer   = transferName(cp->color_trc);
    m.colorRange      = rangeName(cp->color_range);
    m.nclcTag         = nclcTagFromIndices(cp->color_primaries,
                                           cp->color_trc,
                                           cp->color_space);
    m.isHdrContent    = isHdrFromTransfer(m.colorTransfer, m.bitDepth);

    QString tc = readEmbeddedTimecode(ctx, stream);
    if (!tc.isEmpty()) {
        m.hasEmbeddedTimecode = true;
        m.embeddedTimecode    = tc;
    }
}

void extractAudioStream(AVFormatContext *ctx, VideoMetadata &m)
{
    // Enumerate ALL audio streams, not just av_find_best_stream's
    // pick. Broadcast deliverables (ProRes MOV, DNX MXF, IMF MXF)
    // commonly pack each "track" as a separate mono audio stream
    // — an "8-track" 5.1+stereo master is 8 streams of 1 channel
    // each, NOT one stream of 8 channels. Picking best-stream and
    // reporting `nb_channels` from it would say "mono" for files
    // Premiere and Resolve correctly show as 8 channels.
    int totalChannels = 0;
    int audioStreamCount = 0;
    int firstAudioIdx = -1;
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
        AVStream *s = ctx->streams[i];
        if (!s || !s->codecpar) continue;
        if (s->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        ++audioStreamCount;
        totalChannels += s->codecpar->ch_layout.nb_channels;
        if (firstAudioIdx < 0) firstAudioIdx = static_cast<int>(i);
    }
    if (firstAudioIdx < 0) return;   // No audio in this file.

    // Codec name + sample rate are read from the first audio stream
    // — broadcast multi-track masters carry the same codec / rate
    // on every track by convention; mixed-codec audio in one file
    // is exceedingly rare.
    AVCodecParameters *cp = ctx->streams[firstAudioIdx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(cp->codec_id);
    m.audioCodec       = codec ? QString::fromUtf8(codec->name)
                               : QStringLiteral("unknown");
    m.audioSampleRate  = cp->sample_rate;
    m.audioChannels    = totalChannels;
    m.audioStreamCount = audioStreamCount;

    // Channel-layout name. Three shapes:
    //   1. Single multi-channel stream with a valid layout
    //      → use FFmpeg's describe ("stereo", "5.1(side)", etc.).
    //   2. Multiple mono streams (broadcast multi-track convention)
    //      → "N mono tracks" — gives the reviewer the right
    //      mental model.
    //   3. Anything else (single stream with unspecified order, or
    //      mixed channel counts across streams) → "N channels".
    //
    // Defensive: skip describe when the FFmpeg state is half-init
    // (some MXF descriptors land here) — the check on order keeps
    // the extract from asserting on edge-case input.
    if (audioStreamCount == 1
        && cp->ch_layout.nb_channels > 0
        && cp->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC) {
        char layoutBuf[64] = {0};
        if (av_channel_layout_describe(&cp->ch_layout,
                                         layoutBuf, sizeof(layoutBuf)) > 0) {
            m.audioChannelLayoutName = QString::fromUtf8(layoutBuf);
        }
    } else if (audioStreamCount > 1
               && totalChannels == audioStreamCount) {
        // Every stream is exactly 1 channel — classic multi-mono
        // broadcast layout.
        m.audioChannelLayoutName =
            QString::number(audioStreamCount)
            + QStringLiteral(" mono tracks");
    } else if (totalChannels > 0) {
        m.audioChannelLayoutName =
            QString::number(totalChannels)
            + QStringLiteral(" channels");
    }
}

} // namespace

VideoMetadata FFmpegMetadataExtractor::extract(const QString &filePath)
{
    VideoMetadata m;
    if (filePath.isEmpty()) return m;

    // TRACE_FFMPEG_OPEN — start. Lets us correlate concurrent
    // avformat_open_input racing against the playback path's own
    // open. Background-thread origin shows up in the t= column.
    const QString trimmedName = QFileInfo(filePath).fileName();
    qInfo("FFmpegMetadataExtractor: begin '%s'", qPrintable(trimmedName));

    AVFormatContext *ctx = nullptr;
    if (!openFile(filePath, &ctx)) {
        qInfo("FFmpegMetadataExtractor: end (open failed) '%s'",
              qPrintable(trimmedName));
        return m;
    }

    QFileInfo info(filePath);
    if (info.exists()) m.fileSize = info.size();

    if (ctx->duration != AV_NOPTS_VALUE) {
        m.duration = ctx->duration / static_cast<double>(AV_TIME_BASE);
    }

    extractVideoStream(ctx, m);
    // Set loaded=true BEFORE the audio extract — the inspector's
    // visibility gates ("Reading metadata…" → real sections) flip
    // off this flag, and we'd rather show video metadata + an empty
    // audio block than have the whole inspector stuck on the
    // sentinel if an edge-case audio descriptor takes the audio
    // extract path down (recovery would mean re-loading the file).
    // Audio fields are pre-zeroed; if extract succeeds it overwrites
    // them, otherwise the inspector just shows "0 channels" which
    // is at least diagnostically useful.
    m.loaded = true;
    extractAudioStream(ctx, m);

    avformat_close_input(&ctx);
    qInfo("FFmpegMetadataExtractor: end '%s' (video=%dx%d audio=%dch/%dstr)",
          qPrintable(trimmedName), m.width, m.height,
          m.audioChannels, m.audioStreamCount);
    return m;
}

double FFmpegMetadataExtractor::probeDuration(const QString &filePath)
{
    if (filePath.isEmpty()) return 0.0;
    AVFormatContext *ctx = nullptr;
    if (!openFile(filePath, &ctx)) return 0.0;
    const double d = (ctx->duration != AV_NOPTS_VALUE)
        ? ctx->duration / static_cast<double>(AV_TIME_BASE)
        : 0.0;
    avformat_close_input(&ctx);
    return d;
}

} // namespace qcv
