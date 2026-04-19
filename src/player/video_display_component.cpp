#include "video_display_component.h"
#include "direct_exr_cache.h"
#include "exr_transcoder.h"
#include "thumbnail_cache.h"
#include "image_loaders.h"
#include "../timeline/timeline_playback_controller.h"
#include "../timeline/timeline_cache.h"
#include "../color/ocio_pipeline.h"
#include "../utils/debug_utils.h"
#ifdef __linux__
#include "../utils/linux_clipboard.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

#include <GLFW/glfw3.h>
#include <imgui.h>
#ifdef QCVIEW_USE_VULKAN
#include "../gpu/vulkan_texture_pool.h"
#include "../gpu/vulkan_device.h"
#include "../color/vulkan_ocio_renderer.h"
#include <imgui_impl_vulkan.h>
#elif defined(QCVIEW_USE_METAL)
#include "../gpu/metal_texture_pool.h"
#include "../gpu/metal_device_manager.h"
#include "../color/metal_ocio_renderer.h"
#include "../utils/macos_clipboard.h"
#include "../hdr/hdr_color_utils.h"
#else
#include <backends/imgui_impl_opengl3.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include "../gpu/d3d11_device_manager.h"
#include "../color/d3d11_ocio_renderer.h"
#endif

// Include STB image write for PNG output (implementation)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../external/glfw/deps/stb_image_write.h"

// STB image read for clipboard screenshot path (header-only, no IMPLEMENTATION here)
#include "../../external/glfw/deps/stb_image.h"

// External functions from main.cpp
extern ImVec4 GetWindowsAccentColor();

// Pipeline mode configurations - now defined here (was in video_player.cpp)
const std::map<PipelineMode, PipelineConfig> PIPELINE_CONFIGS = {
    {PipelineMode::NORMAL, {
        PipelineMode::NORMAL,
        GL_RGBA8,
        GL_UNSIGNED_BYTE,
        false,  // linear_processing
        false,  // constrain_primaries
        4,      // bytes_per_pixel
        "Standard 8-bit (Best Performance)",
        512, 2048  // recommended_cache_mb, max_cache_mb
    }},
    {PipelineMode::HIGH_RES, {
        PipelineMode::HIGH_RES,
        GL_RGBA16,
        GL_UNSIGNED_SHORT,
        true,   // linear_processing
        false,  // constrain_primaries
        8,      // bytes_per_pixel
        "12-bit OCIO Optimized",
        1024, 4096
    }},
    {PipelineMode::ULTRA_HIGH_RES, {
        PipelineMode::ULTRA_HIGH_RES,
        GL_RGBA16F,
        GL_HALF_FLOAT,
        true,   // linear_processing
        false,  // constrain_primaries
        8,      // bytes_per_pixel
        "16-bit Float (Maximum Precision)",
        1024, 4096
    }},
    {PipelineMode::HDR_RES, {
        PipelineMode::HDR_RES,
        GL_RGBA16F,
        GL_HALF_FLOAT,
        true,   // linear_processing
        false,  // constrain_primaries (no hardcoded colorspace)
        8,      // bytes_per_pixel
        "HDR Passthrough (PQ/BT.2020)",
        1024, 4096
    }},
    {PipelineMode::MF_HDR, {
        PipelineMode::MF_HDR,
        GL_RGBA16F,
        GL_HALF_FLOAT,
        true,   // linear_processing
        false,  // constrain_primaries
        8,      // bytes_per_pixel
        "D3D11VA HDR (Experimental)",
        1024, 4096
    }}
};

// Helper function to convert pipeline mode to string
const char* PipelineModeToString(PipelineMode mode) {
    switch (mode) {
        case PipelineMode::NORMAL: return "Normal";
        case PipelineMode::HIGH_RES: return "High-Res";
        case PipelineMode::ULTRA_HIGH_RES: return "Ultra-High-Res";
        case PipelineMode::HDR_RES: return "HDR";
        case PipelineMode::MF_HDR: return "MF-HDR";
        default: return "Unknown";
    }
}

// Global configuration accessors
extern qcview::DirectEXRCacheConfig GetCurrentEXRCacheConfig();
extern qcview::ThumbnailConfig GetCurrentThumbnailConfig();

//=============================================================================
// Constructor / Destructor
//=============================================================================

VideoDisplayComponent::VideoDisplayComponent() {
    // Initialize SVG renderer so dropdown is available
    svg_overlay_renderer_ = std::make_unique<SVGOverlayRenderer>();
    Debug::Log("VideoDisplayComponent: SVG overlay renderer initialized");

    // Pre-create DirectEXRCache so I/O threads are always running
    exr_cache_ = std::make_shared<qcview::DirectEXRCache>();
    Debug::Log("VideoDisplayComponent: DirectEXRCache pre-created");
}

VideoDisplayComponent::~VideoDisplayComponent() {
    Cleanup();
}

//=============================================================================
// Cross-platform Texture ID
//=============================================================================

ImTextureID VideoDisplayComponent::GetDisplayTextureID() const {
#ifdef _WIN32
    if (use_d3d11_rendering_ && color_srv_d3d_) {
        return reinterpret_cast<ImTextureID>(color_srv_d3d_.Get());
    }
#endif

#ifdef QCVIEW_USE_VULKAN
    // Vulkan path: prefer color-processed texture if available
    if (vulkan_color_tex_.valid && vulkan_color_tex_.descriptor_set != VK_NULL_HANDLE) {
        return reinterpret_cast<ImTextureID>(vulkan_color_tex_.descriptor_set);
    }
    // Fallback: raw video texture from pool
    GLuint tex = video_texture_;
    if (tex == 0) return ImTextureID{};
    return qcview::VulkanTexturePool::Instance().GetImTextureID(static_cast<uint64_t>(tex));
#elif defined(QCVIEW_USE_METAL)
    // Prefer color-processed texture if available (non-owning MTLTexture pointer).
    if (metal_color_mtl_texture_) {
        return (ImTextureID)(intptr_t)metal_color_mtl_texture_;
    }
    GLuint tex = video_texture_;
    if (tex == 0) return ImTextureID{};
    return qcview::MetalTexturePool::Instance().GetImTextureID(static_cast<uint64_t>(tex));
#else
    return (ImTextureID)(intptr_t)color_texture_;
#endif
}

//=============================================================================
// Core Lifecycle
//=============================================================================

bool VideoDisplayComponent::Initialize() {
    Debug::Log("VideoDisplayComponent::Initialize starting...");

    // Create transition placeholder texture
    CreateTransitionPlaceholder();

#ifdef QCVIEW_USE_VULKAN
    // Initialize Vulkan OCIO renderer (compiles shaders via shaderc)
    vulkan_ocio_renderer_ = std::make_unique<qcview::VulkanOCIORenderer>();
    if (vulkan_ocio_renderer_->Initialize()) {
        Debug::Log("VideoDisplayComponent: Vulkan OCIO renderer initialized");
    } else {
        Debug::Log("VideoDisplayComponent: WARNING - Failed to initialize Vulkan OCIO renderer");
        vulkan_ocio_renderer_.reset();
    }
#elif defined(QCVIEW_USE_METAL)
    // Initialize Metal OCIO renderer
    metal_ocio_renderer_ = std::make_unique<qcview::MetalOCIORenderer>();
    if (metal_ocio_renderer_->Initialize()) {
        Debug::Log("VideoDisplayComponent: Metal OCIO renderer initialized");
    } else {
        Debug::Log("VideoDisplayComponent: WARNING - Failed to initialize Metal OCIO renderer");
        metal_ocio_renderer_.reset();
    }
#else
    // Create default passthrough color pipeline (provides stable buffering during transitions)
    auto passthrough = std::make_unique<OCIOPipeline>();
    if (passthrough->CreatePassthroughPipeline()) {
        color_pipeline_ = std::move(passthrough);
        Debug::Log("VideoDisplayComponent: Default passthrough color pipeline created");

        // Create quad VAO/VBO for color pipeline rendering
        if (quad_vao_ == 0) {
            float quad_vertices[] = {
                // positions   // texCoords
                -1.0f,  1.0f,  0.0f, 1.0f,  // top-left
                -1.0f, -1.0f,  0.0f, 0.0f,  // bottom-left
                 1.0f, -1.0f,  1.0f, 0.0f,  // bottom-right
                -1.0f,  1.0f,  0.0f, 1.0f,  // top-left
                 1.0f, -1.0f,  1.0f, 0.0f,  // bottom-right
                 1.0f,  1.0f,  1.0f, 1.0f   // top-right
            };

            glGenVertexArrays(1, &quad_vao_);
            glGenBuffers(1, &quad_vbo_);

            glBindVertexArray(quad_vao_);
            glBindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);

            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

            glBindVertexArray(0);
            Debug::Log("VideoDisplayComponent: Created quad VAO/VBO for color pipeline");
        }

        // Create initial color processing resources at placeholder dimensions
        CreateColorProcessingResourcesForMode(transition_placeholder_width_, transition_placeholder_height_, current_pipeline_mode_);
        ClearColorTextureToBackground();  // Clear FBO to prevent garbage display
    } else {
        Debug::Log("VideoDisplayComponent: WARNING - Failed to create default passthrough pipeline");
    }
#endif

    Debug::Log("VideoDisplayComponent::Initialize complete");
    return true;
}

void VideoDisplayComponent::Cleanup() {
    Debug::Log("VideoDisplayComponent::Cleanup starting...");

    // Reset content dimensions
    content_width_ = 0;
    content_height_ = 0;
    use_content_dimensions_ = false;

    // Shutdown EXR cache
    if (exr_cache_) {
        exr_cache_->Shutdown();
        exr_cache_.reset();
        Debug::Log("VideoDisplayComponent: EXR cache shut down");
    }

    // Clean up thumbnail cache
    if (thumbnail_cache_) {
        thumbnail_cache_.reset();
        Debug::Log("VideoDisplayComponent: Thumbnail cache cleaned up");
    }

#ifdef QCVIEW_USE_VULKAN
    // Clean up Vulkan OCIO renderer and color textures
    DestroyVulkanColorTexture();
    DestroyVulkanDualColorTexture();
    if (vulkan_ocio_renderer_) {
        vulkan_ocio_renderer_->Shutdown();
        vulkan_ocio_renderer_.reset();
    }

    // Vulkan path: Queue texture deletions via VulkanTexturePool
    {
        auto& pool = qcview::VulkanTexturePool::Instance();
        if (video_texture_ && video_texture_ != transition_placeholder_texture_) {
            pool.QueueDelete(static_cast<uint64_t>(video_texture_));
            video_texture_ = 0;
        }
        if (transition_placeholder_texture_) {
            pool.QueueDelete(static_cast<uint64_t>(transition_placeholder_texture_));
            transition_placeholder_texture_ = 0;
        }
        if (gap_placeholder_texture_) {
            pool.QueueDelete(static_cast<uint64_t>(gap_placeholder_texture_));
            gap_placeholder_texture_ = 0;
        }
        pool.ProcessPendingDeletions();
    }
#elif defined(QCVIEW_USE_METAL)
    // Clean up Metal OCIO renderer. The color-processed texture was never
    // pool-owned under the current design — it lives on metal_ocio_renderer_
    // and is released by its Shutdown().
    metal_color_mtl_texture_ = nullptr;
    metal_last_src_pool_id_ = 0;
    if (metal_ocio_renderer_) {
        metal_ocio_renderer_->Shutdown();
        metal_ocio_renderer_.reset();
    }

    // Metal path: Queue texture deletions via MetalTexturePool
    {
        auto& pool = qcview::MetalTexturePool::Instance();
        if (video_texture_ && video_texture_ != transition_placeholder_texture_) {
            pool.QueueDelete(static_cast<uint64_t>(video_texture_));
            video_texture_ = 0;
        }
        if (transition_placeholder_texture_) {
            pool.QueueDelete(static_cast<uint64_t>(transition_placeholder_texture_));
            transition_placeholder_texture_ = 0;
        }
        if (gap_placeholder_texture_) {
            pool.QueueDelete(static_cast<uint64_t>(gap_placeholder_texture_));
            gap_placeholder_texture_ = 0;
        }
        pool.ProcessPendingDeletions();
    }
#else
    // Delete OpenGL textures
    if (video_texture_ && video_texture_ != transition_placeholder_texture_) {
        glDeleteTextures(1, &video_texture_);
        video_texture_ = 0;
    }

    if (transition_placeholder_texture_) {
        glDeleteTextures(1, &transition_placeholder_texture_);
        transition_placeholder_texture_ = 0;
    }

    if (gap_placeholder_texture_) {
        glDeleteTextures(1, &gap_placeholder_texture_);
        gap_placeholder_texture_ = 0;
    }

    // Delete framebuffers and GL resources
    if (fbo_) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }

    if (color_texture_) {
        glDeleteTextures(1, &color_texture_);
        color_texture_ = 0;
    }

    if (color_fbo_) {
        glDeleteFramebuffers(1, &color_fbo_);
        color_fbo_ = 0;
    }

    if (quad_vao_) {
        glDeleteVertexArrays(1, &quad_vao_);
        quad_vao_ = 0;
    }

    if (quad_vbo_) {
        glDeleteBuffers(1, &quad_vbo_);
        quad_vbo_ = 0;
    }
#endif

    // Clear color pipeline
    color_pipeline_.reset();

#ifdef _WIN32
    // Cleanup D3D11 resources
    CleanupD3D11Resources();
#endif

#ifdef __linux__
    qcview::CleanupClipboardResources();
#endif

    Debug::Log("VideoDisplayComponent::Cleanup complete");
}

//=============================================================================
// Texture Management
//=============================================================================

void VideoDisplayComponent::CreateVideoTextures(int width, int height) {
    CreateVideoTexturesForMode(width, height, current_pipeline_mode_);
}

