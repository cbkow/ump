// FFmpegMetadataExtractor — see header.

#include "ffmpeg_metadata_extractor.h"

#include <QFileInfo>
#include <QtLogging>

#include <cmath>
#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/display.h>
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

// Display-matrix rotation → clockwise display degrees {0,90,180,270}.
// av_display_rotation_get reports counter-clockwise degrees and real
// phone files land near-quarter (−89.97…), so negate then snap.
int rotationFromStream(const AVStream *stream)
{
    const AVCodecParameters *cp = stream->codecpar;
    const AVPacketSideData *sd = av_packet_side_data_get(
        cp->coded_side_data, cp->nb_coded_side_data,
        AV_PKT_DATA_DISPLAYMATRIX);
    if (!sd || sd->size < 9 * sizeof(int32_t)) return 0;
    const double theta =
        av_display_rotation_get(reinterpret_cast<const int32_t *>(sd->data));
    if (std::isnan(theta)) return 0;
    int rot = static_cast<int>(std::lround(-theta)) % 360;
    if (rot < 0) rot += 360;
    return ((rot + 45) / 90 * 90) % 360;
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

// Display-ready codec flavor. ProRes: the MOV fourcc is the
// authoritative variant marker and is present even when the header
// probe leaves codecpar->profile unset; map it to Apple's marketing
// names (the strings users know from export menus). ProRes-in-MXF
// carries no fourcc — fall back to the stream profile, which FFmpeg's
// probe fills from the frame header. Other codecs: FFmpeg's profile
// name when the container declares one ("High 4:2:2", "Main 10");
// empty otherwise and the Inspector row hides.
QString codecProfileName(const AVCodecParameters *cp)
{
    if (cp->codec_id == AV_CODEC_ID_PRORES) {
        switch (cp->codec_tag) {
            case MKTAG('a','p','c','o'): return QStringLiteral("ProRes 422 Proxy");
            case MKTAG('a','p','c','s'): return QStringLiteral("ProRes 422 LT");
            case MKTAG('a','p','c','n'): return QStringLiteral("ProRes 422");
            case MKTAG('a','p','c','h'): return QStringLiteral("ProRes 422 HQ");
            case MKTAG('a','p','4','h'): return QStringLiteral("ProRes 4444");
            case MKTAG('a','p','4','x'): return QStringLiteral("ProRes 4444 XQ");
            default: break;
        }
        switch (cp->profile) {
            case AV_PROFILE_PRORES_PROXY:    return QStringLiteral("ProRes 422 Proxy");
            case AV_PROFILE_PRORES_LT:       return QStringLiteral("ProRes 422 LT");
            case AV_PROFILE_PRORES_STANDARD: return QStringLiteral("ProRes 422");
            case AV_PROFILE_PRORES_HQ:       return QStringLiteral("ProRes 422 HQ");
            case AV_PROFILE_PRORES_4444:     return QStringLiteral("ProRes 4444");
            case AV_PROFILE_PRORES_XQ:       return QStringLiteral("ProRes 4444 XQ");
            default:                         return QStringLiteral("ProRes");
        }
    }
    const char *profile = avcodec_profile_name(cp->codec_id, cp->profile);
    return profile ? QString::fromUtf8(profile) : QString{};
}

// ---- Container identity -------------------------------------------

QString dictValue(AVDictionary *dict, const char *key)
{
    if (!dict) return {};
    AVDictionaryEntry *e = av_dict_get(dict, key, nullptr, 0);
    return (e && e->value) ? QString::fromUtf8(e->value).trimmed() : QString{};
}

// Display-ready container family. libavformat's demuxer names are
// union strings ("mov,mp4,m4a,3gp,3g2,mj2") so the ISO-BMFF family is
// split on the major_brand tag (QuickTime "qt  " vs MP4 brands) with
// the file extension as the tiebreak. Always non-empty for an opened
// file — the empty value is reserved as the "cache predates the
// field" sentinel that triggers the container re-probe.
QString containerFormatName(AVFormatContext *ctx, const QString &filePath)
{
    const QString name = ctx->iformat ? QString::fromUtf8(ctx->iformat->name)
                                      : QString{};
    const QString ext  = QFileInfo(filePath).suffix().toLower();

    if (name == QLatin1String("mxf"))  return QStringLiteral("MXF");
    if (name.startsWith(QLatin1String("mov,"))) {
        const QString brand = dictValue(ctx->metadata, "major_brand").toLower();
        if (brand.startsWith(QLatin1String("qt")))  return QStringLiteral("QuickTime");
        if (brand.startsWith(QLatin1String("3g")))  return QStringLiteral("3GPP");
        if (brand == QLatin1String("mj2") || brand == QLatin1String("mjp2"))
            return QStringLiteral("Motion JPEG 2000");
        if (!brand.isEmpty())                       return QStringLiteral("MP4");
        // No ftyp at all — classic QuickTime writers omit it.
        return ext == QLatin1String("mov") ? QStringLiteral("QuickTime")
                                           : QStringLiteral("MP4");
    }
    if (name.startsWith(QLatin1String("matroska"))) {
        return ext == QLatin1String("webm") ? QStringLiteral("WebM")
                                            : QStringLiteral("Matroska");
    }
    if (name == QLatin1String("avi"))    return QStringLiteral("AVI");
    if (name == QLatin1String("mpegts")) return QStringLiteral("MPEG-TS");
    if (name == QLatin1String("mpeg"))   return QStringLiteral("MPEG-PS");
    if (name == QLatin1String("gxf"))    return QStringLiteral("GXF");
    if (name == QLatin1String("asf"))    return QStringLiteral("ASF / WMV");
    if (name == QLatin1String("wav"))    return QStringLiteral("WAV");
    if (name == QLatin1String("aiff"))   return QStringLiteral("AIFF");
    if (name == QLatin1String("flac"))   return QStringLiteral("FLAC");
    if (name == QLatin1String("mp3"))    return QStringLiteral("MP3");
    if (name == QLatin1String("ogg"))    return QStringLiteral("Ogg");
    if (name == QLatin1String("ivf"))    return QStringLiteral("IVF");
    if (name == QLatin1String("yuv4mpegpipe")) return QStringLiteral("Y4M");
    if (name.startsWith(QLatin1String("image2")))
        return QStringLiteral("Image sequence");

    if (ctx->iformat && ctx->iformat->long_name)
        return QString::fromUtf8(ctx->iformat->long_name);
    return name.isEmpty() ? QStringLiteral("Unknown") : name;
}

// MXF operational pattern from the header partition's OP UL, which
// mxfdec exports as "operational_pattern_ul" ("060e2b34.04010101.
// 0d010201.01010900"). SMPTE ST 377-1: byte 12 = item complexity
// (1/2/3 = OP1/2/3, 0x10 = OP-Atom), byte 13 = package complexity
// (1/2/3 = a/b/c), byte 14 = qualifier bits (multi-track, external
// essence …). Same decode mxfdec does internally (mxfdec.c ~888)
// but it only logs; the tag is the exported form. Empty if absent
// or malformed.
QString mxfOperationalPatternName(AVFormatContext *ctx)
{
    QString ul = dictValue(ctx->metadata, "operational_pattern_ul");
    if (ul.isEmpty()) return {};
    ul.remove(QLatin1Char('.'));
    if (ul.size() != 32) return {};
    bool ok12 = false, ok13 = false;
    const int item = ul.mid(24, 2).toInt(&ok12, 16);
    const int pkg  = ul.mid(26, 2).toInt(&ok13, 16);
    if (!ok12 || !ok13) return {};

    if (item == 0x10) return QStringLiteral("OP-Atom");
    if (item >= 1 && item <= 3) {
        QString op = QStringLiteral("OP%1").arg(item);
        if (pkg >= 1 && pkg <= 3) op += QLatin1Char('a' + (pkg - 1));
        return op;
    }
    // Sony's optical-disc variant (0x40) and anything else: surface
    // the raw bytes so the reviewer still sees *something* useful.
    return QStringLiteral("OP %1.%2")
        .arg(item, 2, 16, QLatin1Char('0'))
        .arg(pkg,  2, 16, QLatin1Char('0'));
}

// Authoring tool. MXF Identification set: company_name /
// product_name / product_version / application_platform →
// "Adobe Media Encoder 13.0.2 (win32)" — the vendor is prepended
// only when the product string doesn't already name it ("Adobe
// Systems Incorporated" + "Adobe Media Encoder" stays short; "ARRI"
// + "ALEXA 35" → "ARRI ALEXA 35"). Everything else: the `encoder`
// tag MOV/MKV/MP4 demuxers fill from ©swr / ©too / WritingApp.
QString encoderToolName(AVFormatContext *ctx)
{
    const QString company = dictValue(ctx->metadata, "company_name");
    QString tool          = dictValue(ctx->metadata, "product_name");
    if (tool.isEmpty()) {
        tool = dictValue(ctx->metadata, "encoder");
        if (tool.isEmpty()) return company;
        return tool;
    }
    if (!company.isEmpty()) {
        const QString firstWord = company.section(QLatin1Char(' '), 0, 0);
        if (!firstWord.isEmpty()
            && !tool.contains(firstWord, Qt::CaseInsensitive)) {
            tool = company + QLatin1Char(' ') + tool;
        }
    }
    const QString version  = dictValue(ctx->metadata, "product_version");
    if (!version.isEmpty()) tool += QLatin1Char(' ') + version;
    const QString platform = dictValue(ctx->metadata, "application_platform");
    if (!platform.isEmpty()) tool += QStringLiteral(" (") + platform + QLatin1Char(')');
    return tool;
}

void fillContainerInfo(AVFormatContext *ctx, const QString &filePath,
                       VideoMetadata &m)
{
    m.containerFormat       = containerFormatName(ctx, filePath);
    m.mxfOperationalPattern = mxfOperationalPatternName(ctx);
    m.encoderTool           = encoderToolName(ctx);

    // Whole-file average. libavformat estimates bit_rate from file
    // size ÷ duration when the container doesn't declare one (MXF);
    // fall back to the same arithmetic ourselves if it left it 0.
    if (ctx->bit_rate > 0) {
        m.containerBitrate = ctx->bit_rate;
    } else if (m.fileSize > 0 && m.duration > 0.0) {
        m.containerBitrate =
            static_cast<qint64>(std::llround(m.fileSize * 8.0 / m.duration));
    }
}

// ---- DNxHD / DNxHR flavor ------------------------------------------
//
// FFmpeg's profile for VC-3 is generic ("DNXHD", "DNXHR HQ") and the
// MXF/MOV descriptors don't carry the variant — the discriminator is
// the 32-bit Compression ID at byte 0x28 of every frame header
// (dnxhddec.c dnxhd_decode_header). We read the first video packet
// (already buffered by avformat_find_stream_info, so no extra I/O of
// note) and map CID × frame-rate bucket to Avid's bandwidth names.
// DNxHD frames are fixed-size per CID, so the packet size × fps is
// also the exact video bitrate. DNxHR (CIDs 1270–1274) is
// resolution-independent and VBR-ish — the class name is the label
// and the rate comes from the container average.

struct DnxHeader {
    int  cid        = 0;
    int  bitDepth   = 0;      // 8 / 10 / 12
    bool interlaced = false;
    int  frameBytes = 0;      // packet size (== coded frame size for DNxHD)
};

bool parseDnxHeader(const uint8_t *buf, int size, DnxHeader &h)
{
    if (!buf || size < 0x2D) return false;
    // Header prefixes accepted by the decoder: 00 00 02 80 {01,02,03}
    // (DNxHD / 444 / DNxHR) and 00 00 03 8C 03 (DNxHR variant).
    const bool prefixA = buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x02
                      && buf[3] == 0x80
                      && (buf[4] == 0x01 || buf[4] == 0x02 || buf[4] == 0x03);
    const bool prefixB = buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x03
                      && buf[3] == 0x8C && buf[4] == 0x03;
    if (!prefixA && !prefixB) return false;

    h.interlaced = (buf[5] & 2) != 0;
    switch (buf[0x21] >> 5) {
        case 1:  h.bitDepth = 8;  break;
        case 2:  h.bitDepth = 10; break;
        case 3:  h.bitDepth = 12; break;
        default: h.bitDepth = 0;  break;
    }
    h.cid = (static_cast<int>(buf[0x28]) << 24) | (buf[0x29] << 16)
          | (buf[0x2A] << 8) | buf[0x2B];
    h.frameBytes = size;
    return h.cid > 0;
}

// Read the first packet of stream `idx` and parse it as a DNx frame
// header. Packets up to a small cap are scanned so interleaved audio
// ahead of the first video frame doesn't defeat us.
bool readFirstDnxHeader(AVFormatContext *ctx, int idx, DnxHeader &out)
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return false;
    bool found = false;
    for (int i = 0; i < 256 && !found; ++i) {
        if (av_read_frame(ctx, pkt) < 0) break;
        if (pkt->stream_index == idx) {
            found = parseDnxHeader(pkt->data, pkt->size, out);
            av_packet_unref(pkt);
            break;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    return found;
}

// Avid DNxHD bandwidth names by CID × frame-rate bucket
// {23.976/24, 25, 29.97/30, 50, 59.94/60}. Numbers are Avid's
// published resolution names (Media Composer / AME / Resolve export
// menus), which are the rounded nominal Mb/s at that rate; 0 = not a
// valid combination. Interlaced CIDs are keyed by frame rate (1080i
// 59.94 = 29.97 fps); a container that reports the field rate falls
// into buckets 3/4, which duplicate 1/2 for those rows. The "x"
// suffix (10-bit) and "444" come from the flags, not the table.
struct DnxHdEntry {
    int  cid;
    bool tenBit;
    bool is444;
    int  mbps[5];
};

const DnxHdEntry kDnxHdTable[] = {
    { 1235, true,  false, { 175, 185, 220, 365, 440 } },   // 1080p 10-bit
    { 1237, false, false, { 115, 120, 145, 240, 290 } },   // 1080p 8-bit mid
    { 1238, false, false, { 175, 185, 220, 365, 440 } },   // 1080p 8-bit
    { 1241, true,  false, {   0, 185, 220, 185, 220 } },   // 1080i 10-bit
    { 1242, false, false, {   0, 120, 145, 120, 145 } },   // 1080i 8-bit mid
    { 1243, false, false, {   0, 185, 220, 185, 220 } },   // 1080i 8-bit
    { 1244, false, false, {   0, 120, 145, 120, 145 } },   // 1440×1080i 8-bit
    { 1250, true,  false, {  90,  90, 110, 175, 220 } },   // 720p 10-bit
    { 1251, false, false, {  90,  90, 110, 175, 220 } },   // 720p 8-bit
    { 1252, false, false, {  60,  60,  75, 115, 145 } },   // 720p 8-bit mid
    { 1253, false, false, {  36,  36,  45,  75,  90 } },   // 1080p 8-bit thin
    { 1256, true,  true,  { 350, 365, 440, 730, 880 } },   // 1080p 4:4:4 10-bit
};

int dnxFpsBucket(double fps)
{
    if (fps < 24.5) return 0;
    if (fps < 27.5) return 1;
    if (fps < 35.0) return 2;
    if (fps < 55.0) return 3;
    return 4;
}

QString dnxProfileName(const DnxHeader &h, double fps)
{
    switch (h.cid) {
        case 1270: return QStringLiteral("DNxHR 444");
        case 1271: return QStringLiteral("DNxHR HQX");
        case 1272: return QStringLiteral("DNxHR HQ");
        case 1273: return QStringLiteral("DNxHR SQ");
        case 1274: return QStringLiteral("DNxHR LB");
        default:   break;
    }
    for (const DnxHdEntry &e : kDnxHdTable) {
        if (e.cid != h.cid) continue;
        const int mbps = fps > 0.0 ? e.mbps[dnxFpsBucket(fps)] : 0;
        if (mbps <= 0) break;
        QString name = e.is444 ? QStringLiteral("DNxHD 444 ")
                               : QStringLiteral("DNxHD ");
        name += QString::number(mbps);
        if (e.tenBit) name += QLatin1Char('x');
        return name;
    }
    // Thin-raster / legacy CIDs (1258/1259/1260) and unknown combos:
    // stay truthful rather than guess a marketing number.
    QString name = QStringLiteral("DNxHD");
    if (h.bitDepth > 8) name += QStringLiteral(" %1-bit").arg(h.bitDepth);
    name += QStringLiteral(" (CID %1)").arg(h.cid);
    return name;
}

bool dnxIsFixedFrameSize(int cid)
{
    return cid >= 1235 && cid <= 1260;   // DNxHD family; DNxHR is VBR
}

// Refine VideoMetadata for a VC-3 stream: flavor label + exact
// bitrate when the CID is a fixed-frame-size DNxHD. Leaves the
// FFmpeg-derived profile in place when the header can't be read.
void refineDnxProfile(AVFormatContext *ctx, int idx, VideoMetadata &m)
{
    DnxHeader h;
    if (!readFirstDnxHeader(ctx, idx, h)) return;
    m.codecProfile = dnxProfileName(h, m.frameRate);
    if (m.videoBitrate <= 0 && dnxIsFixedFrameSize(h.cid)
        && h.frameBytes > 0 && m.frameRate > 0.0) {
        m.videoBitrate = static_cast<qint64>(
            std::llround(h.frameBytes * 8.0 * m.frameRate));
    }
}

void extractVideoStream(AVFormatContext *ctx, VideoMetadata &m)
{
    int idx = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (idx < 0) return;

    AVStream *stream = ctx->streams[idx];
    AVCodecParameters *cp = stream->codecpar;

    m.width  = cp->width;
    m.height = cp->height;

    // Sample aspect ratio — reconciles the stream SAR with the codec
    // SAR (av_guess_* prefers the stream's). SAR ≠ 1:1 ⇒ anamorphic /
    // non-square pixels. Detection only; the applied ratio is the
    // per-clip MediaItem::pixelAspectMode (defaults Square).
    AVRational sar = av_guess_sample_aspect_ratio(ctx, stream, nullptr);
    if (sar.num > 0 && sar.den > 0) {
        m.sarNum = sar.num;
        m.sarDen = sar.den;
    }
    m.isAnamorphic = (m.sarNum != m.sarDen);

    // Display-matrix rotation. Detection only; the applied rotation
    // is the per-clip MediaItem::rotationOverride (defaults Auto =
    // this value).
    m.rotationDeg = rotationFromStream(stream);

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
    m.codecProfile = codecProfileName(cp);
    // Stream-declared rate (MOV/MP4 sample tables; MXF leaves it 0).
    // DNx refinement below fills the exact nominal when it's missing
    // and the CID has a fixed frame size.
    if (cp->bit_rate > 0) m.videoBitrate = cp->bit_rate;
    if (cp->codec_id == AV_CODEC_ID_DNXHD) refineDnxProfile(ctx, idx, m);
    // No decoder for a present video stream = we can't play it (e.g.
    // ARRIRAW camera raw). The viewport surfaces a notice instead of
    // a black frame; see WindowManager load-failure handling.
    m.unsupportedCodec = (codec == nullptr);

    const char *pixName = av_get_pix_fmt_name(static_cast<AVPixelFormat>(cp->format));
    m.pixelFormat = pixName ? QString::fromUtf8(pixName) : QString{};
    m.bitDepth    = bitDepthFromPixFmtName(m.pixelFormat);
    m.hasAlpha    = hasAlphaFromPixFmtName(m.pixelFormat);
    if (const AVPixFmtDescriptor *desc =
            av_pix_fmt_desc_get(static_cast<AVPixelFormat>(cp->format))) {
        m.isRgb = (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    }

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

    // Container vendor/model tags (ARRI MXFs carry company_name=ARRI,
    // product_name="ALEXA 35"). Lets the unsupported-codec notice name
    // the camera. Harmless empty for non-camera containers.
    if (AVDictionaryEntry *e =
            av_dict_get(ctx->metadata, "company_name", nullptr, 0)) {
        m.cameraVendor = QString::fromUtf8(e->value);
    }
    if (AVDictionaryEntry *e =
            av_dict_get(ctx->metadata, "product_name", nullptr, 0)) {
        m.cameraModel = QString::fromUtf8(e->value);
    }

    // Container family / MXF operational pattern / authoring tool /
    // whole-file bitrate. Needs fileSize + duration (set above).
    fillContainerInfo(ctx, filePath, m);

    avformat_close_input(&ctx);
    qInfo("FFmpegMetadataExtractor: end '%s' (video=%dx%d audio=%dch/%dstr)",
          qPrintable(trimmedName), m.width, m.height,
          m.audioChannels, m.audioStreamCount);
    return m;
}

int FFmpegMetadataExtractor::probeRotation(const QString &filePath)
{
    if (filePath.isEmpty()) return -1;
    AVFormatContext *ctx = nullptr;
    if (!openFile(filePath, &ctx)) return -1;
    int rot = -1;
    const int idx =
        av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (idx >= 0) rot = rotationFromStream(ctx->streams[idx]);
    avformat_close_input(&ctx);
    return rot;
}

VideoMetadata FFmpegMetadataExtractor::probeContainer(const QString &filePath)
{
    VideoMetadata m;
    if (filePath.isEmpty()) return m;
    AVFormatContext *ctx = nullptr;
    if (!openFile(filePath, &ctx)) return m;   // loaded=false → retry later

    QFileInfo info(filePath);
    if (info.exists()) m.fileSize = info.size();
    if (ctx->duration != AV_NOPTS_VALUE) {
        m.duration = ctx->duration / static_cast<double>(AV_TIME_BASE);
    }

    const int idx =
        av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (idx >= 0) {
        AVStream *stream = ctx->streams[idx];
        AVCodecParameters *cp = stream->codecpar;
        AVRational fr = stream->r_frame_rate;
        if (fr.num == 0 || fr.den == 0) fr = stream->avg_frame_rate;
        m.frameRate = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 0.0;
        m.codecProfile = codecProfileName(cp);
        if (cp->bit_rate > 0) m.videoBitrate = cp->bit_rate;
        if (cp->codec_id == AV_CODEC_ID_DNXHD) refineDnxProfile(ctx, idx, m);
    }
    fillContainerInfo(ctx, filePath, m);
    m.loaded = true;
    avformat_close_input(&ctx);
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
