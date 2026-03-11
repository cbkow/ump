#version 450

//=============================================================================
// YUV to RGB Conversion Shader (Vulkan/GLSL)
//
// Port of the D3D11 HLSL shader. Supports:
// - 2-plane NV12/P010 (interleaved UV)
// - 3-plane planar YUV (YUV420P, YUV422P, YUV444P and 10/12-bit variants)
// - 3-plane GBRP RGB planar (8/10/12-bit)
// - 4-plane YUVA with alpha (8/10/12-bit)
// - 4-plane GBRAP RGB planar with alpha (8/10/12-bit)
// - BT.709 and BT.2020 color matrices
// - Video range (16-235) and full range (0-255)
//=============================================================================

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

//=============================================================================
// Uniform Buffer - matches YUVConstantBuffer (std140 layout)
//=============================================================================

layout(std140, set = 0, binding = 0) uniform YUVParams {
    float bitDepth;        // 8.0, 10.0, or 12.0
    float applyPQ;         // 1.0 for HDR PQ decode (unused, kept for compat)
    float isFullRange;     // 1.0 = full range, 0.0 = video range (16-235)
    float colorSpace;      // 0.0 = BT.709, 1.0 = BT.2020
    float planeCount;      // 2.0 = NV12/P010, 3.0 = planar YUV, 4.0 = YUVA/GBRAP
    float hasAlpha;        // 1.0 = has alpha plane
    float isRGBPlanar;     // 1.0 = GBRP/GBRAP (plane order: G, B, R, [A])
    float bitDepthScale;   // Normalization factor: 1.0 for 8-bit, 65535/1023 for 10-bit
};

//=============================================================================
// Texture Bindings
//=============================================================================

// Plane 0: Y (or G for GBRP)
layout(set = 0, binding = 1) uniform sampler2D texPlane0;

// Plane 1: UV interleaved (NV12/P010) - uses RG channels
layout(set = 0, binding = 2) uniform sampler2D texPlane1_NV12;

// Plane 1: U (planar, or B for GBRP) - uses R channel
layout(set = 0, binding = 3) uniform sampler2D texPlane1_U;

// Plane 2: V (planar, or R for GBRP) - uses R channel
layout(set = 0, binding = 4) uniform sampler2D texPlane2_V;

// Plane 3: Alpha (YUVA/GBRAP) - uses R channel
layout(set = 0, binding = 5) uniform sampler2D texPlane3_A;

//=============================================================================
// Color Matrices - YUV to RGB
//
// GLSL mat3 is column-major. These are constructed so that:
//   rgb = colorMat * vec3(Y, U, V)
// gives the correct result.
//=============================================================================

// BT.709 YUV to RGB matrix (column-major)
const mat3 BT709_MAT = mat3(
    1.0,      1.0,      1.0,        // column 0 (Y coefficients for R, G, B)
    0.0,     -0.18732,  1.8556,     // column 1 (U coefficients for R, G, B)
    1.5748,  -0.46812,  0.0         // column 2 (V coefficients for R, G, B)
);

// BT.2020 YUV to RGB matrix (column-major)
const mat3 BT2020_MAT = mat3(
    1.0,      1.0,      1.0,        // column 0
    0.0,     -0.16455,  1.8814,     // column 1
    1.4746,  -0.57135,  0.0         // column 2
);

void main() {
    vec2 uv = v_uv;
    vec3 rgb;
    float A = 1.0;

    if (isRGBPlanar > 0.5) {
        // GBRP/GBRAP path: planes are G, B, R order
        float G = texture(texPlane0, uv).r * bitDepthScale;
        float B = texture(texPlane1_U, uv).r * bitDepthScale;
        float R = texture(texPlane2_V, uv).r * bitDepthScale;
        rgb = vec3(R, G, B);

        if (hasAlpha > 0.5) {
            A = texture(texPlane3_A, uv).r * bitDepthScale;
        }
    } else {
        // YUV path
        float Y, U, V;
        bool needsScaling = false;

        if (planeCount < 2.5) {
            // 2-plane NV12/P010 layout (interleaved UV)
            // P010 values are MSB-aligned, no scaling needed
            Y = texture(texPlane0, uv).r;
            vec2 UV = texture(texPlane1_NV12, uv).rg;
            U = UV.x;
            V = UV.y;
            needsScaling = false;
        } else {
            // 3 or 4-plane planar YUV (YUV420P, YUV422P, YUV444P, YUVA)
            // Software decode stores values in lower bits, needs scaling for 10/12-bit
            Y = texture(texPlane0, uv).r;
            U = texture(texPlane1_U, uv).r;
            V = texture(texPlane2_V, uv).r;
            needsScaling = (bitDepthScale > 1.5);
        }

        // Apply bit depth scaling for 10/12-bit software-decoded planar content
        if (needsScaling) {
            Y = Y * bitDepthScale;
            U = U * bitDepthScale;
            V = V * bitDepthScale;
        }

        // Range expansion for video range (16-235/16-240)
        if (isFullRange < 0.5) {
            const float foot = 16.0 / 255.0;
            const float yHead = 235.0 / 255.0;
            const float uvHead = 240.0 / 255.0;
            Y = clamp((Y - foot) / (yHead - foot), 0.0, 1.0);
            U = clamp((U - foot) / (uvHead - foot), 0.0, 1.0);
            V = clamp((V - foot) / (uvHead - foot), 0.0, 1.0);
        }

        // Convert UV from [0,1] to [-0.5, 0.5]
        U = U - 0.5;
        V = V - 0.5;

        // Apply color matrix (BT.709 or BT.2020)
        mat3 colorMat = (colorSpace > 0.5) ? BT2020_MAT : BT709_MAT;
        vec3 yuv = vec3(Y, U, V);
        rgb = colorMat * yuv;

        if (hasAlpha > 0.5) {
            float rawA = texture(texPlane3_A, uv).r;
            A = (bitDepthScale > 1.5) ? rawA * bitDepthScale : rawA;
        }
    }

    out_color = vec4(clamp(rgb, 0.0, 1.0), A);
}