void VideoDisplayComponent::CreateVideoTexturesForMode(int width, int height, PipelineMode mode) {
    if (width <= 0 || height <= 0) {
        return;
    }

    auto it = PIPELINE_CONFIGS.find(mode);
    if (it == PIPELINE_CONFIGS.end()) {
        Debug::Log("CreateVideoTexturesForMode: Unknown pipeline mode");
        return;
    }

    const PipelineConfig& config = it->second;

#ifdef QCVIEW_USE_VULKAN
    // Vulkan path: VulkanTexturePool manages textures on-demand.
    // We just track dimensions here. Actual textures are created per-frame
    // by DirectEXRCache/TimelineCache via VulkanTexturePool::CreateTextureFromPixels().
    video_width_ = width;
    video_height_ = height;
    current_internal_format_ = config.internal_format;
    Debug::Log("Vulkan: Set video dimensions: " + std::to_string(width) + "x" + std::to_string(height));
#elif defined(QCVIEW_USE_METAL)
    // Metal path: MetalTexturePool manages textures on-demand (same as Vulkan)
    video_width_ = width;
    video_height_ = height;
    current_internal_format_ = config.internal_format;
    Debug::Log("Metal: Set video dimensions: " + std::to_string(width) + "x" + std::to_string(height));
#else
    // OpenGL path
    // Store old texture to delete AFTER new one is created
    GLuint old_video_texture = video_texture_;
    GLuint old_fbo = fbo_;

    // Clear existing texture through FBO during recreation to prevent flicker
    ClearVideoTextureToBackground();

    // Create new OpenGL texture with pipeline-specific format
    GLuint new_texture = 0;
    glGenTextures(1, &new_texture);
    glBindTexture(GL_TEXTURE_2D, new_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, config.internal_format, width, height,
        0, GL_RGBA, config.data_type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Create new FBO for final output
    GLuint new_fbo = 0;
    glGenFramebuffers(1, &new_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, new_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, new_texture, 0);

    // Check FBO completeness
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        Debug::Log("ERROR: Video FBO incomplete! Status: " + std::to_string(status));
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Assign new resources and clean up old ones
    video_texture_ = new_texture;
    fbo_ = new_fbo;
    video_width_ = width;
    video_height_ = height;
    current_internal_format_ = config.internal_format;

    // Clean up old resources AFTER new ones are assigned
    if (old_video_texture && old_video_texture != transition_placeholder_texture_) {
        glDeleteTextures(1, &old_video_texture);
    }
    if (old_fbo) {
        glDeleteFramebuffers(1, &old_fbo);
    }

    Debug::Log("Created video textures: " + std::to_string(width) + "x" + std::to_string(height));
#endif
}

void VideoDisplayComponent::CreateColorProcessingResourcesForMode(int width, int height, PipelineMode mode) {
#if defined(QCVIEW_USE_VULKAN) || defined(QCVIEW_USE_METAL)
    // Vulkan/Metal color processing is handled dynamically by GPU OCIO renderer
    (void)width; (void)height; (void)mode;
    return;
#else
    if (width <= 0 || height <= 0) {
        return;
    }

    auto it = PIPELINE_CONFIGS.find(mode);
    if (it == PIPELINE_CONFIGS.end()) {
        return;
    }

    const PipelineConfig& config = it->second;

    // Clean up existing color processing resources
    if (color_texture_) {
        glDeleteTextures(1, &color_texture_);
        color_texture_ = 0;
        color_texture_width_ = 0;
        color_texture_height_ = 0;
    }
    if (color_fbo_) {
        glDeleteFramebuffers(1, &color_fbo_);
        color_fbo_ = 0;
    }

    // Create FBO for color processing
    glGenFramebuffers(1, &color_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, color_fbo_);

    // Create color texture with pipeline-specific format
    glGenTextures(1, &color_texture_);
    glBindTexture(GL_TEXTURE_2D, color_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, config.internal_format, width, height,
        0, GL_RGBA, config.data_type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Track color texture dimensions
    color_texture_width_ = width;
    color_texture_height_ = height;

    // Attach to FBO
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, color_texture_, 0);

    // Check FBO completeness
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        Debug::Log("ERROR: Color FBO incomplete! Status: " + std::to_string(status));
        color_texture_width_ = 0;
        color_texture_height_ = 0;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif
}

void VideoDisplayComponent::CreateTransitionPlaceholder() {
#ifdef QCVIEW_USE_VULKAN
    // Vulkan path: Create placeholder via VulkanTexturePool
    if (transition_placeholder_texture_ != 0) {
        qcview::VulkanTexturePool::Instance().QueueDelete(static_cast<uint64_t>(transition_placeholder_texture_));
        qcview::VulkanTexturePool::Instance().ProcessPendingDeletions();
        transition_placeholder_texture_ = 0;
    }

    const int size = 64;
    std::vector<unsigned char> pixels(size * size * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = 27; pixels[i + 1] = 27; pixels[i + 2] = 27; pixels[i + 3] = 255;
    }

    uint64_t pool_id = qcview::VulkanTexturePool::Instance().CreateTextureFromPixels(
        size, size, VK_FORMAT_R8G8B8A8_UNORM, pixels.data(), pixels.size());
    transition_placeholder_texture_ = static_cast<GLuint>(pool_id);
    transition_placeholder_width_ = size;
    transition_placeholder_height_ = size;
    Debug::Log("VideoDisplayComponent: Created Vulkan transition placeholder");
#elif defined(QCVIEW_USE_METAL)
    // Metal path: Create placeholder via MetalTexturePool
    if (transition_placeholder_texture_ != 0) {
        qcview::MetalTexturePool::Instance().QueueDelete(static_cast<uint64_t>(transition_placeholder_texture_));
        qcview::MetalTexturePool::Instance().ProcessPendingDeletions();
        transition_placeholder_texture_ = 0;
    }

    const int size = 64;
    std::vector<unsigned char> pixels(size * size * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = 27; pixels[i + 1] = 27; pixels[i + 2] = 27; pixels[i + 3] = 255;
    }

    uint64_t pool_id = qcview::MetalTexturePool::Instance().CreateTextureFromPixels(
        size, size, 0 /* RGBA8 */, pixels.data(), pixels.size());
    transition_placeholder_texture_ = static_cast<GLuint>(pool_id);
    transition_placeholder_width_ = size;
    transition_placeholder_height_ = size;
    Debug::Log("VideoDisplayComponent: Created Metal transition placeholder");
#else
    // Delete existing if any
    if (transition_placeholder_texture_ != 0) {
        glDeleteTextures(1, &transition_placeholder_texture_);
        transition_placeholder_texture_ = 0;
    }

    // Create a small dark gray texture (64x64) matching background color
    const int size = 64;
    glGenTextures(1, &transition_placeholder_texture_);
    glBindTexture(GL_TEXTURE_2D, transition_placeholder_texture_);

    // Dark gray pixels matching background (#1b1b1b = RGB 27,27,27)
    std::vector<unsigned char> pixels(size * size * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = 27;      // R
        pixels[i + 1] = 27;  // G
        pixels[i + 2] = 27;  // B
        pixels[i + 3] = 255; // A (opaque)
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    transition_placeholder_width_ = size;
    transition_placeholder_height_ = size;

    Debug::Log("VideoDisplayComponent: Created transition placeholder texture");
#endif
}

void VideoDisplayComponent::ClearVideoTextureToBackground() {
#if defined(QCVIEW_USE_VULKAN) || defined(QCVIEW_USE_METAL)
    // No-op on Vulkan/Metal — textures are managed by pool
    return;
#else
    if (fbo_ == 0 || video_texture_ == 0) {
        return;
    }

    GLint current_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current_fbo);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glClearColor(27.0f/255.0f, 27.0f/255.0f, 27.0f/255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
#endif
}

void VideoDisplayComponent::ClearColorTextureToBackground() {
#if defined(QCVIEW_USE_VULKAN) || defined(QCVIEW_USE_METAL)
    // No-op on Vulkan/Metal — color processing handled by GPU renderer
    return;
#else
    if (color_fbo_ == 0 || color_texture_ == 0) {
        return;
    }

    GLint current_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current_fbo);

    glBindFramebuffer(GL_FRAMEBUFFER, color_fbo_);
    glClearColor(27.0f/255.0f, 27.0f/255.0f, 27.0f/255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
#endif
}

//=============================================================================
// Rendering
//=============================================================================

void VideoDisplayComponent::ProcessPendingTextureUploads() {
#if defined(QCVIEW_USE_METAL) || defined(QCVIEW_USE_VULKAN)
    // Metal/Vulkan: UpdatePlayhead now runs from TimelinePlaybackController on main thread.
    // Process EXR texture deletions on main thread.
    if (exr_cache_) {
        exr_cache_->ProcessReadyTextures();
    }

    // Process pending timeline thumbnail uploads
    if (is_timeline_mode_ && timeline_controller_ && timeline_controller_->IsInitialized()) {
        timeline_controller_->ProcessPendingUploads();
    }

    if (has_video_ || (is_timeline_mode_ && timeline_controller_)) {
        UpdateVideoTexture();
    }
#else
    // OpenGL: everything on main thread (GL context is single-threaded)
    if (exr_cache_) {
        exr_cache_->ProcessReadyTextures();
    }

    if (is_timeline_mode_ && timeline_controller_ && timeline_controller_->IsInitialized()) {
        auto* cache = timeline_controller_->GetCache();
        if (cache) {
            double pos = GetPosition();
            int frame = static_cast<int>(std::round(pos * timeline_controller_->GetFPS()));
            if (frame < 0) frame = 0;
            cache->UpdatePlayhead(frame, IsPlaying());
        }
        timeline_controller_->ProcessPendingUploads();
    }

    if (has_video_ || (is_timeline_mode_ && timeline_controller_)) {
        UpdateVideoTexture();
    }
#endif
}

void VideoDisplayComponent::RenderVideoFrame() {
    // NOTE: Texture uploads and UpdateVideoTexture() moved to ProcessPendingTextureUploads()
    // which is called BEFORE ImGui::NewFrame() to avoid GL state corruption

    // Render based on current state (pure ImGui calls only - no GL operations!)
    if (has_video_ && video_texture_) {
        RenderVideoTexture();
    } else {
        RenderPlaceholder();
    }
}

void VideoDisplayComponent::UpdateVideoTexture() {
    // In timeline mode, inject frame from cache (D3D11VideoDecoder provides frames)
    if (is_timeline_mode_ && timeline_controller_) {
        auto t0 = std::chrono::steady_clock::now();
        InjectCurrentTimelineFrame();
        auto t1 = std::chrono::steady_clock::now();
        double inject_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

#ifdef QCVIEW_USE_VULKAN
        if (vulkan_ocio_renderer_ && video_texture_ != 0) {
            ApplyColorPipelineVulkan();
        }
#elif defined(QCVIEW_USE_METAL)
        if (metal_ocio_renderer_ && video_texture_ != 0) {
            ApplyColorPipelineMetal();
        }
#else
        if (color_pipeline_ && color_pipeline_->IsValid() && video_texture_ != 0) {
            ApplyColorPipeline();
        }
#endif
        auto t2 = std::chrono::steady_clock::now();
        double ocio_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        (void)inject_ms; (void)ocio_ms; // inject timing logging disabled
        return;
    }

#ifdef QCVIEW_USE_VULKAN
    // Apply OCIO via Vulkan: video_texture_ (pool ID) → vulkan_color_tex_
    if (vulkan_ocio_renderer_ && video_texture_ != 0) {
        ApplyColorPipelineVulkan();
    }
#elif defined(QCVIEW_USE_METAL)
    // Apply OCIO via Metal: video_texture_ (pool ID) → metal_color_mtl_texture_
    if (metal_ocio_renderer_ && video_texture_ != 0) {
        ApplyColorPipelineMetal();
    }
#else
    // Apply OCIO color pipeline (GL path)
    // Transforms video_texture_ → color_texture_
    if (color_pipeline_ && color_pipeline_->IsValid() && video_texture_ != 0) {
        ApplyColorPipeline();
    }
#endif
}

void VideoDisplayComponent::RenderVideoTexture() {
    if (video_width_ <= 0 || video_height_ <= 0) {
        return;
    }

    float aspect_ratio = (float)video_width_ / (float)video_height_;
    ImVec2 content_region = ImGui::GetContentRegionAvail();

    ImVec2 image_size;
    if (content_region.x / content_region.y > aspect_ratio) {
        image_size.y = content_region.y;
        image_size.x = content_region.y * aspect_ratio;
    } else {
        image_size.x = content_region.x;
        image_size.y = content_region.x / aspect_ratio;
    }

    // Center the image
    ImVec2 cursor_pos = ImGui::GetCursorPos();
    ImVec2 offset = ImVec2(
        (content_region.x - image_size.x) * 0.5f,
        (content_region.y - image_size.y) * 0.5f
    );
    ImGui::SetCursorPos(ImVec2(cursor_pos.x + offset.x, cursor_pos.y + offset.y));

    // Store the display area for annotation overlay alignment
    // Use GetCursorScreenPos() AFTER setting cursor to get exact screen coordinates
    last_display_pos_ = ImGui::GetCursorScreenPos();
    last_display_size_ = image_size;

    // Choose which texture to display
    GLuint display_texture = video_texture_;

#ifdef QCVIEW_USE_VULKAN
    // Vulkan path: prefer color-processed texture, fallback to raw pool texture
    if (display_texture == 0) {
        return;
    }

    ImTextureID im_tex_id;
    if (vulkan_color_tex_.valid && vulkan_color_tex_.descriptor_set != VK_NULL_HANDLE) {
        im_tex_id = reinterpret_cast<ImTextureID>(vulkan_color_tex_.descriptor_set);
    } else {
        im_tex_id = qcview::VulkanTexturePool::Instance().GetImTextureID(
            static_cast<uint64_t>(display_texture));
    }
    if (im_tex_id == ImTextureID{}) {
        return;
    }

    // Mark video texture as HDR passthrough (skip sRGB->PQ conversion in ImGui shader)
    // Video content is already color-processed by OCIO pipeline
    ImGui_ImplVulkan_SetTextureHDRPassthrough(im_tex_id, true);

    ImGui::Image(im_tex_id, image_size);
#elif defined(QCVIEW_USE_METAL)
    // Metal path: prefer color-processed MTLTexture (owned by OCIO renderer or
    // aliased into the pool), fallback to raw pool texture.
    if (display_texture == 0) return;

    ImTextureID im_tex_id = (ImTextureID)0;
    if (metal_color_mtl_texture_) {
        im_tex_id = (ImTextureID)(intptr_t)metal_color_mtl_texture_;
    } else {
        im_tex_id = qcview::MetalTexturePool::Instance().GetImTextureID(
            static_cast<uint64_t>(display_texture));
    }
    if (im_tex_id == (ImTextureID)0) {
        // Fallback to transition placeholder
        if (transition_placeholder_texture_ != 0) {
            im_tex_id = qcview::MetalTexturePool::Instance().GetImTextureID(
                static_cast<uint64_t>(transition_placeholder_texture_));
        }
        if (im_tex_id == (ImTextureID)0) return;
    }

    ImGui::Image(im_tex_id, image_size);
#else
    // OpenGL path
    // Use color-corrected texture if OCIO pipeline is active
    if (color_pipeline_ && color_pipeline_->IsValid()) {
        if (color_texture_ > 0 && glIsTexture(color_texture_)) {
            display_texture = color_texture_;
        }
    }

    // Safety check - make sure we have a valid texture
    if (display_texture == 0 || !glIsTexture(display_texture)) {
        if (video_texture_ != 0 && video_texture_ != display_texture && glIsTexture(video_texture_)) {
            display_texture = video_texture_;
        } else if (transition_placeholder_texture_ != 0 && glIsTexture(transition_placeholder_texture_)) {
            display_texture = transition_placeholder_texture_;
        } else {
            return;
        }
    }

    // Mark video texture as HDR passthrough (skip sRGB->PQ conversion)
    // Video content is already in HDR format from OCIO pipeline
#if !defined(QCVIEW_USE_VULKAN) && !defined(QCVIEW_USE_METAL)
    ImGui_ImplOpenGL3_SetTextureHDRPassthrough((ImTextureID)(intptr_t)display_texture, true);
#endif

    // Display the texture
    ImGui::Image((void*)(intptr_t)display_texture, image_size);
#endif
}

void VideoDisplayComponent::RenderPlaceholder() {
    ImVec2 content_region = ImGui::GetContentRegionAvail();

    // Create invisible button covering the entire content region for drop target
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##PlaceholderDropTarget", content_region);

    // Check if we're being dragged over
    bool is_drag_hovering = ImGui::BeginDragDropTarget();

    if (is_drag_hovering) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEM")) {
            std::string media_id((const char*)payload->Data, payload->DataSize - 1);
            Debug::Log("Viewport drop received (placeholder): " + media_id);
            // Note: In the full implementation, this would trigger media loading
        }
        ImGui::EndDragDropTarget();
    }

    // Draw subtle highlight when dragging over
    if (is_drag_hovering) {
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 window_pos = ImGui::GetWindowPos();
        ImVec4 accent = GetWindowsAccentColor();
        ImU32 highlight_color = IM_COL32(
            (int)(accent.x * 255),
            (int)(accent.y * 255),
            (int)(accent.z * 255),
            30
        );
        draw_list->AddRectFilled(
            window_pos,
            ImVec2(window_pos.x + content_region.x, window_pos.y + content_region.y),
            highlight_color
        );
    }
}

