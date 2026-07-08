#include "exr_image_loader.h"

#include "memory_mapped_istream.h"

#include <Imath/ImathBox.h>
#include <Imath/half.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfInputPart.h>
#include <OpenEXR/ImfMultiPartInputFile.h>
#include <OpenEXR/ImfThreading.h>

#include <QtLogging>
#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace qcv {

namespace {

// Locate the part inside a multi-part EXR whose header name matches
// `layer`. Returns 0 (first part) when the file has only one part
// or when no match is found.
int partIndexForLayer(const Imf::MultiPartInputFile &file,
                      const std::string &layer)
{
    if (layer.empty()) return 0;
    const int parts = file.parts();
    if (parts <= 1) return 0;
    for (int p = 0; p < parts; ++p) {
        const Imf::Header &h = file.header(p);
        if (h.hasName() && h.name() == layer) return p;
    }
    return 0;
}

// Lazily size OpenEXR's global worker pool the first time a
// latency-tier decode asks for it. IMPORTANT side effect: once the
// pool exists, any Imf file opened WITHOUT an explicit numThreads
// would default to it — which is why every open in this file passes
// its thread count explicitly (0 = synchronous, the playback
// default). 8 threads is probe-validated on 4K DWAB (225→56 ms);
// clamped for small machines.
void ensureDecodePool()
{
    static std::once_flag once;
    std::call_once(once, []() {
        const unsigned hw = std::thread::hardware_concurrency();
        Imf::setGlobalThreadCount(
            static_cast<int>(std::clamp(hw / 2, 2u, 8u)));
    });
}

// First layer prefix owning an R channel. Fallback for empty-layer
// reads of files whose channels are ALL prefixed — e.g. Blender
// multi-part renders name part 0's channels "ViewLayer.Combined.R",
// so the bare "R/G/B" probe finds nothing and, before this
// fallback, loadFrame failed outright (2026-07-08 audit find).
std::string firstPrefixedLayer(const Imf::ChannelList &channels)
{
    for (auto it = channels.begin(); it != channels.end(); ++it) {
        const std::string n = it.name();
        if (n.size() > 2 && n.compare(n.size() - 2, 2, ".R") == 0) {
            return n.substr(0, n.size() - 2);
        }
    }
    return {};
}

} // namespace

