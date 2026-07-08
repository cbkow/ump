// EXRImageLoader — port of old QCView's `image_loaders.cpp` EXRImageLoader.
//
// OpenEXR (Imf) backed loader. Reads RGB(A) half-float pixels into
// our PixelData with PixelFormat::RGBA16F + PipelineMode::ULTRA_HIGH_RES,
// the FP16 chain end-to-end downstream. Multi-part files are supported
// via Imf::MultiPartInputFile; the layer name (passed to loadFrame)
// selects a part by header name, falling back to the first part when
// not found.
//
// I/O routes through MemoryMappedIStream so the OpenEXR decompressor
// reads via mmap (kernel page-cache + sequential prefetch) instead of
// plain read syscalls — 4K/8K EXRs would otherwise saturate the
// syscall path before disk throughput. Used by ImageSequenceCache as
// the EXR loader on its IImageLoader factory plug.
//
// Thread safety: each loadFrame opens its own Imf input file, so
// multiple instances can decode in parallel — the cache's IO worker
// thread allocates one fresh loader per async task.

#pragma once

#include "image_loader.h"

#include <map>

namespace qcv {

class EXRImageLoader : public IImageLoader {
public:
    // Multi-layer EXR support — set before loadFrame to pick a
    // non-default part. Empty = root channels (single-part files).
    void setLayer(const std::string &layer) { m_layer = layer; }
    const std::string &layer() const { return m_layer; }

    // Latency-tier decode (EXR perf audit 2026-07-08): N > 0 makes
    // loadFrame's readPixels parallelize chunk decompression across
    // OpenEXR's global pool (lazily sized on first use). Measured on
    // 4K DWAB: 225 ms single → 56 ms at 8. Only loadFrame benefits —
    // the thumbnail paths read one scanline per call, which has no
    // chunk parallelism to exploit. Keep 0 on playback/read-ahead
    // paths (the cache's 16 workers already saturate cores).
    void setDecodeThreads(int n) override { m_decodeThreads = n; }

    std::shared_ptr<PixelData> loadFrame(
        const std::string &path,
        const std::string &layer,           // overrides setLayer if non-empty
        PipelineMode pipeline_mode) override;

    // Phase 3.H.5 — scanline-skip thumbnail loader (port of old
    // QCView image_loaders.cpp:1203). Reads every Nth scanline +
    // every Nth pixel rather than full decode-then-downsample, so
    // a 6K EXR thumbnails in a fraction of the time a full
    // loadFrame would. Returns RGBA16F at roughly max_size × ~ratio.
    std::shared_ptr<PixelData> loadThumbnail(
        const std::string &path, int max_size = 320) override;

    bool getDimensions(const std::string &path,
                       int &width, int &height) override;

    std::string loaderName() const override { return "EXR"; }

    // Contact-sheet batch: thumbnail EVERY layer of a SINGLE-PART
    // multi-layer EXR in one scanline sweep. In a single-part file
    // all channels live interleaved in the same compression blocks,
    // so decoding layers one request at a time re-decompresses the
    // whole file once PER LAYER — OpenEXR fills every slice
    // registered in the FrameBuffer during one readPixels pass, so
    // this turns N decompress passes into 1 (the Inspector's layer
    // grid went from seconds to one pass this way). Layers whose
    // prefixed R/G/B channels don't exist (data AOVs like depth.Z)
    // fall back to the bare-RGB image, matching loadThumbnail's
    // per-layer fallback.
    //
    // Returns an empty map for multi-part files (each part is
    // compressed independently there, so per-layer loadThumbnail
    // calls do no duplicate work — run those instead) and on any
    // read error.
    static std::map<std::string, std::shared_ptr<PixelData>>
    loadThumbnailsAllLayers(const std::string &path, int max_size = 480);

    // Phase 7.4.b layer-discovery. Returns the list of selectable
    // layer names for `path`, in canonical order:
    //   1. Multi-part files: each part's header.name() (empty
    //      string substituted with "default").
    //   2. Single-part files: unique channel-prefix tokens parsed
    //      from header.channels() (e.g. "beauty.R" → "beauty").
    //   3. Single-part files with bare RGB channels: ["default"].
    // Empty vector on read error. Cheap — just opens the header,
    // doesn't decode pixels.
    static std::vector<std::string> discoverLayers(const std::string &path);

    // Read the EXR compression name from the first part's header
    // ("NONE", "RLE", "ZIPS", "ZIP", "PIZ", "PXR24", "B44", "B44A",
    // "DWAA", "DWAB"). Returns empty string on read error or if the
    // value falls outside the known set (forward-compat). Cheap —
    // header-only, no pixel decode.
    static std::string compressionName(const std::string &path);

private:
    std::string m_layer;
    int         m_decodeThreads = 0;
};

} // namespace qcv
