// yuv_to_rgb.metal — YUV to RGBA conversion compute shader for QCView
// Supports NV12 (8-bit) and P010 (10-bit), BT.709 and BT.2020 color matrices
// This file is for precompilation via metallib; the same shader is also
// embedded as a string in metal_yuv_renderer.mm for runtime compilation.

#include <metal_stdlib>
using namespace metal;

struct YUVParams {
    uint width;
    uint height;
    uint bit_depth;      // 8 or 10
    uint is_full_range;  // 0 = video range, 1 = full range
    uint is_bt2020;      // 0 = BT.709, 1 = BT.2020
    uint is_hdr;         // 0 = SDR, 1 = HDR (PQ source)
    uint subsampling;    // 0=4:2:0, 1=4:2:2, 2=4:4:4
};

// BT.709 YCbCr to RGB matrix (from ITU-R BT.709-6)
constant float3x3 bt709_matrix = float3x3(
    float3(1.0,  1.0,     1.0),
    float3(0.0, -0.1873,  1.8556),
    float3(1.5748, -0.4681, 0.0)
);

// BT.2020 NCL YCbCr to RGB matrix (from ITU-R BT.2020)
constant float3x3 bt2020_matrix = float3x3(
    float3(1.0,  1.0,     1.0),
    float3(0.0, -0.16455, 1.8814),
    float3(1.4746, -0.57135, 0.0)
);

kernel void yuv_to_rgba(
    texture2d<float, access::read> y_tex [[texture(0)]],
    texture2d<float, access::read> uv_tex [[texture(1)]],
    texture2d<float, access::write> out_tex [[texture(2)]],
    constant YUVParams& params [[buffer(0)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= params.width || gid.y >= params.height) return;

    float y_raw = y_tex.read(gid).r;
    uint2 uv_pos = uint2(gid.x / 2, gid.y / 2);
    float2 uv_raw = uv_tex.read(uv_pos).rg;

    float y, cb, cr;
    if (params.is_full_range) {
        y = y_raw;
        cb = uv_raw.x - 0.5;
        cr = uv_raw.y - 0.5;
    } else {
        y = (y_raw - 16.0/255.0) * (255.0/219.0);
        cb = (uv_raw.x - 128.0/255.0) * (255.0/224.0);
        cr = (uv_raw.y - 128.0/255.0) * (255.0/224.0);
    }

    float3 ycbcr = float3(y, cb, cr);
    float3 rgb;
    if (params.is_bt2020) {
        rgb = bt2020_matrix * ycbcr;
    } else {
        rgb = bt709_matrix * ycbcr;
    }

    rgb = clamp(rgb, 0.0, 1.0);
    out_tex.write(float4(rgb, 1.0), gid);
}

// Interleaved 4444+Alpha kernel (y416: R=A, G=Y, B=Cb, A=Cr)
kernel void yuv_interleaved_to_rgba(
    texture2d<float, access::read> in_tex [[texture(0)]],
    texture2d<float, access::write> out_tex [[texture(1)]],
    constant YUVParams& params [[buffer(0)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= params.width || gid.y >= params.height) return;

    float4 sample = in_tex.read(gid);
    float alpha  = sample.r;
    float y_val  = sample.g;
    float cb_raw = sample.b;
    float cr_raw = sample.a;

    float y, cb, cr;
    if (params.is_full_range) {
        y  = y_val;
        cb = cb_raw - 0.5;
        cr = cr_raw - 0.5;
    } else {
        y  = (y_val  - 16.0/255.0) * (255.0/219.0);
        cb = (cb_raw - 128.0/255.0) * (255.0/224.0);
        cr = (cr_raw - 128.0/255.0) * (255.0/224.0);
    }

    float3 ycbcr = float3(y, cb, cr);
    float3 rgb;
    if (params.is_bt2020) {
        rgb = bt2020_matrix * ycbcr;
    } else {
        rgb = bt709_matrix * ycbcr;
    }

    rgb = clamp(rgb, 0.0, 1.0);
    out_tex.write(float4(rgb, alpha), gid);
}

// Passthrough shader (identity — used when no color conversion needed)
kernel void rgba_passthrough(
    texture2d<float, access::read> in_tex [[texture(0)]],
    texture2d<float, access::write> out_tex [[texture(1)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= in_tex.get_width() || gid.y >= in_tex.get_height()) return;
    out_tex.write(in_tex.read(gid), gid);
}
