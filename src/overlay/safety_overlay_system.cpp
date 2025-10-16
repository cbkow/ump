#include "safety_overlay_system.h"
#include <sstream>
#include <cstring>
#include <cmath>

SafetyOverlaySystem::SafetyOverlaySystem() {
    // Initialize with default settings - color will be overridden by application
    current_settings.type = SafetyOverlayType::TITLE_SAFE_16_9;
    current_settings.opacity = 0.7f;
    current_settings.line_width = 2.0f;
    current_settings.color[0] = 1.0f; // Default white - will be overridden
    current_settings.color[1] = 1.0f;
    current_settings.color[2] = 1.0f;
    current_settings.color[3] = 1.0f;
    current_settings.show_labels = true;
    current_settings.show_percentage_marks = false;
}

SafetyOverlaySystem::~SafetyOverlaySystem() {
    Cleanup();
}

bool SafetyOverlaySystem::Initialize(int width, int height) {
    video_width = width;
    video_height = height;

    if (video_width <= 0 || video_height <= 0) {
        Debug::Log("SafetyOverlaySystem: Invalid dimensions " + std::to_string(width) + "x" + std::to_string(height));
        return false;
    }

    // Setup FBO and textures
    if (!SetupFramebuffer()) {
        Debug::Log("SafetyOverlaySystem: Failed to setup framebuffer");
        return false;
    }

    // Setup shaders
    if (!SetupShaders()) {
        Debug::Log("SafetyOverlaySystem: Failed to setup shaders");
        Cleanup();
        return false;
    }

    // Setup geometry
    if (!SetupGeometry()) {
        Debug::Log("SafetyOverlaySystem: Failed to setup geometry");
        Cleanup();
        return false;
    }

    is_initialized = true;
    Debug::Log("SafetyOverlaySystem: Initialized successfully (" + std::to_string(width) + "x" + std::to_string(height) + ")");
    return true;
}

void SafetyOverlaySystem::Cleanup() {
    // Cleanup FBO and textures
    if (safety_texture) {
        glDeleteTextures(1, &safety_texture);
        safety_texture = 0;
    }
    if (safety_fbo) {
        glDeleteFramebuffers(1, &safety_fbo);
        safety_fbo = 0;
    }

    // Cleanup geometry
    if (quad_vao) {
        glDeleteVertexArrays(1, &quad_vao);
        quad_vao = 0;
    }
    if (quad_vbo) {
        glDeleteBuffers(1, &quad_vbo);
        quad_vbo = 0;
    }
    if (guide_vao) {
        glDeleteVertexArrays(1, &guide_vao);
        guide_vao = 0;
    }
    if (guide_vbo) {
        glDeleteBuffers(1, &guide_vbo);
        guide_vbo = 0;
    }

    // Cleanup shaders
    CleanupShaders();

    is_initialized = false;
}

void SafetyOverlaySystem::UpdateDimensions(int width, int height) {
    if (width == video_width && height == video_height) {
        return; // No change
    }

    video_width = width;
    video_height = height;

    if (is_initialized) {
        // Recreate framebuffer with new dimensions
        if (safety_texture) {
            glDeleteTextures(1, &safety_texture);
            safety_texture = 0;
        }
        if (safety_fbo) {
            glDeleteFramebuffers(1, &safety_fbo);
            safety_fbo = 0;
        }

        SetupFramebuffer();
        GenerateGuideGeometry(); // Regenerate guides for new aspect ratio
    }
}

void SafetyOverlaySystem::SetOverlaySettings(const SafetyGuideSettings& settings) {
    bool type_changed = (current_settings.type != settings.type);
    current_settings = settings;

    if (type_changed && is_initialized) {
        GenerateGuideGeometry();
    }
}

