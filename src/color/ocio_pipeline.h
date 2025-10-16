#pragma once

#include <OpenColorIO/OpenColorIO.h>
#include <string>
#include <memory>
#include "../utils/debug_utils.h"

namespace OCIO = OCIO_NAMESPACE;

class OCIOPipeline {
public:
    OCIOPipeline();
    ~OCIOPipeline();

    bool BuildTestPipeline();

    // Build processor from transform description
    bool BuildFromDescription(const std::string& src_colorspace,
        const std::string& display,
        const std::string& view,
        const std::string& looks = "");

    // Generate and compile GLSL shader
    bool GenerateAndCompileShader();

    // Get the shader program ID for rendering
    unsigned int GetShaderProgram() const { return shader_program; }

    // Get the LUT texture IDs (if needed)
    const std::vector<unsigned int>& GetLUTTextureIDs() const { return lut_texture_ids; }

    // Check if pipeline is valid and ready to use
    bool IsValid() const { return is_valid; }

    // Update uniforms for rendering
    void UpdateUniforms(int video_texture_unit = 0, int lut_texture_unit = 1);

private:
    OCIO::ConstConfigRcPtr config;
    OCIO::ConstProcessorRcPtr processor;
    OCIO::GpuShaderDescRcPtr shader_desc;

    unsigned int shader_program;
    unsigned int vertex_shader;
    unsigned int fragment_shader;
    std::vector<unsigned int> lut_texture_ids;

    std::vector<std::string> lut_sampler_names;

    bool is_valid;
    bool needs_lut;

    bool CreatePassthroughPipeline();

    // Shader compilation helpers
    bool CompileShader(unsigned int& shader, const char* source, unsigned int type);
    bool LinkProgram();
    void CleanupShaders();
};