//=============================================================================
// OCIO Color Pipeline
//=============================================================================

void VideoDisplayComponent::SetColorPipeline(std::unique_ptr<OCIOPipeline> pipeline) {
    // In HDR_RES mode, ignore non-passthrough pipeline changes
    // HDR content must pass through unchanged to the HDR10 swapchain
    if (current_pipeline_mode_ == PipelineMode::HDR_RES) {
        if (pipeline && !pipeline->IsPassthrough()) {
            Debug::Log("SetColorPipeline: Ignoring non-passthrough pipeline in HDR_RES mode (HDR passthrough requires identity transform)");
            return;
        }
    }

    // Clear any existing pipeline first
    if (color_pipeline_) {
        color_pipeline_.reset();

#ifdef QCVIEW_USE_VULKAN
        // Invalidate cached Vulkan OCIO pipeline so it rebuilds with new shader
        if (vulkan_ocio_renderer_) {
            vulkan_ocio_renderer_->InvalidateOCIOPipeline();
        }
        vulkan_last_src_pool_id_ = 0;  // Force re-apply on current frame
#elif defined(QCVIEW_USE_METAL)
        // Invalidate cached Metal OCIO pipeline so it rebuilds with new shader
        if (metal_ocio_renderer_) {
            metal_ocio_renderer_->InvalidateCache();
        }
        metal_last_src_pool_id_ = 0;  // Force re-apply on current frame
#else
        // Force OpenGL state cleanup
        glUseProgram(0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_3D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
#endif
    }

    color_pipeline_ = std::move(pipeline);

    if (color_pipeline_ && color_pipeline_->IsValid()) {
        if (has_video_ && video_width_ > 0 && video_height_ > 0) {
            SetupColorProcessingResources();
        }
    }
}

void VideoDisplayComponent::ClearColorPipeline() {
    Debug::Log("ClearColorPipeline: Replacing with passthrough pipeline");

#ifdef QCVIEW_USE_VULKAN
    if (vulkan_ocio_renderer_) {
        vulkan_ocio_renderer_->InvalidateOCIOPipeline();
    }
    vulkan_last_src_pool_id_ = 0;  // Force re-apply on current frame
#elif defined(QCVIEW_USE_METAL)
    // Invalidate cached Metal OCIO pipeline
    if (metal_ocio_renderer_) {
        metal_ocio_renderer_->InvalidateCache();
    }
    metal_last_src_pool_id_ = 0;  // Force re-apply on current frame
#else
    // Clean up OpenGL state first
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
#endif

#if defined(QCVIEW_USE_VULKAN) || defined(QCVIEW_USE_METAL)
    // On Vulkan/Metal, just clear the pipeline — GPU OCIO renderer handles passthrough
    color_pipeline_.reset();
#else
    // Create passthrough pipeline
    auto passthrough = std::make_unique<OCIOPipeline>();
    if (passthrough->CreatePassthroughPipeline()) {
        color_pipeline_ = std::move(passthrough);

        if (has_video_ && video_width_ > 0 && video_height_ > 0) {
            SetupColorProcessingResources();
        }
    } else {
        color_pipeline_.reset();
    }
#endif
}

bool VideoDisplayComponent::HasColorPipeline() const {
    return color_pipeline_ && color_pipeline_->IsValid();
}

bool VideoDisplayComponent::HasActiveColorTransform() const {
    return HasColorPipeline() && !color_pipeline_->IsPassthrough();
}

void VideoDisplayComponent::SetupColorProcessingResources() {
#if defined(QCVIEW_USE_VULKAN) || defined(QCVIEW_USE_METAL)
    return;  // Vulkan/Metal color processing handled by GPU renderer
#endif
    int target_width = (use_content_dimensions_ && content_width_ > 0) ? content_width_ : video_width_;
    int target_height = (use_content_dimensions_ && content_height_ > 0) ? content_height_ : video_height_;

    if (target_width <= 0 || target_height <= 0) {
        return;
    }

    CreateColorProcessingResourcesForMode(target_width, target_height, current_pipeline_mode_);
    ClearColorTextureToBackground();  // Clear FBO to prevent garbage display

    // Create fullscreen quad if not already created
    if (quad_vao_ == 0) {
        float quad_vertices[] = {
            -1.0f,  1.0f,  0.0f, 1.0f,
            -1.0f, -1.0f,  0.0f, 0.0f,
             1.0f, -1.0f,  1.0f, 0.0f,
            -1.0f,  1.0f,  0.0f, 1.0f,
             1.0f, -1.0f,  1.0f, 0.0f,
             1.0f,  1.0f,  1.0f, 1.0f
        };

        glGenVertexArrays(1, &quad_vao_);
        glGenBuffers(1, &quad_vbo_);

        glBindVertexArray(quad_vao_);
        glBindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

        glBindVertexArray(0);
    }
}

void VideoDisplayComponent::ApplyColorPipeline() {
#if defined(QCVIEW_USE_VULKAN) || defined(QCVIEW_USE_METAL)
    return;  // Vulkan/Metal use their own GPU OCIO renderers
#endif
    if (!color_pipeline_ || !color_pipeline_->IsValid()) {
        return;
    }

    if (color_fbo_ == 0 || color_texture_ == 0) {
        SetupColorProcessingResources();
        if (color_fbo_ == 0 || color_texture_ == 0) {
            return;
        }
    }

    // Save state
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    GLint current_fbo;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);
    GLint current_program;
    glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
    GLint current_vao;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &current_vao);

    // Determine target render dimensions
    int target_width = (use_content_dimensions_ && content_width_ > 0) ? content_width_ : video_width_;
    int target_height = (use_content_dimensions_ && content_height_ > 0) ? content_height_ : video_height_;

    if (target_width <= 0 || target_height <= 0) {
        return;
    }

    // Check if color resources need to be recreated
    if (color_texture_width_ != target_width || color_texture_height_ != target_height) {
        CreateColorProcessingResourcesForMode(target_width, target_height, current_pipeline_mode_);
    }

    // Bind color FBO
    glBindFramebuffer(GL_FRAMEBUFFER, color_fbo_);
    glViewport(0, 0, color_texture_width_, color_texture_height_);

    // Clear to background color
    glClearColor(27.0f/255.0f, 27.0f/255.0f, 27.0f/255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Use OCIO shader
    GLuint shader_program = color_pipeline_->GetShaderProgram();
    glUseProgram(shader_program);

    // Bind input texture
    glActiveTexture(GL_TEXTURE0);
    if (video_texture_ != 0) {
        glBindTexture(GL_TEXTURE_2D, video_texture_);
    } else {
        return;
    }

    // Bind LUT textures if needed
    const auto& lut_ids = color_pipeline_->GetLUTTextureIDs();
    const auto& lut_dims = color_pipeline_->GetLUTTextureDimensions();
    if (!lut_ids.empty()) {
        for (size_t i = 0; i < lut_ids.size(); ++i) {
            int texture_unit = 1 + static_cast<int>(i);
            glActiveTexture(GL_TEXTURE0 + texture_unit);
            if (i < lut_dims.size()) {
                if (lut_dims[i] == 1) {
                    glBindTexture(GL_TEXTURE_1D, lut_ids[i]);
                } else if (lut_dims[i] == 2) {
                    glBindTexture(GL_TEXTURE_2D, lut_ids[i]);
                } else {
                    glBindTexture(GL_TEXTURE_3D, lut_ids[i]);
                }
            } else {
                glBindTexture(GL_TEXTURE_3D, lut_ids[i]);
            }
        }
    }

    // Set uniforms
    color_pipeline_->UpdateUniforms(0, 1);

    // Set debugMode to 1 to enable actual OCIO processing
    // (debugMode == 0 would just show raw input without color correction)
    GLint debug_loc = glGetUniformLocation(shader_program, "debugMode");
    if (debug_loc >= 0) {
        glUniform1i(debug_loc, 1);  // 1 = OCIO processing enabled
    }

    // Render fullscreen quad
    glBindVertexArray(quad_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Restore state - restore VAO to saved value (critical for ImGui)
    glBindVertexArray(current_vao);
    glUseProgram(current_program);
    glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);

    // Clean up texture bindings - unbind correct texture type based on dimension
    for (size_t i = 0; i < lut_ids.size(); ++i) {
        glActiveTexture(GL_TEXTURE0 + 1 + static_cast<int>(i));
        if (i < lut_dims.size()) {
            if (lut_dims[i] == 1) {
                glBindTexture(GL_TEXTURE_1D, 0);
            } else if (lut_dims[i] == 2) {
                glBindTexture(GL_TEXTURE_2D, 0);
            } else {
                glBindTexture(GL_TEXTURE_3D, 0);
            }
        } else {
            glBindTexture(GL_TEXTURE_3D, 0);
        }
    }
    glActiveTexture(GL_TEXTURE0);
}

