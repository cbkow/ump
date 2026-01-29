#include "video_display_component.h"
#include "direct_exr_cache.h"
#include "exr_transcoder.h"
#include "thumbnail_cache.h"
#include "image_loaders.h"
#include "../timeline/timeline_playback_controller.h"
#include "../timeline/timeline_cache.h"
#include "../color/ocio_pipeline.h"
#include "../utils/debug_utils.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>

// MPV includes for direct GPU rendering
#include <mpv/client.h>
#include <mpv/render_gl.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include "../gpu/d3d11_device_manager.h"
#include "../color/d3d11_ocio_renderer.h"
#endif

// Include STB image write for PNG output (implementation)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../external/glfw/deps/stb_image_write.h"

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
        "HDR (Float)",
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
        default: return "Unknown";
    }
}

// Global configuration accessors
extern ump::DirectEXRCacheConfig GetCurrentEXRCacheConfig();
extern ump::ThumbnailConfig GetCurrentThumbnailConfig();

//=============================================================================
// Constructor / Destructor
//=============================================================================

VideoDisplayComponent::VideoDisplayComponent() {
    // Initialize SVG renderer so dropdown is available
    svg_overlay_renderer_ = std::make_unique<SVGOverlayRenderer>();
    Debug::Log("VideoDisplayComponent: SVG overlay renderer initialized");

    // Pre-create DirectEXRCache so I/O threads are always running
    exr_cache_ = std::make_shared<ump::DirectEXRCache>();
    Debug::Log("VideoDisplayComponent: DirectEXRCache pre-created");
}

VideoDisplayComponent::~VideoDisplayComponent() {
    Cleanup();
}

//=============================================================================
// Core Lifecycle
//=============================================================================

bool VideoDisplayComponent::Initialize() {
    Debug::Log("VideoDisplayComponent::Initialize starting...");

    // Create transition placeholder texture
    CreateTransitionPlaceholder();

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

    Debug::Log("VideoDisplayComponent::Initialize complete");
    return true;
}

void VideoDisplayComponent::Cleanup() {
    Debug::Log("VideoDisplayComponent::Cleanup starting...");

    // Cleanup MPV first
    CleanupMPV();

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

    // Clear color pipeline
    color_pipeline_.reset();

#ifdef _WIN32
    // Cleanup D3D11 resources
    CleanupD3D11Resources();
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
}

void VideoDisplayComponent::CreateColorProcessingResourcesForMode(int width, int height, PipelineMode mode) {
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
}

void VideoDisplayComponent::CreateTransitionPlaceholder() {
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
}

void VideoDisplayComponent::ClearVideoTextureToBackground() {
    if (fbo_ == 0 || video_texture_ == 0) {
        return;
    }

    GLint current_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current_fbo);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glClearColor(27.0f/255.0f, 27.0f/255.0f, 27.0f/255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
}

void VideoDisplayComponent::ClearColorTextureToBackground() {
    if (color_fbo_ == 0 || color_texture_ == 0) {
        return;
    }

    GLint current_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current_fbo);

    glBindFramebuffer(GL_FRAMEBUFFER, color_fbo_);
    glClearColor(27.0f/255.0f, 27.0f/255.0f, 27.0f/255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
}

//=============================================================================
// Rendering
//=============================================================================

