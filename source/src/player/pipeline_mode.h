#pragma once

#include <glad/gl.h>
#include <string>

// Pipeline Mode System for Video Processing Quality
enum class PipelineMode {
    NORMAL,           // RGBA8, standard 8-bit processing (best performance)
    HIGH_RES,         // RGBA16, 12-bit precision without float overhead (OCIO optimized)
    ULTRA_HIGH_RES,   // RGBA16F, maximum precision for complex OCIO workflows
    HDR_RES          // RGBA16F, HDR video (float for wide dynamic range and OCIO processing)
};

struct PipelineConfig {
    PipelineMode mode;
    GLenum internal_format;      // GL_RGBA8, GL_RGBA16, GL_RGBA16F
    GLenum data_type;           // GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT, GL_HALF_FLOAT
    bool linear_processing;      // Enable linear light processing
    bool constrain_primaries;    // Constrain to Rec.2020 for HDR display output
    size_t bytes_per_pixel;     // Memory calculation for cache sizing
    std::string description;     // User-friendly description

    // Default cache size recommendations per mode
    size_t recommended_cache_mb;
    size_t max_cache_mb;
};

#include <map>

// Pipeline configurations map - defined in video_display_component.cpp
extern const std::map<PipelineMode, PipelineConfig> PIPELINE_CONFIGS;

// Helper function to convert pipeline mode to display string
const char* PipelineModeToString(PipelineMode mode);

// Helper function to convert string to pipeline mode
inline PipelineMode StringToPipelineMode(const std::string& str) {
    if (str == "High-Res" || str == "HIGH_RES") return PipelineMode::HIGH_RES;
    if (str == "Ultra-High-Res" || str == "ULTRA_HIGH_RES") return PipelineMode::ULTRA_HIGH_RES;
    if (str == "HDR" || str == "HDR_RES") return PipelineMode::HDR_RES;
    return PipelineMode::NORMAL;
}

// Comparison mode enum - deprecated but kept for compatibility
#ifdef DIFFERENCE
#undef DIFFERENCE
#endif
enum class ComparisonMode {
    DISABLED,
    SIDE_BY_SIDE,
    OVERLAY,
    DIFFERENCE
};