GLuint VideoDisplayComponent::CreateColorCorrectedTexture(GLuint input_texture_id, int tex_width, int tex_height,
                                                          int output_width, int output_height) {
#if defined(QCVIEW_USE_VULKAN) || defined(QCVIEW_USE_METAL)
    (void)input_texture_id; (void)tex_width; (void)tex_height;
    (void)output_width; (void)output_height;
    return 0;  // Color correction handled by GPU OCIO renderer
#endif
    if (!color_pipeline_ || !color_pipeline_->IsValid() || quad_vao_ == 0) {
        return 0;
    }

    // Create output texture
    GLuint output_texture = 0;
    glGenTextures(1, &output_texture);
    glBindTexture(GL_TEXTURE_2D, output_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, output_width, output_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Create temporary framebuffer
    GLuint temp_fbo = 0;
    glGenFramebuffers(1, &temp_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, temp_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output_texture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &temp_fbo);
        glDeleteTextures(1, &output_texture);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return 0;
    }

    // Save current OpenGL state
    GLint current_fbo, current_program, current_viewport[4], current_vao;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
    glGetIntegerv(GL_VIEWPORT, current_viewport);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &current_vao);

    // Render
    glViewport(0, 0, output_width, output_height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    GLuint shader_program = color_pipeline_->GetShaderProgram();
    glUseProgram(shader_program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, input_texture_id);

    const auto& lut_ids = color_pipeline_->GetLUTTextureIDs();
    const auto& lut_dims = color_pipeline_->GetLUTTextureDimensions();
    for (size_t i = 0; i < lut_ids.size(); ++i) {
        int texture_unit = 1 + static_cast<int>(i);
        glActiveTexture(GL_TEXTURE0 + texture_unit);
        if (i < lut_dims.size()) {
            if (lut_dims[i] == 1) {
                glBindTexture(GL_TEXTURE_1D, lut_ids[i]);
            } else if (lut_dims[i] == 2) {
                glBindTexture(GL_TEXTURE_2D, lut_ids[i]);
            } else {
                glBindTexture(GL_TEXTURE_3D, lut_ids[i]);
            }
        } else {
            glBindTexture(GL_TEXTURE_3D, lut_ids[i]);
        }
    }

    color_pipeline_->UpdateUniforms(0, 1);

    // Set debugMode to 1 to enable actual OCIO processing
    GLint debug_loc = glGetUniformLocation(shader_program, "debugMode");
    if (debug_loc >= 0) {
        glUniform1i(debug_loc, 1);
    }

    glBindVertexArray(quad_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Restore state - restore VAO to saved value (critical for ImGui)
    glBindVertexArray(current_vao);
    glUseProgram(current_program);
    glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
    glViewport(current_viewport[0], current_viewport[1], current_viewport[2], current_viewport[3]);

    // Clean up LUT texture bindings - unbind correct texture type based on dimension
    for (size_t i = 0; i < lut_ids.size(); ++i) {
        glActiveTexture(GL_TEXTURE0 + 1 + static_cast<int>(i));
        if (i < lut_dims.size()) {
            if (lut_dims[i] == 1) {
                glBindTexture(GL_TEXTURE_1D, 0);
            } else if (lut_dims[i] == 2) {
                glBindTexture(GL_TEXTURE_2D, 0);
            } else {
                glBindTexture(GL_TEXTURE_3D, 0);
            }
        } else {
            glBindTexture(GL_TEXTURE_3D, 0);
        }
    }
    glActiveTexture(GL_TEXTURE0);

    // Cleanup temporary FBO
    glDeleteFramebuffers(1, &temp_fbo);

    return output_texture;
}

void VideoDisplayComponent::ForceFrameRefresh() {
    if (!has_video_ || video_width_ <= 0 || video_height_ <= 0) {
        return;
    }

    // Force color pipeline to re-render
    if (color_pipeline_ && color_pipeline_->IsValid()) {
        ApplyColorPipeline();
    }
}

//=============================================================================
// Overlay Rendering
//=============================================================================

void VideoDisplayComponent::RenderSVGOverlays(ImDrawList* draw_list, ImVec2 video_pos, ImVec2 video_size,
                                              float opacity, ImU32 color, float line_width) {
    if (!svg_overlays_enabled_ || !svg_overlay_renderer_ || !svg_overlay_renderer_->IsLoaded()) {
        return;
    }

    svg_overlay_renderer_->RenderOverlay(draw_list, video_pos, video_size, opacity, color, line_width);
}

void VideoDisplayComponent::SetSafetyOverlaySettings(const SafetyGuideSettings& settings) {
    safety_settings_ = settings;
}

//=============================================================================
// EXR/Image Sequence Cache
//=============================================================================

void VideoDisplayComponent::InitializeEXRCache(const std::vector<std::string>& sequence_files,
                                               const std::string& layer_name, double fps,
                                               double initial_position) {
    Debug::Log("VideoDisplayComponent::InitializeEXRCache - " + std::to_string(sequence_files.size()) +
               " files, layer: " + layer_name);

    is_exr_mode_ = true;
    exr_sequence_files_ = sequence_files;
    exr_layer_name_ = layer_name;
    exr_frame_rate_ = fps;

    if (!exr_cache_) {
        exr_cache_ = std::make_shared<qcview::DirectEXRCache>();
    }

    double cache_start_position = initial_position >= 0.0 ? initial_position : 0.0;

    auto exr_loader = std::make_unique<qcview::EXRImageLoader>();
    if (exr_cache_->Initialize(std::move(exr_loader), sequence_files, layer_name, fps,
                               PipelineMode::HDR_RES, exr_sequence_start_frame_, cache_start_position)) {
        qcview::DirectEXRCacheConfig config = GetCurrentEXRCacheConfig();
        exr_cache_->SetConfig(config);
        exr_cache_->SetLooping(loop_enabled_);
        exr_cache_->StartBackgroundCaching();

        // Update pipeline mode so GetPipelineMode() returns the correct mode for menu display
        current_pipeline_mode_ = PipelineMode::HDR_RES;

        Debug::Log("VideoDisplayComponent: DirectEXRCache initialized with HDR_RES pipeline");
    } else {
        Debug::Log("VideoDisplayComponent: ERROR - Failed to initialize DirectEXRCache");
        exr_cache_.reset();
    }
}

void VideoDisplayComponent::SetEXRCacheConfig(const qcview::DirectEXRCacheConfig& config) {
    if (exr_cache_) {
        exr_cache_->SetConfig(config);
    }
}

void VideoDisplayComponent::ClearEXRCache() {
    if (exr_cache_) {
        exr_cache_->Shutdown();
        Debug::Log("VideoDisplayComponent: EXR cache shut down");
    }
    is_exr_mode_ = false;
}

std::vector<qcview::CacheSegment> VideoDisplayComponent::GetEXRCacheSegments() const {
    if (exr_cache_) {
        return exr_cache_->GetCacheSegments();
    }
    return {};
}

bool VideoDisplayComponent::HasEXRCache() const {
    return exr_cache_ && exr_cache_->IsInitialized();
}

GLuint VideoDisplayComponent::GetThumbnailForFrame(int frame, bool allow_fallback, int* out_actual_frame) {
    if (thumbnail_cache_) {
        return thumbnail_cache_->GetThumbnail(frame, allow_fallback, out_actual_frame);
    }
    return 0;
}

bool VideoDisplayComponent::HasThumbnailCache() const {
    return thumbnail_cache_ != nullptr;
}

void VideoDisplayComponent::ClearThumbnailCache() {
    if (thumbnail_cache_) {
        thumbnail_cache_->ClearCache();
    }
}

//=============================================================================
// Timeline Integration
//=============================================================================

void VideoDisplayComponent::SetTimelineMode(bool enabled, qcview::TimelinePlaybackController* controller) {
    is_timeline_mode_ = enabled;
    timeline_controller_ = controller;

    // Get timeline dimensions from controller if available
    int target_width = transition_placeholder_width_;
    int target_height = transition_placeholder_height_;
    bool has_timeline_content = false;
    if (enabled && controller && controller->IsInitialized()) {
        int timeline_width = controller->GetWidth();
        int timeline_height = controller->GetHeight();
        if (timeline_width > 0 && timeline_height > 0) {
            target_width = timeline_width;
            target_height = timeline_height;
            SetContentDimensions(timeline_width, timeline_height);
            has_timeline_content = true;
        }
    }

    if (enabled && has_timeline_content) {
        // Only set up video display if timeline has actual content
        video_texture_ = transition_placeholder_texture_;
        video_width_ = target_width;
        video_height_ = target_height;
        has_video_ = true;

        ClearColorTextureToBackground();

        // Reset timeline texture tracking
        timeline_texture_ = 0;
        timeline_texture_width_ = 0;
        timeline_texture_height_ = 0;
        last_timeline_frame_ = -1;

        // Ensure video FBO resources exist
        if (fbo_ == 0 || video_texture_ == transition_placeholder_texture_) {
            CreateVideoTexturesForMode(target_width, target_height, current_pipeline_mode_);
        }
    } else if (!enabled) {
        // Disabling timeline mode - clear video state
        has_video_ = false;
        video_texture_ = 0;
        timeline_texture_ = 0;
        timeline_texture_width_ = 0;
        timeline_texture_height_ = 0;
        last_timeline_frame_ = -1;
    }
    // If enabled but no content, don't set has_video_ = true (show empty viewport)
}

void VideoDisplayComponent::InjectCurrentTimelineFrame() {
    if (!is_timeline_mode_ || !timeline_controller_) {
        return;
    }

    if (!timeline_controller_->IsInitialized()) {
        return;
    }

    // NOTE: ProcessPendingUploads moved to ProcessPendingTextureUploads()
    // which is called BEFORE ImGui::NewFrame()

#ifdef _WIN32
    // D3D11 path - get SRV directly from cache
    if (use_d3d11_rendering_ && timeline_controller_->IsD3D11RenderingMode()) {
        int frame_width = 0, frame_height = 0;
        ID3D11ShaderResourceView* srv = timeline_controller_->UpdateD3D11(frame_width, frame_height);

        if (srv != nullptr) {
            // Store the SRV for later use in ApplyColorPipelineD3D11
            timeline_srv_d3d_ = srv;
            timeline_texture_width_ = frame_width;
            timeline_texture_height_ = frame_height;

            video_width_ = frame_width;
            video_height_ = frame_height;
            has_video_ = true;

            // Apply OCIO color transform via D3D11
            ApplyColorPipelineD3D11();

            int current_frame = timeline_controller_->GetCurrentFrame();
            if (current_frame != last_timeline_frame_) {
                last_timeline_frame_ = current_frame;
            }
        } else {
            // Cache miss - keep previous frame
            if (timeline_srv_d3d_ != nullptr) {
                has_video_ = true;
            } else {
                has_video_ = false;
            }
        }
        return;
    }
#endif

    // OpenGL/Vulkan path — get frame from timeline controller
    int frame_width = 0, frame_height = 0;
    GLuint frame_texture = timeline_controller_->Update(frame_width, frame_height);

    if (frame_texture != 0) {
        timeline_texture_ = frame_texture;
        timeline_texture_width_ = frame_width;
        timeline_texture_height_ = frame_height;

        video_texture_ = frame_texture;
        video_width_ = frame_width;
        video_height_ = frame_height;
        has_video_ = true;

        int current_frame = timeline_controller_->GetCurrentFrame();
        if (current_frame != last_timeline_frame_) {
            last_timeline_frame_ = current_frame;
        }
    } else {
        // Cache miss - use previous frame or placeholder
        if (timeline_texture_ != 0) {
            video_texture_ = timeline_texture_;
            video_width_ = timeline_texture_width_;
            video_height_ = timeline_texture_height_;
            has_video_ = true;
        } else {
            int placeholder_w = (use_content_dimensions_ && content_width_ > 0) ? content_width_ : 1920;
            int placeholder_h = (use_content_dimensions_ && content_height_ > 0) ? content_height_ : 1080;

#ifdef QCVIEW_USE_VULKAN
            // Vulkan path: Create gap placeholder via VulkanTexturePool
            if (gap_placeholder_texture_ == 0 ||
                gap_placeholder_width_ != placeholder_w ||
                gap_placeholder_height_ != placeholder_h) {
                auto& pool = qcview::VulkanTexturePool::Instance();
                if (gap_placeholder_texture_ != 0) {
                    pool.QueueDelete(static_cast<uint64_t>(gap_placeholder_texture_));
                    pool.ProcessPendingDeletions();
                }

                std::vector<unsigned char> black_pixels(placeholder_w * placeholder_h * 4, 0);
                for (size_t i = 3; i < black_pixels.size(); i += 4) {
                    black_pixels[i] = 255;
                }

                uint64_t pool_id = pool.CreateTextureFromPixels(
                    placeholder_w, placeholder_h,
                    VK_FORMAT_R8G8B8A8_UNORM,
                    black_pixels.data(),
                    black_pixels.size()
                );
                gap_placeholder_texture_ = static_cast<GLuint>(pool_id);
                gap_placeholder_width_ = placeholder_w;
                gap_placeholder_height_ = placeholder_h;
            }
#elif defined(QCVIEW_USE_METAL)
            // Metal path: Create gap placeholder via MetalTexturePool
            if (gap_placeholder_texture_ == 0 ||
                gap_placeholder_width_ != placeholder_w ||
                gap_placeholder_height_ != placeholder_h) {
                auto& pool = qcview::MetalTexturePool::Instance();
                if (gap_placeholder_texture_ != 0) {
                    pool.QueueDelete(static_cast<uint64_t>(gap_placeholder_texture_));
                    pool.ProcessPendingDeletions();
                }

                std::vector<unsigned char> black_pixels(placeholder_w * placeholder_h * 4, 0);
                for (size_t i = 3; i < black_pixels.size(); i += 4) {
                    black_pixels[i] = 255;
                }

                uint64_t pool_id = pool.CreateTextureFromPixels(
                    placeholder_w, placeholder_h, 0 /* RGBA8 */,
                    black_pixels.data(), black_pixels.size());
                gap_placeholder_texture_ = static_cast<GLuint>(pool_id);
                gap_placeholder_width_ = placeholder_w;
                gap_placeholder_height_ = placeholder_h;
            }
#else
            // OpenGL path
            if (gap_placeholder_texture_ == 0 ||
                gap_placeholder_width_ != placeholder_w ||
                gap_placeholder_height_ != placeholder_h) {
                if (gap_placeholder_texture_ != 0) {
                    glDeleteTextures(1, &gap_placeholder_texture_);
                }

                std::vector<unsigned char> black_pixels(placeholder_w * placeholder_h * 4, 0);
                for (size_t i = 3; i < black_pixels.size(); i += 4) {
                    black_pixels[i] = 255;
                }

                // Save current texture binding to preserve GL state
                GLint previous_texture = 0;
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);

                glGenTextures(1, &gap_placeholder_texture_);
                glBindTexture(GL_TEXTURE_2D, gap_placeholder_texture_);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, placeholder_w, placeholder_h, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, black_pixels.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

                // Restore previous texture binding
                glBindTexture(GL_TEXTURE_2D, previous_texture);

                gap_placeholder_width_ = placeholder_w;
                gap_placeholder_height_ = placeholder_h;
            }
#endif

            video_texture_ = gap_placeholder_texture_;
            video_width_ = placeholder_w;
            video_height_ = placeholder_h;
            has_video_ = true;
        }
    }
}

void VideoDisplayComponent::SetContentDimensions(int width, int height) {
    content_width_ = width;
    content_height_ = height;
    use_content_dimensions_ = (width > 0 && height > 0);
}

//=============================================================================
// Playback Control (Passthrough to TimelinePlaybackController)
//=============================================================================

void VideoDisplayComponent::Play() {
    if (timeline_controller_) {
        timeline_controller_->Play();
    }
}

void VideoDisplayComponent::Pause() {
    if (timeline_controller_) {
        timeline_controller_->Pause();
    }
}

