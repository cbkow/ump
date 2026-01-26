#pragma once

#include <OpenColorIO/OpenColorIO.h>
#include <string>
#include <memory>
#include <vector>
#include "../utils/debug_utils.h"

#ifdef _WIN32
#include <d3d11_1.h>
#include <wrl/client.h>
#endif

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
        const std::string& looks = "",
        const std::vector<std::string>& scene_lut_files = {},
        const std::vector<std::string>& display_lut_files = {});

    // Generate and compile GLSL shader
    bool GenerateAndCompileShader();

    // Get the shader program ID for rendering (OpenGL)
    unsigned int GetShaderProgram() const { return shader_program; }

    // Get the LUT texture IDs (if needed) (OpenGL)
    const std::vector<unsigned int>& GetLUTTextureIDs() const { return lut_texture_ids; }

    // Get LUT texture dimensions (1 for 1D LUTs, 3 for 3D LUTs)
    const std::vector<int>& GetLUTTextureDimensions() const { return lut_texture_dimensions; }

    // Check if pipeline is valid and ready to use
    bool IsValid() const { return is_valid; }

    // Check if this is a passthrough pipeline (no actual color transform)
    bool IsPassthrough() const { return is_passthrough; }

    // Update uniforms for rendering (OpenGL)
    void UpdateUniforms(int video_texture_unit = 0, int lut_texture_unit = 1);

    // Create a passthrough pipeline (identity transform, no color correction)
    // Used for frame buffering during media transitions when no OCIO config is active
    bool CreatePassthroughPipeline();

#ifdef _WIN32
    //=========================================================================
    // D3D11/HLSL Support
    //=========================================================================

    // Generate and compile HLSL shader for D3D11
    bool GenerateAndCompileShaderD3D11();

    // Create a passthrough pipeline for D3D11
    bool CreatePassthroughPipelineD3D11();

    // Check if D3D11 shaders are available
    bool HasD3D11Shaders() const { return d3d_pixel_shader_ != nullptr; }

    // Get D3D11 shaders
    ID3D11VertexShader* GetD3D11VertexShader() const { return d3d_vertex_shader_.Get(); }
    ID3D11PixelShader* GetD3D11PixelShader() const { return d3d_pixel_shader_.Get(); }

    // Get D3D11 LUT textures and samplers
    const std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>>& GetD3D11LUTSRVs() const { return d3d_lut_srvs_; }
    const std::vector<Microsoft::WRL::ComPtr<ID3D11SamplerState>>& GetD3D11LUTSamplers() const { return d3d_lut_samplers_; }

    // Get constant buffer
    ID3D11Buffer* GetD3D11ConstantBuffer() const { return d3d_constant_buffer_.Get(); }
#endif

private:
    OCIO::ConstConfigRcPtr config;
    OCIO::ConstProcessorRcPtr processor;
    OCIO::GpuShaderDescRcPtr shader_desc;

    // OpenGL resources
    unsigned int shader_program;
    unsigned int vertex_shader;
    unsigned int fragment_shader;
    std::vector<unsigned int> lut_texture_ids;
    std::vector<int> lut_texture_dimensions;  // 1 for 1D LUTs, 3 for 3D LUTs

    std::vector<std::string> lut_sampler_names;

    bool is_valid;
    bool is_passthrough;
    bool needs_lut;

    // Shader compilation helpers (OpenGL)
    bool CompileShader(unsigned int& shader, const char* source, unsigned int type);
    bool LinkProgram();
    void CleanupShaders();

#ifdef _WIN32
    // D3D11 resources
    Microsoft::WRL::ComPtr<ID3D11VertexShader> d3d_vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> d3d_pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> d3d_constant_buffer_;

    // D3D11 LUT textures
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture1D>> d3d_lut_1d_;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture3D>> d3d_lut_3d_;
    std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> d3d_lut_srvs_;
    std::vector<Microsoft::WRL::ComPtr<ID3D11SamplerState>> d3d_lut_samplers_;

    // D3D11 helpers
    void CleanupD3D11Shaders();
#endif
};