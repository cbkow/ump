#pragma once

#include <string>
#include <memory>

#ifdef _WIN32
#include <glad/gl.h>
#else
#include <GL/gl.h>
#endif

namespace ump {

    // GPU compute configuration (stub - not implemented yet)
    struct GPUComputeConfig {
        bool enable_compute_acceleration = true;  // Enable GPU compute processing
        bool prefer_nvidia_cuda = true;           // Use CUDA when available on NVIDIA
        int compute_threads = 4;                  // Parallel compute work groups (1-16)

        bool IsValid() const {
            return compute_threads >= 1 && compute_threads <= 16;
        }
    };

    // GPU compute statistics
    struct GPUComputeStats {
        size_t frames_processed = 0;
        size_t total_processing_time_ms = 0;
        size_t average_processing_time_ms = 0;
        bool gpu_active = false;
        bool opencl_available = false;
        bool cuda_available = false;
        std::string active_device_name;
    };

    // Simple GPU compute processor (stub implementation)
    class GPUComputeProcessor {
    public:
        GPUComputeProcessor();
        ~GPUComputeProcessor();

        // Initialization
        bool Initialize(const GPUComputeConfig& config);
        void Shutdown();
        bool IsInitialized() const { return is_initialized_; }

        // Main processing interface (stub - not implemented)
        bool ProcessFrame(GLuint& gpu_texture);

        // Configuration
        void SetConfig(const GPUComputeConfig& config);
        GPUComputeConfig GetConfig() const { return config_; }

        // Statistics
        GPUComputeStats GetStats() const;
        void ResetStats();

        // Static utility methods
        static bool IsGPUComputeAvailable();
        static std::string GetSystemInfo();

    private:
        bool is_initialized_ = false;
        GPUComputeConfig config_;
        mutable GPUComputeStats stats_;

        void UpdateStats(size_t processing_time_ms);
    };

} // namespace ump