GLuint SafetyOverlaySystem::CompositeOverlays(GLuint input_texture_id, int texture_width, int texture_height) {
    if (!is_initialized || !overlay_enabled || current_settings.type == SafetyOverlayType::NONE) {
        return input_texture_id; // Pass through unchanged
    }

    if (safety_fbo == 0 || safety_texture == 0) {
        Debug::Log("SafetyOverlaySystem: Resources not ready");
        return input_texture_id;
    }

    // Save current OpenGL state
    GLint current_fbo;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    GLint current_program;
    glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);

    // Bind safety FBO
    glBindFramebuffer(GL_FRAMEBUFFER, safety_fbo);
    glViewport(0, 0, video_width, video_height);

    // Clear with transparent background
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // First pass: Copy input texture to safety texture
    glUseProgram(composite_shader_program);

    // Bind input texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, input_texture_id);

    // Set composite shader uniforms
    GLint input_texture_loc = glGetUniformLocation(composite_shader_program, "inputTexture");
    if (input_texture_loc >= 0) {
        glUniform1i(input_texture_loc, 0);
    }

    // Render fullscreen quad to copy input
    glBindVertexArray(quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Second pass: Draw safety guides on top
    if (guide_vertices.size() > 0) {
        // Enable blending for overlay
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // TODO: Implement proper line shader for guide rendering
        // For now, skip line rendering to avoid OpenGL compatibility issues
        // This will be implemented when we add the UI controls

        // Future implementation will use a simple line shader:
        // - Vertex shader: transform positions
        // - Fragment shader: output solid color with opacity

        glDisable(GL_BLEND);
    }

    // Restore OpenGL state
    glUseProgram(current_program);
    glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);

    return safety_texture;
}

bool SafetyOverlaySystem::SetupFramebuffer() {
    // Create FBO for safety overlay compositing
    glGenFramebuffers(1, &safety_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, safety_fbo);

    // Create safety texture with same dimensions as video
    glGenTextures(1, &safety_texture);
    glBindTexture(GL_TEXTURE_2D, safety_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, video_width, video_height,
        0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Attach to FBO
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, safety_texture, 0);

    // Check FBO completeness
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        Debug::Log("ERROR: Safety FBO incomplete! Status: " + std::to_string(status));
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

bool SafetyOverlaySystem::SetupShaders() {
    // Vertex shader for compositing
    const char* vertex_src = R"(
        #version 330 core
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aTexCoord;
        out vec2 TexCoord;

        void main() {
            gl_Position = vec4(aPos, 0.0, 1.0);
            TexCoord = aTexCoord;
        }
    )";

    // Fragment shader for compositing
    const char* fragment_src = R"(
        #version 330 core
        in vec2 TexCoord;
        out vec4 FragColor;
        uniform sampler2D inputTexture;

        void main() {
            FragColor = texture(inputTexture, TexCoord);
        }
    )";

    // Compile vertex shader
    overlay_vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    if (!CompileShader(overlay_vertex_shader, vertex_src, GL_VERTEX_SHADER)) {
        return false;
    }

    // Compile fragment shader
    overlay_fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    if (!CompileShader(overlay_fragment_shader, fragment_src, GL_FRAGMENT_SHADER)) {
        return false;
    }

    // Create and link program
    composite_shader_program = glCreateProgram();
    glAttachShader(composite_shader_program, overlay_vertex_shader);
    glAttachShader(composite_shader_program, overlay_fragment_shader);

    if (!LinkProgram(composite_shader_program)) {
        return false;
    }

    return true;
}

bool SafetyOverlaySystem::SetupGeometry() {
    // Create fullscreen quad for compositing (same pattern as OCIO)
    float quad_vertices[] = {
        // positions   // texCoords
        -1.0f,  1.0f,  0.0f, 1.0f,  // top-left
        -1.0f, -1.0f,  0.0f, 0.0f,  // bottom-left
         1.0f, -1.0f,  1.0f, 0.0f,  // bottom-right

        -1.0f,  1.0f,  0.0f, 1.0f,  // top-left
         1.0f, -1.0f,  1.0f, 0.0f,  // bottom-right
         1.0f,  1.0f,  1.0f, 1.0f   // top-right
    };

    glGenVertexArrays(1, &quad_vao);
    glGenBuffers(1, &quad_vbo);

    glBindVertexArray(quad_vao);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);

    // Position attribute
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    // TexCoord attribute
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    // Setup guide geometry VAO
    glGenVertexArrays(1, &guide_vao);
    glGenBuffers(1, &guide_vbo);

    // Generate initial guide geometry
    GenerateGuideGeometry();

    return true;
}

