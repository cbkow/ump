#include "d3d11_vulkan_yuv_compositor.h"

#include "decode/vulkan/vulkan_device_manager.h"
#include "render/d3d11/d3d11_device_manager.h"

#include <QtLogging>

#include <shaderc/shaderc.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <d3d11_1.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <vector>

using Microsoft::WRL::ComPtr;

namespace qcv {

namespace {

// Phase F.2.4.3 Stage C.2.c — unified YUV(A)/RGB(A) → RGBA16F compute
// shader (moved verbatim from the old D3D11VulkanDecodeBridge). One
// pipeline covers planar YUV 3- and 4-plane, planar GBR(A), and
// biplanar NV12/P010/P012/P016. All format-specific behavior is push-
// constant driven so we share a single pipeline + descriptor set.
constexpr const char *kPlanarYuvRgbComputeGlsl = R"(#version 450

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D plane0;  // Y / G
layout(set = 0, binding = 1) uniform sampler2D plane1;  // U / B
layout(set = 0, binding = 2) uniform sampler2D plane2;  // V / R
layout(set = 0, binding = 3) uniform sampler2D plane3;  // A (or duplicate of plane2 for 3-plane)
layout(set = 0, binding = 4, rgba16f) uniform writeonly image2D outImg;

layout(push_constant) uniform PC {
    uint  width;
    uint  height;
    float bitScale;
    int   colorSpace;
    int   range;
    int   hasAlpha;
    int   isRgb;
    int   isBiplanar;
};

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= int(width) || px.y >= int(height)) return;
    vec2 uv = (vec2(px) + 0.5) / vec2(width, height);

    float s0, s1, s2;
    s0 = textureLod(plane0, uv, 0.0).r * bitScale;
    if (isBiplanar != 0) {
        vec2 uvPair = textureLod(plane1, uv, 0.0).rg * bitScale;
        s1 = uvPair.x;
        s2 = uvPair.y;
    } else {
        s1 = textureLod(plane1, uv, 0.0).r * bitScale;
        s2 = textureLod(plane2, uv, 0.0).r * bitScale;
    }
    float a  = (hasAlpha != 0) ? textureLod(plane3, uv, 0.0).r * bitScale : 1.0;

    float r, g, b;
    if (isRgb != 0) {
        g = s0; b = s1; r = s2;
        if (range == 0) {
            r = (r - 16.0/255.0) * (255.0/219.0);
            g = (g - 16.0/255.0) * (255.0/219.0);
            b = (b - 16.0/255.0) * (255.0/219.0);
        }
    } else {
        float y = s0, u = s1, v = s2;
        if (range == 0) {
            y = (y - 16.0/255.0) * (255.0/219.0);
            u = (u - 128.0/255.0) * (255.0/224.0);
            v = (v - 128.0/255.0) * (255.0/224.0);
        } else {
            u -= 0.5; v -= 0.5;
        }
        if (colorSpace == 1) {           // BT.709
            r = y + 1.5748   * v;
            g = y - 0.1873   * u - 0.4681  * v;
            b = y + 1.8556   * u;
        } else if (colorSpace == 2) {    // BT.2020 NCL
            r = y + 1.4746   * v;
            g = y - 0.16455  * u - 0.57135 * v;
            b = y + 1.8814   * u;
        } else {                         // BT.601
            r = y + 1.402    * v;
            g = y - 0.34414  * u - 0.71414 * v;
            b = y + 1.772    * u;
        }
    }

    imageStore(outImg, px, vec4(r, g, b, a));
}
)";

struct ComputePushConstants {
    uint32_t width;
    uint32_t height;
    float    bitScale;
    int32_t  colorSpace;
    int32_t  range;
    int32_t  hasAlpha;
    int32_t  isRgb;
    int32_t  isBiplanar;
};

} // namespace

struct D3D11VulkanYuvCompositor::Impl {
    bool initialized = false;

