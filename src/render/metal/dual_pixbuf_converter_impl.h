// DualPixbufConverterImpl — task #107 / #108 commit 2.
//
// qcv_render-side implementation of qcv::dual::IDualPixbufConverter.
// Wraps a CvPixbufMetalBridge (per slot) + a single MetalYuvRenderer
// to convert HW-decoded CVPixelBuffers into RGBA MTLTextures the
// dual compositor's fragment shader can sample directly.
//
// Lifetime: owned by MetalPlayerRenderer; injected into the dual
// compositor at dual-mode entry, cleared on exit. Created lazily on
// first use so we don't pay the bridge / cache init cost in pure
// single-flow sessions.

#pragma once

#include "dual/i_dual_pixbuf_converter.h"

namespace qcv {

class DualPixbufConverterImpl : public dual::IDualPixbufConverter {
public:
    DualPixbufConverterImpl();
    ~DualPixbufConverterImpl() override;

    DualPixbufConverterImpl(const DualPixbufConverterImpl &)            = delete;
    DualPixbufConverterImpl &operator=(const DualPixbufConverterImpl &) = delete;

    bool initialize();
    void shutdown();
    bool isInitialized() const;

    // IDualPixbufConverter
    void *convertToRgba(void *cmdBuffer,
                          void *cvPixelBuffer,
                          int slot,
                          int *outW, int *outH,
                          int rangeOverride = 0) override;

private:
    // Pimpl — the bridges + yuv renderer + per-slot strong refs to
    // the last-rendered RGBA texture live in the .mm so this header
    // doesn't drag in Obj-C types (it's included from a regular C++
    // path in MetalPlayerRenderer's interface).
    struct Impl;
    Impl *m_impl = nullptr;
};

} // namespace qcv
