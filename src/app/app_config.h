#pragma once
// ============================================================================
// CacheSettings struct definition
// ============================================================================

#include "player/pipeline_mode.h"

struct CacheSettings {
    // SHARED SETTINGS (apply to both video and EXR)
    int max_cache_size_mb = 12288;        // Individual cache size limit (MB)
    int max_cache_seconds = 20;           // Shared: Maximum seconds to cache (video + EXR)
    PipelineMode current_pipeline_mode = PipelineMode::NORMAL;  // Current pipeline mode
    bool show_pipeline_info = true;       // Show pipeline impact info

    // VIDEO-SPECIFIC SETTINGS
    bool force_software_decode = false;  // Force software decode (disables D3D11VA/NVDEC)
    bool enable_nvidia_decode = false;    // NVIDIA hardware decode setting
    int max_batch_size = 8;               // Frames per extraction batch
    int max_concurrent_batches = 8;       // Number of parallel extraction threads

    // EXR SEQUENCE SETTINGS - Auto-configured based on CPU
    int exr_oiio_threads = 8;             // EXR I/O worker thread count (1-16) - auto-detected (will be 8-16 for modern CPUs)
    int exr_oiio_threads_per_worker = 1;  // EXR internal threads per worker (NOT used in PER_WORKER mode)
    int exr_threading_mode = 1;           // EXR decompression threading: 0=DISABLED, 1=PER_WORKER (1:1 fastest!), 2=AUTO

    // GPU MEMORY SETTINGS
    int gpu_memory_pool_mb = 2048;        // GPU VRAM limit for texture caching in MB (512-8192)

    // THUMBNAIL SCRUBBING SETTINGS
    bool enable_thumbnails = true;        // Enable/disable thumbnail scrubbing on timeline hover
    int thumbnail_width = 320;            // Thumbnail width in pixels (160-640)
    int thumbnail_height = 180;           // Thumbnail height in pixels (90-360)
    int thumbnail_cache_size = 100;       // Number of thumbnails to keep in RAM (50-500)

    // PLAYBACK SETTINGS
    bool auto_play_on_load = false;       // Auto-play videos after loading (with 500ms delay)
    bool loop_enabled = true;             // Loop playback (default ON)

    // COLOR MANAGEMENT SETTINGS
    bool auto_121_enabled = true;         // Auto-apply Rec.709 -> sRGB OCIO when 1-2-1 NCLC detected

    // VIDEO RANGE OVERRIDE (for D3D11 decoders)
    int video_range_mode = 0;             // 0=AUTO, 1=FULL, 2=LIMITED - override YUV color range

    // AUDIO SYNC SETTINGS
    int display_latency_preset = 5;       // 0=60Hz, 1=75Hz, 2=120Hz, 3=144Hz, 4=240Hz, 5=Custom
    float audio_fine_tune_ms = 0.0f;      // Fine-tune offset in milliseconds (±50ms range)
    float custom_display_latency_ms = 4.0f; // Custom display latency for preset index 5
};

extern CacheSettings cache_settings;