    // Compute pipeline + descriptor plumbing — all stateless across
    // streams. Built once in initialize(); reused for every dispatch.
    VkShaderModule        computeShader   = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsLayout        = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout  = VK_NULL_HANDLE;
    VkPipeline            computePipeline = VK_NULL_HANDLE;
    VkDescriptorPool      dsPool          = VK_NULL_HANDLE;
    VkDescriptorSet       dsSet           = VK_NULL_HANDLE;
    VkSampler             sampler         = VK_NULL_HANDLE;
    VkCommandPool         cmdPool         = VK_NULL_HANDLE;
    VkCommandBuffer       cmdBuf          = VK_NULL_HANDLE;
    VkFence               cmdFence        = VK_NULL_HANDLE;   // fallback path only
    VkQueue               computeQueue    = VK_NULL_HANDLE;
    uint32_t              computeQueueFam = UINT32_MAX;

    // Track the storage-image binding so we only re-bind when the
    // bridge's output VkImageView changes (per-stream realloc, or
    // bridge switch). Steady-state has zero storage-image writes.
    VkImageView           boundStorageView = VK_NULL_HANDLE;

    // Cross-API timeline fence — D3D11 fence shared into Vulkan as a
    // timeline semaphore. Vulkan signals on compute submit; D3D11
    // queues a Wait on the immediate context so the next sample of
    // ANY bridge's output SRV blocks GPU-side until our compute is
    // visible. One shared timeline across all bridges is correct
    // because it gates the next D3D11 draw, not per-resource sync.
    Microsoft::WRL::ComPtr<ID3D11Fence>          d3dFence;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> d3dContext4;
    HANDLE          fenceHandle  = nullptr;
    VkSemaphore     vkSemaphore  = VK_NULL_HANDLE;
    uint64_t        timelineValue = 0;
    bool            timelineReady = false;
};

namespace {

bool buildTimelineFence(D3D11VulkanYuvCompositor::Impl &impl)
{
    auto *d3dDev = static_cast<ID3D11Device *>(
        qcv::D3D11DeviceManager::instance().device());
    auto *d3dCtx = static_cast<ID3D11DeviceContext *>(
        qcv::D3D11DeviceManager::instance().context());
    if (!d3dDev || !d3dCtx) return false;

    ComPtr<ID3D11Device5> dev5;
    if (FAILED(d3dDev->QueryInterface(IID_PPV_ARGS(dev5.GetAddressOf())))) {
        qWarning("D3D11VulkanYuvCompositor: ID3D11Device5 unavailable (need "
                 "Windows 10 1607+ for fences)");
        return false;
    }
    if (FAILED(d3dCtx->QueryInterface(IID_PPV_ARGS(impl.d3dContext4.GetAddressOf())))) {
        qWarning("D3D11VulkanYuvCompositor: ID3D11DeviceContext4 unavailable");
        return false;
    }
    if (FAILED(dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                  IID_PPV_ARGS(impl.d3dFence.GetAddressOf())))) {
        qWarning("D3D11VulkanYuvCompositor: ID3D11Device5::CreateFence failed");
        impl.d3dContext4.Reset();
        return false;
    }
    if (FAILED(impl.d3dFence->CreateSharedHandle(
            nullptr, GENERIC_ALL, nullptr, &impl.fenceHandle))) {
        qWarning("D3D11VulkanYuvCompositor: ID3D11Fence::CreateSharedHandle failed");
        impl.d3dFence.Reset();
        impl.d3dContext4.Reset();
        return false;
    }

    auto &vkMgr = qcv::VulkanDeviceManager::instance();
    VkDevice device = vkMgr.device();