void VideoDisplayComponent::ProcessPendingTextureUploads() {
    // Process EXR textures EVERY frame (even when paused)
    // MUST be called BEFORE ImGui::NewFrame() to avoid GL state corruption
    if (exr_cache_) {
        exr_cache_->ProcessReadyTextures();
    }

    // Process timeline textures EVERY frame
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

    // Update video texture (color pipeline, frame injection) BEFORE ImGui
    // This ensures all GL operations complete before ImGui starts rendering
    if (has_video_ && video_texture_) {
        UpdateVideoTexture();
    }
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
    // Step 1: Render MPV frame if MPV is active (direct GPU rendering)
    // MPV renders to mpv_fbo_, then blits to video_texture_
    // When MPV is active, skip timeline injection - MPV already rendered
    if (mpv_gl_ && mpv_file_loaded_) {
        RenderMPVFrame();
        // Skip to color pipeline - MPV provided the frame
    }
    // Step 2: In timeline mode (without direct MPV), inject from cache
    else if (is_timeline_mode_ && timeline_controller_) {
        InjectCurrentTimelineFrame();
    }

    // Step 3: Apply OCIO color pipeline
    // Transforms video_texture_ → color_texture_
    if (color_pipeline_ && color_pipeline_->IsValid() && video_texture_ != 0) {
        ApplyColorPipeline();
    }
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

    // Choose which texture to display
    GLuint display_texture = video_texture_;

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
    ImGui_ImplOpenGL3_SetTextureHDRPassthrough((ImTextureID)(intptr_t)display_texture, true);

    // Display the texture
    ImGui::Image((void*)(intptr_t)display_texture, image_size);
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
    // Clear any existing pipeline first
    if (color_pipeline_) {
        color_pipeline_.reset();

        // Force OpenGL state cleanup
        glUseProgram(0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_3D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
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

    // Clean up OpenGL state first
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

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
}

bool VideoDisplayComponent::HasColorPipeline() const {
    return color_pipeline_ && color_pipeline_->IsValid();
}

bool VideoDisplayComponent::HasActiveColorTransform() const {
    return HasColorPipeline() && !color_pipeline_->IsPassthrough();
}

void VideoDisplayComponent::SetupColorProcessingResources() {
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
        exr_cache_ = std::make_shared<ump::DirectEXRCache>();
    }

    double cache_start_position = initial_position >= 0.0 ? initial_position : 0.0;

    auto exr_loader = std::make_unique<ump::EXRImageLoader>();
    if (exr_cache_->Initialize(std::move(exr_loader), sequence_files, layer_name, fps,
                               PipelineMode::HDR_RES, exr_sequence_start_frame_, cache_start_position)) {
        ump::DirectEXRCacheConfig config = GetCurrentEXRCacheConfig();
        exr_cache_->SetConfig(config);
        exr_cache_->SetLooping(loop_enabled_);
        exr_cache_->StartBackgroundCaching();
        Debug::Log("VideoDisplayComponent: DirectEXRCache initialized");
    } else {
        Debug::Log("VideoDisplayComponent: ERROR - Failed to initialize DirectEXRCache");
        exr_cache_.reset();
    }
}

void VideoDisplayComponent::SetEXRCacheConfig(const ump::DirectEXRCacheConfig& config) {
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

std::vector<ump::CacheSegment> VideoDisplayComponent::GetEXRCacheSegments() const {
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

void VideoDisplayComponent::SetTimelineMode(bool enabled, ump::TimelinePlaybackController* controller) {
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

    // OpenGL path
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
    if (!HasValidTexture()) {
        Debug::Log("Screenshot failed: No valid video texture");
        return false;
    }

    GLuint final_texture = video_texture_;

    if (HasColorPipeline()) {
        GLuint color_corrected = CreateColorCorrectedTexture(video_texture_, video_width_, video_height_,
                                                              video_width_, video_height_);
        if (color_corrected != 0) {
            final_texture = color_corrected;
        }
    }

    std::vector<unsigned char> pixels(video_width_ * video_height_ * 4);

    GLint current_fbo;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);

    GLuint temp_fbo;
    glGenFramebuffers(1, &temp_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, temp_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, final_texture, 0);

    bool success = false;
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glReadPixels(0, 0, video_width_, video_height_, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

#ifdef _WIN32
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();

            std::vector<unsigned char> rgba_pixels = pixels;

            // Convert RGBA to BGRA for Windows
            for (size_t i = 0; i < pixels.size(); i += 4) {
                std::swap(pixels[i], pixels[i + 2]);
            }

            // CF_DIB format
            BITMAPINFOHEADER bi = {};
            bi.biSize = sizeof(BITMAPINFOHEADER);
            bi.biWidth = video_width_;
            bi.biHeight = -video_height_;
            bi.biPlanes = 1;
            bi.biBitCount = 32;
            bi.biCompression = BI_RGB;

            HGLOBAL hDIB = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixels.size());
            if (hDIB) {
                unsigned char* pDIB = (unsigned char*)GlobalLock(hDIB);
                if (pDIB) {
                    memcpy(pDIB, &bi, sizeof(BITMAPINFOHEADER));
                    memcpy(pDIB + sizeof(BITMAPINFOHEADER), pixels.data(), pixels.size());
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
                stbi_write_png_to_func(png_write_func, &png_buffer, video_width_, video_height_,
                                       4, rgba_pixels.data(), video_width_ * 4);

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
#endif

        Debug::Log("Screenshot captured to clipboard (" + std::to_string(video_width_) + "x" +
                   std::to_string(video_height_) + ")");
    }

    glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
    glDeleteFramebuffers(1, &temp_fbo);

    if (final_texture != video_texture_) {
        glDeleteTextures(1, &final_texture);
    }

    return success;
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
        output_filename = base_filename + "_" + std::string(timestamp) + ".png";
#endif
    }

    return CaptureScreenshotToPath(std::filesystem::path(output_filename).parent_path().string(),
                                   std::filesystem::path(output_filename).filename().string());
}

bool VideoDisplayComponent::CaptureScreenshotToPath(const std::string& directory_path, const std::string& filename) {
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

    std::vector<unsigned char> pixels(video_width_ * video_height_ * 4);

    GLint current_fbo;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);

    GLuint temp_fbo;
    glGenFramebuffers(1, &temp_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, temp_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, final_texture, 0);

    bool success = false;
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

    if (final_texture != video_texture_) {
        glDeleteTextures(1, &final_texture);
    }

    return success;
}

//=============================================================================
// Pipeline Mode
//=============================================================================

void VideoDisplayComponent::ApplyPipelineModeConfig(PipelineMode mode) {
    if (!mpv_) return;

    auto it = PIPELINE_CONFIGS.find(mode);
    if (it == PIPELINE_CONFIGS.end()) {
        Debug::Log("ApplyPipelineModeConfig: Unknown pipeline mode");
        return;
    }

    // Use mpv_set_property_string for runtime changes (after mpv_initialize)
    switch (mode) {
        case PipelineMode::NORMAL:
            mpv_set_property_string(mpv_, "tone-mapping", "clip");
            mpv_set_property_string(mpv_, "target-trc", "auto");
            mpv_set_property_string(mpv_, "target-prim", "auto");
            mpv_set_property_string(mpv_, "hdr-compute-peak", "auto");
            mpv_set_property_string(mpv_, "target-peak", "auto");
            Debug::Log("Applied NORMAL pipeline config - RGBA8 standard processing");
            break;

        case PipelineMode::HIGH_RES:
            mpv_set_property_string(mpv_, "tone-mapping", "clip");
            mpv_set_property_string(mpv_, "target-trc", "auto");
            mpv_set_property_string(mpv_, "target-prim", "auto");
            mpv_set_property_string(mpv_, "hdr-compute-peak", "auto");
            mpv_set_property_string(mpv_, "target-peak", "auto");
            Debug::Log("Applied HIGH_RES pipeline config - RGBA16 12-bit precision for OCIO");
            break;

        case PipelineMode::ULTRA_HIGH_RES:
            // Use HIGH_RES behavior for ultra-high-res
            mpv_set_property_string(mpv_, "tone-mapping", "clip");
            mpv_set_property_string(mpv_, "target-trc", "auto");
            mpv_set_property_string(mpv_, "target-prim", "auto");
            mpv_set_property_string(mpv_, "hdr-compute-peak", "auto");
            mpv_set_property_string(mpv_, "target-peak", "auto");
            Debug::Log("Applied ULTRA_HIGH_RES pipeline config - using HIGH_RES behavior");
            break;

        case PipelineMode::HDR_RES:
            // HDR Passthrough - NOT YET IMPLEMENTED
            // Would require HDR swapchain support which OpenGL doesn't provide easily
            // Fall back to HIGH_RES behavior for now
            mpv_set_property_string(mpv_, "tone-mapping", "clip");
            mpv_set_property_string(mpv_, "target-trc", "auto");
            mpv_set_property_string(mpv_, "target-prim", "auto");
            mpv_set_property_string(mpv_, "hdr-compute-peak", "auto");
            mpv_set_property_string(mpv_, "target-peak", "auto");
            Debug::Log("Applied HDR_RES pipeline config - HDR passthrough not yet implemented, using HIGH_RES fallback");
            break;
    }
}

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

    // Apply MPV configuration for new mode
    ApplyPipelineModeConfig(mode);

    // Update mode and internal format
    current_pipeline_mode_ = mode;
    auto it = PIPELINE_CONFIGS.find(mode);
    if (it != PIPELINE_CONFIGS.end()) {
        current_internal_format_ = it->second.internal_format;
    }

    // Recreate video textures with new format
    if (video_width_ > 0 && video_height_ > 0) {
        CreateVideoTexturesForMode(video_width_, video_height_, mode);
        CreateMPVTextures(video_width_, video_height_);

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

ump::DualViewTimeline& VideoDisplayComponent::GetDualViewTimeline() {
    // Return static empty timeline - dual view mode is deprecated
    static ump::DualViewTimeline empty_timeline;
    return empty_timeline;
}

//=============================================================================
// EXR Cache Methods
//=============================================================================

// Helper function to get shared EXRTranscoder instance
static ump::EXRTranscoder& GetSharedTranscoder() {
    static ump::EXRTranscoder s_transcoder;
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
    ump::EXRTranscoder& transcoder = GetSharedTranscoder();
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

//=============================================================================
// MPV Video Playback (Direct GPU Rendering - Three-FBO Architecture)
//
// This implements the proven pattern from the reference app:
// 1. MPV renders to mpv_fbo_/mpv_texture_ (separate FBO)
// 2. glBlitFramebuffer transfers to video_texture_ (main display texture)
// 3. OCIO color pipeline applies to video_texture_ → color_texture_
//
// All video frames stay on GPU - no CPU roundtrip.
//=============================================================================

bool VideoDisplayComponent::InitializeMPV() {
    if (mpv_) {
        Debug::Log("VideoDisplayComponent: MPV already initialized");
        return true;
    }

    Debug::Log("VideoDisplayComponent: Initializing MPV...");

    // Create MPV instance
    mpv_ = mpv_create();
    if (!mpv_) {
        Debug::Log("VideoDisplayComponent: ERROR - mpv_create() failed");
        return false;
    }

    // CRITICAL: Use libmpv video output - renders to our FBO, no window created
    mpv_set_option_string(mpv_, "vo", "libmpv");

    // Configure MPV for frame-accurate seeking
    mpv_set_option_string(mpv_, "keep-open", "always");    // Keep open at EOF (don't unload)
    mpv_set_option_string(mpv_, "keep-open-pause", "no");  // Don't pause at EOF when looping
    mpv_set_option_string(mpv_, "idle", "yes");            // Stay idle when no file
    mpv_set_option_string(mpv_, "pause", "yes");           // Start paused
    mpv_set_option_string(mpv_, "hr-seek", "yes");         // High-quality seek
    mpv_set_option_string(mpv_, "hr-seek-framedrop", "no");// Don't drop frames during seek
    mpv_set_option_string(mpv_, "video-latency-hacks", "yes");

    // Color
    mpv_set_property_string(mpv_, "hdr-compute-peak", "no");
    mpv_set_property_string(mpv_, "inverse-tone-mapping", "no");
    mpv_set_property_string(mpv_, "target-contrast", "inf");
    mpv_set_property_string(mpv_, "target-peak", "10000");
    mpv_set_option_string(mpv_, "tone-mapping", "off");
    //mpv_set_option_string(mpv_, "target-trc", "auto");
    //mpv_set_option_string(mpv_, "target-prim=auto", "auto");

    // Visual settings
    mpv_set_option_string(mpv_, "alpha", "blend");
    mpv_set_option_string(mpv_, "background", "none");
    mpv_set_option_string(mpv_, "blend-subtitles", "yes");

    // Hardware acceleration with fallback
    mpv_set_option_string(mpv_, "hwdec", "auto-safe");

    // Disable OSD (on-screen display) - we handle UI ourselves
    mpv_set_option_string(mpv_, "osd-level", "0");

    // Enable MPV audio - better A/V sync than separate AudioMixer for solo video
    mpv_set_option_string(mpv_, "audio", "auto");
    mpv_set_option_string(mpv_, "volume-max", "100");  // Cap at 100%

    // Terminal/logging
    mpv_set_option_string(mpv_, "terminal", "no");

    // Initialize MPV
    if (mpv_initialize(mpv_) < 0) {
        Debug::Log("VideoDisplayComponent: ERROR - mpv_initialize() failed");
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
        return false;
    }

    // Observe properties for state tracking
    // These trigger MPV_EVENT_PROPERTY_CHANGE events
    mpv_observe_property(mpv_, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "eof-reached", MPV_FORMAT_FLAG);

    // Create OpenGL render context
    static auto get_proc_address = [](void* ctx, const char* name) -> void* {
        return (void*)glfwGetProcAddress(name);
    };

    mpv_opengl_init_params gl_init_params{};
    gl_init_params.get_proc_address = get_proc_address;
    gl_init_params.get_proc_address_ctx = nullptr;

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    if (mpv_render_context_create(&mpv_gl_, mpv_, params) < 0) {
        Debug::Log("VideoDisplayComponent: ERROR - mpv_render_context_create() failed");
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
        return false;
    }

    // Set up async update callback - MPV signals when new frame is ready
    // This decouples decode rate from render rate for smooth scrubbing
    mpv_render_context_set_update_callback(mpv_gl_, [](void* ctx) {
        auto* self = static_cast<VideoDisplayComponent*>(ctx);
        self->mpv_frame_ready_.store(true);
        self->mpv_redraw_needed_.store(true);
    }, this);

    Debug::Log("VideoDisplayComponent: MPV initialized with async update callback");
    return true;
}

void VideoDisplayComponent::CleanupMPV() {
    Debug::Log("VideoDisplayComponent: Cleaning up MPV...");

    // Unload any loaded file first
    UnloadVideoFile();

    // Cleanup MPV textures
    CleanupMPVTextures();

    // Destroy render context
    if (mpv_gl_) {
        mpv_render_context_free(mpv_gl_);
        mpv_gl_ = nullptr;
    }

    // Destroy MPV instance
    if (mpv_) {
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
    }

    Debug::Log("VideoDisplayComponent: MPV cleanup complete");
}

bool VideoDisplayComponent::LoadVideoFile(const std::string& path) {
    if (!mpv_) {
        Debug::Log("VideoDisplayComponent: Cannot load file - MPV not initialized");
        return false;
    }

    if (mpv_file_loaded_) {
        UnloadVideoFile();
    }

    Debug::Log("VideoDisplayComponent: Loading video file: " + path);
    current_video_path_ = path;

    // Load file
    const char* cmd[] = {"loadfile", path.c_str(), nullptr};
    if (mpv_command(mpv_, cmd) < 0) {
        Debug::Log("VideoDisplayComponent: ERROR - loadfile command failed");
        return false;
    }

    // Wait for file-loaded event (timeout 5 seconds)
    auto start_time = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::seconds(5);

    while (true) {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > timeout) {
            Debug::Log("VideoDisplayComponent: ERROR - File load timeout");
            return false;
        }

        mpv_event* event = mpv_wait_event(mpv_, 0.1);
        if (event->event_id == MPV_EVENT_FILE_LOADED) {
            break;
        }
        if (event->event_id == MPV_EVENT_END_FILE) {
            Debug::Log("VideoDisplayComponent: ERROR - File loading failed");
            return false;
        }
    }

    // Extract metadata
    int64_t w = 0, h = 0;
    double fps = 0.0, duration = 0.0;

    mpv_get_property(mpv_, "width", MPV_FORMAT_INT64, &w);
    mpv_get_property(mpv_, "height", MPV_FORMAT_INT64, &h);
    mpv_get_property(mpv_, "container-fps", MPV_FORMAT_DOUBLE, &fps);
    if (fps <= 0.0) {
        mpv_get_property(mpv_, "estimated-vf-fps", MPV_FORMAT_DOUBLE, &fps);
    }
    mpv_get_property(mpv_, "duration", MPV_FORMAT_DOUBLE, &duration);

    // Check if this is an audio-only file (no video track)
    bool is_audio_only = (w <= 0 || h <= 0);

    if (is_audio_only) {
        // Audio-only file: use default canvas dimensions (black display while audio plays)
        video_width_ = 1920;
        video_height_ = 1080;
        cached_fps_ = 24.0;  // Default for timeline display
        cached_duration_ = duration;
        mpv_cached_duration_ = duration;

        Debug::Log("VideoDisplayComponent: Audio-only file loaded - using default canvas " +
                   std::to_string(video_width_) + "x" + std::to_string(video_height_) +
                   ", duration=" + std::to_string(duration) + "s");
    } else {
        // Video file: use actual dimensions
        video_width_ = static_cast<int>(w);
        video_height_ = static_cast<int>(h);
        cached_fps_ = fps > 0 ? fps : 24.0;
        cached_duration_ = duration;
        mpv_cached_duration_ = duration;

        Debug::Log("VideoDisplayComponent: Video loaded - " +
                   std::to_string(video_width_) + "x" + std::to_string(video_height_) +
                   " @ " + std::to_string(cached_fps_) + " fps, " +
                   std::to_string(duration) + "s");
    }

    // has_video_ = true for both video and audio files (textures are created either way)
    has_video_ = true;

    // Create MPV textures for this resolution
    CreateMPVTextures(video_width_, video_height_);

    // Create video textures for display
    CreateVideoTexturesForMode(video_width_, video_height_, current_pipeline_mode_);

    // For audio-only files, clear the textures to black
    // (MPV won't render any video frames, so we need a clean black display)
    if (is_audio_only && fbo_ != 0) {
        GLint prev_fbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
        Debug::Log("VideoDisplayComponent: Cleared FBO to black for audio-only file");
    }

    // Setup color processing resources
    if (color_pipeline_) {
        SetupColorProcessingResources();
    }

    mpv_file_loaded_ = true;

    return true;
}

void VideoDisplayComponent::UnloadVideoFile() {
    if (!mpv_ || !mpv_file_loaded_) {
        return;
    }

    Debug::Log("VideoDisplayComponent: Unloading video file");

    // Stop playback
    const char* cmd[] = {"stop", nullptr};
    mpv_command(mpv_, cmd);

    mpv_file_loaded_ = false;
    mpv_is_playing_ = false;
    mpv_cached_position_ = 0.0;
    current_video_path_.clear();

    // Note: We keep the textures around for reuse
}

void VideoDisplayComponent::CreateMPVTextures(int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }

    // Cleanup existing
    CleanupMPVTextures();

    Debug::Log("VideoDisplayComponent: Creating MPV textures " +
               std::to_string(width) + "x" + std::to_string(height));

    // Determine format based on pipeline mode
    GLenum internal_format = GL_RGBA8;
    GLenum data_type = GL_UNSIGNED_BYTE;

    if (current_pipeline_mode_ == PipelineMode::HIGH_RES) {
        internal_format = GL_RGBA16;
        data_type = GL_UNSIGNED_SHORT;
    } else if (current_pipeline_mode_ == PipelineMode::ULTRA_HIGH_RES ||
               current_pipeline_mode_ == PipelineMode::HDR_RES) {
        internal_format = GL_RGBA16F;
        data_type = GL_HALF_FLOAT;
    }

    // Create texture for MPV rendering
    glGenTextures(1, &mpv_texture_);
    glBindTexture(GL_TEXTURE_2D, mpv_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0,
                 GL_RGBA, data_type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Create FBO for MPV
    glGenFramebuffers(1, &mpv_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, mpv_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mpv_texture_, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        Debug::Log("VideoDisplayComponent: ERROR - MPV FBO incomplete! Status: " + std::to_string(status));
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void VideoDisplayComponent::CleanupMPVTextures() {
    if (mpv_fbo_) {
        glDeleteFramebuffers(1, &mpv_fbo_);
        mpv_fbo_ = 0;
    }

    if (mpv_texture_) {
        glDeleteTextures(1, &mpv_texture_);
        mpv_texture_ = 0;
    }
}

void VideoDisplayComponent::RenderMPVFrame() {
    if (!mpv_gl_ || !mpv_file_loaded_ || mpv_fbo_ == 0) {
        return;
    }

    // Process MPV events (non-blocking)
    ProcessMPVEvents();

    // Async rendering: only render when MPV signals a new frame is ready
    // This prevents wasting GPU cycles re-rendering the same frame
    // We use mpv_render_context_update() to check flags properly
    uint64_t flags = mpv_render_context_update(mpv_gl_);
    if (!(flags & MPV_RENDER_UPDATE_FRAME)) {
        // No new frame ready - keep showing the last rendered frame
        return;
    }

    // Clear the frame ready flag
    mpv_frame_ready_.store(false);

    // Save current FBO binding
    GLint current_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current_fbo);

    // Setup render parameters for MPV
    // Pass internal_format to tell MPV the target pixel format for proper pipeline mode support
    int flip_y = 0;  // Don't flip - we handle this in the blit
    mpv_opengl_fbo mpv_fbo_desc{};
    mpv_fbo_desc.fbo = static_cast<int>(mpv_fbo_);
    mpv_fbo_desc.w = video_width_;
    mpv_fbo_desc.h = video_height_;
    mpv_fbo_desc.internal_format = static_cast<int>(current_internal_format_);  // Pass pipeline format

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &mpv_fbo_desc},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    // Render to mpv_fbo_
    if (mpv_render_context_render(mpv_gl_, params) < 0) {
        Debug::Log("VideoDisplayComponent: WARNING - mpv_render_context_render() failed");
        glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
        return;
    }

    // Blit from mpv_fbo_ to fbo_ (main video texture)
    // This is the fast GPU-to-GPU transfer - use GL_LINEAR for quality
    glBindFramebuffer(GL_READ_FRAMEBUFFER, mpv_fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
    glBlitFramebuffer(
        0, 0, video_width_, video_height_,
        0, 0, video_width_, video_height_,
        GL_COLOR_BUFFER_BIT,
        GL_LINEAR
    );

    // Restore previous FBO
    glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
}

void VideoDisplayComponent::ProcessMPVEvents() {
    if (!mpv_) {
        return;
    }

    // Non-blocking event poll (zero timeout)
    while (true) {
        mpv_event* event = mpv_wait_event(mpv_, 0.0);
        if (event->event_id == MPV_EVENT_NONE) {
            break;
        }

        switch (event->event_id) {
            case MPV_EVENT_PLAYBACK_RESTART:
                // Seek completed or playback started
                break;

            case MPV_EVENT_PROPERTY_CHANGE: {
                mpv_event_property* prop = (mpv_event_property*)event->data;
                if (prop && prop->name) {
                    if (strcmp(prop->name, "time-pos") == 0 && prop->data) {
                        mpv_cached_position_ = *(double*)prop->data;
                        cached_position_ = mpv_cached_position_;
                    } else if (strcmp(prop->name, "pause") == 0 && prop->data) {
                        bool is_paused = *(int*)prop->data != 0;
                        mpv_is_playing_ = !is_paused;
                    } else if (strcmp(prop->name, "duration") == 0 && prop->data) {
                        mpv_cached_duration_ = *(double*)prop->data;
                        cached_duration_ = mpv_cached_duration_;
                    } else if (strcmp(prop->name, "eof-reached") == 0 && prop->data) {
                        bool eof = *(int*)prop->data != 0;
                        if (eof && mpv_loop_enabled_) {
                            // EOF reached with loop - seek to start and play
                            Debug::Log("VideoDisplayComponent: EOF reached with loop - restarting");
                            MPVSeek(0.0);
                            MPVPlay();
                        }
                    }
                }
                break;
            }

            case MPV_EVENT_END_FILE: {
                // Check end-file reason
                mpv_event_end_file* end_file = (mpv_event_end_file*)event->data;
                if (end_file) {
                    if (end_file->reason == MPV_END_FILE_REASON_EOF) {
                        // Normal end of file
                        // ALWAYS invoke EOF callback first - let callback decide if playlist should advance
                        // Callback returns true if it handled the EOF (e.g., playlist transition)
                        bool handled_by_callback = false;
                        if (mpv_eof_callback_) {
                            mpv_eof_callback_();
                            // If callback was invoked, assume it may have handled the transition
                            // The callback will load new media if playlist is active
                            handled_by_callback = true;
                        }

                        // Only do default loop/stop behavior if callback didn't handle it
                        // Note: Callback sets handled_by_callback but we still check loop state
                        // because the callback may have determined playlist isn't active
                        if (!handled_by_callback) {
                            if (mpv_loop_enabled_) {
                                Debug::Log("VideoDisplayComponent: MPV EOF with loop - restarting");
                                MPVSeek(0.0);
                                int pause = 0;
                                mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &pause);
                                mpv_is_playing_ = true;
                            } else {
                                Debug::Log("VideoDisplayComponent: MPV EOF - stopping");
                                mpv_is_playing_ = false;
                            }
                        }
                    } else if (end_file->reason == MPV_END_FILE_REASON_STOP) {
                        // User stopped playback
                        mpv_is_playing_ = false;
                    }
                    // Ignore other reasons (redirect, error, etc.)
                }
                break;
            }

            default:
                break;
        }
    }

    // Update position cache
    double pos = 0.0;
    if (mpv_get_property(mpv_, "time-pos", MPV_FORMAT_DOUBLE, &pos) >= 0) {
        mpv_cached_position_ = pos;
        cached_position_ = pos;
    }
}

void VideoDisplayComponent::MPVPlay() {
    if (!mpv_ || !mpv_file_loaded_) {
        return;
    }

    // Check if we're at EOF - if so, seek to start first
    int eof_reached = 0;
    if (mpv_get_property(mpv_, "eof-reached", MPV_FORMAT_FLAG, &eof_reached) >= 0 && eof_reached) {
        Debug::Log("VideoDisplayComponent: At EOF, seeking to start before play");
        MPVSeek(0.0);
    }

    int pause = 0;
    mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &pause);
    mpv_is_playing_ = true;

    Debug::Log("VideoDisplayComponent: MPV Play");
}

void VideoDisplayComponent::MPVPause() {
    if (!mpv_ || !mpv_file_loaded_) {
        return;
    }

    int pause = 1;
    mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &pause);
    mpv_is_playing_ = false;

    Debug::Log("VideoDisplayComponent: MPV Pause");
}

