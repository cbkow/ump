#version 450

// Passthrough fragment shader - identity blit
// Used for OCIO passthrough and texture copies

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D inputTexture;

void main() {
    out_color = texture(inputTexture, v_uv);
}
