//!HOOK MAIN
//!BIND HOOKED
//!BIND EXR_FRAME
//!DESC EXR Frame Injection
//!WHEN ALWAYS

/*
 * EXR Injection Shader for UnionPlayer
 *
 * This shader replaces the dummy video pixels with EXR frame data.
 * The dummy video provides timing and container, while EXR provides pixels.
 *
 * Uniforms provided by UnionPlayer:
 * - exr_frame_index: Current frame index in EXR sequence
 * - exr_total_frames: Total number of frames in sequence
 * - exr_layer_name: Current EXR layer being displayed
 */

// Custom uniforms from UnionPlayer
uniform int exr_frame_index;
uniform int exr_total_frames;
uniform float exr_frame_progress;

vec4 hook() {
    // Get the original dummy video pixel (black)
    vec4 dummy_pixel = HOOKED_tex(HOOKED_pos);

    // Sample the EXR frame texture at the same position
    vec4 exr_pixel = EXR_FRAME_tex(HOOKED_pos);

    // Check if we have valid EXR data
    if (exr_pixel.a > 0.0) {
        // Use EXR pixel data - convert from linear to display
        // EXR data is typically in linear space, may need tone mapping
        vec3 linear_rgb = exr_pixel.rgb;

        // Simple exposure adjustment (can be made configurable later)
        float exposure = 1.0;
        vec3 exposed = linear_rgb * exposure;

        // Simple tone mapping (Reinhard)
        vec3 tone_mapped = exposed / (exposed + 1.0);

        // Gamma correction (assuming sRGB display)
        vec3 gamma_corrected = pow(tone_mapped, vec3(1.0/2.2));

        return vec4(gamma_corrected, 1.0);
    } else {
        // Fallback to dummy video (should be black for our generated dummies)
        return dummy_pixel;
    }
}