void VideoDisplayComponent::Seek(double position) {
    if (timeline_controller_) {
        timeline_controller_->Seek(position);
    }
}

void VideoDisplayComponent::StepFrame(int direction) {
    if (timeline_controller_) {
        if (direction > 0) {
            timeline_controller_->StepForward(direction);
        } else {
            timeline_controller_->StepBackward(-direction);
        }
    }
}

void VideoDisplayComponent::GoToStart() {
    if (timeline_controller_) {
        timeline_controller_->GoToStart();
    }
}

void VideoDisplayComponent::GoToEnd() {
    if (timeline_controller_) {
        timeline_controller_->GoToEnd();
    }
}

bool VideoDisplayComponent::IsPlaying() const {
    if (timeline_controller_) {
        return timeline_controller_->IsPlaying();
    }
    return false;
}

bool VideoDisplayComponent::IsActuallyPlaying() const {
    if (timeline_controller_) {
        return timeline_controller_->IsActuallyPlaying();
    }
    return false;
}

//=============================================================================
// Media Info (Cached Values)
//=============================================================================

double VideoDisplayComponent::GetPosition() const {
    if (timeline_controller_) {
        return timeline_controller_->GetPosition();
    }
    return cached_position_;
}

double VideoDisplayComponent::GetDuration() const {
    if (timeline_controller_) {
        return timeline_controller_->GetDuration();
    }
    return cached_duration_;
}

double VideoDisplayComponent::GetFrameRate() const {
    if (timeline_controller_) {
        return timeline_controller_->GetFPS();
    }
    if (is_exr_mode_) {
        return exr_frame_rate_;
    }
    return cached_fps_;
}

int VideoDisplayComponent::GetTotalFrames() const {
    double fps = GetFrameRate();
    double duration = GetDuration();
    if (fps > 0 && duration > 0) {
        return static_cast<int>(std::round(duration * fps));
    }
    return 0;
}

int VideoDisplayComponent::GetCurrentFrame() const {
    if (timeline_controller_) {
        return timeline_controller_->GetCurrentFrame();
    }
    double fps = GetFrameRate();
    double pos = GetPosition();
    if (fps > 0) {
        return static_cast<int>(std::round(pos * fps));
    }
    return 0;
}

std::string VideoDisplayComponent::GetFilePath() const {
    if (timeline_controller_) {
        return timeline_controller_->GetSourceFilePath();
    }
    return "";
}

void VideoDisplayComponent::SetLoop(bool enabled) {
    loop_enabled_ = enabled;
    if (timeline_controller_) {
        timeline_controller_->SetLooping(enabled);
    }
    if (exr_cache_) {
        exr_cache_->SetLooping(enabled);
    }
}

//=============================================================================
// Screenshot Capture
//=============================================================================

bool VideoDisplayComponent::CaptureScreenshotToClipboard() {
#ifdef QCVIEW_USE_VULKAN
    if (!HasValidTexture()) {
        Debug::Log("Screenshot (Vulkan): No valid texture");
        return false;
    }

    // Save to temp file (CaptureScreenshotToPath applies OCIO)
    std::string tmp_name = "qcview_clipboard_" + std::to_string(getpid()) + ".png";
    std::string tmp_path = "/tmp/" + tmp_name;
    if (!CaptureScreenshotToPath("/tmp", tmp_name)) {
        Debug::Log("Screenshot (Vulkan): Failed to save temp PNG for clipboard");
        return false;
    }

    // Read the PNG file into memory
    std::vector<uint8_t> png_data;
    {
        std::ifstream file(tmp_path, std::ios::binary | std::ios::ate);
        if (file.is_open()) {
            auto size = file.tellg();
            file.seekg(0);
            png_data.resize(static_cast<size_t>(size));
            file.read(reinterpret_cast<char*>(png_data.data()), size);
        }
    }
    std::remove(tmp_path.c_str());

    if (png_data.empty()) {
        Debug::Log("Screenshot (Vulkan): Failed to read temp PNG");
        return false;
    }

    // Copy to clipboard using native Wayland/X11 protocol
    bool success = qcview::CopyImageToClipboard(png_data);

    if (success) {
        Debug::Log("Screenshot copied to clipboard (" +
                   std::to_string(video_width_) + "x" + std::to_string(video_height_) + ")");
    } else {
        Debug::Log("Screenshot (Vulkan): Native clipboard copy failed");
    }
    return success;
#elif defined(QCVIEW_USE_METAL)
    // Metal: save to temp file, read PNG, copy to clipboard
    if (!HasValidTexture()) {
        Debug::Log("Screenshot (Metal): No valid texture for clipboard");
        return false;
    }

    std::string tmp_name = "qcview_clipboard_tmp.png";
    std::string tmp_path = "/tmp/" + tmp_name;
    if (!CaptureScreenshotToPath("/tmp", tmp_name)) {
        Debug::Log("Screenshot (Metal): Failed to save temp PNG for clipboard");
        return false;
    }

    // Read the PNG file into memory
    std::vector<uint8_t> png_data;
    {
        std::ifstream file(tmp_path, std::ios::binary | std::ios::ate);
        if (file.is_open()) {
            auto size = file.tellg();
            file.seekg(0);
            png_data.resize(static_cast<size_t>(size));
            file.read(reinterpret_cast<char*>(png_data.data()), size);
        }
    }
    std::remove(tmp_path.c_str());

    if (png_data.empty()) {
        Debug::Log("Screenshot (Metal): Failed to read temp PNG");
        return false;
    }

    bool success = CopyImageToClipboard(png_data);
    if (success) {
        Debug::Log("Screenshot copied to clipboard (" +
                   std::to_string(video_width_) + "x" + std::to_string(video_height_) + ")");
    } else {
        Debug::Log("Screenshot (Metal): Clipboard copy failed");
    }
    return success;
#else
    // Windows/GL: save to temp file (includes annotations via CaptureScreenshotToPath),
    // then load and copy to clipboard
    if (!HasValidTexture()) {
        Debug::Log("Screenshot failed: No valid video texture");
        return false;
    }

#ifdef _WIN32
    // Save to temp file
    char temp_path[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_path);
    std::string tmp_dir(temp_path);
    std::string tmp_name = "qcview_clipboard_tmp.png";
    std::string tmp_full = tmp_dir + tmp_name;

    if (!CaptureScreenshotToPath(tmp_dir, tmp_name)) {
        Debug::Log("Screenshot: Failed to save temp PNG for clipboard");
        return false;
    }

    // Read pixels from the temp PNG
    int img_w, img_h, img_channels;
    unsigned char* img_data = stbi_load(tmp_full.c_str(), &img_w, &img_h, &img_channels, 4);
    std::remove(tmp_full.c_str());

    if (!img_data) {
        Debug::Log("Screenshot: Failed to read temp PNG");
        return false;
    }

    bool success = false;
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();

        size_t pixel_size = static_cast<size_t>(img_w) * img_h * 4;

        // BGRA copy for CF_DIB
        std::vector<unsigned char> bgra_pixels(pixel_size);
        memcpy(bgra_pixels.data(), img_data, pixel_size);
        for (size_t i = 0; i < pixel_size; i += 4) {
            std::swap(bgra_pixels[i], bgra_pixels[i + 2]);
        }

        BITMAPINFOHEADER bi = {};
        bi.biSize = sizeof(BITMAPINFOHEADER);
        bi.biWidth = img_w;
        bi.biHeight = -img_h;
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;

        HGLOBAL hDIB = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixel_size);
        if (hDIB) {
            unsigned char* pDIB = (unsigned char*)GlobalLock(hDIB);
            if (pDIB) {
                memcpy(pDIB, &bi, sizeof(BITMAPINFOHEADER));
                memcpy(pDIB + sizeof(BITMAPINFOHEADER), bgra_pixels.data(), pixel_size);
                GlobalUnlock(hDIB);
                SetClipboardData(CF_DIB, hDIB);
            }
        }

        // PNG format
        static UINT CF_PNG = RegisterClipboardFormatA("PNG");
        if (CF_PNG) {
            auto png_write_func = [](void* context, void* data, int size) {
                std::vector<unsigned char>* buffer = (std::vector<unsigned char>*)context;
                unsigned char* bytes = (unsigned char*)data;
                buffer->insert(buffer->end(), bytes, bytes + size);
            };

            std::vector<unsigned char> png_buffer;
            stbi_write_png_to_func(png_write_func, &png_buffer, img_w, img_h,
                                   4, img_data, img_w * 4);

            if (!png_buffer.empty()) {
                HGLOBAL hPNG = GlobalAlloc(GMEM_MOVEABLE, png_buffer.size());
                if (hPNG) {
                    unsigned char* pPNG = (unsigned char*)GlobalLock(hPNG);
                    if (pPNG) {
                        memcpy(pPNG, png_buffer.data(), png_buffer.size());
                        GlobalUnlock(hPNG);
                        SetClipboardData(CF_PNG, hPNG);
                    }
                }
            }
        }

        CloseClipboard();
        success = true;
    }

    free(img_data);

    if (success) {
        Debug::Log("Screenshot captured to clipboard (" + std::to_string(img_w) + "x" +
                   std::to_string(img_h) + ")");
    }
    return success;
#else
    return false;
#endif // _WIN32
#endif // QCVIEW_USE_VULKAN
}

bool VideoDisplayComponent::CaptureScreenshotToDesktop(const std::string& filename) {
    if (!HasValidTexture()) {
        return false;
    }

    std::string output_filename = filename;
    if (output_filename.empty()) {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto tm = *std::localtime(&time_t);

        char timestamp[64];
        std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &tm);

        std::string base_filename = "ump_Screenshot";
        if (is_timeline_mode_ && timeline_controller_) {
            std::string timeline_name = timeline_controller_->GetTimelineName();
            if (!timeline_name.empty()) {
                base_filename = timeline_name;
            }
        }

#ifdef _WIN32
        char desktop_path[MAX_PATH];
        if (SHGetSpecialFolderPathA(nullptr, desktop_path, CSIDL_DESKTOP, FALSE)) {
            output_filename = std::string(desktop_path) + "\\" + base_filename + "_" + timestamp + ".png";
        } else {
            output_filename = base_filename + "_" + std::string(timestamp) + ".png";
        }
#else
        // XDG Desktop or ~/Desktop fallback
        std::string desktop_dir;
        const char* xdg_desktop = std::getenv("XDG_DESKTOP_DIR");
        if (xdg_desktop) {
            desktop_dir = xdg_desktop;
        } else {
            const char* home = std::getenv("HOME");
            if (home) {
                desktop_dir = std::string(home) + "/Desktop";
                if (!std::filesystem::exists(desktop_dir)) {
                    desktop_dir = std::string(home);  // Fall back to home dir
                }
            }
        }
        if (!desktop_dir.empty()) {
            output_filename = desktop_dir + "/" + base_filename + "_" + timestamp + ".png";
        } else {
            output_filename = base_filename + "_" + std::string(timestamp) + ".png";
        }
#endif
    }

    return CaptureScreenshotToPath(std::filesystem::path(output_filename).parent_path().string(),
                                   std::filesystem::path(output_filename).filename().string());
}