std::shared_ptr<PixelData> EXRImageLoader::loadFrame(
    const std::string &path,
    const std::string &layerOverride,
    PipelineMode /*pipeline_mode*/)
{
    const std::string layer =
        layerOverride.empty() ? m_layer : layerOverride;

    try {
        // Phase 7.4.b.2: route through MemoryMappedIStream so the
        // OpenEXR decompressor reads via mmap (kernel page-cache +
        // sequential prefetch) instead of plain read syscalls.
        // 4K/8K EXRs would otherwise saturate the syscall path
        // before the disk's actual throughput.
        if (m_decodeThreads > 0) ensureDecodePool();
        auto stream = std::make_unique<MemoryMappedIStream>(path);
        // Explicit thread count — see ensureDecodePool. 0 keeps the
        // playback path synchronous even after the pool exists.
        Imf::MultiPartInputFile file(*stream, m_decodeThreads);

        const int targetPart = partIndexForLayer(file, layer);
        const Imf::Header &header = file.header(targetPart);
        const Imath::Box2i dispWin = header.displayWindow();
        const Imath::Box2i dataWin = header.dataWindow();
        const bool fastPath = (dispWin == dataWin);

        const int width  = dispWin.max.x - dispWin.min.x + 1;
        const int height = dispWin.max.y - dispWin.min.y + 1;
        if (width <= 0 || height <= 0) return nullptr;

        auto pd = std::make_shared<PixelData>();
        pd->width  = width;
        pd->height = height;
        pd->setFormat(PixelFormat::RGBA16F);
        pd->pipeline_mode = PipelineMode::ULTRA_HIGH_RES;

        const std::size_t pixelCount =
            static_cast<std::size_t>(width) * height * 4;
        pd->pixels.resize(pixelCount * sizeof(Imath::half));
        Imath::half *halves =
            reinterpret_cast<Imath::half *>(pd->pixels.data());

        // Channel discovery. Try the layer's "<layer>.R/G/B/A" first;
        // fall back to root "R/G/B/A" when a layer was requested but
        // the file's channel list doesn't carry the prefix.
        const Imf::ChannelList &channels = header.channels();
        const std::string prefix = layer.empty() ? std::string() : layer + ".";
        std::string nR = prefix + "R";
        std::string nG = prefix + "G";
        std::string nB = prefix + "B";
        std::string nA = prefix + "A";
        const Imf::Channel *cR = channels.findChannel(nR.c_str());
        const Imf::Channel *cG = channels.findChannel(nG.c_str());
        const Imf::Channel *cB = channels.findChannel(nB.c_str());
        const Imf::Channel *cA = channels.findChannel(nA.c_str());
        if (!cR && !layer.empty()) {
            nR = "R"; nG = "G"; nB = "B"; nA = "A";
            cR = channels.findChannel("R");
            cG = channels.findChannel("G");
            cB = channels.findChannel("B");
            cA = channels.findChannel("A");
        }
        if (!cR && layer.empty()) {
            // No bare R/G/B and no layer requested — files whose
            // channels are all prefixed (Blender multi-part) land
            // here. Fall back to the first image-bearing prefix.
            const std::string fb = firstPrefixedLayer(channels);
            if (!fb.empty()) {
                nR = fb + ".R"; nG = fb + ".G";
                nB = fb + ".B"; nA = fb + ".A";
                cR = channels.findChannel(nR.c_str());
                cG = channels.findChannel(nG.c_str());
                cB = channels.findChannel(nB.c_str());
                cA = channels.findChannel(nA.c_str());
            }
        }
        if (!cR || !cG || !cB) {
            qWarning("EXRImageLoader: missing RGB channels in '%s' "
                     "(layer='%s')", path.c_str(), layer.c_str());
            return nullptr;
        }
        const bool hasAlpha = (cA != nullptr);
        const std::string nameByCh[4] = { nR, nG, nB, nA };
        const int numCh = hasAlpha ? 4 : 3;

        Imf::FrameBuffer fb;
        Imf::InputPart part(file, targetPart);

        if (fastPath) {
            // displayWindow == dataWindow — the slice can write
            // directly into the destination buffer with no fixup.
            const std::size_t channelBytes = sizeof(Imath::half);
            const std::size_t cb  = 4 * channelBytes;
            const std::size_t scb = static_cast<std::size_t>(width) * cb;

            for (int c = 0; c < numCh; ++c) {
                fb.insert(nameByCh[c].c_str(),
                    Imf::Slice(
                        Imf::HALF,
                        reinterpret_cast<char *>(halves)
                            + (c * channelBytes),
                        cb, scb, 1, 1, 0.0f));
            }
            // Synthesize opaque alpha when source has none.
            if (!hasAlpha) {
                for (int i = 0; i < width * height; ++i) {
                    halves[i * 4 + 3] = Imath::half(1.0f);
                }
            }
            part.setFrameBuffer(fb);
            part.readPixels(dispWin.min.y, dispWin.max.y);
        } else {
            // displayWindow ≠ dataWindow — read scanline-by-scanline
            // into a temp buffer keyed off dataWindow, then memcpy
            // the intersected portion into the destination row,
            // zero-filling the gaps. This matches the old app's
            // SLOW PATH at image_loaders.cpp:1141-1192.
            const Imath::Box2i isect(
                Imath::V2i(std::max(dispWin.min.x, dataWin.min.x),
                           std::max(dispWin.min.y, dataWin.min.y)),
                Imath::V2i(std::min(dispWin.max.x, dataWin.max.x),
                           std::min(dispWin.max.y, dataWin.max.y)));

            const int dataWidth = dataWin.max.x - dataWin.min.x + 1;
            const std::size_t channelBytes = sizeof(Imath::half);
            const std::size_t cb       = 4 * channelBytes;
            const std::size_t bufSize  = dataWidth * cb;
            std::vector<char> buf(bufSize);

            for (int c = 0; c < numCh; ++c) {
                fb.insert(nameByCh[c].c_str(),
                    Imf::Slice(
                        Imf::HALF,
                        buf.data() - (dataWin.min.x * cb)
                                   + (c * channelBytes),
                        cb, 0, 1, 1, 0.0f));
            }
            part.setFrameBuffer(fb);

            const std::size_t scb =
                static_cast<std::size_t>(width) * cb;
            for (int y = dispWin.min.y; y <= dispWin.max.y; ++y) {
                std::uint8_t *p =
                    pd->pixels.data() +
                    static_cast<std::size_t>(y - dispWin.min.y) * scb;
                std::uint8_t *end = p + scb;

                if (y >= isect.min.y && y <= isect.max.y) {
                    const std::size_t leftPad =
                        static_cast<std::size_t>(
                            isect.min.x - dispWin.min.x) * cb;
                    std::memset(p, 0, leftPad);
                    p += leftPad;

                    const std::size_t copyBytes =
                        static_cast<std::size_t>(
                            isect.max.x - isect.min.x + 1) * cb;
                    part.readPixels(y, y);
                    std::memcpy(p,
                        buf.data() +
                            std::max(dispWin.min.x - dataWin.min.x, 0) * cb,
                        copyBytes);
                    p += copyBytes;
                }
                std::memset(p, 0, end - p);
            }

            if (!hasAlpha) {
                for (int i = 0; i < width * height; ++i) {
                    halves[i * 4 + 3] = Imath::half(1.0f);
                }
            }
        }

        return pd;

    } catch (const std::exception &e) {
        qWarning("EXRImageLoader: exception loading '%s': %s",
                 path.c_str(), e.what());
        return nullptr;
    }
}

