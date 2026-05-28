// IDualScrubDecoder — platform-abstract per-side scrub decoder for the
// dual island.
//
// Two concrete implementations, selected at compile time by the factory
// (see src/dual/CMakeLists.txt):
//   - DualScrubDecoder (Windows/Linux): decodes and reads back to a CPU
//     QImage (DualFrame::Kind::Cpu).
//   - the macOS scrub decoder: publishes zero-copy VideoToolbox
//     CVPixelBuffers (DualFrame::Kind::Metal), mirroring single-flow's
//     ScrubDecoder so the dual compositor's pixbuf converter does YUV→RGB
//     on the GPU with no CPU round-trip.
//
// DualPlaybackController owns one per video side via makeDualScrubDecoder()
// and drives it through this interface — no platform #ifdefs at the call
// sites.

#pragma once

#include <QString>

#include <memory>

class QObject;

namespace qcv::dual {

class DualVideoDecoder;

class IDualScrubDecoder {
public:
    virtual ~IDualScrubDecoder() = default;

    // The paired streaming DualVideoDecoder MUST be open before this
    // opens (the scrub decoder reads its PTS↔frame façades). Sequenced
    // by DualPlaybackController against that decoder.
    virtual bool open(const QString &path) = 0;
    virtual void close() = 0;

    // Latest target wins; older requests are abandoned on the next worker
    // iteration. Cheap to call at slider-drag rate (60+ Hz).
    virtual void requestFrame(int frameNo) = 0;
};

// Platform factory. Windows/Linux → the CPU-readback DualScrubDecoder;
// macOS → the zero-copy Metal scrub decoder. `parent` is an optional
// QObject parent for the Qt-based implementation; non-Qt implementations
// ignore it (the returned unique_ptr is the sole owner).
std::unique_ptr<IDualScrubDecoder>
makeDualScrubDecoder(DualVideoDecoder *streaming, QObject *parent);

} // namespace qcv::dual