void VideoDisplayComponent::MPVSeek(double position) {
    if (!mpv_ || !mpv_file_loaded_) {
        return;
    }

    // Seek to absolute position
    std::string pos_str = std::to_string(position);
    const char* cmd[] = {"seek", pos_str.c_str(), "absolute", "exact", nullptr};
    mpv_command(mpv_, cmd);
    mpv_cached_position_ = position;
    cached_position_ = position;
}

void VideoDisplayComponent::MPVStepFrame(int direction) {
    if (!mpv_ || !mpv_file_loaded_) {
        return;
    }

    // Pause first
    MPVPause();

    if (direction > 0) {
        // Step forward
        const char* cmd[] = {"frame-step", nullptr};
        mpv_command(mpv_, cmd);
    } else if (direction < 0) {
        // Step backward
        const char* cmd[] = {"frame-back-step", nullptr};
        mpv_command(mpv_, cmd);
    }
}

double VideoDisplayComponent::GetMPVPosition() const {
    return mpv_cached_position_;
}

double VideoDisplayComponent::GetMPVDuration() const {
    return mpv_cached_duration_;
}

double VideoDisplayComponent::GetMPVSpeed() const {
    return mpv_speed_;
}

void VideoDisplayComponent::MPVSetLoop(bool enabled) {
    if (!mpv_) {
        return;
    }

    mpv_loop_enabled_ = enabled;

    // Set MPV loop property: "inf" for infinite loop, "no" for no loop
    const char* loop_value = enabled ? "inf" : "no";
    mpv_set_property_string(mpv_, "loop-file", loop_value);

    Debug::Log("VideoDisplayComponent: MPV loop " + std::string(enabled ? "enabled" : "disabled"));
}