    VkSemaphoreTypeCreateInfo timelineInfo{};
    timelineInfo.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineInfo.initialValue  = 0;

    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.pNext = &timelineInfo;
    if (vkCreateSemaphore(device, &sci, nullptr, &impl.vkSemaphore) != VK_SUCCESS) {
        qWarning("D3D11VulkanYuvCompositor: vkCreateSemaphore (timeline) failed");
        CloseHandle(impl.fenceHandle); impl.fenceHandle = nullptr;
        impl.d3dFence.Reset();
        impl.d3dContext4.Reset();
        return false;
    }

    auto pfnImport = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
        vkGetInstanceProcAddr(vkMgr.vkInstance(),
                               "vkImportSemaphoreWin32HandleKHR"));
    if (!pfnImport) {
        qWarning("D3D11VulkanYuvCompositor: vkImportSemaphoreWin32HandleKHR not resolved");
        vkDestroySemaphore(device, impl.vkSemaphore, nullptr); impl.vkSemaphore = VK_NULL_HANDLE;
        CloseHandle(impl.fenceHandle); impl.fenceHandle = nullptr;
        impl.d3dFence.Reset();
        impl.d3dContext4.Reset();
        return false;
    }

    VkImportSemaphoreWin32HandleInfoKHR importInfo{};
    importInfo.sType      = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
    importInfo.semaphore  = impl.vkSemaphore;
    importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
    importInfo.handle     = impl.fenceHandle;
    importInfo.flags      = 0;
    if (pfnImport(device, &importInfo) != VK_SUCCESS) {
        qWarning("D3D11VulkanYuvCompositor: vkImportSemaphoreWin32HandleKHR failed");
        vkDestroySemaphore(device, impl.vkSemaphore, nullptr); impl.vkSemaphore = VK_NULL_HANDLE;
        CloseHandle(impl.fenceHandle); impl.fenceHandle = nullptr;
        impl.d3dFence.Reset();
        impl.d3dContext4.Reset();
        return false;
    }

    impl.timelineValue = 0;
    impl.timelineReady = true;
    return true;
}

void teardownTimelineFence(D3D11VulkanYuvCompositor::Impl &impl)
{
    auto &vkMgr = qcv::VulkanDeviceManager::instance();
    VkDevice device = vkMgr.isInitialized() ? vkMgr.device() : VK_NULL_HANDLE;
    if (device != VK_NULL_HANDLE && impl.vkSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(device, impl.vkSemaphore, nullptr);
    }
    impl.vkSemaphore = VK_NULL_HANDLE;
    if (impl.fenceHandle) { CloseHandle(impl.fenceHandle); impl.fenceHandle = nullptr; }
    impl.d3dFence.Reset();
    impl.d3dContext4.Reset();
    impl.timelineReady = false;
    impl.timelineValue = 0;
}

