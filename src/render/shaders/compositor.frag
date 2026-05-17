#version 440

// Phase 1.7: mode-aware compositor.
//
// Samples up to two source textures (uploaded once at init from
// CPU-generated test images in Phase 1.7; replaced by video-frame
// uploads in Phase 2+) and composites them into compositeRaw based
// on a runtime-selected mode.
//
// Modes:
//   0 = SINGLE         — Source A only, aspect-fit to output.
//   1 = SIDE_BY_SIDE   — Source A in left half, Source B in right half.
//   2 = SPLIT_WIPE     — Vertical split at u.splitPos. uv.x < splitPos
//                        sees Source A; uv.x >= splitPos sees Source B.
//                        A 1px white separator marks the split line.
//
// All modes apply a "fit-inside" letterbox/pillarbox so the source is
// never stretched non-uniformly. Out-of-fit areas are filled with
// black (the letterbox/pillarbox color).

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Uniforms {
    vec2  resolution;   // 0..7   pixel size of compositeRaw
    vec2  srcSize;      // 8..15  shared source pixel size (A and B)
    int   mode;         // 16..19
    float splitPos;     // 20..23
    // 8 bytes pad to align background at offset 32
    vec4  background;   // 32..47 source-over composite color (alpha-aware)
} u;

layout(binding = 1) uniform sampler2D u_srcA;
layout(binding = 2) uniform sampler2D u_srcB;

// Aspect-fit a source of size `srcPx` into a target rect of size
// `tgtPx`, sampling at target-relative position `tgtUV` (0..1).
// Returns sampled RGBA or vec4(0,0,0,0) for letterbox/pillarbox
// area (transparent — the caller composites against the background,
// which then shows the bg color in those regions instead of black).
vec4 sampleFitA(vec2 tgtUV, vec2 tgtPx) {
    vec2 tgtPxPos = tgtUV * tgtPx;
    float scale = min(tgtPx.x / u.srcSize.x, tgtPx.y / u.srcSize.y);
    vec2 scaledSrc = u.srcSize * scale;
    vec2 offset = (tgtPx - scaledSrc) * 0.5;
    vec2 inSrcPx = (tgtPxPos - offset) / scale;
    if (inSrcPx.x < 0.0 || inSrcPx.x >= u.srcSize.x ||
        inSrcPx.y < 0.0 || inSrcPx.y >= u.srcSize.y) {
        return vec4(0.0);
    }
    return texture(u_srcA, inSrcPx / u.srcSize);
}

vec4 sampleFitB(vec2 tgtUV, vec2 tgtPx) {
    vec2 tgtPxPos = tgtUV * tgtPx;
    float scale = min(tgtPx.x / u.srcSize.x, tgtPx.y / u.srcSize.y);
    vec2 scaledSrc = u.srcSize * scale;
    vec2 offset = (tgtPx - scaledSrc) * 0.5;
    vec2 inSrcPx = (tgtPxPos - offset) / scale;
    if (inSrcPx.x < 0.0 || inSrcPx.x >= u.srcSize.x ||
        inSrcPx.y < 0.0 || inSrcPx.y >= u.srcSize.y) {
        return vec4(0.0);
    }
    return texture(u_srcB, inSrcPx / u.srcSize);
}

void main() {
    vec4 src = vec4(0.0);

    if (u.mode == 0) {
        // SINGLE — Source A only, full-frame aspect-fit.
        src = sampleFitA(v_uv, u.resolution);
    }
    else if (u.mode == 1) {
        // SIDE_BY_SIDE — each source aspect-fit into its half.
        vec2 halfSize = vec2(u.resolution.x * 0.5, u.resolution.y);
        if (v_uv.x < 0.5) {
            vec2 lhsUV = vec2(v_uv.x * 2.0, v_uv.y);
            src = sampleFitA(lhsUV, halfSize);
        } else {
            vec2 rhsUV = vec2((v_uv.x - 0.5) * 2.0, v_uv.y);
            src = sampleFitB(rhsUV, halfSize);
        }
    }
    else {
        // SPLIT_WIPE — single shared aspect-fit, source chosen per-pixel.
        if (v_uv.x < u.splitPos) {
            src = sampleFitA(v_uv, u.resolution);
        } else {
            src = sampleFitB(v_uv, u.resolution);
        }
        // 1px white separator at the split line — opaque.
        float pxDist = abs((v_uv.x - u.splitPos) * u.resolution.x);
        if (pxDist < 1.0) {
            src = vec4(1.0);
        }
    }

    // Source-over composite against u.background. swscale's
    // AV_PIX_FMT_RGBA output is straight (non-premultiplied) alpha,
    // so the standard mix formula applies: a=0 picks the bg, a=1
    // picks the source. Out-of-fit areas (sampleFit returned
    // vec4(0)) become the background, replacing the previous
    // opaque black. Output is opaque — alpha against the swapchain
    // is handled in the display path (Phase 2 + Guide 06).
    vec3 final = mix(u.background.rgb, src.rgb, src.a);
    fragColor = vec4(final, 1.0);
}
