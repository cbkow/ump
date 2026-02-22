#pragma once

#include <glad/gl.h>

namespace qcview {

//=============================================================================
// VideoRangeConverter
//
// Simple OpenGL shader pass to convert video (limited) range to full range.
//
// Limited range (10-bit): 64-940 → Full range: 0-1023
// Limited range (8-bit):  16-235 → Full range: 0-255
//
// This is needed because Media Foundation may output limited range even when
// full range is requested.
//=============================================================================

class VideoRangeConverter {
public:
    VideoRangeConverter() = default;
    ~VideoRangeConverter();

    // Delete copy/move
    VideoRangeConverter(const VideoRangeConverter&) = delete;
    VideoRangeConverter& operator=(const VideoRangeConverter&) = delete;

    // Initialize the converter
    bool Initialize();

    // Shutdown and release resources
    void Shutdown();

    // Check if initialized
    bool IsInitialized() const { return initialized_; }

    // Convert limited range texture to full range
    // Creates/resizes output texture as needed
    // Returns the output texture ID, or 0 on failure
    GLuint Convert(GLuint input_texture, int width, int height, bool is_10bit);

    // Get the output texture (same as last Convert result)
    GLuint GetOutputTexture() const { return output_texture_; }

private:
    bool CreateShader();
    bool EnsureOutputTexture(int width, int height);

    bool initialized_ = false;

    // Shader program
    GLuint shader_program_ = 0;
    GLint loc_input_texture_ = -1;
    GLint loc_range_scale_ = -1;
    GLint loc_range_offset_ = -1;

    // Fullscreen quad
    GLuint vao_ = 0;
    GLuint vbo_ = 0;

    // Output texture and FBO
    GLuint output_texture_ = 0;
    GLuint fbo_ = 0;
    int output_width_ = 0;
    int output_height_ = 0;
};

} // namespace qcview