bool buildComputePipeline(D3D11VulkanYuvCompositor::Impl &impl)
{
    auto &vkMgr   = qcv::VulkanDeviceManager::instance();
    VkDevice device = vkMgr.device();
    if (device == VK_NULL_HANDLE) return false;

    // 1. Compile GLSL → SPIR-V via shaderc.
    shaderc::Compiler  compiler;
    shaderc::CompileOptions options;
    options.SetSourceLanguage(shaderc_source_language_glsl);
    options.SetTargetEnvironment(shaderc_target_env_vulkan,
                                  shaderc_env_version_vulkan_1_3);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);

    shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(
        kPlanarYuvRgbComputeGlsl, shaderc_compute_shader,
        "planar_yuv_rgb.comp", options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        qWarning("D3D11VulkanYuvCompositor: shaderc compile failed: %s",
                 result.GetErrorMessage().c_str());
        return false;
    }
    const std::vector<uint32_t> spirv(result.cbegin(), result.cend());

    VkShaderModuleCreateInfo smci{};
    smci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = spirv.size() * sizeof(uint32_t);
    smci.pCode    = spirv.data();
    if (vkCreateShaderModule(device, &smci, nullptr, &impl.computeShader) != VK_SUCCESS) {
        qWarning("D3D11VulkanYuvCompositor: vkCreateShaderModule failed");
        return false;
    }

    // 2. Descriptor set layout — 4 input samplers + 1 storage image
    // output (5 bindings total).
    VkDescriptorSetLayoutBinding bindings[5]{};
    for (int i = 0; i < 4; ++i) {
        bindings[i].binding         = static_cast<uint32_t>(i);
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[4].binding         = 4;
    bindings[4].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 5;
    dslci.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(device, &dslci, nullptr, &impl.dsLayout) != VK_SUCCESS) {
        qWarning("D3D11VulkanYuvCompositor: vkCreateDescriptorSetLayout failed");
        return false;
    }

    // 3. Pipeline layout — descriptor set + push constants.
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(ComputePushConstants);

    VkPipelineLayoutCreateInfo plci{};
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &impl.dsLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pcRange;
    if (vkCreatePipelineLayout(device, &plci, nullptr, &impl.pipelineLayout) != VK_SUCCESS) {
        qWarning("D3D11VulkanYuvCompositor: vkCreatePipelineLayout failed");
        return false;
    }

    // 4. Compute pipeline.
    VkComputePipelineCreateInfo cpci{};
    cpci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.layout = impl.pipelineLayout;
    cpci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = impl.computeShader;
    cpci.stage.pName  = "main";
    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci,
                                   nullptr, &impl.computePipeline) != VK_SUCCESS) {
        qWarning("D3D11VulkanYuvCompositor: vkCreateComputePipelines failed");
        return false;
    }

    // 5. Sampler (one, shared across all input planes).
    VkSamplerCreateInfo sci{};
    sci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter    = VK_FILTER_LINEAR;
    sci.minFilter    = VK_FILTER_LINEAR;
    sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.borderColor  = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    if (vkCreateSampler(device, &sci, nullptr, &impl.sampler) != VK_SUCCESS) {
        qWarning("D3D11VulkanYuvCompositor: vkCreateSampler failed");
        return false;
    }

    // 6. Descriptor pool + persistent descriptor set (4 samplers + 1
    // storage image). The storage-image binding is (re)written
    // whenever a dispatch arrives with a different outputView; the
    // sampler bindings are written every dispatch.
    VkDescriptorPoolSize pss[2]{};
    pss[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pss[0].descriptorCount = 4;
    pss[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pss[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets       = 1;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes    = pss;
    if (vkCreateDescriptorPool(device, &dpci, nullptr, &impl.dsPool) != VK_SUCCESS) {
        qWarning("D3D11VulkanYuvCompositor: vkCreateDescriptorPool failed");
        return false;
    }

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = impl.dsPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &impl.dsLayout;
    if (vkAllocateDescriptorSets(device, &dsai, &impl.dsSet) != VK_SUCCESS) {
        qWarning("D3D11VulkanYuvCompositor: vkAllocateDescriptorSets failed");
        return false;
    }

    // 7. Command pool + command buffer. ONE shared cmd buf across all
    // bridges; the dispatch path's timeline-wait at the top serializes
    // back-to-back dispatches so the buf is safe to reset each time.
    impl.computeQueueFam = vkMgr.graphicsFamily();
    impl.computeQueue    = vkMgr.graphicsQueue();
    VkCommandPoolCreateInfo cpci2{};
    cpci2.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci2.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci2.queueFamilyIndex = impl.computeQueueFam;
    if (vkCreateCommandPool(device, &cpci2, nullptr, &impl.cmdPool) != VK_SUCCESS) {
        qWarning("D3D11VulkanYuvCompositor: vkCreateCommandPool failed");
        return false;
    }

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = impl.cmdPool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &cbai, &impl.cmdBuf) != VK_SUCCESS) {
        qWarning("D3D11VulkanYuvCompositor: vkAllocateCommandBuffers failed");
        return false;
    }

    // 8. Fence for fallback path (when timeline-import isn't available).
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(device, &fci, nullptr, &impl.cmdFence) != VK_SUCCESS) {
        qWarning("D3D11VulkanYuvCompositor: vkCreateFence failed");
        return false;
    }

    qInfo("D3D11VulkanYuvCompositor: compute pipeline built (YUV/RGB → "
          "RGBA16F; queue family %u)", impl.computeQueueFam);
    return true;
}

