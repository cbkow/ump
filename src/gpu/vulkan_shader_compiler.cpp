#include "vulkan_shader_compiler.h"

#ifdef __linux__

#include "vulkan_device.h"
#include "../utils/debug_utils.h"

#include <shaderc/shaderc.hpp>
#include <functional>

namespace qcview {

VulkanShaderCompiler::VulkanShaderCompiler() = default;

VulkanShaderCompiler::~VulkanShaderCompiler() {
    ClearCache();
}

std::string VulkanShaderCompiler::MakeCacheKey(const std::string& source, ShaderStage stage) {
    // Simple hash: combine source hash with stage
    std::size_t h = std::hash<std::string>{}(source);
    h ^= std::hash<int>{}(static_cast<int>(stage)) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return std::to_string(h);
}

std::vector<uint32_t> VulkanShaderCompiler::CompileGLSL(
    const std::string& source,
    ShaderStage stage,
    const std::string& name,
    const std::string& entry_point) {

    // Check cache
    std::string key = MakeCacheKey(source, stage);
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            return it->second;
        }
    }

    // Map stage to shaderc kind
    shaderc_shader_kind kind;
    switch (stage) {
        case ShaderStage::VERTEX:   kind = shaderc_vertex_shader; break;
        case ShaderStage::FRAGMENT: kind = shaderc_fragment_shader; break;
        case ShaderStage::COMPUTE:  kind = shaderc_compute_shader; break;
        default:
            last_error_ = "Unknown shader stage";
            return {};
    }

    // Compile
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;

    // Target Vulkan 1.0 / SPIR-V 1.0
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);

    shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(
        source, kind, name.c_str(), entry_point.c_str(), options);

    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        last_error_ = result.GetErrorMessage();
        Debug::Log("VulkanShaderCompiler: Failed to compile '" + name + "': " + last_error_);
        return {};
    }

    std::vector<uint32_t> spirv(result.cbegin(), result.cend());

    // Cache the result
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        cache_[key] = spirv;
    }

    Debug::Log("VulkanShaderCompiler: Compiled '" + name + "' (" +
               std::to_string(spirv.size() * 4) + " bytes SPIR-V)");

    return spirv;
}

VkShaderModule VulkanShaderCompiler::CreateShaderModule(const std::vector<uint32_t>& spirv) {
    if (spirv.empty()) {
        last_error_ = "Empty SPIR-V binary";
        return VK_NULL_HANDLE;
    }

    auto& device_mgr = VulkanDeviceManager::Instance();
    VkDevice device = device_mgr.GetDevice();
    if (device == VK_NULL_HANDLE) {
        last_error_ = "No Vulkan device";
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = spirv.size() * sizeof(uint32_t);
    create_info.pCode = spirv.data();

    VkShaderModule module = VK_NULL_HANDLE;
    VkResult vr = vkCreateShaderModule(device, &create_info, nullptr, &module);
    if (vr != VK_SUCCESS) {
        last_error_ = "vkCreateShaderModule failed: " + std::to_string(vr);
        Debug::Log("VulkanShaderCompiler: " + last_error_);
        return VK_NULL_HANDLE;
    }

    return module;
}

VkShaderModule VulkanShaderCompiler::CompileAndCreate(
    const std::string& source,
    ShaderStage stage,
    const std::string& name) {

    auto spirv = CompileGLSL(source, stage, name);
    if (spirv.empty()) return VK_NULL_HANDLE;
    return CreateShaderModule(spirv);
}

bool VulkanShaderCompiler::HasCached(const std::string& key) const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    return cache_.find(key) != cache_.end();
}

std::vector<uint32_t> VulkanShaderCompiler::GetCached(const std::string& key) const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        return it->second;
    }
    return {};
}

void VulkanShaderCompiler::ClearCache() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_.clear();
}

} // namespace qcview

#endif // __linux__
