#version 450 core

layout(location = 0) out vec4 fColor;
layout(set=0, binding=0) uniform sampler2D sTexture;
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

// HDR push constants (fragment stage, offset 16 from vertex push constants)
layout(push_constant) uniform uHDRConstants {
    layout(offset = 16) int HDRMode;        // 0 = SDR, 1 = HDR
    int TargetNits;                          // Target brightness * 10 (e.g. 1200 = 120 nits)
    int HDRPassthrough;                      // 0 = convert sRGB->PQ, 1 = skip (already PQ)
} hdr;

// sRGB gamma to linear light
vec3 srgb_to_linear(vec3 srgb) {
    return mix(srgb / 12.92, pow((srgb + 0.055) / 1.055, vec3(2.4)), step(0.04045, srgb));
}

// BT.709 to BT.2020 gamut conversion
vec3 bt709_to_bt2020(vec3 rgb) {
    const mat3 m = mat3(
        0.6274, 0.0691, 0.0164,
        0.3293, 0.9195, 0.0880,
        0.0433, 0.0114, 0.8956
    );
    return m * rgb;
}

// Linear to PQ (ST.2084) encoding
vec3 linear_to_pq(vec3 linear_col) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    // TargetNits is scaled by 10, so divide by 100000 instead of 10000
    vec3 L = max(linear_col * (float(hdr.TargetNits) / 100000.0), vec3(0.0));
    vec3 Lm = pow(L, vec3(m1));
    return pow((c1 + c2 * Lm) / (1.0 + c3 * Lm), vec3(m2));
}

void main()
{
    vec4 color = In.Color * texture(sTexture, In.UV.st);
    if (hdr.HDRMode != 0 && hdr.HDRPassthrough == 0) {
        color.rgb = linear_to_pq(bt709_to_bt2020(srgb_to_linear(color.rgb)));
    }
    fColor = color;
}