bool VideoDisplayComponent::CaptureScreenshotToPath(const std::string& directory_path, const std::string& filename) {
#ifdef QCVIEW_USE_VULKAN
    if (!HasValidTexture()) {
        Debug::Log("Screenshot (Vulkan): No valid texture");
        return false;
    }

    auto& dev = qcview::VulkanDeviceManager::Instance();
    auto& pool = qcview::VulkanTexturePool::Instance();
    VkDevice device = dev.GetDevice();

    uint64_t src_pool_id = static_cast<uint64_t>(video_texture_);

    // Apply OCIO color correction if active
    uint64_t color_pool_id = 0;
    if (color_pipeline_ && color_pipeline_->IsValid() && !color_pipeline_->IsPassthrough()) {
        color_pool_id = CreateColorCorrectedPoolTexture(src_pool_id, video_width_, video_height_);
        if (color_pool_id != 0) {
            src_pool_id = color_pool_id;
        }
    }

    // Get the (possibly color-corrected) texture from pool
    const auto* src_tex = pool.GetTexture(src_pool_id);
    if (!src_tex || !src_tex->is_valid) {
        Debug::Log("Screenshot (Vulkan): Pool texture not found");
        if (color_pool_id) pool.QueueDelete(color_pool_id);
        return false;
    }

    int w = src_tex->width;
    int h = src_tex->height;
    size_t buffer_size = static_cast<size_t>(w) * h * 4;

    // Build output path
    std::string output_filename = directory_path;
    if (!output_filename.empty() && output_filename.back() != '/') {
        output_filename += "/";
    }
    output_filename += filename;

    // Check if we need to composite annotations
    std::vector<qcview::Annotations::ActiveStroke> strokes;
    if (get_annotation_strokes_callback_) {
        strokes = get_annotation_strokes_callback_();
    }

    bool success = false;

    if (!strokes.empty() && vulkan_render_annotations_to_image_callback_) {
        // --- Annotation compositing path: blit to intermediate image, render annotations, readback ---

        // Create offscreen RGBA8 export image
        VkImage export_image = VK_NULL_HANDLE;
        VmaAllocation export_alloc = VK_NULL_HANDLE;
        VkImageView export_view = VK_NULL_HANDLE;

        VkImageCreateInfo img_info{};
        img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_info.imageType = VK_IMAGE_TYPE_2D;
        img_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        img_info.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        img_info.mipLevels = 1;
        img_info.arrayLayers = 1;
        img_info.samples = VK_SAMPLE_COUNT_1_BIT;
        img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        img_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        if (vmaCreateImage(dev.GetAllocator(), &img_info, &alloc_ci,
                           &export_image, &export_alloc, nullptr) != VK_SUCCESS) {
            Debug::Log("Screenshot (Vulkan): Failed to create annotation export image");
            if (color_pool_id) pool.QueueDelete(color_pool_id);
            return false;
        }

        VkImageViewCreateInfo view_ci{};
        view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image = export_image;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_ci.subresourceRange.levelCount = 1;
        view_ci.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device, &view_ci, nullptr, &export_view) != VK_SUCCESS) {
            vmaDestroyImage(dev.GetAllocator(), export_image, export_alloc);
            if (color_pool_id) pool.QueueDelete(color_pool_id);
            return false;
        }

        // Blit source texture onto export image
        {
            VkCommandBuffer cmd = dev.BeginSingleTimeCommands();

            // Transition export image to TRANSFER_DST
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = export_image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);

            // Transition source to TRANSFER_SRC
            VkImageMemoryBarrier src_barrier{};
            src_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            src_barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            src_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            src_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            src_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            src_barrier.image = src_tex->image;
            src_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            src_barrier.subresourceRange.levelCount = 1;
            src_barrier.subresourceRange.layerCount = 1;
            src_barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            src_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &src_barrier);

            // Blit (handles format conversion and scaling)
            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.layerCount = 1;
            blit.srcOffsets[1] = {src_tex->width, src_tex->height, 1};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.layerCount = 1;
            blit.dstOffsets[1] = {w, h, 1};

            vkCmdBlitImage(cmd, src_tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           export_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, VK_FILTER_LINEAR);

            // Transition source back to SHADER_READ_ONLY
            src_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            src_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            src_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            src_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &src_barrier);

            // Transition export image to COLOR_ATTACHMENT_OPTIMAL for annotation rendering
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);

            dev.EndSingleTimeCommands(cmd);
        }

        // Render annotations on top
        vulkan_render_annotations_to_image_callback_(strokes, export_image, export_view, w, h);

        // Readback: copy export image to staging buffer
        VkBuffer staging_buffer = VK_NULL_HANDLE;
        VmaAllocation staging_alloc = VK_NULL_HANDLE;

        VkBufferCreateInfo buf_ci{};
        buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_ci.size = buffer_size;
        buf_ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo staging_ai{};
        staging_ai.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
        staging_ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo staging_info{};
        if (vmaCreateBuffer(dev.GetAllocator(), &buf_ci, &staging_ai,
                            &staging_buffer, &staging_alloc, &staging_info) != VK_SUCCESS) {
            vkDestroyImageView(device, export_view, nullptr);
            vmaDestroyImage(dev.GetAllocator(), export_image, export_alloc);
            if (color_pool_id) pool.QueueDelete(color_pool_id);
            return false;
        }

        {
            VkCommandBuffer cmd = dev.BeginSingleTimeCommands();

            // Transition export image to TRANSFER_SRC
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = export_image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);

            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};

            vkCmdCopyImageToBuffer(cmd, export_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   staging_buffer, 1, &region);

            dev.EndSingleTimeCommands(cmd);
        }

        // Write PNG from export image (always RGBA8)
        if (staging_info.pMappedData) {
            success = stbi_write_png(output_filename.c_str(), w, h, 4,
                                     staging_info.pMappedData, w * 4) != 0;
        }

        // Cleanup
        vmaDestroyBuffer(dev.GetAllocator(), staging_buffer, staging_alloc);
        vkDestroyImageView(device, export_view, nullptr);
        vmaDestroyImage(dev.GetAllocator(), export_image, export_alloc);

    } else {
        // --- Direct path (no annotations): copy source texture straight to staging ---

        VkBuffer staging_buffer = VK_NULL_HANDLE;
        VmaAllocation staging_alloc = VK_NULL_HANDLE;

        VkBufferCreateInfo buf_ci{};
        buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_ci.size = buffer_size;
        buf_ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo staging_ai{};
        staging_ai.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
        staging_ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo staging_info{};
        if (vmaCreateBuffer(dev.GetAllocator(), &buf_ci, &staging_ai,
                            &staging_buffer, &staging_alloc, &staging_info) != VK_SUCCESS) {
            Debug::Log("Screenshot (Vulkan): Failed to create staging buffer");
            if (color_pool_id) pool.QueueDelete(color_pool_id);
            return false;
        }

        // Copy image to staging buffer
        {
            VkCommandBuffer cmd = dev.BeginSingleTimeCommands();

            // Transition to TRANSFER_SRC
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = src_tex->image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);

            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};

            vkCmdCopyImageToBuffer(cmd, src_tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   staging_buffer, 1, &region);

            // Transition back to SHADER_READ_ONLY
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);

            dev.EndSingleTimeCommands(cmd);
        }

        // Write PNG
        if (staging_info.pMappedData) {
            // Handle format differences: pool textures may be RGBA16F or RGBA8
            if (src_tex->format == VK_FORMAT_R8G8B8A8_UNORM || src_tex->format == VK_FORMAT_R8G8B8A8_SRGB) {
                success = stbi_write_png(output_filename.c_str(), w, h, 4,
                                         staging_info.pMappedData, w * 4) != 0;
            } else if (src_tex->format == VK_FORMAT_R16G16B16A16_SFLOAT) {
                // Convert float16 to uint8
                const uint16_t* float_src = static_cast<const uint16_t*>(staging_info.pMappedData);
                std::vector<unsigned char> rgba8(buffer_size);
                for (size_t i = 0; i < static_cast<size_t>(w) * h * 4; ++i) {
                    uint16_t half = float_src[i];
                    uint32_t sign = (half >> 15) & 1;
                    uint32_t exp = (half >> 10) & 0x1F;
                    uint32_t mant = half & 0x3FF;
                    float val;
                    if (exp == 0) {
                        val = (mant / 1024.0f) * (1.0f / 16384.0f);
                    } else if (exp == 31) {
                        val = 1.0f;
                    } else {
                        val = (1.0f + mant / 1024.0f) * std::pow(2.0f, (float)exp - 15.0f);
                    }
                    if (sign) val = -val;
                    val = std::max(0.0f, std::min(1.0f, val));
                    rgba8[i] = static_cast<unsigned char>(val * 255.0f + 0.5f);
                }
                success = stbi_write_png(output_filename.c_str(), w, h, 4,
                                         rgba8.data(), w * 4) != 0;
            } else if (src_tex->format == VK_FORMAT_R16G16B16A16_UNORM) {
                const uint16_t* unorm_src = static_cast<const uint16_t*>(staging_info.pMappedData);
                std::vector<unsigned char> rgba8(buffer_size);
                for (size_t i = 0; i < static_cast<size_t>(w) * h * 4; ++i) {
                    rgba8[i] = static_cast<unsigned char>(unorm_src[i] >> 8);
                }
                success = stbi_write_png(output_filename.c_str(), w, h, 4,
                                         rgba8.data(), w * 4) != 0;
            } else {
                Debug::Log("Screenshot (Vulkan): Unsupported format " + std::to_string(src_tex->format));
            }
        }

        vmaDestroyBuffer(dev.GetAllocator(), staging_buffer, staging_alloc);
    }

    if (color_pool_id) pool.QueueDelete(color_pool_id);

    if (success) {
        Debug::Log("Screenshot saved to: " + output_filename);
    } else {
        Debug::Log("Screenshot (Vulkan): Failed to write " + output_filename);
    }
    return success;
#elif defined(QCVIEW_USE_METAL)
    //=========================================================================
    // Metal screenshot path — readback via MTLTexture getBytes
    //=========================================================================
    if (!HasValidTexture()) {
        Debug::Log("Screenshot (Metal): No valid texture");
        return false;
    }

    std::string output_filename = directory_path;
    if (!output_filename.empty() && output_filename.back() != '/') {
        output_filename += "/";
    }
    output_filename += filename;

    auto& pool = qcview::MetalTexturePool::Instance();

    // Get a shared-storage texture safe for CPU readback.
    // OCIO output textures are already shared storage on Apple Silicon.
    // HW-decoded video_texture_ may be private storage, so we always run
    // through OCIO (even passthrough) to get a shared-storage result,
    // then sync the GPU before reading.
    uint64_t src_pool_id = static_cast<uint64_t>(video_texture_);
    uint64_t temp_pool_id = 0;  // Only set if we allocated a temp texture to delete
    if (metal_ocio_renderer_) {
        if (HasColorPipeline() && color_pipeline_->IsValid() && !color_pipeline_->IsPassthrough()) {
            // OCIO output is a renderer-owned single-buffered MTLTexture also
            // used by the live display. Blit to a throwaway pool texture so
            // annotation compositing and readback don't corrupt the live view.
            void* ocio_tex = metal_ocio_renderer_->Apply(
                color_pipeline_.get(), src_pool_id, video_width_, video_height_);
            if (ocio_tex) {
                temp_pool_id = metal_ocio_renderer_->CopyTextureSync(
                    ocio_tex, video_width_, video_height_);
                if (temp_pool_id != 0) src_pool_id = temp_pool_id;
            }
        } else {
            // Passthrough: source may be private storage (HW decode).
            // CopyTextureSync blits to a new shared-storage texture.
            const qcview::MetalTexture* raw = pool.GetTexture(src_pool_id);
            if (raw && raw->texture) {
                temp_pool_id = metal_ocio_renderer_->CopyTextureSync(
                    raw->texture, video_width_, video_height_);
                if (temp_pool_id != 0) src_pool_id = temp_pool_id;
            }
        }
    }

    const qcview::MetalTexture* src_tex = pool.GetTexture(src_pool_id);
    if (!src_tex || !src_tex->texture) {
        Debug::Log("Screenshot (Metal): Pool texture not found");
        if (temp_pool_id) pool.QueueDelete(temp_pool_id);
        return false;
    }

    int w = src_tex->width;
    int h = src_tex->height;

    // Check if we need to composite annotations
    std::vector<qcview::Annotations::ActiveStroke> strokes;
    if (get_annotation_strokes_callback_) {
        strokes = get_annotation_strokes_callback_();
    }

    // Bake annotations onto the texture if needed
    if (!strokes.empty() && metal_render_annotations_to_image_callback_) {
        metal_render_annotations_to_image_callback_(strokes, src_tex->texture, w, h);
    }

    // Ensure all GPU work (OCIO + annotation render) is complete before CPU readback
    qcview::MetalDeviceManager::Instance().WaitForGPU();

    // Readback texture to CPU via MetalTexturePool
    std::vector<unsigned char> pixels(w * h * 4);
    bool success = pool.ReadbackTexture(src_pool_id, pixels.data());

    if (success) {
        int result = stbi_write_png(output_filename.c_str(), w, h, 4, pixels.data(), w * 4);
        success = (result != 0);
        if (success) {
            Debug::Log("Screenshot saved to: " + output_filename);
        } else {
            Debug::Log("Screenshot (Metal): Failed to write PNG");
        }
    } else {
        Debug::Log("Screenshot (Metal): Texture readback failed");
    }

    if (temp_pool_id) pool.QueueDelete(temp_pool_id);
    return success;
#else
    //=========================================================================
    // OpenGL / Windows screenshot path
    //=========================================================================
    if (!HasValidTexture()) {
        return false;
    }

    std::string output_filename = directory_path;
#ifdef _WIN32
    if (!output_filename.empty() && output_filename.back() != '\\' && output_filename.back() != '/') {
        output_filename += "\\";
    }
#else
    if (!output_filename.empty() && output_filename.back() != '/') {
        output_filename += "/";
    }
#endif
    output_filename += filename;

    GLuint final_texture = video_texture_;
    if (HasColorPipeline()) {
        GLuint color_corrected = CreateColorCorrectedTexture(video_texture_, video_width_, video_height_,
                                                              video_width_, video_height_);
        if (color_corrected != 0) {
            final_texture = color_corrected;
        }
    }

    // Check if we need to composite annotations
    std::vector<qcview::Annotations::ActiveStroke> strokes;
    if (get_annotation_strokes_callback_) {
        strokes = get_annotation_strokes_callback_();
    }

    std::vector<unsigned char> pixels(video_width_ * video_height_ * 4);

    GLint current_fbo;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);

    bool success = false;

    if (!strokes.empty() && render_annotations_to_fbo_callback_) {
        // --- Annotation compositing path: FBO with depth-stencil for NanoVG ---
        GLuint render_fbo = 0, render_texture = 0, depth_stencil_rb = 0;

        glGenFramebuffers(1, &render_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, render_fbo);

        // Create color attachment
        glGenTextures(1, &render_texture);
        glBindTexture(GL_TEXTURE_2D, render_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, video_width_, video_height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, render_texture, 0);

        // Create depth-stencil for NanoVG anti-aliased strokes
        glGenRenderbuffers(1, &depth_stencil_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, depth_stencil_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, video_width_, video_height_);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_stencil_rb);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            glViewport(0, 0, video_width_, video_height_);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClearStencil(0);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

            // Blit video texture to render FBO using simple quad
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            const char* vertex_shader = R"(
                #version 330 core
                layout(location = 0) in vec2 aPos;
                layout(location = 1) in vec2 aTexCoord;
                out vec2 TexCoord;
                void main() {
                    gl_Position = vec4(aPos, 0.0, 1.0);
                    TexCoord = aTexCoord;
                }
            )";
            const char* fragment_shader = R"(
                #version 330 core
                in vec2 TexCoord;
                out vec4 FragColor;
                uniform sampler2D uTexture;
                void main() {
                    FragColor = texture(uTexture, TexCoord);
                }
            )";

            GLuint vs = glCreateShader(GL_VERTEX_SHADER);
            glShaderSource(vs, 1, &vertex_shader, nullptr);
            glCompileShader(vs);
            GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
            glShaderSource(fs, 1, &fragment_shader, nullptr);
            glCompileShader(fs);
            GLuint shader_program = glCreateProgram();
            glAttachShader(shader_program, vs);
            glAttachShader(shader_program, fs);
            glLinkProgram(shader_program);

            float quad_vertices[] = {
                -1.0f, -1.0f,  0.0f, 0.0f,
                 1.0f, -1.0f,  1.0f, 0.0f,
                 1.0f,  1.0f,  1.0f, 1.0f,
                -1.0f, -1.0f,  0.0f, 0.0f,
                 1.0f,  1.0f,  1.0f, 1.0f,
                -1.0f,  1.0f,  0.0f, 1.0f
            };

            GLuint vao, vbo;
            glGenVertexArrays(1, &vao);
            glGenBuffers(1, &vbo);
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
            glEnableVertexAttribArray(1);

            glUseProgram(shader_program);
            glBindTexture(GL_TEXTURE_2D, final_texture);
            glDrawArrays(GL_TRIANGLES, 0, 6);

            glDeleteVertexArrays(1, &vao);
            glDeleteBuffers(1, &vbo);
            glDeleteProgram(shader_program);
            glDeleteShader(vs);
            glDeleteShader(fs);

            // Render annotations on top
            render_annotations_to_fbo_callback_(strokes, video_width_, video_height_);

            // Read pixels
            glReadPixels(0, 0, video_width_, video_height_, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

            glEnable(GL_SCISSOR_TEST);

            int result = stbi_write_png(output_filename.c_str(), video_width_, video_height_, 4,
                                       pixels.data(), video_width_ * 4);
            if (result) {
                Debug::Log("Screenshot saved to: " + output_filename);
                success = true;
            }
        }

        glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
        glDeleteRenderbuffers(1, &depth_stencil_rb);
        glDeleteTextures(1, &render_texture);
        glDeleteFramebuffers(1, &render_fbo);

    } else {
        // --- Direct path (no annotations) ---
        GLuint temp_fbo;
        glGenFramebuffers(1, &temp_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, temp_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, final_texture, 0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            glReadPixels(0, 0, video_width_, video_height_, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

            int result = stbi_write_png(output_filename.c_str(), video_width_, video_height_, 4,
                                       pixels.data(), video_width_ * 4);
            if (result) {
                Debug::Log("Screenshot saved to: " + output_filename);
                success = true;
            }
        }

        glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
        glDeleteFramebuffers(1, &temp_fbo);
    }

    if (final_texture != video_texture_) {
        glDeleteTextures(1, &final_texture);
    }

    return success;
#endif // QCVIEW_USE_VULKAN / QCVIEW_USE_METAL / else
}