void teardownComputePipeline(D3D11VulkanYuvCompositor::Impl &impl)
{
    auto &vkMgr = qcv::VulkanDeviceManager::instance();
    VkDevice device = vkMgr.isInitialized() ? vkMgr.device() : VK_NULL_HANDLE;
    if (device != VK_NULL_HANDLE) {
        if (impl.cmdFence) { vkDestroyFence(device, impl.cmdFence, nullptr); impl.cmdFence = VK_NULL_HANDLE; }
        if (impl.cmdPool)  { vkDestroyCommandPool(device, impl.cmdPool, nullptr); impl.cmdPool = VK_NULL_HANDLE; impl.cmdBuf = VK_NULL_HANDLE; }
        if (impl.sampler)  { vkDestroySampler(device, impl.sampler, nullptr); impl.sampler = VK_NULL_HANDLE; }
        if (impl.dsPool)   { vkDestroyDescriptorPool(device, impl.dsPool, nullptr); impl.dsPool = VK_NULL_HANDLE; impl.dsSet = VK_NULL_HANDLE; }
        if (impl.computePipeline) { vkDestroyPipeline(device, impl.computePipeline, nullptr); impl.computePipeline = VK_NULL_HANDLE; }
        if (impl.pipelineLayout)  { vkDestroyPipelineLayout(device, impl.pipelineLayout, nullptr); impl.pipelineLayout = VK_NULL_HANDLE; }
        if (impl.dsLayout)        { vkDestroyDescriptorSetLayout(device, impl.dsLayout, nullptr); impl.dsLayout = VK_NULL_HANDLE; }
        if (impl.computeShader)   { vkDestroyShaderModule(device, impl.computeShader, nullptr); impl.computeShader = VK_NULL_HANDLE; }
    }
    impl.boundStorageView = VK_NULL_HANDLE;
}

} // namespace

D3D11VulkanYuvCompositor::D3D11VulkanYuvCompositor()
    : m_impl(std::make_unique<Impl>()) {}

D3D11VulkanYuvCompositor::~D3D11VulkanYuvCompositor() { shutdown(); }

bool D3D11VulkanYuvCompositor::isInitialized() const
{
    return m_impl && m_impl->initialized;
}

bool D3D11VulkanYuvCompositor::initialize()
{
    if (m_impl->initialized) return true;

    auto &vk  = VulkanDeviceManager::instance();
    auto &d3d = D3D11DeviceManager::instance();
    if (!vk.isInitialized()) {
        qWarning("D3D11VulkanYuvCompositor: VulkanDeviceManager not initialized");
        return false;
    }
    if (!d3d.isInitialized()) {
        qWarning("D3D11VulkanYuvCompositor: D3D11DeviceManager not initialized");
        return false;
    }

    if (!buildComputePipeline(*m_impl)) {
        qWarning("D3D11VulkanYuvCompositor: compute pipeline init failed");
        teardownComputePipeline(*m_impl);
        return false;
    }

    // Best-effort timeline fence. If it fails, dispatch falls back to a
    // host-side fence wait per submit (matches the old bridge behavior).
    if (buildTimelineFence(*m_impl)) {
        qInfo("D3D11VulkanYuvCompositor: cross-API timeline fence ready "
              "(D3D11 Wait skips the CPU stall after Vulkan compute)");
    } else {
        qInfo("D3D11VulkanYuvCompositor: timeline fence unavailable — "
              "falling back to vkWaitForFences per dispatch");
    }

    m_impl->initialized = true;
    probeExternalMemoryCapabilities();
    return true;
}

