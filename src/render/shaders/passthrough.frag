#version 440

// Phase 1.5: passthrough fragment shader — samples a 2D texture.
// Used by Pass 2 (swapchain pass) to display the contents of the
// compositeRaw offscreen texture written by Pass 1.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D u_tex;

void main() {
    fragColor = texture(u_tex, v_uv);
}