//=============================================================================
// Pipeline Mode
//=============================================================================

void VideoDisplayComponent::SetPipelineMode(PipelineMode mode) {
    if (mode == current_pipeline_mode_) {
        return;  // No change needed
    }

    Debug::Log("Switching pipeline mode from " + std::string(PipelineModeToString(current_pipeline_mode_)) +
               " to " + std::string(PipelineModeToString(mode)));

    // Store current playback state
    bool was_playing = IsPlaying();
    if (was_playing) {
        Pause();
    }

    // For HDR_RES mode, force OCIO passthrough pipeline
    // HDR content must pass through unchanged to the HDR10 swapchain
    if (mode == PipelineMode::HDR_RES) {
        ClearColorPipeline();
        Debug::Log("HDR_RES mode: Forced OCIO passthrough for HDR10 output");
    }

    // Update mode and internal format
    current_pipeline_mode_ = mode;
    auto it = PIPELINE_CONFIGS.find(mode);
    if (it != PIPELINE_CONFIGS.end()) {
        current_internal_format_ = it->second.internal_format;
    }

    // Recreate video textures with new format
    if (video_width_ > 0 && video_height_ > 0) {
        CreateVideoTexturesForMode(video_width_, video_height_, mode);

        // Also recreate color processing resources if OCIO pipeline is active
        if (color_pipeline_ && color_pipeline_->IsValid()) {
            CreateColorProcessingResourcesForMode(video_width_, video_height_, mode);
        }
    }

    // Resume playback if it was playing before
    if (was_playing) {
        Play();
    }

    Debug::Log("Pipeline mode switch completed successfully");
}

const PipelineConfig& VideoDisplayComponent::GetCurrentPipelineConfig() const {
    auto it = PIPELINE_CONFIGS.find(current_pipeline_mode_);
    if (it != PIPELINE_CONFIGS.end()) {
        return it->second;
    }
    // Return NORMAL config as default
    return PIPELINE_CONFIGS.at(PipelineMode::NORMAL);
}

//=============================================================================
// Backward Compatibility Methods
//=============================================================================

void VideoDisplayComponent::ResetState() {
    // Reset to clean state - clears cached values and textures
    cached_position_ = 0.0;
    cached_duration_ = 0.0;
    last_timeline_frame_ = -1;

    // Clear textures to background
    ClearVideoTextureToBackground();
    ClearColorTextureToBackground();

    Debug::Log("VideoDisplayComponent: State reset");
}

std::string VideoDisplayComponent::FormatTimecode(double seconds, double fps) {
    if (fps <= 0) fps = 24.0;

    int total_frames = static_cast<int>(seconds * fps + 0.5);
    int frames = total_frames % static_cast<int>(fps);
    int total_seconds = total_frames / static_cast<int>(fps);
    int secs = total_seconds % 60;
    int mins = (total_seconds / 60) % 60;
    int hours = total_seconds / 3600;

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d:%02d", hours, mins, secs, frames);
    return std::string(buffer);
}

//=============================================================================
// EXR Cache Methods
//=============================================================================

// Helper function to get shared EXRTranscoder instance
static qcview::EXRTranscoder& GetSharedTranscoder() {
    static qcview::EXRTranscoder s_transcoder;
    return s_transcoder;
}

VideoDisplayComponent::EXRCacheStats VideoDisplayComponent::GetEXRCacheStats() const {
    EXRCacheStats result;
    if (exr_cache_) {
        auto stats = exr_cache_->GetStats();
        result.total_frames = stats.totalFrames;
        result.cached_frames = stats.cachedFrames;
        result.pending_requests = stats.pendingRequests;
        result.cache_bytes = stats.cacheBytes;
    }
    return result;
}

size_t VideoDisplayComponent::ClearEXRDiskCache() {
    qcview::EXRTranscoder& transcoder = GetSharedTranscoder();
    transcoder.Initialize();  // Ensure initialized
    size_t bytes_cleared = transcoder.ClearAllTranscodes();
    Debug::Log("VideoDisplayComponent::ClearEXRDiskCache - Cleared " +
               std::to_string(bytes_cleared) + " bytes");
    return bytes_cleared;
}

//=============================================================================
// Fast Seek Passthrough Methods
//=============================================================================

void VideoDisplayComponent::StartRewind() {
    if (timeline_controller_) {
        timeline_controller_->StartRewind();
    }
}

void VideoDisplayComponent::StartFastForward() {
    if (timeline_controller_) {
        timeline_controller_->StartFastForward();
    }
}

void VideoDisplayComponent::StopFastSeek() {
    if (timeline_controller_) {
        timeline_controller_->StopFastSeek();
    }
}

void VideoDisplayComponent::SetScrubMode(bool enabled) {
    if (timeline_controller_) {
        timeline_controller_->SetScrubMode(enabled);
    }
}

void VideoDisplayComponent::UpdateFastSeek() {
    if (timeline_controller_) {
        timeline_controller_->UpdateFastSeek();
    }
}

bool VideoDisplayComponent::IsFastSeeking() const {
    if (timeline_controller_) {
        return timeline_controller_->IsFastSeeking();
    }
    return false;
}

bool VideoDisplayComponent::IsFastForward() const {
    if (timeline_controller_) {
        return timeline_controller_->IsFastForward();
    }
    return false;
}

double VideoDisplayComponent::GetFastSeekSpeed() const {
    if (timeline_controller_) {
        return timeline_controller_->GetFastSeekSpeed();
    }
    return 1.0;
}


#ifdef QCVIEW_USE_VULKAN
//=============================================================================
// Vulkan OCIO Pipeline (Linux)
//=============================================================================

void VideoDisplayComponent::CreateVulkanColorTexture(int width, int height) {
    if (vulkan_color_tex_.valid &&
        vulkan_color_tex_.width == width &&
        vulkan_color_tex_.height == height) {
        return;  // Already correct size
    }

    DestroyVulkanColorTexture();

    auto& dev = qcview::VulkanDeviceManager::Instance();
    VkDevice device = dev.GetDevice();

    // Create VkImage with COLOR_ATTACHMENT + SAMPLED usage
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(dev.GetAllocator(), &image_info, &alloc_info,
                       &vulkan_color_tex_.image, &vulkan_color_tex_.allocation, nullptr) != VK_SUCCESS) {
        Debug::Log("VideoDisplayComponent: Failed to create Vulkan color texture");
        return;
    }

    // Create VkImageView
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = vulkan_color_tex_.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &view_info, nullptr, &vulkan_color_tex_.view) != VK_SUCCESS) {
        Debug::Log("VideoDisplayComponent: Failed to create color texture view");
        vmaDestroyImage(dev.GetAllocator(), vulkan_color_tex_.image, vulkan_color_tex_.allocation);
        vulkan_color_tex_.image = VK_NULL_HANDLE;
        vulkan_color_tex_.allocation = VK_NULL_HANDLE;
        return;
    }

    // Create sampler
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxAnisotropy = 1.0f;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;

    if (vkCreateSampler(device, &sampler_info, nullptr, &vulkan_color_tex_.sampler) != VK_SUCCESS) {
        Debug::Log("VideoDisplayComponent: Failed to create color texture sampler");
        vkDestroyImageView(device, vulkan_color_tex_.view, nullptr);
        vmaDestroyImage(dev.GetAllocator(), vulkan_color_tex_.image, vulkan_color_tex_.allocation);
        vulkan_color_tex_.view = VK_NULL_HANDLE;
        vulkan_color_tex_.image = VK_NULL_HANDLE;
        vulkan_color_tex_.allocation = VK_NULL_HANDLE;
        return;
    }

    // Register with ImGui for display
    vulkan_color_tex_.descriptor_set = ImGui_ImplVulkan_AddTexture(
        vulkan_color_tex_.sampler, vulkan_color_tex_.view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vulkan_color_tex_.width = width;
    vulkan_color_tex_.height = height;
    vulkan_color_tex_.valid = true;

    Debug::Log("VideoDisplayComponent: Created Vulkan color texture " +
               std::to_string(width) + "x" + std::to_string(height));
}

void VideoDisplayComponent::DestroyVulkanColorTexture() {
    if (!vulkan_color_tex_.valid) return;

    auto& dev = qcview::VulkanDeviceManager::Instance();
    VkDevice device = dev.GetDevice();

    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);

        if (vulkan_color_tex_.descriptor_set != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(vulkan_color_tex_.descriptor_set);
        }
        if (vulkan_color_tex_.sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, vulkan_color_tex_.sampler, nullptr);
        }
        if (vulkan_color_tex_.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, vulkan_color_tex_.view, nullptr);
        }
        if (vulkan_color_tex_.image != VK_NULL_HANDLE) {
            vmaDestroyImage(dev.GetAllocator(), vulkan_color_tex_.image, vulkan_color_tex_.allocation);
        }
    }

    vulkan_color_tex_ = VulkanColorTexture{};
}

void VideoDisplayComponent::ApplyColorPipelineVulkan() {
    if (!vulkan_ocio_renderer_ || video_texture_ == 0) return;

    uint64_t src_pool_id = static_cast<uint64_t>(video_texture_);

    // Skip if source texture hasn't changed (same frame displayed multiple times)
    if (src_pool_id == vulkan_last_src_pool_id_ && vulkan_color_tex_.valid) {
        return;  // Previous OCIO result is still valid
    }
    vulkan_last_src_pool_id_ = src_pool_id;

    auto& pool = qcview::VulkanTexturePool::Instance();
    const auto* src_tex = pool.GetTexture(src_pool_id);
    if (!src_tex || !src_tex->is_valid) return;

    // Ensure color texture matches video dimensions
    CreateVulkanColorTexture(src_tex->width, src_tex->height);
    if (!vulkan_color_tex_.valid) return;

    // Use real OCIO pipeline if available, otherwise passthrough
    if (color_pipeline_ && color_pipeline_->IsValid() && !color_pipeline_->IsPassthrough()) {
        vulkan_ocio_renderer_->Apply(
            color_pipeline_.get(),
            src_tex->image_view,
            vulkan_color_tex_.image,
            vulkan_color_tex_.view,
            vulkan_color_tex_.width,
            vulkan_color_tex_.height);
    } else {
        vulkan_ocio_renderer_->ApplyPassthrough(
            src_tex->image_view,
            vulkan_color_tex_.image,
            vulkan_color_tex_.view,
            vulkan_color_tex_.width,
            vulkan_color_tex_.height);
    }
}

void VideoDisplayComponent::CreateVulkanDualColorTexture(int width, int height) {
    if (vulkan_dual_color_tex_.valid &&
        vulkan_dual_color_tex_.width == width &&
        vulkan_dual_color_tex_.height == height) {
        return;
    }

    DestroyVulkanDualColorTexture();

    auto& dev = qcview::VulkanDeviceManager::Instance();
    VkDevice device = dev.GetDevice();

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(dev.GetAllocator(), &image_info, &alloc_info,
                       &vulkan_dual_color_tex_.image, &vulkan_dual_color_tex_.allocation, nullptr) != VK_SUCCESS) {
        Debug::Log("VideoDisplayComponent: Failed to create Vulkan dual color texture");
        return;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = vulkan_dual_color_tex_.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &view_info, nullptr, &vulkan_dual_color_tex_.view) != VK_SUCCESS) {
        vmaDestroyImage(dev.GetAllocator(), vulkan_dual_color_tex_.image, vulkan_dual_color_tex_.allocation);
        vulkan_dual_color_tex_.image = VK_NULL_HANDLE;
        vulkan_dual_color_tex_.allocation = VK_NULL_HANDLE;
        return;
    }

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxAnisotropy = 1.0f;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;

    if (vkCreateSampler(device, &sampler_info, nullptr, &vulkan_dual_color_tex_.sampler) != VK_SUCCESS) {
        vkDestroyImageView(device, vulkan_dual_color_tex_.view, nullptr);
        vmaDestroyImage(dev.GetAllocator(), vulkan_dual_color_tex_.image, vulkan_dual_color_tex_.allocation);
        vulkan_dual_color_tex_.view = VK_NULL_HANDLE;
        vulkan_dual_color_tex_.image = VK_NULL_HANDLE;
        vulkan_dual_color_tex_.allocation = VK_NULL_HANDLE;
        return;
    }

    vulkan_dual_color_tex_.descriptor_set = ImGui_ImplVulkan_AddTexture(
        vulkan_dual_color_tex_.sampler, vulkan_dual_color_tex_.view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vulkan_dual_color_tex_.width = width;
    vulkan_dual_color_tex_.height = height;
    vulkan_dual_color_tex_.valid = true;
}