void D3D11VulkanYuvCompositor::shutdown()
{
    if (!m_impl || !m_impl->initialized) {
        // Even uninitialized, tear down anything that partially built.
        if (m_impl) {
            teardownTimelineFence(*m_impl);
            teardownComputePipeline(*m_impl);
        }
        return;
    }
    // Drain any in-flight compute before destroying the pipeline.
    auto &vkMgr = qcv::VulkanDeviceManager::instance();
    VkDevice device = vkMgr.isInitialized() ? vkMgr.device() : VK_NULL_HANDLE;
    if (device != VK_NULL_HANDLE) vkMgr.waitForGpu();   // Phase I.D — queue-locked

    teardownTimelineFence(*m_impl);
    teardownComputePipeline(*m_impl);
    m_impl->initialized = false;
}

bool D3D11VulkanYuvCompositor::dispatch(const DispatchParams &params)
{
    auto &impl = *m_impl;
    if (!impl.initialized) return false;
    if (!params.outputImage || !params.outputView) return false;
    if (params.width <= 0 || params.height <= 0) return false;

    auto &vkMgr = qcv::VulkanDeviceManager::instance();
    VkDevice device = vkMgr.device();
    if (device == VK_NULL_HANDLE) return false;

    // Wait for the previous compute submission (regardless of which
    // bridge issued it) to complete before we mutate the shared
    // descriptor set / reset the shared cmd buf. Same protection the
    // old bridge had; works for both the timeline path and the
    // fallback path (fallback uses vkWaitForFences explicitly after
    // submit, which means by the time the next dispatch enters, the
    // cmdFence is already signaled and the buf is safe).
    if (impl.timelineReady && impl.timelineValue > 0) {
        VkSemaphoreWaitInfo wi{};
        wi.sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wi.semaphoreCount = 1;
        wi.pSemaphores    = &impl.vkSemaphore;
        wi.pValues        = &impl.timelineValue;
        vkWaitSemaphores(device, &wi, UINT64_MAX);
    }

    // Storage-image binding only needs to change when the bridge's
    // output view changed (new stream, realloc, or first dispatch).
    if (params.outputView != impl.boundStorageView) {
        VkDescriptorImageInfo dii{};
        dii.imageView   = params.outputView;
        dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = impl.dsSet;
        write.dstBinding      = 4;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.pImageInfo      = &dii;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        impl.boundStorageView = params.outputView;
    }

    // Sampler-binding writes — bindings 0..3 every dispatch. The
    // shader's `hasAlpha` push constant gates sampling of binding[3],
    // so a duplicate view there for 3-plane sources is fine.
    VkDescriptorImageInfo planeDii[4]{};
    VkWriteDescriptorSet  planeWrites[4]{};
    for (int i = 0; i < 4; ++i) {
        planeDii[i].sampler     = impl.sampler;
        planeDii[i].imageView   = params.samplerViews[i];
        planeDii[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        planeWrites[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        planeWrites[i].dstSet          = impl.dsSet;
        planeWrites[i].dstBinding      = static_cast<uint32_t>(i);
        planeWrites[i].descriptorCount = 1;
        planeWrites[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        planeWrites[i].pImageInfo      = &planeDii[i];
    }
    vkUpdateDescriptorSets(device, 4, planeWrites, 0, nullptr);

    // Record the dispatch.
    vkResetCommandBuffer(impl.cmdBuf, 0);
    VkCommandBufferBeginInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(impl.cmdBuf, &cbi);

    // UNDEFINED → GENERAL for the storage-image write. UNDEFINED is
    // valid as the prior layout because D3D11 owns the cross-API
    // resource between dispatches; Vulkan's view of its contents is
    // discardable each time.
    VkImageMemoryBarrier b0{};
    b0.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b0.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    b0.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    b0.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b0.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b0.image         = params.outputImage;
    b0.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b0.subresourceRange.levelCount = 1;
    b0.subresourceRange.layerCount = 1;
    b0.srcAccessMask = 0;
    b0.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(impl.cmdBuf,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          0, 0, nullptr, 0, nullptr, 1, &b0);

    vkCmdBindPipeline(impl.cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                       impl.computePipeline);
    vkCmdBindDescriptorSets(impl.cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                             impl.pipelineLayout, 0, 1, &impl.dsSet, 0, nullptr);

    ComputePushConstants pc{};
    pc.width      = static_cast<uint32_t>(params.width);
    pc.height     = static_cast<uint32_t>(params.height);
    pc.bitScale   = params.bitScale;
    pc.colorSpace = params.colorSpace;
    pc.range      = params.range;
    pc.hasAlpha   = params.hasAlpha;
    pc.isRgb      = params.isRgb;
    pc.isBiplanar = params.isBiplanar;
    vkCmdPushConstants(impl.cmdBuf, impl.pipelineLayout,
                        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    const uint32_t gx = (pc.width  + 7) / 8;
    const uint32_t gy = (pc.height + 7) / 8;
    vkCmdDispatch(impl.cmdBuf, gx, gy, 1);

    // GENERAL → SHADER_READ_ONLY_OPTIMAL for the D3D11-side sample.
    VkImageMemoryBarrier b1 = b0;
    b1.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    b1.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b1.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b1.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(impl.cmdBuf,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          0, 0, nullptr, 0, nullptr, 1, &b1);

    vkEndCommandBuffer(impl.cmdBuf);

    // Submit on the timeline path or fall back to host-wait. The
    // D3D11 GPU-side Wait on the timeline path means CPU returns
    // immediately and the next D3D11 sample blocks GPU-side instead.
    if (impl.timelineReady) {
        const uint64_t signalValue = ++impl.timelineValue;
        VkTimelineSemaphoreSubmitInfo tsi{};
        tsi.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        tsi.signalSemaphoreValueCount = 1;
        tsi.pSignalSemaphoreValues    = &signalValue;

        VkSubmitInfo si{};
        si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.pNext                = &tsi;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &impl.cmdBuf;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &impl.vkSemaphore;
        VkResult submitRes;
        {
            // Phase I.D — serialize with FFmpeg decode submits and any
            // wait-idle (see VulkanDeviceManager::queueMutex).
            std::lock_guard<std::recursive_mutex> qlock(vkMgr.queueMutex());
            submitRes = vkQueueSubmit(impl.computeQueue, 1, &si, VK_NULL_HANDLE);
        }
        if (submitRes != VK_SUCCESS) {
            qWarning("D3D11VulkanYuvCompositor: vkQueueSubmit (timeline) failed");
            return false;
        }
        impl.d3dContext4->Wait(impl.d3dFence.Get(), signalValue);
        return true;
    }

    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &impl.cmdBuf;
    vkResetFences(device, 1, &impl.cmdFence);
    VkResult fbSubmitRes;
    {
        // Phase I.D — same serialization as the timeline path.
        std::lock_guard<std::recursive_mutex> qlock(vkMgr.queueMutex());
        fbSubmitRes = vkQueueSubmit(impl.computeQueue, 1, &si, impl.cmdFence);
    }
    if (fbSubmitRes != VK_SUCCESS) {
        qWarning("D3D11VulkanYuvCompositor: vkQueueSubmit (fallback) failed");
        return false;
    }
    vkWaitForFences(device, 1, &impl.cmdFence, VK_TRUE, UINT64_MAX);
    return true;
}

void D3D11VulkanYuvCompositor::probeExternalMemoryCapabilities()
{
    auto &dm = qcv::VulkanDeviceManager::instance();
    if (!dm.isInitialized()) return;

    struct PFmt { VkFormat vk; const char *name; };
    static const PFmt kFormats[] = {
        { VK_FORMAT_R8G8B8A8_UNORM,           "RGBA8"     },
        { VK_FORMAT_R16G16B16A16_UNORM,       "RGBA16"    },
        { VK_FORMAT_R16G16B16A16_SFLOAT,      "RGBA16F"   },
        { VK_FORMAT_R16_UNORM,                "R16"       },
        { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, "NV12"      },
    };
    struct ProbeHandleType {
        VkExternalMemoryHandleTypeFlagBits flag;
        const char *name;
    };
    static const ProbeHandleType kHandles[] = {
        { VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,      "OPAQUE_WIN32"      },
        { VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT,  "OPAQUE_WIN32_KMT"  },
        { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,     "D3D11_TEXTURE"     },
        { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT, "D3D11_TEXTURE_KMT" },
    };

    // External-semaphore probe first — tells us whether the timeline
    // fence import we just attempted is even claimed-to-work.
    {
        struct ProbeSemHt {
            VkExternalSemaphoreHandleTypeFlagBits flag;
            const char *name;
        };
        static const ProbeSemHt kSemHandles[] = {
            { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT,     "OPAQUE_WIN32"     },
            { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT, "OPAQUE_WIN32_KMT" },
            { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT,      "D3D11_FENCE"      },
        };
        qInfo("D3D11VulkanYuvCompositor: external-semaphore capability probe:");
        for (const ProbeSemHt &h : kSemHandles) {
            VkPhysicalDeviceExternalSemaphoreInfo si{};
            si.sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
            si.handleType = h.flag;
            VkExternalSemaphoreProperties ep{};
            ep.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
            vkGetPhysicalDeviceExternalSemaphoreProperties(
                dm.physicalDevice(), &si, &ep);
            qInfo("  semaphore + %-18s -> feats=0x%x compat=0x%x export=0x%x",
                  h.name,
                  static_cast<unsigned>(ep.externalSemaphoreFeatures),
                  static_cast<unsigned>(ep.compatibleHandleTypes),
                  static_cast<unsigned>(ep.exportFromImportedHandleTypes));
        }
    }

    qInfo("D3D11VulkanYuvCompositor: external-memory capability probe:");
    constexpr size_t kNumFormats = sizeof(kFormats) / sizeof(kFormats[0]);
    constexpr size_t kNumHandles = sizeof(kHandles) / sizeof(kHandles[0]);
    for (size_t fi_idx = 0; fi_idx < kNumFormats; ++fi_idx) {
        for (size_t hi_idx = 0; hi_idx < kNumHandles; ++hi_idx) {
            const PFmt             &fmt    = kFormats[fi_idx];
            const ProbeHandleType  &handle = kHandles[hi_idx];

            VkPhysicalDeviceExternalImageFormatInfo extInfo{};
            extInfo.sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
            extInfo.handleType = handle.flag;

            VkPhysicalDeviceImageFormatInfo2 fi{};
            fi.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
            fi.pNext  = &extInfo;
            fi.format = fmt.vk;
            fi.type   = VK_IMAGE_TYPE_2D;
            fi.tiling = VK_IMAGE_TILING_OPTIMAL;
            fi.usage  = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            fi.flags  = 0;

            VkExternalImageFormatProperties extProps{};
            extProps.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
            VkImageFormatProperties2 props{};
            props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
            props.pNext = &extProps;

            VkResult r = vkGetPhysicalDeviceImageFormatProperties2(
                dm.physicalDevice(), &fi, &props);
            const auto feats = extProps.externalMemoryProperties.externalMemoryFeatures;
            qInfo("  %-8s + %-18s -> %s feats=0x%x compat=0x%x export=0x%x",
                  fmt.name, handle.name,
                  r == VK_SUCCESS ? "OK    " : "REJECT",
                  static_cast<unsigned>(feats),
                  static_cast<unsigned>(extProps.externalMemoryProperties.compatibleHandleTypes),
                  static_cast<unsigned>(extProps.externalMemoryProperties.exportFromImportedHandleTypes));
        }
    }
}

} // namespace qcv