void SafetyOverlaySystem::GenerateGuideGeometry() {
    guide_vertices.clear();

    float aspect_ratio = (float)video_width / (float)video_height;

    switch (current_settings.type) {
        case SafetyOverlayType::TITLE_SAFE_16_9:
            GenerateTitleSafeGuides(16.0f / 9.0f);
            break;
        case SafetyOverlayType::ACTION_SAFE_16_9:
            GenerateActionSafeGuides(16.0f / 9.0f);
            break;
        case SafetyOverlayType::TITLE_SAFE_9_16:
            GenerateTitleSafeGuides(9.0f / 16.0f);
            break;
        case SafetyOverlayType::ACTION_SAFE_9_16:
            GenerateActionSafeGuides(9.0f / 16.0f);
            break;
        case SafetyOverlayType::CENTER_CUT_SAFE:
            GenerateCenterCutGuides();
            break;
        default:
            break;
    }

    // Update VBO with new geometry
    if (!guide_vertices.empty()) {
        glBindVertexArray(guide_vao);
        glBindBuffer(GL_ARRAY_BUFFER, guide_vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     guide_vertices.size() * sizeof(float),
                     guide_vertices.data(),
                     GL_DYNAMIC_DRAW);

        // Position attribute only for line rendering
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);

        glBindVertexArray(0);
    }
}

void SafetyOverlaySystem::GenerateTitleSafeGuides(float target_aspect) {
    // Title safe: 10% margin from all edges
    float margin = 0.1f;

    // Calculate safe area bounds in normalized coordinates (-1 to 1)
    float safe_left = -1.0f + 2.0f * margin;
    float safe_right = 1.0f - 2.0f * margin;
    float safe_top = 1.0f - 2.0f * margin;
    float safe_bottom = -1.0f + 2.0f * margin;

    // Add rectangle outline
    AddLine(safe_left, safe_top, safe_right, safe_top);       // Top
    AddLine(safe_right, safe_top, safe_right, safe_bottom);   // Right
    AddLine(safe_right, safe_bottom, safe_left, safe_bottom); // Bottom
    AddLine(safe_left, safe_bottom, safe_left, safe_top);     // Left

    // Add center cross
    AddLine(-0.05f, 0.0f, 0.05f, 0.0f);  // Horizontal center
    AddLine(0.0f, -0.05f, 0.0f, 0.05f);  // Vertical center
}

void SafetyOverlaySystem::GenerateActionSafeGuides(float target_aspect) {
    // Action safe: 5% margin from all edges
    float margin = 0.05f;

    float safe_left = -1.0f + 2.0f * margin;
    float safe_right = 1.0f - 2.0f * margin;
    float safe_top = 1.0f - 2.0f * margin;
    float safe_bottom = -1.0f + 2.0f * margin;

    AddLine(safe_left, safe_top, safe_right, safe_top);
    AddLine(safe_right, safe_top, safe_right, safe_bottom);
    AddLine(safe_right, safe_bottom, safe_left, safe_bottom);
    AddLine(safe_left, safe_bottom, safe_left, safe_top);
}

void SafetyOverlaySystem::GenerateCenterCutGuides() {
    // 4:3 center cut from 16:9
    float cut_aspect = 4.0f / 3.0f;
    float current_aspect = (float)video_width / (float)video_height;

    if (current_aspect > cut_aspect) {
        // Wider than 4:3, show vertical crop lines
        float cut_width = 2.0f * (cut_aspect / current_aspect);
        float cut_left = -cut_width * 0.5f;
        float cut_right = cut_width * 0.5f;

        AddLine(cut_left, -1.0f, cut_left, 1.0f);   // Left crop line
        AddLine(cut_right, -1.0f, cut_right, 1.0f); // Right crop line
    }
}

void SafetyOverlaySystem::AddLine(float x1, float y1, float x2, float y2) {
    guide_vertices.push_back(x1);
    guide_vertices.push_back(y1);
    guide_vertices.push_back(x2);
    guide_vertices.push_back(y2);
}

bool SafetyOverlaySystem::CompileShader(GLuint& shader, const char* source, GLenum type) {
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    int success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);

    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        Debug::Log("ERROR: Safety overlay shader compilation failed: " + std::string(infoLog));
        return false;
    }

    return true;
}

bool SafetyOverlaySystem::LinkProgram(GLuint program) {
    glLinkProgram(program);

    int success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);

    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, nullptr, infoLog);
        Debug::Log("ERROR: Safety overlay shader linking failed: " + std::string(infoLog));
        return false;
    }

    return true;
}

void SafetyOverlaySystem::CleanupShaders() {
    if (composite_shader_program) {
        glDeleteProgram(composite_shader_program);
        composite_shader_program = 0;
    }
    if (overlay_vertex_shader) {
        glDeleteShader(overlay_vertex_shader);
        overlay_vertex_shader = 0;
    }
    if (overlay_fragment_shader) {
        glDeleteShader(overlay_fragment_shader);
        overlay_fragment_shader = 0;
    }
}