std::shared_ptr<PixelData> EXRImageLoader::loadThumbnail(
    const std::string &path, int max_size)
{
    // Scanline-skip thumbnail. Read every Nth scanline directly into
    // a downsized destination buffer instead of decoding the full
    // image and resizing afterwards. For a 6K EXR @ max_size=320,
    // skip_factor ≈ 19 → reads only ~170 of 3240 scanlines, and
    // OpenEXR only decompresses the requested rows. The tradeoff:
    // for DWAB (256-line blocks) we still decompress full blocks
    // even when reading a single line out of one — but the kernel
    // cost is bounded and contention with the main playback path is
    // far better than allocating 150 MB+ buffers per hover.
    if (max_size <= 0) max_size = 320;

    try {
        auto stream = std::make_unique<MemoryMappedIStream>(path);
        Imf::MultiPartInputFile file(*stream, /*numThreads=*/0);

        const int targetPart = partIndexForLayer(file, m_layer);
        const Imf::Header &header = file.header(targetPart);
        const Imath::Box2i dispWin = header.displayWindow();

        const int fullW = dispWin.max.x - dispWin.min.x + 1;
        const int fullH = dispWin.max.y - dispWin.min.y + 1;
        if (fullW <= 0 || fullH <= 0) return nullptr;

        const int maxDim = std::max(fullW, fullH);
        const int skip   = std::max(1, maxDim / max_size);
        const int thumbW = std::max(1, fullW / skip);
        const int thumbH = std::max(1, fullH / skip);

        auto pd = std::make_shared<PixelData>();
        pd->width  = thumbW;
        pd->height = thumbH;
        pd->setFormat(PixelFormat::RGBA16F);
        pd->pipeline_mode = PipelineMode::ULTRA_HIGH_RES;
        const std::size_t pixelCount =
            static_cast<std::size_t>(thumbW) * thumbH * 4;
        pd->pixels.resize(pixelCount * sizeof(Imath::half));
        Imath::half *dst =
            reinterpret_cast<Imath::half *>(pd->pixels.data());

        // Channel discovery (same fallback rules as loadFrame).
        const Imf::ChannelList &channels = header.channels();
        const std::string prefix =
            m_layer.empty() ? std::string() : m_layer + ".";
        std::string nR = prefix + "R";
        std::string nG = prefix + "G";
        std::string nB = prefix + "B";
        std::string nA = prefix + "A";
        const Imf::Channel *cR = channels.findChannel(nR.c_str());
        const Imf::Channel *cG = channels.findChannel(nG.c_str());
        const Imf::Channel *cB = channels.findChannel(nB.c_str());
        const Imf::Channel *cA = channels.findChannel(nA.c_str());
        if (!cR && !m_layer.empty()) {
            nR = "R"; nG = "G"; nB = "B"; nA = "A";
            cR = channels.findChannel("R");
            cG = channels.findChannel("G");
            cB = channels.findChannel("B");
            cA = channels.findChannel("A");
        }
        if (!cR && m_layer.empty()) {
            // Same all-channels-prefixed fallback as loadFrame.
            const std::string fb = firstPrefixedLayer(channels);
            if (!fb.empty()) {
                nR = fb + ".R"; nG = fb + ".G";
                nB = fb + ".B"; nA = fb + ".A";
                cR = channels.findChannel(nR.c_str());
                cG = channels.findChannel(nG.c_str());
                cB = channels.findChannel(nB.c_str());
                cA = channels.findChannel(nA.c_str());
            }
        }
        if (!cR || !cG || !cB) {
            qWarning("EXRImageLoader::loadThumbnail: missing RGB "
                     "channels in '%s'", path.c_str());
            return nullptr;
        }
        const bool hasAlpha = (cA != nullptr);

        Imf::InputPart part(file, targetPart);

        // Scanline temp buffer — interleaved RGBA16F at full source
        // width. We feed the slice base pointer as
        // `base - dispWin.min.x * cb` so the slice maps source pixel
        // x=dispWin.min.x to scanline_buffer[0]. Same trick as the
        // slow-path branch in loadFrame, but only for one row at a
        // time.
        std::vector<Imath::half> scanline(
            static_cast<std::size_t>(fullW) * 4);
        const std::size_t channelBytes = sizeof(Imath::half);
        const std::size_t cb = 4 * channelBytes;

        Imf::FrameBuffer fb;
        fb.insert(nR.c_str(), Imf::Slice(
            Imf::HALF,
            reinterpret_cast<char *>(scanline.data())
                - dispWin.min.x * cb,
            cb, 0, 1, 1, 0.0f));
        fb.insert(nG.c_str(), Imf::Slice(
            Imf::HALF,
            reinterpret_cast<char *>(scanline.data() + 1)
                - dispWin.min.x * cb,
            cb, 0, 1, 1, 0.0f));
        fb.insert(nB.c_str(), Imf::Slice(
            Imf::HALF,
            reinterpret_cast<char *>(scanline.data() + 2)
                - dispWin.min.x * cb,
            cb, 0, 1, 1, 0.0f));
        if (hasAlpha) {
            fb.insert(nA.c_str(), Imf::Slice(
                Imf::HALF,
                reinterpret_cast<char *>(scanline.data() + 3)
                    - dispWin.min.x * cb,
                cb, 0, 1, 1, 0.0f));
        }
        part.setFrameBuffer(fb);

        for (int ty = 0; ty < thumbH; ++ty) {
            const int sy = dispWin.min.y + ty * skip;
            if (sy > dispWin.max.y) break;
            part.readPixels(sy, sy);

            for (int tx = 0; tx < thumbW; ++tx) {
                const int sx = tx * skip;
                if (sx >= fullW) break;
                const std::size_t srcIdx =
                    static_cast<std::size_t>(sx) * 4;
                const std::size_t dstIdx =
                    (static_cast<std::size_t>(ty) * thumbW + tx) * 4;
                dst[dstIdx + 0] = scanline[srcIdx + 0];
                dst[dstIdx + 1] = scanline[srcIdx + 1];
                dst[dstIdx + 2] = scanline[srcIdx + 2];
                dst[dstIdx + 3] = hasAlpha
                                  ? scanline[srcIdx + 3]
                                  : Imath::half(1.0f);
            }
        }

        return pd;

    } catch (const std::exception &e) {
        qWarning("EXRImageLoader::loadThumbnail: '%s' — %s",
                 path.c_str(), e.what());
        return nullptr;
    }
}

