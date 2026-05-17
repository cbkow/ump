#version 440

// Phase 1.8.2b: YUV biplanar → RGB conversion.
//
// Inputs: Y plane (single channel), UV plane (two channel) sampled at
// normalized [0..1] UVs. The Metal-side bridge wraps each plane of a
// CVPixelBuffer as a separate QRhiTexture; the platform driver
// returns 0..1 normalized values from samplers regardless of the
// underlying bit depth (R8 or R16 unsigned-normalized).
//
// Output: linear-ish RGB in the working colorspace. For Phase 1.8.2b
// this is BT.709 gamma-encoded sRGB-equivalent (matching what
// swscale's CPU path produces; OCIO replaces it in Phase 2).
//
// Color matrix (BT.601 / BT.709 / BT.2020) and color range
// (limited 16-235 / full 0-255) selected via uniforms set by the
// bridge from the source CVPixelBuffer's metadata.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Uniforms {
    int   matrixIdx;   // 0=BT.601, 1=BT.709, 2=BT.2020
    int   fullRange;   // 1 if PC-range YCbCr (no level scaling), 0 otherwise
    // pad to 16 bytes
    int   pad0;
    int   pad1;
} u;

layout(binding = 1) uniform sampler2D u_yPlane;
layout(binding = 2) uniform sampler2D u_uvPlane;

// Returns the 3x3 matrix that maps (Y',Cb,Cr) to linear-ish RGB.
// Standard inverse matrices for limited-range YCbCr to RGB; the
// chroma values are pre-shifted to [-0.5, 0.5] before applying.
mat3 ycbcrToRgb(int idx) {
    if (idx == 0) {
        // BT.601
        return mat3(
            1.0,        1.0,        1.0,
            0.0,       -0.344136,   1.772,
            1.402,     -0.714136,   0.0
        );
    } else if (idx == 2) {
        // BT.2020
        return mat3(
            1.0,        1.0,        1.0,
            0.0,       -0.187326,   1.8556,
            1.4746,    -0.571353,   0.0
        );
    }
    // BT.709 (default)
    return mat3(
        1.0,        1.0,        1.0,
        0.0,       -0.187324,   1.8556,
        1.5748,    -0.468124,   0.0
    );
}

void main() {
    // CVPixelBuffer / IOSurface stores rows top-down; QRhi's NDC →
    // texture mapping through the offscreen Pass 0 round-trip would
    // otherwise display the image upside down. Flip v here so the
    // top of the source image lands at the top of m_srcA.
    vec2 sampleUv = vec2(v_uv.x, 1.0 - v_uv.y);

    float y    = texture(u_yPlane,  sampleUv).r;
    vec2  cbcr = texture(u_uvPlane, sampleUv).rg;

    // Limited range: Y in [16/255, 235/255], CbCr in [16/255, 240/255]
    // with 128 as the chroma midpoint. Full range: Y in [0..1],
    // CbCr midpoint at 0.5.
    if (u.fullRange == 0) {
        y    = (y    - 16.0/255.0)  * (255.0 / 219.0);
        cbcr = (cbcr - vec2(128.0/255.0)) * (255.0 / 224.0);
    } else {
        cbcr = cbcr - vec2(0.5);
    }

    vec3 yuv = vec3(y, cbcr.x, cbcr.y);
    vec3 rgb = ycbcrToRgb(u.matrixIdx) * yuv;

    fragColor = vec4(clamp(rgb, vec3(0.0), vec3(1.0)), 1.0);
}