void VideoDisplayComponent::DestroyVulkanDualColorTexture() {
    if (!vulkan_dual_color_tex_.valid) return;

    auto& dev = qcview::VulkanDeviceManager::Instance();
    VkDevice device = dev.GetDevice();

    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);

        if (vulkan_dual_color_tex_.descriptor_set != VK_NULL_HANDLE)
            ImGui_ImplVulkan_RemoveTexture(vulkan_dual_color_tex_.descriptor_set);
        if (vulkan_dual_color_tex_.sampler != VK_NULL_HANDLE)
            vkDestroySampler(device, vulkan_dual_color_tex_.sampler, nullptr);
        if (vulkan_dual_color_tex_.view != VK_NULL_HANDLE)
            vkDestroyImageView(device, vulkan_dual_color_tex_.view, nullptr);
        if (vulkan_dual_color_tex_.image != VK_NULL_HANDLE)
            vmaDestroyImage(dev.GetAllocator(), vulkan_dual_color_tex_.image, vulkan_dual_color_tex_.allocation);
    }

    vulkan_dual_color_tex_ = VulkanColorTexture{};
}

uint64_t VideoDisplayComponent::CreateColorCorrectedPoolTexture(uint64_t input_pool_id, int width, int height) {
    if (!vulkan_ocio_renderer_ || input_pool_id == 0) return 0;

    auto& pool = qcview::VulkanTexturePool::Instance();
    const auto* src_tex = pool.GetTexture(input_pool_id);
    if (!src_tex || !src_tex->is_valid) return 0;

    // Ensure dual color texture matches dimensions
    CreateVulkanDualColorTexture(width, height);
    if (!vulkan_dual_color_tex_.valid) return 0;

    // Apply OCIO transform: source pool texture → dual color texture
    if (color_pipeline_ && color_pipeline_->IsValid() && !color_pipeline_->IsPassthrough()) {
        vulkan_ocio_renderer_->Apply(
            color_pipeline_.get(),
            src_tex->image_view,
            vulkan_dual_color_tex_.image,
            vulkan_dual_color_tex_.view,
            width, height);
    } else {
        vulkan_ocio_renderer_->ApplyPassthrough(
            src_tex->image_view,
            vulkan_dual_color_tex_.image,
            vulkan_dual_color_tex_.view,
            width, height);
    }

    // Register the color texture output in the pool so it can be displayed via ImGui
    // We create a new pool texture from the color output image each time
    uint64_t color_pool_id = pool.CreateTextureFromImage(
        vulkan_dual_color_tex_.image, width, height, VK_FORMAT_R8G8B8A8_UNORM);

    return color_pool_id;
}

#endif // QCVIEW_USE_VULKAN

#ifdef QCVIEW_USE_METAL
//=============================================================================
// Metal OCIO Implementation (macOS)
//=============================================================================

void VideoDisplayComponent::ApplyColorPipelineMetal() {
    if (!metal_ocio_renderer_ || video_texture_ == 0) return;

    uint64_t src_pool_id = static_cast<uint64_t>(video_texture_);

    // Skip if source texture hasn't changed (same frame displayed multiple times)
    if (src_pool_id == metal_last_src_pool_id_ && metal_color_mtl_texture_ != nullptr) {
        return;  // Previous OCIO result is still valid
    }
    metal_last_src_pool_id_ = src_pool_id;

    // Apply OCIO (uses persistent output texture owned by the renderer; never pool-evicted).
    if (color_pipeline_ && color_pipeline_->IsValid() && !color_pipeline_->IsPassthrough()) {
        metal_color_mtl_texture_ = metal_ocio_renderer_->Apply(
            color_pipeline_.get(), src_pool_id, video_width_, video_height_);
    } else {
        metal_color_mtl_texture_ = metal_ocio_renderer_->ApplyPassthrough(
            src_pool_id, video_width_, video_height_);
    }

    // When linear EDR is active, pre-encode linear → sRGB so ImGui's EDR fragment
    // shader can linearize back correctly. Not needed for gamma EDR modes.
    if (qcview::IsEDRLinearActive() && metal_color_mtl_texture_) {
        void* srgb_tex = metal_ocio_renderer_->ApplyLinearToSRGB(
            metal_color_mtl_texture_, video_width_, video_height_);
        if (srgb_tex) {
            metal_color_mtl_texture_ = srgb_tex;
        }
    }
}

uint64_t VideoDisplayComponent::CreateColorCorrectedPoolTextureMetal(uint64_t input_pool_id, int width, int height) {
    if (!metal_ocio_renderer_ || input_pool_id == 0) return 0;

    // Get the OCIO result as an MTLTexture pointer (uses persistent texture, async commit)
    void* ocio_tex = nullptr;
    if (color_pipeline_ && color_pipeline_->IsValid() && !color_pipeline_->IsPassthrough()) {
        ocio_tex = metal_ocio_renderer_->Apply(
            color_pipeline_.get(), input_pool_id, width, height);
    } else {
        ocio_tex = metal_ocio_renderer_->ApplyPassthrough(input_pool_id, width, height);
    }
    if (!ocio_tex) return 0;

    // Synchronous copy to a new shared-storage pool texture.
    // Needed because HW-decoded textures may use private storage, the
    // persistent OCIO output is single-buffered (shared with live display),
    // and the caller reads pixels from the returned pool id.
    return metal_ocio_renderer_->CopyTextureSync(ocio_tex, width, height);
}

#endif // QCVIEW_USE_METAL


#ifdef _WIN32
//=============================================================================
// D3D11 Rendering Implementation (Windows)
//=============================================================================

void VideoDisplayComponent::SetD3D11RenderingMode(bool enabled) {
    if (use_d3d11_rendering_ == enabled) return;

    auto& device_mgr = qcview::D3D11DeviceManager::Instance();
    if (enabled && !device_mgr.IsInitialized()) {
        Debug::Log("VideoDisplayComponent: Cannot enable D3D11 mode - device not initialized");
        return;
    }

    use_d3d11_rendering_ = enabled;

    // Propagate to timeline controller
    if (timeline_controller_) {
        timeline_controller_->SetD3D11RenderingMode(enabled);
    }

    if (enabled) {
        // Initialize D3D11 OCIO renderer
        if (!d3d11_ocio_renderer_) {
            d3d11_ocio_renderer_ = std::make_unique<qcview::D3D11OCIORenderer>();
            if (!d3d11_ocio_renderer_->Initialize()) {
                Debug::Log("VideoDisplayComponent: Failed to initialize D3D11 OCIO renderer");
                d3d11_ocio_renderer_.reset();
                use_d3d11_rendering_ = false;
                return;
            }
        }

        // Create D3D11 textures if we have video dimensions
        if (video_width_ > 0 && video_height_ > 0) {
            CreateD3D11VideoTextures(video_width_, video_height_);
            CreateD3D11ColorTextures(color_texture_width_ > 0 ? color_texture_width_ : video_width_,
                                     color_texture_height_ > 0 ? color_texture_height_ : video_height_);
        }

        Debug::Log("VideoDisplayComponent: D3D11 rendering mode enabled");
    } else {
        CleanupD3D11Resources();
        Debug::Log("VideoDisplayComponent: D3D11 rendering mode disabled");
    }
}

void VideoDisplayComponent::CreateD3D11VideoTextures(int width, int height) {
    if (width <= 0 || height <= 0) return;

    auto& device_mgr = qcview::D3D11DeviceManager::Instance();
    if (!device_mgr.IsInitialized()) return;

    // Determine format based on pipeline mode
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    switch (current_pipeline_mode_) {
        case PipelineMode::HIGH_RES:
            format = DXGI_FORMAT_R16G16B16A16_UNORM;
            break;
        case PipelineMode::ULTRA_HIGH_RES:
        case PipelineMode::HDR_RES:
            format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            break;
        default:
            format = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
    }

    // Release existing textures
    video_texture_d3d_.Reset();
    video_srv_d3d_.Reset();

    // Create video texture
    video_texture_d3d_ = device_mgr.CreateTexture2D(
        width, height, format,
        D3D11_USAGE_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE
    );

    if (!video_texture_d3d_) {
        Debug::Log("VideoDisplayComponent: Failed to create D3D11 video texture");
        return;
    }

    // Create SRV
    video_srv_d3d_ = device_mgr.CreateSRV(video_texture_d3d_.Get());

    Debug::Log("VideoDisplayComponent: Created D3D11 video texture " +
               std::to_string(width) + "x" + std::to_string(height));
}

void VideoDisplayComponent::CreateD3D11ColorTextures(int width, int height) {
    if (width <= 0 || height <= 0) return;

    auto& device_mgr = qcview::D3D11DeviceManager::Instance();
    if (!device_mgr.IsInitialized()) return;

    // Determine format based on pipeline mode
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    switch (current_pipeline_mode_) {
        case PipelineMode::HIGH_RES:
            format = DXGI_FORMAT_R16G16B16A16_UNORM;
            break;
        case PipelineMode::ULTRA_HIGH_RES:
        case PipelineMode::HDR_RES:
            format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            break;
        default:
            format = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
    }

    // Release existing textures
    color_texture_d3d_.Reset();
    color_srv_d3d_.Reset();
    color_rtv_d3d_.Reset();

    // Create color texture (render target)
    color_texture_d3d_ = device_mgr.CreateTexture2D(
        width, height, format,
        D3D11_USAGE_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET
    );

    if (!color_texture_d3d_) {
        Debug::Log("VideoDisplayComponent: Failed to create D3D11 color texture");
        return;
    }

    // Create SRV and RTV
    color_srv_d3d_ = device_mgr.CreateSRV(color_texture_d3d_.Get());
    color_rtv_d3d_ = device_mgr.CreateRTV(color_texture_d3d_.Get());

    Debug::Log("VideoDisplayComponent: Created D3D11 color texture " +
               std::to_string(width) + "x" + std::to_string(height));
}

void VideoDisplayComponent::ApplyColorPipelineD3D11() {
    if (!use_d3d11_rendering_ || !d3d11_ocio_renderer_) {
        return;
    }

    // Use timeline SRV if available (timeline mode), otherwise use video_srv_d3d_
    ID3D11ShaderResourceView* input_srv = timeline_srv_d3d_ ? timeline_srv_d3d_ : video_srv_d3d_.Get();
    if (!input_srv || !color_rtv_d3d_) {
        return;
    }

    // Determine target render dimensions
    int target_width = (use_content_dimensions_ && content_width_ > 0) ? content_width_ : video_width_;
    int target_height = (use_content_dimensions_ && content_height_ > 0) ? content_height_ : video_height_;

    if (target_width <= 0 || target_height <= 0) {
        return;
    }

    // Check if color resources need to be recreated
    if (!color_texture_d3d_) {
        CreateD3D11ColorTextures(target_width, target_height);
    } else {
        D3D11_TEXTURE2D_DESC desc;
        color_texture_d3d_->GetDesc(&desc);
        if (desc.Width != static_cast<UINT>(target_width) ||
            desc.Height != static_cast<UINT>(target_height)) {
            CreateD3D11ColorTextures(target_width, target_height);
        }
    }

    if (!color_rtv_d3d_) {
        return;
    }

    // Generate D3D11 HLSL shader if not already done
    if (color_pipeline_ && color_pipeline_->IsValid() && !color_pipeline_->HasD3D11Shaders()) {
        color_pipeline_->GenerateAndCompileShaderD3D11();
    }

    // Apply color pipeline
    if (color_pipeline_ && color_pipeline_->HasD3D11Shaders()) {
        d3d11_ocio_renderer_->Apply(
            color_pipeline_.get(),
            input_srv,
            color_rtv_d3d_.Get(),
            target_width, target_height
        );
    } else {
        // Passthrough if no OCIO pipeline
        d3d11_ocio_renderer_->ApplyPassthrough(
            input_srv,
            color_rtv_d3d_.Get(),
            target_width, target_height
        );
    }
}

void VideoDisplayComponent::CleanupD3D11Resources() {
    d3d11_ocio_renderer_.reset();
    color_rtv_d3d_.Reset();
    color_srv_d3d_.Reset();
    color_texture_d3d_.Reset();
    video_srv_d3d_.Reset();
    video_texture_d3d_.Reset();
    use_d3d11_rendering_ = false;
}

void VideoDisplayComponent::RenderVideoToHDRTarget(ID3D11RenderTargetView* rtv,
                                                    int x, int y, int width, int height) {
    if (!rtv || !has_video_) {
        return;
    }

    auto& device_mgr = qcview::D3D11DeviceManager::Instance();
    if (!device_mgr.IsInitialized()) {
        return;
    }

    auto* context = device_mgr.GetContext();
    if (!context) {
        return;
    }

    // Get the source texture (color-corrected output if available)
    ID3D11ShaderResourceView* src_srv = nullptr;

    // In D3D11 rendering mode, use the color-corrected texture
    if (use_d3d11_rendering_ && color_srv_d3d_) {
        src_srv = color_srv_d3d_.Get();
    }
    // Otherwise, in timeline mode, use the timeline SRV
    else if (timeline_srv_d3d_) {
        src_srv = timeline_srv_d3d_;
    }

    if (!src_srv) {
        // Clear to black if no source
        float clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        context->ClearRenderTargetView(rtv, clear_color);
        return;
    }

    // Use D3D11OCIORenderer for passthrough copy to the RTV
    // This renders the video with proper scaling to the HDR target
    if (d3d11_ocio_renderer_) {
        d3d11_ocio_renderer_->ApplyPassthrough(src_srv, rtv, width, height);
    }
}

#endif // _WIN32
