// ============================================================================
// Raster drawing utilities + Base64 helpers
// ============================================================================

#include "app/drawing_utils.h"
#include "app/application.h"
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include <string>

extern "C" {
#include <libavutil/base64.h>
}

// Helper to draw a line on an image buffer (Bresenham's algorithm)
void DrawLineOnImage(unsigned char* image, int width, int height,
                     int x0, int y0, int x1, int y1,
                     unsigned char r, unsigned char g, unsigned char b, unsigned char a, int thickness) {
    auto setPixel = [&](int x, int y) {
        if (x >= 0 && x < width && y >= 0 && y < height) {
            int idx = (y * width + x) * 4;
            // Alpha blending
            float alpha = a / 255.0f;
            image[idx + 0] = (unsigned char)(r * alpha + image[idx + 0] * (1 - alpha));
            image[idx + 1] = (unsigned char)(g * alpha + image[idx + 1] * (1 - alpha));
            image[idx + 2] = (unsigned char)(b * alpha + image[idx + 2] * (1 - alpha));
        }
    };

    // Draw thick line by drawing multiple parallel lines
    for (int t = -thickness/2; t <= thickness/2; t++) {
        int dx = abs(x1 - x0);
        int dy = abs(y1 - y0);
        int sx = x0 < x1 ? 1 : -1;
        int sy = y0 < y1 ? 1 : -1;
        int err = dx - dy;

        int x = x0;
        int y = y0;

        while (true) {
            if (dx > dy) {
                setPixel(x, y + t);
            } else {
                setPixel(x + t, y);
            }

            if (x == x1 && y == y1) break;

            int e2 = 2 * err;
            if (e2 > -dy) {
                err -= dy;
                x += sx;
            }
            if (e2 < dx) {
                err += dx;
                y += sy;
            }
        }
    }
}

// Helper to draw a rectangle on an image buffer
void DrawRectangleOnImage(unsigned char* image, int width, int height,
                          int x0, int y0, int x1, int y1,
                          unsigned char r, unsigned char g, unsigned char b, unsigned char a,
                          int thickness, bool filled) {
    if (filled) {
        // Draw filled rectangle
        int minX = std::min(x0, x1);
        int maxX = std::max(x0, x1);
        int minY = std::min(y0, y1);
        int maxY = std::max(y0, y1);

        for (int y = minY; y <= maxY; y++) {
            for (int x = minX; x <= maxX; x++) {
                if (x >= 0 && x < width && y >= 0 && y < height) {
                    int idx = (y * width + x) * 4;
                    float alpha = a / 255.0f;
                    image[idx + 0] = (unsigned char)(r * alpha + image[idx + 0] * (1 - alpha));
                    image[idx + 1] = (unsigned char)(g * alpha + image[idx + 1] * (1 - alpha));
                    image[idx + 2] = (unsigned char)(b * alpha + image[idx + 2] * (1 - alpha));
                }
            }
        }
    } else {
        // Draw rectangle outline
        DrawLineOnImage(image, width, height, x0, y0, x1, y0, r, g, b, a, thickness); // Top
        DrawLineOnImage(image, width, height, x1, y0, x1, y1, r, g, b, a, thickness); // Right
        DrawLineOnImage(image, width, height, x1, y1, x0, y1, r, g, b, a, thickness); // Bottom
        DrawLineOnImage(image, width, height, x0, y1, x0, y0, r, g, b, a, thickness); // Left
    }
}

