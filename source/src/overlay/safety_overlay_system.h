#pragma once

#include <glad/gl.h>
#include <string>
#include <memory>
#include <vector>
#include "../utils/debug_utils.h"

enum class SafetyOverlayType {
    TITLE_SAFE_16_9,      // Standard 16:9 title safe area (10% margins)
    ACTION_SAFE_16_9,     // Standard 16:9 action safe area (5% margins)
    TITLE_SAFE_9_16,      // Mobile/vertical 9:16 title safe
    ACTION_SAFE_9_16,     // Mobile/vertical 9:16 action safe
    BROADCAST_SAFE,       // Broadcast safe area (various aspect ratios)
    CENTER_CUT_SAFE,      // Center cut safe for 4:3 extraction
    CUSTOM_GUIDE,         // User-defined guide lines
    NONE
};

struct SafetyGuideSettings {
    SafetyOverlayType type = SafetyOverlayType::NONE;
    float opacity = 0.7f;
    float line_width = 2.0f;
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};  // RGBA - Default overridden at runtime
    bool show_labels = true;
    bool show_percentage_marks = false;
};

class SafetyOverlaySystem {
public:
    SafetyOverlaySystem();
    ~SafetyOverlaySystem();

    // Core lifecycle
    bool Initialize(int video_width, int video_height);
    void Cleanup();
    void UpdateDimensions(int video_width, int video_height);

    // Overlay management
    void SetOverlaySettings(const SafetyGuideSettings& settings);
    const SafetyGuideSettings& GetOverlaySettings() const { return current_settings; }

    // Enable/disable overlay rendering
    void SetEnabled(bool enabled) { overlay_enabled = enabled; }
    bool IsEnabled() const { return overlay_enabled; }

    // Main compositing function - applies safety overlays to input texture
    // Returns texture ID of result, or input_texture_id if no overlay active
    GLuint CompositeOverlays(GLuint input_texture_id, int texture_width, int texture_height);

    // Check if system is ready to render
    bool IsReady() const { return is_initialized && safety_fbo != 0 && safety_texture != 0; }

    // Get output texture (for direct access)
    GLuint GetOutputTexture() const { return safety_texture; }

private:
    // OpenGL resources
    GLuint safety_fbo = 0;
    GLuint safety_texture = 0;
    GLuint quad_vao = 0;
    GLuint quad_vbo = 0;

    // Shader resources
    GLuint composite_shader_program = 0;
    GLuint overlay_vertex_shader = 0;
    GLuint overlay_fragment_shader = 0;

    // Geometry for safety guides
    GLuint guide_vao = 0;
    GLuint guide_vbo = 0;
    std::vector<float> guide_vertices;

    // State
    int video_width = 0;
    int video_height = 0;
    bool is_initialized = false;
    bool overlay_enabled = false;
    SafetyGuideSettings current_settings;

    // Setup functions
    bool SetupFramebuffer();
    bool SetupShaders();
    bool SetupGeometry();
    void GenerateGuideGeometry();

    // Shader compilation helpers
    bool CompileShader(GLuint& shader, const char* source, GLenum type);
    bool LinkProgram(GLuint program);
    void CleanupShaders();

    // Geometry generation for different overlay types
    void GenerateTitleSafeGuides(float aspect_ratio);
    void GenerateActionSafeGuides(float aspect_ratio);
    void GenerateCenterCutGuides();
    void GenerateCustomGuides();

    // Coordinate conversion helpers
    void AddLine(float x1, float y1, float x2, float y2);
    void AddRect(float x, float y, float width, float height, bool filled = false);
    void NormalizeCoordinates(float& x, float& y) const;
};