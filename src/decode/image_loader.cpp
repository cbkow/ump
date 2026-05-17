#include "image_loader.h"

#include <cstring>

namespace qcv {

std::shared_ptr<PixelData> MakeGapSentinel(int w, int h)
{
    auto pd = std::make_shared<PixelData>();
    pd->width = w;
    pd->height = h;
    pd->setFormat(PixelFormat::RGBA8);
    pd->pipeline_mode = PipelineMode::NORMAL;
    pd->is_sentinel = true;
    pd->pixels.assign(static_cast<std::size_t>(w) * h * 4, 0); // transparent
    return pd;
}

std::shared_ptr<PixelData> MakeBrokenSentinel(int w, int h)
{
    // Mid-gray fill — old QCView overlays a centered broken.png icon
    // at this stage; the port starts plain and re-adds the icon
    // once thumbnail loading lands in 7.4.b. The gray gives an
    // immediate "this frame failed" cue without requiring the
    // shipping resource to exist on disk.
    auto pd = std::make_shared<PixelData>();
    pd->width = w;
    pd->height = h;
    pd->setFormat(PixelFormat::RGBA8);
    pd->pipeline_mode = PipelineMode::NORMAL;
    pd->is_sentinel = true;
    const std::size_t pxCount = static_cast<std::size_t>(w) * h;
    pd->pixels.resize(pxCount * 4);
    std::uint8_t *p = pd->pixels.data();
    for (std::size_t i = 0; i < pxCount; ++i) {
        p[i * 4 + 0] = 0x78; // R
        p[i * 4 + 1] = 0x78; // G
        p[i * 4 + 2] = 0x78; // B
        p[i * 4 + 3] = 0xFF; // A — opaque, so it occludes the prior frame
    }
    return pd;
}

} // namespace qcv