// Helper to draw an oval/ellipse on an image buffer (midpoint algorithm)
void DrawOvalOnImage(unsigned char* image, int width, int height,
                     int x0, int y0, int x1, int y1,
                     unsigned char r, unsigned char g, unsigned char b, unsigned char a,
                     int thickness, bool filled) {
    auto setPixel = [&](int x, int y) {
        if (x >= 0 && x < width && y >= 0 && y < height) {
            int idx = (y * width + x) * 4;
            float alpha = a / 255.0f;
            image[idx + 0] = (unsigned char)(r * alpha + image[idx + 0] * (1 - alpha));
            image[idx + 1] = (unsigned char)(g * alpha + image[idx + 1] * (1 - alpha));
            image[idx + 2] = (unsigned char)(b * alpha + image[idx + 2] * (1 - alpha));
        }
    };

    int cx = (x0 + x1) / 2;
    int cy = (y0 + y1) / 2;
    int rx = abs(x1 - x0) / 2;
    int ry = abs(y1 - y0) / 2;

    if (rx == 0 || ry == 0) return;

    // Midpoint ellipse algorithm
    auto drawEllipsePoints = [&](int x, int y) {
        for (int t = -thickness/2; t <= thickness/2; t++) {
            setPixel(cx + x, cy + y + t);
            setPixel(cx - x, cy + y + t);
            setPixel(cx + x, cy - y + t);
            setPixel(cx - x, cy - y + t);
        }
    };

    if (filled) {
        // Draw filled ellipse by drawing horizontal lines
        for (int y = -ry; y <= ry; y++) {
            int x = (int)(rx * sqrt(1.0 - (double)(y * y) / (ry * ry)));
            for (int xi = -x; xi <= x; xi++) {
                setPixel(cx + xi, cy + y);
            }
        }
    } else {
        // Draw ellipse outline
        int x = 0;
        int y = ry;

        // Region 1
        int rx2 = rx * rx;
        int ry2 = ry * ry;
        int p1 = ry2 - (rx2 * ry) + (0.25 * rx2);
        int dx = 2 * ry2 * x;
        int dy = 2 * rx2 * y;

        while (dx < dy) {
            drawEllipsePoints(x, y);
            x++;
            dx += 2 * ry2;
            if (p1 < 0) {
                p1 += dx + ry2;
            } else {
                y--;
                dy -= 2 * rx2;
                p1 += dx - dy + ry2;
            }
        }

        // Region 2
        int p2 = ry2 * (x + 0.5) * (x + 0.5) + rx2 * (y - 1) * (y - 1) - rx2 * ry2;
        while (y >= 0) {
            drawEllipsePoints(x, y);
            y--;
            dy -= 2 * rx2;
            if (p2 > 0) {
                p2 += rx2 - dy;
            } else {
                x++;
                dx += 2 * ry2;
                p2 += dx - dy + rx2;
            }
        }
    }
}

// Helper to draw an arrow on an image buffer
void DrawArrowOnImage(unsigned char* image, int width, int height,
                      int x0, int y0, int x1, int y1,
                      unsigned char r, unsigned char g, unsigned char b, unsigned char a, int thickness) {
    // Draw the main line
    DrawLineOnImage(image, width, height, x0, y0, x1, y1, r, g, b, a, thickness);

    // Calculate arrowhead
    double angle = atan2(y1 - y0, x1 - x0);
    double arrowLength = 15.0 + thickness * 2;
    double arrowAngle = 0.4; // ~23 degrees

    int ax1 = x1 - (int)(arrowLength * cos(angle - arrowAngle));
    int ay1 = y1 - (int)(arrowLength * sin(angle - arrowAngle));
    int ax2 = x1 - (int)(arrowLength * cos(angle + arrowAngle));
    int ay2 = y1 - (int)(arrowLength * sin(angle + arrowAngle));

    // Draw arrowhead lines
    DrawLineOnImage(image, width, height, x1, y1, ax1, ay1, r, g, b, a, thickness);
    DrawLineOnImage(image, width, height, x1, y1, ax2, ay2, r, g, b, a, thickness);
}


// ============================================================================
// BASE64 HELPERS (Frame.io Token Obfuscation)
// ============================================================================

std::string Application::EncodeBase64(const std::string& input) {
    if (input.empty()) return "";

    // Calculate required buffer size
    size_t output_size = AV_BASE64_SIZE(input.size());
    std::vector<char> output(output_size);

    // Encode using FFmpeg's base64 implementation
    char* result = av_base64_encode(output.data(), output_size,
                                   reinterpret_cast<const uint8_t*>(input.data()),
                                   input.size());

    if (result) {
        return std::string(result);
    }
    return "";
}

std::string Application::DecodeBase64(const std::string& input) {
    if (input.empty()) return "";

    // Calculate maximum decoded size
    size_t max_decoded_size = input.size();  // Base64 decodes to smaller size
    std::vector<uint8_t> output(max_decoded_size);

    // Decode using FFmpeg's base64 implementation
    int decoded_size = av_base64_decode(output.data(), input.c_str(), max_decoded_size);

    if (decoded_size > 0) {
        return std::string(reinterpret_cast<char*>(output.data()), decoded_size);
    }
    return "";
}