bool EXRImageLoader::getDimensions(const std::string &path,
                                   int &width, int &height)
{
    try {
        auto stream = std::make_unique<MemoryMappedIStream>(path);
        Imf::InputFile file(*stream, /*numThreads=*/0);
        const Imath::Box2i dw = file.header().displayWindow();
        width  = dw.max.x - dw.min.x + 1;
        height = dw.max.y - dw.min.y + 1;
        return width > 0 && height > 0;
    } catch (const std::exception &) {
        return false;
    }
}

namespace {
// Cryptomatte filter — old QCView's exr_layer_detector.cpp:473-480
// uses a single criterion: case-insensitive substring "crypto".
// Cryptomatte layers carry hash IDs in lowercase rgba channels,
// not displayable image data, so we strip them from the picker.
bool isCryptomatte(const std::string &name)
{
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lower.find("crypto") != std::string::npos;
}
} // namespace

std::vector<std::string>
EXRImageLoader::discoverLayers(const std::string &path)
{
    std::vector<std::string> out;
    try {
        auto stream = std::make_unique<MemoryMappedIStream>(path);
        Imf::MultiPartInputFile file(*stream, /*numThreads=*/0);
        const int parts = file.parts();

        // Multi-part path. Each part's name() identifies the layer
        // for end-user picking; nameless parts surface as "default
        // (part N)". Cryptomatte parts are filtered — non-image
        // data, not useful in a viewer.
        if (parts > 1) {
            for (int p = 0; p < parts; ++p) {
                const Imf::Header &h = file.header(p);
                const std::string name =
                    (h.hasName() && !h.name().empty())
                    ? h.name()
                    : ("default (part " + std::to_string(p) + ")");
                if (isCryptomatte(name)) continue;
                out.push_back(name);
            }
            return out;
        }

        // Single-part path. Parse layer prefix tokens from the
        // channel list. Channels are typically "<layer>.R / .G /
        // .B / .A" — we extract everything before the LAST dot
        // and dedupe. Channels with no dot get bucketed under
        // "default" (the bare RGB case). MultiPartInputFile::header
        // requires an explicit part index — pass 0 for the lone part.
        const Imf::ChannelList &channels = file.header(0).channels();
        std::vector<std::string> uniq;
        bool sawBare = false;
        for (auto it = channels.begin(); it != channels.end(); ++it) {
            const std::string n = it.name();
            const std::size_t dot = n.find_last_of('.');
            if (dot == std::string::npos) {
                sawBare = true;
                continue;
            }
            const std::string prefix = n.substr(0, dot);
            if (isCryptomatte(prefix)) continue;
            if (std::find(uniq.begin(), uniq.end(), prefix) == uniq.end()) {
                uniq.push_back(prefix);
            }
        }
        if (sawBare) out.push_back("default");
        for (const auto &p : uniq) out.push_back(p);
        if (out.empty()) out.push_back("default");
        return out;

    } catch (const std::exception &) {
        return {};
    }
}