void VideoDisplayComponent::MPVSetSpeed(double speed) {
    if (!mpv_ || !mpv_file_loaded_) {
        return;
    }

    // Clamp speed to reasonable range (0.1x to 8x)
    if (speed < 0.1) speed = 0.1;
    if (speed > 8.0) speed = 8.0;

    mpv_speed_ = speed;
    mpv_set_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &speed);

    Debug::Log("VideoDisplayComponent: MPV speed set to " + std::to_string(speed) + "x");
}

void VideoDisplayComponent::MPVSetVolume(double volume) {
    if (!mpv_) {
        return;
    }

    // Clamp volume to 0.0 - 1.0
    if (volume < 0.0) volume = 0.0;
    if (volume > 1.0) volume = 1.0;

    mpv_volume_ = volume;

    // MPV uses 0-100 scale
    double mpv_vol = volume * 100.0;
    mpv_set_property(mpv_, "volume", MPV_FORMAT_DOUBLE, &mpv_vol);
}

void VideoDisplayComponent::MPVSetMute(bool muted) {
    if (!mpv_) {
        return;
    }

    mpv_muted_ = muted;

    int mute_flag = muted ? 1 : 0;
    mpv_set_property(mpv_, "mute", MPV_FORMAT_FLAG, &mute_flag);
}

