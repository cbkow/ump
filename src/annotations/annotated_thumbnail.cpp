#include "annotated_thumbnail.h"
#include "annotation_serializer.h"
#include "stroke_tessellator.h"
#include "../utils/debug_utils.h"

#include <png.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <filesystem>

// stb_image_write (implementation lives in video_display_component.cpp)
#include "../../external/glfw/deps/stb_image_write.h"

namespace qcview {
namespace Annotations {

// Simple edge-function triangle rasterizer for small thumbnail images.
// Operates on RGBA8 pixel buffer with straight-alpha blending.

static inline float EdgeFunction(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static void RasterizeTriangle(
    uint8_t* pixels, int img_w, int img_h,
    float x0, float y0, float x1, float y1, float x2, float y2,
    float r, float g, float b, float a
) {
    // Bounding box
    int min_x = std::max(0, static_cast<int>(std::floor(std::min({x0, x1, x2}))));
    int min_y = std::max(0, static_cast<int>(std::floor(std::min({y0, y1, y2}))));
    int max_x = std::min(img_w - 1, static_cast<int>(std::ceil(std::max({x0, x1, x2}))));
    int max_y = std::min(img_h - 1, static_cast<int>(std::ceil(std::max({y0, y1, y2}))));

    float area = EdgeFunction(x0, y0, x1, y1, x2, y2);
    if (std::abs(area) < 1e-6f) return;  // Degenerate triangle

    // Ensure consistent winding (positive area)
    if (area < 0.0f) {
        // Swap v1 and v2 to flip winding
        std::swap(x1, x2);
        std::swap(y1, y2);
        area = -area;
    }

    float inv_area = 1.0f / area;

    // Source color as 0-255
    float src_r = r * 255.0f;
    float src_g = g * 255.0f;
    float src_b = b * 255.0f;
    float src_a = a;

    for (int py = min_y; py <= max_y; ++py) {
        for (int px = min_x; px <= max_x; ++px) {
            float cx = px + 0.5f;
            float cy = py + 0.5f;

            float w0 = EdgeFunction(x1, y1, x2, y2, cx, cy);
            float w1 = EdgeFunction(x2, y2, x0, y0, cx, cy);
            float w2 = EdgeFunction(x0, y0, x1, y1, cx, cy);

            if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                // Inside triangle — alpha blend
                uint8_t* dst = pixels + (py * img_w + px) * 4;

                float dst_r = dst[0];
                float dst_g = dst[1];
                float dst_b = dst[2];
                float dst_a = dst[3] / 255.0f;

                // Standard over compositing: src over dst
                float out_a = src_a + dst_a * (1.0f - src_a);
                if (out_a > 0.0f) {
                    float out_r = (src_r * src_a + dst_r * dst_a * (1.0f - src_a)) / out_a;
                    float out_g = (src_g * src_a + dst_g * dst_a * (1.0f - src_a)) / out_a;
                    float out_b = (src_b * src_a + dst_b * dst_a * (1.0f - src_a)) / out_a;

                    dst[0] = static_cast<uint8_t>(std::clamp(out_r, 0.0f, 255.0f));
                    dst[1] = static_cast<uint8_t>(std::clamp(out_g, 0.0f, 255.0f));
                    dst[2] = static_cast<uint8_t>(std::clamp(out_b, 0.0f, 255.0f));
                }
                dst[3] = static_cast<uint8_t>(std::clamp(out_a * 255.0f, 0.0f, 255.0f));
            }
        }
    }
}

static bool LoadPNG(const std::string& path, std::vector<uint8_t>& pixels, int& width, int& height) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;

    png_byte header[8];
    fread(header, 1, 8, fp);
    if (png_sig_cmp(header, 0, 8)) {
        fclose(fp);
        return false;
    }

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr) { fclose(fp); return false; }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        fclose(fp);
        return false;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        fclose(fp);
        return false;
    }

    png_init_io(png_ptr, fp);
    png_set_sig_bytes(png_ptr, 8);
    png_read_info(png_ptr, info_ptr);

    width = png_get_image_width(png_ptr, info_ptr);
    height = png_get_image_height(png_ptr, info_ptr);
    png_byte color_type = png_get_color_type(png_ptr, info_ptr);
    png_byte bit_depth = png_get_bit_depth(png_ptr, info_ptr);

    // Convert to RGBA8
    if (bit_depth == 16) png_set_strip_16(png_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png_ptr);
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr);

    png_read_update_info(png_ptr, info_ptr);

    pixels.resize(width * height * 4);
    std::vector<png_bytep> row_pointers(height);
    for (int y = 0; y < height; y++) {
        row_pointers[y] = &pixels[y * width * 4];
    }

    png_read_image(png_ptr, row_pointers.data());
    png_read_end(png_ptr, nullptr);

    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    fclose(fp);
    return true;
}

std::string GetAnnotatedThumbnailPath(const std::string& clean_png_path) {
    // "note_01_02_03_04.png" -> "note_01_02_03_04_annotated.png"
    std::filesystem::path p(clean_png_path);
    std::string stem = p.stem().string();
    std::string ext = p.extension().string();
    return (p.parent_path() / (stem + "_annotated" + ext)).string();
}

bool GenerateAnnotatedThumbnail(
    const std::string& clean_png_path,
    const std::string& annotated_png_path,
    const std::string& annotation_data_json
) {
    // Load clean thumbnail
    std::vector<uint8_t> pixels;
    int width = 0, height = 0;
    if (!LoadPNG(clean_png_path, pixels, width, height)) {
        Debug::Log("GenerateAnnotatedThumbnail: Failed to load clean PNG: " + clean_png_path);
        return false;
    }

    // Deserialize strokes
    auto strokes = AnnotationSerializer::JsonStringToStrokes(annotation_data_json);
    if (strokes.empty()) {
        return false;
    }

    // Tessellate and rasterize each stroke
    float display_w = static_cast<float>(width);
    float display_h = static_cast<float>(height);
    // Scale stroke width proportionally to thumbnail size
    // Strokes are authored at viewport resolution; thumbnails are much smaller
    float line_width_scale = display_w / 1920.0f;
    // Ensure a minimum so thin strokes remain visible at small thumbnail sizes
    if (line_width_scale < 0.15f) line_width_scale = 0.15f;

    for (const auto& stroke : strokes) {
        TessellatedMesh mesh = StrokeTessellator::Tessellate(
            stroke, 0.0f, 0.0f, display_w, display_h,
            line_width_scale, true
        );

        // Rasterize each triangle
        for (size_t i = 0; i + 2 < mesh.vertices.size(); i += 3) {
            const auto& v0 = mesh.vertices[i];
            const auto& v1 = mesh.vertices[i + 1];
            const auto& v2 = mesh.vertices[i + 2];
            RasterizeTriangle(
                pixels.data(), width, height,
                v0.x, v0.y, v1.x, v1.y, v2.x, v2.y,
                v0.r, v0.g, v0.b, v0.a
            );
        }
    }

    // Save result
    int result = stbi_write_png(annotated_png_path.c_str(), width, height, 4, pixels.data(), width * 4);
    if (!result) {
        Debug::Log("GenerateAnnotatedThumbnail: Failed to write PNG: " + annotated_png_path);
        return false;
    }

    Debug::Log("GenerateAnnotatedThumbnail: Saved " + annotated_png_path);
    return true;
}

} // namespace Annotations
} // namespace qcview