std::map<std::string, std::shared_ptr<PixelData>>
EXRImageLoader::loadThumbnailsAllLayers(const std::string &path,
                                        int max_size)
{
    if (max_size <= 0) max_size = 480;
    std::map<std::string, std::shared_ptr<PixelData>> out;

    try {
        auto stream = std::make_unique<MemoryMappedIStream>(path);
        Imf::MultiPartInputFile file(*stream, /*numThreads=*/0);
        // Multi-part: each part is compressed independently, so a
        // one-pass sweep buys nothing over per-layer loadThumbnail
        // calls. Signal "not applicable" and let the caller fan out.
        if (file.parts() != 1) return out;

        const Imf::Header &header = file.header(0);
        const Imath::Box2i dispWin = header.displayWindow();
        const int fullW = dispWin.max.x - dispWin.min.x + 1;
        const int fullH = dispWin.max.y - dispWin.min.y + 1;
        if (fullW <= 0 || fullH <= 0) return out;

        const int maxDim = std::max(fullW, fullH);
        const int skip   = std::max(1, maxDim / max_size);
        const int thumbW = std::max(1, fullW / skip);
        const int thumbH = std::max(1, fullH / skip);

        // Layer discovery — same rules as discoverLayers' single-part
        // branch (prefix before the LAST dot, crypto filtered, bare
        // channels bucketed under "default").
        const Imf::ChannelList &channels = header.channels();
        std::vector<std::string> layerNames;
        bool sawBare = false;
        for (auto it = channels.begin(); it != channels.end(); ++it) {
            const std::string n = it.name();
            const std::size_t dot = n.find_last_of('.');
            if (dot == std::string::npos) { sawBare = true; continue; }
            const std::string prefix = n.substr(0, dot);
            if (isCryptomatte(prefix)) continue;
            if (std::find(layerNames.begin(), layerNames.end(), prefix)
                == layerNames.end()) {
                layerNames.push_back(prefix);
            }
        }
        if (sawBare) layerNames.insert(layerNames.begin(), "default");
        if (layerNames.empty()) layerNames.push_back("default");

        // Per-layer channel resolution, deduplicated: layers whose
        // prefixed R/G/B don't exist (data AOVs like "depth" whose
        // only channel is depth.Z) fall back to the bare-RGB image —
        // several layers may therefore resolve to the SAME channels,
        // and Imf::FrameBuffer keys slices by channel name, so each
        // distinct channel set gets ONE scanline buffer + one dst;
        // aliases share the resulting PixelData.
        struct LayerRead {
            std::string rName, gName, bName, aName; // aName empty = opaque
            std::vector<Imath::half> scanline;      // fullW * 4
            std::shared_ptr<PixelData> dst;
        };
        std::vector<LayerRead> reads;
        // layer name → index into `reads`
        std::map<std::string, std::size_t> layerToRead;
        std::map<std::string, std::size_t> channelKeyToRead;

        for (const std::string &layer : layerNames) {
            const std::string prefix =
                (layer == "default") ? std::string() : layer + ".";
            std::string nR = prefix + "R", nG = prefix + "G",
                        nB = prefix + "B", nA = prefix + "A";
            if (!channels.findChannel(nR.c_str())) {
                nR = "R"; nG = "G"; nB = "B"; nA = "A";
            }
            if (!channels.findChannel(nR.c_str())
                || !channels.findChannel(nG.c_str())
                || !channels.findChannel(nB.c_str())) {
                continue;   // no displayable channels anywhere
            }
            const bool hasAlpha =
                channels.findChannel(nA.c_str()) != nullptr;

            const std::string key = nR;   // R name identifies the set
            auto found = channelKeyToRead.find(key);
            if (found != channelKeyToRead.end()) {
                layerToRead[layer] = found->second;
                continue;
            }

            LayerRead lr;
            lr.rName = nR; lr.gName = nG; lr.bName = nB;
            lr.aName = hasAlpha ? nA : std::string();
            lr.scanline.resize(static_cast<std::size_t>(fullW) * 4);
            lr.dst = std::make_shared<PixelData>();
            lr.dst->width  = thumbW;
            lr.dst->height = thumbH;
            lr.dst->setFormat(PixelFormat::RGBA16F);
            lr.dst->pipeline_mode = PipelineMode::ULTRA_HIGH_RES;
            lr.dst->pixels.resize(
                static_cast<std::size_t>(thumbW) * thumbH * 4
                * sizeof(Imath::half));
            reads.push_back(std::move(lr));
            channelKeyToRead[key] = reads.size() - 1;
            layerToRead[layer]    = reads.size() - 1;
        }
        if (reads.empty()) return out;

        // One FrameBuffer carrying every distinct channel set —
        // OpenEXR decompresses each block once and fills all slices.
        // Slice bases offset by -dispWin.min.x*cb so source pixel
        // x=dispWin.min.x lands at scanline[0] (same trick as
        // loadThumbnail).
        const std::size_t channelBytes = sizeof(Imath::half);
        const std::size_t cb = 4 * channelBytes;
        Imf::FrameBuffer fb;
        for (LayerRead &lr : reads) {
            char *base = reinterpret_cast<char *>(lr.scanline.data());
            fb.insert(lr.rName.c_str(), Imf::Slice(
                Imf::HALF, base - dispWin.min.x * cb,
                cb, 0, 1, 1, 0.0f));
            fb.insert(lr.gName.c_str(), Imf::Slice(
                Imf::HALF, base + 1 * channelBytes - dispWin.min.x * cb,
                cb, 0, 1, 1, 0.0f));
            fb.insert(lr.bName.c_str(), Imf::Slice(
                Imf::HALF, base + 2 * channelBytes - dispWin.min.x * cb,
                cb, 0, 1, 1, 0.0f));
            if (!lr.aName.empty()) {
                fb.insert(lr.aName.c_str(), Imf::Slice(
                    Imf::HALF, base + 3 * channelBytes - dispWin.min.x * cb,
                    cb, 0, 1, 1, 0.0f));
            }
        }

        Imf::InputPart part(file, 0);
        part.setFrameBuffer(fb);

        for (int ty = 0; ty < thumbH; ++ty) {
            const int sy = dispWin.min.y + ty * skip;
            if (sy > dispWin.max.y) break;
            part.readPixels(sy, sy);

            for (LayerRead &lr : reads) {
                Imath::half *dst =
                    reinterpret_cast<Imath::half *>(lr.dst->pixels.data());
                const bool hasAlpha = !lr.aName.empty();
                for (int tx = 0; tx < thumbW; ++tx) {
                    const int sx = tx * skip;
                    if (sx >= fullW) break;
                    const std::size_t srcIdx =
                        static_cast<std::size_t>(sx) * 4;
                    const std::size_t dstIdx =
                        (static_cast<std::size_t>(ty) * thumbW + tx) * 4;
                    dst[dstIdx + 0] = lr.scanline[srcIdx + 0];
                    dst[dstIdx + 1] = lr.scanline[srcIdx + 1];
                    dst[dstIdx + 2] = lr.scanline[srcIdx + 2];
                    dst[dstIdx + 3] = hasAlpha
                                      ? lr.scanline[srcIdx + 3]
                                      : Imath::half(1.0f);
                }
            }
        }

        for (const auto &[layer, readIdx] : layerToRead) {
            out[layer] = reads[readIdx].dst;
        }
        return out;

    } catch (const std::exception &e) {
        qWarning("EXRImageLoader::loadThumbnailsAllLayers: '%s' — %s",
                 path.c_str(), e.what());
        return {};
    }
}