//=============================================================================
// Async Scrubbing - Playhead moves instantly, decode catches up
//=============================================================================

void VideoDisplayComponent::MPVStartScrub() {
    if (!mpv_ || !mpv_file_loaded_) {
        return;
    }

    mpv_is_scrubbing_.store(true);
    mpv_scrub_target_.store(mpv_cached_position_);

    // Pause playback during scrub
    MPVPause();

    Debug::Log("VideoDisplayComponent: MPV scrub started");
}

void VideoDisplayComponent::MPVUpdateScrub(double position) {
    if (!mpv_ || !mpv_file_loaded_) {
        return;
    }

    // Clamp position
    if (position < 0.0) position = 0.0;
    if (position > mpv_cached_duration_) position = mpv_cached_duration_;

    // Update scrub target immediately (for instant playhead response)
    mpv_scrub_target_.store(position);

    // Issue async seek - MPV will decode in background and signal via callback
    // Use "keyframes" hint during active scrub for speed, exact on end
    std::string pos_str = std::to_string(position);
    const char* cmd[] = {"seek", pos_str.c_str(), "absolute", "keyframes", nullptr};
    mpv_command_async(mpv_, 0, cmd);
}

void VideoDisplayComponent::MPVEndScrub() {
    if (!mpv_ || !mpv_file_loaded_) {
        return;
    }

    // Final exact seek to the target position
    double target = mpv_scrub_target_.load();
    std::string pos_str = std::to_string(target);
    const char* cmd[] = {"seek", pos_str.c_str(), "absolute", "exact", nullptr};
    mpv_command(mpv_, cmd);

    mpv_is_scrubbing_.store(false);

    Debug::Log("VideoDisplayComponent: MPV scrub ended at " + std::to_string(target));
}

