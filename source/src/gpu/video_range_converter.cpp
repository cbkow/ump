#include "video_range_converter.h"
#include "../utils/debug_utils.h"

namespace ump {

// Simple vertex shader for fullscreen quad
static const char* vertex_shader_source = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 TexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

// Fragment shader for range expansion
static const char* fragment_shader_source = R"(
#version 330 core
in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D inputTexture;
uniform float rangeScale;   // 1023/(940-64) = 1.1678 for 10-bit
uniform float rangeOffset;  // 64/1023 = 0.0626 for 10-bit

void main() {
    vec4 color = texture(inputTexture, TexCoord);

    // Expand limited range to full range
    // output = (input - offset) * scale
    color.rgb = (color.rgb - rangeOffset) * rangeScale;

    // Clamp to valid range
    color.rgb = clamp(color.rgb, 0.0, 1.0);

    FragColor = color;
}
)";

VideoRangeConverter::~VideoRangeConverter() {
    Shutdown();
}

bool VideoRangeConverter::Initialize() {
    if (initialized_) {
        return true;
    }

    if (!CreateShader()) {
        Debug::Log("VideoRangeConverter: ERROR - Failed to create shader");
        return false;
    }

    // Create fullscreen quad VAO/VBO
    float quadVertices[] = {
        // positions   // texCoords
        -1.0f,  1.0f,  0.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,

        -1.0f,  1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f
    };

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    // Create FBO
    glGenFramebuffers(1, &fbo_);

    initialized_ = true;
    Debug::Log("VideoRangeConverter: Initialized");
    return true;
}

void VideoRangeConverter::Shutdown() {
    if (fbo_) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }

    if (output_texture_) {
        glDeleteTextures(1, &output_texture_);
        output_texture_ = 0;
    }

    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }

    if (vbo_) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }

    if (shader_program_) {
        glDeleteProgram(shader_program_);
        shader_program_ = 0;
    }

    output_width_ = 0;
    output_height_ = 0;
    initialized_ = false;
}

bool VideoRangeConverter::CreateShader() {
    // Compile vertex shader
    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &vertex_shader_source, nullptr);
    glCompileShader(vertex_shader);

    GLint success;
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(vertex_shader, 512, nullptr, log);
        Debug::Log("VideoRangeConverter: Vertex shader error: " + std::string(log));
        glDeleteShader(vertex_shader);
        return false;
    }

    // Compile fragment shader
    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &fragment_shader_source, nullptr);
    glCompileShader(fragment_shader);

    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(fragment_shader, 512, nullptr, log);
        Debug::Log("VideoRangeConverter: Fragment shader error: " + std::string(log));
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        return false;
    }

    // Link program
    shader_program_ = glCreateProgram();
    glAttachShader(shader_program_, vertex_shader);
    glAttachShader(shader_program_, fragment_shader);
    glLinkProgram(shader_program_);

    glGetProgramiv(shader_program_, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(shader_program_, 512, nullptr, log);
        Debug::Log("VideoRangeConverter: Shader link error: " + std::string(log));
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        glDeleteProgram(shader_program_);
        shader_program_ = 0;
        return false;
    }

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    // Get uniform locations
    loc_input_texture_ = glGetUniformLocation(shader_program_, "inputTexture");
    loc_range_scale_ = glGetUniformLocation(shader_program_, "rangeScale");
    loc_range_offset_ = glGetUniformLocation(shader_program_, "rangeOffset");

    return true;
}

bool VideoRangeConverter::EnsureOutputTexture(int width, int height) {
    if (output_texture_ && output_width_ == width && output_height_ == height) {
        return true;
    }

    if (output_texture_) {
        glDeleteTextures(1, &output_texture_);
    }

    glGenTextures(1, &output_texture_);
    glBindTexture(GL_TEXTURE_2D, output_texture_);

    // Use RGB10_A2 for 10-bit output to match input format
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, width, height, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, nullptr);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    output_width_ = width;
    output_height_ = height;

    return output_texture_ != 0;
}

GLuint VideoRangeConverter::Convert(GLuint input_texture, int width, int height, bool is_10bit) {
    if (!initialized_ || !input_texture) {
        return 0;
    }

    if (!EnsureOutputTexture(width, height)) {
        return 0;
    }

    // Save current state
    GLint prev_fbo, prev_viewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);

    // Bind our FBO with output texture
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output_texture_, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        Debug::Log("VideoRangeConverter: ERROR - Framebuffer incomplete");
        glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
        return 0;
    }

    glViewport(0, 0, width, height);

    // Use shader
    glUseProgram(shader_program_);

    // Bind input texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, input_texture);
    glUniform1i(loc_input_texture_, 0);

    // Set range conversion parameters
    // Only fix blacks - don't expand brights (already clipping on SDR monitors anyway)
    // Map 64-1023 → 0-1023 (10-bit) or 16-255 → 0-255 (8-bit)
    float offset, scale;
    if (is_10bit) {
        offset = 64.0f / 1023.0f;   // 0.0626
        scale = 1023.0f / (1023.0f - 64.0f);  // 1.0667
    } else {
        offset = 16.0f / 255.0f;    // 0.0627
        scale = 255.0f / (255.0f - 16.0f);   // 1.0669
    }

    glUniform1f(loc_range_offset_, offset);
    glUniform1f(loc_range_scale_, scale);

    // Draw fullscreen quad
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Restore state
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);

    return output_texture_;
}

} // namespace ump
