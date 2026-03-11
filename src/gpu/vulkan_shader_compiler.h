#pragma once

#ifdef __linux__

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

#include <vulkan/vulkan.h>

namespace qcview {

//=============================================================================
// ShaderStage
//=============================================================================

enum class ShaderStage {
    VERTEX,
    FRAGMENT,
    COMPUTE
};

//=============================================================================
// VulkanShaderCompiler
//
// Thin wrapper around shaderc for runtime GLSL -> SPIR-V compilation.
// Caches compiled modules by source hash to avoid recompilation.
//
// Usage:
//   VulkanShaderCompiler compiler;
//   auto spirv = compiler.CompileGLSL(glsl_source, ShaderStage::FRAGMENT, "my_shader");
//   VkShaderModule module = compiler.CreateShaderModule(spirv);
//=============================================================================

class VulkanShaderCompiler {
public:
    VulkanShaderCompiler();
    ~VulkanShaderCompiler();

    VulkanShaderCompiler(const VulkanShaderCompiler&) = delete;
    VulkanShaderCompiler& operator=(const VulkanShaderCompiler&) = delete;

    //=========================================================================
    // Compilation
    //=========================================================================

    // Compile GLSL source to SPIR-V binary
    // Returns empty vector on failure
    std::vector<uint32_t> CompileGLSL(
        const std::string& source,
        ShaderStage stage,
        const std::string& name = "shader",
        const std::string& entry_point = "main");

    // Create a VkShaderModule from SPIR-V binary
    // Caller owns the returned module (must destroy with vkDestroyShaderModule)
    VkShaderModule CreateShaderModule(const std::vector<uint32_t>& spirv);

    // Compile GLSL and create module in one step
    VkShaderModule CompileAndCreate(
        const std::string& source,
        ShaderStage stage,
        const std::string& name = "shader");

    //=========================================================================
    // Cache
    //=========================================================================

    // Check if a compiled module is cached
    bool HasCached(const std::string& key) const;

    // Get cached SPIR-V (returns empty vector if not cached)
    std::vector<uint32_t> GetCached(const std::string& key) const;

    // Clear the cache
    void ClearCache();

    //=========================================================================
    // Error Reporting
    //=========================================================================

    const std::string& GetLastError() const { return last_error_; }

private:
    // Cache: source hash -> SPIR-V binary
    std::unordered_map<std::string, std::vector<uint32_t>> cache_;
    mutable std::mutex cache_mutex_;

    std::string last_error_;

    // Generate cache key from source + stage
    static std::string MakeCacheKey(const std::string& source, ShaderStage stage);
};

} // namespace qcview

#endif // __linux__