//=============================================================================
// AB-Loop for In/Out Point Playback
//=============================================================================

void VideoDisplayComponent::MPVSetABLoop(double in_point, double out_point) {
    if (!mpv_) {
        return;
    }

    // Set MPV's ab-loop-a and ab-loop-b properties
    // These create a native loop region that MPV handles automatically
    if (in_point >= 0.0) {
        mpv_set_property(mpv_, "ab-loop-a", MPV_FORMAT_DOUBLE, &in_point);
        mpv_ab_loop_a_ = in_point;
    } else {
        // Clear in point by setting to "no"
        mpv_set_property_string(mpv_, "ab-loop-a", "no");
        mpv_ab_loop_a_ = -1.0;
    }

    if (out_point >= 0.0) {
        mpv_set_property(mpv_, "ab-loop-b", MPV_FORMAT_DOUBLE, &out_point);
        mpv_ab_loop_b_ = out_point;
    } else {
        // Clear out point by setting to "no"
        mpv_set_property_string(mpv_, "ab-loop-b", "no");
        mpv_ab_loop_b_ = -1.0;
    }

    mpv_ab_loop_active_ = (in_point >= 0.0 || out_point >= 0.0);

    Debug::Log("VideoDisplayComponent: MPV AB-loop set: A=" +
               (in_point >= 0.0 ? std::to_string(in_point) : "none") + ", B=" +
               (out_point >= 0.0 ? std::to_string(out_point) : "none"));
}

