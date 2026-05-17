#version 440

// Phase 1.8.2c: AYpCbCr16 packed → RGBA conversion.
//
// VideoToolbox decodes ProRes 4444 (10-bit and 12-bit) into
// kCVPixelFormatType_4444AYpCbCr16 — a single packed plane with
// 16 bits per channel. The IOSurface byte order maps the channels
// in this order per pixel:
//
//     A (16 bits)  Y' (16 bits)  Cb (16 bits)  Cr (16 bits)
//
// When the bridge wraps this as MTLPixelFormatRGBA16Unorm, the
// shader's texture() returns:
//
//     .r = A   .g = Y'   .b = Cb   .a = Cr
//
// 12-bit content is MSB-aligned in the 16-bit slots (12-bit max =
// 0xFFF stored as 0xFFF0). Sampling normalizes to ~0..1 with a
// tiny scale error (~0.5% at white) which is well below visible
// thresholds. OCIO in Phase 2 will subsume this conversion entirely
// and pick the right precision-correct path.
//
// Same matrix selection + range handling as yuv_to_rgb_biplanar.frag.
// Alpha is preserved straight through.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Uniforms {
    int   matrixIdx;   // 0=BT.601, 1=BT.709, 2=BT.2020
    int   fullRange;   // 1 if PC-range YCbCr, 0 otherwise
    int   pad0;
    int   pad1;
} u;

layout(binding = 1) uniform sampler2D u_packed;

mat3 ycbcrToRgb(int idx) {
    if (idx == 0) {
        return mat3(
            1.0,        1.0,        1.0,
            0.0,       -0.344136,   1.772,
            1.402,     -0.714136,   0.0
        );
    } else if (idx == 2) {
        return mat3(
            1.0,        1.0,        1.0,
            0.0,       -0.187326,   1.8556,
            1.4746,    -0.571353,   0.0
        );
    }
    return mat3(
        1.0,        1.0,        1.0,
        0.0,       -0.187324,   1.8556,
        1.5748,    -0.468124,   0.0
    );
}

void main() {
    // Same Y-flip as yuv_to_rgb_biplanar.frag — CVPixelBuffer rows
    // are top-down; QRhi NDC → texture mapping through the
    // offscreen Pass 0 round-trip would otherwise display upside down.
    vec2 sampleUv = vec2(v_uv.x, 1.0 - v_uv.y);

    vec4 ayuv = texture(u_packed, sampleUv);
    float a    = ayuv.r;
    float y    = ayuv.g;
    vec2  cbcr = ayuv.ba;

    if (u.fullRange == 0) {
        y    = (y    - 16.0/255.0) * (255.0 / 219.0);
        cbcr = (cbcr - vec2(128.0/255.0)) * (255.0 / 224.0);
    } else {
        cbcr = cbcr - vec2(0.5);
    }

    vec3 yuv = vec3(y, cbcr.x, cbcr.y);
    vec3 rgb = ycbcrToRgb(u.matrixIdx) * yuv;

    fragColor = vec4(clamp(rgb, vec3(0.0), vec3(1.0)),
                     clamp(a, 0.0, 1.0));
}
