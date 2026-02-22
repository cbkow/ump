#include "image_loader_interface.h"
#include "image_loaders.h"
#include "../utils/asset_path.h"
#include "../utils/debug_utils.h"
#include "../../external/stb/stb_image_resize2.h"
#include <mutex>

namespace qcview {

// Cached broken.png icon — loaded once, reused for all broken sentinels
static std::once_flag s_broken_png_flag;
static std::vector<uint8_t> s_broken_png_pixels;
static int s_broken_png_w = 0;
static int s_broken_png_h = 0;
static bool s_broken_png_loaded = false;

static void LoadBrokenPng() {
    std::string png_path = (g_exe_dir / "assets" / "image" / "broken.png").string();
    PipelineMode mode = PipelineMode::NORMAL;
    if (PNGLoader::Load(png_path, s_broken_png_pixels, s_broken_png_w, s_broken_png_h, mode)) {
        s_broken_png_loaded = true;
        Debug::Log("BrokenSentinel: Loaded broken.png (" +
                   std::to_string(s_broken_png_w) + "x" + std::to_string(s_broken_png_h) + ")");
    } else {
        Debug::Log("BrokenSentinel: Failed to load broken.png — using plain gray fallback");
    }
}

std::shared_ptr<PixelData> MakeBrokenSentinel(int w, int h) {
    // Lazy-load the icon once
    std::call_once(s_broken_png_flag, LoadBrokenPng);

    // Allocate frame filled with #787878 gray
    auto pd = std::make_shared<PixelData>();
    pd->width = w;
    pd->height = h;
    pd->gl_format = GL_RGBA;
    pd->gl_type = GL_UNSIGNED_BYTE;
    pd->pipeline_mode = PipelineMode::NORMAL;
    pd->is_sentinel = true;

    size_t pixel_count = static_cast<size_t>(w) * h;
    pd->pixels.resize(pixel_count * 4);

    // Fill with #787878FF
    for (size_t i = 0; i < pixel_count; ++i) {
        pd->pixels[i * 4 + 0] = 0x78;  // R
        pd->pixels[i * 4 + 1] = 0x78;  // G
        pd->pixels[i * 4 + 2] = 0x78;  // B
        pd->pixels[i * 4 + 3] = 0xFF;  // A
    }

    // Composite the icon if available and frame is large enough
    if (s_broken_png_loaded && w >= 4 && h >= 4) {
        int min_dim = (w < h) ? w : h;
        int target = static_cast<int>(min_dim * 0.92);
        if (target < 1) target = 1;

        // Resize cached PNG to target x target
        std::vector<uint8_t> resized(static_cast<size_t>(target) * target * 4);
        stbir_resize_uint8_linear(
            s_broken_png_pixels.data(), s_broken_png_w, s_broken_png_h, 0,
            resized.data(), target, target, 0,
            STBIR_RGBA
        );

        // Center the icon on the background
        int x_off = (w - target) / 2;
        int y_off = (h - target) / 2;

        for (int row = 0; row < target; ++row) {
            int dst_y = y_off + row;
            for (int col = 0; col < target; ++col) {
                int dst_x = x_off + col;
                size_t src_idx = (static_cast<size_t>(row) * target + col) * 4;
                size_t dst_idx = (static_cast<size_t>(dst_y) * w + dst_x) * 4;

                uint8_t sa = resized[src_idx + 3];
                if (sa == 255) {
                    // Fully opaque — direct copy
                    pd->pixels[dst_idx + 0] = resized[src_idx + 0];
                    pd->pixels[dst_idx + 1] = resized[src_idx + 1];
                    pd->pixels[dst_idx + 2] = resized[src_idx + 2];
                    pd->pixels[dst_idx + 3] = 255;
                } else if (sa > 0) {
                    // Semi-transparent — alpha "over" composite
                    uint8_t da = pd->pixels[dst_idx + 3];
                    uint16_t out_a = sa + da * (255 - sa) / 255;
                    if (out_a > 0) {
                        for (int c = 0; c < 3; ++c) {
                            pd->pixels[dst_idx + c] = static_cast<uint8_t>(
                                (resized[src_idx + c] * sa +
                                 pd->pixels[dst_idx + c] * da * (255 - sa) / 255) / out_a
                            );
                        }
                        pd->pixels[dst_idx + 3] = static_cast<uint8_t>(out_a > 255 ? 255 : out_a);
                    }
                }
                // sa == 0: fully transparent — keep background
            }
        }
    }

    return pd;
}

} // namespace qcview