std::string
EXRImageLoader::compressionName(const std::string &path)
{
    try {
        auto stream = std::make_unique<MemoryMappedIStream>(path);
        Imf::MultiPartInputFile file(*stream, /*numThreads=*/0);
        if (file.parts() <= 0) return {};
        // First part's compression. Multi-part files can vary per part
        // in theory, but in practice review deliverables use the same
        // compression across parts — showing one value is the right
        // tradeoff vs. a per-part list in the inspector.
        switch (file.header(0).compression()) {
        case Imf::NO_COMPRESSION:    return "NONE";
        case Imf::RLE_COMPRESSION:   return "RLE";
        case Imf::ZIPS_COMPRESSION:  return "ZIPS";
        case Imf::ZIP_COMPRESSION:   return "ZIP";
        case Imf::PIZ_COMPRESSION:   return "PIZ";
        case Imf::PXR24_COMPRESSION: return "PXR24";
        case Imf::B44_COMPRESSION:   return "B44";
        case Imf::B44A_COMPRESSION:  return "B44A";
        case Imf::DWAA_COMPRESSION:  return "DWAA";
        case Imf::DWAB_COMPRESSION:  return "DWAB";
        default: return {};   // forward-compat for new compressions
        }
    } catch (const std::exception &) {
        return {};
    }
}

} // namespace qcv
