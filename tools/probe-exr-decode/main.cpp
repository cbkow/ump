// probe-exr-decode — EXR decode performance audit CLI.
//
// Usage:
//   probe-exr-decode <any-frame.exr> [options]
//     --frames N     frames per timing pass (default 48, capped to
//                    the sequence length; the sequence = all .exr in
//                    the same directory, sorted)
//     --workers T    worker threads for the playback sim (default 16
//                    — matches the app's imageSeqThreads default)
//     --layer NAME   decode this layer (default: loader default)
//     --skip-threads skip the OpenEXR-internal-threads comparison
//
// Passes:
//   0. Anatomy — parts, compression, display/data window (slow-path
//      flag), channels grouped by layer prefix.
//   1. Single-worker loadFrame over N frames, run twice (pass 1
//      part-cold, pass 2 page-cache warm) — per-frame ms + MB/s.
//   2. Playback sim — T workers pull frames from a shared counter
//      (the ImageSequenceCache shape) — aggregate fps.
//   3. OpenEXR internal threads {0,4,8} on ONE frame — single-frame
//      latency sensitivity (scrub / first-frame cost).
//   4. Thumbnail paths — loadThumbnail(320) + loadThumbnailsAllLayers.

#include "decode/exr_image_loader.h"
#include "decode/image_loader.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfMultiPartInputFile.h>
#include <OpenEXR/ImfThreading.h>
#include <ImathBox.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(
        Clock::now() - t0).count();
}

struct Stats {
    double mn = 1e9, mx = 0.0, total = 0.0;
    int n = 0;
    void add(double ms) {
        mn = std::min(mn, ms);
        mx = std::max(mx, ms);
        total += ms;
        ++n;
    }
    double avg() const { return n ? total / n : 0.0; }
};