void VideoDisplayComponent::MPVClearABLoop() {
    if (!mpv_) {
        return;
    }

    mpv_set_property_string(mpv_, "ab-loop-a", "no");
    mpv_set_property_string(mpv_, "ab-loop-b", "no");
    mpv_ab_loop_a_ = -1.0;
    mpv_ab_loop_b_ = -1.0;
    mpv_ab_loop_active_ = false;

    Debug::Log("VideoDisplayComponent: MPV AB-loop cleared");
}

//=============================================================================
// Async Fast Seek - Decoupled from decode (like scrubbing)
//=============================================================================

void VideoDisplayComponent::MPVStartFastSeek(bool forward) {
    if (!mpv_ || !mpv_file_loaded_) {
        return;
    }

    mpv_fast_seeking_ = true;
    mpv_fast_forward_ = forward;
    mpv_fast_seek_speed_ = 2.0;  // Start at 2x
    mpv_fast_seek_position_ = mpv_cached_position_;
    mpv_fast_seek_start_time_ = std::chrono::steady_clock::now();
    mpv_fast_seek_last_update_ = mpv_fast_seek_start_time_;

    if (forward) {
        // Fast Forward: Use MPV's native speed control for smooth playback
        MPVSetSpeed(mpv_fast_seek_speed_);
        if (!mpv_is_playing_) {
            MPVPlay();
        }
        Debug::Log("VideoDisplayComponent: MPV fast forward started (native speed) at " +
                   std::to_string(mpv_fast_seek_speed_) + "x");
    } else {
        // Rewind: MPV can't play backwards, use decoupled periodic seeks
        MPVPause();
        Debug::Log("VideoDisplayComponent: MPV rewind started (decoupled seeks)");
    }
}

