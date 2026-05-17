#version 440

// Phase 1.4: fullscreen quad / oversized triangle vertex shader.
// Three vertices form a triangle that covers the screen after clipping;
// no index buffer needed.

layout(location = 0) in vec2 a_position;

layout(location = 0) out vec2 v_uv;

void main() {
    // Position is in clip-space (-1..1); UVs are 0..1 across the screen.
    v_uv = a_position * 0.5 + 0.5;
    gl_Position = vec4(a_position, 0.0, 1.0);
}