std::vector<std::string> sequenceFrames(const fs::path &anyFrame)
{
    std::vector<std::string> out;
    for (const auto &e : fs::directory_iterator(anyFrame.parent_path())) {
        if (!e.is_regular_file()) continue;
        auto ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".exr") out.push_back(e.path().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

const char *compressionName(Imf::Compression c)
{
    switch (c) {
    case Imf::NO_COMPRESSION:    return "NONE";
    case Imf::RLE_COMPRESSION:   return "RLE";
    case Imf::ZIPS_COMPRESSION:  return "ZIPS (1-line blocks)";
    case Imf::ZIP_COMPRESSION:   return "ZIP (16-line blocks)";
    case Imf::PIZ_COMPRESSION:   return "PIZ (32-line blocks)";
    case Imf::PXR24_COMPRESSION: return "PXR24";
    case Imf::B44_COMPRESSION:   return "B44";
    case Imf::B44A_COMPRESSION:  return "B44A";
    case Imf::DWAA_COMPRESSION:  return "DWAA (32-line blocks)";
    case Imf::DWAB_COMPRESSION:  return "DWAB (256-line blocks)";
    default:                     return "UNKNOWN";
    }
}

void printAnatomy(const std::string &path)
{
    std::printf("== anatomy: %s\n", path.c_str());
    std::printf("   file size: %.1f MB\n",
                double(fs::file_size(path)) / (1024.0 * 1024.0));

    Imf::MultiPartInputFile file(path.c_str());
    std::printf("   parts: %d\n", file.parts());

    for (int p = 0; p < file.parts(); ++p) {
        const Imf::Header &h = file.header(p);
        const Imath::Box2i disp = h.displayWindow();
        const Imath::Box2i data = h.dataWindow();
        const int w = disp.max.x - disp.min.x + 1;
        const int h2 = disp.max.y - disp.min.y + 1;

        std::printf("   part %d%s%s: %dx%d, %s\n", p,
                    h.hasName() ? " '" : "",
                    h.hasName() ? (h.name() + "'").c_str() : "",
                    w, h2, compressionName(h.compression()));
        if (disp != data) {
            std::printf(
                "     !! dataWindow != displayWindow "
                "(data %d,%d -> %d,%d) — loadFrame takes the "
                "scanline-by-scanline SLOW PATH for this part\n",
                data.min.x, data.min.y, data.max.x, data.max.y);
        }

        // Channels grouped by layer prefix.
        std::map<std::string, int> layers;
        int totalCh = 0;
        const Imf::ChannelList &ch = h.channels();
        for (auto it = ch.begin(); it != ch.end(); ++it) {
            const std::string n = it.name();
            const auto dot = n.find_last_of('.');
            layers[dot == std::string::npos
                       ? std::string("<root>")
                       : n.substr(0, dot)]++;
            ++totalCh;
        }
        std::printf("     channels: %d total across %zu layer(s)\n",
                    totalCh, layers.size());
        for (const auto &[name, count] : layers) {
            std::printf("       %-28s %d ch\n", name.c_str(), count);
        }
        if (file.parts() == 1 && layers.size() > 1) {
            std::printf(
                "     !! SINGLE-PART MULTI-LAYER: decoding ANY one "
                "layer decompresses ALL %d channels per block — "
                "playback cost scales with total channels, not the "
                "displayed layer\n", totalCh);
        }
    }
}

double benchPass(const std::vector<std::string> &frames, int count,
                 const std::string &layer, Stats &st)
{
    const auto t0 = Clock::now();
    std::size_t bytes = 0;
    for (int i = 0; i < count; ++i) {
        qcv::EXRImageLoader loader;
        if (!layer.empty()) loader.setLayer(layer);
        const auto f0 = Clock::now();
        auto pd = loader.loadFrame(frames[i % frames.size()],
                                   std::string(),
                                   qcv::PipelineMode::ULTRA_HIGH_RES);
        st.add(msSince(f0));
        if (pd) bytes += pd->byteSize();
    }
    const double wall = msSince(t0);
    std::printf("     %d frames in %.0f ms — %.2f fps, %.0f MB/s "
                "decoded output\n",
                count, wall, count * 1000.0 / wall,
                bytes / (1024.0 * 1024.0) / (wall / 1000.0));
    return wall;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: probe-exr-decode <any-frame.exr> [--frames N] "
            "[--workers T] [--layer NAME] [--skip-threads]\n");
        return 2;
    }

    std::string firstFrame = argv[1];
    int frameCount = 48;
    int workers = 16;
    std::string layer;
    bool skipThreads = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) {
            frameCount = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--workers") && i + 1 < argc) {
            workers = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--layer") && i + 1 < argc) {
            layer = argv[++i];
        } else if (!std::strcmp(argv[i], "--skip-threads")) {
            skipThreads = true;
        }
    }

    try {
        printAnatomy(firstFrame);

        const auto frames = sequenceFrames(firstFrame);
        std::printf("   sequence: %zu frames in directory\n\n",
                    frames.size());
        if (frames.empty()) return 1;
        frameCount = std::min<int>(frameCount,
                                   static_cast<int>(frames.size()));

        // Pass 1+2 — single worker, sequential frames. First pass
        // may be page-cache cold; second is warm.
        std::printf("== single worker, loadFrame x%d (layer='%s')\n",
                    frameCount, layer.c_str());
        for (int pass = 1; pass <= 2; ++pass) {
            Stats st;
            std::printf("   pass %d (%s):\n", pass,
                        pass == 1 ? "cold-ish" : "warm");
            benchPass(frames, frameCount, layer, st);
            std::printf("     per-frame ms: min %.1f / avg %.1f / "
                        "max %.1f\n", st.mn, st.avg(), st.mx);
        }

        // Pass 3 — playback sim: N workers pulling from a counter,
        // fresh loader per frame (the ImageSequenceCache shape).
        std::printf("\n== playback sim: %d workers, %d frames\n",
                    workers, frameCount);
        {
            std::atomic<int> next{0};
            const auto t0 = Clock::now();
            std::vector<std::thread> pool;
            for (int w = 0; w < workers; ++w) {
                pool.emplace_back([&]() {
                    while (true) {
                        const int i = next.fetch_add(1);
                        if (i >= frameCount) return;
                        qcv::EXRImageLoader loader;
                        if (!layer.empty()) loader.setLayer(layer);
                        loader.loadFrame(frames[i % frames.size()],
                                         std::string(),
                                         qcv::PipelineMode::ULTRA_HIGH_RES);
                    }
                });
            }
            for (auto &t : pool) t.join();
            const double wall = msSince(t0);
            std::printf("   %d frames in %.0f ms — %.2f fps aggregate\n",
                        frameCount, wall, frameCount * 1000.0 / wall);
        }

        // Pass 4 — latency-tier decode (setDecodeThreads, the express
        // lane's mechanism) sensitivity on ONE frame.
        if (!skipThreads) {
            std::printf("\n== setDecodeThreads latency tier, "
                        "1 frame x5\n");
            for (int k : {0, 4, 8}) {
                Stats st;
                for (int i = 0; i < 5; ++i) {
                    qcv::EXRImageLoader loader;
                    if (!layer.empty()) loader.setLayer(layer);
                    loader.setDecodeThreads(k);
                    const auto f0 = Clock::now();
                    loader.loadFrame(frames[0], std::string(),
                                     qcv::PipelineMode::ULTRA_HIGH_RES);
                    st.add(msSince(f0));
                }
                std::printf("   threads=%d: min %.1f / avg %.1f ms\n",
                            k, st.mn, st.avg());
            }
        }

        // Pass 5 — thumbnail paths.
        std::printf("\n== thumbnail paths (frame 0)\n");
        {
            qcv::EXRImageLoader loader;
            if (!layer.empty()) loader.setLayer(layer);
            const auto t0 = Clock::now();
            loader.loadThumbnail(frames[0], 320);
            std::printf("   loadThumbnail(320): %.1f ms\n", msSince(t0));
        }
        {
            const auto t0 = Clock::now();
            const auto all = qcv::EXRImageLoader::loadThumbnailsAllLayers(
                frames[0], 480);
            std::printf("   loadThumbnailsAllLayers(480): %.1f ms "
                        "(%zu layers)\n", msSince(t0), all.size());
        }

        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