void VideoDisplayComponent::MPVUpdateFastSeek() {
    if (!mpv_ || !mpv_file_loaded_ || !mpv_fast_seeking_) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    double elapsed_since_start = std::chrono::duration<double>(now - mpv_fast_seek_start_time_).count();
    double elapsed_since_last = std::chrono::duration<double>(now - mpv_fast_seek_last_update_).count();
    mpv_fast_seek_last_update_ = now;

    // Accelerate speed over time (doubles every second, up to 32x)
    // For FF: cap at 8x for watchable playback, for RW: can go higher
    double max_speed = mpv_fast_forward_ ? 8.0 : 32.0;
    mpv_fast_seek_speed_ = std::min(max_speed, 2.0 * std::pow(2.0, elapsed_since_start));

    if (mpv_fast_forward_) {
        // Fast Forward: Use MPV's native speed control
        // Just update the speed - MPV handles the playback
        MPVSetSpeed(mpv_fast_seek_speed_);
        // Position is tracked by MPV, not us
        mpv_fast_seek_position_ = mpv_cached_position_;
    } else {
        // Rewind: Decoupled periodic seeks (MPV can't play backwards)
        double delta = mpv_fast_seek_speed_ * elapsed_since_last;
        mpv_fast_seek_position_ -= delta;

        // Clamp to valid range
        if (mpv_fast_seek_position_ < 0.0) {
            mpv_fast_seek_position_ = 0.0;
        }

        // Update scrub target for instant playhead response
        mpv_scrub_target_.store(mpv_fast_seek_position_);

        // Throttle actual MPV seeks to avoid flooding - only seek every 150ms
        static std::chrono::steady_clock::time_point last_mpv_seek_time;
        double ms_since_last_seek = std::chrono::duration<double, std::milli>(now - last_mpv_seek_time).count();

        if (ms_since_last_seek >= 150.0) {
            last_mpv_seek_time = now;
            std::string pos_str = std::to_string(mpv_fast_seek_position_);
            const char* cmd[] = {"seek", pos_str.c_str(), "absolute", "keyframes", nullptr};
            mpv_command_async(mpv_, 0, cmd);
        }
    }
}

void VideoDisplayComponent::MPVStopFastSeek() {
    if (!mpv_ || !mpv_fast_seeking_) {
        return;
    }

    if (mpv_fast_forward_) {
        // Fast Forward: Reset speed and pause
        MPVSetSpeed(1.0);
        MPVPause();
    } else {
        // Rewind: Final exact seek to the position
        std::string pos_str = std::to_string(mpv_fast_seek_position_);
        const char* cmd[] = {"seek", pos_str.c_str(), "absolute", "exact", nullptr};
        mpv_command(mpv_, cmd);
        mpv_scrub_target_.store(mpv_fast_seek_position_);
    }

    mpv_fast_seeking_ = false;

    Debug::Log("VideoDisplayComponent: MPV fast seek stopped at " +
               std::to_string(mpv_fast_forward_ ? mpv_cached_position_ : mpv_fast_seek_position_) +
               " (speed was " + std::to_string(mpv_fast_seek_speed_) + "x)");
}

#ifdef _WIN32
//=============================================================================
// D3D11 Rendering Implementation (Windows)
//=============================================================================

void VideoDisplayComponent::SetD3D11RenderingMode(bool enabled) {
    if (use_d3d11_rendering_ == enabled) return;

    auto& device_mgr = ump::D3D11DeviceManager::Instance();
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
            d3d11_ocio_renderer_ = std::make_unique<ump::D3D11OCIORenderer>();
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

    auto& device_mgr = ump::D3D11DeviceManager::Instance();
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

    auto& device_mgr = ump::D3D11DeviceManager::Instance();
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

    auto& device_mgr = ump::D3D11DeviceManager::Instance();
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
