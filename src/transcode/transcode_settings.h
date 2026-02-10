#pragma once

#include <string>

/**
 * Global transcode settings structure.
 * Defined in main.cpp, accessible from other translation units via extern.
 */
struct TranscodeSettings {
    int encoder_thread_count = 0;       // 0 = auto
    bool prefer_hardware_encoding = true;
    std::string hardware_encoder = "";
    int default_worker_count = 2;
    int max_worker_count = 8;
    int prefetch_buffer_size = 32;
    int prefetch_ahead_count = 16;
    int concurrent_loads = 8;
    int openexr_threads = 2;
};

// Global instance declared in main.cpp
extern TranscodeSettings transcode_settings;
